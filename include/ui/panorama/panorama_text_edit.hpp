#pragma once

#include "ui/panorama/panorama_dom.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// Single-line text-entry editing for Panorama <TextEntry> controls. This is a
// faithful port of WebCore's editing semantics restricted to one line:
//   - WebCore/editing/FrameSelection.cpp modify() drives caret movement;
//   - WebCore/editing/EditorCommand.cpp maps each command to a (granularity,
//     direction, alteration) triple (MoveLeft, MoveWordRight, DeleteBackward, ...);
//   - WebCore/html/TextFieldInputType.cpp sanitizeValue()/handleBeforeTextInserted
//     enforces the single-line constraint (line breaks stripped/replaced) and the
//     maxlength cap (counted in user-perceived characters).
//
// The field value lives in `node.text`; the selection is the closed byte range
// [min, max] of `node.text_caret` and `node.text_selection_anchor`, collapsed to a
// bare caret when they coincide (WebCore selectionStart == selectionEnd). Offsets
// are always kept on UTF-8 codepoint boundaries. Every mutator returns true when
// the value or selection actually changed, so the caller can recompute styles and
// fire `ontextentrychange` plus the legacy `ontextentrychanged` alias.
namespace panorama
{
// WebCore TextGranularity, restricted to the units a single line needs.
enum class PanoramaTextGranularity : std::uint8_t
{
    Character,    // CharacterGranularity
    Word,         // WordGranularity
    LineBoundary, // LineBoundary (Home/End — start/end of the single line)
};

// WebCore SelectionDirection. In an LTR single-line field Left == Backward and
// Right == Forward; the input layer maps the arrow keys accordingly.
enum class PanoramaTextDirection : std::uint8_t
{
    Backward,
    Forward,
};

// True for a node the editing engine operates on (a <TextEntry>).
[[nodiscard]] bool panorama_node_is_text_entry(const PanoramaNode& node);

// selectionStart / selectionEnd (HTMLTextFormControlElement): byte offsets, the
// min/max of caret and anchor.
[[nodiscard]] int panorama_text_entry_selection_start(const PanoramaNode& node);
[[nodiscard]] int panorama_text_entry_selection_end(const PanoramaNode& node);
[[nodiscard]] bool panorama_text_entry_has_selection(const PanoramaNode& node);

// Clamps caret + anchor into [0, text.size()] and snaps them onto codepoint
// boundaries. Call after any external mutation of `text` (e.g. JS `panel.text =`).
void panorama_text_entry_clamp_selection(PanoramaNode& node);

// HTMLTextFormControlElement::setSelectionRange: start/end clamped and ordered;
// anchor = start, caret = end (forward direction).
bool panorama_text_entry_set_selection(PanoramaNode& node, int start, int end);

// Editor "SelectAll" (anchor = 0, caret = len).
bool panorama_text_entry_select_all(PanoramaNode& node);

// Places a collapsed caret at the end of the value and clears the selection. Used
// when a field gains focus from a click and on programmatic value sets.
bool panorama_text_entry_collapse_to_end(PanoramaNode& node);

// TextFieldInputType::sanitizeValue + setRangeText: replaces the field value,
// stripping line breaks, then collapses the caret to the end.
bool panorama_text_entry_set_value(PanoramaNode& node, std::string_view utf8);

// TypingCommand::insertText through handleBeforeTextInsertedEvent: replaces the
// current selection with `utf8` (line breaks become spaces), honouring the
// `maxchars` cap, then collapses the caret after the inserted run.
bool panorama_text_entry_insert(PanoramaNode& node, std::string_view utf8);

// FrameSelection::deleteKeyPressed / forwardDeleteKeyPressed. With a range
// selection both just delete the selection (WebCore). Otherwise Backspace removes
// the unit before the caret and Delete the unit after it; `granularity` selects
// Character (Backspace/Delete) vs Word (Ctrl+Backspace/Ctrl+Delete).
bool panorama_text_entry_delete(
    PanoramaNode& node, PanoramaTextDirection direction, PanoramaTextGranularity granularity);

// FrameSelection::modify(alter, direction, granularity). `extend` is the
// Shift-held AlterationExtend (move the caret, keep the anchor); otherwise
// AlterationMove (a range collapses to the edge in `direction`; a bare caret
// advances one unit). Returns true when caret/anchor moved.
bool panorama_text_entry_move(
    PanoramaNode& node, PanoramaTextDirection direction, PanoramaTextGranularity granularity, bool extend);
}
