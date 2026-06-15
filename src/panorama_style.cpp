#include "ui/panorama/panorama_style.hpp"

#include "panorama_string_util.hpp"
#include "ui/panorama/panorama_dom.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace openstrike
{
PanoramaCascadeStats& panorama_cascade_stats()
{
    static PanoramaCascadeStats stats;
    return stats;
}

namespace
{
using panorama_strings::is_space;
using panorama_strings::starts_with;
using panorama_strings::to_lower;
using panorama_strings::trim;

bool is_custom_property_name(std::string_view property)
{
    return property.size() >= 2 && property[0] == '-' && property[1] == '-';
}

bool contains(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}

bool environment_flag_set(const char* name)
{
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
    {
        return false;
    }
    std::free(value);
    return size > 0;
#else
    return std::getenv(name) != nullptr;
#endif
}

// Parses the leading numeric portion of a token, ignoring a trailing unit.
float parse_number(std::string_view token)
{
    const std::string text = trim(token);
    if (text.empty())
    {
        return 0.0F;
    }
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    return end == text.c_str() ? 0.0F : value;
}

// Parses a length token, reporting whether it carried a `%` unit. The numeric
// part is returned regardless (so "50%" -> {50, true}, "6px" -> {6, false}).
struct EdgeLength
{
    float value = 0.0F;
    bool percent = false;
};

EdgeLength parse_edge_length(std::string_view token)
{
    const std::string text = trim(token);
    return EdgeLength{parse_number(text), text.find('%') != std::string::npos};
}

// Returns the contents inside the first parenthesis pair, e.g. "fill-parent-flow(2)" -> "2".
std::string paren_arg(std::string_view value)
{
    const std::size_t open = value.find('(');
    const std::size_t close = value.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open)
    {
        return {};
    }
    return trim(value.substr(open + 1, close - open - 1));
}

// Splits a CSS value on whitespace while keeping parenthesized groups (e.g.
// "rgba(0, 0, 0, 0.5)") together as single tokens. Case is preserved.
std::vector<std::string> split_css_value_tokens(std::string_view value)
{
    std::vector<std::string> tokens;
    std::string current;
    int depth = 0;
    for (const char ch : value)
    {
        if (ch == '(')
        {
            ++depth;
        }
        else if (ch == ')')
        {
            depth = std::max(0, depth - 1);
        }
        if (depth == 0 && std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
            if (!current.empty())
            {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty())
    {
        tokens.push_back(current);
    }
    return tokens;
}

std::string parse_css_url(std::string_view value)
{
    std::string text = trim(value);
    if (text.empty() || to_lower(text) == "none")
    {
        return {};
    }

    const std::string lowered = to_lower(text);
    const std::size_t url = lowered.find("url(");
    if (url == std::string::npos)
    {
        return {};
    }
    const std::size_t open = url + 3U;
    const std::size_t close = text.find(')', open + 1U);
    if (close == std::string::npos || close <= open + 1U)
    {
        return {};
    }

    text = trim(std::string_view(text).substr(open + 1U, close - open - 1U));
    if (text.size() >= 2U && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\'')))
    {
        text = text.substr(1U, text.size() - 2U);
    }
    return text;
}

int parse_font_weight(std::string_view value, int inherited_weight)
{
    const std::string t = to_lower(trim(value));
    if (t == "normal" || t == "regular")
    {
        return 400;
    }
    if (t == "medium")
    {
        return 500;
    }
    if (t == "semibold" || t == "semi-bold" || t == "demibold" || t == "demi-bold")
    {
        return 600;
    }
    if (t == "bold")
    {
        return 700;
    }
    if (t == "extrabold" || t == "extra-bold")
    {
        return 800;
    }
    if (t == "black" || t == "heavy")
    {
        return 900;
    }
    if (t == "bolder")
    {
        return std::min(900, inherited_weight + 300);
    }
    if (t == "lighter")
    {
        return std::max(100, inherited_weight - 300);
    }

    const int numeric = static_cast<int>(std::lround(parse_number(t)));
    return numeric > 0 ? std::clamp(numeric, 1, 1000) : inherited_weight;
}

std::string strip_css_comments(std::string_view css)
{
    std::string out;
    out.reserve(css.size());
    bool in_comment = false;
    for (std::size_t i = 0; i < css.size(); ++i)
    {
        if (!in_comment && i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*')
        {
            in_comment = true;
            ++i;
        }
        else if (in_comment && i + 1 < css.size() && css[i] == '*' && css[i + 1] == '/')
        {
            in_comment = false;
            ++i;
        }
        else if (!in_comment)
        {
            out.push_back(css[i]);
        }
    }
    return out;
}

std::vector<std::string> split_top_level(std::string_view text, char delimiter)
{
    std::vector<std::string> parts;
    int depth = 0;
    char quote = '\0';
    bool escape = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (escape)
        {
            escape = false;
            continue;
        }
        if (quote != '\0')
        {
            if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == quote)
            {
                quote = '\0';
            }
            continue;
        }
        if (ch == '"' || ch == '\'')
        {
            quote = ch;
            continue;
        }
        if (ch == '(' || ch == '[')
        {
            ++depth;
        }
        else if (ch == ')' || ch == ']')
        {
            if (depth > 0)
            {
                --depth;
            }
        }
        else if (ch == delimiter && depth == 0)
        {
            parts.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.emplace_back(text.substr(start));
    return parts;
}

std::size_t find_matching_paren(std::string_view text, std::size_t open)
{
    int depth = 0;
    char quote = '\0';
    bool escape = false;
    for (std::size_t i = open; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (escape)
        {
            escape = false;
            continue;
        }
        if (quote != '\0')
        {
            if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == quote)
            {
                quote = '\0';
            }
            continue;
        }
        if (ch == '"' || ch == '\'')
        {
            quote = ch;
            continue;
        }
        if (ch == '(')
        {
            ++depth;
        }
        else if (ch == ')')
        {
            if (--depth == 0)
            {
                return i;
            }
        }
    }
    return std::string_view::npos;
}

std::size_t find_top_level_comma(std::string_view text)
{
    int depth = 0;
    char quote = '\0';
    bool escape = false;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (escape)
        {
            escape = false;
            continue;
        }
        if (quote != '\0')
        {
            if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == quote)
            {
                quote = '\0';
            }
            continue;
        }
        if (ch == '"' || ch == '\'')
        {
            quote = ch;
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{')
        {
            ++depth;
        }
        else if (ch == ')' || ch == ']' || ch == '}')
        {
            if (depth > 0)
            {
                --depth;
            }
        }
        else if (ch == ',' && depth == 0)
        {
            return i;
        }
    }
    return std::string_view::npos;
}

struct CssValueResolution
{
    bool valid = true;
    std::string value;
};

CssValueResolution resolve_custom_property_references(
    std::string_view value, const PanoramaCustomProperties& custom_properties, int depth)
{
    if (depth > 16)
    {
        return {false, {}};
    }

    CssValueResolution result;
    result.value.reserve(value.size());
    char quote = '\0';
    bool escape = false;
    for (std::size_t i = 0; i < value.size();)
    {
        const char ch = value[i];
        if (escape)
        {
            result.value.push_back(ch);
            escape = false;
            ++i;
            continue;
        }
        if (quote != '\0')
        {
            result.value.push_back(ch);
            if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == quote)
            {
                quote = '\0';
            }
            ++i;
            continue;
        }
        if (ch == '"' || ch == '\'')
        {
            quote = ch;
            result.value.push_back(ch);
            ++i;
            continue;
        }

        const bool function_boundary =
            i == 0 || (std::isalnum(static_cast<unsigned char>(value[i - 1])) == 0 && value[i - 1] != '_' &&
                          value[i - 1] != '-');
        if (function_boundary && i + 4 <= value.size() && to_lower(value.substr(i, 3)) == "var" &&
            value[i + 3] == '(')
        {
            const std::size_t close = find_matching_paren(value, i + 3);
            if (close == std::string_view::npos)
            {
                return {false, {}};
            }

            const std::string_view inner = value.substr(i + 4, close - i - 4);
            const std::size_t comma = find_top_level_comma(inner);
            const std::string name =
                comma == std::string_view::npos ? trim(inner) : trim(inner.substr(0, comma));
            CssValueResolution replacement;
            if (is_custom_property_name(name))
            {
                const std::string* declared = custom_properties.find(name);
                if (declared != nullptr)
                {
                    replacement = resolve_custom_property_references(*declared, custom_properties, depth + 1);
                }
                else if (comma != std::string_view::npos)
                {
                    replacement = resolve_custom_property_references(inner.substr(comma + 1), custom_properties, depth + 1);
                }
                else
                {
                    replacement.valid = false;
                }
            }
            else if (comma != std::string_view::npos)
            {
                replacement = resolve_custom_property_references(inner.substr(comma + 1), custom_properties, depth + 1);
            }
            else
            {
                replacement.valid = false;
            }

            if (!replacement.valid)
            {
                return {false, {}};
            }
            result.value += replacement.value;
            i = close + 1;
            continue;
        }

        result.value.push_back(ch);
        ++i;
    }
    result.value = trim(result.value);
    return result;
}

std::string unescape_css_token(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    bool escape = false;
    for (const char ch : text)
    {
        if (escape)
        {
            out.push_back(ch);
            escape = false;
        }
        else if (ch == '\\')
        {
            escape = true;
        }
        else
        {
            out.push_back(ch);
        }
    }
    if (escape)
    {
        out.push_back('\\');
    }
    return out;
}

std::string normalize_css_identifier_or_string(std::string_view value, bool* was_quoted = nullptr)
{
    std::string text = trim(value);
    bool quoted = false;
    if (text.size() >= 2U &&
        ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\'')))
    {
        quoted = true;
        text = unescape_css_token(std::string_view(text).substr(1U, text.size() - 2U));
    }
    else
    {
        text = unescape_css_token(text);
    }

    if (was_quoted != nullptr)
    {
        *was_quoted = quoted;
    }
    return text;
}

bool parse_attribute_selector(std::string_view raw, PanoramaAttributeSelector& out)
{
    std::string text = trim(raw);
    if (!text.empty() && text.back() == ']')
    {
        text.pop_back();
        text = trim(text);
    }
    if (text.empty())
    {
        return false;
    }

    std::size_t i = 0;
    while (i < text.size() && !is_space(text[i]) && text[i] != '=' && text[i] != '~' && text[i] != '|' &&
           text[i] != '^' && text[i] != '$' && text[i] != '*')
    {
        ++i;
    }

    out = PanoramaAttributeSelector{};
    out.name = to_lower(trim(std::string_view(text).substr(0, i)));
    if (out.name.empty())
    {
        return false;
    }

    while (i < text.size() && is_space(text[i]))
    {
        ++i;
    }
    if (i == text.size())
    {
        out.match = PanoramaAttributeMatch::Exists;
        return true;
    }

    if (text[i] == '=')
    {
        out.match = PanoramaAttributeMatch::Exact;
        ++i;
    }
    else if (i + 1 < text.size() && text[i + 1] == '=')
    {
        switch (text[i])
        {
        case '~':
            out.match = PanoramaAttributeMatch::Includes;
            break;
        case '|':
            out.match = PanoramaAttributeMatch::DashMatch;
            break;
        case '^':
            out.match = PanoramaAttributeMatch::Prefix;
            break;
        case '$':
            out.match = PanoramaAttributeMatch::Suffix;
            break;
        case '*':
            out.match = PanoramaAttributeMatch::Substring;
            break;
        default:
            return false;
        }
        i += 2;
    }
    else
    {
        return false;
    }

    while (i < text.size() && is_space(text[i]))
    {
        ++i;
    }

    const std::string_view rest(text.data() + i, text.size() - i);
    if (rest.empty())
    {
        return false;
    }

    std::string flag_text;
    if (rest.front() == '"' || rest.front() == '\'')
    {
        const char quote = rest.front();
        std::string value;
        bool escape = false;
        std::size_t j = 1;
        for (; j < rest.size(); ++j)
        {
            const char ch = rest[j];
            if (escape)
            {
                value.push_back(ch);
                escape = false;
            }
            else if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == quote)
            {
                break;
            }
            else
            {
                value.push_back(ch);
            }
        }
        if (j == rest.size() || rest[j] != quote)
        {
            return false;
        }
        out.value = std::move(value);
        flag_text = trim(rest.substr(j + 1));
    }
    else
    {
        std::size_t j = 0;
        while (j < rest.size() && !is_space(rest[j]))
        {
            ++j;
        }
        out.value = unescape_css_token(rest.substr(0, j));
        flag_text = trim(rest.substr(j));
    }

    if (!flag_text.empty())
    {
        if (flag_text.size() != 1)
        {
            return false;
        }
        if (flag_text[0] == 'i' || flag_text[0] == 'I')
        {
            out.case_insensitive = true;
        }
        else if (flag_text[0] != 's' && flag_text[0] != 'S')
        {
            return false;
        }
    }

    return true;
}

PanoramaSelector parse_selector(std::string_view text);
bool selector_matches(const PanoramaNode& node, const PanoramaSelector& selector);
std::array<int, 3> specificity(const PanoramaSelector& selector);

PanoramaSimpleSelector parse_simple_selector(std::string_view text)
{
    PanoramaSimpleSelector selector;
    std::size_t i = 0;
    // Leading type/universal token.
    while (i < text.size() && text[i] != '.' && text[i] != '#' && text[i] != ':' && text[i] != '[')
    {
        ++i;
    }
    const std::string head = trim(text.substr(0, i));
    if (head == "*")
    {
        selector.universal = true;
    }
    else if (!head.empty())
    {
        selector.type = to_lower(head);
    }

    while (i < text.size())
    {
        const char marker = text[i];
        ++i;
        const std::size_t start = i;
        int paren_depth = 0;
        int bracket_depth = marker == '[' ? 1 : 0;
        while (i < text.size())
        {
            const char ch = text[i];
            if (ch == '(')
            {
                ++paren_depth;
            }
            else if (ch == ')' && paren_depth > 0)
            {
                --paren_depth;
            }
            else if (ch == '[')
            {
                ++bracket_depth;
            }
            else if (ch == ']' && bracket_depth > 0)
            {
                --bracket_depth;
            }
            else if (paren_depth == 0 && bracket_depth == 0 && (ch == '.' || ch == '#' || ch == ':' || ch == '['))
            {
                break;
            }
            ++i;
        }
        const std::string raw_token = trim(text.substr(start, i - start));
        const std::string lowered_token = to_lower(raw_token);
        if (raw_token.empty())
        {
            continue;
        }
        switch (marker)
        {
        case '.':
            selector.classes.push_back(raw_token);
            break;
        case '#':
            selector.id = raw_token;
            break;
        case ':':
            if (starts_with(lowered_token, "not(") && lowered_token.size() >= 5 && lowered_token.back() == ')')
            {
                std::vector<std::shared_ptr<PanoramaSelector>> alternatives;
                const std::string_view inner = std::string_view(raw_token).substr(4, raw_token.size() - 5);
                for (const std::string& raw_alternative : split_top_level(inner, ','))
                {
                    const std::string alternative = trim(raw_alternative);
                    if (!alternative.empty())
                    {
                        PanoramaSelector parsed = parse_selector(alternative);
                        if (!parsed.compounds.empty())
                        {
                            parsed.specificity = specificity(parsed);
                            alternatives.push_back(std::make_shared<PanoramaSelector>(std::move(parsed)));
                        }
                    }
                }
                if (!alternatives.empty())
                {
                    selector.not_selector_groups.push_back(std::move(alternatives));
                }
                else
                {
                    selector.pseudos.push_back(lowered_token);
                }
            }
            else
            {
                selector.pseudos.push_back(lowered_token);
            }
            break;
        case '[':
            if (PanoramaAttributeSelector attr; parse_attribute_selector(raw_token, attr))
            {
                selector.attributes.push_back(std::move(attr));
            }
            else
            {
                selector.pseudos.push_back("[");
            }
            break;
        default:
            break;
        }
    }
    return selector;
}

PanoramaSelector parse_selector(std::string_view text)
{
    PanoramaSelector selector;
    PanoramaCombinator pending = PanoramaCombinator::Descendant;
    std::size_t start = std::string_view::npos;
    int depth = 0;
    const auto flush = [&](std::size_t end) {
        if (start == std::string_view::npos)
        {
            return;
        }
        const std::string compound = trim(text.substr(start, end - start));
        if (!compound.empty())
        {
            if (!selector.compounds.empty())
            {
                selector.combinators.push_back(pending);
            }
            selector.compounds.push_back(parse_simple_selector(compound));
            pending = PanoramaCombinator::Descendant;
        }
        start = std::string_view::npos;
    };

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (ch == '(' || ch == '[')
        {
            ++depth;
        }
        else if ((ch == ')' || ch == ']') && depth > 0)
        {
            --depth;
        }

        if (depth == 0 && (ch == '>' || ch == '+' || ch == '~'))
        {
            flush(i);
            if (ch == '>')
            {
                pending = PanoramaCombinator::Child;
            }
            else if (ch == '+')
            {
                pending = PanoramaCombinator::AdjacentSibling;
            }
            else
            {
                pending = PanoramaCombinator::GeneralSibling;
            }
            continue;
        }
        if (depth == 0 && is_space(ch))
        {
            flush(i);
            continue;
        }
        if (start == std::string_view::npos)
        {
            start = i;
        }
    }
    flush(text.size());
    return selector;
}

bool node_disabled(const PanoramaNode& node)
{
    if (std::find(node.classes.begin(), node.classes.end(), "disabled") != node.classes.end())
    {
        return true;
    }
    const auto it = node.attributes.find("enabled");
    return it != node.attributes.end() && it->second == "false";
}

bool node_or_descendant_focused(const PanoramaNode& node)
{
    if (node.focused)
    {
        return true;
    }
    for (const auto& child : node.children)
    {
        if (node_or_descendant_focused(*child))
        {
            return true;
        }
    }
    return false;
}

bool lookup_attribute_value(const PanoramaNode& node, std::string_view name, std::string& out)
{
    if (name == "id")
    {
        if (node.id.empty())
        {
            return false;
        }
        out = node.id;
        return true;
    }
    if (name == "class")
    {
        if (node.classes.empty())
        {
            return false;
        }
        out.clear();
        for (const std::string& klass : node.classes)
        {
            if (!out.empty())
            {
                out.push_back(' ');
            }
            out += klass;
        }
        return true;
    }
    if (name == "style")
    {
        if (node.inline_style.empty())
        {
            return false;
        }
        out = node.inline_style;
        return true;
    }
    if (name == "text")
    {
        if (node.text.empty())
        {
            return false;
        }
        out = node.text;
        return true;
    }

    const auto it = node.attributes.find(std::string(name));
    if (it == node.attributes.end())
    {
        return false;
    }
    out = it->second;
    return true;
}

bool attribute_value_matches(std::string actual, const PanoramaAttributeSelector& attr)
{
    std::string expected = attr.value;
    if (attr.case_insensitive)
    {
        actual = to_lower(actual);
        expected = to_lower(expected);
    }

    switch (attr.match)
    {
    case PanoramaAttributeMatch::Exists:
        return true;
    case PanoramaAttributeMatch::Exact:
        return actual == expected;
    case PanoramaAttributeMatch::Includes:
    {
        std::istringstream stream(actual);
        std::string token;
        while (stream >> token)
        {
            if (token == expected)
            {
                return true;
            }
        }
        return false;
    }
    case PanoramaAttributeMatch::DashMatch:
        return actual == expected ||
               (actual.size() > expected.size() && starts_with(actual, expected) && actual[expected.size()] == '-');
    case PanoramaAttributeMatch::Prefix:
        return starts_with(actual, expected);
    case PanoramaAttributeMatch::Suffix:
        return actual.size() >= expected.size() &&
               actual.compare(actual.size() - expected.size(), expected.size(), expected) == 0;
    case PanoramaAttributeMatch::Substring:
        return actual.find(expected) != std::string::npos;
    }
    return false;
}

