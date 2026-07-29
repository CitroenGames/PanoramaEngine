#include "ui/panorama/panorama_geometry_cache.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_style.hpp"
#include "ui/panorama/panorama_view.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <numeric>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef PANORAMA_PERF_BUILD_CONFIG
#define PANORAMA_PERF_BUILD_CONFIG "unknown"
#endif

#define PANORAMA_STRINGIFY_INNER(value) #value
#define PANORAMA_STRINGIFY(value) PANORAMA_STRINGIFY_INNER(value)

namespace
{
using Clock = std::chrono::steady_clock;

constexpr std::size_t kSections = 8;
constexpr std::size_t kCardsPerSection = 12;
constexpr std::size_t kNodesPerCard = 4;
constexpr std::size_t kExpectedNodes = 1 + 1 + kSections + kSections * kCardsPerSection * kNodesPerCard;
constexpr std::size_t kExpectedRules = 10;
constexpr std::size_t kIdleFrames = 4;
constexpr std::string_view kSchema = "panorama-performance-v1";

enum class OutputFormat
{
    Text,
    Json,
    Csv,
};

struct Options
{
    bool help = false;
    bool measure = false;
    bool iterations_set = false;
    bool warmup_set = false;
    std::size_t iterations = 2;
    std::size_t warmup = 0;
    OutputFormat format = OutputFormat::Text;
    std::string label = "unlabeled";
};

struct WorkRecord
{
    std::uint64_t fingerprint = 0;
    std::size_t nodes = 0;
    std::size_t rules = 0;
    std::size_t commands = 0;
    std::size_t contexts = 0;
    std::size_t vertices = 0;
    std::size_t indices = 0;
    std::size_t resource_reads = 0;
    std::size_t resource_bytes = 0;
    std::size_t idle_frames = 0;
    std::size_t changed_frames = 0;
    std::size_t geometry_commands = 0;
    std::size_t geometry_compiles = 0;
    std::size_t geometry_reuses = 0;
    std::size_t geometry_uploaded_bytes = 0;
    std::size_t backend_render_calls = 0;
    std::size_t backend_release_calls = 0;
    std::size_t backend_compiled_bytes = 0;
    std::size_t destroyed_nodes = 0;
    panorama::PanoramaCascadeStats cascade;
};

[[noreturn]] void fail(std::string_view message)
{
    throw std::runtime_error(std::string(message));
}

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        fail(message);
    }
}

std::size_t parse_count(std::string_view text, std::string_view option)
{
    std::size_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value > 10000)
    {
        throw std::runtime_error(std::string(option) + " expects an integer from 0 through 10000");
    }
    return value;
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        const auto value_after = [&](std::string_view option) -> std::string_view {
            if (++index >= argc)
            {
                throw std::runtime_error(std::string(option) + " requires a value");
            }
            return argv[index];
        };

        if (argument == "--help" || argument == "-h")
        {
            options.help = true;
        }
        else if (argument == "--check")
        {
            options.measure = false;
        }
        else if (argument == "--measure")
        {
            options.measure = true;
        }
        else if (argument == "--iterations")
        {
            options.iterations = parse_count(value_after(argument), argument);
            options.iterations_set = true;
        }
        else if (argument == "--warmup")
        {
            options.warmup = parse_count(value_after(argument), argument);
            options.warmup_set = true;
        }
        else if (argument == "--format")
        {
            const std::string_view format = value_after(argument);
            if (format == "text")
            {
                options.format = OutputFormat::Text;
            }
            else if (format == "json")
            {
                options.format = OutputFormat::Json;
            }
            else if (format == "csv")
            {
                options.format = OutputFormat::Csv;
            }
            else
            {
                fail("--format expects text, json, or csv");
            }
        }
        else if (argument == "--label")
        {
            options.label = std::string(value_after(argument));
        }
        else
        {
            throw std::runtime_error("unknown option: " + std::string(argument));
        }
    }

    if (options.measure)
    {
        if (!options.iterations_set)
        {
            options.iterations = 20;
        }
        if (!options.warmup_set)
        {
            options.warmup = 3;
        }
    }
    if (!options.help && options.iterations == 0)
    {
        fail("--iterations must be at least 1");
    }
    return options;
}

void print_help()
{
    std::cout
        << "PanoramaEngine deterministic performance-regression harness\n\n"
        << "Usage: PanoramaPerformanceRegression [options]\n\n"
        << "  --check             Deterministic correctness/work-count mode (default).\n"
        << "  --measure           Also collect informational wall time; never fails on timing.\n"
        << "  --iterations N      Measured/check runs (default: 2 check, 20 measure).\n"
        << "  --warmup N          Untimed warm-up runs (default: 0 check, 3 measure).\n"
        << "  --format F          text, json, or csv (default: text).\n"
        << "  --label VALUE       Baseline/candidate label included in structured output.\n"
        << "  --help, -h          Show this help.\n\n"
        << "Every run validates generated resource, DOM, cascade, layout, paint, frame,\n"
        << "geometry-cache, stats-disabled, reload, and teardown invariants. Shared CI\n"
        << "should use --check. Use --measure only on a controlled runner and compare\n"
        << "the emitted distributions outside this executable.\n";
}

