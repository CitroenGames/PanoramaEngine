#pragma once

#include "ui/panorama/panorama_package.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace panorama
{
enum class PanoramaSourceKind
{
    None,
    JavaScript,
    Xml,
    Css,
};

// The source extensions Panorama executes or parses as text. This classifier is
// deliberately part of PanoramaEngine's public API so hosts do not grow their own
// subtly different packaging allowlists.
[[nodiscard]] inline PanoramaSourceKind classify_panorama_source_path(
    const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (extension == ".js")
    {
        return PanoramaSourceKind::JavaScript;
    }
    if (extension == ".xml")
    {
        return PanoramaSourceKind::Xml;
    }
    if (extension == ".css")
    {
        return PanoramaSourceKind::Css;
    }
    return PanoramaSourceKind::None;
}

[[nodiscard]] inline bool is_panorama_source_path(const std::filesystem::path& path)
{
    return classify_panorama_source_path(path) != PanoramaSourceKind::None;
}

// Returns the content-relative directory ending in "panorama" which owns a
// JS/XML/CSS resource. A script elsewhere in a project is not Panorama content.
[[nodiscard]] inline std::optional<std::filesystem::path> panorama_source_root(
    const std::filesystem::path& logical_path)
{
    if (!is_panorama_source_path(logical_path))
    {
        return std::nullopt;
    }

    std::filesystem::path root;
    for (const std::filesystem::path& component : logical_path.parent_path())
    {
        root /= component;
        std::string name = component.string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (name == "panorama")
        {
            return root;
        }
    }
    return std::nullopt;
}

struct PanoramaSourceCookStats
{
    std::size_t base_resources = 0;
    std::size_t javascript_files = 0;
    std::size_t xml_files = 0;
    std::size_t css_files = 0;

    [[nodiscard]] std::size_t source_files() const noexcept
    {
        return javascript_files + xml_files + css_files;
    }
};

// Cooks the loose JS/XML/CSS tree rooted at a project's `panorama` directory into
// one deterministic in-memory package. When `base_package` is supplied, its resources
// are retained and loose sources override matching package entries, matching the normal
// Panorama directory-over-package authoring model. The result has no ZIP framing and is
// ready for a host-native container (for example Ferrite PUI0).
[[nodiscard]] bool cook_panorama_source_tree(
    const std::filesystem::path& source_root,
    PanoramaPackage& output,
    const PanoramaPackage* base_package = nullptr,
    PanoramaSourceCookStats* stats = nullptr,
    std::string* error_message = nullptr);
}
