#include "ui/panorama/panorama_document_session.hpp"

#include "panorama_string_util.hpp"
#include "ui/panorama/panorama_log.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

namespace openstrike
{
namespace
{
using panorama_strings::starts_with;
}

std::unique_ptr<PanoramaNode> clone_panorama_node(const PanoramaNode& source, PanoramaNode* parent)
{
    auto node = std::make_unique<PanoramaNode>();
    node->tag = source.tag;
    node->tag_lower = source.tag_lower;
    node->id = source.id;
    node->classes = source.classes;
    node->attributes = source.attributes;
    node->inline_style = source.inline_style;
    node->text = source.text;
    node->style_scope_mark = source.style_scope_mark;
    node->parent = parent;
    for (const auto& child : source.children)
    {
        node->children.push_back(clone_panorama_node(*child, node.get()));
    }
    return node;
}

PanoramaDocumentSession::PanoramaDocumentSession()
{
    panorama_add_node_lifetime_observer(*this);
}

PanoramaDocumentSession::~PanoramaDocumentSession()
{
    panorama_remove_node_lifetime_observer(*this);
}

void PanoramaDocumentSession::on_panorama_node_destroyed(PanoramaNode& node)
{
    for (PanoramaScriptInclude& script : script_refs_)
    {
        if (script.context == &node)
        {
            script.context = nullptr;
        }
    }
}

PanoramaResourceManager& PanoramaDocumentSession::resources() noexcept
{
    return resources_;
}

const PanoramaResourceManager& PanoramaDocumentSession::resources() const noexcept
{
    return resources_;
}

PanoramaLocalization& PanoramaDocumentSession::localization() noexcept
{
    return localization_;
}

const PanoramaLocalization& PanoramaDocumentSession::localization() const noexcept
{
    return localization_;
}

PanoramaDocument& PanoramaDocumentSession::document() noexcept
{
    return document_;
}

const PanoramaDocument& PanoramaDocumentSession::document() const noexcept
{
    return document_;
}

PanoramaStyleSheet& PanoramaDocumentSession::style_sheet() noexcept
{
    return style_sheet_;
}

const PanoramaStyleSheet& PanoramaDocumentSession::style_sheet() const noexcept
{
    return style_sheet_;
}

void PanoramaDocumentSession::clear()
{
    resources_.clear();
    resource_root_.clear();
    max_depth_ = 8;
    localize_text_ = true;
    clear_document();
}

void PanoramaDocumentSession::clear_document()
{
    document_ = PanoramaDocument{};
    style_sheet_.clear(); // PanoramaStyleSheet is non-copyable (cache identity)
    script_refs_.clear(); // context pointers die with the document tree
    loaded_stylesheets_.clear();
    layout_scopes_.clear();
    next_layout_scope_ = PanoramaStyleSheet::kRootLayoutScope + 1;
}

bool PanoramaDocumentSession::load(std::string_view document_path, PanoramaDocumentSessionOptions options)
{
    clear_document();
    resource_root_ = std::move(options.resource_root);
    max_depth_ = options.max_depth;
    localize_text_ = options.localize_text;
    localization_.load(resource_root_);

    document_.root = std::make_unique<PanoramaNode>();
    document_.root->tag = "root";
    document_.root->tag_lower = "root";

    const PanoramaDocumentLoadResult result = load_into(document_path, *document_.root, 0);
    return result.loaded && valid();
}

PanoramaDocumentLoadResult PanoramaDocumentSession::load_into(
    std::string_view document_path,
    PanoramaNode& parent,
    std::size_t depth)
{
    PanoramaDocumentLoadResult result;
    if (depth > max_depth_)
    {
        return result;
    }

    const std::string normalized = normalize_panorama_entry_path(document_path);
    const std::string xml = read_text_resource(normalized);
    if (xml.empty())
    {
        pano_log_warning("Panorama document session: missing resource '{}'", normalized);
        return result;
    }

    PanoramaDocument fragment = parse_panorama_xml(xml);
    const PanoramaLocalizeCallback localize = [this](std::string_view text) {
        return localization_.localize(text);
    };
    if (fragment.root != nullptr)
    {
        if (localize_text_)
        {
            localization_.localize_tree(*fragment.root);
        }
        ensure_panorama_text_entry_placeholders(*fragment.root, localize);
    }

    // Layout scope: real Panorama scopes a layout file's <styles> to the panels
    // that layout creates. The initially loaded document is the root scope (its
    // sheets style everything); every other layout file gets its own scope id,
    // stamped on the nodes it creates below. CS:GO depends on this both ways:
    // hudmoney.xml includes buymenu.css whose `#HudBottomRight { padding-bottom:
    // 96px; }` must not push the HUD's weapon panel up, while hudweaponpanel.css
    // must still style the shell its module root was merged onto.
    const bool is_root_layout = depth == 0 && document_.root != nullptr && &parent == document_.root.get();
    std::uint16_t layout_scope = PanoramaStyleSheet::kRootLayoutScope;
    if (!is_root_layout)
    {
        const auto [scope_it, inserted] = layout_scopes_.try_emplace(normalized, next_layout_scope_);
        if (inserted)
        {
            ++next_layout_scope_;
        }
        layout_scope = scope_it->second;
    }

    // Valve loads each stylesheet FILE once per document: a sheet re-included by
    // a later sublayout keeps its original cascade position. Every CS:GO module
    // layout re-includes csgostyles.css; without dedup its rules would be
    // re-appended AFTER the module's own sheet and win equal-specificity
    // conflicts (e.g. csgostyles' 320x22 `ProgressBar` would beat
    // hudhealtharmor.css's 82x12 bars — the real HUD renders 82x12). A
    // re-include only widens the sheet's layout-scope set.
    for (const std::string& include : fragment.stylesheet_includes)
    {
        const std::string css_path = resolve_resource_path(include, normalized);
        if (const auto it = loaded_stylesheets_.find(css_path); it != loaded_stylesheets_.end())
        {
            style_sheet_.add_source_scope(it->second, layout_scope);
            continue;
        }
        const std::string css = read_text_resource(css_path);
        if (!css.empty())
        {
            loaded_stylesheets_.emplace(css_path, style_sheet_.add_source(css, layout_scope));
        }
    }
    for (const std::string& inline_css : fragment.inline_styles)
    {
        style_sheet_.add_source(inline_css, layout_scope);
    }

    for (const std::string& include : fragment.script_includes)
    {
        std::string script_path = resolve_resource_path(include, normalized);
        document_.script_includes.push_back(script_path);
        // `parent` is this layout file's root: scripts run with it as their
        // $.GetContextPanel() (frame sublayouts get the frame, not the outer load target).
        script_refs_.push_back(PanoramaScriptInclude{script_path, &parent});
        result.scripts_added.push_back(PanoramaScriptInclude{std::move(script_path), &parent});
    }

    for (auto& [name, snippet] : fragment.snippets)
    {
        if (snippet != nullptr)
        {
            if (localize_text_)
            {
                localization_.localize_tree(*snippet);
            }
            ensure_panorama_text_entry_placeholders(*snippet, localize);
            // Snippet instances keep their defining layout's style scope (the
            // clone copies the mark), wherever they are instantiated.
            if (layout_scope != PanoramaStyleSheet::kRootLayoutScope)
            {
                for (const auto& snippet_child : snippet->children)
                {
                    snippet_child->style_scope_mark = layout_scope;
                }
            }
        }
        document_.snippets[name] = std::move(snippet);
    }

    if (fragment.root != nullptr)
    {
        for (auto& child : fragment.root->children)
        {
            child->parent = &parent;
            // Scoped layouts stamp the panels they create so their sheets only
            // style these subtrees (root-scope sheets style everything).
            if (layout_scope != PanoramaStyleSheet::kRootLayoutScope)
            {
                child->style_scope_mark = layout_scope;
            }
            parent.children.push_back(std::move(child));
        }
    }

    result.loaded = true;
    expand_frames(parent, normalized, depth, result.scripts_added);
    return result;
}

PanoramaDocumentLoadResult PanoramaDocumentSession::load_sublayout(PanoramaNode& target, std::string_view source)
{
    const std::string path = resolve_resource_path(source, "panorama/layout/");
    const std::size_t first_new_child = target.children.size();
    PanoramaDocumentLoadResult result = load_into(path, target, 1);

    // Real Panorama BLoadLayout semantics: the layout file's single root panel is
    // merged ONTO the target panel — the target gains the root's classes,
    // attributes, and inline style, and the root's children become the target's
    // direct children. CS:GO scripts rely on this: friendslist.js does
    // GetContextPanel().FindChild('AntiAddiction') + MoveChildBefore to slot the
    // local playercard into the friendslist's own down-flow, which only works
    // when the loaded root's children are direct children of the loaded panel.
    // (<Frame> expansion intentionally keeps its content nested instead.)
    if (result.loaded && target.children.size() == first_new_child + 1)
    {
        std::unique_ptr<PanoramaNode> wrapper = std::move(target.children[first_new_child]);
        target.children.erase(target.children.begin() + static_cast<std::ptrdiff_t>(first_new_child));
        for (const std::string& klass : wrapper->classes)
        {
            if (!target.has_class(klass))
            {
                target.classes.push_back(klass);
            }
        }
        for (const auto& [key, value] : wrapper->attributes)
        {
            target.attributes.try_emplace(key, value); // target's own attributes win
        }
        if (!wrapper->inline_style.empty())
        {
            target.inline_style += target.inline_style.empty() ? wrapper->inline_style
                                                               : (";" + wrapper->inline_style);
        }
        // The loaded layout's scope mark travels with the merge: its sheets must
        // style the target panel itself (hudweaponpanel.css's `.WeaponPanel`
        // styles the #HudWeaponPanel shell its module root merged onto).
        if (wrapper->style_scope_mark != 0)
        {
            target.style_scope_mark = wrapper->style_scope_mark;
        }
        for (auto& grandchild : wrapper->children)
        {
            grandchild->parent = &target;
            target.children.push_back(std::move(grandchild));
        }
        // The wrapper node is discarded; nothing references it yet (its scripts
        // run after this returns, against `target` as their context).
    }

    // Inserted nodes start style-dirty, but the ancestor chain must learn about
    // them for compute_invalidated to reach the new subtree.
    target.mark_style_dirty();
    return result;
}

bool PanoramaDocumentSession::instantiate_snippet(PanoramaNode& target, std::string_view name)
{
    const auto it = document_.snippets.find(std::string(name));
    if (it == document_.snippets.end() || it->second == nullptr)
    {
        return false;
    }
    // Real Panorama BLoadLayoutSnippet semantics match BLoadLayout (see
    // load_sublayout above): a single snippet root panel is merged ONTO the
    // target — the target gains the root's classes/attributes/inline style and
    // the root's children become the target's direct children. CS:GO's play menu
    // relies on this: map tiles are ToggleButtons that load the
    // "MapGroupSelection" snippet (root `Panel.map-selection-btn`), and the
    // `.map-selection-btn:selected` styling must match the ToggleButton itself.
    if (it->second->children.size() == 1)
    {
        const std::unique_ptr<PanoramaNode> wrapper = clone_panorama_node(*it->second->children.front(), nullptr);
        if (target.id.empty() && !wrapper->id.empty())
        {
            target.id = wrapper->id;
        }
        if (target.text.empty() && !wrapper->text.empty())
        {
            target.text = wrapper->text;
        }
        for (const std::string& klass : wrapper->classes)
        {
            if (!target.has_class(klass))
            {
                target.classes.push_back(klass);
            }
        }
        for (const auto& [key, value] : wrapper->attributes)
        {
            target.attributes.try_emplace(key, value); // target's own attributes win
        }
        if (!wrapper->inline_style.empty())
        {
            target.inline_style += target.inline_style.empty() ? wrapper->inline_style
                                                               : (";" + wrapper->inline_style);
        }
        // As with BLoadLayout: the snippet's defining-layout scope travels with
        // the merge so that layout's sheets style the target panel itself.
        if (wrapper->style_scope_mark != 0)
        {
            target.style_scope_mark = wrapper->style_scope_mark;
        }
        for (auto& grandchild : wrapper->children)
        {
            grandchild->parent = &target;
            target.children.push_back(std::move(grandchild));
        }
    }
    else
    {
        for (const auto& child : it->second->children)
        {
            target.children.push_back(clone_panorama_node(*child, &target));
        }
    }
    target.mark_style_dirty(); // ancestors must route the partial recompute here
    return true;
}

bool PanoramaDocumentSession::has_snippet(std::string_view name) const
{
    return document_.snippets.find(std::string(name)) != document_.snippets.end();
}

bool PanoramaDocumentSession::valid() const
{
    return document_.root != nullptr && !document_.root->children.empty();
}

std::string PanoramaDocumentSession::resolve_resource_path(std::string_view source, std::string_view base_path) const
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
    return normalize_panorama_entry_path((base_parent / normalized).lexically_normal().generic_string());
}

