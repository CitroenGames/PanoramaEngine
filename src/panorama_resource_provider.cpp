#include "ui/panorama/panorama_resource_provider.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace panorama
{
namespace
{
std::string normalized_resource_key(std::string_view path)
{
    return normalize_panorama_entry_path(path);
}

bool has_parent_reference(std::string_view path)
{
    std::string normalized(path);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    std::size_t cursor = 0;
    while (cursor <= normalized.size())
    {
        const std::size_t slash = normalized.find('/', cursor);
        const std::size_t count = slash == std::string::npos ? std::string::npos : slash - cursor;
        if (std::string_view(normalized).substr(cursor, count) == "..")
        {
            return true;
        }
        if (slash == std::string::npos)
        {
            break;
        }
        cursor = slash + 1U;
    }

    return false;
}

std::string strip_panorama_prefix(std::string path)
{
    if (path.rfind("panorama/", 0) == 0)
    {
        path.erase(0, std::string("panorama/").size());
    }
    return path;
}

std::vector<std::filesystem::path> candidate_relative_paths(
    std::string_view path, bool already_normalized)
{
    if (has_parent_reference(path))
    {
        return {};
    }

    const std::string normalized =
        already_normalized ? std::string(path) : normalized_resource_key(path);
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(normalized);

    const std::string without_panorama = strip_panorama_prefix(normalized);
    if (without_panorama != normalized)
    {
        candidates.emplace_back(without_panorama);
    }
    return candidates;
}

std::optional<std::filesystem::path> safe_join(const std::filesystem::path& root, const std::filesystem::path& relative)
{
    if (relative.empty() || relative.is_absolute())
    {
        return std::nullopt;
    }

    const std::filesystem::path normalized = relative.lexically_normal();
    for (const std::filesystem::path& part : normalized)
    {
        if (part == "..")
        {
            return std::nullopt;
        }
    }

    return root / normalized;
}

std::optional<std::filesystem::path> resolve_normalized_file(
    const std::filesystem::path& root, std::string_view normalized_path)
{
    if (root.empty())
    {
        return std::nullopt;
    }
    for (const std::filesystem::path& relative :
         candidate_relative_paths(normalized_path, true))
    {
        const std::optional<std::filesystem::path> candidate = safe_join(root, relative);
        if (!candidate)
        {
            continue;
        }
        std::error_code error;
        if (std::filesystem::is_regular_file(*candidate, error))
        {
            return candidate->lexically_normal();
        }
    }
    return std::nullopt;
}

std::vector<unsigned char> read_file_exact(
    const std::filesystem::path& path, PanoramaResourceReadStats* stats)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        return {};
    }
    const std::streamoff end = file.tellg();
    if (end < 0 ||
        static_cast<std::uintmax_t>(end) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
    {
        return {};
    }
    const std::size_t size = static_cast<std::size_t>(end);
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
    {
        return {};
    }
    std::vector<unsigned char> bytes(size);
    if (stats != nullptr)
    {
        ++stats->file_buffer_allocations;
    }
    file.seekg(0, std::ios::beg);
    if (size != 0)
    {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!file || static_cast<std::size_t>(file.gcount()) != size)
        {
            return {};
        }
    }
    return bytes;
}
}

std::optional<std::filesystem::path> PanoramaResourceProvider::resolve_file(std::string_view path) const
{
    (void)path;
    return std::nullopt;
}

bool PanoramaResourceProvider::read_view(
    std::string_view normalized_path,
    PanoramaResourceView& out,
    PanoramaResourceReadStats* stats) const
{
    PanoramaResource owned;
    if (!read(normalized_path, owned))
    {
        return false;
    }
    PanoramaResourceView view;
    view.data = PanoramaSharedBytes::from_owned(std::move(owned.bytes));
    view.source = std::move(owned.source);
    if (stats != nullptr)
    {
        ++stats->owned_fallback_hits;
    }
    out = std::move(view);
    return true;
}

void PanoramaResourceManager::add_provider(
    std::unique_ptr<PanoramaResourceProvider> provider,
    int priority,
    std::string identifier)
{
    if (provider == nullptr)
    {
        return;
    }

    providers_.push_back(ProviderEntry{
        .priority = priority,
        .sequence = next_sequence_++,
        .identifier = std::move(identifier),
        .provider = std::move(provider),
    });
    std::stable_sort(providers_.begin(), providers_.end(), [](const ProviderEntry& a, const ProviderEntry& b) {
        if (a.priority != b.priority)
        {
            return a.priority < b.priority;
        }
        return a.sequence < b.sequence;
    });
}

