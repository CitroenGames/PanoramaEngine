#include "ui/panorama/panorama_text_edit.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <unordered_map>

namespace panorama
{
namespace
{
thread_local bool g_text_edit_counters_enabled = false;
thread_local PanoramaTextEditStats g_text_edit_stats;

struct TextMetadata
{
    const char* data = nullptr;
    std::size_t size = 0;
    int codepoints = 0;
    std::uint64_t last_use = 0;
};

thread_local std::unordered_map<const PanoramaNode*, TextMetadata> g_text_metadata;
thread_local std::uint64_t g_text_metadata_clock = 0;
constexpr std::size_t kTextMetadataBudget = 2048;

void record_scan(std::size_t bytes, bool retained)
{
    if (!g_text_edit_counters_enabled)
    {
        return;
    }
    if (retained)
    {
        g_text_edit_stats.retained_bytes_scanned += bytes;
    }
    else
    {
        g_text_edit_stats.inserted_bytes_scanned += bytes;
    }
}

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

int count_codepoints(std::string_view text, bool retained = false)
{
    record_scan(text.size(), retained);
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

void store_metadata(const PanoramaNode& node, int codepoints)
{
    if (g_text_metadata.size() >= kTextMetadataBudget && g_text_metadata.find(&node) == g_text_metadata.end())
    {
        const auto oldest = std::min_element(g_text_metadata.begin(), g_text_metadata.end(),
            [](const auto& a, const auto& b) { return a.second.last_use < b.second.last_use; });
        if (oldest != g_text_metadata.end())
        {
            g_text_metadata.erase(oldest);
        }
    }
    TextMetadata& metadata = g_text_metadata[&node];
    metadata.data = node.text.data();
    metadata.size = node.text.size();
    metadata.codepoints = codepoints;
    metadata.last_use = ++g_text_metadata_clock;
}

int text_codepoints(const PanoramaNode& node)
{
    const auto found = g_text_metadata.find(&node);
    if (found != g_text_metadata.end() && found->second.data == node.text.data() &&
        found->second.size == node.text.size())
    {
        found->second.last_use = ++g_text_metadata_clock;
        if (g_text_edit_counters_enabled)
        {
            ++g_text_edit_stats.metadata_hits;
        }
        return found->second.codepoints;
    }
    if (g_text_edit_counters_enabled)
    {
        ++g_text_edit_stats.metadata_misses;
    }
    const int result = count_codepoints(node.text, true);
    store_metadata(node, result);
    return result;
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

void clamp_selection_without_metadata_refresh(PanoramaNode& node)
{
    node.text_caret = clamp_to_boundary_down(node.text, node.text_caret);
    node.text_selection_anchor = clamp_to_boundary_down(node.text, node.text_selection_anchor);
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

void panorama_enable_text_edit_counters(bool enabled) noexcept
{
    g_text_edit_counters_enabled = enabled;
}

void panorama_reset_text_edit_counters() noexcept
{
    g_text_edit_stats = {};
}

PanoramaTextEditStats panorama_text_edit_stats() noexcept
{
    return g_text_edit_stats;
}

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
    clamp_selection_without_metadata_refresh(node);
    // This public hook is the documented synchronization point after direct
    // external writes to node.text, including same-size writes that pointer/
    // size validation alone cannot detect.
    store_metadata(node, count_codepoints(node.text, true));
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
    store_metadata(node, count_codepoints(node.text));
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
    clamp_selection_without_metadata_refresh(node);
    const int start = panorama_text_entry_selection_start(node);
    const int end = panorama_text_entry_selection_end(node);

    std::string insertion_storage;
    std::string_view insertion = utf8;
    const bool needs_normalization = utf8.find_first_of("\r\n") != std::string_view::npos;
    const char* value_begin = node.text.data();
    const char* value_end = value_begin + node.text.size();
    const bool aliases_value = !utf8.empty() && utf8.data() >= value_begin && utf8.data() < value_end;
    if (needs_normalization)
    {
        insertion_storage = newlines_to_spaces(utf8);
        insertion = insertion_storage;
    }
    else if (aliases_value)
    {
        // replace()/reserve() may invalidate a view into the value itself.
        insertion_storage.assign(utf8);
        insertion = insertion_storage;
    }
    const int selected_codepoints =
        count_codepoints(std::string_view(node.text).substr(static_cast<std::size_t>(start),
                             static_cast<std::size_t>(end - start)),
            true);
    const int retained_codepoints = text_codepoints(node) - selected_codepoints;
    const int cap = max_chars(node);
    if (cap > 0)
    {
        insertion = truncate_codepoints(insertion, std::max(0, cap - retained_codepoints));
    }

    const bool text_changed = start != end || !insertion.empty();
    if (text_changed)
    {
        const std::size_t old_capacity = node.text.capacity();
        const std::size_t next_size =
            node.text.size() - static_cast<std::size_t>(end - start) + insertion.size();
        if (next_size > old_capacity)
        {
            node.text.reserve(next_size);
            if (g_text_edit_counters_enabled)
            {
                ++g_text_edit_stats.capacity_growths;
            }
        }
        if (g_text_edit_counters_enabled)
        {
            ++g_text_edit_stats.edits;
            ++g_text_edit_stats.in_place_edits;
            g_text_edit_stats.bytes_moved += node.text.size() - static_cast<std::size_t>(end);
        }
        node.text.replace(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start),
            insertion.data(), insertion.size());
    }
    const int caret = start + static_cast<int>(insertion.size());
    const bool sel_changed = assign_selection(node, caret, caret);
    store_metadata(node, retained_codepoints + count_codepoints(insertion));
    return text_changed || sel_changed;
}

bool panorama_text_entry_delete(
    PanoramaNode& node, PanoramaTextDirection direction, PanoramaTextGranularity granularity)
{
    if (!panorama_node_is_text_entry(node))
    {
        return false;
    }
    clamp_selection_without_metadata_refresh(node);

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

    const int removed_codepoints =
        count_codepoints(std::string_view(node.text).substr(static_cast<std::size_t>(from),
                             static_cast<std::size_t>(to - from)),
            true);
    const int before_codepoints = text_codepoints(node);
    if (g_text_edit_counters_enabled)
    {
        ++g_text_edit_stats.edits;
        ++g_text_edit_stats.in_place_edits;
        g_text_edit_stats.bytes_moved += node.text.size() - static_cast<std::size_t>(to);
    }
    node.text.erase(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
    store_metadata(node, before_codepoints - removed_codepoints);
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
    clamp_selection_without_metadata_refresh(node);

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