bool simple_matches(const PanoramaNode& node, const PanoramaSimpleSelector& simple)
{
    ++panorama_cascade_stats().simple_tests;
    // Interaction pseudo-classes are resolved from the node's live state; any
    // pseudo we don't model means the rule doesn't apply.
    for (const std::string& pseudo : simple.pseudos)
    {
        if (pseudo == "hover")
        {
            if (!node.hovered)
            {
                return false;
            }
        }
        else if (pseudo == "active")
        {
            if (!node.active)
            {
                return false;
            }
        }
        else if (pseudo == "selected")
        {
            const bool sel = node.selected ||
                std::find(node.classes.begin(), node.classes.end(), "selected") != node.classes.end();
            if (!sel)
            {
                return false;
            }
        }
        else if (pseudo == "enabled")
        {
            if (node_disabled(node))
            {
                return false;
            }
        }
        else if (pseudo == "focus")
        {
            if (!node.focused)
            {
                return false;
            }
        }
        else if (pseudo == "focus-within" || pseudo == "focuswithin" || pseudo == "descendantfocus")
        {
            if (!node_or_descendant_focused(node))
            {
                return false;
            }
        }
        else if (pseudo == "disabled")
        {
            if (!node_disabled(node))
            {
                return false;
            }
        }
        else if (pseudo == "root")
        {
            if (node.parent != nullptr)
            {
                return false;
            }
        }
        else
        {
            return false; // unsupported pseudo -> no match
        }
    }
    if (!simple.universal && !simple.type.empty() && simple.type != node.tag_lower)
    {
        return false;
    }
    if (!simple.id.empty() && simple.id != node.id)
    {
        return false;
    }
    for (const std::string& klass : simple.classes)
    {
        if (!node.has_class(klass))
        {
            return false;
        }
    }
    for (const PanoramaAttributeSelector& attr : simple.attributes)
    {
        std::string value;
        if (!lookup_attribute_value(node, attr.name, value) || !attribute_value_matches(std::move(value), attr))
        {
            return false;
        }
    }
    for (const std::vector<std::shared_ptr<PanoramaSelector>>& not_group : simple.not_selector_groups)
    {
        const bool excluded = std::any_of(not_group.begin(), not_group.end(), [&](const auto& not_selector) {
            return not_selector && selector_matches(node, *not_selector);
        });
        if (excluded)
        {
            return false;
        }
    }
    return true;
}

const PanoramaNode* previous_sibling(const PanoramaNode& node)
{
    if (node.parent == nullptr)
    {
        return nullptr;
    }

    const PanoramaNode* previous = nullptr;
    for (const auto& sibling : node.parent->children)
    {
        if (sibling.get() == &node)
        {
            return previous;
        }
        previous = sibling.get();
    }
    return nullptr;
}

bool selector_matches(const PanoramaNode& node, const PanoramaSelector& selector)
{
    ++panorama_cascade_stats().selector_tests;
    if (selector.compounds.empty())
    {
        return false;
    }
    const auto& subject = selector.compounds.back();
    if (!simple_matches(node, subject))
    {
        return false;
    }
    // Walk remaining compounds (right to left), following WebCore's selector
    // relations: descendants/children move through ancestors, sibling relations
    // move through previous siblings and then resume from that sibling.
    const PanoramaNode* current = &node;
    for (std::size_t idx = selector.compounds.size() - 1; idx-- > 0;)
    {
        const auto& need = selector.compounds[idx];
        const PanoramaCombinator combinator = selector.combinators[idx];
        bool matched = false;
        if (combinator == PanoramaCombinator::Child)
        {
            const PanoramaNode* ancestor = current->parent;
            if (ancestor != nullptr && simple_matches(*ancestor, need))
            {
                matched = true;
                current = ancestor;
            }
        }
        else if (combinator == PanoramaCombinator::Descendant)
        {
            const PanoramaNode* ancestor = current->parent;
            while (ancestor != nullptr)
            {
                if (simple_matches(*ancestor, need))
                {
                    matched = true;
                    current = ancestor;
                    break;
                }
                ancestor = ancestor->parent;
            }
        }
        else if (combinator == PanoramaCombinator::AdjacentSibling)
        {
            const PanoramaNode* sibling = previous_sibling(*current);
            if (sibling != nullptr && simple_matches(*sibling, need))
            {
                matched = true;
                current = sibling;
            }
        }
        else
        {
            const PanoramaNode* sibling = previous_sibling(*current);
            while (sibling != nullptr)
            {
                if (simple_matches(*sibling, need))
                {
                    matched = true;
                    current = sibling;
                    break;
                }
                sibling = previous_sibling(*sibling);
            }
        }
        if (!matched)
        {
            return false;
        }
    }
    return true;
}

std::array<int, 3> specificity(const PanoramaSelector& selector)
{
    std::array<int, 3> spec{0, 0, 0};
    for (const auto& compound : selector.compounds)
    {
        if (!compound.id.empty())
        {
            spec[0] += 1;
        }
        spec[1] += static_cast<int>(compound.classes.size() + compound.attributes.size() + compound.pseudos.size());
        for (const std::vector<std::shared_ptr<PanoramaSelector>>& not_group : compound.not_selector_groups)
        {
            std::array<int, 3> group_spec{0, 0, 0};
            for (const auto& not_selector : not_group)
            {
                if (!not_selector)
                {
                    continue;
                }
                const std::array<int, 3> not_spec = specificity(*not_selector);
                if (not_spec > group_spec)
                {
                    group_spec = not_spec;
                }
            }
            spec[0] += group_spec[0];
            spec[1] += group_spec[1];
            spec[2] += group_spec[2];
        }
        if (!compound.type.empty())
        {
            spec[2] += 1;
        }
    }
    return spec;
}

// ---- SelectorFilter ancestor fast-reject (WebCore css/SelectorFilter.{h,cpp}) --
// A counting bloom filter of every ancestor's identifier hashes is maintained
// while the cascade walks the tree. Each selector precomputes (at parse time) up
// to four salted hashes of identifiers it REQUIRES on some ancestor; if any is
// absent from the filter the selector cannot match, skipping the full
// ancestor-walking match test. False positives only — never false negatives.

// Salts to separate otherwise identical string hashes, so a class selector like
// `.article` won't match an <article> tag (WebCore uses the same constants).
enum SelectorFilterSalt : std::uint32_t
{
    kSelectorFilterTagSalt = 13,
    kSelectorFilterIdSalt = 17,
    kSelectorFilterClassSalt = 19,
    kSelectorFilterAttributeSalt = 23,
};

std::uint32_t selector_filter_hash(std::string_view text)
{
    // FNV-1a 32-bit; both the element and selector sides hash through here so the
    // exact algorithm only has to agree with itself.
    std::uint32_t hash = 2166136261u;
    for (const char ch : text)
    {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 16777619u;
    }
    // 0 is the ancestor_hashes terminator; the salts are odd (invertible mod 2^32)
    // so only hash==0 itself could produce a salted 0.
    return hash != 0 ? hash : 1u;
}

// WTF::CountingBloomFilter<12> with the parent stack folded in: two 8-bit
// buckets per key (low and high hash halves), saturating at 255 (saturated
// buckets stick until clear(), which each compute() does anyway).
class SelectorAncestorFilter
{
public:
    void clear()
    {
        buckets_.fill(0);
        pushed_hashes_.clear();
        pushed_counts_.clear();
    }

    void push_element(const PanoramaNode& node)
    {
        const std::size_t before = pushed_hashes_.size();
        collect_element_identifier_hashes(node, pushed_hashes_);
        for (std::size_t i = before; i < pushed_hashes_.size(); ++i)
        {
            add(pushed_hashes_[i]);
        }
        pushed_counts_.push_back(static_cast<std::uint32_t>(pushed_hashes_.size() - before));
    }

    void pop_element()
    {
        const std::uint32_t count = pushed_counts_.back();
        pushed_counts_.pop_back();
        for (std::uint32_t i = 0; i < count; ++i)
        {
            remove(pushed_hashes_.back());
            pushed_hashes_.pop_back();
        }
    }

    [[nodiscard]] bool fast_reject(const std::array<std::uint32_t, 4>& hashes) const
    {
        for (const std::uint32_t hash : hashes)
        {
            if (hash == 0)
            {
                return false; // 0-terminator: all required hashes present
            }
            if (!may_contain(hash))
            {
                return true;
            }
        }
        return false;
    }

    // The identifiers an element contributes: lowercased tag, id, classes, and
    // attribute names (attribute keys are stored lowercased; id/class/style are
    // not in the attribute map, so no exclusion list is needed). Ids and classes
    // hash case-preserved because simple_matches compares them case-sensitively.
    static void collect_element_identifier_hashes(const PanoramaNode& node, std::vector<std::uint32_t>& out)
    {
        out.push_back(selector_filter_hash(node.tag_lower) * kSelectorFilterTagSalt);
        if (!node.id.empty())
        {
            out.push_back(selector_filter_hash(node.id) * kSelectorFilterIdSalt);
        }
        for (const std::string& klass : node.classes)
        {
            out.push_back(selector_filter_hash(klass) * kSelectorFilterClassSalt);
        }
        for (const auto& [name, value] : node.attributes)
        {
            out.push_back(selector_filter_hash(name) * kSelectorFilterAttributeSalt);
        }
    }

private:
    static constexpr unsigned kKeyBits = 12;
    static constexpr std::uint32_t kKeyMask = (1u << kKeyBits) - 1;

    [[nodiscard]] bool may_contain(std::uint32_t hash) const
    {
        return buckets_[hash & kKeyMask] != 0 && buckets_[(hash >> 16) & kKeyMask] != 0;
    }

    void add(std::uint32_t hash)
    {
        std::uint8_t& first = buckets_[hash & kKeyMask];
        std::uint8_t& second = buckets_[(hash >> 16) & kKeyMask];
        if (first < 255)
        {
            ++first;
        }
        if (second < 255)
        {
            ++second;
        }
    }

    void remove(std::uint32_t hash)
    {
        std::uint8_t& first = buckets_[hash & kKeyMask];
        std::uint8_t& second = buckets_[(hash >> 16) & kKeyMask];
        if (first > 0 && first < 255)
        {
            --first;
        }
        if (second > 0 && second < 255)
        {
            --second;
        }
    }

    std::array<std::uint8_t, (1u << kKeyBits)> buckets_{};
    std::vector<std::uint32_t> pushed_hashes_; // flat stack of per-element hashes
    std::vector<std::uint32_t> pushed_counts_; // hashes contributed per level
};

SelectorAncestorFilter& selector_ancestor_filter()
{
    static thread_local SelectorAncestorFilter filter;
    return filter;
}

