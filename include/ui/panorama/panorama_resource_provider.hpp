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

// A single-source abstraction over "where do bytes for a Panorama-relative path
// come from". The engine never touches a filesystem or archive directly; every
// XML/CSS/script/image load goes through a PanoramaResourceProvider (or, more
// usually, several of them layered in a PanoramaResourceManager) so a host can
// back Panorama paths with loose files, an in-memory map, or a .pbin package
// without the engine caring which.
namespace panorama
{
// `bytes` is the raw file content; `source` is a human-readable origin (e.g. a
// package path or "memory") used only for logging/diagnostics, never parsed.
struct PanoramaResource
{
    std::vector<unsigned char> bytes;
    std::string source;
};

// One resource backend. Implement `read()`; everything else has a usable
// default. `path` arguments are Panorama-relative (already normalized by the
// caller, e.g. via normalize_panorama_entry_path) — a provider does not need to
// handle `file://`/`{resources}` prefixes itself.
class PanoramaResourceProvider
{
public:
    virtual ~PanoramaResourceProvider() = default;

    // Returns false (leaving `out` untouched) when this provider has no entry
    // at `path`; that is not an error; PanoramaResourceManager just moves on
    // to the next provider.
    [[nodiscard]] virtual bool read(std::string_view path, PanoramaResource& out) const = 0;
    // Exposes a real filesystem path for `path` when one exists, for consumers
    // that need to hand a path to an external API (e.g. a system font loader)
    // instead of reading bytes themselves. Default: unsupported (nullopt) — a
    // provider backed by memory or a zip has no real path to give.
    [[nodiscard]] virtual std::optional<std::filesystem::path> resolve_file(std::string_view path) const;
};

// Layers zero or more PanoramaResourceProvider instances behind one `read()`
// call. Providers are tried in ascending `priority` order (lowest first, ties
// broken by insertion order) and the first one whose `read()` succeeds wins —
// so to make a provider take precedence over another, give it a *lower*
// priority number (the default is 0; use e.g. -1 for an override layer placed
// in front of a priority-0 base provider). See docs/integration.md for the
// override-layering pattern.
class PanoramaResourceManager
{
public:
    void add_provider(
        std::unique_ptr<PanoramaResourceProvider> provider,
        int priority = 0,
        std::string identifier = {});
    // Removes every provider previously registered with this `identifier`
    // (providers added with the default empty identifier can't be targeted
    // individually this way — use clear() instead).
    void remove_providers(std::string_view identifier);
    void clear();

    [[nodiscard]] bool read(std::string_view path, PanoramaResource& out) const;
    // Convenience wrapper over read() that returns the bytes reinterpreted as
    // text, or nullopt if no provider has `path`.
    [[nodiscard]] std::optional<std::string> read_text(std::string_view path) const;
    // Ascending-priority order, same as read(): the first provider that
    // resolves `path` wins.
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

// An in-memory `path -> bytes` map. Useful for tests, for UI a host authors as
// C++ string literals instead of loading from disk, or as a virtual-path
// override layer registered ahead of a real package/directory provider. `add`
// and `add_text` overwrite any existing entry at the same path.
class PanoramaMemoryResourceProvider final : public PanoramaResourceProvider
{
public:
    void add(std::string_view path, std::span<const unsigned char> bytes);
    void add_text(std::string_view path, std::string_view text);

    [[nodiscard]] bool read(std::string_view path, PanoramaResource& out) const override;

private:
    std::unordered_map<std::string, PanoramaResource> resources_;
};

// Reads through an already-open PanoramaPackage (.pbin). Does not own the
// package — the referenced PanoramaPackage must outlive this provider.
class PanoramaPackageResourceProvider final : public PanoramaResourceProvider
{
public:
    explicit PanoramaPackageResourceProvider(const PanoramaPackage& package);

    [[nodiscard]] bool read(std::string_view path, PanoramaResource& out) const override;

private:
    const PanoramaPackage* package_ = nullptr;
};

// Reads loose files under `root` on the real filesystem. The only shipped
// provider that implements resolve_file() (returns `root / path` when the
// file exists).
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
