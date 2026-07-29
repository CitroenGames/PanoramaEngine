#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Reader for `.pbin` packages: Valve-style zip archives containing only
// stored (uncompressed) entries with no zip data descriptors — real CS:GO
// packages satisfy this, but an arbitrarily-produced zip (e.g. one written
// with DEFLATE compression) will fail `open()` or `read()`. Loads the whole
// archive into memory once on open() and serves entries from there; there is
// no streaming or lazy per-entry I/O. Feed an opened package into
// PanoramaPackageResourceProvider to use it with a PanoramaResourceManager.
namespace panorama
{
struct PanoramaTransparentStringHash
{
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }
};

// Immutable byte range plus the shared storage that keeps it alive. A view
// remains valid after its package/provider is cleared, replaced, moved, or
// destroyed. `from_owned()` adopts a vector allocation without copying it.
class PanoramaSharedBytes
{
public:
    PanoramaSharedBytes() = default;

    [[nodiscard]] static PanoramaSharedBytes from_owned(std::vector<unsigned char>&& bytes);
    [[nodiscard]] static PanoramaSharedBytes copy_of(std::span<const unsigned char> bytes);

    [[nodiscard]] std::span<const unsigned char> bytes() const noexcept;
    [[nodiscard]] const unsigned char* data() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    friend class PanoramaPackage;

    PanoramaSharedBytes(
        std::shared_ptr<const std::vector<unsigned char>> storage,
        std::size_t offset,
        std::size_t size);

    std::shared_ptr<const std::vector<unsigned char>> storage_;
    std::size_t offset_ = 0;
    std::size_t size_ = 0;
};

struct PanoramaPackageResource
{
    std::string path;
    PanoramaSharedBytes data;
};

// Opt-in structural counters. They measure ownership operations rather than
// time, so deterministic CI can distinguish adoption from payload copying.
struct PanoramaPackageStorageStats
{
    std::size_t input_payload_bytes = 0;
    std::size_t resident_payload_bytes = 0;
    std::size_t peak_live_payload_bytes = 0;
    std::size_t payload_copy_operations = 0;
    std::size_t copied_payload_bytes = 0;
    std::size_t payload_move_operations = 0;
    std::size_t adopted_payloads = 0;
    std::size_t file_buffer_allocations = 0;
};

class PanoramaPackage
{
public:
    // Opens and indexes `path`. Replaces any previously opened package (calls
    // clear() first). Returns false on I/O failure, a missing zip local-file
    // header, or an unsupported entry (e.g. compressed rather than stored);
    // when `error_message` is non-null it receives a human-readable reason.
    [[nodiscard]] bool open(
        const std::filesystem::path& path, std::string* error_message = nullptr);
    [[nodiscard]] bool open(
        const std::filesystem::path& path,
        std::string* error_message,
        PanoramaPackageStorageStats* storage_stats);
    // Opens an archive already read by a host asset system (VPK, .fasset, network, etc.).
    // `source_path` is diagnostic identity only; no filesystem access is performed.
    [[nodiscard]] bool open_bytes(std::span<const unsigned char> bytes,
        std::filesystem::path source_path = {}, std::string* error_message = nullptr);
    [[nodiscard]] bool open_bytes(std::span<const unsigned char> bytes,
        std::filesystem::path source_path, std::string* error_message,
        PanoramaPackageStorageStats* storage_stats);
    // Move-taking form for hosts that already own the archive allocation.
    [[nodiscard]] bool open_bytes(std::vector<unsigned char>&& bytes,
        std::filesystem::path source_path = {}, std::string* error_message = nullptr,
        PanoramaPackageStorageStats* storage_stats = nullptr);
    // Mounts already-decoded resources, used by native host asset containers that
    // deliberately do not retain ZIP framing.
    [[nodiscard]] bool open_resources(
        const std::vector<std::pair<std::string, std::vector<unsigned char>>>& resources,
        std::filesystem::path source_path = {}, std::string* error_message = nullptr);
    [[nodiscard]] bool open_resources(
        const std::vector<std::pair<std::string, std::vector<unsigned char>>>& resources,
        std::filesystem::path source_path, std::string* error_message,
        PanoramaPackageStorageStats* storage_stats);
    [[nodiscard]] bool open_resources(
        std::vector<std::pair<std::string, std::vector<unsigned char>>>&& resources,
        std::filesystem::path source_path = {}, std::string* error_message = nullptr,
        PanoramaPackageStorageStats* storage_stats = nullptr);
    // Shared-view form used by the source cooker. Payload allocations are
    // transferred/shared directly; only path/index metadata is rebuilt.
    [[nodiscard]] bool open_resources(
        std::vector<PanoramaPackageResource>&& resources,
        std::filesystem::path source_path = {}, std::string* error_message = nullptr,
        PanoramaPackageStorageStats* storage_stats = nullptr);
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
    // One normalized lookup and no payload copy. `try_read_normalized` is for a
    // manager/provider that already normalized the path at its ingress.
    [[nodiscard]] bool try_read(
        std::string_view entry_path, PanoramaSharedBytes& out) const;
    [[nodiscard]] bool try_read_normalized(
        std::string_view normalized_entry_path, PanoramaSharedBytes& out) const;
    // Same as read(), reinterpreted as text without any encoding conversion
    // (callers needing UTF-16 BOM handling, e.g. localization files, decode
    // separately — see PanoramaLocalization::load).
    [[nodiscard]] std::string read_text(std::string_view entry_path) const;

private:
    struct Entry
    {
        std::string name;
        PanoramaSharedBytes data;
        std::size_t compressed_size = 0;
        std::size_t uncompressed_size = 0;
        std::uint16_t compression_method = 0;
    };

    std::filesystem::path path_;
    std::shared_ptr<const std::vector<unsigned char>> archive_storage_;
    std::unordered_map<
        std::string,
        Entry,
        PanoramaTransparentStringHash,
        std::equal_to<>> entries_;
};

[[nodiscard]] std::string normalize_panorama_entry_path(std::string_view entry_path);
}
