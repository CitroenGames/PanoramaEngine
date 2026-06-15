#pragma once

#include "ui/panorama/panorama_package.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
struct PanoramaConversionOptions
{
    std::filesystem::path resource_root;
    std::size_t max_frame_depth = 8;
    bool include_default_style = true;
};

struct PanoramaConversionResult
{
    std::string rml;
    std::vector<std::string> scripts;
    std::vector<std::string> missing_resources;
};

[[nodiscard]] PanoramaConversionResult convert_panorama_document(
    const PanoramaPackage& package,
    std::string_view document_path,
    const PanoramaConversionOptions& options = {});

[[nodiscard]] PanoramaConversionResult convert_panorama_document(
    const PanoramaResourceManager& resources,
    std::string_view document_path,
    const PanoramaConversionOptions& options = {});

[[nodiscard]] std::string convert_panorama_css(std::string_view css);
}