std::string generated_layout()
{
    std::ostringstream xml;
    xml.imbue(std::locale::classic());
    xml << R"xml(<root>
    <styles>
        <include src="file://{resources}/styles/performance.css" />
    </styles>
    <Panel id="PerfRoot" class="perf-root">
)xml";
    for (std::size_t section = 0; section < kSections; ++section)
    {
        xml << "        <Panel id=\"Section_" << section << "\" class=\"section tone-" << (section % 3) << "\">\n";
        for (std::size_t card = 0; card < kCardsPerSection; ++card)
        {
            xml << "            <Panel id=\"Card_" << section << '_' << card << "\" class=\"card";
            if ((card % 4) == 0)
            {
                xml << " hot";
            }
            xml << "\">\n"
                << "                <Label class=\"card-title\" text=\"Card " << section << '-' << card << "\" />\n"
                << "                <Panel class=\"meter\">\n"
                << "                    <Panel class=\"meter-fill\" />\n"
                << "                </Panel>\n"
                << "            </Panel>\n";
        }
        xml << "        </Panel>\n";
    }
    xml << R"xml(    </Panel>
</root>
)xml";
    return xml.str();
}

constexpr std::string_view kGeneratedStyle = R"css(
.perf-root {
    flow-children: down;
    width: 100%;
    height: 100%;
    padding: 8px;
    background-color: #10151dff;
}
.section {
    flow-children: right;
    width: 100%;
    height: fill-parent-flow(1.0);
}
.card {
    flow-children: down;
    width: fill-parent-flow(1.0);
    height: 100%;
    margin: 1px;
    padding: 3px;
    background-color: #202a36ff;
    border: 1px solid #354456ff;
}
.card.hot {
    background-color: #283747ff;
}
.card.selected {
    background-color: #476b42ff;
}
.card-title {
    width: 100%;
    height: 18px;
    font-size: 12px;
    color: #d8e1ebff;
}
.meter {
    width: 100%;
    height: 8px;
    background-color: #0b0e12ff;
}
.meter-fill {
    width: 50%;
    height: 100%;
    background-color: #4e89c7ff;
}
.tone-1 .meter-fill {
    background-color: #9c6bc7ff;
}
.tone-2 .meter-fill {
    background-color: #c78a4eff;
}
)css";

class CountingResourceProvider final : public panorama::PanoramaResourceProvider
{
public:
    void add_text(std::string path, std::string_view text)
    {
        resources_[std::move(path)] = std::vector<unsigned char>(text.begin(), text.end());
    }

    [[nodiscard]] bool read(std::string_view path, panorama::PanoramaResource& out) const override
    {
        ++attempts;
        const auto found = resources_.find(std::string(path));
        if (found == resources_.end())
        {
            return false;
        }
        out.bytes = found->second;
        out.source = "generated";
        ++hits;
        bytes_returned += out.bytes.size();
        return true;
    }

    mutable std::size_t attempts = 0;
    mutable std::size_t hits = 0;
    mutable std::size_t bytes_returned = 0;

private:
    std::unordered_map<std::string, std::vector<unsigned char>> resources_;
};

class CountingLifetimeObserver final : public panorama::PanoramaNodeLifetimeObserver
{
public:
    void on_panorama_node_destroyed(panorama::PanoramaNode&) override { ++destroyed; }
    std::size_t destroyed = 0;
};

class LifetimeObserverBinding
{
public:
    explicit LifetimeObserverBinding(panorama::PanoramaNodeLifetimeObserver& observer) : observer_(observer)
    {
        panorama::panorama_add_node_lifetime_observer(observer_);
    }
    ~LifetimeObserverBinding()
    {
        panorama::panorama_remove_node_lifetime_observer(observer_);
    }

    LifetimeObserverBinding(const LifetimeObserverBinding&) = delete;
    LifetimeObserverBinding& operator=(const LifetimeObserverBinding&) = delete;

private:
    panorama::PanoramaNodeLifetimeObserver& observer_;
};

class CountingBackend final : public panorama::PanoramaRenderBackend
{
public:
    panorama::PanoramaTextureId generate_texture(std::span<const unsigned char>, int, int) override
    {
        return next_texture++;
    }

    void release_texture(panorama::PanoramaTextureId) override {}

    panorama::PanoramaCompiledGeometryHandle compile_geometry(
        std::span<const panorama::PanoramaPaintVertex> vertices,
        std::span<const int> indices,
        float) override
    {
        const panorama::PanoramaCompiledGeometryHandle handle = next_geometry++;
        live_geometry.insert(handle);
        ++compile_calls;
        compiled_bytes += vertices.size_bytes() + indices.size_bytes();
        return handle;
    }

    void render_geometry(
        panorama::PanoramaCompiledGeometryHandle geometry,
        panorama::PanoramaTextureId,
        const panorama::PanoramaDrawConstants&) override
    {
        if (!live_geometry.contains(geometry))
        {
            invalid_render = true;
        }
        ++render_calls;
    }

    void release_geometry(panorama::PanoramaCompiledGeometryHandle geometry) override
    {
        if (live_geometry.erase(geometry) != 1)
        {
            invalid_release = true;
        }
        ++release_calls;
    }

