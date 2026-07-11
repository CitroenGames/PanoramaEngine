#include "ui/panorama/panorama_resource_provider.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
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

std::vector<std::filesystem::path> candidate_relative_paths(std::string_view path)
{
    if (has_parent_reference(path))
    {
        return {};
    }

    const std::string normalized = normalized_resource_key(path);
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
}

std::optional<std::filesystem::path> PanoramaResourceProvider::resolve_file(std::string_view path) const
{
    (void)path;
    return std::nullopt;
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
    for (const ProviderEntry& entry : providers_)
    {
        PanoramaResource resource;
        if (entry.provider != nullptr && entry.provider->read(path, resource))
        {
            out = std::move(resource);
            return true;
        }
    }
    return false;
}

std::optional<std::string> PanoramaResourceManager::read_text(std::string_view path) const
{
    PanoramaResource resource;
    if (!read(path, resource))
    {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(resource.bytes.data()), resource.bytes.size());
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
    PanoramaResource resource;
    resource.bytes.assign(bytes.begin(), bytes.end());
    resource.source = "memory:" + normalized_resource_key(path);
    resources_[normalized_resource_key(path)] = std::move(resource);
}

void PanoramaMemoryResourceProvider::add_text(std::string_view path, std::string_view text)
{
    add(path, std::span<const unsigned char>(
                  reinterpret_cast<const unsigned char*>(text.data()),
                  text.size()));
}

bool PanoramaMemoryResourceProvider::read(std::string_view path, PanoramaResource& out) const
{
    const auto it = resources_.find(normalized_resource_key(path));
    if (it == resources_.end())
    {
        return false;
    }
    out = it->second;
    return true;
}

PanoramaPackageResourceProvider::PanoramaPackageResourceProvider(const PanoramaPackage& package)
    : package_(&package)
{
}

bool PanoramaPackageResourceProvider::read(std::string_view path, PanoramaResource& out) const
{
    if (package_ == nullptr || !package_->contains(path))
    {
        return false;
    }

    try
    {
        out.bytes = package_->read(path);
        out.source = package_->path().empty()
                         ? ("package:" + normalized_resource_key(path))
                         : (package_->path().generic_string() + "#" + normalized_resource_key(path));
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
    const std::optional<std::filesystem::path> file_path = resolve_file(path);
    if (!file_path)
    {
        return false;
    }

    std::ifstream file(*file_path, std::ios::binary);
    if (!file)
    {
        return false;
    }

    out.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    out.source = file_path->generic_string();
    return true;
}

std::optional<std::filesystem::path> PanoramaDirectoryResourceProvider::resolve_file(std::string_view path) const
{
    if (root_.empty())
    {
        return std::nullopt;
    }

    for (const std::filesystem::path& relative : candidate_relative_paths(path))
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
