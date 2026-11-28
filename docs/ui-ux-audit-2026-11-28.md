# OpenRGB2 UI/UX audit: layout management — 2026-11-28

## Bottom line

The app is not short of panels. It has too many, in the wrong places. Every page is one
vertical column of cards inside one scroll view. That works for Settings, but it breaks
the two pages where the work happens:

- **Lighting** is 11 stacked cards, **8.5 screens tall at 1366×768**. The palette editor and
  the preview of its result are about 3 screens apart, so you never see an edit and its
  effect at the same time.
- **Layout** is a 3D editor laid out like a web form. The viewport is a fixed 430 px strip
  using **23–30 % of the window**. The object tree, type library and transform inspector
  sit in a separate card **1,437 px below the fold** at 1600×1000. Selecting something in
  3D and then typing its coordinates means scrolling the 3D view off-screen.

Nothing is resizable (0 `SplitView`s) and nothing is collapsible. Fixed-pixel panels
(430, 320, 340, 330 px) don't respond to window size. Many objects show up 2–4 times on
the same page. The fix is fewer, persistent regions, not more cards. Section 5 lays out
a concrete target for each page.

Rendering quality is good. Contrast mostly passes WCAG AA. I found no overlapping
controls and no clipped text at normal sizes. The problems are structural.

## 1. Method

- **Build under test:** the installed build at `C:\Program Files\OpenRGB2` (11:41 today, `main`
  @ `b4ae37f`), run as a second non-elevated `openrgb2-ui --qa 5590` instance from
  `build/package/OpenRGB2-0.1.0-win64` against the live engine (7/7 controllers
  connected). I made no edits, saves or applies. I selected one object and then
  deselected it. `workspace.canUndo` stayed `false` throughout.
- **Measurement:** a JS walker (`docs/qa/2026-11-28-ux-audit/audit.js`) ran inside the live
  QML tree through the QA `eval` op. It reported elided or overflowing text, clipping by
  ancestors, overlapping controls, hit targets, font sizes, colors and scroll depth.
  `outline.js` dumped each page's card stack. Both ran for **5 window sizes ×
  5 views**: 960×620 (the minimum), 1366×768, 1600×1000, 1920×1080 and 2560×1392 (screen
  3440×1440 @1×). I also ran the audit at each scroll position.
- **Static checks:** fixed sizes, hard-coded fonts and colors, and locally built
  controls in `ui/**.qml`; WCAG contrast of the Theme tokens.
- **Task walks:** (a) restyle the lights and see the result; (b) select a device in 3D and
  set its position; (c) use the app at minimum and ultrawide sizes.
- **Evidence:** screenshots and raw JSON are in [`docs/qa/2026-11-28-ux-audit/`](qa/2026-11-28-ux-audit/).

## 2. Scorecard (measured)

| Metric | 960×620 | 1366×768 | 1600×1000 | 1920×1080 | 2560×1392 |
|---|---|---|---|---|---|
| Lighting page height, in screens | **13.2** | **8.45** | 6.05 | 5.27 | 3.88 |
| Layout page height, in screens | 4.88 | 3.71 | 2.74 | 2.51 | 1.90 |
| Devices page height, in screens | 4.36 | 3.30 | 2.48 | 2.28 | 1.71 |
| 3D viewport size (px) | 314×430 | 720×430 | 954×430 | 1274×430 | 1914×430 |
| Viewport aspect ratio | **0.73** (portrait) | 1.67 | 2.22 | 2.96 | **4.45** (ribbon) |
| Viewport share of the window | 22.7 % | 29.5 % | 25.6 % | 26.4 % | 23.1 % |
| Elided device names (Layout) | 16 | 16 | 16 | 16 | 16 (25 incl. scrolled) |

| Static metric | Value |
|---|---|
| Resizable / collapsible panels (`SplitView`, drawers) | **0** |
| Fixed-pixel sizes ≥ 100 px in QML | 29 (8 in `LayoutPage.qml`) |
| Locally built button components duplicating `AppButton` | **8** (`LBtn`, `IconBtn`×2, `DlgBtn`×2, `DelBtn`, `Btn`, `TBtn`) |
| Distinct font sizes on Layout (Theme defines 4) | **8**: 10, 11, 12, 13, 14, 18, 24, 28 px |
| Hard-coded `pixelSize` / colors outside Theme | 19 / 23 |
| Global banner chrome above every page | 116 px (15 % of a 768 px window) |
| Fixed sidebar width | 220 px (23 % of the window at the 960 px minimum) |
| WCAG AA text contrast | Passes, except `textDisabled` 4.32 and `bad` 4.27 on `surfaceAlt` |

