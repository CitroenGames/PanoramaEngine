#include "ui/panorama/panorama_package.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace panorama
{
namespace
{
constexpr std::uint32_t kZipLocalFileHeader = 0x04034B50U;
constexpr std::uint16_t kZipStoredMethod = 0;

std::uint16_t read_u16_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 2U)
    {
        throw std::runtime_error("unexpected end of panorama package");
    }

    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t read_u32_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4U)
    {
        throw std::runtime_error("unexpected end of panorama package");
    }

    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::size_t find_first_local_file_header(std::span<const unsigned char> bytes)
{
    for (std::size_t offset = 0;
         offset <= bytes.size() && bytes.size() - offset >= 4U;
         ++offset)
    {
        if (read_u32_le(bytes, offset) == kZipLocalFileHeader)
        {
            return offset;
        }
    }

    return std::string::npos;
}

std::vector<unsigned char> read_file_exact(
    const std::filesystem::path& path,
    PanoramaPackageStorageStats* storage_stats)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("failed to open panorama package '" + path.string() + "'");
    }

    const std::streamoff end = file.tellg();
    if (end < 0 ||
        static_cast<std::uintmax_t>(end) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
    {
        throw std::runtime_error("failed to determine panorama package size '" + path.string() + "'");
    }
    const std::size_t size = static_cast<std::size_t>(end);
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
    {
        throw std::runtime_error("panorama package is too large to read '" + path.string() + "'");
    }
    std::vector<unsigned char> bytes(size);
    if (storage_stats != nullptr)
    {
        storage_stats->file_buffer_allocations = 1;
    }
    file.seekg(0, std::ios::beg);
    if (size != 0)
    {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!file || static_cast<std::size_t>(file.gcount()) != size)
        {
            throw std::runtime_error("failed to read panorama package '" + path.string() + "'");
        }
    }
    return bytes;
}
}

PanoramaSharedBytes::PanoramaSharedBytes(
    std::shared_ptr<const std::vector<unsigned char>> storage,
    std::size_t offset,
    std::size_t size)
    : storage_(std::move(storage)), offset_(offset), size_(size)
{
}

PanoramaSharedBytes PanoramaSharedBytes::from_owned(std::vector<unsigned char>&& bytes)
{
    const std::size_t size = bytes.size();
    return PanoramaSharedBytes(
        std::make_shared<const std::vector<unsigned char>>(std::move(bytes)), 0, size);
}

PanoramaSharedBytes PanoramaSharedBytes::copy_of(std::span<const unsigned char> bytes)
{
    std::vector<unsigned char> owned(bytes.begin(), bytes.end());
    return from_owned(std::move(owned));
}

std::span<const unsigned char> PanoramaSharedBytes::bytes() const noexcept
{
    if (size_ == 0)
    {
        return {};
    }
    if (storage_ == nullptr || offset_ > storage_->size() || size_ > storage_->size() - offset_)
    {
        return {};
    }
    return {storage_->data() + offset_, size_};
}

const unsigned char* PanoramaSharedBytes::data() const noexcept
{
    return bytes().data();
}

std::size_t PanoramaSharedBytes::size() const noexcept
{
    return bytes().size();
}

bool PanoramaSharedBytes::empty() const noexcept
{
    return size() == 0;
}

std::string normalize_panorama_entry_path(std::string_view entry_path)
{
    constexpr std::string_view file_scheme = "file://";
    constexpr std::string_view resources_prefix = "{resources}/";

    std::string normalized(entry_path);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    if (normalized.rfind(file_scheme, 0) == 0)
    {
        normalized.erase(0, file_scheme.size());
    }

    if (normalized.rfind(resources_prefix, 0) == 0)
    {
        normalized.erase(0, resources_prefix.size());
    }

    while (!normalized.empty() && normalized.front() == '/')
    {
        normalized.erase(normalized.begin());
    }

    // Collapse interior "//" — shipped CS:GO content references e.g.
    // "{resources}/layout//matchmaking_status.xml" and real Panorama resolves it.
    std::size_t double_slash = 0;
    while ((double_slash = normalized.find("//", double_slash)) != std::string::npos)
    {
        normalized.erase(double_slash, 1);
    }

    if (!normalized.empty() && normalized.rfind("panorama/", 0) != 0)
    {
        normalized = "panorama/" + normalized;
    }

    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    return normalized;
}

