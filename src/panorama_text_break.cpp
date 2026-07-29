#include "ui/panorama/panorama_text_break.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace panorama
{
namespace
{
// ---- WebCore BreakLines.cpp: the printable-ASCII line-break pair table -------
// Row = the character broken AFTER, column = the following character. Break
// opportunities: before opening punctuation such as '(' '<' '[' '{' after
// certain characters (Firefox-compatible), and after '-' and '?' (IE-
// compatible). Ported verbatim; see WebCore for the cross-browser matrix
// reference (webkit.org bug 37698).
constexpr unsigned char kLineBreakTableFirstCharacter = '!';
constexpr unsigned char kLineBreakTableLastCharacter = 127;
constexpr unsigned kLineBreakTableColumnCount = (kLineBreakTableLastCharacter - kLineBreakTableFirstCharacter) / 8 + 1;

// Pack 8 bits into one byte (WebCore's B macro).
#define B(a, b, c, d, e, f, g, h) \
    ((a) | ((b) << 1) | ((c) << 2) | ((d) << 3) | ((e) << 4) | ((f) << 5) | ((g) << 6) | ((h) << 7))

// Line breaking table row for each digit (0-9).
#define DI {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

// Line breaking table row for ascii letters (a-z A-Z).
#define AL {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

#define F 0xFF

// clang-format off
constexpr unsigned char kLineBreakTable[][kLineBreakTableColumnCount] = {
    //  !  "  #  $  %  &  '  (     )  *  +  ,  -  .  /  0  1-8   9  :  ;  <  =  >  ?  @     A-X      Y  Z  [  \  ]  ^  _  `     a-x      y  z  {  |  }  ~  DEL
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // !
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // "
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // #
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // $
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // %
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // &
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // '
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // (
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // )
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // *
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // +
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // ,
    { B(1, 1, 1, 1, 1, 1, 1, 1), B(1, 1, 1, 0, 1, 0, 1, 0), 0, B(0, 1, 1, 1, 1, 1, 1, 1), F, F, F, B(1, 1, 1, 1, 1, 1, 1, 1), F, F, F, B(1, 1, 1, 1, 1, 1, 1, 1) }, // - Note: breaking before '0'-'9' is handled hard-coded in panorama_should_break_after().
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // .
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // /
    DI,  DI,  DI,  DI,  DI,  DI,  DI,  DI,  DI,  DI, // 0-9
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // :
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // ;
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // <
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // =
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // >
    { B(0, 0, 1, 1, 1, 1, 0, 1), B(0, 1, 1, 0, 1, 0, 0, 1), F, B(1, 0, 0, 1, 1, 1, 0, 1), F, F, F, B(1, 1, 1, 1, 0, 1, 1, 1), F, F, F, B(1, 1, 1, 1, 0, 1, 1, 0) }, // ?
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // @
    AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL, // A-Z
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // [
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // '\'
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // ]
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // ^
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // _
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // `
    AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL, // a-z
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // {
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // |
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // }
    { B(0, 0, 0, 0, 0, 0, 0, 1), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 1, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 1, 0, 0, 0, 0, 0) }, // ~
    { B(0, 0, 0, 0, 0, 0, 0, 0), B(0, 0, 0, 0, 0, 0, 0, 0), 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0), 0, 0, 0, B(0, 0, 0, 0, 0, 0, 0, 0) }, // DEL
};
// clang-format on

#undef B
#undef F
#undef DI
#undef AL

static_assert(sizeof(kLineBreakTable) / sizeof(kLineBreakTable[0]) ==
        kLineBreakTableLastCharacter - kLineBreakTableFirstCharacter + 1,
    "line break table consistency");

bool is_ascii_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

bool is_ascii_alphanumeric(unsigned char c)
{
    return is_ascii_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
}

bool panorama_is_breakable_space(char c)
{
    switch (c)
    {
    case ' ':
    case '\n':
    case '\t':
        return true;
    default:
        return false;
    }
}

