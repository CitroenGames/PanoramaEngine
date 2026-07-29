#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_source_cooker.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
void expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

std::vector<unsigned char> make_bytes(std::size_t size, unsigned char seed)
{
    std::vector<unsigned char> bytes(size);
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<unsigned char>(
            seed + static_cast<unsigned char>((index * 17U) & 0xFFU));
    }
    return bytes;
}

void expect_bytes(
    std::span<const unsigned char> actual,
    std::span<const unsigned char> expected,
    std::string_view message)
{
    expect(actual.size() == expected.size(), message);
    expect(std::equal(actual.begin(), actual.end(), expected.begin()), message);
}

void append_u16(std::vector<unsigned char>& out, std::uint16_t value)
{
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<unsigned char>& out, std::uint32_t value)
{
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
}

void append_stored_zip_entry(
    std::vector<unsigned char>& archive,
    std::string_view name,
    std::span<const unsigned char> bytes)
{
    append_u32(archive, 0x04034B50U);
    append_u16(archive, 20);
    append_u16(archive, 0);
    append_u16(archive, 0);
    append_u16(archive, 0);
    append_u16(archive, 0);
    append_u32(archive, 0);
    append_u32(archive, static_cast<std::uint32_t>(bytes.size()));
    append_u32(archive, static_cast<std::uint32_t>(bytes.size()));
    append_u16(archive, static_cast<std::uint16_t>(name.size()));
    append_u16(archive, 0);
    archive.insert(archive.end(), name.begin(), name.end());
    archive.insert(archive.end(), bytes.begin(), bytes.end());
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path() /
                ("panorama-resource-ownership-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void write_file(
    const std::filesystem::path& path, std::span<const unsigned char> bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("failed to create ownership test fixture");
    }
    file.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!file)
    {
        throw std::runtime_error("failed to write ownership test fixture");
    }
}

void test_memory_views_survive_mutation_and_provider_destruction()
{
    const std::vector<unsigned char> original = make_bytes(1024, 7);
    const std::vector<unsigned char> replacement = make_bytes(1024 * 1024, 29);

    panorama::PanoramaResourceManager manager;
    auto provider = std::make_unique<panorama::PanoramaMemoryResourceProvider>();
    panorama::PanoramaMemoryResourceProvider* provider_ptr = provider.get();
    provider_ptr->add("layout/main.xml", original);
    manager.add_provider(std::move(provider));

    panorama::PanoramaResourceReadStats stats;
    panorama::PanoramaResourceView first;
    expect(
        manager.read_view("{resources}\\LAYOUT//main.xml", first, &stats),
        "memory resource view lookup failed");
    expect(stats.path_normalizations == 1, "manager must normalize once");
    expect(stats.provider_lookups == 1, "memory hit must use one provider lookup");
    expect(stats.zero_copy_hits == 1, "memory hit must be zero-copy");
    expect(stats.payload_copy_operations == 0, "view read copied its payload");
    expect_bytes(first.bytes(), original, "memory view bytes changed");
    const unsigned char* first_pointer = first.bytes().data();

    provider_ptr->add("layout/main.xml", replacement);
    panorama::PanoramaResourceView second;
    expect(manager.read_view("layout/main.xml", second), "replacement view lookup failed");
    expect_bytes(second.bytes(), replacement, "replacement bytes changed");
    expect(first.bytes().data() == first_pointer, "mutation invalidated the prior view");
    expect_bytes(first.bytes(), original, "mutation changed prior immutable bytes");

    manager.clear();
    expect(first.bytes().data() == first_pointer, "provider destruction invalidated the view");
    expect_bytes(first.bytes(), original, "provider destruction changed view bytes");
}

void test_layered_precedence_and_one_normalization()
{
    panorama::PanoramaResourceManager manager;
    auto override_provider = std::make_unique<panorama::PanoramaMemoryResourceProvider>();
    auto base_provider = std::make_unique<panorama::PanoramaMemoryResourceProvider>();
    const std::vector<unsigned char> override_bytes = make_bytes(1024, 3);
    const std::vector<unsigned char> base_bytes = make_bytes(1024, 91);
    override_provider->add("panorama/data/shared.bin", override_bytes);
    base_provider->add("panorama/data/shared.bin", base_bytes);
    base_provider->add("panorama/data/base-only.bin", base_bytes);
    manager.add_provider(std::move(base_provider), 0, "base");
    manager.add_provider(std::move(override_provider), -1, "override");

    panorama::PanoramaResourceReadStats hit_stats;
    panorama::PanoramaResourceView hit;
    expect(
        manager.read_view("file://data//SHARED.bin", hit, &hit_stats),
        "layered override lookup failed");
    expect_bytes(hit.bytes(), override_bytes, "lower-priority number did not win");
    expect(hit_stats.path_normalizations == 1, "layered hit normalized more than once");
    expect(hit_stats.provider_lookups == 1, "override hit fell through unnecessarily");

    panorama::PanoramaResourceReadStats fallthrough_stats;
    panorama::PanoramaResourceView fallthrough;
    expect(
        manager.read_view("{resources}/data/base-only.bin", fallthrough, &fallthrough_stats),
        "base fallthrough lookup failed");
    expect_bytes(fallthrough.bytes(), base_bytes, "base fallthrough bytes changed");
    expect(fallthrough_stats.path_normalizations == 1, "fallthrough normalized more than once");
    expect(fallthrough_stats.provider_lookups == 2, "fallthrough lookup count changed");
    expect(fallthrough_stats.provider_fallthroughs == 1, "fallthrough count changed");
}

