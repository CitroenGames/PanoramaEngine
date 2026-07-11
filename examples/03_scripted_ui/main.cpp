// PanoramaEngine example 03 — scripted, interactive UI.
//
// Demonstrates the full headless loop a host runs every frame:
//
//   PanoramaDocumentSession (XML + CSS + <scripts>) -> PanoramaRuntime (QuickJS
//   with the Panorama `$` / Panel surface bound to the live PanoramaNode tree)
//   -> PanoramaInputController (hit-test + hover/active + onactivate bubbling)
//   -> consume_dirty -> recompute styles -> relayout.
//
// A synthetic pointer clicks the TOGGLE button twice; the document's own
// JavaScript updates the status label and flips a class, and the example shows
// the DOM reacting — no window or GPU involved.
#include "ui/panorama/panorama_document_session.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_runtime.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace
{
constexpr std::string_view kLayoutXml = R"xml(<root>
    <styles>
        <include src="file://{resources}/styles/app.css" />
    </styles>
    <scripts>
        <include src="file://{resources}/scripts/app.js" />
    </scripts>
    <Panel id="App" class="app">
        <Label id="Status" class="status" text="Ready" />
        <Button id="Toggle" class="button" onactivate="OnToggle()">
            <Label class="button-label" text="TOGGLE" />
        </Button>
    </Panel>
</root>
)xml";

constexpr std::string_view kStyleCss = R"css(
.app
{
    flow-children: down;
    width: 100%;
    height: 100%;
    padding: 24px;
    background-color: #1b1e24ff;
}

.status
{
    font-size: 24px;
    color: #aab2bdff;
    margin-bottom: 16px;
}

.status--armed
{
    color: #5cd97aff;
}

.button
{
    width: 220px;
    height: 56px;
    background-color: #2a2f37ff;
}

.button:hover
{
    background-color: #39414cff;
}

.button-label
{
    font-size: 22px;
    color: white;
}
)css";

// The document's own script: plain Panorama JavaScript, as shipped inside CS:GO
// packages. `$('#id')` selects panels, Panel properties/methods mutate the DOM,
// and $.RegisterEventHandler/$.DispatchEvent ride the engine's event bus.
constexpr std::string_view kAppJs = R"js(
var g_count = 0;

function OnToggle()
{
    g_count += 1;
    var status = $('#Status');
    status.text = 'Clicked ' + g_count + ' time(s)';
    status.SetHasClass('status--armed', (g_count % 2) === 1);
    $.DispatchEvent('AppToggled');
}

$.RegisterEventHandler('AppToggled', $.GetContextPanel(), function () {
    $.Msg('app.js: AppToggled fired, count = ' + g_count);
});

$.Msg('app.js loaded');
)js";

void recompute(panorama::PanoramaDocumentSession& session, panorama::PanoramaNode& root)
{
    session.style_sheet().compute(root);
    panorama::panorama_apply_visibility_overrides(root);
    panorama::panorama_apply_control_presentation(root);
    panorama::layout_panorama_tree(root, 1280.0F, 720.0F);
}

void print_status(panorama::PanoramaNode& root)
{
    const panorama::PanoramaNode* status = root.find_by_id("Status");
    if (status == nullptr)
    {
        std::printf("#Status not found\n");
        return;
    }
    std::printf(
        "#Status text=\"%s\" armed=%s\n",
        status->text.c_str(),
        status->has_class("status--armed") ? "yes" : "no");
}
}

int main()
{
    using namespace panorama;

    // Load the document (example-01 pipeline) ...
    PanoramaDocumentSession session;
    auto memory = std::make_unique<PanoramaMemoryResourceProvider>();
    memory->add_text("panorama/layout/app.xml", kLayoutXml);
    memory->add_text("panorama/styles/app.css", kStyleCss);
    memory->add_text("panorama/scripts/app.js", kAppJs);
    session.resources().add_provider(std::move(memory));
    if (!session.load("panorama/layout/app.xml"))
    {
        std::fprintf(stderr, "failed to load panorama/layout/app.xml\n");
        return 1;
    }

    PanoramaNode& root = *session.document().root;
    recompute(session, root);

    // ... then boot the JS runtime against the live tree. The session already
    // collected the <scripts> includes; the runtime reads their source through
    // the same resource manager.
    PanoramaRuntime runtime;
    if (!runtime.initialize(root, session.resources(), session.document().script_includes))
    {
        std::fprintf(stderr, "failed to initialize the Panorama JS runtime\n");
        return 1;
    }

    // Script execution may have mutated the DOM — same dance a host does each
    // frame: consume_dirty -> recompute + relayout.
    if (runtime.consume_dirty())
    {
        recompute(session, root);
    }

    std::printf("before any input:\n  ");
    print_status(root);

    // Drive the UI with synthetic pointer samples. PanoramaInputController owns
    // hover/active/focus flags, bubbling, radio groups, and dropdown emulation;
    // a real host feeds it the same calls from SDL/Win32 mouse state.
    PanoramaInputController input;
    PanoramaNode* toggle = root.find_by_id("Toggle");
    if (toggle == nullptr)
    {
        std::fprintf(stderr, "#Toggle not found\n");
        return 1;
    }

    const float click_x = toggle->layout.x + toggle->layout.width * 0.5F;
    const float click_y = toggle->layout.y + toggle->layout.height * 0.5F;
    for (int click = 1; click <= 2; ++click)
    {
        bool dirty = input.update_pointer(root, click_x, click_y, true, &runtime);  // press
        dirty |= input.update_pointer(root, click_x, click_y, false, &runtime);     // release
        runtime.update(1.0 / 60.0); // pump $.Schedule + the JS micro-task queue
        if (dirty || runtime.consume_dirty())
        {
            recompute(session, root);
        }
        std::printf("after click %d:\n  ", click);
        print_status(root);
    }

    runtime.shutdown();
    return 0;
}
