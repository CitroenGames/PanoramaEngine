#include "ui/panorama/panorama_source_cooker.hpp"

#include <fstream>
#include <limits>
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
PanoramaSharedBytes read_source_file(
    const std::filesystem::path& path, PanoramaSourceCookStats& stats)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("failed to read Panorama source '" + path.string() + "'");
    }
    const std::streamoff end = file.tellg();
    if (end < 0 ||
        static_cast<std::uintmax_t>(end) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
    {
        throw std::runtime_error(
            "failed to determine Panorama source size '" + path.string() + "'");
    }
    const std::size_t size = static_cast<std::size_t>(end);
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
    {
        throw std::runtime_error(
            "Panorama source is too large to read '" + path.string() + "'");
    }
    std::vector<unsigned char> bytes(size);
    ++stats.source_file_allocations;
    stats.source_payload_bytes += size;
    file.seekg(0, std::ios::beg);
    if (size != 0)
    {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!file || static_cast<std::size_t>(file.gcount()) != size)
        {
            throw std::runtime_error("failed to read Panorama source '" + path.string() + "'");
        }
    }
    return PanoramaSharedBytes::from_owned(std::move(bytes));
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
        std::map<std::string, PanoramaSharedBytes> resources;
        if (base_package != nullptr)
        {
            for (const std::string& entry : base_package->entries())
            {
                const std::string normalized = normalize_panorama_entry_path(entry);
                PanoramaSharedBytes data;
                if (!base_package->try_read_normalized(normalized, data))
                {
                    throw std::runtime_error(
                        "panorama package entry not found: " + entry);
                }
                resources[normalized] = std::move(data);
                ++cooked_stats.base_resources;
                ++cooked_stats.shared_base_payloads;
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

            resources[resource_path] = read_source_file(source_file, cooked_stats);
            count_source(cooked_stats, classify_panorama_source_path(source_file));
        }

        if (resources.empty())
        {
            throw std::runtime_error(
                "Panorama source tree contains no JS, XML, CSS, or base-package resources");
        }

        std::vector<PanoramaPackageResource> package_resources;
        package_resources.reserve(resources.size());
        for (auto& [path, data] : resources)
        {
            cooked_stats.final_payload_bytes += data.size();
            package_resources.push_back(
                PanoramaPackageResource{path, std::move(data)});
        }

        std::string package_error;
        PanoramaPackageStorageStats package_stats;
        if (!output.open_resources(
                std::move(package_resources), source_root, &package_error, &package_stats))
        {
            throw std::runtime_error(package_error);
        }
        cooked_stats.peak_live_payload_bytes = package_stats.peak_live_payload_bytes;
        cooked_stats.payload_copy_operations = package_stats.payload_copy_operations;
        cooked_stats.copied_payload_bytes = package_stats.copied_payload_bytes;
        cooked_stats.adopted_payloads = package_stats.adopted_payloads;
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