## 3. Findings, ranked

Severity: **P1** blocks or seriously slows a core task; **P2** costs real time or causes
mistakes; **P3** polish.

### P1-1 Layout: the editor is split across a scrolling page

*Shots 06, 07, 08.* The **Arrangement** card (viewport) sits at the top. The **Scene Objects**
card with the device tree, type library and transform inspector starts 1,279 px down
(at 1366×768). After I selected the keyboard in 3D, the transform inspector was at
y = 2,437 in a 1,000 px window. To type X/Y/Z you scroll the viewport out of sight, so you
can't watch the object move. The **Selection** card next to the viewport shows read-only
facts but has no edit fields. Every 3D editor users already know (Blender, Unity, CAD)
keeps viewport, outliner and inspector on screen together. This is the biggest layout
problem in the app.

### P1-2 Layout: the viewport ignores the window

`LayoutPage.qml:606` sets `height: 430` (also `:454`, `:675`). Width follows the window and
height never does, so the canvas turns into a portrait sliver at the minimum size and a
4.45:1 ribbon at 2560 px. At 2560×1392 there is **about 550 px of empty background under the
canvas** while the Add Devices column keeps going (shot 08). At every size the canvas gets
less than 30 % of the window.

### P1-3 Lighting: edit and result never share a screen

*Shots 02–05.* There are 11 cards in one column: Presets → Mode → Playback state → Devices
(639 px) → Palette (420) → Scene controls → Effect layers (1,016) → Reactive inputs →
Draft preview (**1,132 px**) → Applied output (412) → Profiles. At 1366×768 the palette
sits at y≈1,600 and the draft preview at y≈3,650, about 3 screens apart. Tuning means
scrolling back and forth. The draft preview card is 1,132 px tall but only about 450 px of
its 1,300 px width holds content.

### P1-4 Minimum window size: controls fall off the window

*Shot 09, 960×620.* The Layout toolbar runs past the edge: **Save sits at x = 994 in a
960 px window**, and *Workspace…* is cut off. The **bloom** and **reduced motion**
checkboxes (x = 617–837) are hidden behind the right column. The arrangement hint runs
under the LIVE badge. The window minimum is 960×620 (`Main.qml:26`), so the layout must
work at that size, and it doesn't.

### P2-1 The same thing appears 2–4 times

- **Devices / zones:** Layout lists them in *Add Devices* (22 rows), the *Devices* tree
  and *LED Identification* (7 rows). Lighting lists them in *Devices* (40 chips) **and**
  *Palette* (40 swatches, the same names in the same order, all the same green, shot 03).
- **Previews:** Dashboard *Lighting preview*, Lighting *Draft preview*, Lighting *Applied
  output* and the Layout 3D viewport. All four are LED strips or a render of the same scene.
- **Actions:** *Add all devices* appears twice on Layout (toolbar and Add Devices card).
  There are two Undo/Redo pairs (page toolbar and transform inspector).
- **Help text:** the viewport hint overlay and the caption under the canvas repeat the
  same shortcut list.

Each duplicate costs vertical space and makes you ask which one is the real one.

### P2-2 No panel can be resized, collapsed or remembered

There are 0 splitters. The Layout side column is fixed at 330 px (`LayoutPage.qml:1118`),
the tree at 320×430 and the library at 340×430 (`:1740`, `:1746`). Because the column
can't widen, **16 device names are elided at every window width**, including at 2560 px
with 1,900 px of canvas next to it. Five rows read "Gigabyte AORUS GeForce RTX 5080 …" and
can't be told apart. Inside the 430 px panels, the library list shows 177 px of a
1,674 px list (9.5 screens through a slot), nested inside a page that scrolls too.

### P2-3 Two design systems in one window

The imported studio panels (Scene Objects, LED calibrator) use their own Rectangle-built
buttons: 8 local components; `IconBtn` is 26×22, under the 24 px minimum target. They
also use 10–11 px text, abbreviations (`Grp`, `Un`, `Mir`, `Del`, `Rfr`, `L`, `●`), and a
dense IDE look. The rest of the app uses `AppButton`, 12–14 px text and roomy cards
(shot 07 vs shot 12). Section titles disagree as well. Settings uses sentence case
("Engine connection") and its content starts 8 px higher (card y = 87 vs 95 elsewhere), so
the header jumps when you switch to it.

