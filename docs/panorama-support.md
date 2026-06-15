# Panorama Support

This page summarizes the supported Panorama feature set and the known gaps. It
is not a full CSS reference; it is a practical map for hosts and contributors.

## Documents And Resources

Supported:

- Panorama XML documents parsed into `PanoramaNode`.
- `<styles>` and `<scripts>` includes.
- `<Frame>` expansion with bounded recursion.
- `<snippets>` collection and instantiation.
- Initial document load and runtime sublayout load helpers.
- Memory, directory, and `.pbin` package resource providers.
- `#token` localization through `PanoramaLocalization`.

Package limits:

- `.pbin` package entries must be stored zip entries.
- Compressed entries and zip data descriptors are rejected.

## CSS Cascade And Selectors

Supported selector features include:

- Type, universal, id, class, and attribute selectors.
- Descendant, child, adjacent sibling, and general sibling combinators.
- `:not(...)` selector groups.
- Specificity and source-order cascade.
- Inline styles.
- Layout-scoped stylesheets for sublayouts.

Supported pseudo-classes:

- `:hover`
- `:active`
- `:selected`
- `:enabled`
- `:disabled`
- `:focus`
- `:focus-within`
- `:root`

Unsupported pseudo-classes do not match.

Supported stylesheet features include:

- `@define` variable substitution.
- CSS custom properties (`--name`) and `var(--name, fallback)`.
- `@keyframes`.
- Transition and animation longhands used by the runtime.

## Layout

Supported layout primitives include:

- `flow-children: none`, `right`, `left`, `down`, `up`, `right-wrap`, and
  `down-wrap`.
- `width`, `height`, `min-*`, `max-*`.
- `fit-children`, `fill-parent-flow(N)`, percent lengths,
  `width-percentage(N%)`, and `height-percentage(N%)`.
- `margin`, `padding`, borders, and per-side borders.
- `horizontal-align`, `vertical-align`, and `align`.
- Panorama `position: x y z` plus `x`, `y`, and `z` longhands.
- `overflow` values for clipping, squish, scroll, and visible behavior.
- Overlay scrollbar geometry and scroll offsets.
- Closed dropdown presentation and open dropdown popup geometry.

Text layout:

- `font-size`, `font-weight`, `font-style`, `letter-spacing`, `line-height`,
  `text-align`, `text-transform`, `white-space`, and `text-overflow`.
- Labels wrap to resolved content width by default.
- `white-space: nowrap` opts out of wrapping.
- `text-overflow: ellipsis`, `shrink`, `clip`, and `noclip` are represented.
- Inline label markup supports the common `<b>` and `<i>` tags when
  `html="true"`.

Text wrapping follows the in-repo WebCore-style break helper: breakable spaces,
selected ASCII punctuation opportunities, and `\n` forced breaks. There is no
ICU, hyphenation, CJK line breaking, or break-anywhere fallback.

## Styling And Paint

Supported paint/style features include:

- Solid backgrounds and Panorama/CSS-style gradients.
- Background images, opacity, size, position, repeat, and wash/brightness.
- Border color, width, per-side borders, and border radius.
- Text shadows, image shadows, and box shadows.
- `opacity`.
- `z-index` ordering within the painter.
- `transform`, `transform-origin`, and `pre-transform-scale2d`.
- `-mix-blend-mode` mapped to `PanoramaBlendMode`.
- `blur: gaussian(...)` and `fastgaussian(...)` as backdrop blur commands.
- `clip: rect(...)` and `clip: radial(...)` as render-time clips.

Draw-list notes:

- Paint output is `PanoramaDrawList`, not immediate GPU calls.
- Text requires a host `PanoramaGlyphSource`; without it, text is skipped and
  boxes still paint.
- Colors are straight RGBA.
- Backdrop blur commands contain no geometry and must be handled by the host
  renderer.

## Animation

Supported CSS transitions and keyframes cover:

- `opacity`
- `position`, `x`, `y`, `z`
- Colors, including background, foreground, and wash color
- Brightness
- `transform`
- `width` and `height`
- Borders
- `box-shadow`
- `blur`
- `clip`
- `pre-transform-scale2d`

`border-radius` is supported for painting but is not currently animated.

Transition completion is reported through
`PanoramaAnimationAdvanceResult::transition_ends`. Hosts should forward each
entry to `PanoramaRuntime::dispatch_property_transition_end`.

Smooth scroll animations are advanced separately with
`panorama_advance_scroll_animations`.

## Runtime And Scripting

Implemented runtime surface includes:

- `$` selector lookup.
- `$.Msg`, `$.Warning`, and `$.Localize`.
- `$.GetContextPanel`.
- `$.CreatePanel`.
- `$.RegisterEventHandler` and `$.RegisterForUnhandledEvent`.
- `$.DispatchEvent`, `$.DispatchEventAsync`, `$.Schedule`, and
  `$.CancelScheduled`.
- Panel class helpers such as `AddClass`, `RemoveClass`, `ToggleClass`,
  `SetHasClass`, `BHasClass`, and `SwitchClass`.
- Panel traversal and mutation helpers such as `FindChild`, `GetChild`,
  `Children`, `SetParent`, `MoveChildBefore`, `MoveChildAfter`,
  `RemoveAndDeleteChildren`, and `DeleteAsync`.
- Panel attributes, text, visibility, enabled, selected, checked, group,
  tooltip, style setters, and `Data()`.
- Dropdown, radio, slider, scroll, image, movie, sound, and panel-event helper
  methods used by shipped layouts.
- `BLoadLayout`, `LoadLayout`, `LoadLayoutAsync`, `BLoadLayoutSnippet`, and
  `BCreateChildren` through host loaders.

Game-backed CS:GO API namespaces are intentionally partial. The bootstrap
installs graceful stubs for most namespaces so shipped scripts can continue
running without throwing. Selected actions from `GameInterfaceAPI`,
`LobbyAPI`, and `GameTypesAPI` can be bridged to the host through
`set_host_action_handler`.

## Input

`PanoramaInputController` supports:

- Front-to-back hit testing.
- Open dropdown popup hit testing above normal page content.
- `:hover`, `:active`, `:focus`, and related dirty marking.
- `onmouseover`, `onmouseout`, and bubbled `onactivate` handlers.
- Radio-group exclusivity.
- Dropdown open, select, and dismiss behavior.
- Wheel propagation to the innermost scrollable ancestor that can move.
- Scrollbar thumb dragging.
- Slider dragging.

Hosts pass design-space pointer coordinates and button state. The controller
observes node destruction and clears stale pointers automatically; call
`reset()` when replacing the whole tree.

## Threading

DOM, runtime, input, style mutation, and lifetime observer state are
single-threaded. Build resources or decode images on other threads only if the
resulting DOM/runtime calls are marshaled back to the UI thread.
