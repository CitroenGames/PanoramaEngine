#pragma once

#include "ui/panorama/panorama_package.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openstrike
{
struct PanoramaResource
{
    std::vector<unsigned char> bytes;
    std::string source;
};

class PanoramaResourceProvider
{
public:
    virtual ~PanoramaResourceProvider() = default;

    [[nodiscard]] virtual bool read(std::string_view path, PanoramaResource& out) const = 0;
    [[nodiscard]] virtual std::optional<std::filesystem::path> resolve_file(std::string_view path) const;
};

class PanoramaResourceManager
{
public:
    void add_provider(
        std::unique_ptr<PanoramaResourceProvider> provider,
        int priority = 0,
        std::string identifier = {});
    void remove_providers(std::string_view identifier);
    void clear();

    [[nodiscard]] bool read(std::string_view path, PanoramaResource& out) const;
    [[nodiscard]] std::optional<std::string> read_text(std::string_view path) const;
    [[nodiscard]] std::optional<std::filesystem::path> resolve_file(std::string_view path) const;
    [[nodiscard]] bool empty() const noexcept;

private:
    struct ProviderEntry
    {
        int priority = 0;
        std::size_t sequence = 0;
        std::string identifier;
        std::unique_ptr<PanoramaResourceProvider> provider;
    };

    std::vector<ProviderEntry> providers_;
    std::size_t next_sequence_ = 0;
};

class PanoramaMemoryResourceProvider final : public PanoramaResourceProvider
{
public:
    void add(std::string_view path, std::span<const unsigned char> bytes);
    void add_text(std::string_view path, std::string_view text);

    [[nodiscard]] bool read(std::string_view path, PanoramaResource& out) const override;

private:
    std::unordered_map<std::string, PanoramaResource> resources_;
};

class PanoramaPackageResourceProvider final : public PanoramaResourceProvider
{
public:
    explicit PanoramaPackageResourceProvider(const PanoramaPackage& package);

    [[nodiscard]] bool read(std::string_view path, PanoramaResource& out) const override;

private:
    const PanoramaPackage* package_ = nullptr;
};

class PanoramaDirectoryResourceProvider final : public PanoramaResourceProvider
{
public:
    explicit PanoramaDirectoryResourceProvider(std::filesystem::path root);

    [[nodiscard]] bool read(std::string_view path, PanoramaResource& out) const override;
    [[nodiscard]] std::optional<std::filesystem::path> resolve_file(std::string_view path) const override;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;

private:
    std::filesystem::path root_;
};
}