// True when the selector (or any selector inside its :not groups) relates two
// compounds with a sibling combinator — i.e. a node's state can affect nodes it
// does not contain. Drives compute_invalidated's widened sibling recompute.
bool selector_uses_sibling_combinator(const PanoramaSelector& selector)
{
    for (const PanoramaCombinator combinator : selector.combinators)
    {
        if (combinator == PanoramaCombinator::AdjacentSibling || combinator == PanoramaCombinator::GeneralSibling)
        {
            return true;
        }
    }
    for (const PanoramaSimpleSelector& compound : selector.compounds)
    {
        for (const auto& not_group : compound.not_selector_groups)
        {
            for (const auto& not_selector : not_group)
            {
                if (not_selector && selector_uses_sibling_combinator(*not_selector))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

// True when the selector (or any :not argument) uses :focus-within in any of its
// spellings. Such a selector's subject style depends on a DESCENDANT's focus, which
// the sibling-local style-sharing comparison does not see, so its presence gates the
// focus-within cross-check in can_share_style.
bool selector_uses_focus_within(const PanoramaSelector& selector)
{
    for (const PanoramaSimpleSelector& compound : selector.compounds)
    {
        for (const std::string& pseudo : compound.pseudos)
        {
            if (pseudo == "focus-within" || pseudo == "focuswithin" || pseudo == "descendantfocus")
            {
                return true;
            }
        }
        for (const auto& not_group : compound.not_selector_groups)
        {
            for (const auto& not_selector : not_group)
            {
                if (not_selector && selector_uses_focus_within(*not_selector))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

// Fills selector.ancestor_hashes from the compounds that must match ANCESTORS:
// a compound whose right-side combinator is descendant/child is an ancestor
// requirement; a compound left of a sibling combinator matches a sibling (or a
// sibling's ancestor — shared with the subject, reached via its own descendant
// relation later in the walk), so it contributes nothing. Mirrors WebCore's
// SelectorFilter::collectSelectorHashes + chooseSelectorHashesForFilter,
// preferring id > attribute > class > tag hashes for the four slots.
void collect_selector_ancestor_hashes(PanoramaSelector& selector)
{
    selector.ancestor_hashes = {0, 0, 0, 0};
    if (selector.compounds.size() < 2)
    {
        return;
    }

    std::vector<std::uint32_t> ids;
    std::vector<std::uint32_t> attributes;
    std::vector<std::uint32_t> classes;
    std::vector<std::uint32_t> tags;
    for (std::size_t idx = selector.compounds.size() - 1; idx-- > 0;)
    {
        const PanoramaCombinator relation = selector.combinators[idx];
        if (relation != PanoramaCombinator::Descendant && relation != PanoramaCombinator::Child)
        {
            continue; // sibling relation: not an ancestor of the subject
        }
        const PanoramaSimpleSelector& compound = selector.compounds[idx];
        // Negations (:not) are exclusions, never requirements — do not collect.
        if (!compound.id.empty())
        {
            ids.push_back(selector_filter_hash(compound.id) * kSelectorFilterIdSalt);
        }
        for (const PanoramaAttributeSelector& attribute : compound.attributes)
        {
            attributes.push_back(selector_filter_hash(attribute.name) * kSelectorFilterAttributeSalt);
        }
        for (const std::string& klass : compound.classes)
        {
            classes.push_back(selector_filter_hash(klass) * kSelectorFilterClassSalt);
        }
        if (!compound.universal && !compound.type.empty())
        {
            tags.push_back(selector_filter_hash(compound.type) * kSelectorFilterTagSalt);
        }
    }

    std::size_t index = 0;
    const auto add_if_new = [&](std::uint32_t hash) {
        for (std::size_t i = 0; i < index; ++i)
        {
            if (selector.ancestor_hashes[i] == hash)
            {
                return;
            }
        }
        selector.ancestor_hashes[index++] = hash;
    };
    const auto copy_hashes = [&](const std::vector<std::uint32_t>& hashes) {
        for (const std::uint32_t hash : hashes)
        {
            add_if_new(hash);
            if (index == selector.ancestor_hashes.size())
            {
                return true;
            }
        }
        return false;
    };
    // Limited slots: prefer the more selective identifier kinds.
    if (copy_hashes(ids) || copy_hashes(attributes) || copy_hashes(classes) || copy_hashes(tags))
    {
        return;
    }
}
}

PanoramaLength parse_panorama_length(std::string_view value)
{
    const std::string lowered = to_lower(trim(value));
    PanoramaLength length;
    if (lowered.empty() || lowered == "auto")
    {
        return length; // FitChildren
    }
    if (contains(lowered, "fit-children"))
    {
        length.type = PanoramaLengthType::FitChildren;
        return length;
    }
    if (contains(lowered, "fill-parent-flow"))
    {
        length.type = PanoramaLengthType::FillParentFlow;
        const std::string arg = paren_arg(lowered);
        length.value = arg.empty() ? 1.0F : parse_number(arg);
        if (length.value <= 0.0F)
        {
            length.value = 1.0F;
        }
        return length;
    }
    if (contains(lowered, "width-percentage"))
    {
        length.type = PanoramaLengthType::WidthPercent;
        length.value = parse_number(paren_arg(lowered));
        return length;
    }
    if (contains(lowered, "height-percentage"))
    {
        length.type = PanoramaLengthType::HeightPercent;
        length.value = parse_number(paren_arg(lowered));
        return length;
    }
    if (lowered.back() == '%')
    {
        length.type = PanoramaLengthType::Percent;
        length.value = parse_number(lowered);
        return length;
    }
    length.type = PanoramaLengthType::Pixels;
    length.value = parse_number(lowered);
    return length;
}

namespace
{
std::uint8_t hex_pair(char hi, char lo)
{
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
        {
            return ch - '0';
        }
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lower >= 'a' && lower <= 'f')
        {
            return 10 + (lower - 'a');
        }
        return 0;
    };
    return static_cast<std::uint8_t>(digit(hi) * 16 + digit(lo));
}

PanoramaColor parse_hex_color(std::string_view hex)
{
    PanoramaColor color;
    color.a = 0xFF;
    if (hex.size() == 3 || hex.size() == 4)
    {
        color.r = hex_pair(hex[0], hex[0]);
        color.g = hex_pair(hex[1], hex[1]);
        color.b = hex_pair(hex[2], hex[2]);
        if (hex.size() == 4)
        {
            color.a = hex_pair(hex[3], hex[3]);
        }
    }
    else if (hex.size() == 6 || hex.size() == 8)
    {
        color.r = hex_pair(hex[0], hex[1]);
        color.g = hex_pair(hex[2], hex[3]);
        color.b = hex_pair(hex[4], hex[5]);
        if (hex.size() == 8)
        {
            color.a = hex_pair(hex[6], hex[7]);
        }
    }
    return color;
}
}

PanoramaColor parse_panorama_color(std::string_view value)
{
    const std::string lowered = to_lower(trim(value));
    if (lowered.empty() || lowered == "none" || lowered == "transparent")
    {
        return PanoramaColor{};
    }

    // The full CSS named-color set (extended-color keywords), ported from WebCore's
    // platform/ColorData.gperf so values match a real browser exactly. The CS:GO
    // menu CSS uses several of these beyond the basic set (lightgreen, darkgrey,
    // lightgray, gold, wheat, purple, ...) which previously fell through to a
    // transparent/black fill. `transparent` is also handled earlier by keyword.
    static const std::unordered_map<std::string, PanoramaColor> keywords{
        {"aliceblue", {0xf0, 0xf8, 0xff, 0xff}},
        {"antiquewhite", {0xfa, 0xeb, 0xd7, 0xff}},
        {"aqua", {0x00, 0xff, 0xff, 0xff}},
        {"aquamarine", {0x7f, 0xff, 0xd4, 0xff}},
        {"azure", {0xf0, 0xff, 0xff, 0xff}},
        {"beige", {0xf5, 0xf5, 0xdc, 0xff}},
        {"bisque", {0xff, 0xe4, 0xc4, 0xff}},
        {"black", {0x00, 0x00, 0x00, 0xff}},
        {"blanchedalmond", {0xff, 0xeb, 0xcd, 0xff}},
        {"blue", {0x00, 0x00, 0xff, 0xff}},
        {"blueviolet", {0x8a, 0x2b, 0xe2, 0xff}},
        {"brown", {0xa5, 0x2a, 0x2a, 0xff}},
        {"burlywood", {0xde, 0xb8, 0x87, 0xff}},
        {"cadetblue", {0x5f, 0x9e, 0xa0, 0xff}},
        {"chartreuse", {0x7f, 0xff, 0x00, 0xff}},
        {"chocolate", {0xd2, 0x69, 0x1e, 0xff}},
        {"coral", {0xff, 0x7f, 0x50, 0xff}},
        {"cornflowerblue", {0x64, 0x95, 0xed, 0xff}},
        {"cornsilk", {0xff, 0xf8, 0xdc, 0xff}},
        {"crimson", {0xdc, 0x14, 0x3c, 0xff}},
        {"cyan", {0x00, 0xff, 0xff, 0xff}},
        {"darkblue", {0x00, 0x00, 0x8b, 0xff}},
        {"darkcyan", {0x00, 0x8b, 0x8b, 0xff}},
        {"darkgoldenrod", {0xb8, 0x86, 0x0b, 0xff}},
        {"darkgray", {0xa9, 0xa9, 0xa9, 0xff}},
        {"darkgrey", {0xa9, 0xa9, 0xa9, 0xff}},
        {"darkgreen", {0x00, 0x64, 0x00, 0xff}},
        {"darkkhaki", {0xbd, 0xb7, 0x6b, 0xff}},
        {"darkmagenta", {0x8b, 0x00, 0x8b, 0xff}},
        {"darkolivegreen", {0x55, 0x6b, 0x2f, 0xff}},
        {"darkorange", {0xff, 0x8c, 0x00, 0xff}},
        {"darkorchid", {0x99, 0x32, 0xcc, 0xff}},
        {"darkred", {0x8b, 0x00, 0x00, 0xff}},
        {"darksalmon", {0xe9, 0x96, 0x7a, 0xff}},
        {"darkseagreen", {0x8f, 0xbc, 0x8f, 0xff}},
        {"darkslateblue", {0x48, 0x3d, 0x8b, 0xff}},
        {"darkslategray", {0x2f, 0x4f, 0x4f, 0xff}},
        {"darkslategrey", {0x2f, 0x4f, 0x4f, 0xff}},
        {"darkturquoise", {0x00, 0xce, 0xd1, 0xff}},
        {"darkviolet", {0x94, 0x00, 0xd3, 0xff}},
        {"deeppink", {0xff, 0x14, 0x93, 0xff}},
        {"deepskyblue", {0x00, 0xbf, 0xff, 0xff}},
        {"dimgray", {0x69, 0x69, 0x69, 0xff}},
        {"dimgrey", {0x69, 0x69, 0x69, 0xff}},
        {"dodgerblue", {0x1e, 0x90, 0xff, 0xff}},
        {"firebrick", {0xb2, 0x22, 0x22, 0xff}},
        {"floralwhite", {0xff, 0xfa, 0xf0, 0xff}},
        {"forestgreen", {0x22, 0x8b, 0x22, 0xff}},
        {"fuchsia", {0xff, 0x00, 0xff, 0xff}},
        {"gainsboro", {0xdc, 0xdc, 0xdc, 0xff}},
        {"ghostwhite", {0xf8, 0xf8, 0xff, 0xff}},
        {"gold", {0xff, 0xd7, 0x00, 0xff}},
        {"goldenrod", {0xda, 0xa5, 0x20, 0xff}},
        {"gray", {0x80, 0x80, 0x80, 0xff}},
        {"grey", {0x80, 0x80, 0x80, 0xff}},
        {"green", {0x00, 0x80, 0x00, 0xff}},
        {"greenyellow", {0xad, 0xff, 0x2f, 0xff}},
        {"honeydew", {0xf0, 0xff, 0xf0, 0xff}},
        {"hotpink", {0xff, 0x69, 0xb4, 0xff}},
        {"indianred", {0xcd, 0x5c, 0x5c, 0xff}},
        {"indigo", {0x4b, 0x00, 0x82, 0xff}},
        {"ivory", {0xff, 0xff, 0xf0, 0xff}},
        {"khaki", {0xf0, 0xe6, 0x8c, 0xff}},
        {"lavender", {0xe6, 0xe6, 0xfa, 0xff}},
        {"lavenderblush", {0xff, 0xf0, 0xf5, 0xff}},
        {"lawngreen", {0x7c, 0xfc, 0x00, 0xff}},
        {"lemonchiffon", {0xff, 0xfa, 0xcd, 0xff}},
        {"lightblue", {0xad, 0xd8, 0xe6, 0xff}},
        {"lightcoral", {0xf0, 0x80, 0x80, 0xff}},
        {"lightcyan", {0xe0, 0xff, 0xff, 0xff}},
        {"lightgoldenrodyellow", {0xfa, 0xfa, 0xd2, 0xff}},
        {"lightgray", {0xd3, 0xd3, 0xd3, 0xff}},
        {"lightgrey", {0xd3, 0xd3, 0xd3, 0xff}},
        {"lightgreen", {0x90, 0xee, 0x90, 0xff}},
        {"lightpink", {0xff, 0xb6, 0xc1, 0xff}},
        {"lightsalmon", {0xff, 0xa0, 0x7a, 0xff}},
        {"lightseagreen", {0x20, 0xb2, 0xaa, 0xff}},
        {"lightskyblue", {0x87, 0xce, 0xfa, 0xff}},
        {"lightslateblue", {0x84, 0x70, 0xff, 0xff}},
        {"lightslategray", {0x77, 0x88, 0x99, 0xff}},
        {"lightslategrey", {0x77, 0x88, 0x99, 0xff}},
        {"lightsteelblue", {0xb0, 0xc4, 0xde, 0xff}},
        {"lightyellow", {0xff, 0xff, 0xe0, 0xff}},
        {"lime", {0x00, 0xff, 0x00, 0xff}},
        {"limegreen", {0x32, 0xcd, 0x32, 0xff}},
        {"linen", {0xfa, 0xf0, 0xe6, 0xff}},
        {"magenta", {0xff, 0x00, 0xff, 0xff}},
        {"maroon", {0x80, 0x00, 0x00, 0xff}},
        {"mediumaquamarine", {0x66, 0xcd, 0xaa, 0xff}},
        {"mediumblue", {0x00, 0x00, 0xcd, 0xff}},
        {"mediumorchid", {0xba, 0x55, 0xd3, 0xff}},
        {"mediumpurple", {0x93, 0x70, 0xdb, 0xff}},
        {"mediumseagreen", {0x3c, 0xb3, 0x71, 0xff}},
        {"mediumslateblue", {0x7b, 0x68, 0xee, 0xff}},
        {"mediumspringgreen", {0x00, 0xfa, 0x9a, 0xff}},
        {"mediumturquoise", {0x48, 0xd1, 0xcc, 0xff}},
        {"mediumvioletred", {0xc7, 0x15, 0x85, 0xff}},
        {"midnightblue", {0x19, 0x19, 0x70, 0xff}},
        {"mintcream", {0xf5, 0xff, 0xfa, 0xff}},
        {"mistyrose", {0xff, 0xe4, 0xe1, 0xff}},
        {"moccasin", {0xff, 0xe4, 0xb5, 0xff}},
        {"navajowhite", {0xff, 0xde, 0xad, 0xff}},
        {"navy", {0x00, 0x00, 0x80, 0xff}},
        {"oldlace", {0xfd, 0xf5, 0xe6, 0xff}},
        {"olive", {0x80, 0x80, 0x00, 0xff}},
        {"olivedrab", {0x6b, 0x8e, 0x23, 0xff}},
        {"orange", {0xff, 0xa5, 0x00, 0xff}},
        {"orangered", {0xff, 0x45, 0x00, 0xff}},
        {"orchid", {0xda, 0x70, 0xd6, 0xff}},
        {"palegoldenrod", {0xee, 0xe8, 0xaa, 0xff}},
        {"palegreen", {0x98, 0xfb, 0x98, 0xff}},
        {"paleturquoise", {0xaf, 0xee, 0xee, 0xff}},
        {"palevioletred", {0xdb, 0x70, 0x93, 0xff}},
        {"papayawhip", {0xff, 0xef, 0xd5, 0xff}},
        {"peachpuff", {0xff, 0xda, 0xb9, 0xff}},
        {"peru", {0xcd, 0x85, 0x3f, 0xff}},
        {"pink", {0xff, 0xc0, 0xcb, 0xff}},
        {"plum", {0xdd, 0xa0, 0xdd, 0xff}},
        {"powderblue", {0xb0, 0xe0, 0xe6, 0xff}},
        {"purple", {0x80, 0x00, 0x80, 0xff}},
        {"rebeccapurple", {0x66, 0x33, 0x99, 0xff}},
        {"red", {0xff, 0x00, 0x00, 0xff}},
        {"rosybrown", {0xbc, 0x8f, 0x8f, 0xff}},
        {"royalblue", {0x41, 0x69, 0xe1, 0xff}},
        {"saddlebrown", {0x8b, 0x45, 0x13, 0xff}},
        {"salmon", {0xfa, 0x80, 0x72, 0xff}},
        {"sandybrown", {0xf4, 0xa4, 0x60, 0xff}},
        {"seagreen", {0x2e, 0x8b, 0x57, 0xff}},
        {"seashell", {0xff, 0xf5, 0xee, 0xff}},
        {"sienna", {0xa0, 0x52, 0x2d, 0xff}},
        {"silver", {0xc0, 0xc0, 0xc0, 0xff}},
        {"skyblue", {0x87, 0xce, 0xeb, 0xff}},
        {"slateblue", {0x6a, 0x5a, 0xcd, 0xff}},
        {"slategray", {0x70, 0x80, 0x90, 0xff}},
        {"slategrey", {0x70, 0x80, 0x90, 0xff}},
        {"snow", {0xff, 0xfa, 0xfa, 0xff}},
        {"springgreen", {0x00, 0xff, 0x7f, 0xff}},
        {"steelblue", {0x46, 0x82, 0xb4, 0xff}},
        {"tan", {0xd2, 0xb4, 0x8c, 0xff}},
        {"teal", {0x00, 0x80, 0x80, 0xff}},
        {"thistle", {0xd8, 0xbf, 0xd8, 0xff}},
        {"tomato", {0xff, 0x63, 0x47, 0xff}},
        {"transparent", {0x00, 0x00, 0x00, 0x00}},
        {"turquoise", {0x40, 0xe0, 0xd0, 0xff}},
        {"violet", {0xee, 0x82, 0xee, 0xff}},
        {"violetred", {0xd0, 0x20, 0x90, 0xff}},
        {"wheat", {0xf5, 0xde, 0xb3, 0xff}},
        {"white", {0xff, 0xff, 0xff, 0xff}},
        {"whitesmoke", {0xf5, 0xf5, 0xf5, 0xff}},
        {"yellow", {0xff, 0xff, 0x00, 0xff}},
        {"yellowgreen", {0x9a, 0xcd, 0x32, 0xff}},
    };
    if (const auto it = keywords.find(lowered); it != keywords.end())
    {
        return it->second;
    }

    if (lowered.front() == '#')
    {
        return parse_hex_color(std::string_view(lowered).substr(1));
    }

    if (starts_with(lowered, "rgb(") || starts_with(lowered, "rgba("))
    {
        const std::vector<std::string> parts = split_top_level(paren_arg(lowered), ',');
        PanoramaColor color;
        color.a = 0xFF;
        if (parts.size() >= 3)
        {
            color.r = static_cast<std::uint8_t>(std::clamp(static_cast<int>(parse_number(parts[0])), 0, 255));
            color.g = static_cast<std::uint8_t>(std::clamp(static_cast<int>(parse_number(parts[1])), 0, 255));
            color.b = static_cast<std::uint8_t>(std::clamp(static_cast<int>(parse_number(parts[2])), 0, 255));
        }
        if (parts.size() >= 4)
        {
            // Alpha may be 0-1 (CSS) or 0-255 (Panorama); disambiguate by the dot.
            const float alpha = parse_number(parts[3]);
            const float scaled = contains(parts[3], ".") ? alpha * 255.0F : alpha;
            color.a = static_cast<std::uint8_t>(std::clamp(static_cast<int>(scaled + 0.5F), 0, 255));
        }
        return color;
    }

    // Functional colors that are not parsed by a property-specific path still get
    // a representative fallback instead of becoming invisible.
    if (const std::size_t hash = lowered.find('#'); hash != std::string::npos)
    {
        std::size_t end = hash + 1;
        while (end < lowered.size() && std::isxdigit(static_cast<unsigned char>(lowered[end])) != 0)
        {
            ++end;
        }
        return parse_hex_color(std::string_view(lowered).substr(hash + 1, end - hash - 1));
    }

    return PanoramaColor{};
}

// True when `token` reads as a CSS colour on its own — hex, rgb()/rgba(),
// `transparent`, or a named keyword — and fills `out` from it. Used by the
// shorthand parsers (border, box-shadow, text-shadow) to locate the colour
// among width/style/offset tokens, the way WebCore's shorthand consumers try
// consumeColor on each token (CSSPropertyParserHelpers).
bool try_parse_color_token(std::string_view token, PanoramaColor& out)
{
    const std::string text = trim(token);
    if (text.empty())
    {
        return false;
    }
    const std::string lowered = to_lower(text);
    if (lowered == "transparent")
    {
        out = PanoramaColor{0x00, 0x00, 0x00, 0x00};
        return true;
    }
    if (text.front() == '#' || starts_with(lowered, "rgb(") || starts_with(lowered, "rgba("))
    {
        out = parse_panorama_color(text);
        return true;
    }
    // Named keywords parse to an opaque colour; anything unknown comes back
    // fully transparent and is not a colour token.
    const PanoramaColor parsed = parse_panorama_color(text);
    if (parsed.a != 0)
    {
        out = parsed;
        return true;
    }
    return false;
}

namespace
{
// Forward declaration: defined later in this anonymous namespace but used here.
std::vector<std::string> split_css_whitespace_values(std::string_view value);

// Parses a `border` / `border-<side>` shorthand ("<width> <style> <color>").
// The width is the first numeric token; the colour is the first token that
// reads as a colour — keyword, rgb()/rgba(), or #hex — wherever it appears
// (shipped sheets write `1px solid black`, `2px solid rgba(...)`, ...). The
// `none`/`hidden` styles drop the border, like the CSS border-style values.
void parse_border_shorthand(std::string_view value, float& width, PanoramaColor& color)
{
    width = 0.0F;
    color = PanoramaColor{};
    bool saw_width = false;
    for (const std::string& token : split_css_value_tokens(value))
    {
        const std::string lowered = to_lower(token);
        if (lowered == "none" || lowered == "hidden")
        {
            width = 0.0F;
            color = PanoramaColor{};
            return;
        }
        if (lowered == "solid" || lowered == "dashed" || lowered == "dotted" || lowered == "double" ||
            lowered == "groove" || lowered == "ridge" || lowered == "inset" || lowered == "outset")
        {
            continue; // every border paints solid; the style keyword is skipped
        }
        const char head = lowered.front();
        if (!saw_width && (head == '-' || head == '.' || std::isdigit(static_cast<unsigned char>(head)) != 0))
        {
            width = std::max(0.0F, parse_number(token));
            saw_width = true;
            continue;
        }
        PanoramaColor token_color;
        if (try_parse_color_token(token, token_color))
        {
            color = token_color;
        }
    }
}

// Switches a style to per-side border storage, seeding every side from the
// uniform shorthand values so a later `border-bottom: ...` only changes that
// side (CSS longhand-over-shorthand cascade).
void ensure_border_per_side(PanoramaComputedStyle& style)
{
    if (style.border_per_side)
    {
        return;
    }
    style.border_per_side = true;
    style.border_width_top = style.border_width_right = style.border_width_bottom = style.border_width_left =
        style.border_width;
    style.border_color_top = style.border_color_right = style.border_color_bottom = style.border_color_left =
        style.border_color;
}

// Switches a style to per-corner radius storage, seeding the four corners from
// the uniform radius so `border-bottom-left-radius: 3px` composes with an
// earlier uniform `border-radius`. Percent radii collapse to pixels-only here
// (matching the existing per-corner limitation).
void ensure_border_radius_per_corner(PanoramaComputedStyle& style)
{
    if (style.border_radius_per_corner)
    {
        return;
    }
    const float uniform = style.border_radius_percent ? 0.0F : style.border_radius;
    style.border_radius_per_corner = true;
    style.border_radius_percent = false;
    style.border_radius_tl = style.border_radius_tr = style.border_radius_br = style.border_radius_bl = uniform;
}

float parse_gradient_offset(std::string_view value)
{
    const std::string text = trim(value);
    const float number = parse_number(text);
    return std::clamp(text.find('%') != std::string::npos ? number / 100.0F : number, 0.0F, 1.0F);
}

bool parse_gradient_point(std::string_view value, float& x, float& y)
{
    const std::vector<std::string> parts = split_css_whitespace_values(value);
    if (parts.size() != 2)
    {
        return false;
    }
    x = parse_number(parts[0]);
    y = parse_number(parts[1]);
    return true;
}

// Matches `name(` allowing whitespace between the name and the paren: shipped
// CS:GO sheets write `to (rgba(...))` / `from ( #0A5A84 )` and the real
// Panorama renderer accepts them (the buy-menu rarity fades depend on it).
bool gradient_stop_head_matches(const std::string& lowered, std::string_view name)
{
    if (!starts_with(lowered, name))
    {
        return false;
    }
    std::size_t i = name.size();
    while (i < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[i])) != 0)
    {
        ++i;
    }
    return i < lowered.size() && lowered[i] == '(';
}

bool parse_gradient_stop(std::string_view value, PanoramaGradientStop& out)
{
    const std::string text = trim(value);
    const std::string lowered = to_lower(text);
    if (gradient_stop_head_matches(lowered, "from"))
    {
        out = {0.0F, parse_panorama_color(paren_arg(text))};
        return true;
    }
    if (gradient_stop_head_matches(lowered, "to"))
    {
        out = {1.0F, parse_panorama_color(paren_arg(text))};
        return true;
    }
    if (gradient_stop_head_matches(lowered, "color-stop"))
    {
        const std::vector<std::string> args = split_top_level(paren_arg(text), ',');
        if (args.size() < 2)
        {
            return false;
        }
        out = {parse_gradient_offset(args[0]), parse_panorama_color(args[1])};
        return true;
    }
    return false;
}

void normalize_gradient_stops(std::vector<PanoramaGradientStop>& stops)
{
    std::stable_sort(stops.begin(), stops.end(), [](const PanoramaGradientStop& lhs, const PanoramaGradientStop& rhs) {
        return lhs.offset < rhs.offset;
    });
    for (PanoramaGradientStop& stop : stops)
    {
        stop.offset = std::clamp(stop.offset, 0.0F, 1.0F);
    }
    if (!stops.empty() && stops.front().offset > 0.0F)
    {
        stops.insert(stops.begin(), PanoramaGradientStop{0.0F, stops.front().color});
    }
    if (!stops.empty() && stops.back().offset < 1.0F)
    {
        stops.push_back(PanoramaGradientStop{1.0F, stops.back().color});
    }
}

bool parse_panorama_gradient(std::string_view value, PanoramaGradient& out)
{
    const std::string text = trim(value);
    const std::string lowered = to_lower(text);
    if (!starts_with(lowered, "gradient("))
    {
        return false;
    }
    const std::vector<std::string> args = split_top_level(paren_arg(text), ',');
    if (args.size() < 4)
    {
        return false;
    }

    PanoramaGradient gradient;
    const std::string kind = to_lower(trim(args[0]));
    std::size_t stop_start = 0;
    if (kind == "linear")
    {
        gradient.type = PanoramaGradientType::Linear;
        if (!parse_gradient_point(args[1], gradient.x0, gradient.y0) ||
            !parse_gradient_point(args[2], gradient.x1, gradient.y1))
        {
            return false;
        }
        stop_start = 3;
    }
    else if (kind == "radial")
    {
        // Panorama radial: `gradient(radial, cx cy, ox oy, rx ry, stops...)` — an
        // ellipse centred at (cx+ox, cy+oy) with radii rx/ry, all % of the box.
        gradient.type = PanoramaGradientType::Radial;
        if (args.size() < 5 ||
            !parse_gradient_point(args[1], gradient.radial_center_x, gradient.radial_center_y) ||
            !parse_gradient_point(args[2], gradient.radial_offset_x, gradient.radial_offset_y) ||
            !parse_gradient_point(args[3], gradient.radial_radius_x, gradient.radial_radius_y))
        {
            return false;
        }
        stop_start = 4;
    }
    else
    {
        return false;
    }

    for (std::size_t i = stop_start; i < args.size(); ++i)
    {
        PanoramaGradientStop stop;
        if (parse_gradient_stop(args[i], stop))
        {
            gradient.stops.push_back(stop);
        }
    }
    normalize_gradient_stops(gradient.stops);
    if (!gradient.present())
    {
        return false;
    }

    out = std::move(gradient);
    return true;
}

PanoramaColor gradient_fallback_color(const PanoramaGradient& gradient)
{
    return gradient.stops.empty() ? PanoramaColor{} : gradient.stops.front().color;
}

std::vector<float> parse_edge_values(std::string_view value)
{
    std::string normalized(value);
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::vector<float> values;
    std::istringstream stream(normalized);
    std::string token;
    while (stream >> token)
    {
        values.push_back(parse_number(token));
    }
    return values;
}

void apply_edges(PanoramaEdges& edges, std::string_view value)
{
    const std::vector<float> v = parse_edge_values(value);
    switch (v.size())
    {
    case 1:
        edges = {v[0], v[0], v[0], v[0]};
        break;
    case 2:
        edges = {v[0], v[1], v[0], v[1]};
        break;
    case 3:
        edges = {v[0], v[1], v[2], v[1]};
        break;
    case 4:
        edges = {v[0], v[1], v[2], v[3]};
        break;
    default:
        break;
    }
}

// Margin edge bits in PanoramaComputedStyle::margin_pct_mask (shared with layout).
constexpr std::uint8_t kMarginTop = kPanoramaMarginTopPct;
constexpr std::uint8_t kMarginRight = kPanoramaMarginRightPct;
constexpr std::uint8_t kMarginBottom = kPanoramaMarginBottomPct;
constexpr std::uint8_t kMarginLeft = kPanoramaMarginLeftPct;

// Sets one margin edge from a token, routing a `%` value to the percent spec (so
// the layout resolves it against the containing-block width) and a px value to
// the resolved field.
void set_margin_edge(PanoramaComputedStyle& style, std::uint8_t bit, float& px_edge, float& pct_edge,
    std::string_view token)
{
    const EdgeLength len = parse_edge_length(token);
    if (len.percent)
    {
        pct_edge = len.value;
        style.margin_pct_mask |= bit;
    }
    else
    {
        px_edge = len.value;
        style.margin_pct_mask &= static_cast<std::uint8_t>(~bit);
    }
}

// Percent-aware `margin` shorthand (1-4 values, CSS top/right/bottom/left order).
void apply_margin_edges(PanoramaComputedStyle& style, std::string_view value)
{
    std::string normalized(value);
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::vector<std::string> tokens;
    std::istringstream stream(normalized);
    std::string token;
    while (stream >> token)
    {
        tokens.push_back(token);
    }
    std::string top, right, bottom, left;
    switch (tokens.size())
    {
    case 1:
        top = right = bottom = left = tokens[0];
        break;
    case 2:
        top = bottom = tokens[0];
        right = left = tokens[1];
        break;
    case 3:
        top = tokens[0];
        right = left = tokens[1];
        bottom = tokens[2];
        break;
    case 4:
        top = tokens[0];
        right = tokens[1];
        bottom = tokens[2];
        left = tokens[3];
        break;
    default:
        return;
    }
    set_margin_edge(style, kMarginTop, style.margin.top, style.margin_pct.top, top);
    set_margin_edge(style, kMarginRight, style.margin.right, style.margin_pct.right, right);
    set_margin_edge(style, kMarginBottom, style.margin.bottom, style.margin_pct.bottom, bottom);
    set_margin_edge(style, kMarginLeft, style.margin.left, style.margin_pct.left, left);
}

struct TransformLength
{
    float value = 0.0F;
    bool percent = false;
};

std::vector<std::string> split_transform_args(std::string_view args)
{
    std::vector<std::string> parts = split_top_level(args, ',');
    if (parts.size() == 1)
    {
        std::istringstream stream(trim(args));
        parts.clear();
        std::string token;
        while (stream >> token)
        {
            parts.push_back(token);
        }
    }
    for (std::string& part : parts)
    {
        part = trim(part);
    }
    return parts;
}

TransformLength parse_transform_length(std::string_view value)
{
    const std::string text = to_lower(trim(value));
    TransformLength out;
    out.value = parse_number(text);
    out.percent = !text.empty() && text.find('%') != std::string::npos;
    return out;
}

// CSS angle to degrees. The unit check must test `grad` before `rad` (the
// substring match would otherwise read 100grad as 100 radians).
float parse_rotation_degrees(std::string_view value)
{
    const std::string text = to_lower(trim(value));
    const float number = parse_number(text);
    if (contains(text, "grad"))
    {
        return number * 0.9F;
    }
    if (contains(text, "turn"))
    {
        return number * 360.0F;
    }
    if (contains(text, "rad"))
    {
        return number * 180.0F / 3.1415926535F;
    }
    return number;
}

PanoramaTransform parse_panorama_transform(std::string_view value)
{
    PanoramaTransform transform;
    const std::string text = to_lower(trim(value));
    if (text.empty() || text == "none")
    {
        return transform;
    }

    std::size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && is_space(text[i]))
        {
            ++i;
        }
        const std::size_t name_start = i;
        while (i < text.size() && (std::isalpha(static_cast<unsigned char>(text[i])) != 0 || text[i] == '-' || text[i] == '2' ||
                                      text[i] == '3'))
        {
            ++i;
        }
        const std::string name = text.substr(name_start, i - name_start);
        while (i < text.size() && is_space(text[i]))
        {
            ++i;
        }
        if (name.empty() || i >= text.size() || text[i] != '(')
        {
            break;
        }
        const std::size_t open = i++;
        int depth = 1;
        while (i < text.size() && depth > 0)
        {
            if (text[i] == '(')
            {
                ++depth;
            }
            else if (text[i] == ')')
            {
                --depth;
            }
            ++i;
        }
        if (depth != 0)
        {
            break;
        }
        const std::string args_text = text.substr(open + 1, i - open - 2);
        const std::vector<std::string> args = split_transform_args(args_text);

        if ((name == "translatex" || name == "translatey") && !args.empty())
        {
            const TransformLength len = parse_transform_length(args[0]);
            PanoramaTransformOp op;
            op.type = PanoramaTransformOp::Type::Translate;
            if (name == "translatex")
            {
                op.x = len.value;
                op.x_percent = len.percent;
            }
            else
            {
                op.y = len.value;
                op.y_percent = len.percent;
            }
            transform.ops.push_back(op);
        }
        else if ((name == "translate" || name == "translate3d") && !args.empty())
        {
            const TransformLength x = parse_transform_length(args[0]);
            const TransformLength y = args.size() > 1 ? parse_transform_length(args[1]) : TransformLength{};
            PanoramaTransformOp op;
            op.type = PanoramaTransformOp::Type::Translate;
            op.x = x.value;
            op.y = y.value;
            op.x_percent = x.percent;
            op.y_percent = y.percent;
            transform.ops.push_back(op);
        }
        else if ((name == "scalex" || name == "scaley") && !args.empty())
        {
            PanoramaTransformOp op;
            op.type = PanoramaTransformOp::Type::Scale;
            op.x = name == "scaley" ? 1.0F : parse_number(args[0]);
            op.y = name == "scalex" ? 1.0F : parse_number(args[0]);
            transform.ops.push_back(op);
        }
        else if ((name == "scale" || name == "scale3d") && !args.empty())
        {
            PanoramaTransformOp op;
            op.type = PanoramaTransformOp::Type::Scale;
            op.x = parse_number(args[0]);
            op.y = args.size() > 1 ? parse_number(args[1]) : op.x;
            transform.ops.push_back(op);
        }
        else if ((name == "rotate" || name == "rotatez") && !args.empty())
        {
            PanoramaTransformOp op;
            op.type = PanoramaTransformOp::Type::Rotate;
            op.x = parse_rotation_degrees(args[0]);
            transform.ops.push_back(op);
        }
        else if ((name == "rotatex" || name == "rotatey") && !args.empty())
        {
            // 3D rotations projected to the 2D plane (the engine has no
            // perspective): rotateX(a) flattens vertically to scaleY(cos a),
            // rotateY(a) flattens horizontally to scaleX(cos a) — the same
            // projection WebCore's rotate3d yields at z=0 (RotateTransform-
            // Operation::apply). rotateX(180deg) thus mirrors to scaleY(-1).
            const float radians = parse_rotation_degrees(args[0]) * 3.1415926535F / 180.0F;
            PanoramaTransformOp op;
            op.type = PanoramaTransformOp::Type::Scale;
            op.x = name == "rotatey" ? std::cos(radians) : 1.0F;
            op.y = name == "rotatex" ? std::cos(radians) : 1.0F;
            transform.ops.push_back(op);
        }
        // translatez(...) is deliberately ignored: a z-offset has no effect
        // without perspective projection.
    }
    return transform;
}

TransformLength parse_origin_component(std::string_view token)
{
    const std::string text = to_lower(trim(token));
    if (text == "left" || text == "top")
    {
        return {0.0F, true};
    }
    if (text == "center" || text == "middle")
    {
        return {50.0F, true};
    }
    if (text == "right" || text == "bottom")
    {
        return {100.0F, true};
    }
    if (text.empty())
    {
        return {50.0F, true};
    }
    return parse_transform_length(text);
}

PanoramaEasing parse_easing(std::string_view value)
{
    // The keyword timing functions are the CSS-standard cubic-bezier control points
    // (https://drafts.csswg.org/css-easing-1). `linear` stays the identity fast-path.
    const std::string t = to_lower(trim(value));
    if (t == "ease")
    {
        return {0.25F, 0.1F, 0.25F, 1.0F, false};
    }
    if (t == "ease-in")
    {
        return {0.42F, 0.0F, 1.0F, 1.0F, false};
    }
    if (t == "ease-out")
    {
        return {0.0F, 0.0F, 0.58F, 1.0F, false};
    }
    if (t == "ease-in-out")
    {
        return {0.42F, 0.0F, 0.58F, 1.0F, false};
    }
    if (starts_with(t, "cubic-bezier"))
    {
        // cubic-bezier(x1, y1, x2, y2): solved exactly by PanoramaEasing::evaluate.
        const std::vector<std::string> parts = split_top_level(paren_arg(t), ',');
        if (parts.size() >= 4)
        {
            return {parse_number(trim(parts[0])), parse_number(trim(parts[1])),
                    parse_number(trim(parts[2])), parse_number(trim(parts[3])), false};
        }
    }
    return {};
}

PanoramaTransformOrigin parse_transform_origin(std::string_view value)
{
    std::vector<std::string> parts;
    std::istringstream stream(to_lower(trim(value)));
    std::string token;
    while (stream >> token)
    {
        parts.push_back(token);
    }
    PanoramaTransformOrigin origin;
    if (!parts.empty())
    {
        const TransformLength x = parse_origin_component(parts[0]);
        origin.x = x.value;
        origin.x_percent = x.percent;
    }
    if (parts.size() > 1)
    {
        const TransformLength y = parse_origin_component(parts[1]);
        origin.y = y.value;
        origin.y_percent = y.percent;
    }
    return origin;
}

std::vector<std::string> split_css_whitespace_values(std::string_view value)
{
    std::string normalized = to_lower(trim(value));
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::istringstream stream(normalized);
    std::vector<std::string> parts;
    std::string token;
    while (stream >> token)
    {
        parts.push_back(token);
    }
    return parts;
}

enum class PositionSide
{
    None,
    Left,
    Right,
    Top,
    Bottom,
    Center,
};

PositionSide position_side(std::string_view token)
{
    if (token == "left")
    {
        return PositionSide::Left;
    }
    if (token == "right")
    {
        return PositionSide::Right;
    }
    if (token == "top")
    {
        return PositionSide::Top;
    }
    if (token == "bottom")
    {
        return PositionSide::Bottom;
    }
    if (token == "center" || token == "middle")
    {
        return PositionSide::Center;
    }
    return PositionSide::None;
}

bool is_horizontal_position_keyword_only(PositionSide side)
{
    return side == PositionSide::Left || side == PositionSide::Right;
}

bool is_vertical_position_keyword_only(PositionSide side)
{
    return side == PositionSide::Top || side == PositionSide::Bottom;
}

struct BackgroundAxisValue
{
    float value = 50.0F;
    bool percent = true;
    bool from_end = false;
    bool side_offset = false;
    bool set = false;
};

BackgroundAxisValue centered_background_axis()
{
    return {50.0F, true, false, false, true};
}

BackgroundAxisValue background_axis_from_length(std::string_view token)
{
    return {parse_number(token), token.find('%') != std::string_view::npos, false, false, true};
}

BackgroundAxisValue background_axis_from_keyword(PositionSide side, bool horizontal)
{
    if (side == PositionSide::Center)
    {
        return centered_background_axis();
    }
    const bool end = horizontal ? side == PositionSide::Right : side == PositionSide::Bottom;
    return {end ? 100.0F : 0.0F, true, false, false, true};
}

BackgroundAxisValue background_axis_from_side_offset(PositionSide side, std::string_view offset)
{
    return {parse_number(offset), offset.find('%') != std::string_view::npos,
        side == PositionSide::Right || side == PositionSide::Bottom, true, true};
}

bool background_axis_from_token(std::string_view token, bool horizontal, BackgroundAxisValue& out)
{
    const PositionSide side = position_side(token);
    if (side == PositionSide::None)
    {
        out = background_axis_from_length(token);
        return true;
    }
    if (side == PositionSide::Center || (horizontal && is_horizontal_position_keyword_only(side)) ||
        (!horizontal && is_vertical_position_keyword_only(side)))
    {
        out = background_axis_from_keyword(side, horizontal);
        return true;
    }
    return false;
}

bool set_background_axis(BackgroundAxisValue& target, const BackgroundAxisValue& value)
{
    if (target.set)
    {
        return false;
    }
    target = value;
    return true;
}

void assign_background_position(PanoramaBackgroundPosition& out, const BackgroundAxisValue& x, const BackgroundAxisValue& y)
{
    out.x = x.value;
    out.y = y.value;
    out.x_percent = x.percent;
    out.y_percent = y.percent;
    out.x_from_end = x.from_end;
    out.y_from_end = y.from_end;
    out.x_side_offset = x.side_offset;
    out.y_side_offset = y.side_offset;
}

bool parse_keyword_offset_background_position(
    const std::vector<std::string>& parts, BackgroundAxisValue& x, BackgroundAxisValue& y)
{
    bool center_pending = false;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        const PositionSide side = position_side(parts[i]);
        if (side == PositionSide::None)
        {
            return false;
        }
        if (side == PositionSide::Center)
        {
            if (center_pending)
            {
                return false;
            }
            center_pending = true;
            continue;
        }

        BackgroundAxisValue axis;
        if (i + 1 < parts.size() && position_side(parts[i + 1]) == PositionSide::None)
        {
            axis = background_axis_from_side_offset(side, parts[i + 1]);
            ++i;
        }
        else
        {
            axis = background_axis_from_keyword(side, is_horizontal_position_keyword_only(side));
        }

        if (is_horizontal_position_keyword_only(side))
        {
            if (!set_background_axis(x, axis))
            {
                return false;
            }
        }
        else if (!set_background_axis(y, axis))
        {
            return false;
        }
    }

    if (center_pending)
    {
        if (x.set && y.set)
        {
            return false;
        }
        if (!x.set)
        {
            x = centered_background_axis();
        }
        else
        {
            y = centered_background_axis();
        }
    }
    return x.set && y.set;
}

bool parse_panorama_tolerant_three_value_background_position(
    const std::vector<std::string>& parts, BackgroundAxisValue& x, BackgroundAxisValue& y)
{
    if (parts.size() != 3)
    {
        return false;
    }
    const PositionSide side = position_side(parts[0]);
    if ((is_horizontal_position_keyword_only(side) || is_vertical_position_keyword_only(side)) &&
        position_side(parts[1]) == PositionSide::None && position_side(parts[2]) == PositionSide::None)
    {
        if (is_horizontal_position_keyword_only(side))
        {
            x = background_axis_from_side_offset(side, parts[1]);
            return background_axis_from_token(parts[2], false, y);
        }
        y = background_axis_from_side_offset(side, parts[1]);
        return background_axis_from_token(parts[2], true, x);
    }
    return false;
}

bool parse_background_position(std::string_view value, PanoramaBackgroundPosition& out)
{
    const std::vector<std::string> parts = split_css_whitespace_values(value);
    if (parts.empty() || parts.size() > 4)
    {
        return false;
    }

    BackgroundAxisValue x;
    BackgroundAxisValue y;
    if (parts.size() == 1)
    {
        const PositionSide side = position_side(parts[0]);
        if (is_vertical_position_keyword_only(side))
        {
            x = centered_background_axis();
            y = background_axis_from_keyword(side, false);
        }
        else
        {
            if (!background_axis_from_token(parts[0], true, x))
            {
                return false;
            }
            y = centered_background_axis();
        }
    }
    else if (parts.size() == 2)
    {
        const PositionSide side1 = position_side(parts[0]);
        const PositionSide side2 = position_side(parts[1]);
        const bool value1_id = side1 != PositionSide::None;
        const bool value2_id = side2 != PositionSide::None;
        const bool must_order_xy =
            is_horizontal_position_keyword_only(side1) || is_vertical_position_keyword_only(side2) || !value1_id || !value2_id;
        const bool must_order_yx = is_vertical_position_keyword_only(side1) || is_horizontal_position_keyword_only(side2);
        if (must_order_xy && must_order_yx)
        {
            return false;
        }
        if (must_order_yx)
        {
            if (!background_axis_from_token(parts[1], true, x) || !background_axis_from_token(parts[0], false, y))
            {
                return false;
            }
        }
        else if (!background_axis_from_token(parts[0], true, x) || !background_axis_from_token(parts[1], false, y))
        {
            return false;
        }
    }
    else if (!parse_keyword_offset_background_position(parts, x, y) &&
        !parse_panorama_tolerant_three_value_background_position(parts, x, y))
    {
        return false;
    }

    assign_background_position(out, x, y);
    return true;
}
}

