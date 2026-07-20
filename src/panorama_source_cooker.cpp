#include "ui/panorama/panorama_source_cooker.hpp"

#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace panorama
{
namespace
{
std::vector<unsigned char> read_source_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("failed to read Panorama source '" + path.string() + "'");
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void count_source(PanoramaSourceCookStats& stats, PanoramaSourceKind kind)
{
    switch (kind)
    {
    case PanoramaSourceKind::JavaScript:
        ++stats.javascript_files;
        break;
    case PanoramaSourceKind::Xml:
        ++stats.xml_files;
        break;
    case PanoramaSourceKind::Css:
        ++stats.css_files;
        break;
    case PanoramaSourceKind::None:
        break;
    }
}
}

bool cook_panorama_source_tree(
    const std::filesystem::path& source_root,
    PanoramaPackage& output,
    const PanoramaPackage* base_package,
    PanoramaSourceCookStats* stats,
    std::string* error_message)
{
    output.clear();
    PanoramaSourceCookStats cooked_stats;
    if (stats != nullptr)
    {
        *stats = {};
    }
    if (error_message != nullptr)
    {
        error_message->clear();
    }

    try
    {
        std::error_code filesystem_error;
        if (!std::filesystem::is_directory(source_root, filesystem_error))
        {
            throw std::runtime_error(
                "Panorama source root is not a directory: " + source_root.string());
        }

        // std::map fixes serialized entry order regardless of filesystem iteration order.
        std::map<std::string, std::vector<unsigned char>> resources;
        if (base_package != nullptr)
        {
            for (const std::string& entry : base_package->entries())
            {
                resources[normalize_panorama_entry_path(entry)] = base_package->read(entry);
                ++cooked_stats.base_resources;
            }
        }

        std::vector<std::filesystem::path> source_files;
        for (std::filesystem::recursive_directory_iterator it(
                 source_root,
                 std::filesystem::directory_options::skip_permission_denied,
                 filesystem_error);
             !filesystem_error && it != std::filesystem::recursive_directory_iterator();
             it.increment(filesystem_error))
        {
            std::error_code file_error;
            if (it->is_regular_file(file_error) && is_panorama_source_path(it->path()))
            {
                source_files.push_back(it->path());
            }
        }
        if (filesystem_error)
        {
            throw std::runtime_error(
                "failed to enumerate Panorama source root '" + source_root.string() +
                "': " + filesystem_error.message());
        }
        std::sort(source_files.begin(), source_files.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.generic_string() < rhs.generic_string();
        });

        std::unordered_set<std::string> source_resource_paths;
        for (const std::filesystem::path& source_file : source_files)
        {
            const std::filesystem::path relative =
                std::filesystem::relative(source_file, source_root, filesystem_error);
            if (filesystem_error || relative.empty() || relative.is_absolute())
            {
                throw std::runtime_error(
                    "failed to make Panorama source path relative: " + source_file.string());
            }

            const std::string resource_path =
                normalize_panorama_entry_path(relative.generic_string());
            if (resource_path.empty() || !source_resource_paths.insert(resource_path).second)
            {
                throw std::runtime_error(
                    "duplicate normalized Panorama source path: " + resource_path);
            }

            resources[resource_path] = read_source_file(source_file);
            count_source(cooked_stats, classify_panorama_source_path(source_file));
        }

        if (resources.empty())
        {
            throw std::runtime_error(
                "Panorama source tree contains no JS, XML, CSS, or base-package resources");
        }

        std::vector<std::pair<std::string, std::vector<unsigned char>>> package_resources;
        package_resources.reserve(resources.size());
        for (auto& [path, bytes] : resources)
        {
            package_resources.emplace_back(path, std::move(bytes));
        }

        std::string package_error;
        if (!output.open_resources(package_resources, source_root, &package_error))
        {
            throw std::runtime_error(package_error);
        }
        if (stats != nullptr)
        {
            *stats = cooked_stats;
        }
        return true;
    }
    catch (const std::exception& error)
    {
        output.clear();
        if (error_message != nullptr)
        {
            *error_message = error.what();
        }
        return false;
    }
}
}