void test_package_move_adopts_archive_and_views_outlive_package()
{
    const std::vector<unsigned char> payload = make_bytes(1024 * 1024, 41);
    std::vector<unsigned char> archive;
    archive.reserve(payload.size() + 128);
    append_stored_zip_entry(archive, "panorama/data/large.bin", payload);
    const unsigned char* archive_pointer = archive.data();
    const std::size_t archive_size = archive.size();

    panorama::PanoramaSharedBytes retained;
    {
        panorama::PanoramaPackage package;
        panorama::PanoramaPackageStorageStats stats;
        std::string error;
        expect(
            package.open_bytes(std::move(archive), "owned.pbin", &error, &stats),
            error);
        expect(stats.payload_copy_operations == 0, "move-open copied archive bytes");
        expect(stats.copied_payload_bytes == 0, "move-open reported copied bytes");
        expect(stats.payload_move_operations == 1, "move-open did not record adoption");
        expect(stats.file_buffer_allocations == 0, "memory open allocated a file buffer");
        expect(
            package.try_read("DATA\\LARGE.bin", retained),
            "move-open package lookup failed");
        expect_bytes(retained.bytes(), payload, "move-open package bytes changed");
        const std::uintptr_t archive_begin =
            reinterpret_cast<std::uintptr_t>(archive_pointer);
        const std::uintptr_t entry_begin =
            reinterpret_cast<std::uintptr_t>(retained.bytes().data());
        expect(
            entry_begin >= archive_begin &&
                entry_begin < archive_begin + archive_size,
            "entry view does not point inside the adopted archive");

        panorama::PanoramaPackage moved = std::move(package);
        const std::vector<unsigned char> moved_bytes =
            moved.read("panorama/data/large.bin");
        expect_bytes(
            moved_bytes,
            payload,
            "moved package read changed bytes");
        moved.clear();
        expect_bytes(retained.bytes(), payload, "package clear invalidated retained view");
    }
    expect_bytes(retained.bytes(), payload, "package destruction invalidated retained view");
}

void test_package_provider_zero_copy_for_1k_and_1m_reads()
{
    std::vector<std::pair<std::string, std::vector<unsigned char>>> resources;
    const std::vector<unsigned char> small = make_bytes(1024, 13);
    const std::vector<unsigned char> large = make_bytes(1024 * 1024, 53);
    resources.emplace_back("panorama/data/small.bin", small);
    resources.emplace_back("panorama/data/large.bin", large);

    panorama::PanoramaPackage package;
    panorama::PanoramaPackageStorageStats open_stats;
    std::string error;
    expect(
        package.open_resources(std::move(resources), "resources.pbin", &error, &open_stats),
        error);
    expect(open_stats.payload_copy_operations == 0, "rvalue resources were copied");
    expect(open_stats.adopted_payloads == 2, "resource payload adoption count changed");

    panorama::PanoramaResourceManager manager;
    manager.add_provider(
        std::make_unique<panorama::PanoramaPackageResourceProvider>(package));

    panorama::PanoramaResourceReadStats small_stats;
    panorama::PanoramaResourceView small_view;
    expect(manager.read_view("data/small.bin", small_view, &small_stats), "1KiB view failed");
    expect_bytes(small_view.bytes(), small, "1KiB view identity failed");
    expect(small_stats.path_normalizations == 1, "1KiB read normalized more than once");
    expect(small_stats.zero_copy_hits == 1, "1KiB read was not zero-copy");
    expect(small_stats.payload_copy_operations == 0, "1KiB view copied bytes");

    panorama::PanoramaResourceReadStats large_stats;
    panorama::PanoramaResourceView large_view;
    expect(manager.read_view("data/large.bin", large_view, &large_stats), "1MiB view failed");
    expect_bytes(large_view.bytes(), large, "1MiB view identity failed");
    expect(large_stats.path_normalizations == 1, "1MiB read normalized more than once");
    expect(large_stats.zero_copy_hits == 1, "1MiB read was not zero-copy");
    expect(large_stats.payload_copy_operations == 0, "1MiB view copied bytes");

    panorama::PanoramaResourceReadStats legacy_stats;
    panorama::PanoramaResource legacy;
    expect(manager.read("data/large.bin", legacy, &legacy_stats), "legacy adapter read failed");
    expect_bytes(legacy.bytes, large, "legacy adapter bytes changed");
    expect(legacy_stats.payload_copy_operations == 1, "legacy adapter must copy exactly once");
    expect(legacy_stats.copied_payload_bytes == large.size(), "legacy copied-byte count changed");

    package.clear();
    expect_bytes(large_view.bytes(), large, "package mutation invalidated provider view");
}

