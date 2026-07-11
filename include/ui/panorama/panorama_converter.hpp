#pragma once

#include "ui/panorama/panorama_package.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Best-effort, lossy source-to-source converter from Panorama XML/CSS to
// RmlUi's RML/RCSS markup. This is an ALTERNATE integration path for hosts
// that want to render (a visually approximate version of) Panorama UI through
// RmlUi's own layout and renderer instead of this engine's native
// layout/paint pipeline (panorama_layout.hpp / panorama_paint.hpp) — it does
// not feed back into anything else in this library and nothing else in this
// library depends on it.
//
// Because RmlUi's box model and CSS dialect differ from Panorama's, the
// conversion actively drops what it cannot faithfully translate rather than
// emit markup RmlUi would reject: Panorama-only sizing primitives
// (`fit-children`, `fill-parent-flow`, `width-percentage`/`height-percentage`),
// gradients, `blur`, transforms, unresolved `@define`/theme-variable values,
// `@keyframes`/`@media` blocks, and any other value `is_safe_css_value`/
// `is_rml_color`/`is_length_like` (in panorama_converter.cpp) can't map end up
// silently omitted from the output rather than converted approximately. Do
// not expect pixel parity with the native pipeline; use this when "renders
// something reasonable in an RmlUi host" is the goal, not "renders exactly
// like real Panorama".
namespace openstrike
{
struct PanoramaConversionOptions
{
    // Extra directory searched for resources not found through the resource
    // manager/package passed to convert_panorama_document (registered as a
    // PanoramaDirectoryResourceProvider at lower priority than the package —
    // see the `.cpp` overload that takes a PanoramaPackage). Leave empty to
    // resolve strictly through the given package/resource manager.
    std::filesystem::path resource_root;
    // Recursion cap for `<Frame src>` expansion, mirroring
    // PanoramaDocumentSessionOptions::max_depth. A document nesting Frames
    // deeper than this stops expanding and records a
    // "... (frame recursion limit)" entry in `missing_resources`.
    std::size_t max_frame_depth = 8;
    // When true (default), prepends a small built-in RCSS baseline
    // (`kDefaultPanoramaStyle` in panorama_converter.cpp: box-sizing: border-box,
    // flex-based `.panorama-flow-right`/`.panorama-flow-down`, a `.hidden`
    // class, etc.) ahead of the converted stylesheets, since Panorama has no
    // built-in RmlUi-equivalent default stylesheet of its own.
    bool include_default_style = true;
};

struct PanoramaConversionResult
{
    // The converted document as a single self-contained RML string (styles
    // inlined into a `<head>`, converted body markup in `<body>`) — write
    // this directly to a `.rml` file or feed it to RmlUi's loader.
    std::string rml;
    // Resolved paths of every `<scripts>` include found while converting,
    // collected but NOT translated — Panorama JavaScript has no RmlUi
    // equivalent, so the host must decide separately whether/how to run these
    // (e.g. drop them, or run them against this engine's own PanoramaRuntime
    // alongside the RmlUi-rendered visuals).
    std::vector<std::string> scripts;
    // Resource paths (`<styles>`/`<Frame src>` includes, or a frame-depth-limit
    // marker) that were referenced but could not be read through the
    // package/resource manager passed in. A non-empty list means the
    // conversion is incomplete, not necessarily that it failed outright — log
    // or surface these to catch missing assets before shipping the converted
    // output.
    std::vector<std::string> missing_resources;
};

// Converts the document at `document_path` (and everything it recursively
// includes via `<styles>`/`<Frame src>`) by reading through `package`. When
// `options.resource_root` is set, it's layered in as a secondary lookup path
// for entries not found in `package` (see PanoramaConversionOptions above).
[[nodiscard]] PanoramaConversionResult convert_panorama_document(
    const PanoramaPackage& package,
    std::string_view document_path,
    const PanoramaConversionOptions& options = {});

// Same conversion, but reading through an already-assembled
// PanoramaResourceManager (e.g. one a host already built for the native
// pipeline) instead of a single PanoramaPackage — use this overload when
// resources come from memory, loose files, or multiple layered providers.
[[nodiscard]] PanoramaConversionResult convert_panorama_document(
    const PanoramaResourceManager& resources,
    std::string_view document_path,
    const PanoramaConversionOptions& options = {});

// Converts a standalone Panorama CSS stylesheet to RCSS using the same
// declaration-by-declaration rules `convert_panorama_document` applies to
// `<styles>` includes (see panorama_converter.cpp), without needing a whole
// document or resource provider. `@`-rules (`@keyframes`, `@media`, ...) and
// comments are stripped; declarations the converter can't map to RCSS are
// dropped rather than passed through.
[[nodiscard]] std::string convert_panorama_css(std::string_view css);
}
