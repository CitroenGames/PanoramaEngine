#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openstrike
{
class PanoramaPackage
{
public:
    [[nodiscard]] bool open(const std::filesystem::path& path, std::string* error_message = nullptr);
    void clear();

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool contains(std::string_view entry_path) const;
    [[nodiscard]] std::vector<std::string> entries() const;
    [[nodiscard]] std::vector<unsigned char> read(std::string_view entry_path) const;
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
