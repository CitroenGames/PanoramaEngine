#pragma once

#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

// Line breaking for label text, ported from WebCore's BreakLines.{h,cpp}
// (rendering/): breakable spaces (space / tab / newline), the printable-ASCII
// line-break pair table (break before opening punctuation, after '-' and '?'),
// and the hyphen-before-digit minus-sign exception. WebCore defers anything
// outside printable ASCII to ICU; this engine has no ICU, so non-ASCII bytes
// are simply not break opportunities (no CJK breaking — acceptable for the
// CS:GO Latin UI corpus).
namespace openstrike
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