bool PanoramaPackage::open(
    const std::filesystem::path& path, std::string* error_message)
{
    return open(path, error_message, nullptr);
}

bool PanoramaPackage::open(
    const std::filesystem::path& path,
    std::string* error_message,
    PanoramaPackageStorageStats* storage_stats)
{
    if (storage_stats != nullptr)
    {
        *storage_stats = {};
    }
    try
    {
        PanoramaPackageStorageStats file_stats;
        std::vector<unsigned char> bytes = read_file_exact(path, &file_stats);
        PanoramaPackageStorageStats open_stats;
        const bool opened = open_bytes(std::move(bytes), path, error_message, &open_stats);
        if (storage_stats != nullptr)
        {
            *storage_stats = open_stats;
            storage_stats->file_buffer_allocations = file_stats.file_buffer_allocations;
        }
        return opened;
    }
    catch (const std::exception& error)
    {
        if (error_message != nullptr)
        {
            *error_message = error.what();
        }
        clear();
        path_ = path;
        return false;
    }
}

bool PanoramaPackage::open_bytes(
    std::span<const unsigned char> bytes,
    std::filesystem::path source_path,
    std::string* error_message)
{
    return open_bytes(bytes, std::move(source_path), error_message, nullptr);
}

bool PanoramaPackage::open_bytes(std::span<const unsigned char> bytes,
    std::filesystem::path source_path, std::string* error_message,
    PanoramaPackageStorageStats* storage_stats)
{
    clear();
    path_ = source_path;
    if (storage_stats != nullptr)
    {
        *storage_stats = {};
    }
    try
    {
        const std::size_t copied_bytes = bytes.size();
        std::vector<unsigned char> owned(bytes.begin(), bytes.end());
        PanoramaPackageStorageStats moved_stats;
        const bool opened =
            open_bytes(std::move(owned), source_path, error_message, &moved_stats);
        if (storage_stats != nullptr)
        {
            *storage_stats = moved_stats;
            storage_stats->payload_copy_operations = 1;
            storage_stats->copied_payload_bytes = copied_bytes;
        }
        return opened;
    }
    catch (const std::exception& error)
    {
        if (error_message != nullptr)
        {
            *error_message = error.what();
        }
        clear();
        path_ = source_path;
        return false;
    }
}