### P2-4 Global banners take 116 px from every page

The ownership banner ("Another app is controlling your lights…") and the draft-recovery
strip stack above every page, including Settings and Dashboard. They wrap to 120 px at
960 px. They are important, but they are page-agnostic, persistent and full-width, and
they eat the vertical space that Layout and Lighting need most.

### P2-5 Internal IDs and raw data shown as UI

- Lighting chips and swatches show IDs: `case_fan_b1`, `slw_fan_0/body`,
  `mouse_logo/body`. Each group appears twice (`x` and `x/body`), and "unverified" is
  repeated 40 times.
- The Devices *Event log* prints raw JSON
  (`{"active":true,"draft_revision":2,"paused":true,…}`, shot 11).
- The Devices controller rows are a wall of counters (`replaced pending 27048 ·
  suppressed 264 · …`) with no hierarchy.

### P2-6 Contradictory or misplaced status

- The Selection card for the keyboard says "zone: Keyboard · **0 LEDs**" and "emitters:
  **107** (107 addressed)" (shot 06).
- Lighting shows an orange "preset 'wave' is not an uploaded playback preset" on the
  *Animation playback* card while Live effects is the active mode (shot 02). It reads as
  an error in a mode you didn't pick.
- Add Devices offers **Add** for every zone even when it is already placed in the scene.
  I couldn't tell from the UI whether that duplicates it.

### P3 Polish

- The nav label renders literally as **"Devices && Diagnostics"** (`Main.qml:359`). `&&` is
  an escape for Qt mnemonic text, but `NavButton` draws `text` in a plain `Text`, so it
  should be `&`.
- Plurals: "1 LEDs" (`DeviceLibrary.qml:558` and the Add Devices rows), "1 stops".
- The Lighting preset grid uses fixed 260 px tiles and leaves an empty band on the right
  (240 px at 1600, and the right half at 960). The mode cards beneath stretch full width,
  so the edges don't line up. Rows are uneven because one tile is taller.
- Slider columns don't line up across cards. Label and value columns sit at different x
  positions (value at x = 1,385 / 1,478 / 1,497 / 1,516), and the Brightness slider is
  about 1,100 px long.
- Inside Effect layers, the layer editor (*Wave*) renders **above** the stack it edits,
  followed by "Select a layer in the stack to edit it".
- The LED Identification rows spread five controls across 1,300 px with big gaps; the
  disabled "led #" field reads as broken.
- Two contrast tokens are slightly under AA on `surfaceAlt` (4.32, 4.27).

### What's good (keep it)

- The visual quality is consistent and calm, with clear status pills and an honest
  state vocabulary.
- I measured no overlapping controls and no clipped text at ≥ 1366 px. Issues UI-01…UI-04
  and UI-06 from the 2026-09-28 review look fixed. UI-05 is half fixed: the HSV sliders now
  have labels and values, but the 8 quick-color picks are still roughly 14 px dots.
- The pinned Save bar on Lighting (shown only when dirty) is the right pattern; it is
  worth extending to Layout.
- The Dashboard is a good, glanceable page.
- Keyboard view switching (Ctrl+1…5) and a single undo owner are already in place.

## 4. Root causes

1. **One layout primitive for everything.** Each page is a `PageFlickable` → `Column` →
   `Card…`. That fits a settings form, not editors.
2. **Fixed pixel geometry** instead of `Layout.fill*` plus stretch factors for the
   regions that should grow (the viewport, the preview).
3. **Features were added as cards.** Each new capability (studio import, reactive inputs,
   looks, profiles) became another card at the bottom of the stack, so the order
   follows when things were built rather than how they're used.
4. **Imported studio UI was dropped in as-is**, with its own controls and density, inside a
   single card.

## 5. Recommendation: fewer, persistent regions

You said "more panels is not so good", and I agree. The target below **reduces** the
region count on both editor pages, and it moves content instead of adding to it.

### Layout → a real editor workspace (3 regions, no page scroll)