void apply_panorama_declaration(PanoramaComputedStyle& style, std::string_view property, std::string_view value_in)
{
    const std::string prop = to_lower(trim(property));
    const std::string value = trim(value_in);
    if (prop.empty() || value.empty())
    {
        return;
    }

    if (prop == "width")
    {
        style.width = parse_panorama_length(value);
    }
    else if (prop == "height")
    {
        style.height = parse_panorama_length(value);
    }
    else if (prop == "min-width")
    {
        style.min_width = parse_panorama_length(value);
    }
    else if (prop == "max-width")
    {
        style.max_width = parse_panorama_length(value);
    }
    else if (prop == "min-height")
    {
        style.min_height = parse_panorama_length(value);
    }
    else if (prop == "max-height")
    {
        style.max_height = parse_panorama_length(value);
    }
    else if (prop == "margin")
    {
        apply_margin_edges(style, value);
    }
    else if (prop == "margin-top")
    {
        set_margin_edge(style, kMarginTop, style.margin.top, style.margin_pct.top, value);
    }
    else if (prop == "margin-right")
    {
        set_margin_edge(style, kMarginRight, style.margin.right, style.margin_pct.right, value);
    }
    else if (prop == "margin-bottom")
    {
        set_margin_edge(style, kMarginBottom, style.margin.bottom, style.margin_pct.bottom, value);
    }
    else if (prop == "margin-left")
    {
        set_margin_edge(style, kMarginLeft, style.margin.left, style.margin_pct.left, value);
    }
    else if (prop == "padding")
    {
        apply_edges(style.padding, value);
    }
    else if (prop == "padding-top")
    {
        style.padding.top = parse_number(value);
    }
    else if (prop == "padding-right")
    {
        style.padding.right = parse_number(value);
    }
    else if (prop == "padding-bottom")
    {
        style.padding.bottom = parse_number(value);
    }
    else if (prop == "padding-left")
    {
        style.padding.left = parse_number(value);
    }
    else if (prop == "flow-children")
    {
        const std::string lowered = to_lower(value);
        if (lowered == "right")
        {
            style.flow = PanoramaFlow::Right;
        }
        else if (lowered == "left")
        {
            style.flow = PanoramaFlow::Left;
        }
        else if (lowered == "down")
        {
            style.flow = PanoramaFlow::Down;
        }
        else if (lowered == "up")
        {
            style.flow = PanoramaFlow::Up;
        }
        else if (lowered == "right-wrap")
        {
            style.flow = PanoramaFlow::RightWrap;
        }
        else if (lowered == "down-wrap")
        {
            style.flow = PanoramaFlow::Down_Wrap;
        }
        else
        {
            style.flow = PanoramaFlow::None;
        }
    }
    else if (prop == "align" || prop == "horizontal-align" || prop == "vertical-align")
    {
        std::istringstream stream(to_lower(value));
        std::string token;
        std::vector<std::string> tokens;
        while (stream >> token)
        {
            tokens.push_back(token);
        }
        const auto set_h = [&](const std::string& t) {
            if (t == "left")
            {
                style.halign = PanoramaHAlign::Left;
            }
            else if (t == "center" || t == "middle")
            {
                style.halign = PanoramaHAlign::Center;
            }
            else if (t == "right")
            {
                style.halign = PanoramaHAlign::Right;
            }
        };
        const auto set_v = [&](const std::string& t) {
            if (t == "top")
            {
                style.valign = PanoramaVAlign::Top;
            }
            else if (t == "center" || t == "middle")
            {
                style.valign = PanoramaVAlign::Middle;
            }
            else if (t == "bottom")
            {
                style.valign = PanoramaVAlign::Bottom;
            }
        };
        if (prop == "horizontal-align" && !tokens.empty())
        {
            set_h(tokens[0]);
        }
        else if (prop == "vertical-align" && !tokens.empty())
        {
            set_v(tokens[0]);
        }
        else
        {
            if (tokens.size() >= 1)
            {
                set_h(tokens[0]);
            }
            if (tokens.size() >= 2)
            {
                set_v(tokens[1]);
            }
        }
    }
    else if (prop == "position")
    {
        // Panorama position is "x y z"; x/y may be px or % (of parent width/height).
        std::string normalized = value;
        std::replace(normalized.begin(), normalized.end(), ',', ' ');
        std::istringstream stream(normalized);
        std::string token;
        std::vector<std::string> coords;
        while (stream >> token)
        {
            coords.push_back(token);
        }
        if (!coords.empty())
        {
            style.has_position = true;
            if (coords.size() > 0)
            {
                style.pos_x = parse_number(coords[0]);
                style.pos_x_percent = coords[0].find('%') != std::string::npos;
            }
            if (coords.size() > 1)
            {
                style.pos_y = parse_number(coords[1]);
                style.pos_y_percent = coords[1].find('%') != std::string::npos;
            }
            if (coords.size() > 2)
            {
                style.pos_z = parse_number(coords[2]);
            }
        }
    }
    else if (prop == "background-color")
    {
        PanoramaGradient gradient;
        if (parse_panorama_gradient(value, gradient))
        {
            style.background_gradient = gradient;
            style.background_color = gradient_fallback_color(gradient);
        }
        else
        {
            style.background_gradient = {};
            style.background_color = parse_panorama_color(value);
        }
    }
    else if (prop == "background-image")
    {
        PanoramaGradient gradient;
        if (parse_panorama_gradient(value, gradient))
        {
            style.background_gradient = gradient;
            style.background_color = gradient_fallback_color(gradient);
            style.background_image.clear();
        }
        else
        {
            style.background_image = to_lower(trim(value)) == "none" ? std::string{} : parse_css_url(value);
        }
    }
    else if (prop == "background-size")
    {
        const std::string t = to_lower(value);
        if (t == "contain")
        {
            style.background_size = {PanoramaBackgroundSizeType::Contain, {}, {}};
        }
        else if (t == "cover" || t == "clip_then_cover")
        {
            style.background_size = {PanoramaBackgroundSizeType::Cover, {}, {}};
        }
        else
        {
            const std::vector<std::string> parts = split_css_whitespace_values(t);
            if (parts.empty())
            {
                style.background_size = {PanoramaBackgroundSizeType::Stretch, {}, {}};
            }
            else
            {
                const PanoramaLength w = parts[0] == "auto" ? PanoramaLength{} : parse_panorama_length(parts[0]);
                const PanoramaLength h =
                    parts.size() > 1 && parts[1] != "auto" ? parse_panorama_length(parts[1]) : PanoramaLength{};
                if (w.type == PanoramaLengthType::Auto && h.type == PanoramaLengthType::Auto)
                {
                    style.background_size = {PanoramaBackgroundSizeType::Stretch, {}, {}};
                    return;
                }
                // `100% 100%` is just our default stretch; a lone `100%` remains
                // CSS/WebCore's `100% auto`.
                const bool full = w.type == PanoramaLengthType::Percent && std::abs(w.value - 100.0F) < 0.01F &&
                    parts.size() > 1 && h.type == PanoramaLengthType::Percent && std::abs(h.value - 100.0F) < 0.01F;
                style.background_size =
                    full ? PanoramaBackgroundSize{} : PanoramaBackgroundSize{PanoramaBackgroundSizeType::Fixed, w, h};
            }
        }
    }
    else if (prop == "background-position")
    {
        PanoramaBackgroundPosition parsed = style.background_position;
        if (parse_background_position(value, parsed))
        {
            style.background_position = parsed;
        }
    }
    else if (prop == "background-repeat")
    {
        const auto axis_mode = [](const std::string& token, PanoramaBackgroundRepeat& out) {
            if (token == "repeat")
            {
                out = PanoramaBackgroundRepeat::Repeat;
            }
            else if (token == "no-repeat")
            {
                out = PanoramaBackgroundRepeat::NoRepeat;
            }
            else if (token == "space")
            {
                out = PanoramaBackgroundRepeat::Space;
            }
            else if (token == "round")
            {
                out = PanoramaBackgroundRepeat::Round;
            }
            else
            {
                return false;
            }
            return true;
        };
        const std::vector<std::string> parts = split_css_whitespace_values(to_lower(value));
        PanoramaBackgroundRepeatXY parsed;
        if (parts.size() == 1)
        {
            // The one-value form sets both axes; repeat-x / repeat-y are
            // single-value-only shorthands (CSS background-repeat grammar).
            if (parts[0] == "repeat-x")
            {
                parsed = {PanoramaBackgroundRepeat::Repeat, PanoramaBackgroundRepeat::NoRepeat};
            }
            else if (parts[0] == "repeat-y")
            {
                parsed = {PanoramaBackgroundRepeat::NoRepeat, PanoramaBackgroundRepeat::Repeat};
            }
            else if (axis_mode(parts[0], parsed.x))
            {
                parsed.y = parsed.x;
            }
            else
            {
                return; // invalid declaration: ignored, previous value kept
            }
            style.background_repeat = parsed;
        }
        else if (parts.size() == 2 && axis_mode(parts[0], parsed.x) && axis_mode(parts[1], parsed.y))
        {
            style.background_repeat = parsed;
        }
    }
    else if (prop == "background-img-opacity" || prop == "background-image-opacity")
    {
        style.background_image_opacity = std::clamp(parse_number(value), 0.0F, 1.0F);
    }
    else if (prop == "background")
    {
        const std::string image = parse_css_url(value);
        const bool none = to_lower(trim(value)) == "none";
        if (!image.empty() || none)
        {
            style.background_image = image;
            if (image.empty())
            {
                style.background_gradient = {};
                style.background_color = {};
            }
        }
        else
        {
            PanoramaGradient gradient;
            if (parse_panorama_gradient(value, gradient))
            {
                style.background_gradient = gradient;
                style.background_color = gradient_fallback_color(gradient);
            }
            else
            {
                style.background_gradient = {};
                style.background_color = parse_panorama_color(value);
            }
        }
    }
    else if (prop == "color")
    {
        style.color = parse_panorama_color(value);
    }
    else if (prop == "wash-color" || prop == "wash-color-fast")
    {
        // `none` means no wash (show the texture as-is) -> white tint. The `-fast`
        // variant only differs in suppressing the implicit transition, which we do
        // not model on this property, so both map to the same tint.
        style.wash_color = to_lower(value) == "none" ? PanoramaColor{0xFF, 0xFF, 0xFF, 0xFF} : parse_panorama_color(value);
    }
    else if (prop == "brightness")
    {
        style.brightness = std::max(0.0F, parse_number(value));
    }
    else if (prop == "border-color")
    {
        style.border_color = parse_panorama_color(value);
        if (style.border_per_side)
        {
            style.border_color_top = style.border_color_right = style.border_color_bottom = style.border_color_left =
                style.border_color;
        }
    }
    else if (prop == "border-width")
    {
        style.border_width = parse_number(value);
        if (style.border_per_side)
        {
            style.border_width_top = style.border_width_right = style.border_width_bottom = style.border_width_left =
                style.border_width;
        }
    }
    else if (prop == "border-radius")
    {
        // CSS expansion of 1-4 radii (corner order TL TR BR BL; a `/ v` vertical
        // list collapses onto the horizontal radii — the engine's corners are
        // circular). `%` is supported on the uniform form only and is resolved
        // against the box at paint time.
        std::string horizontal = trim(value);
        const std::size_t slash = horizontal.find('/');
        if (slash != std::string::npos)
        {
            horizontal = trim(horizontal.substr(0, slash));
        }
        std::istringstream tokens{horizontal};
        std::vector<std::string> parts;
        std::string token;
        while (tokens >> token && parts.size() < 4)
        {
            parts.push_back(token);
        }
        style.border_radius_per_corner = false;
        if (parts.empty())
        {
            style.border_radius = 0.0F;
            style.border_radius_percent = false;
        }
        else
        {
            style.border_radius = parse_number(parts[0]);
            style.border_radius_percent = parts[0].find('%') != std::string::npos;
            if (parts.size() > 1)
            {
                // 2 values: TL/BR, TR/BL. 3 values: TL, TR/BL, BR. 4: TL TR BR BL.
                const float a = style.border_radius;
                const float b = parse_number(parts[1]);
                const float c = parts.size() > 2 ? parse_number(parts[2]) : a;
                const float d = parts.size() > 3 ? parse_number(parts[3]) : b;
                if (!(a == b && b == c && c == d))
                {
                    style.border_radius_per_corner = true;
                    style.border_radius_percent = false;
                    style.border_radius_tl = a;
                    style.border_radius_tr = b;
                    style.border_radius_br = c;
                    style.border_radius_bl = d;
                }
            }
        }
    }
    else if (prop == "border")
    {
        // "<width> <style> <color>" shorthand; resets any per-side longhands
        // (the CSS border shorthand resets every border-* longhand).
        parse_border_shorthand(value, style.border_width, style.border_color);
        style.border_per_side = false;
        style.border_width_top = style.border_width_right = style.border_width_bottom = style.border_width_left =
            style.border_width;
        style.border_color_top = style.border_color_right = style.border_color_bottom = style.border_color_left =
            style.border_color;
    }
    else if (prop == "border-top" || prop == "border-right" || prop == "border-bottom" || prop == "border-left")
    {
        ensure_border_per_side(style);
        float width = 0.0F;
        PanoramaColor color;
        parse_border_shorthand(value, width, color);
        if (prop == "border-top")
        {
            style.border_width_top = width;
            style.border_color_top = color;
        }
        else if (prop == "border-right")
        {
            style.border_width_right = width;
            style.border_color_right = color;
        }
        else if (prop == "border-bottom")
        {
            style.border_width_bottom = width;
            style.border_color_bottom = color;
        }
        else
        {
            style.border_width_left = width;
            style.border_color_left = color;
        }
    }
    else if (prop == "border-top-width" || prop == "border-right-width" || prop == "border-bottom-width" ||
        prop == "border-left-width")
    {
        ensure_border_per_side(style);
        const float width = std::max(0.0F, parse_number(value));
        if (prop == "border-top-width")
        {
            style.border_width_top = width;
        }
        else if (prop == "border-right-width")
        {
            style.border_width_right = width;
        }
        else if (prop == "border-bottom-width")
        {
            style.border_width_bottom = width;
        }
        else
        {
            style.border_width_left = width;
        }
    }
    else if (prop == "border-top-color" || prop == "border-right-color" || prop == "border-bottom-color" ||
        prop == "border-left-color")
    {
        ensure_border_per_side(style);
        const PanoramaColor color = parse_panorama_color(value);
        if (prop == "border-top-color")
        {
            style.border_color_top = color;
        }
        else if (prop == "border-right-color")
        {
            style.border_color_right = color;
        }
        else if (prop == "border-bottom-color")
        {
            style.border_color_bottom = color;
        }
        else
        {
            style.border_color_left = color;
        }
    }
    else if (prop == "border-top-left-radius" || prop == "border-top-right-radius" ||
        prop == "border-bottom-right-radius" || prop == "border-bottom-left-radius")
    {
        ensure_border_radius_per_corner(style);
        const float radius = std::max(0.0F, parse_number(value));
        if (prop == "border-top-left-radius")
        {
            style.border_radius_tl = radius;
        }
        else if (prop == "border-top-right-radius")
        {
            style.border_radius_tr = radius;
        }
        else if (prop == "border-bottom-right-radius")
        {
            style.border_radius_br = radius;
        }
        else
        {
            style.border_radius_bl = radius;
        }
    }
    else if (prop == "font-size")
    {
        style.font_size = parse_number(value);
    }
    else if (prop == "font-weight")
    {
        style.font_weight = parse_font_weight(value, style.font_weight);
    }
    else if (prop == "text-transform")
    {
        const std::string t = to_lower(value);
        if (t == "uppercase")
        {
            style.text_transform = PanoramaTextTransform::Uppercase;
        }
        else if (t == "lowercase")
        {
            style.text_transform = PanoramaTextTransform::Lowercase;
        }
        else
        {
            style.text_transform = PanoramaTextTransform::None;
        }
    }
    else if (prop == "text-decoration")
    {
        // Line keywords only — the corpus uses underline(8) / line-through(4) /
        // none(2); decoration style/color/thickness longhands are not used.
        bool underline = false;
        bool line_through = false;
        bool valid = true;
        for (const std::string& token : split_css_whitespace_values(to_lower(value)))
        {
            if (token == "underline")
            {
                underline = true;
            }
            else if (token == "line-through")
            {
                line_through = true;
            }
            else if (token != "none")
            {
                valid = false;
            }
        }
        if (valid)
        {
            style.text_decoration_underline = underline;
            style.text_decoration_line_through = line_through;
        }
    }
    else if (prop == "white-space")
    {
        // Corpus values: nowrap(23) / normal(5). Anything unrecognized keeps the
        // wrapping default (normal).
        style.white_space_nowrap = to_lower(value) == "nowrap";
    }
    else if (prop == "letter-spacing")
    {
        style.letter_spacing = parse_number(value);
    }
    else if (prop == "line-height")
    {
        // `normal` -> auto (font metrics). A unitless number is a multiple of font-size;
        // a px value is absolute.
        const std::string t = to_lower(value);
        if (t == "normal")
        {
            style.line_height = 0.0F;
        }
        else if (t.find("px") == std::string::npos && t.find('%') == std::string::npos)
        {
            style.line_height = parse_number(t) * style.font_size; // unitless multiplier
        }
        else
        {
            style.line_height = parse_number(t);
        }
    }
    else if (prop == "text-shadow")
    {
        style.text_shadow = parse_panorama_text_shadow(value);
    }
    else if (prop == "img-shadow")
    {
        // Same value shape as text-shadow (`h v [blur] color`), applied to an
        // Image panel's texture as an offset silhouette.
        style.img_shadow = parse_panorama_text_shadow(value);
    }
    else if (prop == "font-style")
    {
        // Panorama uses the keyword `italics`; accept the CSS spellings too.
        const std::string t = to_lower(value);
        style.font_italic = t == "italic" || t == "italics" || t == "oblique";
    }
    else if (prop == "box-shadow")
    {
        style.box_shadow = parse_panorama_box_shadow(value);
    }
    else if (prop == "blur")
    {
        style.blur = parse_panorama_blur(value);
    }
    else if (prop == "clip")
    {
        style.clip = parse_panorama_clip(value);
    }
    else if (prop == "text-overflow")
    {
        const std::string t = to_lower(value);
        if (t == "ellipsis")
        {
            style.text_overflow = PanoramaTextOverflow::Ellipsis;
        }
        else if (t == "shrink")
        {
            style.text_overflow = PanoramaTextOverflow::Shrink;
        }
        else if (t == "noclip")
        {
            style.text_overflow = PanoramaTextOverflow::NoClip;
        }
        else
        {
            style.text_overflow = PanoramaTextOverflow::Clip;
        }
    }
    else if (prop == "text-align")
    {
        const std::string t = to_lower(value);
        if (t == "center" || t == "middle")
        {
            style.text_align = PanoramaHAlign::Center;
        }
        else if (t == "right")
        {
            style.text_align = PanoramaHAlign::Right;
        }
        else
        {
            style.text_align = PanoramaHAlign::Left;
        }
    }
    else if (prop == "opacity")
    {
        style.opacity = std::clamp(parse_number(value), 0.0F, 1.0F);
    }
    else if (prop == "-mix-blend-mode" || prop == "mix-blend-mode" || prop == "-s2-mix-blend-mode")
    {
        const std::string t = to_lower(value);
        if (t == "additive" || t == "srgbadditive")
        {
            style.blend_mode = PanoramaBlendMode::Additive;
        }
        else if (t == "screen")
        {
            style.blend_mode = PanoramaBlendMode::Screen;
        }
        else if (t == "multiply")
        {
            style.blend_mode = PanoramaBlendMode::Multiply;
        }
        else if (t == "opaque")
        {
            style.blend_mode = PanoramaBlendMode::Opaque;
        }
        else
        {
            style.blend_mode = PanoramaBlendMode::Normal;
        }
    }
    else if (prop == "overflow")
    {
        // `overflow: <horizontal> <vertical>` with axis values squish/scroll/clip
        // (all clip) or noclip (visible). A single value applies to both axes.
        std::istringstream stream(to_lower(value));
        std::vector<std::string> tokens;
        std::string tok;
        while (stream >> tok)
        {
            tokens.push_back(tok);
        }
        const auto clips = [](const std::string& t) { return t == "squish" || t == "scroll" || t == "clip"; };
        const auto squishes = [](const std::string& t) { return t == "squish"; };
        const auto scrolls = [](const std::string& t) { return t == "scroll"; };
        if (!tokens.empty())
        {
            style.overflow_clip_x = clips(tokens[0]);
            style.overflow_clip_y = tokens.size() > 1 ? clips(tokens[1]) : clips(tokens[0]);
            style.overflow_squish_x = squishes(tokens[0]);
            style.overflow_squish_y = tokens.size() > 1 ? squishes(tokens[1]) : squishes(tokens[0]);
            style.overflow_scroll_x = scrolls(tokens[0]);
            style.overflow_scroll_y = tokens.size() > 1 ? scrolls(tokens[1]) : scrolls(tokens[0]);
        }
    }
    else if (prop == "z-index")
    {
        // `auto` (and anything non-numeric) resolves to the default 0.
        style.z_index = to_lower(trim(value)) == "auto" ? 0 : static_cast<int>(parse_number(value));
    }
    else if (prop == "visibility")
    {
        const std::string lowered = to_lower(value);
        style.visible = !(lowered == "collapse" || lowered == "hidden");
    }
    else if (prop == "transform")
    {
        style.transform = parse_panorama_transform(value);
    }
    else if (prop == "transform-origin")
    {
        style.transform_origin = parse_transform_origin(value);
    }
    else if (prop == "pre-transform-scale2d")
    {
        // `s` (uniform) or `sx, sy`.
        const std::vector<std::string> parts = split_top_level(value, ',');
        if (!parts.empty())
        {
            style.pre_scale_x = parse_number(trim(parts[0]));
            style.pre_scale_y = parts.size() > 1 ? parse_number(trim(parts[1])) : style.pre_scale_x;
        }
    }
    else if (prop == "transition-property")
    {
        style.transition_properties.clear();
        for (const std::string& part : split_top_level(value, ','))
        {
            style.transition_properties.push_back(to_lower(trim(part)));
        }
    }
    else if (prop == "transition-duration" || prop == "transition-delay")
    {
        std::vector<float>& out = prop == "transition-duration" ? style.transition_durations : style.transition_delays;
        out.clear();
        for (const std::string& part : split_top_level(value, ','))
        {
            // `2s`, `.25s`, `250ms`, or a bare number (seconds).
            const std::string t = to_lower(trim(part));
            float seconds = parse_number(t);
            if (t.size() >= 2 && t.compare(t.size() - 2, 2, "ms") == 0)
            {
                seconds /= 1000.0F;
            }
            out.push_back(seconds);
        }
    }
    else if (prop == "transition-timing-function")
    {
        style.transition_easings.clear();
        for (const std::string& part : split_top_level(value, ','))
        {
            style.transition_easings.push_back(parse_easing(part));
        }
    }
    else if (prop == "x" || prop == "y" || prop == "z")
    {
        // Panorama position longhands: `x`/`y`/`z` set one coordinate of `position`.
        style.has_position = true;
        const float number = parse_number(value);
        const bool percent = value.find('%') != std::string::npos;
        if (prop == "x")
        {
            style.pos_x = number;
            style.pos_x_percent = percent;
        }
        else if (prop == "y")
        {
            style.pos_y = number;
            style.pos_y_percent = percent;
        }
        else
        {
            style.pos_z = number;
        }
    }
    else if (prop == "animation-name")
    {
        bool quoted = false;
        const std::string name = normalize_css_identifier_or_string(value, &quoted);
        const std::string keyword = to_lower(trim(value));
        style.animation_name = !quoted && keyword == "none" ? std::string() : name;
    }
    else if (prop == "animation-duration" || prop == "animation-delay")
    {
        float& out = prop == "animation-duration" ? style.animation_duration : style.animation_delay;
        const std::string t = to_lower(trim(value));
        float seconds = parse_number(t);
        if (t.size() >= 2 && t.compare(t.size() - 2, 2, "ms") == 0)
        {
            seconds /= 1000.0F;
        }
        out = seconds;
    }
    else if (prop == "animation-timing-function")
    {
        style.animation_easing = parse_easing(value);
    }
    else if (prop == "animation-iteration-count")
    {
        const std::string t = to_lower(trim(value));
        style.animation_iteration_count = t == "infinite" ? -1.0F : parse_number(t);
    }
    else if (prop == "animation-direction")
    {
        const std::string t = to_lower(trim(value));
        if (t == "reverse")
        {
            style.animation_direction = PanoramaAnimDirection::Reverse;
        }
        else if (t == "alternate")
        {
            style.animation_direction = PanoramaAnimDirection::Alternate;
        }
        else if (t == "alternate-reverse")
        {
            style.animation_direction = PanoramaAnimDirection::AlternateReverse;
        }
        else
        {
            style.animation_direction = PanoramaAnimDirection::Normal;
        }
    }
    else if (prop == "animation-fill-mode")
    {
        const std::string t = to_lower(trim(value));
        if (t == "forwards")
        {
            style.animation_fill_mode = PanoramaAnimFillMode::Forwards;
        }
        else if (t == "backwards")
        {
            style.animation_fill_mode = PanoramaAnimFillMode::Backwards;
        }
        else if (t == "both")
        {
            style.animation_fill_mode = PanoramaAnimFillMode::Both;
        }
        else
        {
            style.animation_fill_mode = PanoramaAnimFillMode::None;
        }
    }
}

