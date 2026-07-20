#include "ui/panorama/panorama_diagnostics.hpp"
#include "ui/panorama/panorama_dom.hpp"
#include "ui/panorama/panorama_font_atlas.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_source_cooker.hpp"
#include "ui/panorama/panorama_text_edit.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

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

bool write_text_file(const std::filesystem::path& path, std::string_view text)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

bool test_panorama_source_cooker()
{
    using namespace panorama;

    if (!expect(classify_panorama_source_path("layout.xml") == PanoramaSourceKind::Xml,
            "XML source classification failed") ||
        !expect(classify_panorama_source_path("SCRIPT.JS") == PanoramaSourceKind::JavaScript,
            "case-insensitive JavaScript source classification failed") ||
        !expect(classify_panorama_source_path("styles.css") == PanoramaSourceKind::Css,
            "CSS source classification failed") ||
        !expect(!is_panorama_source_path("image.png"), "non-source image was classified as cookable") ||
        !expect(panorama_source_root("csgo/panorama/layout/main.xml") ==
                std::optional<std::filesystem::path>("csgo/panorama"),
            "content-relative Panorama source root was not discovered") ||
        !expect(!panorama_source_root("scripts/game.js"),
            "non-Panorama JavaScript was claimed by the Panorama cooker"))
    {
        return false;
    }

    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() /
        ("panorama_source_cooker_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    const bool wrote_sources =
        write_text_file(scratch / "layout/main.xml", "<root id=\"cooked\" />") &&
        write_text_file(scratch / "styles/main.CSS", "#cooked { color: white; }") &&
        write_text_file(scratch / "scripts/main.js", "globalThis.cooked = true;") &&
        write_text_file(scratch / "images/not_cooked.png", "not an actual image");
    if (!expect(wrote_sources, "could not create Panorama source-cooker fixtures"))
    {
        std::filesystem::remove_all(scratch, error);
        return false;
    }

    PanoramaPackage base;
    const std::vector<std::pair<std::string, std::vector<unsigned char>>> base_resources{
        {"panorama/layout/main.xml", {'o', 'l', 'd'}},
        {"panorama/images/base.bin", {1U, 2U, 3U}},
    };
    std::string package_error;
    if (!expect(base.open_resources(base_resources, "base.pbin", &package_error),
            "base resource package could not be created"))
    {
        std::filesystem::remove_all(scratch, error);
        return false;
    }

    PanoramaPackage cooked;
    PanoramaSourceCookStats stats;
    const bool cooked_ok =
        cook_panorama_source_tree(scratch, cooked, &base, &stats, &package_error);
    const bool valid =
        expect(cooked_ok, "Panorama JS/XML/CSS source tree did not cook") &&
        expect(stats.base_resources == 2U, "base resource count is wrong") &&
        expect(stats.javascript_files == 1U && stats.xml_files == 1U && stats.css_files == 1U,
            "Panorama source-kind counts are wrong") &&
        expect(cooked.entries().size() == 4U, "cooked package resource count is wrong") &&
        expect(cooked.read_text("panorama/layout/main.xml") == "<root id=\"cooked\" />",
            "loose XML did not override the base package resource") &&
        expect(cooked.read_text("panorama/styles/main.css") == "#cooked { color: white; }",
            "CSS source bytes were not retained") &&
        expect(cooked.read_text("panorama/scripts/main.js") == "globalThis.cooked = true;",
            "JavaScript source bytes were not retained") &&
        expect(cooked.contains("panorama/images/base.bin"), "base-only resource was not retained") &&
        expect(!cooked.contains("panorama/images/not_cooked.png"),
            "non-source loose resource was unexpectedly folded into the source package");

    std::filesystem::remove_all(scratch, error);
    return valid;
}
}

int main()
{
    using namespace panorama;

    if (!test_clipboard_paste_api())
    {
        return 1;
    }
    if (!test_panorama_source_cooker())
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

    std::puts("Panorama standalone input, source cooking, diagnostics, and font configuration passed");
    return 0;
}