void test_file_open_uses_one_known_size_buffer()
{
    TemporaryDirectory temporary;
    const std::vector<unsigned char> payload = make_bytes(1024, 101);
    std::vector<unsigned char> archive;
    append_stored_zip_entry(archive, "panorama/data/file-open.bin", payload);
    const std::filesystem::path package_path = temporary.path() / "fixture.pbin";
    write_file(package_path, archive);

    panorama::PanoramaPackage package;
    panorama::PanoramaPackageStorageStats stats;
    std::string error;
    expect(package.open(package_path, &error, &stats), error);
    expect(stats.file_buffer_allocations == 1, "file open did not use one exact-size buffer");
    expect(stats.payload_copy_operations == 0, "file open copied its archive buffer");
    expect(stats.copied_payload_bytes == 0, "file open reported copied payload bytes");
    expect(stats.payload_move_operations == 1, "file open did not adopt its read buffer");
    expect(stats.peak_live_payload_bytes == archive.size(), "file open peak-byte count changed");
    const std::vector<unsigned char> read = package.read("data/file-open.bin");
    expect_bytes(read, payload, "file-open package bytes changed");
}

void test_source_cooker_shares_base_and_adopts_loose_payloads()
{
    TemporaryDirectory temporary;
    const std::vector<unsigned char> base_large = make_bytes(1024 * 1024, 67);
    const std::vector<unsigned char> base_overridden{'b', 'a', 's', 'e'};
    std::vector<std::pair<std::string, std::vector<unsigned char>>> base_resources;
    base_resources.emplace_back("panorama/data/base-large.bin", base_large);
    base_resources.emplace_back("panorama/layout/main.xml", base_overridden);

    panorama::PanoramaPackage base;
    std::string error;
    expect(
        base.open_resources(std::move(base_resources), "base.pbin", &error),
        error);

    const std::vector<unsigned char> cooked_xml{
        '<', 'r', 'o', 'o', 't', ' ', 'i', 'd', '=', '"', 'c', 'o', 'o', 'k', 'e', 'd', '"', '/',
        '>'};
    const std::vector<unsigned char> cooked_js = make_bytes(1024, 79);
    write_file(temporary.path() / "layout" / "main.xml", cooked_xml);
    write_file(temporary.path() / "scripts" / "main.js", cooked_js);

    panorama::PanoramaPackage cooked;
    panorama::PanoramaSourceCookStats stats;
    expect(
        panorama::cook_panorama_source_tree(
            temporary.path(), cooked, &base, &stats, &error),
        error);
    expect(stats.base_resources == 2, "base resource count changed");
    expect(stats.shared_base_payloads == 2, "base resources were not shared");
    expect(stats.source_files() == 2, "source file count changed");
    expect(stats.source_file_allocations == 2, "source files did not use one allocation each");
    expect(stats.source_payload_bytes == cooked_xml.size() + cooked_js.size(), "source byte count changed");
    expect(stats.payload_copy_operations == 0, "source cooking copied payload bytes");
    expect(stats.copied_payload_bytes == 0, "source cooking reported copied bytes");
    expect(stats.adopted_payloads == 3, "final package adoption count changed");
    expect(
        stats.peak_live_payload_bytes == stats.final_payload_bytes,
        "source cooking retained duplicate aggregate payload storage");
    const std::vector<std::string> entries = cooked.entries();
    const std::vector<std::string> expected_entries{
        "panorama/data/base-large.bin",
        "panorama/layout/main.xml",
        "panorama/scripts/main.js",
    };
    expect(entries == expected_entries, "cooked source order is not deterministic");
    const std::vector<unsigned char> cooked_xml_read =
        cooked.read("panorama/layout/main.xml");
    expect_bytes(
        cooked_xml_read,
        cooked_xml,
        "loose source did not override base resource");
    const std::vector<unsigned char> cooked_base_read =
        cooked.read("panorama/data/base-large.bin");
    expect_bytes(
        cooked_base_read,
        base_large,
        "base-only resource bytes changed");

    panorama::PanoramaSharedBytes retained;
    expect(
        cooked.try_read("panorama/data/base-large.bin", retained),
        "cooked base view lookup failed");
    base.clear();
    cooked.clear();
    expect_bytes(retained.bytes(), base_large, "shared cooked view lost ownership");
}
}

int main()
{
    try
    {
        test_memory_views_survive_mutation_and_provider_destruction();
        test_layered_precedence_and_one_normalization();
        test_package_move_adopts_archive_and_views_outlive_package();
        test_package_provider_zero_copy_for_1k_and_1m_reads();
        test_file_open_uses_one_known_size_buffer();
        test_source_cooker_shares_base_and_adopts_loose_payloads();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "resource ownership regression test failed: %s\n", error.what());
        return 1;
    }
    std::puts("resource ownership regression tests passed");
    return 0;
}