float PanoramaEasing::evaluate(float t) const
{
    const float clamped = std::clamp(t, 0.0F, 1.0F);
    if (linear)
    {
        return clamped;
    }

    // Solve the cubic-bezier for the parametric value u where X(u) == t, then return
    // Y(u). Implicit endpoints (0,0) and (1,1). Newton-Raphson with a bisection
    // fallback — ported from WebKit's UnitBezier (Apple, BSD-licensed).
    const double x = clamped;
    const double cx = 3.0 * static_cast<double>(x1);
    const double bx = 3.0 * static_cast<double>(x2 - x1) - cx;
    const double ax = 1.0 - cx - bx;
    const double cy = 3.0 * static_cast<double>(y1);
    const double by = 3.0 * static_cast<double>(y2 - y1) - cy;
    const double ay = 1.0 - cy - by;

    const auto sample_x = [&](double u) { return ((ax * u + bx) * u + cx) * u; };
    const auto sample_dx = [&](double u) { return (3.0 * ax * u + 2.0 * bx) * u + cx; };

    constexpr double epsilon = 1e-5;
    double u = x;
    bool solved = false;
    for (int i = 0; i < 8; ++i)
    {
        const double x_err = sample_x(u) - x;
        if (std::fabs(x_err) < epsilon)
        {
            solved = true;
            break;
        }
        const double d = sample_dx(u);
        if (std::fabs(d) < 1e-6)
        {
            break;
        }
        u -= x_err / d;
    }

    if (!solved)
    {
        double t0 = 0.0;
        double t1 = 1.0;
        u = x;
        while (t0 < t1)
        {
            const double x_value = sample_x(u);
            if (std::fabs(x_value - x) < epsilon)
            {
                break;
            }
            if (x > x_value)
            {
                t0 = u;
            }
            else
            {
                t1 = u;
            }
            u = (t1 - t0) * 0.5 + t0;
        }
    }

    const double y = ((ay * u + by) * u + cy) * u;
    return static_cast<float>(y);
}

