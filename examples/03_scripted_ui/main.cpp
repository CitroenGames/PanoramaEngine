// PanoramaEngine example 03 — scripted, interactive UI.
//
// Demonstrates the standalone PanoramaView facade:
//
//   resources -> PanoramaView (document + QuickJS + input + cascade + layout +
//   animation + draw-list lifecycle).
//
// A synthetic pointer clicks the TOGGLE button twice; the document's own
// JavaScript updates the status label and flips a class, and the example shows
// the DOM reacting — no window or GPU involved.
#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_view.hpp"

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
        <Panel id="Dynamic" />
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
    if (g_count === 1)
        $('#Dynamic').BLoadLayout('child.xml', false, false);
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

constexpr std::string_view kChildLayoutXml = R"xml(<root>
    <scripts>
        <include src="file://{resources}/scripts/child.js" />
    </scripts>
    <Panel class="dynamic-child">
        <Label id="ChildStatus" text="pending" />
    </Panel>
</root>
)xml";

constexpr std::string_view kChildJs = R"js(
$.GetContextPanel().FindChildTraverse('ChildStatus').text = 'child loaded';
)js";

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

bool status_matches(panorama::PanoramaNode& root, std::string_view expected_text, bool expected_armed)
{
    const panorama::PanoramaNode* status = root.find_by_id("Status");
    return status != nullptr && status->text == expected_text &&
           status->has_class("status--armed") == expected_armed;
}
}

int main()
{
    using namespace panorama;

    PanoramaView view;
    view.set_viewport(1280.0F, 720.0F);
    auto memory = std::make_unique<PanoramaMemoryResourceProvider>();
    memory->add_text("panorama/layout/app.xml", kLayoutXml);
    memory->add_text("panorama/styles/app.css", kStyleCss);
    memory->add_text("panorama/scripts/app.js", kAppJs);
    memory->add_text("panorama/layout/child.xml", kChildLayoutXml);
    memory->add_text("panorama/scripts/child.js", kChildJs);
    view.resources().add_provider(std::move(memory));
    if (!view.load("panorama/layout/app.xml"))
    {
        std::fprintf(stderr, "failed to load panorama/layout/app.xml\n");
        return 1;
    }

    PanoramaNode& root = *view.root();

    const PanoramaViewUpdateResult idle = view.update(0.0F);
    if (idle.style_changed || idle.layout_changed || idle.draw_list_rebuilt)
    {
        std::fprintf(stderr, "unchanged view unexpectedly rebuilt a pipeline stage\n");
        return 1;
    }

    view.set_viewport(640.0F, 360.0F);
    const PanoramaViewUpdateResult resized = view.update(0.0F);
    if (!resized.layout_changed || !resized.draw_list_rebuilt || root.layout.width != 640.0F ||
        root.layout.height != 360.0F)
    {
        std::fprintf(stderr, "view resize did not relayout and rebuild the draw list\n");
        return 1;
    }

    std::printf("before any input:\n  ");
    print_status(root);
    if (!status_matches(root, "Ready", false))
    {
        return 1;
    }

    // A real host feeds the same calls from SDL/Win32 mouse state and renders
    // view.draw_list(); the view owns the engine-side sequencing.
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
        (void)view.update_pointer(click_x, click_y, true);  // press
        (void)view.update_pointer(click_x, click_y, false); // release
        const PanoramaViewUpdateResult changed = view.update(1.0F / 60.0F);
        std::printf("after click %d:\n  ", click);
        print_status(root);
        const std::string expected = "Clicked " + std::to_string(click) + " time(s)";
        const PanoramaNode* child_status = root.find_by_id("ChildStatus");
        if (!changed.style_changed || !changed.layout_changed || !changed.draw_list_rebuilt ||
            !status_matches(root, expected, (click % 2) == 1) || child_status == nullptr ||
            child_status->text != "child loaded")
        {
            std::fprintf(stderr, "PanoramaView did not propagate scripted click %d\n", click);
            return 1;
        }
    }

    return 0;
}