    panorama::PanoramaTextureId next_texture = 100;
    panorama::PanoramaCompiledGeometryHandle next_geometry = 1;
    std::unordered_set<panorama::PanoramaCompiledGeometryHandle> live_geometry;
    std::size_t compile_calls = 0;
    std::size_t render_calls = 0;
    std::size_t release_calls = 0;
    std::size_t compiled_bytes = 0;
    bool invalid_render = false;
    bool invalid_release = false;
};

class BackendBinding
{
public:
    explicit BackendBinding(panorama::PanoramaRenderBackend& backend)
        : previous_(panorama::panorama_render_backend())
    {
        panorama::set_panorama_render_backend(&backend);
    }
    ~BackendBinding()
    {
        panorama::set_panorama_render_backend(previous_);
    }

    BackendBinding(const BackendBinding&) = delete;
    BackendBinding& operator=(const BackendBinding&) = delete;

private:
    panorama::PanoramaRenderBackend* previous_ = nullptr;
};

panorama::PanoramaGlyphSource generated_glyph_source()
{
    panorama::PanoramaGlyphSource source;
    source.atlas_texture = 7;
    source.ascent = [](float font_size, int) { return font_size * 0.8F; };
    source.glyph = [](char32_t codepoint, float font_size, int, panorama::PanoramaGlyph& out) {
        out.advance = font_size * 0.5F;
        if (codepoint == U' ')
        {
            out.valid = false;
            return true;
        }
        const float cell = 1.0F / 16.0F;
        const float column = static_cast<float>(codepoint % 16U);
        const float row = static_cast<float>((codepoint / 16U) % 16U);
        out.bearing_y = font_size * 0.8F;
        out.width = font_size * 0.45F;
        out.height = font_size;
        out.u0 = column * cell;
        out.v0 = row * cell;
        out.u1 = out.u0 + cell;
        out.v1 = out.v0 + cell;
        out.valid = true;
        return true;
    };
    return source;
}

std::size_t count_nodes(const panorama::PanoramaNode& node)
{
    std::size_t count = 1;
    for (const auto& child : node.children)
    {
        count += count_nodes(*child);
    }
    return count;
}

class StableHash
{
public:
    void add_byte(std::uint8_t value)
    {
        value_ ^= value;
        value_ *= 1099511628211ULL;
    }