bool PanoramaPackage::open_bytes(std::vector<unsigned char>&& bytes,
    std::filesystem::path source_path, std::string* error_message,
    PanoramaPackageStorageStats* storage_stats)
{
    clear();
    path_ = source_path;
    if (storage_stats != nullptr)
    {
        *storage_stats = {};
        storage_stats->input_payload_bytes = bytes.size();
        storage_stats->resident_payload_bytes = bytes.size();
        storage_stats->peak_live_payload_bytes = bytes.size();
        storage_stats->payload_move_operations = 1;
        storage_stats->adopted_payloads = 1;
    }
    try
    {
        const std::size_t archive_size = bytes.size();
        archive_storage_ =
            std::make_shared<const std::vector<unsigned char>>(std::move(bytes));
        const std::span<const unsigned char> archive(
            archive_storage_->data(), archive_storage_->size());
        const std::size_t first_header = find_first_local_file_header(archive);
        if (first_header == std::string::npos)
        {
            throw std::runtime_error("panorama package contains no zip local file headers");
        }

        std::size_t cursor = first_header;
        while (cursor <= archive.size() && archive.size() - cursor >= 30U &&
               read_u32_le(archive, cursor) == kZipLocalFileHeader)
        {
            const std::uint16_t general_flags = read_u16_le(archive, cursor + 6U);
            const std::uint16_t compression_method = read_u16_le(archive, cursor + 8U);
            const std::uint32_t compressed_size = read_u32_le(archive, cursor + 18U);
            const std::uint32_t uncompressed_size = read_u32_le(archive, cursor + 22U);
            const std::uint16_t name_length = read_u16_le(archive, cursor + 26U);
            const std::uint16_t extra_length = read_u16_le(archive, cursor + 28U);

            if ((general_flags & 0x08U) != 0)
            {
                throw std::runtime_error("panorama package uses unsupported zip data descriptors");
            }

            const std::size_t name_offset = cursor + 30U;
            if (name_offset > archive.size() ||
                name_length > archive.size() - name_offset ||
                extra_length > archive.size() - name_offset - name_length)
            {
                throw std::runtime_error("panorama package zip entry is truncated");
            }
            const std::size_t data_offset = name_offset + name_length + extra_length;
            if (compressed_size > archive.size() - data_offset)
            {
                throw std::runtime_error("panorama package zip entry is truncated");
            }
            const std::size_t next_cursor = data_offset + compressed_size;

            std::string entry_name(
                reinterpret_cast<const char*>(archive.data() + name_offset),
                reinterpret_cast<const char*>(archive.data() + name_offset + name_length));
            std::replace(entry_name.begin(), entry_name.end(), '\\', '/');
            const bool directory_entry = !entry_name.empty() && entry_name.back() == '/';
            if (!entry_name.empty() && !directory_entry)
            {
                const std::string key = normalize_panorama_entry_path(entry_name);
                entries_[key] = Entry{
                    .name = entry_name,
                    .data = PanoramaSharedBytes(
                        archive_storage_, data_offset, compressed_size),
                    .compressed_size = compressed_size,
                    .uncompressed_size = uncompressed_size,
                    .compression_method = compression_method,
                };
            }

            cursor = next_cursor;
        }

        if (storage_stats != nullptr)
        {
            storage_stats->resident_payload_bytes = archive_size;
            storage_stats->peak_live_payload_bytes = archive_size;
        }
        return true;
    }
    catch (const std::exception& error)
    {
        if (error_message != nullptr)
        {
            *error_message = error.what();
        }
        clear();
        path_ = source_path;
        return false;
    }
}

bool PanoramaPackage::open_resources(
    const std::vector<std::pair<std::string, std::vector<unsigned char>>>& resources,
    std::filesystem::path source_path,
    std::string* error_message)
{
    return open_resources(resources, std::move(source_path), error_message, nullptr);
}

bool PanoramaPackage::open_resources(
    const std::vector<std::pair<std::string, std::vector<unsigned char>>>& resources,
    std::filesystem::path source_path, std::string* error_message,
    PanoramaPackageStorageStats* storage_stats)
{
    clear();
    path_ = source_path;
    if (storage_stats != nullptr)
    {
        *storage_stats = {};
    }
    try
    {
        std::vector<PanoramaPackageResource> shared_resources;
        shared_resources.reserve(resources.size());
        std::size_t copied_bytes = 0;
        for (const auto& [resource_path, resource_bytes] : resources)
        {
            copied_bytes += resource_bytes.size();
            shared_resources.push_back(PanoramaPackageResource{
                resource_path, PanoramaSharedBytes::copy_of(resource_bytes)});
        }
        PanoramaPackageStorageStats adopted_stats;
        const bool opened = open_resources(
            std::move(shared_resources), source_path, error_message, &adopted_stats);
        if (storage_stats != nullptr)
        {
            *storage_stats = adopted_stats;
            storage_stats->payload_copy_operations = resources.size();
            storage_stats->copied_payload_bytes = copied_bytes;
        }
        return opened;
    }
    catch (const std::exception& error)
    {
        if (error_message != nullptr)
        {
            *error_message = error.what();
        }
        clear();
        path_ = source_path;
        return false;
    }
}

bool PanoramaPackage::open_resources(
    std::vector<std::pair<std::string, std::vector<unsigned char>>>&& resources,
    std::filesystem::path source_path, std::string* error_message,
    PanoramaPackageStorageStats* storage_stats)
{
    clear();
    path_ = source_path;
    if (storage_stats != nullptr)
    {
        *storage_stats = {};
    }
    try
    {
        std::vector<PanoramaPackageResource> shared_resources;
        shared_resources.reserve(resources.size());
        for (auto& [resource_path, resource_bytes] : resources)
        {
            shared_resources.push_back(PanoramaPackageResource{
                std::move(resource_path),
                PanoramaSharedBytes::from_owned(std::move(resource_bytes))});
        }
        return open_resources(
            std::move(shared_resources), source_path, error_message, storage_stats);
    }
    catch (const std::exception& error)
    {
        if (error_message != nullptr)
        {
            *error_message = error.what();
        }
        clear();
        path_ = source_path;
        return false;
    }
}