void PanoramaResourceManager::remove_providers(std::string_view identifier)
{
    providers_.erase(
        std::remove_if(
            providers_.begin(),
            providers_.end(),
            [identifier](const ProviderEntry& entry) {
                return std::string_view(entry.identifier) == identifier;
            }),
        providers_.end());
}

void PanoramaResourceManager::clear()
{
    providers_.clear();
    next_sequence_ = 0;
}

bool PanoramaResourceManager::read(std::string_view path, PanoramaResource& out) const
{
    return read(path, out, nullptr);
}

bool PanoramaResourceManager::read(
    std::string_view path,
    PanoramaResource& out,
    PanoramaResourceReadStats* stats) const
{
    PanoramaResourceView view;
    if (!read_view(path, view, stats))
    {
        return false;
    }
    PanoramaResource resource;
    const std::span<const unsigned char> bytes = view.bytes();
    resource.bytes.assign(bytes.begin(), bytes.end());
    resource.source = std::move(view.source);
    if (stats != nullptr)
    {
        ++stats->payload_copy_operations;
        stats->copied_payload_bytes += bytes.size();
    }
    out = std::move(resource);
    return true;
}

bool PanoramaResourceManager::read_view(
    std::string_view path,
    PanoramaResourceView& out,
    PanoramaResourceReadStats* stats) const
{
    const std::string normalized = normalized_resource_key(path);
    if (stats != nullptr)
    {
        ++stats->path_normalizations;
    }
    for (const ProviderEntry& entry : providers_)
    {
        if (entry.provider == nullptr)
        {
            continue;
        }
        if (stats != nullptr)
        {
            ++stats->provider_lookups;
        }
        PanoramaResourceView resource;
        if (entry.provider->read_view(normalized, resource, stats))
        {
            out = std::move(resource);
            return true;
        }
        if (stats != nullptr)
        {
            ++stats->provider_fallthroughs;
        }
    }
    return false;
}

std::optional<std::string> PanoramaResourceManager::read_text(std::string_view path) const
{
    return read_text(path, nullptr);
}

