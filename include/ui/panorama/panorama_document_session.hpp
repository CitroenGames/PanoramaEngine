#pragma once

#include "ui/panorama/panorama_dom.hpp"
#include "ui/panorama/panorama_localization.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_style.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace panorama
{
struct PanoramaDocumentSessionOptions
{
    std::filesystem::path resource_root;
    std::size_t max_depth = 8;
    bool localize_text = true;
};

// A script include together with the panel its layout file was loaded into.
// Real Panorama runs each layout file's scripts with that layout's root as
// $.GetContextPanel() — including <Frame> sublayouts nested inside another
// load (e.g. matchmaking_status.js must see the Frame, not the outer panel,
// or its "hide my own root" logic hides the whole friends list).
struct PanoramaScriptInclude
{
    std::string path;
    PanoramaNode* context = nullptr;
};

struct PanoramaDocumentLoadResult
{
    bool loaded = false;
    std::vector<PanoramaScriptInclude> scripts_added;
};

[[nodiscard]] std::unique_ptr<PanoramaNode> clone_panorama_node(
    const PanoramaNode& source,
    PanoramaNode* parent = nullptr);

// Owns the reusable Panorama document-loading state: resource providers,
// localization, parsed/expanded DOM, collected snippets, script includes, and
// stylesheet cascade. Hosts still decide rendering, input, and native APIs.
class PanoramaDocumentSession : public PanoramaNodeLifetimeObserver
{
public:
    PanoramaDocumentSession();
    ~PanoramaDocumentSession() override;

    PanoramaDocumentSession(const PanoramaDocumentSession&) = delete;
    PanoramaDocumentSession& operator=(const PanoramaDocumentSession&) = delete;

    // PanoramaNodeLifetimeObserver: nulls script_refs_ contexts whose layout
    // root is destroyed (scripts then run against the document root).
    void on_panorama_node_destroyed(PanoramaNode& node) override;

    [[nodiscard]] PanoramaResourceManager& resources() noexcept;
    [[nodiscard]] const PanoramaResourceManager& resources() const noexcept;
    [[nodiscard]] PanoramaLocalization& localization() noexcept;
    [[nodiscard]] const PanoramaLocalization& localization() const noexcept;
    [[nodiscard]] PanoramaDocument& document() noexcept;
    [[nodiscard]] const PanoramaDocument& document() const noexcept;
    [[nodiscard]] PanoramaStyleSheet& style_sheet() noexcept;
    [[nodiscard]] const PanoramaStyleSheet& style_sheet() const noexcept;

    // Drops registered resource providers and calls clear_document(); use
    // between independently loaded documents in the same session.
    void clear();
    // Resets the document/stylesheet/snippet/script-include state but keeps
    // registered resource providers. load() calls this itself, so hosts only
    // need it directly when re-loading without touching resources().
    void clear_document();

    // Loads `document_path` as the session's root document: resets prior
    // document state (clear_document()), reloads localization from
    // `options.resource_root`, then parses the XML and recursively resolves
    // its `<styles>` includes and `<Frame src>` children (up to
    // `options.max_depth`) into one styled tree. Returns false if the root
    // resource is missing/unparsable, or if the loaded root ends up empty
    // (valid() is false); check stderr/the log sink for the specific missing
    // path. On success, use document()/style_sheet() to inspect the result —
    // this does not run the cascade or layout itself.
    [[nodiscard]] bool load(std::string_view document_path, PanoramaDocumentSessionOptions options = {});
    // Lower-level primitive load() and <Frame src> expansion are both built on:
    // parses `document_path` and appends its content as children of `parent`
    // (does NOT reset any existing document state, unlike load()). `depth` is
    // the recursion depth so far; returns an empty (loaded=false) result once
    // `depth` exceeds the session's max_depth without reading anything, which
    // is how runaway `<Frame>` cycles are stopped. Prefer load() for a fresh
    // document and load_sublayout() for a runtime BLoadLayout-style load; call
    // this directly only when building custom recursive-include logic.
    [[nodiscard]] PanoramaDocumentLoadResult load_into(
        std::string_view document_path,
        PanoramaNode& parent,
        std::size_t depth = 0);
    // Runtime sublayout load matching real Panorama `BLoadLayout` semantics:
    // parses `source` (resolved against "panorama/layout/") and merges its
    // single root panel ONTO `target` — `target` gains the loaded root's
    // classes/attributes/inline style, and the loaded root's own children
    // become direct children of `target` (unlike <Frame> expansion, which
    // keeps the loaded content nested under a wrapper). Scripts that assume a
    // freshly loaded panel's children are their own direct children (e.g.
    // CS:GO's friendslist.js) depend on this merge. Does not recompute the
    // cascade or layout — the caller does that after checking `.loaded`.
    [[nodiscard]] PanoramaDocumentLoadResult load_sublayout(PanoramaNode& target, std::string_view source);

    // Instantiates a `<snippet name="...">` previously collected while
    // loading the document, appending its subtree as a child of `target`.
    // Returns false (no-op) if no snippet named `name` was collected.
    [[nodiscard]] bool instantiate_snippet(PanoramaNode& target, std::string_view name);
    [[nodiscard]] bool has_snippet(std::string_view name) const;

    // True once a document has been loaded and its root node exists (does not
    // imply the cascade or layout has run).
    [[nodiscard]] bool valid() const;
    // Resolves a Panorama `src`/`include` value (`file://{resources}/...`,
    // `{images}/...`, or a bare relative path) against `base_path`, the way
    // <styles>/<scripts>/<Frame src> resolution does internally.
    [[nodiscard]] std::string resolve_resource_path(std::string_view source, std::string_view base_path) const;
    // Reads `path` through resources() and returns it as text, or an empty
    // string if no provider has it (indistinguishable from an empty file —
    // callers that must tell the two apart should go through resources()
    // directly instead).
    [[nodiscard]] std::string read_text_resource(std::string_view path) const;

    // Every script include collected so far (initial document + all sublayout /
    // frame loads), each paired with its layout-root context panel. Mirrors
    // document().script_includes but keeps the context association.
    [[nodiscard]] const std::vector<PanoramaScriptInclude>& script_refs() const noexcept { return script_refs_; }

private:
    void expand_frames(
        PanoramaNode& parent,
        std::string_view base_path,
        std::size_t depth,
        std::vector<PanoramaScriptInclude>& scripts_added);

    PanoramaResourceManager resources_;
    PanoramaLocalization localization_;
    PanoramaDocument document_;
    PanoramaStyleSheet style_sheet_;
    // Resolved stylesheet path -> style_sheet_ source index. A re-included file
    // keeps its first cascade position (no re-append; csgostyles.css must not
    // jump behind a module's own sheet) and only widens its layout-scope set.
    std::unordered_map<std::string, std::uint16_t> loaded_stylesheets_;
    // Layout file path -> layout scope id (PanoramaNode::style_scope_mark).
    // The initially loaded document is kRootLayoutScope (its sheets style the
    // whole tree); every other layout file gets its own scope so its sheets
    // only style the subtrees it creates (Valve scoping — hudmoney.xml's
    // buymenu.css include must not restyle the HUD's #HudBottomRight).
    std::unordered_map<std::string, std::uint16_t> layout_scopes_;
    std::uint16_t next_layout_scope_ = PanoramaStyleSheet::kRootLayoutScope + 1;
    std::vector<PanoramaScriptInclude> script_refs_;
    std::filesystem::path resource_root_;
    std::size_t max_depth_ = 8;
    bool localize_text_ = true;
};
}
