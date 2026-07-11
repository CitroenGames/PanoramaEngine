#include "ui/panorama/panorama_text_edit.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace openstrike
{
namespace
{
// ---- UTF-8 boundary helpers --------------------------------------------------
// Offsets into the value are always kept on codepoint boundaries, so caret motion
// never splits a multi-byte sequence (WebCore operates on full characters).

bool is_continuation(unsigned char b)
{
    return (b & 0xC0U) == 0x80U;
}

int clamp_to_boundary_down(std::string_view text, int pos)
{
    const int n = static_cast<int>(text.size());
    pos = std::clamp(pos, 0, n);
    while (pos > 0 && pos < n && is_continuation(static_cast<unsigned char>(text[static_cast<std::size_t>(pos)])))
    {
        --pos;
    }
    return pos;
}

int next_boundary(std::string_view text, int pos)
{
    const int n = static_cast<int>(text.size());
    if (pos >= n)
    {
        return n;
    }
    ++pos;
    while (pos < n && is_continuation(static_cast<unsigned char>(text[static_cast<std::size_t>(pos)])))
    {
        ++pos;
    }
    return pos;
}

int prev_boundary(std::string_view text, int pos)
{
    if (pos <= 0)
    {
        return 0;
    }
    --pos;
    while (pos > 0 && is_continuation(static_cast<unsigned char>(text[static_cast<std::size_t>(pos)])))
    {
        --pos;
    }
    return pos;
}

int count_codepoints(std::string_view text)
{
    int count = 0;
    for (char c : text)
    {
        if (!is_continuation(static_cast<unsigned char>(c)))
        {
            ++count;
        }
    }
    return count;
}

// Truncates `text` to at most `max_codepoints` codepoints, returning the byte view.
std::string_view truncate_codepoints(std::string_view text, int max_codepoints)
{
    if (max_codepoints <= 0)
    {
        return {};
    }
    int count = 0;
    int pos = 0;
    while (pos < static_cast<int>(text.size()))
    {
        pos = next_boundary(text, pos);
        if (++count >= max_codepoints)
        {
            return text.substr(0, static_cast<std::size_t>(pos));
        }
    }
    return text;
}

// Word classification, approximating WebCore's ICU word break: ASCII letters and
// digits are word characters; everything below 0x80 that is not alnum (spaces,
// punctuation) is a separator; bytes >= 0x80 (the lead/continuation of non-ASCII
// codepoints, i.e. letters in most scripts) count as word characters.
bool byte_starts_word_char(std::string_view text, int pos)
{
    if (pos < 0 || pos >= static_cast<int>(text.size()))
    {
        return false;
    }
    const unsigned char b = static_cast<unsigned char>(text[static_cast<std::size_t>(pos)]);
    if (b >= 0x80U)
    {
        return true;
    }
    return std::isalnum(static_cast<int>(b)) != 0;
}

// rightWordPosition on Windows (EditingBehavior::shouldSkipSpaceWhenMovingRight()
// == true): skip the remainder of the current word, then the following
// separators, landing on the start of the next word.
int forward_word(std::string_view text, int pos)
{
    const int n = static_cast<int>(text.size());
    while (pos < n && byte_starts_word_char(text, pos))
    {
        pos = next_boundary(text, pos);
    }
    while (pos < n && !byte_starts_word_char(text, pos))
    {
        pos = next_boundary(text, pos);
    }
    return pos;
}

// leftWordPosition: skip separators before the caret, then the word characters,
// landing on the start of the current/previous word.
int backward_word(std::string_view text, int pos)
{
    while (pos > 0 && !byte_starts_word_char(text, prev_boundary(text, pos)))
    {
        pos = prev_boundary(text, pos);
    }
    while (pos > 0 && byte_starts_word_char(text, prev_boundary(text, pos)))
    {
        pos = prev_boundary(text, pos);
    }
    return pos;
}

// TextFieldInputType::sanitizeValue: a single-line field holds no line breaks.
std::string strip_line_breaks(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        if (c != '\r' && c != '\n')
        {
            out.push_back(c);
        }
    }
    return out;
}

// handleBeforeTextInsertedEvent: inserted text keeps its glyphs but line breaks
// become spaces (so a pasted multi-line string collapses onto one line).
std::string newlines_to_spaces(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '\r')
        {
            out.push_back(' ');
            if (i + 1 < text.size() && text[i + 1] == '\n')
            {
                ++i; // CRLF -> single space
            }
        }
        else if (c == '\n')
        {
            out.push_back(' ');
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

// The `maxchars` cap (Valve TextEntry attribute; WebCore maxlength), 0/absent = no
// limit. Counted in characters, like HTMLInputElement::maxEffectiveLength.
int max_chars(const PanoramaNode& node)
{
    const auto it = node.attributes.find("maxchars");
    if (it == node.attributes.end())
    {
        return 0;
    }
    const int value = std::atoi(it->second.c_str());
    return value > 0 ? value : 0;
}

// Writes caret/anchor back to the node, returning whether either changed.
bool assign_selection(PanoramaNode& node, int caret, int anchor)
{
    const bool changed = node.text_caret != caret || node.text_selection_anchor != anchor;
    node.text_caret = caret;
    node.text_selection_anchor = anchor;
    return changed;
}

// Logical caret advance by one unit in `direction` at `granularity`.
int advance_position(std::string_view text, int pos, PanoramaTextDirection direction, PanoramaTextGranularity gran)
{
    switch (gran)
    {
    case PanoramaTextGranularity::Character:
        return direction == PanoramaTextDirection::Backward ? prev_boundary(text, pos) : next_boundary(text, pos);
    case PanoramaTextGranularity::Word:
        return direction == PanoramaTextDirection::Backward ? backward_word(text, pos) : forward_word(text, pos);
    case PanoramaTextGranularity::LineBoundary:
        return direction == PanoramaTextDirection::Backward ? 0 : static_cast<int>(text.size());
    }
    return pos;
}
} // namespace

bool panorama_node_is_text_entry(const PanoramaNode& node)
{
    return node.tag_lower == "textentry";
}

int panorama_text_entry_selection_start(const PanoramaNode& node)
{
    return std::min(node.text_caret, node.text_selection_anchor);
}

int panorama_text_entry_selection_end(const PanoramaNode& node)
{
    return std::max(node.text_caret, node.text_selection_anchor);
}

bool panorama_text_entry_has_selection(const PanoramaNode& node)
{
    return node.text_caret != node.text_selection_anchor;
}

void panorama_text_entry_clamp_selection(PanoramaNode& node)
{
    node.text_caret = clamp_to_boundary_down(node.text, node.text_caret);
    node.text_selection_anchor = clamp_to_boundary_down(node.text, node.text_selection_anchor);
}

bool panorama_text_entry_set_selection(PanoramaNode& node, int start, int end)
{
    if (!panorama_node_is_text_entry(node))
    {
        return false;
    }
    start = clamp_to_boundary_down(node.text, start);
    end = clamp_to_boundary_down(node.text, end);
    if (start > end)
    {
        std::swap(start, end);
    }
    return assign_selection(node, end, start); // forward: anchor = start, caret = end
}

bool panorama_text_entry_select_all(PanoramaNode& node)
{
    if (!panorama_node_is_text_entry(node))
    {
        return false;
    }
    return assign_selection(node, static_cast<int>(node.text.size()), 0);
}

bool panorama_text_entry_collapse_to_end(PanoramaNode& node)
{
    if (!panorama_node_is_text_entry(node))
    {
        return false;
    }
    const int end = static_cast<int>(node.text.size());
    return assign_selection(node, end, end);
}

bool panorama_text_entry_set_value(PanoramaNode& node, std::string_view utf8)
{
    if (!panorama_node_is_text_entry(node))
    {
        return false;
    }
    std::string sanitized = strip_line_breaks(utf8);
    const int cap = max_chars(node);
    if (cap > 0)
    {
        sanitized = std::string(truncate_codepoints(sanitized, cap));
    }
    const bool text_changed = node.text != sanitized;
    node.text = std::move(sanitized);
    const int end = static_cast<int>(node.text.size());
    const bool sel_changed = assign_selection(node, end, end);
    return text_changed || sel_changed;
}

bool panorama_text_entry_insert(PanoramaNode& node, std::string_view utf8)
{
    if (!panorama_node_is_text_entry(node))
    {
        return false;
    }
    panorama_text_entry_clamp_selection(node);
    const int start = panorama_text_entry_selection_start(node);
    const int end = panorama_text_entry_selection_end(node);

    const std::string prefix = node.text.substr(0, static_cast<std::size_t>(start));
    const std::string suffix = node.text.substr(static_cast<std::size_t>(end));

    std::string insertion = newlines_to_spaces(utf8);
    const int cap = max_chars(node);
    if (cap > 0)
    {
        const int kept = count_codepoints(prefix) + count_codepoints(suffix);
        insertion = std::string(truncate_codepoints(insertion, std::max(0, cap - kept)));
    }

    std::string next = prefix + insertion + suffix;
    const bool text_changed = node.text != next;
    node.text = std::move(next);
    const int caret = start + static_cast<int>(insertion.size());
    const bool sel_changed = assign_selection(node, caret, caret);
    return text_changed || sel_changed;
}

bool panorama_text_entry_delete(
    PanoramaNode& node, PanoramaTextDirection direction, PanoramaTextGranularity granularity)
{
    if (!panorama_node_is_text_entry(node))
    {
        return false;
    }
    panorama_text_entry_clamp_selection(node);

    int from = panorama_text_entry_selection_start(node);
    int to = panorama_text_entry_selection_end(node);
    if (from == to)
    {
        // Collapsed caret: the delete defines the range itself (WebCore
        // deleteKeyPressed / forwardDeleteKeyPressed extend by one unit).
        const int caret = node.text_caret;
        if (direction == PanoramaTextDirection::Backward)
        {
            from = advance_position(node.text, caret, PanoramaTextDirection::Backward, granularity);
            to = caret;
        }
        else
        {
            from = caret;
            to = advance_position(node.text, caret, PanoramaTextDirection::Forward, granularity);
        }
    }
    if (from >= to)
    {
        return false;
    }

    node.text = node.text.substr(0, static_cast<std::size_t>(from)) + node.text.substr(static_cast<std::size_t>(to));
    assign_selection(node, from, from);
    return true;
}

bool panorama_text_entry_move(
    PanoramaNode& node, PanoramaTextDirection direction, PanoramaTextGranularity granularity, bool extend)
{
    if (!panorama_node_is_text_entry(node))
    {
        return false;
    }
    panorama_text_entry_clamp_selection(node);

    if (!extend)
    {
        // AlterationMove. A range selection moved by Character collapses to the
        // directional edge without advancing (WebCore modifyMovingLeft/Right
        // return the selection boundary). Word/line moves compute from the caret.
        if (panorama_text_entry_has_selection(node) && granularity == PanoramaTextGranularity::Character)
        {
            const int edge = direction == PanoramaTextDirection::Backward
                ? panorama_text_entry_selection_start(node)
                : panorama_text_entry_selection_end(node);
            return assign_selection(node, edge, edge);
        }
        const int pos = advance_position(node.text, node.text_caret, direction, granularity);
        return assign_selection(node, pos, pos);
    }

    // AlterationExtend: move the caret (focus), keep the anchor fixed.
    const int pos = advance_position(node.text, node.text_caret, direction, granularity);
    return assign_selection(node, pos, node.text_selection_anchor);
}
}
