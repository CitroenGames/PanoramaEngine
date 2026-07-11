#pragma once

#include "ui/panorama/panorama_dom.hpp"
#include "ui/panorama/panorama_package.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace panorama
{
struct PanoramaRuntimeScript
{
    std::string origin;
    std::string source;
};

// A script include path paired with the panel that script's layout file was
// loaded into. Scripts run with that panel as $.GetContextPanel() (real
// Panorama semantics: each layout file's scripts see their own layout root,
// including <Frame> sublayouts). A null context falls back to the document.
struct PanoramaRuntimeScriptInclude
{
    std::string path;
    PanoramaNode* context = nullptr;
};

class PanoramaRuntimeClient
{
public:
    virtual ~PanoramaRuntimeClient() = default;

    [[nodiscard]] virtual std::vector<PanoramaRuntimeScript> bootstrap_scripts() { return {}; }
    virtual void on_host_action(const std::string&, const std::string&) {}
};

[[nodiscard]] PanoramaRuntimeScript make_panorama_csgo_bootstrap_script();

// PanoramaRuntime hosts a QuickJS interpreter and exposes a Panorama-compatible
// JavaScript surface (`$`, Panel objects, an event bus) backed by live
// PanoramaNode elements. It runs the scripts a converted Panorama document pulls
// in, so that the UI can drive itself the way it does inside CS:GO.
//
// This is intentionally a best-effort port: Valve's own scripts reach for a huge
// native API (UiToolkitAPI, GameInterfaceAPI, persona/inventory data models, ...)
// plus a live game backend. Those namespaces are installed as graceful no-op
// stubs so scripts execute as far as they can without throwing, and self-authored
// Panorama-style layouts become fully interactive.
//
// The Panel binding operates on PanoramaNode (our own DOM), so the host can run
// CS:GO's real scripts against the natively-rendered tree.
class PanoramaRuntime
{
public:
    PanoramaRuntime();
    ~PanoramaRuntime();

    PanoramaRuntime(const PanoramaRuntime&) = delete;
    PanoramaRuntime& operator=(const PanoramaRuntime&) = delete;

    // Boots the interpreter against the PanoramaNode tree rooted at `root` and runs
    // every script in `scripts` (package-relative paths). Script text is read from
    // `package`, falling back to `resource_root` on disk. Returns false if the
    // interpreter could not be created.
    bool initialize(
        PanoramaNode& root,
        const PanoramaPackage& package,
        const std::vector<std::string>& scripts,
        const std::filesystem::path& resource_root = {});

    bool initialize(
        PanoramaNode& root,
        const PanoramaResourceManager& resources,
        const std::vector<std::string>& scripts,
        const std::filesystem::path& resource_root = {});

    // Same, but each script carries the panel its layout file was loaded into
    // (its $.GetContextPanel()); null contexts resolve to the document root.
    // Distinct name (not an overload): a brace-init scripts argument would
    // otherwise be ambiguous between the two vector element types.
    bool initialize_with_script_contexts(
        PanoramaNode& root,
        const PanoramaResourceManager& resources,
        const std::vector<PanoramaRuntimeScriptInclude>& scripts,
        const std::filesystem::path& resource_root = {});

    void shutdown();

    // Pumps scheduled callbacks and the JS micro-task queue. dt is in seconds.
    void update(double dt_seconds);

    // Fires a Panorama event into the JS event bus from native code.
    void dispatch_event(const std::string& event_name);

    // Fires 'PropertyTransitionEnd' (panelName, propertyName) for a CSS
    // transition that completed on `panel` (PanoramaAnimationAdvanceResult::
    // transition_ends). Delivery honors RegisterEventHandler's panel scoping:
    // handlers registered for a different panel are skipped. CS:GO's menus key
    // panel teardown off this event (mainmenu.js hides a faded-out tab with
    // `visible = false`, which is what frees its full-screen hit-test box).
    void dispatch_property_transition_end(PanoramaNode& panel, const char* property);

    // Runs the JS in a node's `on<event>` attribute (e.g. onactivate) with that
    // node as the context panel. Used by native input once hit-testing exists.
    void run_node_handler(PanoramaNode& node, const std::string& event_attr);

    // Host loaders for sublayouts: `file_loader` loads a layout XML into a panel
    // (BLoadLayout/LoadLayout), `snippet_loader` instantiates a named <snippet>
    // (BLoadLayoutSnippet), and `snippet_exists` answers BHasLayoutSnippet().
    // Set before initialize() so init-time loads work.
    using LayoutLoader = std::function<void(PanoramaNode& target, const std::string& source)>;
    using SnippetExists = std::function<bool(const std::string& name)>;
    void set_layout_loaders(LayoutLoader file_loader, LayoutLoader snippet_loader, SnippetExists snippet_exists = {});

    // Host bridge for engine actions the JS triggers: action "cmd" (run a console
    // command) and "play" (start a match). Lets the menu reach the engine even
    // though the matchmaking APIs are otherwise stubbed. Set before initialize().
    using HostActionHandler = std::function<void(const std::string& action, const std::string& arg)>;
    void set_host_action_handler(HostActionHandler handler);

    // Host bridge for script-driven focus: Panel.SetFocus()/Focus() calls this with
    // the target panel (null = clear focus). The host moves keyboard focus through
    // its PanoramaInputController (the focus authority). Set before initialize().
    using FocusRequestHandler = std::function<void(PanoramaNode* panel)>;
    void set_focus_request_handler(FocusRequestHandler handler);

    void set_client(PanoramaRuntimeClient* client);
    void set_bootstrap_scripts(std::vector<PanoramaRuntimeScript> scripts);

    // Evaluates `source` with `context` as the active context panel (so a loaded
    // sublayout's scripts see GetContextPanel() == the panel they were loaded into).
    void run_source_in_context(const std::string& source, const std::string& origin, PanoramaNode& context);

    // True if script execution mutated the DOM (classes, attributes, text, child
    // list, visibility) since the last call; clears the flag. The host uses this
    // to decide when to recompute styles + relayout.
    [[nodiscard]] bool consume_dirty();

    [[nodiscard]] bool active() const noexcept;

    // Opaque implementation detail; defined in the .cpp. Public only so the
    // QuickJS C-function trampolines in that translation unit can name it.
    struct Impl;

private:
    void run_script_source(const std::string& source, const char* origin);

    LayoutLoader file_loader_;
    LayoutLoader snippet_loader_;
    SnippetExists snippet_exists_;
    HostActionHandler host_action_;
    FocusRequestHandler focus_request_;
    PanoramaRuntimeClient* client_ = nullptr;
    std::vector<PanoramaRuntimeScript> bootstrap_scripts_;
    std::unique_ptr<Impl> impl_;
};
}