bool PanoramaPackage::open_resources(
    std::vector<PanoramaPackageResource>&& resources,
    std::filesystem::path source_path, std::string* error_message,
    PanoramaPackageStorageStats* storage_stats)
{
    clear();
    path_ = source_path;
    if (storage_stats != nullptr)
    {
        *storage_stats = {};
    }
    try
    {
        for (PanoramaPackageResource& resource : resources)
        {
            const std::string key = normalize_panorama_entry_path(resource.path);
            if (key.empty() || entries_.contains(key))
            {
                throw std::runtime_error("Panorama resources contain an empty or duplicate path: " + key);
            }
            const std::size_t size = resource.data.size();
            entries_.emplace(key, Entry{
                .name = key,
                .data = std::move(resource.data),
                .compressed_size = size,
                .uncompressed_size = size,
                .compression_method = kZipStoredMethod,
            });
            if (storage_stats != nullptr)
            {
                storage_stats->input_payload_bytes += size;
                storage_stats->resident_payload_bytes += size;
                ++storage_stats->payload_move_operations;
                ++storage_stats->adopted_payloads;
            }
        }
        if (entries_.empty())
        {
            throw std::runtime_error("Panorama resources contain no entries");
        }
        if (storage_stats != nullptr)
        {
            storage_stats->peak_live_payload_bytes =
                storage_stats->resident_payload_bytes;
        }
        return true;
    }
    catch (const std::exception& error)
    {
        if (error_message != nullptr)
        {
            *error_message = error.what();
        }
        clear();
        path_ = source_path;
        return false;
    }
}

void PanoramaPackage::clear()
{
    path_.clear();
    archive_storage_.reset();
    entries_.clear();
}

const std::filesystem::path& PanoramaPackage::path() const noexcept
{
    return path_;
}

bool PanoramaPackage::empty() const noexcept
{
    return entries_.empty();
}

bool PanoramaPackage::contains(std::string_view entry_path) const
{
    return entries_.find(normalize_panorama_entry_path(entry_path)) != entries_.end();
}

std::vector<std::string> PanoramaPackage::entries() const
{
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto& [_, entry] : entries_)
    {
        names.push_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<unsigned char> PanoramaPackage::read(std::string_view entry_path) const
{
    PanoramaSharedBytes view;
    if (!try_read(entry_path, view))
    {
        throw std::runtime_error("panorama package entry not found: " + std::string(entry_path));
    }
    const std::span<const unsigned char> bytes = view.bytes();
    return std::vector<unsigned char>(bytes.begin(), bytes.end());
}

bool PanoramaPackage::try_read(
    std::string_view entry_path, PanoramaSharedBytes& out) const
{
    const std::string key = normalize_panorama_entry_path(entry_path);
    return try_read_normalized(key, out);
}

bool PanoramaPackage::try_read_normalized(
    std::string_view normalized_entry_path, PanoramaSharedBytes& out) const
{
    const auto it = entries_.find(normalized_entry_path);
    if (it == entries_.end())
    {
        return false;
    }

    const Entry& entry = it->second;
    if (entry.compression_method != kZipStoredMethod)
    {
        throw std::runtime_error("panorama package entry uses unsupported compression: " + entry.name);
    }

    if (entry.compressed_size != entry.uncompressed_size)
    {
        throw std::runtime_error("panorama package stored entry has mismatched sizes: " + entry.name);
    }
    if (entry.data.size() != entry.uncompressed_size)
    {
        throw std::runtime_error("panorama package zip entry is truncated");
    }
    out = entry.data;
    return true;
}

std::string PanoramaPackage::read_text(std::string_view entry_path) const
{
    PanoramaSharedBytes view;
    if (!try_read(entry_path, view))
    {
        throw std::runtime_error("panorama package entry not found: " + std::string(entry_path));
    }
    const std::span<const unsigned char> bytes = view.bytes();
    if (bytes.empty())
    {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
}
