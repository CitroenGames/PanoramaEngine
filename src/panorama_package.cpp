#include "ui/panorama/panorama_package.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

namespace panorama
{
namespace
{
constexpr std::uint32_t kZipLocalFileHeader = 0x04034B50U;
constexpr std::uint16_t kZipStoredMethod = 0;

std::uint16_t read_u16_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    if (offset + 2U > bytes.size())
    {
        throw std::runtime_error("unexpected end of panorama package");
    }

    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t read_u32_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    if (offset + 4U > bytes.size())
    {
        throw std::runtime_error("unexpected end of panorama package");
    }

    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::size_t find_first_local_file_header(const std::vector<unsigned char>& bytes)
{
    for (std::size_t offset = 0; offset + 4U <= bytes.size(); ++offset)
    {
        if (read_u32_le(bytes, offset) == kZipLocalFileHeader)
        {
            return offset;
        }
    }

    return std::string::npos;
}
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

bool PanoramaPackage::open(const std::filesystem::path& path, std::string* error_message)
{
    try
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("failed to open panorama package '" + path.string() + "'");
        }
        const std::vector<unsigned char> bytes{
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        return open_bytes(bytes, path, error_message);
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

bool PanoramaPackage::open_bytes(std::span<const unsigned char> bytes,
    std::filesystem::path source_path, std::string* error_message)
{
    clear();
    path_ = source_path;
    try
    {
        bytes_.assign(bytes.begin(), bytes.end());
        const std::size_t first_header = find_first_local_file_header(bytes_);
        if (first_header == std::string::npos)
        {
            throw std::runtime_error("panorama package contains no zip local file headers");
        }

        std::size_t cursor = first_header;
        while (cursor + 30U <= bytes_.size() && read_u32_le(bytes_, cursor) == kZipLocalFileHeader)
        {
            const std::uint16_t general_flags = read_u16_le(bytes_, cursor + 6U);
            const std::uint16_t compression_method = read_u16_le(bytes_, cursor + 8U);
            const std::uint32_t compressed_size = read_u32_le(bytes_, cursor + 18U);
            const std::uint32_t uncompressed_size = read_u32_le(bytes_, cursor + 22U);
            const std::uint16_t name_length = read_u16_le(bytes_, cursor + 26U);
            const std::uint16_t extra_length = read_u16_le(bytes_, cursor + 28U);

            if ((general_flags & 0x08U) != 0)
            {
                throw std::runtime_error("panorama package uses unsupported zip data descriptors");
            }

            const std::size_t name_offset = cursor + 30U;
            const std::size_t data_offset = name_offset + name_length + extra_length;
            const std::size_t next_cursor = data_offset + compressed_size;
            if (data_offset > bytes_.size() || next_cursor > bytes_.size())
            {
                throw std::runtime_error("panorama package zip entry is truncated");
            }

            std::string entry_name(
                reinterpret_cast<const char*>(bytes_.data() + name_offset),
                reinterpret_cast<const char*>(bytes_.data() + name_offset + name_length));
            std::replace(entry_name.begin(), entry_name.end(), '\\', '/');
            const bool directory_entry = !entry_name.empty() && entry_name.back() == '/';
            if (!entry_name.empty() && !directory_entry)
            {
                const std::string key = normalize_panorama_entry_path(entry_name);
                entries_[key] = Entry{
                    .name = entry_name,
                    .data_offset = data_offset,
                    .compressed_size = compressed_size,
                    .uncompressed_size = uncompressed_size,
                    .compression_method = compression_method,
                };
            }

            cursor = next_cursor;
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
    std::filesystem::path source_path, std::string* error_message)
{
    clear();
    path_ = source_path;
    try
    {
        for (const auto& [resource_path, resource_bytes] : resources)
        {
            const std::string key = normalize_panorama_entry_path(resource_path);
            if (key.empty() || entries_.contains(key))
            {
                throw std::runtime_error("Panorama resources contain an empty or duplicate path: " + key);
            }
            const std::size_t offset = bytes_.size();
            bytes_.insert(bytes_.end(), resource_bytes.begin(), resource_bytes.end());
            entries_.emplace(key, Entry{
                .name = key,
                .data_offset = offset,
                .compressed_size = resource_bytes.size(),
                .uncompressed_size = resource_bytes.size(),
                .compression_method = kZipStoredMethod,
            });
        }
        if (entries_.empty())
        {
            throw std::runtime_error("Panorama resources contain no entries");
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
    bytes_.clear();
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
    const std::string key = normalize_panorama_entry_path(entry_path);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        throw std::runtime_error("panorama package entry not found: " + std::string(entry_path));
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

    return std::vector<unsigned char>(
        bytes_.begin() + static_cast<std::ptrdiff_t>(entry.data_offset),
        bytes_.begin() + static_cast<std::ptrdiff_t>(entry.data_offset + entry.uncompressed_size));
}

std::string PanoramaPackage::read_text(std::string_view entry_path) const
{
    const std::vector<unsigned char> data = read(entry_path);
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}
}