```
┌ toolbar: Undo Redo | 2D 3D | quality ghost bloom | Add… Import/Export Workspace | Save ┐
├──────────────────────────────────────────────┬────────────────────────────────────────┤
│                                              │ [Objects] [Add] [Identify]  ← tabs     │
│      VIEWPORT  (fills all remaining space)    │  tree / add-list / LED identify        │
│                                              ├──────── draggable split ───────────────┤
│                                              │ Inspector for the selection:           │
│                                              │  name · binding · X Y Z · rX rY rZ     │
└──────────────────────────────────────────────┴────────────────────────────────────────┘
        ↑ draggable splitter (SplitView), width persisted in uiPrefs
```

- Make the viewport `Layout.fillHeight/fillWidth`, and delete `height: 430`.
- Merge **Selection** and **TransformInspector** into one inspector that sits next to the
  viewport, so selecting and editing happen on one screen.
- Make **Add Devices**, the **Devices tree** and **LED Identification** tabs of one side
  rail. Identify becomes a context action on the selected object or tree row.
- Remove the duplicate *Add all devices*, the inspector's own Undo/Redo and the caption
  under the canvas (keep the overlay hint, and let it collapse after first use).
- Use a `SplitView` for the rail and save its width. Below about 1100 px, the rail
  collapses to an icon strip or overlay instead of crushing the canvas.

### Lighting → controls on the left, a live result on the right

```
┌ scene name · mode (Live/Playback) · Save ─────────────────────────────────────────────┐
├──────────────────────────────┬─────────────────────────────────────────────────────────┤
│ [Look] [Colors] [Motion]     │  PREVIEW (sticky, always visible)                        │
│ [Reactive] [Library]  ← tabs │   Draft ◉ / Applied ○  toggle                            │
│                              │   compact strips per device (or the 3D view)            │
│ the active tab's controls    │                                                          │
│ only, scrolling inside       │                                                          │
└──────────────────────────────┴─────────────────────────────────────────────────────────┘
```

- **One** preview with a Draft/Applied toggle replaces the two stacked previews (1,544 px
  together today). Pin it so every slider has visible feedback.
- Merge Devices and Palette into **one list**: one row per group with its swatch and a
  selection checkbox. Hide the `/body` sub-groups behind a disclosure.
- Presets move to a "Look" tab, or a compact horizontal strip, instead of a 700 px grid
  at the top.
- Group cards into 4–5 tabs, or an accordion with one open, so the page stops being a
  13-screen scroll.

### App shell

- Merge the two banners into **one compact status strip** (or a badge on the sidebar
  pill and nav item, with details on the relevant page). The goal is ≤ 40 px of global
  chrome.
- Make the sidebar collapsible to an icon rail (220 → about 56 px), which matters at the
  960 px minimum.
- Devices & Diagnostics: put the controller list first, collapse the counters into
  "details", and render the event log as a table (time · kind · summary) with raw JSON on
  expand.

### Consistency pass (cheap, do it alongside)

- Replace the 8 local button components with `AppButton` / an `AppIconButton` (≥ 24 px),
  replace abbreviations with icons + tooltips, and pull the 10/11/13/28 px fonts into the
  Theme scale.
- Use one section-title style everywhere, and fix Settings' 8 px offset.
- Fix `&&` → `&` and the "1 LEDs" / "1 stops" plurals.

## 6. Suggested order

| Step | Change | Effort | Payoff |
|---|---|---|---|
| 1 | P3 quick fixes: `&&`, plurals, title style, Settings offset | S | Removes visible sloppiness |
| 2 | Layout: viewport fills its space plus a side rail with a `SplitView`; inspector beside the viewport | M | Fixes P1-1, P1-2, P1-4 and most of P2-2 |
| 3 | Lighting: sticky preview with a Draft/Applied toggle; merge Devices+Palette | M | Fixes P1-3 and halves page height |
| 4 | Lighting: tabs or accordion for the remaining cards | M | Brings the page to ≤ 2 screens at 1366×768 |
| 5 | Shell: one status strip, collapsible sidebar | S–M | Gives back ~80 px vertical and ~160 px horizontal |
| 6 | Studio controls onto the app component set | M | Fixes P2-3 |
| 7 | Re-run `audit.js` / `outline.js`; targets: Lighting ≤ 2 screens and Layout no page scroll at 1366×768, viewport ≥ 55 % of the window, 0 elided names at ≥ 1600 px | S | Proves it |

## 6a. Follow-up: Layout rework (branch `feat/layout-editor-workspace`)

