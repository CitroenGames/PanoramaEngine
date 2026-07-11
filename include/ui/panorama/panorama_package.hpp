#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Reader for `.pbin` packages: Valve-style zip archives containing only
// stored (uncompressed) entries with no zip data descriptors — real CS:GO
// packages satisfy this, but an arbitrarily-produced zip (e.g. one written
// with DEFLATE compression) will fail `open()` or `read()`. Loads the whole
// archive into memory once on open() and serves entries from there; there is
// no streaming or lazy per-entry I/O. Feed an opened package into
// PanoramaPackageResourceProvider to use it with a PanoramaResourceManager.
namespace openstrike
{
class PanoramaPackage
{
public:
    // Opens and indexes `path`. Replaces any previously opened package (calls
    // clear() first). Returns false on I/O failure, a missing zip local-file
    // header, or an unsupported entry (e.g. compressed rather than stored);
    // when `error_message` is non-null it receives a human-readable reason.
    [[nodiscard]] bool open(const std::filesystem::path& path, std::string* error_message = nullptr);
    void clear();

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    // `entry_path` is matched via normalize_panorama_entry_path, so callers
    // don't need to pre-normalize slashes/case themselves.
    [[nodiscard]] bool contains(std::string_view entry_path) const;
    [[nodiscard]] std::vector<std::string> entries() const;
    // Throws std::runtime_error if `entry_path` isn't present, or if the
    // matched entry is stored with mismatched compressed/uncompressed sizes
    // (a malformed or non-stored entry) — check contains() first if a missing
    // entry is an expected case rather than a bug.
    [[nodiscard]] std::vector<unsigned char> read(std::string_view entry_path) const;
    // Same as read(), reinterpreted as text without any encoding conversion
    // (callers needing UTF-16 BOM handling, e.g. localization files, decode
    // separately — see PanoramaLocalization::load).
    [[nodiscard]] std::string read_text(std::string_view entry_path) const;

private:
    struct Entry
    {
        std::string name;
        std::size_t data_offset = 0;
        std::size_t compressed_size = 0;
        std::size_t uncompressed_size = 0;
        std::uint16_t compression_method = 0;
    };

    std::filesystem::path path_;
    std::vector<unsigned char> bytes_;
    std::unordered_map<std::string, Entry> entries_;
};

[[nodiscard]] std::string normalize_panorama_entry_path(std::string_view entry_path);
}