bool PanoramaComputedStyle::find_transition(std::string_view property, PanoramaTransition& out) const
{
    if (transition_properties.empty() || transition_durations.empty())
    {
        return false;
    }
    for (std::size_t i = 0; i < transition_properties.size(); ++i)
    {
        const std::string& name = transition_properties[i];
        if (name != property && name != "all")
        {
            continue;
        }
        // CSS repeats shorter value lists across the property list.
        out.property = name;
        out.duration = transition_durations[i % transition_durations.size()];
        out.delay = transition_delays.empty() ? 0.0F : transition_delays[i % transition_delays.size()];
        out.easing = transition_easings.empty() ? PanoramaEasing{}
                                                : transition_easings[i % transition_easings.size()];
        return out.duration > 0.0F;
    }
    return false;
}

namespace
{
// Maps a keyframe-stop property name to the animatable channel it drives, or 0 if
// the property is not interpolated by the keyframe runtime.
std::uint32_t channel_for_property(std::string_view property)
{
    const std::string p = to_lower(trim(property));
    if (p == "opacity")
    {
        return PanoramaAnimOpacity;
    }
    if (p == "brightness")
    {
        return PanoramaAnimBrightness;
    }
    if (p == "background-color")
    {
        return PanoramaAnimBackgroundColor;
    }
    if (p == "background-img-opacity" || p == "background-image-opacity")
    {
        return PanoramaAnimBackgroundImageOpacity;
    }
    if (p == "color")
    {
        return PanoramaAnimColor;
    }
    if (p == "wash-color" || p == "wash-color-fast")
    {
        return PanoramaAnimWashColor;
    }
    if (p == "transform")
    {
        return PanoramaAnimTransform;
    }
    if (p == "x" || p == "y" || p == "z" || p == "position")
    {
        return PanoramaAnimPosition;
    }
    if (p == "box-shadow")
    {
        return PanoramaAnimBoxShadow;
    }
    if (p == "blur")
    {
        return PanoramaAnimBlur;
    }
    if (p == "clip")
    {
        return PanoramaAnimClip;
    }
    return 0;
}

bool strip_important_suffix(std::string& value)
{
    std::string text = trim(value);
    const std::string lower = to_lower(text);
    std::size_t end = lower.size();
    while (end > 0 && is_space(lower[end - 1]))
    {
        --end;
    }

    constexpr std::string_view important = "important";
    if (end < important.size() ||
        lower.compare(end - important.size(), important.size(), important.data(), important.size()) != 0)
    {
        value = std::move(text);
        return false;
    }

    std::size_t bang = end - important.size();
    while (bang > 0 && is_space(lower[bang - 1]))
    {
        --bang;
    }
    if (bang == 0 || lower[bang - 1] != '!')
    {
        value = std::move(text);
        return false;
    }

    value = trim(std::string_view(text).substr(0, bang - 1));
    return !value.empty();
}

bool parse_declaration(std::string_view raw, PanoramaDeclaration& out)
{
    const std::size_t colon = raw.find(':');
    if (colon == std::string::npos)
    {
        return false;
    }

    const std::string property = trim(raw.substr(0, colon));
    out.property = is_custom_property_name(property) ? property : to_lower(property);
    out.value = trim(raw.substr(colon + 1));
    out.important = strip_important_suffix(out.value);
    out.resolved_value.clear();
    return !out.property.empty() && !out.value.empty();
}

// `from`/`to`/`N%` -> offset in [0,1].
float parse_keyframe_offset(std::string_view token)
{
    const std::string t = to_lower(trim(token));
    if (t == "from")
    {
        return 0.0F;
    }
    if (t == "to")
    {
        return 1.0F;
    }
    float value = parse_number(t);
    if (t.find('%') != std::string::npos)
    {
        value /= 100.0F;
    }
    return std::clamp(value, 0.0F, 1.0F);
}

// Parses one `@keyframes <name> { <stop> { decls } ... }` body into `out`. WebCore
// accepts the name as either an identifier token or a string token; normalizing here
// strips CSS quotes/escapes so Panorama's quoted names match `animation-name`.
// A later same-name block replaces an earlier one, per CSS. Stop declarations keep
// their raw values; resolve_all_values() later substitutes @defines and derives each
// stop's typed `resolved` style + `channels`.
void parse_keyframes_block(
    std::string_view name_in, std::string_view body, std::unordered_map<std::string, PanoramaKeyframes>& out)
{
    bool quoted = false;
    const std::string name = normalize_css_identifier_or_string(name_in, &quoted);
    const std::string keyword = to_lower(trim(name_in));
    if (name.empty() || (!quoted && keyword == "none"))
    {
        return;
    }

    PanoramaKeyframes keyframes;
    std::size_t i = 0;
    while (i < body.size())
    {
        const std::size_t open = body.find('{', i);
        if (open == std::string_view::npos)
        {
            break;
        }
        const std::string_view selector = body.substr(i, open - i);

        int depth = 1;
        std::size_t j = open + 1;
        for (; j < body.size() && depth > 0; ++j)
        {
            if (body[j] == '{')
            {
                ++depth;
            }
            else if (body[j] == '}')
            {
                --depth;
            }
        }
        const std::size_t decls_begin = open + 1;
        const std::size_t decls_end = j > 0 ? j - 1 : j; // the matching '}'
        const std::string_view decls_text =
            decls_end > decls_begin ? body.substr(decls_begin, decls_end - decls_begin) : std::string_view{};
        i = j;

        // Parse this block's declarations once; reuse for each offset it lists.
        PanoramaKeyframeStop prototype;
        for (const std::string& decl : split_top_level(decls_text, ';'))
        {
            PanoramaDeclaration declaration;
            if (!parse_declaration(decl, declaration))
            {
                continue;
            }
            if (declaration.property == "animation-timing-function")
            {
                // Per-stop easing governs the segment starting at this stop.
                prototype.easing = parse_easing(declaration.value);
                prototype.has_easing = true;
                continue;
            }
            prototype.declarations.push_back(std::move(declaration));
        }

        for (const std::string& offset_token : split_top_level(selector, ','))
        {
            if (trim(offset_token).empty())
            {
                continue;
            }
            PanoramaKeyframeStop stop = prototype;
            stop.offset = parse_keyframe_offset(offset_token);
            keyframes.stops.push_back(std::move(stop));
        }
    }

    std::stable_sort(keyframes.stops.begin(), keyframes.stops.end(),
        [](const PanoramaKeyframeStop& a, const PanoramaKeyframeStop& b) { return a.offset < b.offset; });
    out[name] = std::move(keyframes);
}

// Captures `@define name: value;` into `defines`, parses `@keyframes` blocks into
// `keyframes`, and removes all @-rules from the source: `@define`/other
// `;`-terminated at-rules are dropped, and block at-rules (@keyframes/@media/...)
// are removed along with their balanced braces. Returns a clean stylesheet of
// ordinary `selector { ... }` rules.
std::string extract_defines_and_strip(std::string_view css, std::unordered_map<std::string, std::string>& defines,
    std::unordered_map<std::string, PanoramaKeyframes>& keyframes)
{
    std::string out;
    out.reserve(css.size());
    std::size_t i = 0;
    int brace_depth = 0;
    while (i < css.size())
    {
        if (css[i] == '@' && brace_depth == 0)
        {
            // Read the at-rule keyword.
            std::size_t k = i + 1;
            while (k < css.size() && (std::isalnum(static_cast<unsigned char>(css[k])) != 0 || css[k] == '-'))
            {
                ++k;
            }
            const std::string keyword = to_lower(css.substr(i + 1, k - i - 1));
            // Find whether this at-rule is `;`-terminated or a `{...}` block.
            std::size_t scan = k;
            while (scan < css.size() && css[scan] != ';' && css[scan] != '{')
            {
                ++scan;
            }
            if (scan < css.size() && css[scan] == ';')
            {
                if (keyword == "define")
                {
                    const std::string decl{css.substr(k, scan - k)}; // " name: value"
                    const std::size_t colon = decl.find(':');
                    if (colon != std::string::npos)
                    {
                        const std::string name = trim(decl.substr(0, colon));
                        const std::string value = trim(decl.substr(colon + 1));
                        if (!name.empty())
                        {
                            defines[name] = value;
                        }
                    }
                }
                i = scan + 1;
                continue;
            }
            // Block at-rule: skip balanced braces (capturing @keyframes en route).
            int depth = 0;
            std::size_t j = scan;
            for (; j < css.size(); ++j)
            {
                if (css[j] == '{')
                {
                    ++depth;
                }
                else if (css[j] == '}')
                {
                    if (--depth == 0)
                    {
                        ++j;
                        break;
                    }
                }
            }
            // `@keyframes <name> { ... }` (also vendor-prefixed, e.g. @-webkit-keyframes):
            // the name is between the keyword and the opening brace, the body between the
            // braces (`scan` indexes the opening '{', `j - 1` the matching '}').
            if (keyword.find("keyframes") != std::string::npos && scan < css.size() && css[scan] == '{')
            {
                const std::string_view name = css.substr(k, scan - k);
                const std::size_t body_begin = scan + 1;
                const std::size_t body_end = j > 0 ? j - 1 : j;
                if (body_end > body_begin)
                {
                    parse_keyframes_block(name, css.substr(body_begin, body_end - body_begin), keyframes);
                }
            }
            i = j;
            continue;
        }
        if (css[i] == '{')
        {
            ++brace_depth;
        }
        else if (css[i] == '}' && brace_depth > 0)
        {
            --brace_depth;
        }
        out.push_back(css[i]);
        ++i;
    }
    return out;
}
}

std::string PanoramaStyleSheet::resolve_value_impl(std::string_view value, int depth) const
{
    if (depth > 8 || defines_.empty())
    {
        return std::string(value);
    }
    std::string out;
    out.reserve(value.size());
    bool changed = false;
    std::size_t i = 0;
    while (i < value.size())
    {
        const char ch = value[i];
        const bool ident_start = std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
        if (!ident_start)
        {
            out.push_back(ch);
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < value.size() &&
               (std::isalnum(static_cast<unsigned char>(value[j])) != 0 || value[j] == '_' || value[j] == '-'))
        {
            ++j;
        }
        const std::string token(value.substr(i, j - i));
        const auto it = defines_.find(token);
        if (it != defines_.end())
        {
            out += it->second;
            changed = true;
        }
        else
        {
            out += token;
        }
        i = j;
    }
    return changed ? resolve_value_impl(out, depth + 1) : out;
}

std::string PanoramaStyleSheet::resolve_value(std::string_view value) const
{
    return resolve_value_impl(value, 0);
}

void PanoramaStyleSheet::clear()
{
    rules_.clear();
    source_scopes_.clear();
    defines_.clear();
    keyframes_.clear();
    next_source_order_ = 0;
    has_sibling_rules_ = false;
    has_focus_within_rules_ = false;
    rules_by_id_.clear();
    rules_by_class_.clear();
    rules_by_type_.clear();
    rules_universal_.clear();
    ++generation_; // dependent caches keyed on (instance, generation) revalidate
}

std::uint16_t PanoramaStyleSheet::add_source(std::string_view css, std::uint16_t layout_scope)
{
    const std::uint16_t source_index = static_cast<std::uint16_t>(source_scopes_.size());
    source_scopes_.push_back({layout_scope});

    const std::string stripped = extract_defines_and_strip(strip_css_comments(css), defines_, keyframes_);
    std::size_t cursor = 0;
    while (cursor < stripped.size())
    {
        const std::size_t open = stripped.find('{', cursor);
        if (open == std::string::npos)
        {
            break;
        }
        const std::string selector_text = trim(std::string_view(stripped).substr(cursor, open - cursor));

        // Find the matching '}', skipping braces that appear inside a string or url()
        // value — e.g. `background-image: url("file://{resources}/x.webm")`. A naive
        // search for the first '}' would stop at the brace in `{resources}` and split
        // the rule body, dropping every declaration after it (this is what hid
        // `min-width` on the play menu's GO button). CSS does not treat braces inside
        // strings/parentheses as block delimiters.
        std::size_t close = std::string::npos;
        int paren = 0;
        char quote = '\0';
        for (std::size_t i = open + 1; i < stripped.size(); ++i)
        {
            const char ch = stripped[i];
            if (quote != '\0')
            {
                if (ch == quote)
                {
                    quote = '\0';
                }
                continue;
            }
            if (ch == '"' || ch == '\'')
            {
                quote = ch;
            }
            else if (ch == '(')
            {
                ++paren;
            }
            else if (ch == ')')
            {
                if (paren > 0)
                {
                    --paren;
                }
            }
            else if (ch == '}' && paren == 0)
            {
                close = i;
                break;
            }
        }
        if (close == std::string::npos)
        {
            break;
        }
        const std::string body(std::string_view(stripped).substr(open + 1, close - open - 1));
        cursor = close + 1;

        if (selector_text.empty())
        {
            continue;
        }

        PanoramaRule rule;
        rule.source_order = next_source_order_++;
        rule.source_index = source_index;
        // NOTE: unlike WebCore (where one invalid selector drops the whole list),
        // Valve's Panorama parser keeps the valid segments of a selector list.
        // CS:GO depends on this leniency: `.content-navbar__tabs RadioButton, ...,
        // .content-navbar__tabs Button,` ends in a trailing comma yet its 28px tab
        // margins/96px height clearly apply in the real client.
        for (const std::string& sel : split_top_level(selector_text, ','))
        {
            PanoramaSelector parsed = parse_selector(trim(sel));
            if (!parsed.compounds.empty())
            {
                parsed.specificity = specificity(parsed);
                collect_selector_ancestor_hashes(parsed);
                has_sibling_rules_ = has_sibling_rules_ || selector_uses_sibling_combinator(parsed);
                has_focus_within_rules_ = has_focus_within_rules_ || selector_uses_focus_within(parsed);
                rule.selectors.push_back(std::move(parsed));
            }
        }
        for (const std::string& decl : split_top_level(body, ';'))
        {
            PanoramaDeclaration declaration;
            if (parse_declaration(decl, declaration))
            {
                rule.declarations.push_back(std::move(declaration));
            }
        }
        if (!rule.selectors.empty() && !rule.declarations.empty())
        {
            rules_.push_back(std::move(rule));
            index_rule(static_cast<int>(rules_.size()) - 1);
        }
    }

    // A later sheet's @define can affect declarations parsed earlier in this or a
    // previous add_source call, so re-resolve every declaration's value now.
    resolve_all_values();
    // Invalidate (sheet, generation)-keyed caches: @defines may have changed any
    // declaration's resolved value, and rules_ growth moved declaration storage.
    ++generation_;
    return source_index;
}