The Layout page is now an editor workspace. The viewport fills the page, a resizable
side rail (`SplitView`) has tabs **Objects / Library / Detected / Identify** above an
always-visible **inspector** (selection, transform, orientation, link, binding,
calibration), and both split sizes persist in `uiPrefs` (`layoutRailWidth`,
`layoutInspectorHeight`). The toolbar and view bar wrap instead of running off the
window. A *Hide panel* button gives the viewport the full width. Duplicates are gone
(toolbar *Add all devices*, the inspector's own Undo/Redo, the 3D caption under the
canvas). The viewport hint stops at the LIVE badge.

Re-measured with the same scripts (the last pass ran with the engine offline,
which doesn't change geometry):

| Window | Viewport before → after | Aspect before → after | Share of window before → after |
|---|---|---|---|
| 960×620 | 314×430 → 340×254 | 0.73 → 1.34 | 22.7 % → 14.5 % |
| 1366×768 | 720×430 → 722×448 | 1.67 → 1.61 | 29.5 % → 30.8 % |
| 1600×1000 | 954×430 → 886×680 | 2.22 → 1.30 | 25.6 % → 37.7 % |
| 1920×1080 | 1274×430 → 1180×760 | 2.96 → 1.55 | 26.4 % → 43.2 % |
| 2560×1392 | 1914×430 → 1820×1072 | 4.45 → 1.70 | 23.1 % → 54.8 % |

- Page scroll: 1.9–4.9 screens → **none**. The transform inspector moved from 1,437 px
  below the fold to **beside the viewport** at every size.
- Save is inside the window at every size (it was off-window at 960 px). Clipped text and
  control overlaps are 0 at every size. Elided names on the Detected tab at 1600 are
  16 → 0.
- **Still short:** at the 960×620 minimum (with both global banners showing) the
  viewport is smaller than before, because the rail now takes its share. *Hide panel*
  gives the width back. The ≥ 55 % target at 1366×768 was unrealistic: sidebar plus
  banners already take 29 % of that window. The remaining gains are in the shell
  (step 5: one status strip, collapsible sidebar).

## 6b. Follow-up: Lighting rework (branch `feat/lighting-workspace`)

Lighting is now two panes. On the left are tabbed controls: **Look** (preset tiles that
fill the row, then the playback mode), **Colors**, **Motion** (scene controls, uploaded
preset, effect layers), **Reactive**, and **Library** (profiles). On the right is a
**sticky preview** with a **Draft / Applied** toggle (it follows the draft feed until
you pick one) and the playback state below it. The preview width is resizable and
persists (`uiPrefs.lightingPreviewWidth`). The scene name moved into the page header,
so it and the Save bar are always visible.

Colors replaces the two duplicate lists (40 Devices chips + 40 Palette swatches) with
**one checkable group list**. Each row shows its current color, and `x/body`
sub-parts indent under their parent. The picker and **Apply to selected** are pinned
above the list; only the list scrolls.

Re-measured live (engine connected, 43 groups), in screens of scrolling:

| | Before | After (1366×768) | After (1600×1000) |
|---|---|---|---|
| Lighting page | 8.45 (one column) | **no page scroll** | **no page scroll** |
| Look tab | — | 2.34 | 1.20 |
| Colors group list (picker pinned) | — | 5.85 | 3.20 |
| Motion tab | — | 2.54 | 1.64 |
| Reactive / Library | — | 0.73 / 0.62 | 0.50 / 0.43 |
| Distance from a palette edit to its preview | ~3 screens | 0 (side by side) | 0 |
| Apply to selected | at y ≈ 1,800 | in view at every size | in view |

- Clipped text and control overlaps are 0 on every tab at every size. Elided text is 0
  at ≥ 1366 px; at 960 px some group-row details elide, by design.
- At the 960×620 minimum each pane is narrow (controls 384 px, preview 300 px).
  **Hide preview** (next to the tabs) gives the controls the full width, like Layout's
  *Hide panel*.

## 7. Not covered

- I didn't exercise the modal dialogs (Import/Export, Workspace, preset editor) or the
  LED calibrator flow, and I sent no edits, saves or LED identification.
- I made no keyboard-only or screen-reader pass beyond the existing Ctrl+1…5 shortcuts.
- I tested at 100 % scaling only, so 125 %/150 % Windows scaling is untested. At 150 %
  the 960 px minimum becomes 1,440 physical px, and the fixed-pixel issues above get
  worse relative to the screen.