bool panorama_should_break_after(char last_ch, char ch, char next_ch)
{
    const auto last = static_cast<unsigned char>(last_ch);
    const auto character = static_cast<unsigned char>(ch);
    const auto next = static_cast<unsigned char>(next_ch);

    // WebCore shouldBreakAfter: don't allow a break between '-' and a digit if
    // the '-' may mean a minus sign in the context, while allowing breaking in
    // 'ABCD-1234' and '1234-5678' which may be in long URLs.
    if (character == '-' && is_ascii_digit(next))
    {
        return is_ascii_alphanumeric(last);
    }

    if (character >= kLineBreakTableFirstCharacter && character <= kLineBreakTableLastCharacter &&
        next >= kLineBreakTableFirstCharacter && next <= kLineBreakTableLastCharacter)
    {
        const unsigned char* table_row = kLineBreakTable[character - kLineBreakTableFirstCharacter];
        const unsigned next_index = next - kLineBreakTableFirstCharacter;
        return (table_row[next_index / 8] & (1U << (next_index % 8))) != 0;
    }
    // WebCore defers non-ASCII pairs to ICU; without ICU they are not break
    // opportunities.
    return false;
}

std::size_t panorama_next_breakable_position(
    std::string_view text, std::size_t start, char prior_last, char prior_second_to_last)
{
    // WebCore nextBreakablePosition: lastLastCharacter / lastCharacter seed from
    // the prior context when the scan starts at the string head.
    char last_last = start > 1 ? text[start - 2] : prior_second_to_last;
    char last = start > 0 ? text[start - 1] : prior_last;
    for (std::size_t i = start; i < text.size(); ++i)
    {
        const char c = text[i];
        if (panorama_is_breakable_space(c) || panorama_should_break_after(last_last, last, c))
        {
            return i;
        }
        last_last = last;
        last = c;
    }
    return text.size();
}