void PanoramaStyleSheet::add_source_scope(std::uint16_t source_index, std::uint16_t layout_scope)
{
    if (source_index >= source_scopes_.size())
    {
        return;
    }
    std::vector<std::uint16_t>& scopes = source_scopes_[static_cast<std::size_t>(source_index)];
    if (std::find(scopes.begin(), scopes.end(), layout_scope) != scopes.end())
    {
        return;
    }
    scopes.push_back(layout_scope);
    ++generation_; // the set of nodes the sheet's rules apply to changed
}

void PanoramaStyleSheet::index_rule(int rule_index)
{
    const PanoramaRule& rule = rules_[static_cast<std::size_t>(rule_index)];
    for (const PanoramaSelector& selector : rule.selectors)
    {
        // Bucket by the subject compound's most selective component. A node can only
        // match if it carries that id / class / type, so this is a conservative
        // candidate filter (the full selector test still runs in compute_node).
        const PanoramaSimpleSelector& subject = selector.compounds.back();
        if (!subject.id.empty())
        {
            rules_by_id_[to_lower(subject.id)].push_back(rule_index);
        }
        else if (!subject.classes.empty())
        {
            rules_by_class_[to_lower(subject.classes.front())].push_back(rule_index);
        }
        else if (!subject.universal && !subject.type.empty())
        {
            rules_by_type_[subject.type].push_back(rule_index);
        }
        else
        {
            rules_universal_.push_back(rule_index);
        }
    }
}

void PanoramaStyleSheet::resolve_all_values()
{
    for (PanoramaRule& rule : rules_)
    {
        for (PanoramaDeclaration& decl : rule.declarations)
        {
            decl.resolved_value = resolve_value(decl.value);
        }
    }

    // Derive each keyframe stop's typed `resolved` style and `channels` after
    // @define substitution, and the per-keyframes channel union. Idempotent, so it
    // is safe to re-run on every add_source.
    for (auto& [name, keyframes] : keyframes_)
    {
        keyframes.channels = 0;
        for (PanoramaKeyframeStop& stop : keyframes.stops)
        {
            stop.resolved = PanoramaComputedStyle{};
            stop.channels = 0;
            for (PanoramaDeclaration& decl : stop.declarations)
            {
                decl.resolved_value = resolve_value(decl.value);
                apply_panorama_declaration(stop.resolved, decl.property, decl.resolved_value);
                stop.channels |= channel_for_property(decl.property);
            }
            keyframes.channels |= stop.channels;
        }
    }
}

namespace
{
// ---- matched-declarations cache --------------------------------------------
// WebCore's MatchedDeclarationsCache (style/MatchedDeclarationsCache.h): two
// elements whose cascades saw the same matched declaration lists, the same
// inline style, the same UA defaults, and the same inherited inputs produce
// byte-identical computed styles — so the second one reuses the first's result
// instead of re-applying (and re-parsing) every declaration. Hit rate is high in
// real documents because most siblings share rule sets.
//
// The inherited inputs below MUST mirror the inheritance block in compute_node;
// adding an inherited property there requires adding it here, or the cache will
// serve stale styles.
struct MatchedStyleParentSnapshot
{
    PanoramaColor color;
    float font_size = 0.0F;
    int font_weight = 0;
    bool font_italic = false;
    PanoramaTextTransform text_transform = PanoramaTextTransform::None;
    PanoramaHAlign text_align = PanoramaHAlign::Left;
    bool white_space_nowrap = false;
    float letter_spacing = 0.0F;
    float line_height = 0.0F;
    PanoramaTextShadow text_shadow;
    PanoramaColor wash_color;
    float brightness = 0.0F;
    PanoramaCustomProperties custom_properties;
};

bool colors_equal(const PanoramaColor& a, const PanoramaColor& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool text_shadows_equal(const PanoramaTextShadow& a, const PanoramaTextShadow& b)
{
    return a.present == b.present && a.offset_x == b.offset_x && a.offset_y == b.offset_y && a.blur == b.blur &&
           a.strength == b.strength && colors_equal(a.color, b.color);
}

bool parent_snapshot_matches(const MatchedStyleParentSnapshot& snapshot, const PanoramaComputedStyle& parent)
{
    return colors_equal(snapshot.color, parent.color) && snapshot.font_size == parent.font_size &&
           snapshot.font_weight == parent.font_weight && snapshot.font_italic == parent.font_italic &&
           snapshot.text_transform == parent.text_transform && snapshot.text_align == parent.text_align &&
           snapshot.white_space_nowrap == parent.white_space_nowrap &&
           snapshot.letter_spacing == parent.letter_spacing && snapshot.line_height == parent.line_height &&
           text_shadows_equal(snapshot.text_shadow, parent.text_shadow) &&
           colors_equal(snapshot.wash_color, parent.wash_color) && snapshot.brightness == parent.brightness &&
           snapshot.custom_properties.shares_storage(parent.custom_properties);
}

struct MatchedStyleCacheEntry
{
    std::vector<const std::vector<PanoramaDeclaration>*> declaration_lists; // cascade order
    std::string inline_style;
    bool ua_bold = false;
    bool ua_italic = false;
    bool has_parent = false;
    MatchedStyleParentSnapshot parent;
    PanoramaComputedStyle style;
};

// Per-thread cache, revalidated against the owning sheet's never-reused instance
// id and content generation (declaration-list pointers dangle after add_source
// re-allocates rules_, and @define changes alter resolved values).
struct MatchedStyleCache
{
    std::uint64_t sheet_instance = 0;
    std::uint64_t sheet_generation = 0;
    std::size_t entry_count = 0;
    std::unordered_map<std::uint64_t, std::vector<MatchedStyleCacheEntry>> buckets;
};

void hash_mix_bytes(std::uint64_t& hash, const void* data, std::size_t size)
{
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kPrime;
    }
}

template <typename T>
void hash_mix(std::uint64_t& hash, const T& value)
{
    hash_mix_bytes(hash, &value, sizeof(T));
}
}

std::uint64_t PanoramaStyleSheet::next_sheet_instance_id() noexcept
{
    static std::uint64_t counter = 0;
    return ++counter;
}

bool PanoramaStyleSheet::can_share_style(const PanoramaNode& node, const PanoramaNode& candidate) const
{
    // Sibling combinators (`.a + .b`, `.a ~ .b`) can give two siblings with
    // identical state different styles, so they disable sharing entirely.
    if (has_sibling_rules_)
    {
        return false;
    }
    // Every input simple_matches() reads must be equal for the two nodes to match
    // the same rule set. Siblings already share the parent (inherited inputs +
    // custom properties) and the whole ancestor chain (descendant/child-combinator
    // matches), so only the node-local inputs need comparing here.
    if (node.tag_lower != candidate.tag_lower || node.id != candidate.id ||
        node.style_scope_mark != candidate.style_scope_mark)
    {
        return false;
    }
    if (node.hovered != candidate.hovered || node.active != candidate.active ||
        node.focused != candidate.focused || node.selected != candidate.selected)
    {
        return false;
    }
    // Inline style feeds the cascade directly; classes/attributes drive class,
    // attribute, :enabled/:disabled and `.selected`-class matching. unordered_map
    // operator== is order-insensitive; class order differences only cost a missed
    // share, never correctness.
    if (node.inline_style != candidate.inline_style || node.classes != candidate.classes ||
        node.attributes != candidate.attributes)
    {
        return false;
    }
    // :focus-within depends on a descendant's focus, which the sibling-local
    // comparison above does not see. Only relevant when such a rule exists AND some
    // node is actually focused (sharing_focus_within_active_), keeping the subtree
    // walk off the common path.
    if (sharing_focus_within_active_ &&
        node_or_descendant_focused(node) != node_or_descendant_focused(candidate))
    {
        return false;
    }
    return true;
}

void PanoramaStyleSheet::compute_node(PanoramaNode& node, const PanoramaNode* prev_sibling) const
{
    ++panorama_cascade_stats().nodes;
    // This subtree is being (re)computed in full; its dirty marks are consumed.
    node.style_dirty = false;
    node.descendant_style_dirty = false;
    node.style_fresh = true; // tells the selective anim re-capture this node changed

    // Recurse into children, threading each child's immediately preceding sibling so
    // it can reuse that sibling's computed style (style sharing). The node's own
    // identifiers join the ancestor bloom filter for the descendant match tests.
    const auto recurse_children = [this](PanoramaNode& parent_node) {
        selector_ancestor_filter().push_element(parent_node);
        const PanoramaNode* prev = nullptr;
        for (const auto& child : parent_node.children)
        {
            compute_node(*child, prev);
            prev = child.get();
        }
        selector_ancestor_filter().pop_element();
    };

    // ---- style sharing fast path --------------------------------------------
    // If the preceding sibling carries identical style-affecting state, this node's
    // computed style is provably identical: copy it and skip candidate gathering,
    // selector matching and the cache lookup entirely.
    if (sharing_active_ && prev_sibling != nullptr && can_share_style(node, *prev_sibling))
    {
        node.computed = prev_sibling->computed;
        ++panorama_cascade_stats().shared_nodes;
        recurse_children(node);
        return;
    }

    // WebCore's user-agent stylesheet maps `strong, b` to `font-weight: bold` and
    // `i, em` to `font-style: italic`. Applied (on cache misses) before author and
    // inline declarations so authored `font-weight`/`font-style` win the cascade;
    // part of the cache key either way.
    const bool ua_bold = node.tag_lower == "b" || node.tag_lower == "strong";
    const bool ua_italic = node.tag_lower == "i" || node.tag_lower == "em";

    // Gather candidate rules from the acceleration index instead of scanning the
    // whole sheet: only rules whose subject component matches this node's id, one of
    // its classes, its type, or that are universal can possibly apply. Reused
    // scratch buffers avoid per-node allocation in this hot path.
    static thread_local std::vector<int> candidates;
    candidates.clear();
    // Diagnostic A/B switch: OPENSTRIKE_PANORAMA_NOINDEX=1 reverts to scanning the
    // whole sheet, so the indexed path can be profiled against the old behaviour.
    static const bool no_index = environment_flag_set("OPENSTRIKE_PANORAMA_NOINDEX");
    if (no_index)
    {
        candidates.resize(rules_.size());
        for (std::size_t i = 0; i < rules_.size(); ++i)
        {
            candidates[i] = static_cast<int>(i);
        }
    }
    else
    {
        // Candidate order is irrelevant — matches are stable_sorted by (specificity,
        // source_order) below, and source_order is unique per rule — so duplicates
        // (a rule bucketed under several of this node's classes, or twice in one
        // bucket via two selectors) are filtered with an epoch stamp per rule
        // instead of the old per-node sort + unique.
        static thread_local std::vector<std::uint32_t> candidate_seen;
        static thread_local std::uint32_t candidate_epoch = 0;
        if (candidate_seen.size() < rules_.size())
        {
            candidate_seen.resize(rules_.size(), 0);
        }
        ++candidate_epoch;
        if (candidate_epoch == 0)
        {
            std::fill(candidate_seen.begin(), candidate_seen.end(), 0);
            candidate_epoch = 1;
        }
        const auto add_candidate = [](int rule_index) {
            std::uint32_t& seen = candidate_seen[static_cast<std::size_t>(rule_index)];
            if (seen != candidate_epoch)
            {
                seen = candidate_epoch;
                candidates.push_back(rule_index);
            }
        };
        const auto add_bucket = [&](const std::unordered_map<std::string, std::vector<int>>& bucket,
                                    const std::string& key) {
            const auto it = bucket.find(key);
            if (it != bucket.end())
            {
                for (const int rule_index : it->second)
                {
                    add_candidate(rule_index);
                }
            }
        };
        // Reused lowercase-key scratch: the old code allocated a fresh lowered
        // string per id/class per node per compute.
        static thread_local std::string lower_key;
        const auto to_lower_into = [](std::string_view text, std::string& out) {
            out.assign(text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
        };
        if (!node.id.empty())
        {
            to_lower_into(node.id, lower_key);
            add_bucket(rules_by_id_, lower_key);
        }
        for (const std::string& klass : node.classes)
        {
            to_lower_into(klass, lower_key);
            add_bucket(rules_by_class_, lower_key);
        }
        add_bucket(rules_by_type_, node.tag_lower);
        for (const int rule_index : rules_universal_)
        {
            add_candidate(rule_index);
        }
    }

    // Collect matching rules with the best specificity among their selectors.
    struct Match
    {
        std::array<int, 3> spec;
        int source_order;
        const std::vector<PanoramaDeclaration>* declarations;
    };
    static thread_local std::vector<Match> matches;
    matches.clear();
    panorama_cascade_stats().candidate_rules += candidates.size();

    // Layout-scope marks on this node's ancestor-or-self chain. A scoped sheet
    // (see add_source) only styles nodes inside the subtrees its including
    // layout created; kRootLayoutScope sheets apply everywhere.
    static thread_local std::vector<std::uint16_t> active_scope_marks;
    active_scope_marks.clear();
    for (const PanoramaNode* scope_walk = &node; scope_walk != nullptr; scope_walk = scope_walk->parent)
    {
        if (scope_walk->style_scope_mark != 0)
        {
            active_scope_marks.push_back(scope_walk->style_scope_mark);
        }
    }
    const auto rule_in_scope = [&](const PanoramaRule& rule) {
        const std::vector<std::uint16_t>& scopes = source_scopes_[static_cast<std::size_t>(rule.source_index)];
        for (const std::uint16_t scope : scopes)
        {
            if (scope == kRootLayoutScope ||
                std::find(active_scope_marks.begin(), active_scope_marks.end(), scope) != active_scope_marks.end())
            {
                return true;
            }
        }
        return false;
    };

    const SelectorAncestorFilter& ancestor_filter = selector_ancestor_filter();
    for (const int rule_index : candidates)
    {
        const PanoramaRule& rule = rules_[static_cast<std::size_t>(rule_index)];
        if (!rule_in_scope(rule))
        {
            continue;
        }
        std::array<int, 3> best{-1, -1, -1};
        bool any = false;
        for (const PanoramaSelector& selector : rule.selectors)
        {
            // Bloom-filter fast path: skip the ancestor-walking match test when a
            // required ancestor identifier is provably absent.
            if (ancestor_filter.fast_reject(selector.ancestor_hashes))
            {
                ++panorama_cascade_stats().filter_rejects;
                continue;
            }
            if (selector_matches(node, selector))
            {
                if (!any || selector.specificity > best)
                {
                    best = selector.specificity;
                    any = true;
                }
            }
        }
        if (any)
        {
            matches.push_back({best, rule.source_order, &rule.declarations});
        }
    }
    panorama_cascade_stats().matched_rules += matches.size();

    std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
        if (a.spec != b.spec)
        {
            return a.spec < b.spec;
        }
        return a.source_order < b.source_order;
    });

    // ---- matched-declarations cache lookup ----------------------------------
    // Computed style is a pure function of (matched declaration lists in cascade
    // order, inline style text, UA tag defaults, parent's inherited outputs), so
    // identical inputs reuse the previously computed style outright.
    static thread_local MatchedStyleCache style_cache;
    if (style_cache.sheet_instance != instance_id_ || style_cache.sheet_generation != generation_)
    {
        style_cache.buckets.clear();
        style_cache.entry_count = 0;
        style_cache.sheet_instance = instance_id_;
        style_cache.sheet_generation = generation_;
    }

    const PanoramaComputedStyle* parent_style = node.parent != nullptr ? &node.parent->computed : nullptr;
    std::uint64_t key_hash = 1469598103934665603ULL;
    for (const Match& match : matches)
    {
        hash_mix(key_hash, match.declarations);
    }
    hash_mix(key_hash, node.inline_style.size());
    hash_mix_bytes(key_hash, node.inline_style.data(), node.inline_style.size());
    hash_mix(key_hash, ua_bold);
    hash_mix(key_hash, ua_italic);
    const bool has_parent = parent_style != nullptr;
    hash_mix(key_hash, has_parent);
    if (parent_style != nullptr)
    {
        // Field-wise (never whole-struct) hashing: struct padding is indeterminate.
        hash_mix(key_hash, parent_style->color);
        hash_mix(key_hash, parent_style->font_size);
        hash_mix(key_hash, parent_style->font_weight);
        hash_mix(key_hash, parent_style->font_italic);
        hash_mix(key_hash, parent_style->text_transform);
        hash_mix(key_hash, parent_style->text_align);
        hash_mix(key_hash, parent_style->white_space_nowrap);
        hash_mix(key_hash, parent_style->letter_spacing);
        hash_mix(key_hash, parent_style->line_height);
        hash_mix(key_hash, parent_style->text_shadow.present);
        hash_mix(key_hash, parent_style->text_shadow.offset_x);
        hash_mix(key_hash, parent_style->text_shadow.offset_y);
        hash_mix(key_hash, parent_style->text_shadow.blur);
        hash_mix(key_hash, parent_style->text_shadow.strength);
        hash_mix(key_hash, parent_style->text_shadow.color);
        hash_mix(key_hash, parent_style->wash_color);
        hash_mix(key_hash, parent_style->brightness);
        hash_mix(key_hash, parent_style->custom_properties.storage_key());
    }

    if (const auto bucket_it = style_cache.buckets.find(key_hash); bucket_it != style_cache.buckets.end())
    {
        for (const MatchedStyleCacheEntry& entry : bucket_it->second)
        {
            if (entry.ua_bold != ua_bold || entry.ua_italic != ua_italic || entry.has_parent != has_parent ||
                entry.declaration_lists.size() != matches.size() || entry.inline_style != node.inline_style)
            {
                continue;
            }
            bool same_lists = true;
            for (std::size_t i = 0; i < matches.size(); ++i)
            {
                if (entry.declaration_lists[i] != matches[i].declarations)
                {
                    same_lists = false;
                    break;
                }
            }
            if (!same_lists || (parent_style != nullptr && !parent_snapshot_matches(entry.parent, *parent_style)))
            {
                continue;
            }
            node.computed = entry.style;
            recurse_children(node);
            return;
        }
    }

    // ---- cache miss: build the style by applying the cascade -----------------
    PanoramaComputedStyle style;
    // Inherit the inheritable properties from the parent's resolved style. This
    // list MUST stay in sync with MatchedStyleParentSnapshot above.
    if (parent_style != nullptr)
    {
        style.color = parent_style->color;
        style.font_size = parent_style->font_size;
        style.font_weight = parent_style->font_weight;
        style.font_italic = parent_style->font_italic;
        style.text_transform = parent_style->text_transform;
        style.text_align = parent_style->text_align;
        style.white_space_nowrap = parent_style->white_space_nowrap;
        style.letter_spacing = parent_style->letter_spacing;
        style.line_height = parent_style->line_height;
        style.text_shadow = parent_style->text_shadow;
        style.wash_color = parent_style->wash_color;
        style.brightness = parent_style->brightness;
        style.custom_properties = parent_style->custom_properties;
    }
    if (ua_bold)
    {
        style.font_weight = 700;
    }
    if (ua_italic)
    {
        style.font_italic = true;
    }

    // Inline style is parsed once per (source text, sheet state) and cached on the
    // node — WebCore parses the style attribute into typed StyleProperties when it
    // is set, never during style resolution. The cache revalidates on the exact
    // source string so JS writes to `style` (which rewrite inline_style) reparse.
    static const std::vector<PanoramaDeclaration> no_inline_declarations;
    const std::vector<PanoramaDeclaration>* inline_declarations = &no_inline_declarations;
    if (!node.inline_style.empty())
    {
        PanoramaInlineStyleCache& cache = node.inline_style_cache;
        if (!cache.valid || cache.sheet != this || cache.sheet_generation != generation_ ||
            cache.source != node.inline_style)
        {
            cache.declarations.clear();
            cache.declarations.reserve(4);
            for (const std::string& decl_text : split_top_level(node.inline_style, ';'))
            {
                PanoramaDeclaration decl;
                if (parse_declaration(decl_text, decl))
                {
                    decl.resolved_value = resolve_value(decl.value);
                    cache.declarations.push_back(std::move(decl));
                }
            }
            cache.source = node.inline_style;
            cache.sheet = this;
            cache.sheet_generation = generation_;
            cache.valid = true;
        }
        inline_declarations = &cache.declarations;
    }

    const auto visit_sheet_declarations = [&](bool important, const auto& visitor) {
        for (const Match& match : matches)
        {
            for (const PanoramaDeclaration& decl : *match.declarations)
            {
                if (decl.important == important)
                {
                    visitor(decl);
                }
            }
        }
    };

    const auto visit_inline_declarations = [&](bool important, const auto& visitor) {
        for (const PanoramaDeclaration& decl : *inline_declarations)
        {
            if (decl.important == important)
            {
                visitor(decl);
            }
        }
    };

    const auto apply_custom_property = [&](const PanoramaDeclaration& decl) {
        if (!is_custom_property_name(decl.property))
        {
            return;
        }

        const std::string keyword = to_lower(trim(decl.resolved_value));
        if (keyword == "initial" || keyword == "revert" || keyword == "revert-layer")
        {
            style.custom_properties.erase(decl.property);
        }
        else if (keyword != "inherit" && keyword != "unset")
        {
            style.custom_properties.set(decl.property, decl.resolved_value);
        }
    };

    const auto apply_typed_property = [&](const PanoramaDeclaration& decl) {
        if (is_custom_property_name(decl.property))
        {
            return;
        }

        ++panorama_cascade_stats().declarations_applied;
        const CssValueResolution resolved =
            resolve_custom_property_references(decl.resolved_value, style.custom_properties, 0);
        if (resolved.valid)
        {
            apply_panorama_declaration(style, decl.property, resolved.value);
        }
    };

    // Match WebCore's PropertyCascade shape at this scale: first collect the
    // winning custom-property map, then resolve ordinary property values against
    // that final map. This lets `color: var(--x); --x: red;` work independent of
    // declaration order inside the same cascade result.
    visit_sheet_declarations(false, apply_custom_property);
    visit_inline_declarations(false, apply_custom_property);
    visit_sheet_declarations(true, apply_custom_property);
    visit_inline_declarations(true, apply_custom_property);

    visit_sheet_declarations(false, apply_typed_property);
    visit_inline_declarations(false, apply_typed_property);
    visit_sheet_declarations(true, apply_typed_property);
    visit_inline_declarations(true, apply_typed_property);

    // Remember the result for identical future cascades. The cap bounds memory on
    // pathological documents; clearing wholesale keeps the bookkeeping trivial
    // (WebCore sweeps on a timer instead).
    constexpr std::size_t kMatchedStyleCacheMaxEntries = 4096;
    if (style_cache.entry_count >= kMatchedStyleCacheMaxEntries)
    {
        style_cache.buckets.clear();
        style_cache.entry_count = 0;
    }
    MatchedStyleCacheEntry entry;
    entry.declaration_lists.reserve(matches.size());
    for (const Match& match : matches)
    {
        entry.declaration_lists.push_back(match.declarations);
    }
    entry.inline_style = node.inline_style;
    entry.ua_bold = ua_bold;
    entry.ua_italic = ua_italic;
    entry.has_parent = has_parent;
    if (parent_style != nullptr)
    {
        entry.parent.color = parent_style->color;
        entry.parent.font_size = parent_style->font_size;
        entry.parent.font_weight = parent_style->font_weight;
        entry.parent.font_italic = parent_style->font_italic;
        entry.parent.text_transform = parent_style->text_transform;
        entry.parent.text_align = parent_style->text_align;
        entry.parent.white_space_nowrap = parent_style->white_space_nowrap;
        entry.parent.letter_spacing = parent_style->letter_spacing;
        entry.parent.line_height = parent_style->line_height;
        entry.parent.text_shadow = parent_style->text_shadow;
        entry.parent.wash_color = parent_style->wash_color;
        entry.parent.brightness = parent_style->brightness;
        entry.parent.custom_properties = parent_style->custom_properties;
    }
    entry.style = style;
    style_cache.buckets[key_hash].push_back(std::move(entry));
    ++style_cache.entry_count;

    node.computed = std::move(style);
    recurse_children(node);
}