    void add_u64(std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
        {
            add_byte(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    }

    void add_i64(std::int64_t value) { add_u64(static_cast<std::uint64_t>(value)); }
    void add_bool(bool value) { add_byte(value ? 1U : 0U); }

    void add_string(std::string_view value)
    {
        add_u64(value.size());
        for (const unsigned char ch : value)
        {
            add_byte(ch);
        }
    }

    void add_float(float value)
    {
        require(std::isfinite(value), "fingerprint encountered a non-finite float");
        constexpr double scale = 1000.0;
        const double scaled = static_cast<double>(value) * scale;
        require(
            scaled >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
                scaled <= static_cast<double>(std::numeric_limits<std::int64_t>::max()),
            "fingerprint float exceeded its canonical range");
        add_i64(static_cast<std::int64_t>(std::llround(scaled)));
    }

    [[nodiscard]] std::uint64_t value() const { return value_; }

private:
    std::uint64_t value_ = 14695981039346656037ULL;
};

void hash_color(StableHash& hash, const panorama::PanoramaColor& color)
{
    hash.add_byte(color.r);
    hash.add_byte(color.g);
    hash.add_byte(color.b);
    hash.add_byte(color.a);
}

void hash_node(StableHash& hash, const panorama::PanoramaNode& node)
{
    hash.add_string(node.tag_lower);
    hash.add_string(node.id);
    hash.add_u64(node.classes.size());
    for (const std::string& value : node.classes)
    {
        hash.add_string(value);
    }
    hash.add_string(node.text);
    hash.add_bool(node.computed.visible);
    hash_color(hash, node.computed.background_color);
    hash_color(hash, node.computed.color);
    hash.add_float(node.layout.x);
    hash.add_float(node.layout.y);
    hash.add_float(node.layout.width);
    hash.add_float(node.layout.height);
    hash.add_u64(node.children.size());
    for (const auto& child : node.children)
    {
        hash_node(hash, *child);
    }
}

std::uint64_t output_fingerprint(
    const panorama::PanoramaNode& root,
    const panorama::PanoramaDrawList& draw_list)
{
    StableHash hash;
    hash.add_string(kSchema);
    hash_node(hash, root);
    hash.add_u64(draw_list.contexts.size());
    for (const panorama::PanoramaLayerContextEntry& context : draw_list.contexts)
    {
        hash.add_i64(context.parent_context_index);
        if (context.source_node != nullptr)
        {
            hash.add_string(context.source_node->id);
        }
        else
        {
            hash.add_string({});
        }
    }
    hash.add_u64(draw_list.commands.size());
    for (const panorama::PanoramaDrawCommand& command : draw_list.commands)
    {
        hash.add_u64(command.vertices.size());
        for (const panorama::PanoramaPaintVertex& vertex : command.vertices)
        {
            hash.add_float(vertex.x);
            hash.add_float(vertex.y);
            hash.add_float(vertex.u);
            hash.add_float(vertex.v);
            hash_color(hash, vertex.color);
        }
        hash.add_u64(command.indices.size());
        for (const int index : command.indices)
        {
            hash.add_i64(index);
        }
        hash.add_u64(static_cast<std::uint64_t>(command.texture));
        hash.add_u64(static_cast<std::uint64_t>(command.blend_mode));
        hash.add_bool(command.scissor);
        hash.add_float(command.scissor_x);
        hash.add_float(command.scissor_y);
        hash.add_float(command.scissor_width);
        hash.add_float(command.scissor_height);
        hash.add_float(command.blur_std_x);
        hash.add_float(command.blur_std_y);
        hash.add_i64(command.blur_passes);
        hash.add_float(command.constants.a);
        hash.add_float(command.constants.b);
        hash.add_float(command.constants.c);
        hash.add_float(command.constants.d);
        hash.add_float(command.constants.e);
        hash.add_float(command.constants.f);
        hash.add_float(command.constants.opacity);
        hash.add_bool(command.constants_patchable);
        hash.add_i64(command.context_index);
    }
    return hash.value();
}

bool same_cascade(
    const panorama::PanoramaCascadeStats& left,
    const panorama::PanoramaCascadeStats& right)
{
    return left.nodes == right.nodes && left.candidate_rules == right.candidate_rules &&
           left.selector_tests == right.selector_tests && left.simple_tests == right.simple_tests &&
           left.matched_rules == right.matched_rules &&
           left.declarations_applied == right.declarations_applied &&
           left.filter_rejects == right.filter_rejects && left.shared_nodes == right.shared_nodes;
}

bool same_record(const WorkRecord& left, const WorkRecord& right)
{
    return left.fingerprint == right.fingerprint && left.nodes == right.nodes && left.rules == right.rules &&
           left.commands == right.commands && left.contexts == right.contexts &&
           left.vertices == right.vertices && left.indices == right.indices &&
           left.resource_reads == right.resource_reads && left.resource_bytes == right.resource_bytes &&
           left.idle_frames == right.idle_frames && left.changed_frames == right.changed_frames &&
           left.geometry_commands == right.geometry_commands &&
           left.geometry_compiles == right.geometry_compiles &&
           left.geometry_reuses == right.geometry_reuses &&
           left.geometry_uploaded_bytes == right.geometry_uploaded_bytes &&
           left.backend_render_calls == right.backend_render_calls &&
           left.backend_release_calls == right.backend_release_calls &&
           left.backend_compiled_bytes == right.backend_compiled_bytes &&
           left.destroyed_nodes == right.destroyed_nodes && same_cascade(left.cascade, right.cascade);
}

WorkRecord run_workload()
{
    using namespace panorama;

    (void)panorama_cascade_stats_take();
    CountingLifetimeObserver lifetime;
    LifetimeObserverBinding lifetime_binding(lifetime);

    PanoramaView view;
    view.set_viewport(960.0F, 540.0F);
    view.set_glyph_source(generated_glyph_source());

    auto provider = std::make_unique<CountingResourceProvider>();
    CountingResourceProvider* provider_view = provider.get();
    const std::string layout = generated_layout();
    provider->add_text("panorama/layout/performance.xml", layout);
    provider->add_text("panorama/styles/performance.css", kGeneratedStyle);
    view.resources().add_provider(std::move(provider), 0, "generated-performance");

    PanoramaViewLoadOptions load_options;
    load_options.enable_scripting = false;
    load_options.document.localize_text = false;
    require(view.load("panorama/layout/performance.xml", load_options), "generated document did not load");
    require(view.root() != nullptr, "loaded view did not expose a root");

    PanoramaNode& root = *view.root();
    WorkRecord record;
    record.nodes = count_nodes(root);
    record.rules = view.session().style_sheet().rules().size();
    record.commands = view.draw_list().commands.size();
    record.contexts = view.draw_list().contexts.size();
    record.vertices = view.draw_list().total_vertices();
    record.indices = view.draw_list().total_indices();
    record.fingerprint = output_fingerprint(root, view.draw_list());

    require(record.nodes == kExpectedNodes, "generated DOM node count changed");
    require(record.rules == kExpectedRules, "generated stylesheet rule count changed");
    require(record.commands > 0 && record.vertices > 0 && record.indices > 0, "paint produced no geometry");
    require(provider_view->attempts == 2 && provider_view->hits == 2, "cold resource read count changed");
    const std::size_t cold_provider_attempts = provider_view->attempts;
    const std::size_t cold_provider_hits = provider_view->hits;
    const std::size_t cold_provider_bytes = provider_view->bytes_returned;
    const PanoramaDocumentSourceStats cold_source = view.session().source_stats();
    const std::uint64_t cold_source_generation = view.session().source_cache_generation();
    require(
        cold_source.resource_reads == 2 && cold_source.layout_parses == 1 &&
            cold_source.style_parses == 1 && cold_source.layout_cache_hits == 0 &&
            cold_source.style_cache_hits == 0 && cold_source.invalidations == 0,
        "cold source-cache work counters changed");

    for (std::size_t frame = 0; frame < kIdleFrames; ++frame)
    {
        const PanoramaViewUpdateResult idle = view.update(0.0F);
        require(
            !idle.style_changed && !idle.layout_changed && !idle.visual_changed &&
                !idle.draw_list_rebuilt && !idle.animation_active,
            "idle frame unexpectedly performed pipeline work");
        ++record.idle_frames;
    }

    PanoramaNode* selected = root.find_by_id("Card_3_4");
    require(selected != nullptr, "mutation target is missing");
    const std::size_t selected_subtree_nodes = count_nodes(*selected);
    require(selected_subtree_nodes == kNodesPerCard, "mutation subtree structure changed");
    selected->classes.push_back("selected");
    selected->mark_style_dirty();
    PanoramaViewWorkStats selected_work;
    PanoramaViewUpdateOptions measured_update;
    measured_update.work_stats = &selected_work;
    const PanoramaViewUpdateResult selected_update = view.update(0.0F, measured_update);
    require(
        selected_update.style_changed && !selected_update.layout_changed &&
            selected_update.visual_changed && selected_update.draw_list_rebuilt,
        "paint-only style mutation did not skip layout");
    require(
        selected_work.incremental_style_passes == 1 && selected_work.full_style_passes == 0 &&
            selected_work.layout_passes == 0 && selected_work.draw_list_builds == 1 &&
            selected_work.transition_nodes_visited == 0 && selected_work.keyframe_nodes_visited == 0 &&
            selected_work.scroll_nodes_visited == 0,
        "paint-only mutation did not use the measured incremental frame plan");
    require(
        output_fingerprint(root, view.draw_list()) != record.fingerprint,
        "style mutation did not change the output fingerprint");
    ++record.changed_frames;

    require(selected->classes.back() == "selected", "mutation class ordering changed");
    selected->classes.pop_back();
    selected->mark_style_dirty();
    PanoramaViewWorkStats restored_work;
    measured_update.work_stats = &restored_work;
    const PanoramaViewUpdateResult restored_update = view.update(0.0F, measured_update);
    require(
        restored_update.style_changed && !restored_update.layout_changed &&
            restored_update.visual_changed && restored_update.draw_list_rebuilt,
        "restoring paint-only style did not skip layout");
    require(
        restored_work.incremental_style_passes == 1 && restored_work.full_style_passes == 0 &&
            restored_work.layout_passes == 0 && restored_work.draw_list_builds == 1 &&
            restored_work.transition_nodes_visited == 0 && restored_work.keyframe_nodes_visited == 0 &&
            restored_work.scroll_nodes_visited == 0,
        "paint-only restore did not use the measured incremental frame plan");
    require(
        output_fingerprint(root, view.draw_list()) == record.fingerprint,
        "restoring the style did not restore the output fingerprint");
    ++record.changed_frames;

    view.set_viewport(1024.0F, 576.0F);
    const PanoramaViewUpdateResult resized = view.update(0.0F);
    require(
        !resized.style_changed && resized.layout_changed && resized.visual_changed &&
            resized.draw_list_rebuilt,
        "viewport resize did not run layout and paint only");
    require(
        output_fingerprint(root, view.draw_list()) != record.fingerprint,
        "viewport resize did not change the output fingerprint");
    ++record.changed_frames;

    view.set_viewport(960.0F, 540.0F);
    const PanoramaViewUpdateResult size_restored = view.update(0.0F);
    require(
        !size_restored.style_changed && size_restored.layout_changed &&
            size_restored.visual_changed && size_restored.draw_list_rebuilt,
        "restoring the viewport did not run layout and paint only");
    require(
        output_fingerprint(root, view.draw_list()) == record.fingerprint,
        "restoring the viewport did not restore the output fingerprint");
    ++record.changed_frames;

    CountingBackend backend;
    BackendBinding backend_binding(backend);
    PanoramaGeometryCache cache;

    // The first submit deliberately omits the optional stats pointer. Its
    // externally visible work must match the profiled path and leave a valid
    // cache; PanoramaGeometryCache documents that this path performs no clock
    // reads.
    cache.submit(view.draw_list(), backend, 1.0F, nullptr);
    require(cache.valid(), "stats-disabled geometry submit did not produce a valid cache");
    const std::size_t compiled_without_stats = backend.compile_calls;
    require(compiled_without_stats > 0, "stats-disabled geometry submit compiled nothing");

    PanoramaGeometrySubmitStats reuse_stats;
    cache.submit(view.draw_list(), backend, 1.0F, &reuse_stats);
    require(cache.valid(), "profiled geometry reuse submit invalidated the cache");
    require(
        reuse_stats.commands == static_cast<int>(compiled_without_stats) &&
            reuse_stats.reused == reuse_stats.commands && reuse_stats.recompiled == 0 &&
            reuse_stats.uploaded_bytes == 0,
        "geometry reuse work counters changed");
    require(backend.compile_calls == compiled_without_stats, "unchanged submit recompiled geometry");

    const std::size_t render_calls_before_replay = backend.render_calls;
    require(cache.replay(backend), "unchanged geometry cache did not replay");
    require(
        backend.render_calls - render_calls_before_replay == compiled_without_stats,
        "geometry replay call count changed");

    record.geometry_commands = static_cast<std::size_t>(reuse_stats.commands);
    record.geometry_compiles = compiled_without_stats;
    record.geometry_reuses = static_cast<std::size_t>(reuse_stats.reused);
    record.geometry_uploaded_bytes = reuse_stats.uploaded_bytes;
    record.backend_render_calls = backend.render_calls;
    record.backend_compiled_bytes = backend.compiled_bytes;

    cache.release();
    record.backend_release_calls = backend.release_calls;
    require(
        backend.release_calls == compiled_without_stats && backend.live_geometry.empty() &&
            !backend.invalid_render && !backend.invalid_release,
        "geometry teardown did not release every live handle exactly once");

    const std::size_t before_first_unload = lifetime.destroyed;
    view.unload();
    const std::size_t first_destroyed = lifetime.destroyed - before_first_unload;
    require(first_destroyed == record.nodes, "first unload destruction count changed");
    require(!view.loaded() && view.root() == nullptr && view.draw_list().commands.empty(), "unload left live view state");

    require(view.load("panorama/layout/performance.xml", load_options), "generated document did not reload");
    require(view.root() != nullptr, "reloaded view did not expose a root");
    require(count_nodes(*view.root()) == record.nodes, "reload changed the generated DOM structure");
    require(
        output_fingerprint(*view.root(), view.draw_list()) == record.fingerprint,
        "reload changed the output fingerprint");
    const PanoramaDocumentSourceStats reload_source = view.session().source_stats();
    require(
        provider_view->attempts == cold_provider_attempts &&
            provider_view->hits == cold_provider_hits &&
            provider_view->bytes_returned == cold_provider_bytes,
        "cached reload repeated provider reads");
    require(
        reload_source.resource_reads == cold_source.resource_reads &&
            reload_source.layout_parses == cold_source.layout_parses &&
            reload_source.style_parses == cold_source.style_parses &&
            reload_source.layout_cache_hits == cold_source.layout_cache_hits + 1 &&
            reload_source.style_cache_hits == cold_source.style_cache_hits + 1 &&
            reload_source.invalidations == cold_source.invalidations &&
            view.session().source_cache_generation() == cold_source_generation,
        "cached reload source-cache counters changed");

    const std::size_t before_second_unload = lifetime.destroyed;
    view.unload();
    const std::size_t second_destroyed = lifetime.destroyed - before_second_unload;
    require(second_destroyed == record.nodes, "second unload destruction count changed");
    record.destroyed_nodes = first_destroyed + second_destroyed;

    record.resource_reads = provider_view->hits;
    record.resource_bytes = provider_view->bytes_returned;
    record.cascade = panorama_cascade_stats_take();
    const std::size_t expected_cascade_nodes = record.nodes * 2 + selected_subtree_nodes * 2;
    require(
        record.cascade.nodes == expected_cascade_nodes,
        "cascade node visits no longer match two full loads plus two incremental mutation subtrees: expected " +
            std::to_string(expected_cascade_nodes) + ", got " + std::to_string(record.cascade.nodes));
    return record;
}

std::string hex_fingerprint(std::uint64_t value)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::string platform_name()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string architecture_name()
{
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::string compiler_name()
{
#if defined(__clang__)
    return "clang-" __clang_version__;
#elif defined(_MSC_VER)
    return "msvc-" PANORAMA_STRINGIFY(_MSC_FULL_VER);
#elif defined(__GNUC__)
    return "gcc-" PANORAMA_STRINGIFY(__GNUC__) "." PANORAMA_STRINGIFY(__GNUC_MINOR__) "." PANORAMA_STRINGIFY(__GNUC_PATCHLEVEL__);
#else
    return "unknown";
#endif
}

std::string json_escape(std::string_view value)
{
    std::ostringstream out;
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(ch)
                    << std::dec;
            }
            else
            {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

std::string csv_escape(std::string_view value)
{
    if (value.find_first_of(",\"\r\n") == std::string_view::npos)
    {
        return std::string(value);
    }
    std::string escaped = "\"";
    for (const char ch : value)
    {
        if (ch == '"')
        {
            escaped += '"';
        }
        escaped += ch;
    }
    escaped += '"';
    return escaped;
}

struct TimingSummary
{
    double minimum_ms = 0.0;
    double median_ms = 0.0;
    double mean_ms = 0.0;
    double maximum_ms = 0.0;
};

TimingSummary summarize_timings(const std::vector<double>& timings)
{
    require(!timings.empty(), "cannot summarize an empty timing sample");
    std::vector<double> ordered = timings;
    std::sort(ordered.begin(), ordered.end());
    TimingSummary summary;
    summary.minimum_ms = ordered.front();
    summary.maximum_ms = ordered.back();
    summary.mean_ms = std::accumulate(ordered.begin(), ordered.end(), 0.0) /
        static_cast<double>(ordered.size());
    const std::size_t middle = ordered.size() / 2;
    summary.median_ms = (ordered.size() % 2) != 0
        ? ordered[middle]
        : (ordered[middle - 1] + ordered[middle]) * 0.5;
    return summary;
}

void print_text(
    const Options& options,
    const WorkRecord& record,
    const std::vector<double>& timings)
{
    std::cout
        << "schema=" << kSchema << '\n'
        << "label=" << options.label << '\n'
        << "mode=" << (options.measure ? "measure" : "check") << '\n'
        << "environment.platform=" << platform_name() << '\n'
        << "environment.arch=" << architecture_name() << '\n'
        << "environment.compiler=" << compiler_name() << '\n'
        << "environment.build_config=" << PANORAMA_PERF_BUILD_CONFIG << '\n'
        << "environment.pointer_bits=" << sizeof(void*) * 8 << '\n'
        << "environment.hardware_threads=" << std::thread::hardware_concurrency() << '\n'
        << "iterations=" << options.iterations << '\n'
        << "warmup=" << options.warmup << '\n'
        << "fingerprint=" << hex_fingerprint(record.fingerprint) << '\n'
        << "work.nodes=" << record.nodes << '\n'
        << "work.rules=" << record.rules << '\n'
        << "work.commands=" << record.commands << '\n'
        << "work.contexts=" << record.contexts << '\n'
        << "work.vertices=" << record.vertices << '\n'
        << "work.indices=" << record.indices << '\n'
        << "work.resource_reads=" << record.resource_reads << '\n'
        << "work.resource_bytes=" << record.resource_bytes << '\n'
        << "work.idle_frames=" << record.idle_frames << '\n'
        << "work.changed_frames=" << record.changed_frames << '\n'
        << "work.geometry_commands=" << record.geometry_commands << '\n'
        << "work.geometry_compiles=" << record.geometry_compiles << '\n'
        << "work.geometry_reuses=" << record.geometry_reuses << '\n'
        << "work.geometry_uploaded_bytes=" << record.geometry_uploaded_bytes << '\n'
        << "work.backend_render_calls=" << record.backend_render_calls << '\n'
        << "work.backend_release_calls=" << record.backend_release_calls << '\n'
        << "work.backend_compiled_bytes=" << record.backend_compiled_bytes << '\n'
        << "work.destroyed_nodes=" << record.destroyed_nodes << '\n'
        << "cascade.nodes=" << record.cascade.nodes << '\n'
        << "cascade.candidate_rules=" << record.cascade.candidate_rules << '\n'
        << "cascade.selector_tests=" << record.cascade.selector_tests << '\n'
        << "cascade.simple_tests=" << record.cascade.simple_tests << '\n'
        << "cascade.matched_rules=" << record.cascade.matched_rules << '\n'
        << "cascade.declarations_applied=" << record.cascade.declarations_applied << '\n'
        << "cascade.filter_rejects=" << record.cascade.filter_rejects << '\n'
        << "cascade.shared_nodes=" << record.cascade.shared_nodes << '\n';
    if (options.measure)
    {
        const TimingSummary timing = summarize_timings(timings);
        std::cout << std::fixed << std::setprecision(3)
                  << "timing.minimum_ms=" << timing.minimum_ms << '\n'
                  << "timing.median_ms=" << timing.median_ms << '\n'
                  << "timing.mean_ms=" << timing.mean_ms << '\n'
                  << "timing.maximum_ms=" << timing.maximum_ms << '\n'
                  << "timing.samples_ms=";
        for (std::size_t index = 0; index < timings.size(); ++index)
        {
            std::cout << (index == 0 ? "" : ",") << timings[index];
        }
        std::cout << '\n';
    }
    else
    {
        std::cout << "timing=disabled\n";
    }
}

void print_json(
    const Options& options,
    const WorkRecord& record,
    const std::vector<double>& timings)
{
    std::cout
        << "{\"schema\":\"" << kSchema
        << "\",\"label\":\"" << json_escape(options.label)
        << "\",\"mode\":\"" << (options.measure ? "measure" : "check")
        << "\",\"environment\":{\"platform\":\"" << platform_name()
        << "\",\"arch\":\"" << architecture_name()
        << "\",\"compiler\":\"" << json_escape(compiler_name())
        << "\",\"build_config\":\"" << json_escape(PANORAMA_PERF_BUILD_CONFIG)
        << "\",\"pointer_bits\":" << sizeof(void*) * 8
        << ",\"hardware_threads\":" << std::thread::hardware_concurrency()
        << "},\"run\":{\"iterations\":" << options.iterations
        << ",\"warmup\":" << options.warmup
        << "},\"work\":{\"fingerprint\":\"" << hex_fingerprint(record.fingerprint)
        << "\",\"nodes\":" << record.nodes
        << ",\"rules\":" << record.rules
        << ",\"commands\":" << record.commands
        << ",\"contexts\":" << record.contexts
        << ",\"vertices\":" << record.vertices
        << ",\"indices\":" << record.indices
        << ",\"resource_reads\":" << record.resource_reads
        << ",\"resource_bytes\":" << record.resource_bytes
        << ",\"idle_frames\":" << record.idle_frames
        << ",\"changed_frames\":" << record.changed_frames
        << ",\"geometry_commands\":" << record.geometry_commands
        << ",\"geometry_compiles\":" << record.geometry_compiles
        << ",\"geometry_reuses\":" << record.geometry_reuses
        << ",\"geometry_uploaded_bytes\":" << record.geometry_uploaded_bytes
        << ",\"backend_render_calls\":" << record.backend_render_calls
        << ",\"backend_release_calls\":" << record.backend_release_calls
        << ",\"backend_compiled_bytes\":" << record.backend_compiled_bytes
        << ",\"destroyed_nodes\":" << record.destroyed_nodes
        << "},\"cascade\":{\"nodes\":" << record.cascade.nodes
        << ",\"candidate_rules\":" << record.cascade.candidate_rules
        << ",\"selector_tests\":" << record.cascade.selector_tests
        << ",\"simple_tests\":" << record.cascade.simple_tests
        << ",\"matched_rules\":" << record.cascade.matched_rules
        << ",\"declarations_applied\":" << record.cascade.declarations_applied
        << ",\"filter_rejects\":" << record.cascade.filter_rejects
        << ",\"shared_nodes\":" << record.cascade.shared_nodes << "},\"timing\":";
    if (options.measure)
    {
        const TimingSummary timing = summarize_timings(timings);
        std::cout << std::fixed << std::setprecision(3)
                  << "{\"minimum_ms\":" << timing.minimum_ms
                  << ",\"median_ms\":" << timing.median_ms
                  << ",\"mean_ms\":" << timing.mean_ms
                  << ",\"maximum_ms\":" << timing.maximum_ms
                  << ",\"samples_ms\":[";
        for (std::size_t index = 0; index < timings.size(); ++index)
        {
            std::cout << (index == 0 ? "" : ",") << timings[index];
        }
        std::cout << "]}";
    }
    else
    {
        std::cout << "null";
    }
    std::cout << "}\n";
}

void print_csv(
    const Options& options,
    const WorkRecord& record,
    const std::vector<double>& timings)
{
    std::cout
        << "schema,label,mode,platform,arch,compiler,build_config,pointer_bits,hardware_threads,"
           "iterations,warmup,fingerprint,nodes,rules,commands,contexts,vertices,indices,"
           "resource_reads,resource_bytes,idle_frames,changed_frames,geometry_commands,"
           "geometry_compiles,geometry_reuses,geometry_uploaded_bytes,backend_render_calls,"
           "backend_release_calls,backend_compiled_bytes,destroyed_nodes,cascade_nodes,"
           "cascade_candidate_rules,cascade_selector_tests,cascade_simple_tests,"
           "cascade_matched_rules,cascade_declarations_applied,cascade_filter_rejects,"
           "cascade_shared_nodes,timing_minimum_ms,timing_median_ms,timing_mean_ms,"
           "timing_maximum_ms,timing_samples_ms\n";
    std::cout
        << kSchema << ',' << csv_escape(options.label) << ','
        << (options.measure ? "measure" : "check") << ','
        << platform_name() << ',' << architecture_name() << ','
        << csv_escape(compiler_name()) << ',' << csv_escape(PANORAMA_PERF_BUILD_CONFIG) << ','
        << sizeof(void*) * 8 << ',' << std::thread::hardware_concurrency() << ','
        << options.iterations << ',' << options.warmup << ','
        << hex_fingerprint(record.fingerprint) << ','
        << record.nodes << ',' << record.rules << ',' << record.commands << ','
        << record.contexts << ',' << record.vertices << ',' << record.indices << ','
        << record.resource_reads << ',' << record.resource_bytes << ','
        << record.idle_frames << ',' << record.changed_frames << ','
        << record.geometry_commands << ',' << record.geometry_compiles << ','
        << record.geometry_reuses << ',' << record.geometry_uploaded_bytes << ','
        << record.backend_render_calls << ',' << record.backend_release_calls << ','
        << record.backend_compiled_bytes << ',' << record.destroyed_nodes << ','
        << record.cascade.nodes << ',' << record.cascade.candidate_rules << ','
        << record.cascade.selector_tests << ',' << record.cascade.simple_tests << ','
        << record.cascade.matched_rules << ',' << record.cascade.declarations_applied << ','
        << record.cascade.filter_rejects << ',' << record.cascade.shared_nodes << ',';
    if (options.measure)
    {
        const TimingSummary timing = summarize_timings(timings);
        std::cout << std::fixed << std::setprecision(3)
                  << timing.minimum_ms << ',' << timing.median_ms << ','
                  << timing.mean_ms << ',' << timing.maximum_ms << ",\"";
        for (std::size_t index = 0; index < timings.size(); ++index)
        {
            std::cout << (index == 0 ? "" : ";") << timings[index];
        }
        std::cout << '"';
    }
    else
    {
        std::cout << ",,,,";
    }
    std::cout << '\n';
}
}

int main(int argc, char** argv)
{
    try
    {
        std::cout.imbue(std::locale::classic());
        const Options options = parse_options(argc, argv);
        if (options.help)
        {
            print_help();
            return EXIT_SUCCESS;
        }

        for (std::size_t warmup = 0; warmup < options.warmup; ++warmup)
        {
            (void)run_workload();
        }

        WorkRecord baseline;
        bool have_baseline = false;
        std::vector<double> timings;
        timings.reserve(options.measure ? options.iterations : 0);
        for (std::size_t iteration = 0; iteration < options.iterations; ++iteration)
        {
            WorkRecord current;
            if (options.measure)
            {
                const Clock::time_point start = Clock::now();
                current = run_workload();
                const Clock::time_point end = Clock::now();
                timings.push_back(std::chrono::duration<double, std::milli>(end - start).count());
            }
            else
            {
                current = run_workload();
            }

            if (!have_baseline)
            {
                baseline = current;
                have_baseline = true;
            }
            else
            {
                require(
                    same_record(baseline, current),
                    "deterministic work record changed between identical iterations");
            }
        }

        switch (options.format)
        {
        case OutputFormat::Text:
            print_text(options, baseline, timings);
            break;
        case OutputFormat::Json:
            print_json(options, baseline, timings);
            break;
        case OutputFormat::Csv:
            print_csv(options, baseline, timings);
            break;
        }
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Panorama performance regression failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
