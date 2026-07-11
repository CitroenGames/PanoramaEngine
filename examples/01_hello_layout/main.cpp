// PanoramaEngine example 01 — hello layout.
//
// Demonstrates the minimal embedding pipeline with zero host integration:
//
//   resources (in-memory) -> PanoramaDocumentSession (XML parse + CSS cascade
//   sources) -> PanoramaStyleSheet::compute -> layout_panorama_tree -> inspect
//   the resulting box tree.
//
// No renderer, no window, no filesystem: layout XML and Panorama CSS are
// registered with a PanoramaMemoryResourceProvider and the laid-out tree is
// printed to stdout.
#include "ui/panorama/panorama_document_session.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace
{
// A Panorama layout document, exactly as it would live inside a .pbin package.
// <styles> includes resolve through the session's resource manager.
constexpr std::string_view kLayoutXml = R"xml(<root>
    <styles>
        <include src="file://{resources}/styles/hello.css" />
    </styles>
    <Panel id="Screen" class="screen">
        <Panel id="NavBar" class="nav">
            <Label id="Title" class="title" text="OPENSTRIKE" />
            <Button id="PlayButton" class="nav-button" onactivate="OnPlay()">
                <Label text="PLAY" />
            </Button>
            <Button id="SettingsButton" class="nav-button">
                <Label text="SETTINGS" />
            </Button>
        </Panel>
        <Panel id="Content" class="content">
            <Label class="hint" text="fill-parent-flow content area" />
        </Panel>
    </Panel>
</root>
)xml";

// Panorama CSS — note the engine-specific sizing primitives (fill-parent-flow,
// flow-children) that have no direct equivalent in web CSS.
constexpr std::string_view kStyleCss = R"css(
@define navBackground: #14161aff;

.screen
{
    flow-children: down;
    width: 100%;
    height: 100%;
    background-color: #1e2126ff;
}

.nav
{
    flow-children: right;
    width: 100%;
    height: 96px;
    padding: 16px;
    background-color: navBackground;
}

.title
{
    font-size: 32px;
    color: white;
    margin-right: 24px;
}

.nav-button
{
    width: 160px;
    height: fill-parent-flow( 1.0 );
    margin-right: 8px;
    background-color: #2a2e35ff;
    transition: background-color 0.15s ease-in-out 0s;
}

.nav-button:hover
{
    background-color: #3a404aff;
}

.content
{
    width: 100%;
    height: fill-parent-flow( 1.0 );
    background-color: #23262cff;
}

.hint
{
    font-size: 20px;
    color: #8a919c;
}
)css";

void print_tree(const panorama::PanoramaNode& node, int depth)
{
    const panorama::PanoramaLayoutBox& box = node.layout;
    std::printf(
        "%*s<%s>%s%s  box=(%.0f, %.0f  %.0fx%.0f)%s\n",
        depth * 2,
        "",
        node.tag.c_str(),
        node.id.empty() ? "" : " #",
        node.id.c_str(),
        box.x,
        box.y,
        box.width,
        box.height,
        node.computed.visible ? "" : "  [hidden]");
    for (const auto& child : node.children)
    {
        print_tree(*child, depth + 1);
    }
}
}

int main()
{
    using namespace panorama;

    // 1. Register the document + stylesheet with an in-memory resource provider.
    //    Real hosts add a PanoramaPackageResourceProvider (.pbin zip) and/or a
    //    PanoramaDirectoryResourceProvider (loose files) instead.
    PanoramaDocumentSession session;
    auto memory = std::make_unique<PanoramaMemoryResourceProvider>();
    memory->add_text("panorama/layout/hello.xml", kLayoutXml);
    memory->add_text("panorama/styles/hello.css", kStyleCss);
    session.resources().add_provider(std::move(memory));

    // 2. Load the document: parses the XML, pulls in <styles> includes, expands
    //    <Frame src> references, and collects <scripts> includes + <snippets>.
    if (!session.load("panorama/layout/hello.xml"))
    {
        std::fprintf(stderr, "failed to load panorama/layout/hello.xml\n");
        return 1;
    }

    PanoramaNode& root = *session.document().root;

    // 3. Run the cascade (specificity + source order + inline styles), then the
    //    standard post-passes the engine expects before layout.
    session.style_sheet().compute(root);
    panorama_apply_visibility_overrides(root);
    panorama_apply_control_presentation(root);

    // 4. Solve layout for a 1280x720 viewport. The default text measurer is a
    //    metrics-free approximation; PanoramaFontAtlas provides the built-in
    //    FreeType-backed path for pixel-accurate label sizing.
    layout_panorama_tree(root, 1280.0F, 720.0F);

    std::printf("Laid out %zu rule(s) at 1280x720:\n\n", session.style_sheet().rules().size());
    print_tree(root, 0);

    // 5. Interaction flags feed pseudo-class rules: flag the Play button hovered,
    //    recompute, and show that the :hover background applies.
    if (PanoramaNode* play = root.find_by_id("PlayButton"))
    {
        play->hovered = true;
        session.style_sheet().compute(root);
        const PanoramaColor& bg = play->computed.background_color;
        std::printf("\n#PlayButton background while :hover -> rgba(%u, %u, %u, %u)\n", bg.r, bg.g, bg.b, bg.a);
    }

    return 0;
}