void PanoramaStyleSheet::seed_sharing_flags(const PanoramaNode& root) const
{
    static const bool env_disabled = environment_flag_set("OPENSTRIKE_PANORAMA_NOSHARE");
    sharing_active_ = style_sharing_enabled_ && !env_disabled;
    // The focus-within cross-check only matters when such a rule exists and a node is
    // actually focused; otherwise :focus-within is uniformly false across siblings.
    sharing_focus_within_active_ =
        sharing_active_ && has_focus_within_rules_ && node_or_descendant_focused(root);
}

void PanoramaStyleSheet::compute(PanoramaNode& root) const
{
    // Seed the ancestor bloom filter with everything above `root` (compute may be
    // called on a subtree); compute_node pushes/pops per node below it. A counting
    // filter is order-insensitive, so the upward walk can push directly.
    SelectorAncestorFilter& filter = selector_ancestor_filter();
    filter.clear();
    for (const PanoramaNode* ancestor = root.parent; ancestor != nullptr; ancestor = ancestor->parent)
    {
        filter.push_element(*ancestor);
    }
    seed_sharing_flags(root);
    compute_node(root);
}

void PanoramaStyleSheet::compute_invalidated(PanoramaNode& root) const
{
    SelectorAncestorFilter& filter = selector_ancestor_filter();
    filter.clear();
    for (const PanoramaNode* ancestor = root.parent; ancestor != nullptr; ancestor = ancestor->parent)
    {
        filter.push_element(*ancestor);
    }
    seed_sharing_flags(root);
    compute_invalidated_node(root);
}

void PanoramaStyleSheet::compute_invalidated_node(PanoramaNode& node) const
{
    if (node.style_dirty)
    {
        compute_node(node); // full subtree recompute; consumes the marks
        return;
    }
    if (!node.descendant_style_dirty)
    {
        return; // clean subtree: computed styles still valid
    }
    node.descendant_style_dirty = false;

    // This node keeps its computed style but its identifiers still belong in the
    // ancestor filter for the dirty descendants being recomputed below it.
    SelectorAncestorFilter& filter = selector_ancestor_filter();
    filter.push_element(node);
    // When the sheet has sibling combinators, a dirty child's state change can
    // also alter its FOLLOWING siblings' matches (`.a ~ .b`, `.a + .b .c`), so the
    // recompute widens to every later sibling once a dirty child is seen.
    bool force_following_siblings = false;
    for (const auto& child : node.children)
    {
        if (child->style_dirty || force_following_siblings)
        {
            force_following_siblings = force_following_siblings || (child->style_dirty && has_sibling_rules_);
            compute_node(*child);
        }
        else
        {
            compute_invalidated_node(*child);
        }
    }
    filter.pop_element();
}

PanoramaTextShadow parse_panorama_text_shadow(std::string_view value)
{
    PanoramaTextShadow shadow;
    const std::string text = trim(value);
    if (text.empty() || to_lower(text) == "none")
    {
        return shadow;
    }

    // The colour token may come before or after the lengths (WebCore's
    // consumeSingleShadow tries the colour first on every iteration; shipped
    // sheets use both `2px 1px 4px #000` and `#FFFFFF 0px 0px 6px 4.75`). The
    // remaining number tokens are: offset-x, offset-y, [blur], [strength].
    std::vector<float> components;
    bool have_color = false;
    for (const std::string& token : split_css_value_tokens(text))
    {
        PanoramaColor token_color;
        if (!have_color && try_parse_color_token(token, token_color))
        {
            shadow.color = token_color;
            have_color = true;
            continue;
        }
        components.push_back(parse_number(token));
    }
    shadow.present = true;
    if (!components.empty())
    {
        shadow.offset_x = components[0];
    }
    if (components.size() > 1)
    {
        shadow.offset_y = components[1];
    }
    if (components.size() > 2)
    {
        shadow.blur = std::max(0.0F, components[2]);
    }
    if (components.size() > 3)
    {
        shadow.strength = std::max(0.0F, components[3]);
    }
    return shadow;
}

PanoramaBoxShadow parse_panorama_box_shadow(std::string_view value)
{
    PanoramaBoxShadow shadow;
    std::string text = trim(value);
    if (text.empty() || to_lower(text) == "none")
    {
        return shadow;
    }
    // `fill` (shadow also paints under the panel) and `inset` (inner shadow)
    // may appear anywhere in the value — shipped sheets write both
    // `fill #000 -1px ...` and `0px 0px 6px 3px fill rgba(...)`. The colour
    // token (keyword / rgb[a]() / #hex) likewise floats freely (Panorama's
    // canonical order is colour-first, CS:GO sheets also use the CSS
    // colour-last order); the remaining number tokens are h v blur spread.
    std::vector<float> numbers;
    bool have_color = false;
    for (const std::string& token : split_css_value_tokens(text))
    {
        const std::string lowered = to_lower(token);
        if (lowered == "fill")
        {
            shadow.fill = true;
            continue;
        }
        if (lowered == "inset")
        {
            shadow.inset = true;
            continue;
        }
        PanoramaColor token_color;
        if (!have_color && try_parse_color_token(token, token_color))
        {
            shadow.color = token_color;
            have_color = true;
            continue;
        }
        numbers.push_back(parse_number(token));
    }
    shadow.present = true;
    if (!numbers.empty())
    {
        shadow.offset_x = numbers[0];
    }
    if (numbers.size() > 1)
    {
        shadow.offset_y = numbers[1];
    }
    if (numbers.size() > 2)
    {
        shadow.blur = std::max(0.0F, numbers[2]);
    }
    if (numbers.size() > 3)
    {
        shadow.spread = numbers[3];
    }
    return shadow;
}

PanoramaBlur parse_panorama_blur(std::string_view value)
{
    PanoramaBlur blur;
    const std::string text = to_lower(trim(value));
    if (text.empty() || text == "none")
    {
        return blur;
    }
    // Expect `gaussian( stdX px, stdY px, passes )`; read the parenthesised args.
    const std::size_t open = text.find('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open)
    {
        return blur;
    }
    const std::vector<std::string> parts = split_top_level(text.substr(open + 1, close - open - 1), ',');
    if (parts.empty())
    {
        return blur;
    }
    blur.present = true;
    blur.std_x = parse_number(trim(parts[0]));
    blur.std_y = parts.size() > 1 ? parse_number(trim(parts[1])) : blur.std_x;
    blur.passes = parts.size() > 2 ? parse_number(trim(parts[2])) : 1.0F;
    return blur;
}

PanoramaClip parse_panorama_clip(std::string_view value)
{
    PanoramaClip clip;
    const std::string text = to_lower(trim(value));
    if (text.empty() || text == "none")
    {
        return clip;
    }
    const std::size_t open = text.find('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open)
    {
        return clip;
    }
    const std::string head = trim(text.substr(0, open));
    const std::vector<std::string> parts = split_top_level(text.substr(open + 1, close - open - 1), ',');
    if (head == "rect")
    {
        // rect( top, right, bottom, left ): visible-rect edges, % of the border box.
        if (parts.size() < 4)
        {
            return clip;
        }
        clip.type = PanoramaClipType::Rect;
        clip.rect_top = parse_number(trim(parts[0]));
        clip.rect_right = parse_number(trim(parts[1]));
        clip.rect_bottom = parse_number(trim(parts[2]));
        clip.rect_left = parse_number(trim(parts[3]));
    }
    else if (head == "radial")
    {
        // radial( cx cy, start, sweep ): hide the wedge swept clockwise from
        // `start` (0deg = up) over `sweep` degrees about (cx,cy) % of the box.
        if (parts.size() < 3)
        {
            return clip;
        }
        std::istringstream center(trim(parts[0]));
        std::string cx;
        std::string cy;
        center >> cx >> cy;
        clip.type = PanoramaClipType::Radial;
        clip.radial_center_x = parse_number(trim(cx));
        clip.radial_center_y = cy.empty() ? clip.radial_center_x : parse_number(trim(cy));
        clip.radial_start = parse_number(trim(parts[1]));
        clip.radial_sweep = parse_number(trim(parts[2]));
    }
    return clip;
}

std::vector<float> panorama_gaussian_kernel(float sigma, int radius)
{
    if (sigma <= 0.0F)
    {
        return {1.0F};
    }
    if (radius <= 0)
    {
        radius = static_cast<int>(std::ceil(3.0F * sigma));
    }
    radius = std::max(1, radius);
    std::vector<float> weights(static_cast<std::size_t>(2 * radius + 1));
    const float two_sigma_sq = 2.0F * sigma * sigma;
    float sum = 0.0F;
    for (int i = -radius; i <= radius; ++i)
    {
        const float weight = std::exp(-static_cast<float>(i * i) / two_sigma_sq);
        weights[static_cast<std::size_t>(i + radius)] = weight;
        sum += weight;
    }
    if (sum > 0.0F)
    {
        for (float& weight : weights)
        {
            weight /= sum;
        }
    }
    return weights;
}

std::vector<unsigned char> panorama_blur_rgba(
    const std::vector<unsigned char>& rgba, int width, int height, float std_x, float std_y)
{
    const std::size_t expected = static_cast<std::size_t>(std::max(0, width)) * static_cast<std::size_t>(std::max(0, height)) * 4U;
    if (width <= 0 || height <= 0 || rgba.size() != expected)
    {
        return rgba;
    }

    const auto sample = [](const std::vector<unsigned char>& src, int x, int y, int w, int h, int c) -> float {
        const int cx = std::clamp(x, 0, w - 1); // clamp-to-edge
        const int cy = std::clamp(y, 0, h - 1);
        return static_cast<float>(src[(static_cast<std::size_t>(cy) * w + cx) * 4U + static_cast<std::size_t>(c)]);
    };
    const auto store = [](std::vector<unsigned char>& dst, int x, int y, int w, int c, float v) {
        dst[(static_cast<std::size_t>(y) * w + x) * 4U + static_cast<std::size_t>(c)] =
            static_cast<unsigned char>(std::clamp(v, 0.0F, 255.0F) + 0.5F);
    };

    std::vector<unsigned char> out = rgba;

    // Horizontal pass.
    if (std_x > 0.0F)
    {
        const std::vector<float> kernel = panorama_gaussian_kernel(std_x);
        const int radius = static_cast<int>(kernel.size() / 2);
        const std::vector<unsigned char> in = out;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                for (int c = 0; c < 4; ++c)
                {
                    float acc = 0.0F;
                    for (int k = -radius; k <= radius; ++k)
                    {
                        acc += kernel[static_cast<std::size_t>(k + radius)] * sample(in, x + k, y, width, height, c);
                    }
                    store(out, x, y, width, c, acc);
                }
            }
        }
    }

    // Vertical pass.
    if (std_y > 0.0F)
    {
        const std::vector<float> kernel = panorama_gaussian_kernel(std_y);
        const int radius = static_cast<int>(kernel.size() / 2);
        const std::vector<unsigned char> in = out;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                for (int c = 0; c < 4; ++c)
                {
                    float acc = 0.0F;
                    for (int k = -radius; k <= radius; ++k)
                    {
                        acc += kernel[static_cast<std::size_t>(k + radius)] * sample(in, x, y + k, width, height, c);
                    }
                    store(out, x, y, width, c, acc);
                }
            }
        }
    }

    return out;
}

std::string panorama_transform_text(std::string_view text, PanoramaTextTransform transform)
{
    if (transform == PanoramaTextTransform::None)
    {
        return std::string(text);
    }
    std::string out(text);
    for (char& ch : out)
    {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (uc >= 0x80)
        {
            continue; // leave UTF-8 multibyte sequences untouched (ASCII-only casing)
        }
        ch = transform == PanoramaTextTransform::Uppercase ? static_cast<char>(std::toupper(uc))
                                                           : static_cast<char>(std::tolower(uc));
    }
    return out;
}

std::string_view panorama_transform_text_view(
    std::string_view text, PanoramaTextTransform transform, std::string& storage)
{
    if (transform == PanoramaTextTransform::None)
    {
        return text;
    }
    storage = panorama_transform_text(text, transform);
    return storage;
}

std::vector<PanoramaTextRun> panorama_parse_inline_markup(std::string_view text)
{
    //   strong, b              { font-weight: bold; }
    //   i, cite, em, var, ...  { font-style: italic; }
    // WebCore makes these ordinary inline elements styled by the cascade; we lack an
    // inline formatting context, so we fold the same tag->style mapping into styled
    // text runs instead. Returns 0 for "not a markup tag", 1 for bold, 2 for italic.
    const auto classify = [](std::string_view name) -> int {
        std::string lower;
        lower.reserve(name.size());
        for (char ch : name)
        {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        if (lower == "b" || lower == "strong")
        {
            return 1;
        }
        if (lower == "i" || lower == "em" || lower == "cite" || lower == "var" || lower == "dfn")
        {
            return 2;
        }
        return 0;
    };

    std::vector<PanoramaTextRun> runs;
    int bold = 0;
    int italic = 0;
    std::size_t pos = 0;       // scan cursor
    std::size_t run_start = 0; // start of the current text run

    const auto flush = [&](std::size_t end) {
        if (end > run_start)
        {
            runs.push_back({text.substr(run_start, end - run_start), bold > 0, italic > 0});
        }
    };

    while (pos < text.size())
    {
        if (text[pos] != '<')
        {
            ++pos;
            continue;
        }

        // Try to read a recognized inline tag: '<' '/'? letters '>'.
        std::size_t scan = pos + 1;
        const bool closing = scan < text.size() && text[scan] == '/';
        if (closing)
        {
            ++scan;
        }
        const std::size_t name_start = scan;
        while (scan < text.size() && std::isalpha(static_cast<unsigned char>(text[scan])) != 0)
        {
            ++scan;
        }
        const std::string_view name = text.substr(name_start, scan - name_start);
        const int kind = (scan < text.size() && text[scan] == '>') ? classify(name) : 0;
        if (kind == 0)
        {
            // Not a tag we handle: leave the '<' as literal text and keep scanning.
            ++pos;
            continue;
        }

        flush(pos);
        int& depth = kind == 1 ? bold : italic;
        if (closing)
        {
            depth = std::max(0, depth - 1);
        }
        else
        {
            ++depth;
        }
        pos = scan + 1; // skip past '>'
        run_start = pos;
    }

    flush(text.size());
    return runs;
}

int panorama_run_font_weight(int base_weight, bool bold)
{
    // CSS `font-weight: bold` is the absolute keyword 700 (html.css uses `bold`, not
    // the relative `bolder`); non-bold runs keep the element's computed weight.
    return bold ? 700 : base_weight;
}
}
