#include "ui/panorama/panorama_text_break.hpp"

#include <cstdint>

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
}