std::vector<PanoramaTextWrapLine> panorama_wrap_text_lines(
    const std::vector<PanoramaTextWrapRun>& runs, float available_width, const PanoramaTextWrapMeasure& measure)
{
    // Per-word advances vs the layout's full-string measure can drift by float
    // summation order; don't wrap on noise.
    constexpr float kFitEpsilon = 0.25F;

    std::vector<PanoramaTextWrapLine> lines;
    PanoramaTextWrapLine current;

    // Breakable spaces between words are held back until the next word commits
    // to the same line; a wrap collapses them (white-space: normal — they appear
    // on no line and contribute no width). Words accumulate segments because a
    // word may span styled runs with no break opportunity at the run boundary.
    std::vector<PanoramaTextWrapSegment> pending_spaces;
    float pending_spaces_width = 0.0F;
    std::vector<PanoramaTextWrapSegment> word;
    float word_width = 0.0F;

    const auto flush_word = [&]() {
        if (word.empty())
        {
            return;
        }
        if (!current.segments.empty() &&
            current.width + pending_spaces_width + word_width > available_width + kFitEpsilon)
        {
            // The word doesn't fit after what's already on the line: wrap. The
            // held spaces collapse at the break.
            lines.push_back(std::move(current));
            current = {};
        }
        else if (!current.segments.empty())
        {
            current.segments.insert(current.segments.end(), pending_spaces.begin(), pending_spaces.end());
            current.width += pending_spaces_width;
        }
        pending_spaces.clear();
        pending_spaces_width = 0.0F;
        current.segments.insert(current.segments.end(), word.begin(), word.end());
        current.width += word_width;
        word.clear();
        word_width = 0.0F;
    };

    // Pair-table context carried across spaces, newlines, and run boundaries
    // (WebCore's LazyLineBreakIterator priorContext).
    char prior_last = '\0';
    char prior_second_to_last = '\0';
    const auto consumed = [&](std::string_view text, std::size_t begin, std::size_t end) {
        if (end - begin >= 2)
        {
            prior_second_to_last = text[end - 2];
            prior_last = text[end - 1];
        }
        else if (end - begin == 1)
        {
            prior_second_to_last = prior_last;
            prior_last = text[end - 1];
        }
    };

    for (std::size_t r = 0; r < runs.size(); ++r)
    {
        const std::string_view text = runs[r].text;
        std::size_t i = 0;
        while (i < text.size())
        {
            const char c = text[i];
            if (c == '\n')
            {
                // Forced break: the line ends here even if empty (blank line).
                flush_word();
                pending_spaces.clear();
                pending_spaces_width = 0.0F;
                lines.push_back(std::move(current));
                current = {};
                consumed(text, i, i + 1);
                ++i;
                continue;
            }
            if (panorama_is_breakable_space(c))
            {
                flush_word();
                const std::size_t ws_begin = i;
                while (i < text.size() && text[i] != '\n' && panorama_is_breakable_space(text[i]))
                {
                    ++i;
                }
                if (!current.segments.empty())
                {
                    pending_spaces.push_back({static_cast<int>(r), ws_begin, i});
                    pending_spaces_width += measure(static_cast<int>(r), ws_begin, i);
                }
                // Leading spaces at a line start collapse (white-space: normal).
                consumed(text, ws_begin, i);
                continue;
            }

            // A run boundary inside a word: flush if a break is allowed before
            // this position given the prior run's trailing characters (e.g. the
            // previous run ended with '-').
            if (i == 0 && !word.empty() && panorama_should_break_after(prior_second_to_last, prior_last, c))
            {
                flush_word();
            }

            // Word chunk: [i, next break opportunity). Scanning from i+1 keeps
            // the chunk non-empty when a break is already allowed before i.
            const std::size_t next = i + 1 < text.size()
                ? panorama_next_breakable_position(text, i + 1, prior_last, prior_second_to_last)
                : text.size();
            word.push_back({static_cast<int>(r), i, next});
            word_width += measure(static_cast<int>(r), i, next);
            consumed(text, i, next);
            i = next;
            if (next < text.size() && !panorama_is_breakable_space(text[next]))
            {
                // Break allowed before text[next] without whitespace (pair
                // table, e.g. after '-'): the word is complete.
                flush_word();
            }
            // Otherwise the word may continue into the next run (or ends at a
            // space / newline handled above).
        }
    }

    flush_word();
    if (!current.segments.empty() || lines.empty())
    {
        lines.push_back(std::move(current));
    }
    return lines;
}

