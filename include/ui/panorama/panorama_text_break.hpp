#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Line breaking for label text, ported from WebCore's BreakLines.{h,cpp}
// (rendering/): breakable spaces (space / tab / newline), the printable-ASCII
// line-break pair table (break before opening punctuation, after '-' and '?'),
// and the hyphen-before-digit minus-sign exception. WebCore defers anything
// outside printable ASCII to ICU; this engine has no ICU, so non-ASCII bytes
// are simply not break opportunities (no CJK breaking — acceptable for the
// CS:GO Latin UI corpus).
namespace panorama
{
// WebCore isBreakableSpace (NonBreakingSpaceBehavior::IgnoreNonBreakingSpace —
// the white-space:normal default; NBSP is U+00A0, outside ASCII here anyway).
[[nodiscard]] bool panorama_is_breakable_space(char c);

// WebCore shouldBreakAfter(lastCh, ch, nextCh): whether a break is allowed
// between `ch` and `next_ch`. `last_ch` is the character before `ch` (0 when
// none) — it gates the '-' before-digit case ("ABCD-1234" breaks, "-1234"'s
// minus sign does not).
[[nodiscard]] bool panorama_should_break_after(char last_ch, char ch, char next_ch);

// WebCore nextBreakablePosition (ASCII shortcut path): the smallest i in
// [start, text.size()] such that a line break is allowed BEFORE text[i].
// `prior_last` / `prior_second_to_last` seed the pair-table context when the
// text continues an earlier run (WebCore's LazyLineBreakIterator priorContext),
// e.g. styled runs of one html="true" label split mid-word.
[[nodiscard]] std::size_t panorama_next_breakable_position(
    std::string_view text, std::size_t start, char prior_last = '\0', char prior_second_to_last = '\0');

// One styled run of a label's display text (already case-transformed). Plain
// labels pass a single run; html="true" labels pass one per markup run so bold
// spans measure at their own weight.
struct PanoramaTextWrapRun
{
    std::string_view text;
    int font_weight = 400;
};

// A slice of one run placed on a line: [begin, end) byte offsets into
// runs[run].text.
struct PanoramaTextWrapSegment
{
    int run = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct PanoramaTextWrapLine
{
    float width = 0.0F; // advance of the line's segments (trailing spaces excluded)
    std::vector<PanoramaTextWrapSegment> segments;
};

struct PanoramaTextShapedGlyph
{
    char32_t codepoint = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
    float advance = 0.0F;
    float leading_kerning = 0.0F;
};

struct PanoramaTextArtifactRunInput
{
    std::string_view text;
    int font_weight = 400;
    bool italic = false;
};

// Lightweight lookup key. source_text is borrowed only for the duration of the
// call; cached artifacts own their key and transformed display string.
struct PanoramaTextArtifactRequest
{
    std::string_view source_text;
    std::string_view display_text;
    std::span<const PanoramaTextArtifactRunInput> runs;
    float font_size = 0.0F;
    int font_weight = 400;
    float letter_spacing = 0.0F;
    float line_height = 0.0F;
    float available_width = 0.0F;
    int text_transform = 0;
    int text_overflow = 0;
    bool html = false;
    bool wrap = false;
    std::uint64_t font_generation = 0;
};

struct PanoramaTextArtifactRun
{
    std::size_t display_begin = 0;
    std::size_t display_end = 0;
    int font_weight = 400;
    bool italic = false;
    float width = 0.0F;
    std::vector<PanoramaTextShapedGlyph> glyphs;
    std::vector<float> prefix_advances; // one after each glyph; [0] == 0
};

struct PanoramaTextArtifact
{
    std::uint64_t version = 0;
    std::uint64_t font_generation = 0;
    std::string display_text;
    std::vector<PanoramaTextArtifactRun> runs;
    std::vector<PanoramaTextWrapLine> lines;
    float width = 0.0F;
    float height = 0.0F;
    float line_advance = 0.0F;
};

using PanoramaTextShapeRun = std::function<float(std::string_view text, float font_size, int font_weight,
    float letter_spacing, std::vector<PanoramaTextShapedGlyph>& glyphs)>;

struct PanoramaTextArtifactCacheBudgets
{
    std::size_t entries = 1024;
    std::size_t bytes = 4U * 1024U * 1024U;
};

struct PanoramaTextArtifactStats
{
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t builds = 0;
    std::uint64_t evictions = 0;
    std::uint64_t shaped_runs = 0;
    std::uint64_t shaped_glyphs = 0;
    std::uint64_t wrap_passes = 0;
    std::uint64_t prefix_queries = 0;
};

void panorama_set_text_artifact_cache_budgets(PanoramaTextArtifactCacheBudgets budgets);
[[nodiscard]] PanoramaTextArtifactCacheBudgets panorama_text_artifact_cache_budgets();
void panorama_enable_text_artifact_counters(bool enabled);
void panorama_reset_text_artifact_counters();
[[nodiscard]] PanoramaTextArtifactStats panorama_text_artifact_stats();

[[nodiscard]] std::shared_ptr<const PanoramaTextArtifact> panorama_prepare_text_artifact(
    const PanoramaTextArtifactRequest& request, const PanoramaTextShapeRun& shape);
[[nodiscard]] std::shared_ptr<const PanoramaTextArtifact> panorama_find_text_artifact(
    const PanoramaTextArtifactRequest& request);
[[nodiscard]] float panorama_text_artifact_segment_width(
    const PanoramaTextArtifact& artifact, int run, std::size_t begin, std::size_t end);
[[nodiscard]] float panorama_text_artifact_prefix_width(
    const PanoramaTextArtifact& artifact, int run, std::size_t byte_offset);

// Advance width of runs[run].text.substr(begin, end - begin) at that run's
// weight. The caller binds font size / letter spacing.
using PanoramaTextWrapMeasure = std::function<float(int run, std::size_t begin, std::size_t end)>;

// Greedy line fitter over WebCore break opportunities (the shape of WebCore's
// simple line layout): words are placed until the next one would overflow
// `available_width`, then the line flushes. White-space:normal collapsing at
// line edges — a break consumes the breakable spaces around it (they appear on
// no line and contribute no width); a word wider than the line overflows on its
// own line (no mid-word break — break-anywhere/hyphenation unsupported, as in
// WebCore without those properties). '\n' is a forced break (Panorama labels
// honour embedded newlines, unlike HTML white-space:normal).
// Returns one entry per line; a single-line result means nothing wrapped.
[[nodiscard]] std::vector<PanoramaTextWrapLine> panorama_wrap_text_lines(
    const std::vector<PanoramaTextWrapRun>& runs, float available_width, const PanoramaTextWrapMeasure& measure);
}