std::string PanoramaDocumentSession::read_text_resource(std::string_view path) const
{
    if (std::optional<std::string> text = resources_.read_text(path))
    {
        return *text;
    }
    return {};
}

void PanoramaDocumentSession::expand_frames(
    PanoramaNode& parent,
    std::string_view base_path,
    std::size_t depth,
    std::vector<PanoramaScriptInclude>& scripts_added)
{
    for (const auto& child : parent.children)
    {
        if (child->tag_lower == "frame")
        {
            // A frame that already has children was expanded by an earlier load
            // pass (frames are empty in source XML) — re-expanding it would
            // duplicate its content and script includes.
            if (!child->children.empty())
            {
                continue;
            }
            const auto src = child->attributes.find("src");
            if (src != child->attributes.end() && !src->second.empty())
            {
                const std::string frame_path = resolve_resource_path(src->second, base_path);
                PanoramaDocumentLoadResult frame = load_into(frame_path, *child, depth + 1);
                // The frame layout's sheets style the <Frame> panel itself too
                // (matchmaking_status.css sizes the .matchmaking-status-container
                // frame node hud.xml declares).
                if (const auto scope_it = layout_scopes_.find(normalize_panorama_entry_path(frame_path));
                    scope_it != layout_scopes_.end())
                {
                    child->style_scope_mark = scope_it->second;
                }
                scripts_added.insert(
                    scripts_added.end(),
                    std::make_move_iterator(frame.scripts_added.begin()),
                    std::make_move_iterator(frame.scripts_added.end()));
            }
            continue;
        }
        expand_frames(*child, base_path, depth, scripts_added);
    }
}
}