namespace
{
struct ArtifactIdentity
{
    std::string source_text;
    float font_size = 0.0F;
    int font_weight = 0;
    float letter_spacing = 0.0F;
    float line_height = 0.0F;
    float available_width = 0.0F;
    int text_transform = 0;
    int text_overflow = 0;
    bool html = false;
    bool wrap = false;
    std::uint64_t font_generation = 0;
};

struct ArtifactCacheEntry
{
    ArtifactIdentity identity;
    std::shared_ptr<PanoramaTextArtifact> artifact;
    std::size_t bytes = 0;
    std::uint64_t last_use = 0;
};

thread_local std::unordered_multimap<std::size_t, ArtifactCacheEntry> g_artifact_cache;
thread_local std::size_t g_artifact_cache_bytes = 0;
thread_local std::uint64_t g_artifact_clock = 0;
thread_local std::uint64_t g_artifact_version = 0;
thread_local PanoramaTextArtifactCacheBudgets g_artifact_budgets;
thread_local bool g_artifact_counters_enabled = false;
thread_local PanoramaTextArtifactStats g_artifact_stats;

void hash_combine(std::size_t& seed, std::size_t value)
{
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

std::size_t artifact_hash(const PanoramaTextArtifactRequest& request)
{
    std::size_t hash = std::hash<std::string_view>{}(request.source_text);
    hash_combine(hash, std::hash<float>{}(request.font_size));
    hash_combine(hash, std::hash<int>{}(request.font_weight));
    hash_combine(hash, std::hash<float>{}(request.letter_spacing));
    hash_combine(hash, std::hash<float>{}(request.line_height));
    hash_combine(hash, std::hash<float>{}(request.available_width));
    hash_combine(hash, std::hash<int>{}(request.text_transform));
    hash_combine(hash, std::hash<int>{}(request.text_overflow));
    hash_combine(hash, std::hash<bool>{}(request.html));
    hash_combine(hash, std::hash<bool>{}(request.wrap));
    hash_combine(hash, std::hash<std::uint64_t>{}(request.font_generation));
    return hash;
}

bool identity_matches(const ArtifactIdentity& identity, const PanoramaTextArtifactRequest& request)
{
    return identity.source_text == request.source_text && identity.font_size == request.font_size &&
        identity.font_weight == request.font_weight && identity.letter_spacing == request.letter_spacing &&
        identity.line_height == request.line_height && identity.available_width == request.available_width &&
        identity.text_transform == request.text_transform && identity.text_overflow == request.text_overflow &&
        identity.html == request.html && identity.wrap == request.wrap &&
        identity.font_generation == request.font_generation;
}

ArtifactIdentity own_identity(const PanoramaTextArtifactRequest& request)
{
    return {std::string(request.source_text), request.font_size, request.font_weight, request.letter_spacing,
        request.line_height, request.available_width, request.text_transform, request.text_overflow,
        request.html, request.wrap, request.font_generation};
}

std::shared_ptr<const PanoramaTextArtifact> find_artifact(
    const PanoramaTextArtifactRequest& request, bool count_miss)
{
    const std::size_t hash = artifact_hash(request);
    const auto [begin, end] = g_artifact_cache.equal_range(hash);
    for (auto it = begin; it != end; ++it)
    {
        if (identity_matches(it->second.identity, request))
        {
            it->second.last_use = ++g_artifact_clock;
            if (g_artifact_counters_enabled)
            {
                ++g_artifact_stats.hits;
            }
            return it->second.artifact;
        }
    }
    if (count_miss && g_artifact_counters_enabled)
    {
        ++g_artifact_stats.misses;
    }
    return {};
}

char32_t artifact_next_codepoint(std::string_view text, std::size_t& index)
{
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80U)
    {
        ++index;
        return lead;
    }
    int extra = (lead & 0xE0U) == 0xC0U ? 1 : (lead & 0xF0U) == 0xE0U ? 2 : (lead & 0xF8U) == 0xF0U ? 3 : 0;
    if (extra == 0 || index + static_cast<std::size_t>(extra) >= text.size())
    {
        ++index;
        return 0xFFFD;
    }
    char32_t codepoint = lead & (extra == 1 ? 0x1FU : extra == 2 ? 0x0FU : 0x07U);
    for (int i = 0; i < extra; ++i)
    {
        const unsigned char continuation = static_cast<unsigned char>(text[index + 1U + static_cast<std::size_t>(i)]);
        if ((continuation & 0xC0U) != 0x80U)
        {
            ++index;
            return 0xFFFD;
        }
        codepoint = (codepoint << 6U) | (continuation & 0x3FU);
    }
    index += static_cast<std::size_t>(extra) + 1U;
    return codepoint;
}

void evict_artifacts_to_budget()
{
    while ((!g_artifact_cache.empty() && g_artifact_cache.size() > g_artifact_budgets.entries) ||
        (!g_artifact_cache.empty() && g_artifact_cache_bytes > g_artifact_budgets.bytes))
    {
        const auto oldest = std::min_element(g_artifact_cache.begin(), g_artifact_cache.end(),
            [](const auto& a, const auto& b) { return a.second.last_use < b.second.last_use; });
        if (oldest == g_artifact_cache.end())
        {
            break;
        }
        g_artifact_cache_bytes -= oldest->second.bytes;
        g_artifact_cache.erase(oldest);
        if (g_artifact_counters_enabled)
        {
            ++g_artifact_stats.evictions;
        }
    }
}
}

