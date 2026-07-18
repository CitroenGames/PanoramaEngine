#include "ui/panorama/panorama_diagnostics.hpp"
#include "ui/panorama/panorama_dom.hpp"
#include "ui/panorama/panorama_font_atlas.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_text_edit.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace
{
bool expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "standalone test failed: %s\n", message);
    }
    return condition;
}

bool test_clipboard_paste_api()
{
    using namespace panorama;

    PanoramaNode root;
    root.tag = "Panel";
    root.tag_lower = "panel";

    PanoramaNode field;
    field.tag = "TextEntry";
    field.tag_lower = "textentry";

    PanoramaInputController input;
    input.set_focus(root, &field, nullptr);
    panorama_text_entry_set_value(field, "start finish");
    panorama_text_entry_set_selection(field, 6, 12);

    const std::string pasted = "\xE2\x9C\x93\r\nok"; // U+2713 + CRLF + "ok"
    if (!expect(input.handle_paste(root, pasted, nullptr), "paste did not edit the focused TextEntry") ||
        !expect(field.text == "start \xE2\x9C\x93 ok", "paste did not replace selection/normalize CRLF") ||
        !expect(field.text_caret == static_cast<int>(field.text.size()), "paste did not collapse the caret after UTF-8 text") ||
        !expect(!panorama_text_entry_has_selection(field), "paste left a stale selection"))
    {
        return false;
    }

    field.attributes["maxchars"] = "8";
    panorama_text_entry_set_value(field, "12xxxx78");
    panorama_text_entry_set_selection(field, 2, 6);
    const std::string accents = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"; // four U+00E9 codepoints
    if (!expect(input.handle_paste(root, accents, nullptr), "UTF-8 paste did not replace the selection") ||
        !expect(field.text == "12" + accents + "78", "maxchars counted UTF-8 paste bytes instead of codepoints") ||
        !expect(!input.handle_paste(root, {}, nullptr), "empty clipboard payload reported an edit"))
    {
        return false;
    }

    input.reset();
    return expect(!input.handle_paste(root, "ignored", nullptr), "paste edited a field without focus");
}
}

int main()
{
    using namespace panorama;

    if (!test_clipboard_paste_api())
    {
        return 1;
    }

    const PanoramaDiagnostics previous_diagnostics = panorama_diagnostics();
    set_panorama_diagnostics(PanoramaDiagnostics{
        .tree_guard = true,
        .disable_style_index = true,
        .disable_style_sharing = true,
    });
    const PanoramaDiagnostics configured = panorama_diagnostics();
    if (!expect(configured.tree_guard && configured.disable_style_index && configured.disable_style_sharing,
            "public diagnostics configuration did not round-trip") ||
        !expect(panorama_debug_tree_guard_enabled(), "tree guard did not use public diagnostics configuration"))
    {
        set_panorama_diagnostics(previous_diagnostics);
        return 1;
    }
    set_panorama_diagnostics(previous_diagnostics);

    const std::filesystem::path source_root = std::filesystem::path(PANORAMA_TEST_SOURCE_DIR);
    const std::filesystem::path sample_root = source_root / "examples/04_window_raster/sample";
    const std::filesystem::path font_directory = sample_root / "resource/ui/fonts";

    PanoramaFontAtlas discovered;
    PanoramaFontAtlasLoadOptions discovery_options;
    discovery_options.search_directories.push_back(font_directory);
    if (!expect(discovered.load(discovery_options), "font discovery did not honor an explicit search directory") ||
        !expect(discovered.loaded(), "discovered font atlas did not report loaded"))
    {
        return 1;
    }

    const PanoramaTextMeasure measure = discovered.text_measure();
    const auto [width, height] = measure.measure("Standalone", 24.0F, 400, 0.0F);
    if (!expect(std::isfinite(width) && width > 0.0F && std::isfinite(height) && height > 0.0F,
            "loaded font did not produce usable text metrics"))
    {
        return 1;
    }
    discovered.clear();
    if (!expect(!discovered.loaded(), "clear did not reset the font atlas"))
    {
        return 1;
    }

    PanoramaFontAtlas explicit_atlas;
    PanoramaFontAtlasLoadOptions explicit_options;
    explicit_options.resource_root = sample_root;
    explicit_options.faces = {
        PanoramaFontAtlasFace{"resource/ui/fonts/LatoLatin-Regular.ttf", 400},
        PanoramaFontAtlasFace{"resource/ui/fonts/LatoLatin-Bold.ttf", 700},
    };
    if (!expect(explicit_atlas.load(explicit_options), "explicit relative font faces did not load") ||
        !expect(explicit_atlas.loaded(), "explicit font atlas did not report loaded"))
    {
        return 1;
    }

    std::puts("Panorama standalone input, diagnostics, and font configuration passed");
    return 0;
}