std::optional<std::string> PanoramaResourceManager::read_text(
    std::string_view path, PanoramaResourceReadStats* stats) const
{
    PanoramaResourceView resource;
    if (!read_view(path, resource, stats))
    {
        return std::nullopt;
    }
    const std::span<const unsigned char> bytes = resource.bytes();
    if (stats != nullptr)
    {
        ++stats->payload_copy_operations;
        stats->copied_payload_bytes += bytes.size();
    }
    if (bytes.empty())
    {
        return std::string{};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::optional<std::filesystem::path> PanoramaResourceManager::resolve_file(std::string_view path) const
{
    for (const ProviderEntry& entry : providers_)
    {
        if (entry.provider == nullptr)
        {
            continue;
        }
        if (std::optional<std::filesystem::path> resolved = entry.provider->resolve_file(path))
        {
            return resolved;
        }
    }
    return std::nullopt;
}

bool PanoramaResourceManager::empty() const noexcept
{
    return providers_.empty();
}

void PanoramaMemoryResourceProvider::add(std::string_view path, std::span<const unsigned char> bytes)
{
    const std::string key = normalized_resource_key(path);
    StoredResource resource;
    resource.data = PanoramaSharedBytes::copy_of(bytes);
    resource.source = "memory:" + key;
    resources_[key] = std::move(resource);
}

void PanoramaMemoryResourceProvider::add_text(std::string_view path, std::string_view text)
{
    add(path, std::span<const unsigned char>(
                  reinterpret_cast<const unsigned char*>(text.data()),
                  text.size()));
}

bool PanoramaMemoryResourceProvider::read(std::string_view path, PanoramaResource& out) const
{
    const std::string normalized = normalized_resource_key(path);
    PanoramaResourceView view;
    if (!read_view(normalized, view))
    {
        return false;
    }
    PanoramaResource resource;
    const std::span<const unsigned char> bytes = view.bytes();
    resource.bytes.assign(bytes.begin(), bytes.end());
    resource.source = std::move(view.source);
    out = std::move(resource);
    return true;
}

bool PanoramaMemoryResourceProvider::read_view(
    std::string_view normalized_path,
    PanoramaResourceView& out,
    PanoramaResourceReadStats* stats) const
{
    const auto it = resources_.find(normalized_path);
    if (it == resources_.end())
    {
        return false;
    }
    PanoramaResourceView view;
    view.data = it->second.data;
    view.source = it->second.source;
    if (stats != nullptr)
    {
        ++stats->zero_copy_hits;
    }
    out = std::move(view);
    return true;
}

PanoramaPackageResourceProvider::PanoramaPackageResourceProvider(const PanoramaPackage& package)
    : package_(&package)
{
}

bool PanoramaPackageResourceProvider::read(std::string_view path, PanoramaResource& out) const
{
    const std::string normalized = normalized_resource_key(path);
    PanoramaResourceView view;
    if (!read_view(normalized, view))
    {
        return false;
    }
    PanoramaResource resource;
    const std::span<const unsigned char> bytes = view.bytes();
    resource.bytes.assign(bytes.begin(), bytes.end());
    resource.source = std::move(view.source);
    out = std::move(resource);
    return true;
}

bool PanoramaPackageResourceProvider::read_view(
    std::string_view normalized_path,
    PanoramaResourceView& out,
    PanoramaResourceReadStats* stats) const
{
    if (package_ == nullptr)
    {
        return false;
    }
    try
    {
        PanoramaSharedBytes data;
        if (!package_->try_read_normalized(normalized_path, data))
        {
            return false;
        }
        PanoramaResourceView view;
        view.data = std::move(data);
        view.source = package_->path().empty()
                          ? ("package:" + std::string(normalized_path))
                          : (package_->path().generic_string() + "#" +
                             std::string(normalized_path));
        if (stats != nullptr)
        {
            ++stats->zero_copy_hits;
        }
        out = std::move(view);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

PanoramaDirectoryResourceProvider::PanoramaDirectoryResourceProvider(std::filesystem::path root)
    : root_(std::move(root))
{
}

bool PanoramaDirectoryResourceProvider::read(std::string_view path, PanoramaResource& out) const
{
    const std::string normalized = normalized_resource_key(path);
    PanoramaResourceView view;
    if (!read_view(normalized, view))
    {
        return false;
    }
    PanoramaResource resource;
    const std::span<const unsigned char> bytes = view.bytes();
    resource.bytes.assign(bytes.begin(), bytes.end());
    resource.source = std::move(view.source);
    out = std::move(resource);
    return true;
}

bool PanoramaDirectoryResourceProvider::read_view(
    std::string_view normalized_path,
    PanoramaResourceView& out,
    PanoramaResourceReadStats* stats) const
{
    const std::optional<std::filesystem::path> file_path =
        resolve_normalized_file(root_, normalized_path);
    if (!file_path)
    {
        return false;
    }
    std::vector<unsigned char> bytes = read_file_exact(*file_path, stats);
    std::error_code size_error;
    const std::uintmax_t expected_size = std::filesystem::file_size(*file_path, size_error);
    if (size_error || expected_size != bytes.size())
    {
        return false;
    }
    PanoramaResourceView view;
    view.data = PanoramaSharedBytes::from_owned(std::move(bytes));
    view.source = file_path->generic_string();
    if (stats != nullptr)
    {
        ++stats->owned_fallback_hits;
    }
    out = std::move(view);
    return true;
}

std::optional<std::filesystem::path> PanoramaDirectoryResourceProvider::resolve_file(std::string_view path) const
{
    if (root_.empty())
    {
        return std::nullopt;
    }

    for (const std::filesystem::path& relative : candidate_relative_paths(path, false))
    {
        const std::optional<std::filesystem::path> candidate = safe_join(root_, relative);
        if (!candidate)
        {
            continue;
        }

        std::error_code error;
        if (std::filesystem::is_regular_file(*candidate, error))
        {
            return candidate->lexically_normal();
        }
    }
    return std::nullopt;
}

const std::filesystem::path& PanoramaDirectoryResourceProvider::root() const noexcept
{
    return root_;
}
}