void panorama_set_text_artifact_cache_budgets(PanoramaTextArtifactCacheBudgets budgets)
{
    g_artifact_budgets = budgets;
    evict_artifacts_to_budget();
}

PanoramaTextArtifactCacheBudgets panorama_text_artifact_cache_budgets()
{
    return g_artifact_budgets;
}

void panorama_enable_text_artifact_counters(bool enabled)
{
    g_artifact_counters_enabled = enabled;
}

void panorama_reset_text_artifact_counters()
{
    g_artifact_stats = {};
}

PanoramaTextArtifactStats panorama_text_artifact_stats()
{
    return g_artifact_stats;
}

float panorama_text_artifact_segment_width(
    const PanoramaTextArtifact& artifact, int run_index, std::size_t begin, std::size_t end)
{
    if (run_index < 0 || static_cast<std::size_t>(run_index) >= artifact.runs.size() || begin >= end)
    {
        return 0.0F;
    }
    const PanoramaTextArtifactRun& run = artifact.runs[static_cast<std::size_t>(run_index)];
    float width = 0.0F;
    bool first = true;
    for (const PanoramaTextShapedGlyph& glyph : run.glyphs)
    {
        if (glyph.begin < begin || glyph.end > end)
        {
            continue;
        }
        width += glyph.advance - (first ? glyph.leading_kerning : 0.0F);
        first = false;
    }
    return width;
}

float panorama_text_artifact_prefix_width(
    const PanoramaTextArtifact& artifact, int run_index, std::size_t byte_offset)
{
    if (g_artifact_counters_enabled)
    {
        ++g_artifact_stats.prefix_queries;
    }
    if (run_index < 0 || static_cast<std::size_t>(run_index) >= artifact.runs.size())
    {
        return 0.0F;
    }
    const PanoramaTextArtifactRun& run = artifact.runs[static_cast<std::size_t>(run_index)];
    float width = 0.0F;
    for (const PanoramaTextShapedGlyph& glyph : run.glyphs)
    {
        if (glyph.end > byte_offset)
        {
            break;
        }
        width += glyph.advance;
    }
    return width;
}

std::shared_ptr<const PanoramaTextArtifact> panorama_find_text_artifact(
    const PanoramaTextArtifactRequest& request)
{
    return find_artifact(request, true);
}

