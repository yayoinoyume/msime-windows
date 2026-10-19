# UI ownership

| Surface | Renderer | Owner |
|---|---|---|
| Candidate, floating toolbar, tray menu | msimeui native Direct2D/DirectWrite | `src/window/` presenters in the Server process |
| Settings | msimeui native Direct2D/DirectWrite | `src/settings/native/` in the separate `MetasequoiaImeSettings.exe` |
| Keyboard, handwriting, emoji and voice panels | Their existing native application/presenter | Shared actions/configuration stay in Server; panels do not own input sessions |

Direct2D is the only renderer. The WebView2 small windows, the WebView2 settings host and
the `ui-html/` pages have been removed, together with the `appearance.ui_backend` and
`appearance.settings_ui_backend` configuration keys; existing values are dropped by the
template merge on upgrade.

The candidate worker resolves output text, auxiliary codes, source badges and
translations once into `CandidateViewItem`, and the native renderer consumes these
values as plain text. Clicks enter the existing `EnqueueCandidateUiAction` queue with the
focus owner checks and action policy. The ten-entry mouse range is distinct from the
1–9 keyboard shortcuts. New business behavior belongs in shared actions/session/view
models, not in a presenter.

## Settings window

`MetasequoiaImeSettings.exe` draws its window with msimeui in `src/settings/native/`.
The window speaks the message contract in `engine/contracts/webview/` (the name is
historical): it receives `configSnapshot` and the response messages as JSON and posts
`configUpdate` and the request messages through `HandleClientMessage` in
`settings_app.cpp`, so config paths are mapped in one place. Its image assets live in
`server/assets/settings/` and are installed to `<data dir>\assets\settings\`.

The pages were ported one to one from the former HTML/CSS settings page: each page
builder in `native_page_*.cpp` names the partial and module it came from, and the
controls in `native_settings_controls.*` name the CSS rule they mirror. Those sources
remain in git history under `ui-html/webview2/settings/ime-settings/src/`.

Sciter is not an active product backend. The msimeui library owns generic controls and
rendering, never Server configuration, pipe sessions, dictionary access, or global
input state.

## Candidate window above the caret

The card opens below the caret (preedit row on top, candidates downwards) unless
`FanyImeUi::CandidateFitsBelowCaret` (`window/candidate_placement.h`) says it would cross the
bottom edge pad; then it flips above and reverses: the preedit row is the bottom row, right
against the caret, and candidates stack upwards, so the rows nearest the caret are the first
candidates instead of the last. `CandidateCardSitsAboveCaret` and
`AdjustCandidateWindowPosition` make the decision through the same helper, so the window is
never on one side while its rows are ordered for the other.

`CandidateSkinScene::SetAboveCaret` implements the reversal without rebuilding the scene: the
preedit row moves to the end of `body_` (the body is measured to its content, so last is also
bottom-most, and `VerticalAlignment::Trailing` keeps it against the card's bottom edge when a
sticky card height leaves slack), and `CandidateList::SetReverseVertical` mirrors the rows
inside the list (vertical layout only; horizontal rows already read top to bottom). Candidate 1
is therefore the row right above the preedit row and the numbers grow upwards. Hit testing, the
selected-row highlight and the vertical arrow keys all read the same mirrored geometry.

Paging arrows stay under the list, so above the caret they end up above the list and under the
preedit row; the vertical expand panel stays below the first row. What mirrors with the row
order is the decoration that only makes sense on one side: the preedit divider moves from the
block's bottom edge to its top (`TextBlock::SetTopRule`), the preedit row's margin swaps top for
bottom (the skin's row gap has to point at the candidates again), and
`CandidateList::SetOuterCornersEnabled` moves the card-radius corners to whichever list edge now
sits on the card. The flip decision is measured with the card height, so reversing the rows on a
frame does not move the flip.

## v2 skins

`schema_version = 2` skins must use `base = "canvas"` (see `skin/candidate_skin_catalog.h`);
the fields are documented in `examples/skin-examples/schema/README.md`. `canvas` is the
full-texture base: transparent panel, no stroke, square corners, no shadow by default. It is
only usable as a base and is not part of `BuiltInIds()`.

- Geometry lives in `window/candidate_skin_layers.*` (pure arithmetic, no rendering) and is
  shared by the candidate window and the settings preview: `ResolveNineSlice` follows CSS
  `border-image` (slices in source-image pixels, widths in DIP); `ResolveContentPadding`
  measures `content_insets_dip` from the panel's outer edge and never below the border width;
  `SplitDecorations` stably sorts decorations and splits them at `kDecorationFrontZIndex` (1)
  into a layer below and a layer above the card.
- In `CandidatePresenter`'s scene the card and two `CandidateSkinOverlay` layers share one
  cell: back decorations, card, front decorations. Overlay layers measure 0 and do not hit
  test. The panel is drawn by `msimeui::Card::SetBackgroundNineSlice`; with a panel image the
  card's fill, stroke, radius and shadow are cleared. `line_gap_dip` becomes the preedit row's
  bottom margin, so list/pager spacing is unaffected. `outer_insets_dip` becomes extra window
  space around the card. `anchor` and `offset_*_dip`: positive values move right and down.
- Animation: images are fetched through `DeviceResources::GetImageFrame`. `Present` samples
  the animation clock with `BeginFrame`, renders, then reads `Scene::GetNextFrameDeadline()`
  and, only while the candidate window is shown, arms one one-shot coalescable timer
  (`kCandidateAnimationTimerId`) that presents again. Hiding, a deferred show and a skin change
  stop it; the clock restarts when the window goes from hidden to shown. With
  `SPI_GETCLIENTAREAANIMATION` off, images are drawn still (first frame), as for v1 skins.
