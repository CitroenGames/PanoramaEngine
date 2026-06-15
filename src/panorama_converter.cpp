#include "ui/panorama/panorama_converter.hpp"

#include "panorama_string_util.hpp"
#include "ui/panorama/panorama_xml.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace openstrike
{
namespace
{
constexpr std::string_view kDefaultPanoramaStyle = R"(
body {
    width: 100%;
    height: 100%;
    margin: 0;
    background-color: #111418;
    color: #f2f2f2;
    font-family: LatoLatin;
}
.panorama-panel {
    display: block;
    box-sizing: border-box;
}
.panorama-flow-right {
    display: flex;
    flex-direction: row;
}
.panorama-flow-down {
    display: flex;
    flex-direction: column;
}
.panorama-button {
    min-width: 96px;
    padding: 8px 12px;
    margin: 2px;
    color: #f2f2f2;
    background-color: #2b323c;
    border-width: 0;
}
.panorama-label {
    display: inline;
}
.hidden {
    display: none;
}
.panorama-image,
.panorama-movie {
    min-width: 64px;
    min-height: 64px;
    background-color: #050608;
}
)";

using panorama_strings::starts_with;
using panorama_strings::to_lower;
using panorama_strings::trim;

bool contains_text(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}

std::string escape_rml(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text)
    {
        switch (ch)
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string strip_css_comments(std::string_view css)
{
    std::string stripped;
    stripped.reserve(css.size());

    bool in_comment = false;
    for (std::size_t index = 0; index < css.size(); ++index)
    {
        if (!in_comment && index + 1U < css.size() && css[index] == '/' && css[index + 1U] == '*')
        {
            in_comment = true;
            ++index;
            continue;
        }
        if (in_comment && index + 1U < css.size() && css[index] == '*' && css[index + 1U] == '/')
        {
            in_comment = false;
            ++index;
            continue;
        }
        if (!in_comment)
        {
            stripped.push_back(css[index]);
        }
    }

    return stripped;
}

// Panorama-only value tokens that RmlUi cannot parse. Declarations carrying any
// of these are dropped rather than passed through (which RmlUi rejects loudly).
// Removes @-rule blocks (@keyframes, @media, ...) including their nested braces.
// The flat declaration parser below cannot cope with nesting and would otherwise
// emit stray '}' tokens that RmlUi reports as "Invalid character".
std::string strip_at_rules(std::string_view css)
{
    std::string out;
    out.reserve(css.size());
    std::size_t index = 0;
    int brace_depth = 0;
    while (index < css.size())
    {
        // Only treat '@' as an at-rule at the top level. An '@' inside a
        // declaration block (e.g. an '@2x' image suffix) is an ordinary value
        // character; eating it here would corrupt the following rules.
        if (css[index] != '@' || brace_depth > 0)
        {
            if (css[index] == '{')
            {
                ++brace_depth;
            }
            else if (css[index] == '}' && brace_depth > 0)
            {
                --brace_depth;
            }
            out.push_back(css[index]);
            ++index;
            continue;
        }

        std::size_t cursor = index;
        while (cursor < css.size() && css[cursor] != '{' && css[cursor] != ';')
        {
            ++cursor;
        }
        if (cursor >= css.size())
        {
            break;
        }
        if (css[cursor] == ';')
        {
            index = cursor + 1;
            continue;
        }

        int depth = 0;
        while (cursor < css.size())
        {
            if (css[cursor] == '{')
            {
                ++depth;
            }
            else if (css[cursor] == '}')
            {
                --depth;
                if (depth == 0)
                {
                    ++cursor;
                    break;
                }
            }
            ++cursor;
        }
        index = cursor;
    }
    return out;
}

bool is_safe_css_value(std::string_view value)
{
    const std::string lowered = to_lower(value);
    static const std::string_view banned[] = {
        "gradient(", "s2-mix", "url(\"file://", "url('file://", "blur(",
        "fit-children", "fill-parent-flow", "height-percentage", "width-percentage",
        "fit-percentage", "squish", "noclip", "translate", "rotate", "scale", "matrix",
    };
    for (const std::string_view token : banned)
    {
        if (contains_text(lowered, token))
        {
            return false;
        }
    }
    return true;
}

// True if a value is a color RmlUi understands. Panorama theme variables
// (e.g. contentPanelBackground, color-hud-0, baseText) are bare identifiers and
// must be dropped instead of emitted.
bool is_rml_color(std::string_view value)
{
    const std::string lowered = to_lower(trim(value));
    if (lowered.empty())
    {
        return false;
    }
    if (lowered.front() == '#' || starts_with(lowered, "rgb(") || starts_with(lowered, "rgba(") ||
        starts_with(lowered, "hsl(") || starts_with(lowered, "hsla("))
    {
        return true;
    }
    static const std::unordered_set<std::string> keywords{
        "white", "black", "red", "green", "blue", "yellow", "cyan", "magenta", "gray", "grey",
        "transparent", "silver", "maroon", "olive", "lime", "aqua", "teal", "navy", "fuchsia",
        "purple", "orange", "brown", "pink", "gold", "darkgray", "lightgray",
    };
    return keywords.find(lowered) != keywords.end();
}

// True if every whitespace-separated token is a number (with optional unit) or a
// size keyword. Drops Panorama variable names (btnBorderRadius, contextmenu_zindex,
// mousepanningcursorsize) that RmlUi would reject on numeric properties.
bool is_length_like(std::string_view value)
{
    std::istringstream stream{std::string(value)};
    std::string token;
    bool saw_token = false;
    while (stream >> token)
    {
        saw_token = true;
        const std::string lowered = to_lower(token);
        if (lowered == "auto" || lowered == "none")
        {
            continue;
        }
        if (std::none_of(lowered.begin(), lowered.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        {
            return false;
        }
    }
    return saw_token;
}

std::vector<std::pair<std::string, std::string>> convert_css_declaration(std::string_view property, std::string_view value)
{
    const std::string prop = to_lower(trim(property));
    std::string val = trim(value);
    if (prop.empty() || val.empty())
    {
        return {};
    }

    if (prop == "flow-children")
    {
        const std::string lowered_value = to_lower(val);
        if (lowered_value == "right")
        {
            return {{"display", "flex"}, {"flex-direction", "row"}};
        }
        if (lowered_value == "down")
        {
            return {{"display", "flex"}, {"flex-direction", "column"}};
        }
        return {};
    }

    if (prop == "visibility")
    {
        if (to_lower(val) == "collapse")
        {
            return {{"display", "none"}};
        }
        return {{"visibility", val}};
    }

    // RmlUi takes a single family name; Panorama lists fallbacks and quotes them.
    // Only keep families we actually ship — Panorama also uses unresolved CSS
    // variables (stratum2Font, defaultFont) as families, which spam "No font face".
    if (prop == "font-family")
    {
        std::string family = val.substr(0, val.find(','));
        family.erase(std::remove(family.begin(), family.end(), '\''), family.end());
        family.erase(std::remove(family.begin(), family.end(), '"'), family.end());
        family = trim(family);

        static const std::unordered_set<std::string> known_families{
            "latolatin", "lato", "stratum2", "stratum2 monodigit", "noto sans",
            "noto serif", "noto mono", "noto sans symbols", "arial",
        };
        if (known_families.find(to_lower(family)) == known_families.end())
        {
            return {};
        }
        return {{"font-family", family}};
    }

    // Panorama uses keyword weights; RmlUi wants normal/bold or a 100-900 number.
    if (prop == "font-weight")
    {
        static const std::unordered_map<std::string, std::string> weights{
            {"thin", "100"}, {"extralight", "200"}, {"light", "300"}, {"lighter", "300"},
            {"normal", "400"}, {"regular", "400"}, {"medium", "500"}, {"semibold", "600"},
            {"bold", "700"}, {"bolder", "700"}, {"extrabold", "800"}, {"black", "900"}, {"heavy", "900"},
        };
        const auto it = weights.find(to_lower(val));
        return {{"font-weight", it != weights.end() ? it->second : val}};
    }

    if (prop == "font-style")
    {
        std::string lowered = to_lower(val);
        if (lowered == "italics")
        {
            lowered = "italic";
        }
        if (lowered != "italic" && lowered != "normal")
        {
            return {};
        }
        return {{"font-style", lowered}};
    }

    if (prop == "overflow")
    {
        const std::string lowered = to_lower(val);
        if (contains_text(lowered, "scroll"))
        {
            return {{"overflow", "auto"}};
        }
        if (contains_text(lowered, "noclip"))
        {
            return {{"overflow", "visible"}};
        }
        if (contains_text(lowered, "clip") || lowered == "hidden")
        {
            return {{"overflow", "hidden"}};
        }
        if (lowered == "visible" || lowered == "auto")
        {
            return {{"overflow", lowered}};
        }
        return {};
    }

    if (prop == "color" || prop == "background-color")
    {
        return is_rml_color(val) ? std::vector<std::pair<std::string, std::string>>{{prop, std::move(val)}}
                                 : std::vector<std::pair<std::string, std::string>>{};
    }

    // Panorama's `position` is a coordinate triple, not a CSS keyword.
    if (prop == "position")
    {
        const std::string lowered = to_lower(val);
        if (lowered == "absolute" || lowered == "relative" || lowered == "fixed" || lowered == "static")
        {
            return {{"position", lowered}};
        }
        return {};
    }

    if (!is_safe_css_value(val))
    {
        return {};
    }

    // Panorama writes box metrics with commas (margin: 4px, 8px, ...); RmlUi wants spaces.
    static const std::unordered_set<std::string> box_metrics{
        "margin", "margin-top", "margin-bottom", "margin-left", "margin-right",
        "padding", "padding-top", "padding-bottom", "padding-left", "padding-right",
    };
    if (box_metrics.find(prop) != box_metrics.end())
    {
        std::replace(val.begin(), val.end(), ',', ' ');
    }

    // RmlUi rejects percentage radii and the border shorthands Panorama uses.
    if (prop == "border-radius" && contains_text(val, "%"))
    {
        return {};
    }

    // Numeric properties must carry lengths/numbers, not Panorama variable names.
    static const std::unordered_set<std::string> numeric_properties{
        "width", "height", "min-width", "min-height", "max-width", "max-height",
        "top", "bottom", "left", "right", "border-radius", "border-width",
        "z-index", "line-height", "font-size", "flex-grow", "opacity",
        "margin", "margin-top", "margin-bottom", "margin-left", "margin-right",
        "padding", "padding-top", "padding-bottom", "padding-left", "padding-right",
    };
    if (numeric_properties.find(prop) != numeric_properties.end() && !is_length_like(val))
    {
        return {};
    }

    static const std::unordered_set<std::string> safe_properties{
        "align-items",
        "background-color",
        "border-color",
        "border-radius",
        "border-width",
        "bottom",
        "box-sizing",
        "color",
        "display",
        "flex",
        "flex-direction",
        "flex-grow",
        "font-size",
        "height",
        "justify-content",
        "left",
        "line-height",
        "margin",
        "margin-bottom",
        "margin-left",
        "margin-right",
        "margin-top",
        "max-height",
        "max-width",
        "min-height",
        "min-width",
        "opacity",
        "padding",
        "padding-bottom",
        "padding-left",
        "padding-right",
        "padding-top",
        "right",
        "text-align",
        "text-decoration",
        "top",
        "vertical-align",
        "width",
        "z-index",
    };

    if (safe_properties.find(prop) == safe_properties.end())
    {
        return {};
    }

    return {{prop, std::move(val)}};
}

// Runs an inline style="..." attribute through the same per-declaration filter so
// element styles get the same RmlUi-compatible treatment as <style> blocks.
std::string convert_inline_style(std::string_view style)
{
    std::string output;
    std::size_t cursor = 0;
    while (cursor < style.size())
    {
        const std::size_t semicolon = style.find(';', cursor);
        const std::size_t end = semicolon == std::string_view::npos ? style.size() : semicolon;
        const std::string_view declaration = style.substr(cursor, end - cursor);
        cursor = semicolon == std::string_view::npos ? style.size() : semicolon + 1U;

        const std::size_t colon = declaration.find(':');
        if (colon == std::string_view::npos)
        {
            continue;
        }
        for (const auto& [property, value] : convert_css_declaration(declaration.substr(0, colon), declaration.substr(colon + 1U)))
        {
            output += property + ": " + value + "; ";
        }
    }
    return output;
}

std::optional<std::string> attribute_string(const PanoramaXmlAttributes& attributes, std::string_view key)
{
    if (const std::string* value = attributes.find(key))
    {
        return *value;
    }
    return std::nullopt;
}

std::string mapped_rml_tag(std::string_view panorama_tag)
{
    const std::string lowered = to_lower(panorama_tag);
    if (lowered == "label")
    {
        return "span";
    }
    if (lowered == "button" || lowered == "radiobutton" || lowered == "togglebutton")
    {
        return "button";
    }
    return "div";
}

std::string tag_class(std::string_view panorama_tag)
{
    std::string lowered = to_lower(panorama_tag);
    for (char& ch : lowered)
    {
        if (!std::isalnum(static_cast<unsigned char>(ch)))
        {
            ch = '-';
        }
    }
    return "panorama-tag-" + lowered;
}

std::optional<std::string> command_from_onactivate(std::string_view script)
{
    if (contains_text(script, "OnHomeButtonPressed") || contains_text(script, "ShowHomePage"))
    {
        return "ShowHomePage";
    }
    if (contains_text(script, "OpenPlayMenu") || contains_text(script, "ShowPlayPage"))
    {
        return "ShowPlayPage";
    }
    if (contains_text(script, "OpenSettings") || contains_text(script, "JsSettings") ||
        contains_text(script, "settings/settings"))
    {
        return "OpenSettings";
    }
    if (contains_text(script, "OpenServerBrowser"))
    {
        return "OpenServerBrowser";
    }
    if (contains_text(script, "OpenConsole"))
    {
        return "OpenConsole";
    }
    if (contains_text(script, "OnQuitButtonPressed") || contains_text(script, "QuitGame"))
    {
        return "QuitNoConfirm";
    }
    return std::nullopt;
}

class PanoramaConverter
{
public:
    PanoramaConverter(const PanoramaResourceManager& resources, PanoramaConversionOptions options)
        : resources_(resources), options_(std::move(options))
    {
    }

    [[nodiscard]] PanoramaConversionResult convert_document(std::string_view document_path)
    {
        const std::string path = normalize_panorama_entry_path(document_path);
        const std::string body = convert_fragment(path, 0);

        PanoramaConversionResult result;
        std::ostringstream rml;
        rml << "<rml><head>";
        if (options_.include_default_style)
        {
            rml << "<style>" << kDefaultPanoramaStyle << "</style>";
        }
        for (const std::string& style : styles_)
        {
            rml << "<style>" << style << "</style>";
        }
        rml << "</head><body>" << body << "</body></rml>";
        result.rml = rml.str();
        result.scripts = scripts_;
        result.missing_resources = missing_resources_;
        return result;
    }

    [[nodiscard]] std::string convert_fragment(const std::string& document_path, std::size_t depth);
    void include_stylesheet(std::string_view source, const std::string& base_path)
    {
        const std::string path = resolve_resource_path(source, base_path);
        if (path.empty())
        {
            return;
        }

        if (!included_styles_.insert(path).second)
        {
            return;
        }

        if (const std::optional<std::string> css = read_text_resource(path))
        {
            styles_.push_back(convert_panorama_css(*css));
        }
        else
        {
            missing_resources_.push_back(path);
        }
    }

    void include_script(std::string_view source, const std::string& base_path)
    {
        const std::string path = resolve_resource_path(source, base_path);
        if (!path.empty() && included_scripts_.insert(path).second)
        {
            scripts_.push_back(path);
        }
    }

    [[nodiscard]] std::string resolve_resource_path(std::string_view source, const std::string& base_path) const
    {
        std::string normalized(source);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');

        constexpr std::string_view file_scheme = "file://";
        constexpr std::string_view resources_prefix = "{resources}/";
        constexpr std::string_view images_prefix = "{images}/";

        if (starts_with(normalized, file_scheme))
        {
            normalized.erase(0, file_scheme.size());
        }

        if (starts_with(normalized, resources_prefix))
        {
            normalized.erase(0, resources_prefix.size());
            return normalize_panorama_entry_path(normalized);
        }

        if (starts_with(normalized, images_prefix))
        {
            normalized.erase(0, images_prefix.size());
            return normalize_panorama_entry_path("panorama/images/" + normalized);
        }

        if (starts_with(normalized, "panorama/"))
        {
            return normalize_panorama_entry_path(normalized);
        }

        const std::filesystem::path base_parent = std::filesystem::path(base_path).parent_path();
        return normalize_panorama_entry_path((base_parent / normalized).generic_string());
    }

private:
    [[nodiscard]] std::optional<std::string> read_text_resource(const std::string& path) const
    {
        return resources_.read_text(path);
    }

    const PanoramaResourceManager& resources_;
    PanoramaConversionOptions options_;
    std::vector<std::string> styles_;
    std::vector<std::string> scripts_;
    std::vector<std::string> missing_resources_;
    std::unordered_set<std::string> included_styles_;
    std::unordered_set<std::string> included_scripts_;
};

class PanoramaXmlParser final : public PanoramaXmlSaxParser
{
public:
    PanoramaXmlParser(PanoramaConverter& converter, std::string base_path, std::size_t depth)
        : converter_(converter), base_path_(std::move(base_path)), depth_(depth)
    {
        register_cdata_tag("style");
        register_cdata_tag("script");
    }

    [[nodiscard]] std::string convert(std::string_view xml)
    {
        parse(xml, base_path_);
        return output_;
    }

protected:
    void handle_element_start(const std::string& name, const PanoramaXmlAttributes& attributes) override
    {
        const std::string lowered_name = to_lower(name);
        if (style_depth_ > 0)
        {
            ++style_depth_;
            if (lowered_name == "include")
            {
                if (const std::optional<std::string> src = attribute_string(attributes, "src"))
                {
                    converter_.include_stylesheet(*src, base_path_);
                }
            }
            return;
        }
        if (script_depth_ > 0)
        {
            ++script_depth_;
            if (lowered_name == "include")
            {
                if (const std::optional<std::string> src = attribute_string(attributes, "src"))
                {
                    converter_.include_script(*src, base_path_);
                }
            }
            return;
        }
        if (snippets_depth_ > 0)
        {
            ++snippets_depth_;
            return;
        }

        if (lowered_name == "root")
        {
            return;
        }
        if (lowered_name == "styles")
        {
            style_depth_ = 1;
            return;
        }
        if (lowered_name == "scripts")
        {
            script_depth_ = 1;
            return;
        }
        if (lowered_name == "snippets")
        {
            snippets_depth_ = 1;
            return;
        }

        if (lowered_name == "frame")
        {
            if (const std::optional<std::string> src = attribute_string(attributes, "src"))
            {
                const std::string frame_path = converter_.resolve_resource_path(*src, base_path_);
                output_ += converter_.convert_fragment(frame_path, depth_ + 1U);
            }
            emitted_tags_.push_back({});
            return;
        }

        emit_element_start(name, lowered_name, attributes);
    }

    void handle_element_end(const std::string& name) override
    {
        const std::string lowered_name = to_lower(name);
        if (style_depth_ > 0)
        {
            --style_depth_;
            return;
        }
        if (script_depth_ > 0)
        {
            --script_depth_;
            return;
        }
        if (snippets_depth_ > 0)
        {
            --snippets_depth_;
            return;
        }
        if (lowered_name == "root")
        {
            return;
        }

        if (emitted_tags_.empty())
        {
            return;
        }

        const std::string rml_tag = std::move(emitted_tags_.back());
        emitted_tags_.pop_back();
        if (!rml_tag.empty())
        {
            output_ += "</" + rml_tag + ">";
        }
    }

    void handle_data(const std::string& data, PanoramaXmlDataType) override
    {
        if (style_depth_ > 0 || script_depth_ > 0 || snippets_depth_ > 0)
        {
            return;
        }

        output_ += escape_rml(data);
    }

private:
    void emit_element_start(const std::string& name, const std::string& lowered_name, const PanoramaXmlAttributes& attributes)
    {
        const std::string rml_tag = mapped_rml_tag(name);
        std::string classes = "panorama-panel " + tag_class(name);
        if (lowered_name == "label")
        {
            classes += " panorama-label";
        }
        else if (lowered_name == "image")
        {
            classes += " panorama-image";
        }
        else if (lowered_name == "movie")
        {
            classes += " panorama-movie";
        }
        else if (rml_tag == "button")
        {
            classes += " panorama-button";
        }

        if (const std::optional<std::string> existing_class = attribute_string(attributes, "class"))
        {
            classes += " " + *existing_class;
        }

        if (const std::optional<std::string> flow = attribute_string(attributes, "flow-children"))
        {
            const std::string lowered_flow = to_lower(*flow);
            if (lowered_flow == "right")
            {
                classes += " panorama-flow-right";
            }
            else if (lowered_flow == "down")
            {
                classes += " panorama-flow-down";
            }
        }

        std::optional<std::string> onactivate = attribute_string(attributes, "onactivate");
        std::optional<std::string> command = onactivate ? command_from_onactivate(*onactivate) : std::nullopt;
        if (command)
        {
            classes += " menu-btn";
        }

        output_ += "<" + rml_tag;
        output_ += " data-panorama-tag=\"" + escape_rml(name) + "\"";
        output_ += " class=\"" + escape_rml(classes) + "\"";

        if (const std::optional<std::string> id = attribute_string(attributes, "id"))
        {
            output_ += " id=\"" + escape_rml(*id) + "\"";
        }
        if (const std::optional<std::string> style = attribute_string(attributes, "style"))
        {
            const std::string converted_style = convert_inline_style(*style);
            if (!converted_style.empty())
            {
                output_ += " style=\"" + escape_rml(converted_style) + "\"";
            }
        }
        if (command)
        {
            output_ += " data-command=\"" + escape_rml(*command) + "\"";
        }

        for (const auto& [key, value] : attributes)
        {
            const std::string lowered_key = to_lower(key);
            if (lowered_key == "id" || lowered_key == "class" || lowered_key == "style" || lowered_key == "text")
            {
                continue;
            }

            const std::string& string_value = value;
            if (lowered_key == "src")
            {
                output_ += " data-panorama-src=\"" + escape_rml(converter_.resolve_resource_path(string_value, base_path_)) + "\"";
            }
            else if (starts_with(lowered_key, "on"))
            {
                output_ += " data-panorama-" + lowered_key + "=\"" + escape_rml(string_value) + "\"";
            }
            else if (lowered_key == "flow-children")
            {
                output_ += " data-panorama-flow-children=\"" + escape_rml(string_value) + "\"";
            }
            else
            {
                output_ += " data-panorama-" + lowered_key + "=\"" + escape_rml(string_value) + "\"";
            }
        }

        output_ += ">";

        if (const std::optional<std::string> text = attribute_string(attributes, "text"))
        {
            output_ += escape_rml(*text);
        }

        emitted_tags_.push_back(rml_tag);
    }

    PanoramaConverter& converter_;
    std::string base_path_;
    std::size_t depth_ = 0;
    std::string output_;
    std::vector<std::string> emitted_tags_;
    int style_depth_ = 0;
    int script_depth_ = 0;
    int snippets_depth_ = 0;
};

std::string PanoramaConverter::convert_fragment(const std::string& document_path, std::size_t depth)
{
    if (depth > options_.max_frame_depth)
    {
        missing_resources_.push_back(document_path + " (frame recursion limit)");
        return {};
    }

    const std::string normalized_path = normalize_panorama_entry_path(document_path);
    std::optional<std::string> xml = read_text_resource(normalized_path);
    if (!xml)
    {
        missing_resources_.push_back(normalized_path);
        return {};
    }

    PanoramaXmlParser parser(*this, normalized_path, depth);
    return parser.convert(*xml);
}
}

std::string convert_panorama_css(std::string_view css)
{
    const std::string stripped = strip_at_rules(strip_css_comments(css));
    std::string output;

    std::size_t cursor = 0;
    while (cursor < stripped.size())
    {
        const std::size_t open = stripped.find('{', cursor);
        if (open == std::string::npos)
        {
            break;
        }
        const std::size_t close = stripped.find('}', open + 1U);
        if (close == std::string::npos)
        {
            break;
        }

        const std::string selector = trim(std::string_view(stripped).substr(cursor, open - cursor));
        const std::string body = std::string(std::string_view(stripped).substr(open + 1U, close - open - 1U));
        cursor = close + 1U;

        if (selector.empty() || selector.front() == '@')
        {
            continue;
        }

        std::vector<std::pair<std::string, std::string>> converted_declarations;
        std::size_t declaration_cursor = 0;
        while (declaration_cursor < body.size())
        {
            const std::size_t semicolon = body.find(';', declaration_cursor);
            const std::size_t declaration_end = semicolon == std::string::npos ? body.size() : semicolon;
            const std::string_view declaration = std::string_view(body).substr(declaration_cursor, declaration_end - declaration_cursor);
            declaration_cursor = semicolon == std::string::npos ? body.size() : semicolon + 1U;

            const std::size_t colon = declaration.find(':');
            if (colon == std::string_view::npos)
            {
                continue;
            }

            const std::vector<std::pair<std::string, std::string>> declarations =
                convert_css_declaration(declaration.substr(0, colon), declaration.substr(colon + 1U));
            converted_declarations.insert(converted_declarations.end(), declarations.begin(), declarations.end());
        }

        if (converted_declarations.empty())
        {
            continue;
        }

        output += selector + " {\n";
        for (const auto& [property, value] : converted_declarations)
        {
            output += "    " + property + ": " + value + ";\n";
        }
        output += "}\n";
    }

    return output;
}

PanoramaConversionResult convert_panorama_document(
    const PanoramaPackage& package,
    std::string_view document_path,
    const PanoramaConversionOptions& options)
{
    PanoramaResourceManager resources;
    resources.add_provider(std::make_unique<PanoramaPackageResourceProvider>(package), 0, "package");
    if (!options.resource_root.empty())
    {
        resources.add_provider(std::make_unique<PanoramaDirectoryResourceProvider>(options.resource_root), 100, "resource-root");
    }
    return convert_panorama_document(resources, document_path, options);
}

PanoramaConversionResult convert_panorama_document(
    const PanoramaResourceManager& resources,
    std::string_view document_path,
    const PanoramaConversionOptions& options)
{
    PanoramaConverter converter(resources, options);
    return converter.convert_document(document_path);
}
}