std::shared_ptr<const PanoramaTextArtifact> panorama_prepare_text_artifact(
    const PanoramaTextArtifactRequest& request, const PanoramaTextShapeRun& shape)
{
    if (std::shared_ptr<const PanoramaTextArtifact> cached = find_artifact(request, true))
    {
        return cached;
    }
    auto artifact = std::make_shared<PanoramaTextArtifact>();
    artifact->version = ++g_artifact_version;
    artifact->font_generation = request.font_generation;
    std::shared_ptr<const PanoramaTextArtifact> shaped_base;
    if (request.wrap)
    {
        PanoramaTextArtifactRequest base_request = request;
        base_request.available_width = -1.0F;
        base_request.wrap = false;
        shaped_base = panorama_prepare_text_artifact(base_request, shape);
    }
    if (shaped_base)
    {
        artifact->display_text = shaped_base->display_text;
        artifact->runs = shaped_base->runs;
        artifact->width = shaped_base->width;
        artifact->height = shaped_base->height;
    }
    else
    {
        artifact->display_text.assign(request.display_text);
        artifact->height = request.font_size * 1.2F;
        artifact->runs.reserve(request.runs.size());
    }

    const char* display_begin = request.display_text.data();
    const char* display_end = display_begin + request.display_text.size();
    for (const PanoramaTextArtifactRunInput& input : shaped_base
            ? std::span<const PanoramaTextArtifactRunInput>{}
            : request.runs)
    {
        PanoramaTextArtifactRun run;
        if (input.text.data() >= display_begin && input.text.data() <= display_end &&
            input.text.data() + input.text.size() <= display_end)
        {
            run.display_begin = static_cast<std::size_t>(input.text.data() - display_begin);
            run.display_end = run.display_begin + input.text.size();
        }
        run.font_weight = input.font_weight;
        run.italic = input.italic;
        float run_height = 0.0F;
        if (shape)
        {
            run_height = shape(input.text, request.font_size, input.font_weight, request.letter_spacing, run.glyphs);
        }
        else
        {
            std::size_t offset = 0;
            while (offset < input.text.size())
            {
                const std::size_t begin = offset;
                const char32_t codepoint = artifact_next_codepoint(input.text, offset);
                run.glyphs.push_back(
                    {codepoint, begin, offset, request.font_size * 0.5F + request.letter_spacing, 0.0F});
            }
            run_height = request.font_size * 1.2F;
        }
        run.prefix_advances.reserve(run.glyphs.size() + 1U);
        run.prefix_advances.push_back(0.0F);
        for (const PanoramaTextShapedGlyph& glyph : run.glyphs)
        {
            run.width += glyph.advance;
            run.prefix_advances.push_back(run.width);
        }
        artifact->width += run.width;
        artifact->height = std::max(artifact->height, run_height);
        if (g_artifact_counters_enabled)
        {
            ++g_artifact_stats.shaped_runs;
            g_artifact_stats.shaped_glyphs += run.glyphs.size();
        }
        artifact->runs.push_back(std::move(run));
    }

    if (request.wrap && request.available_width > 0.0F)
    {
        std::vector<PanoramaTextWrapRun> wrap_runs;
        wrap_runs.reserve(artifact->runs.size());
        for (const PanoramaTextArtifactRun& run : artifact->runs)
        {
            wrap_runs.push_back({std::string_view(artifact->display_text).substr(
                                     run.display_begin, run.display_end - run.display_begin),
                run.font_weight});
        }
        artifact->lines = panorama_wrap_text_lines(wrap_runs, request.available_width,
            [&](int run, std::size_t begin, std::size_t end) {
                return panorama_text_artifact_segment_width(*artifact, run, begin, end);
            });
        if (artifact->lines.size() <= 1)
        {
            artifact->lines.clear();
        }
        if (g_artifact_counters_enabled)
        {
            ++g_artifact_stats.wrap_passes;
        }
    }
    artifact->line_advance = request.line_height > 0.0F ? request.line_height : artifact->height;

    std::size_t bytes = sizeof(PanoramaTextArtifact) + artifact->display_text.capacity();
    for (const PanoramaTextArtifactRun& run : artifact->runs)
    {
        bytes += sizeof(PanoramaTextArtifactRun) +
            run.glyphs.capacity() * sizeof(PanoramaTextShapedGlyph) +
            run.prefix_advances.capacity() * sizeof(float);
    }
    for (const PanoramaTextWrapLine& line : artifact->lines)
    {
        bytes += sizeof(PanoramaTextWrapLine) +
            line.segments.capacity() * sizeof(PanoramaTextWrapSegment);
    }
    if (g_artifact_counters_enabled)
    {
        ++g_artifact_stats.builds;
    }
    if (g_artifact_budgets.entries != 0 && g_artifact_budgets.bytes != 0)
    {
        const std::size_t hash = artifact_hash(request);
        ArtifactCacheEntry entry{own_identity(request), artifact, bytes, ++g_artifact_clock};
        g_artifact_cache.emplace(hash, std::move(entry));
        g_artifact_cache_bytes += bytes;
        evict_artifacts_to_budget();
    }
    return artifact;
}
}
