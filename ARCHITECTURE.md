# Closing Time — Architecture

How the plugin is put together, why it is put together that way, and what is deliberately
left for later. Read this before changing the render path or the persistence format.

## What it is

A single OBS source type, **Credits Marquee** (`closing_time_credits`), that scrolls a
designed credit roll up the canvas and fires a configurable action when the roll clears the
frame. Content is authored in a dedicated designer window; canvas, playback and the ending
action live in the normal OBS properties dialog.

## Decisions taken up front

| Question | Choice |
|---|---|
| Rendering | Qt/QPainter composites the whole roll into one tall image on a dedicated thread, uploaded as GPU textures and scrolled by offset |
| Persistence | The document lives in the source's `obs_data` settings, so it saves with the scene collection |
| Ending actions | Built-in actions, named-hotkey trigger, filter toggle, and a signal/frontend event — all four |
| Logo variants | "… w/ Logo" = text **with a logo beside it**; "Logo …" = the heading **is** an image, no text |
| Playback | Starts on source visibility, plus start/pause/restart hotkeys, an optional loop, and lead-in/lead-out padding |
| Import | Delimited files (CSV/TSV) with a preview and per-column field mapping |
| Style reuse | Named presets on the document; sections bind by name and fall back to their own style |

Rendering with QPainter rather than native libobs text sources buys exact WYSIWYG parity
between the designer preview and the video output, real text layout (wrapping, alignment,
line spacing, multi-column flow), and a per-frame cost of nothing more than a few textured
quads. The price is that logos are static images — no animated GIF or video logos. That
trade is revisitable: see *Hybrid logos* below.

## Layout of the source tree

```
src/
  plugin-main.cpp          module entry; registers the source and the global signal
  model/
    CreditsModel.{hpp,cpp} TextStyle, StylePreset, LogoRef, Entry, Section, Document + obs_data (de)serialisation
    BridgeArt.{hpp,cpp}    the table of bridge types and the SVG tile each one is drawn from
    EndingAction.{hpp,cpp} ending-action config and execution
  render/
    RenderThread.{hpp,cpp} the shared rasterisation thread and its job queue
    StripRenderer.{hpp,cpp} LogoCache, layout/measure, tiled QImage rasterisation
    BridgeArtRenderer.{hpp,cpp} SVG tile cache, bridge tiling and painting
    ImageEffects.{hpp,cpp} box blur and tinting, shared by both shadow paths
  source/
    CreditsSource.{hpp,cpp} obs_source_info: playback, GPU upload, draw, hotkeys, properties
  ui/
    DesignerDialog.{hpp,cpp} three-pane designer window
    SectionEditor.{hpp,cpp}  per-section editor + StyleEditor
    StyleControls.{hpp,cpp}  colour button, gradient stop editor and its swatch
    PreviewWidget.{hpp,cpp}  scaled preview of the rendered strip
    CsvImportDialog.{hpp,cpp} file picker, preview, column mapping
  util/
    CsvParser.{hpp,cpp}     RFC 4180 parser and delimiter guessing
```

The dependency direction is strictly `ui → render → model` and `source → render → model`.
Nothing in `model/` or `render/` knows the UI exists, which is what lets the same renderer
serve both the designer preview and the video output.

## Data model

A `Document` is a canvas (`width`, `height`, `background`), playback settings
(`scrollSpeed`, `leadIn`, `leadOut`, `loop`, `startOnShow`, `startDelay`), an
`EndingActionConfig`, a list of `StylePreset`s, and an ordered list of `Section`s.

`Section` is a single struct covering all twelve types rather than a class hierarchy. The
fields a given type actually uses are described by four predicates —
`sectionUsesText/Logos/Entries/Columns` — which drive both the layout switch and the
editor's field visibility. One struct keeps serialisation trivial and makes changing a
section's type in the designer a non-destructive operation: nothing is thrown away, the
unused fields simply stop being read.

### Section types

| Type | Content | Notes |
|---|---|---|
| `Title`, `Header` | one text block | differ only in default size/padding |
| `TitleWithLogo`, `HeaderWithLogo` | text + a logo beside it | `logoSide` picks the side, `logoPlacement` how the two relate; see below |
| `LogoTitle`, `LogoHeader` | a logo, no text | for wordmarks used as the heading itself |
| `Bridged` | entry list of left/right pairs | joined by `bridge`, e.g. `Director . . . . . . Jane Doe`; see below |
| `TextList` | entry list, one column | |
| `LogoList` | entry list of logos, one column | |
| `MultiTextList`, `MultiLogoList` | entry list over `columns` columns | `fillAcross` picks row-major vs column-major |
| `Spacer` | nothing | a blank run of `spacerHeight` px |

### Logo rows

`TitleWithLogo` and `HeaderWithLogo` place a logo against a line of text, and
**`logoPlacement`** decides what "against" means:

| | |
|---|---|
| `Edge` | the logo is pinned to the section edge and the text is handed everything left over |
| `Hug` | logo, gap and text are measured as one group and aligned as one |
| `Bridged` | the logo caps one end and the text the other, with the bridge running between |

`Edge` came first and has a trap in it that is worth naming, because it looks like a bug and
is really a layout consequence: the text is given the *entire* remaining column and then
aligns inside it, so a centred title ends up halfway across the frame from its own logo.
`logoGap` cannot pull them together, because under `Edge` it only ever sets the minimum
distance between two columns, never the distance drawn. `Hug` is the fix — measuring the
pair together and aligning the group is what makes the gap the real separation — and it is
what `Section::makeDefault` now hands out. `Edge` remains the *load-time* fallback, so
documents written before the setting existed keep the layout they were built against.

`Bridged` reuses the Bridged section's machinery outright: `bridgeType`, `bridge` and
`bridgeFill` mean exactly what they mean there, and the leader is hung off the text's own
baseline, so it lands on the line the words sit on whatever it is drawn from. `bridgeSizing`,
`bridgeSplit` and `bridgeRowAlign` stay out of it — they describe two *texts* sharing a row,
which is not the shape of this one. Here `logoGap` becomes padding at each end of the span,
so the leader touches neither the logo nor the text.

### Bridged rows

A bridged row is three parts — left text, bridge, right text — and three settings decide what
the bridge is made of and how the width is carved up between them.

**`bridgeType`** is what the bridge is drawn from. `Text` is the original — a string set in the
section's font — and every other type is **vector art**: a small SVG tile laid across the gap.
See *Bridge artwork* below.

**`bridgeFill`** is what the bridge does with a gap wider than one copy of itself:

| | |
|---|---|
| `Fixed` | drawn once at its natural width |
| `Repeat` | tiled as many *whole* times as fit, centred in the gap |
| `Stretch` | spread across the gap, so the run meets both ends of it exactly |

`Repeat` deliberately refuses a partial copy: cutting a leader mid-glyph — or mid-dot — reads
as damage once the bridge is a word rather than a run of full stops. The cost is up to one unit
of slack, so a short unit is what makes it look tight. `Stretch` closes that gap: for text it
divides the shortfall by the full character count rather than the gaps between characters,
which lands the run's advance on the gap exactly instead of overshooting it by one gap; for art
it widens the space between whole tiles rather than distorting them.

**`bridgeSizing`** is how the two text columns are measured:

| | |
|---|---|
| `Split` | the left column is `bridgeSplit` of the shared space, so the bridge starts at the same x on every row — a tab stop. Long text wraps inside the column. |
| `Natural` | each side takes only what its own text needs and the bridge absorbs the rest, so the row reaches both edges but the bridge starts wherever the text ends. |

`bridgeSplit` divides the space the two texts *share* rather than the whole width, which is
what makes `Split` + `Fixed` + `0.5` reproduce the old hard-coded 50/50 layout exactly.
With a filling bridge there is nothing to reserve, so the same number reads as a plain tab
stop. Under `Natural`, two texts that together overflow the row shrink in proportion rather
than letting whichever comes first swallow the row and wrap the other out of existence.

From there **one placement path covers every combination**: whatever the columns leave over
becomes the bridge, and the row is aligned within the section by `bridgeRowAlign`. That
alignment only moves anything for `Natural` + `Fixed`, because every other combination has
already consumed the full width — an invariant worth keeping, since it is what the editor's
field visibility is built on, and it is asserted across the whole mode matrix.

`bridgeSpanEmpty` lets a side with no text give its column up to the bridge, for a heading
row inside an otherwise bridged list. It is confined to a filling bridge: a fixed one has
nothing to cover the freed space with, so collapsing the column there would only shorten the
row — and would break the invariant above. Under `Natural` an empty side already measures
zero, so the flag only really bites under `Split`, where the column is reserved whether or
not anything is in it.

Finally, the three parts share a **baseline** rather than a top edge, anchored on whichever
reaches lowest so nothing climbs into the row above. That is what keeps a leader running
through the middle of the text when the two sides are set at different sizes; when they
match, every offset is zero and the result is what it always was.

### Bridge artwork

A bridge drawn from a string can only ever be whatever characters the font happens to carry, at
whatever weight it draws them: `" . . . . . . "` is a leader by coincidence, and its dots change
size, shape and spacing with the typeface. So a bridge is **drawn from an SVG tile** instead,
and the string is one type among several rather than the only option.

`model/BridgeArt.{hpp,cpp}` is a **table**, not a switch. Each row is an id, a display name, the
markup for one tile, the tile's aspect and what it does when stretched — and that is the whole
of what a bridge type is. Adding one (a chain, a zigzag, a rule with a diamond in the middle) is
a row and a locale string; nothing in the renderer, the editor or the persistence format knows
which types exist. `Custom` is the same mechanism pointed at a file the user picks.

Three decisions hold the rest of it together:

**A tile is described in units of its own height.** The markup is drawn in a box one unit tall
and `aspect` units wide, and `bridgeTypeSvg` derives the `viewBox` from `aspect` so the two
cannot drift. One number — `bridgeThickness`, in pixels — then sizes the whole thing, and
because it is vector art it is as crisp at 3 px as at 30 without a font to supply it.

**Art is a stencil, not a picture.** The built-in tiles are painted white so the renderer can
rasterise them and fill the silhouette with the section's own `TextStyle`: the same colour,
the same gradient mapped over the same block, the same outline, the same shadow the words either
side get. That parity is the point — a leader belongs to the row rather than sitting on it. The
one thing there is no path left to stroke, so an outline grows the silhouette by a ring of
offset copies instead, dense enough that consecutive ones overlap within half a pixel. Only a
custom file has colours of its own worth keeping, so only there does `bridgeTint` get a say.

**Tiling belongs to the type.** `Spread` types (dots, dashes, diamonds) keep whole tiles at
their own size and open up the space between them, so a leader's dots stay round however long
the run is. `Scale` types (line, double line) are continuous and have nothing to count, so both
filling modes stretch one tile across the gap exactly — tiling a rule would leave it short of
the gap by up to a whole tile, which on an unbroken line reads as damage rather than design.

`bridgeOffset` lifts the art off the baseline it otherwise rests on, which is the difference
between a run of leader dots and a rule through the middle of the text, and `bridgeGap` keeps it
clear of the words at each end. Both are confined to art: a text bridge carries its own spacing
in the string the user typed, and applying either to it would move every bridged row in every
document written before this existed. `Text` is likewise the **load-time** fallback for
`bridgeType`, while `Section::makeDefault` hands out `Dots` — the same bargain `logoPlacement`
struck, and for the same reason.

Tiles are parsed into a `BridgeArtCache` that lives for the length of one measure or render
rather than being kept between them, unlike `LogoCache`. A tile is a few hundred bytes of
markup, so the cache exists to avoid re-parsing once per *row*, not once per render — and
rebuilding it each time is what makes a custom SVG edited on disk show up in the next rebuild
with nothing having to watch the file.

### Text fills, outlines and shadows

A `TextStyle` says what the glyphs are *filled* with as well as what font they are set in.
`TextFill` picks between the plain `color` and a linear or radial `GradientSpec`; on top of
that a style may carry a `TextOutline` and a `TextShadow`.

Three decisions are worth recording:

**A gradient is mapped over the block of text being drawn** — one title, one list entry, one
side of a bridged row — not over the canvas and not over the strip. Mapping over the strip
would make a stop's colour depend on how long the roll happened to be, so adding a section at
the bottom would restyle everything above it. Per block, a run of names all share the same
sweep as each other. The block is the run's *laid-out* height and the horizontal extent its
lines actually ink: laid-out height rather than ink bounds is what keeps a bridged row
coherent, since a run of leader dots inks only a few pixels of height and mapping the sweep
onto that would cram the whole gradient into the dots while the text beside them showed a
sliver of it. `textFillBrush()` is exported from the renderer so the designer's gradient
swatch is painted by the same mapping rather than a second copy of it that can drift.

**Nothing here takes part in layout.** An outline or a shadow paints outside the text's box
without growing it, the way a CSS `text-shadow` does — otherwise switching a preset's shadow
on would reflow every section bound to it and change the roll's duration. The cost is that a
section can paint outside its own box, which the tile loop has to know about: `effectBleed()`
reports the furthest any style reaches and `render()` widens the range of sections it visits
per tile by that much, so a shadow cast across a tile boundary is drawn into both tiles
instead of being cut off at the seam.

**Effects render through glyph paths, plain text does not.** `QTextLayout::draw()` can only
put a pen colour through the glyphs, so a style with a fill, an outline or a shadow is
converted to a `QPainterPath` by way of `QRawFont::pathForGlyph` — which keeps the shaping
`QTextLayout` already did, unlike rebuilding the run from the source string. A style with
none of the three keeps the original `layout.draw()` path, so documents that predate this
rasterise exactly as they did before. Outlines are stroked at twice their width underneath
the fill, which covers the inner half back up and leaves the outline growing outward only.
Shadows are drawn into a scratch buffer and softened with three box passes; the buffer is
bounded, and a blur too large to buffer falls back to a hard shadow with a log line.

All of that cost lands on rasterisation, which happens once per document change on the render
thread, not per frame — playback still draws the same tiles it always did.

### Style presets

A `StylePreset` is a named `TextStyle` on the document. A section may bind to one by name
(`stylePresetName`, and `secondaryStylePresetName` for the right-hand side of a `Bridged`
section); everything that lays text out goes through `Document::effectiveStyle` /
`effectiveSecondaryStyle` rather than reading `Section::style` directly, so a binding cannot
be bypassed by forgetting to resolve it in one branch of the layout switch.

Binding is **non-destructive in both directions**, the same way changing a section's type
is. A bound section keeps its own `TextStyle` untouched, and a name that no longer resolves
— a preset deleted, or a document moved between machines — falls back to it rather than
failing. Deleting a preset also clears the bindings that named it, so a later preset reusing
the name cannot silently recapture sections the user had let go.

In the designer, a bound style stays editable and an edit to it *is* an edit to the preset.
That is the whole point of the feature: "restyle every header" is one change, not one change
per header.

### Persistence

`Document::save`/`load` write directly to the source's settings object. Section lists are
`obs_data` arrays of objects; ending-action fields are flat `ea_`-prefixed keys on the same
object so `obs_properties` can bind to them without the source marshalling a sub-object on
every edit.

Enums persist by **string id**, never by ordinal (`sectionTypeId`, `endingActionId`,
`hAlignId`, …). The enums can be reordered freely; only the id strings are contractual.

Font sizes are stored and laid out in **pixels**, not points, so a roll renders identically
regardless of the DPI of whatever screen OBS happens to be running on.

## Render pipeline

```
Document ──StripRenderer.render()──> Strip { tiles: [ {top, QImage} ], width, height }
              (render thread)                      │
                                                   ├──> PreviewWidget  (designer, scaled)
                                                   │    posted back to the UI thread
                                                   └──> gs_texture_create per tile
                                                        (graphics thread) ──> gs_draw_sprite_subregion
```

**Two-pass layout.** `layoutSection()` takes an optional `QPainter`. With a null painter it
measures; with a painter it draws. Both passes run the identical code, so measurement and
painting cannot drift apart — the harness asserts `measure()` agrees with `render()`.

**Tiling.** The strip is cut into ≤2048 px tall tiles. A long roll easily exceeds the
maximum texture height a GPU will accept (commonly 16384 px), and tiling also avoids
allocating one enormous texture when only a screenful is ever visible. Total strip height is
capped at 200 000 px as a backstop against a runaway import.

**Alpha.** QPainter needs a premultiplied buffer; OBS composites with straight alpha. Each
tile is unpremultiplied once at the end of rasterisation (`Format_ARGB32`, which is `GS_BGRA`
in memory on little-endian) rather than corrected per frame on the GPU.

**Scrolling.** The strip's top edge sits one canvas-height below the top of the frame at
offset 0 and travels upward, so the roll enters from the bottom. Total travel is
`canvasHeight + stripHeight`; `leadIn`/`leadOut` are baked into the strip itself as blank
space at its top and bottom.

**Clipping.** Rather than a scissor rect — which lives in screen space and would fight the
scene item's transform — each tile is drawn as the sub-rectangle that actually falls inside
the canvas, via `gs_draw_sprite_subregion`. The background is a solid quad drawn underneath.

## Threading

Three threads touch the source, and the split is deliberate:

- **Graphics thread** owns `document`, all playback advancement, the GPU textures, and
  drawing. libobs defers `update()` for video sources to the graphics thread, so the
  document has exactly one writer.
- **Render thread** owns rasterisation. One thread, shared by every source and every open
  designer window, running jobs strictly in the order they were posted (`RenderThread.hpp`).
  Rebuilds coalesce: while one is in flight, further edits set a re-run flag instead of
  queueing another job, so a long roll cannot accumulate a backlog of stale frames behind
  the one being waited on. Both the source and the designer do this.
- **UI thread** owns the widgets. A finished preview comes back to it through
  `QMetaObject::invokeMethod(qApp, …, Qt::QueuedConnection)`.
- **Either UI or graphics** may touch playback state — hotkeys and proc handlers arrive on
  the UI thread, `video_tick` on the graphics thread — so phase, offset and the pause flag
  sit behind `stateMutex`. The finished rendering crosses to the graphics thread through
  `pendingStrip` under `handoffMutex`.

Painting off the GUI thread is sound because the paint device is a `QImage`, which is the
case Qt supports; `QFontMetrics`, `QTextLayout` and `QImageReader` come along with it. What
would *not* work is painting a `QPixmap` or a widget, and the renderer does neither.

**Lifetimes across the handoff.** A rebuild job holds a weak source reference and upgrades
it before touching anything; a successful upgrade means `destroy()` cannot be running, so
the source data stays valid for the length of the job. A designer preview job instead holds
a shared `PreviewSink` whose pointer the dialog's destructor clears, so a strip that lands
after the window closed is simply dropped. Neither path reads anything from the source
itself on the render thread — even the source name is copied into the job at queue time.

Ending actions are decided on the graphics thread but touch the frontend and the scene
graph, so `EndingActionConfig::execute` always hands the work to the UI queue, holding a
strong source reference so the source cannot be destroyed underneath it.

`LogoCache` is **not** shared: the source and each designer window own one. Render jobs run
one at a time, which is what makes each cache single-threaded without a lock to say so. The
designer's is held by `shared_ptr`, because a job in flight outlives the window that posted
it.

`obs_module_unload` calls `stopRenderThread()`, which discards queued jobs, waits for the
one running, and joins — nothing is left executing code in a module about to be unmapped.

## Ending actions

One action per source, selected by type, plus a delay. Whatever is selected — including
`None` — the source also emits:

- `credits_finished(ptr source)` on the source's own signal handler, and
- `closing_time_finished(ptr source)` on the global signal handler, so a script can listen
  in one place for any roll finishing.

Loop and ending actions are mutually exclusive by construction: a looping roll never
reaches the finished phase.

`HideSelf` walks **every** scene rather than the current one, hiding each item backed by the
source. That is what makes it work for the arrangements people actually build: a roll parked
in a nested scene, or the same source placed in several scenes at once. Nesting needs no
special handling — a nested scene is itself a scene in that walk — and groups recurse
through the item callback.

`FireHotkey` works by looking the hotkey up by its registration name (plus the registering
source's name, for source hotkeys, since names like "Show"/"Hide" repeat) and calling
`obs_hotkey_trigger_routed_callback`. The OBS frontend enables callback rerouting, so this
is the supported way to make a hotkey fire from code. Both the press and the release edge
are delivered, because toggle-style hotkeys act on one and reset on the other.

## Designer window

Three panes in a splitter: section list, editor for the selected section, live preview.

Ownership is split cleanly with the properties dialog — **the designer owns content
(sections and style presets), the properties dialog owns canvas, playback and the ending
action**. On Apply, the designer re-reads the live settings and writes back only its own
half, so edits made in the properties window while the designer was open are not clobbered.

The editor keeps one widget set and hides the rows that do not apply to the selected type
(`QFormLayout::setRowVisible`, Qt 6.4+) rather than rebuilding, which keeps focus and scroll
position stable while clicking down the section list. Preview re-renders are debounced by
250 ms so typing does not re-rasterise the strip on every keystroke, and the render itself
happens off-thread, so even a roll that takes seconds to rasterise leaves the window usable.

Sections reorder by drag-and-drop as well as by the move buttons. The list widget reports
the drop rather than performing it — the document owns the order, and letting the view
rearrange its own items too would leave two orders to reconcile.

**Undo** is a stack of whole-document snapshots covering exactly what the designer owns:
sections and style presets. Snapshots rather than command objects because a document is a
handful of kilobytes of implicitly shared containers and no edit here would meaningfully
compress into a command. Structural edits take a step each; runs of small edits (typing a
title, dragging a spinbox) coalesce into one step that closes on the next selection change,
the next structural edit, or 900 ms of quiet. Undo/redo take Ctrl+Z and Ctrl+Shift+Z away
from the text fields' own undo, deliberately: one stack that restores the whole section
beats two that disagree about what the last change was.

One window per source, tracked in a registry keyed by source pointer. Because sources are
destroyed on the graphics thread, `closeDesignerFor` queues the close onto the UI thread and
only acts once the dialog's weak reference has actually expired — which also covers a later
source reusing the same address.

## CSV import

`parseCsv` handles quoted fields, doubled quotes, embedded newlines, and LF/CRLF/CR line
endings. `guessDelimiter` scores comma/tab/semicolon/pipe by how many rows agree on the most
common column count, weighted by that count, so a separator that never actually splits
anything loses to one that yields a consistent table.

The dialog offers the fields that make sense for the target section type — left/right for
Bridged, path/height for logo lists, text otherwise — defaulting to mapping columns onto
those fields in order. Import replaces or appends, per a checkbox.

## Known limitations

1. **Logos are static.** No animated GIF/WebM logos. *Hybrid logos*: the strip stays as-is
   for text, and logo slots become separate textured quads fed by libobs image sources, at
   the cost of a second draw path with its own transform bookkeeping. This is the one
   remaining item that is a genuine fork in the architecture rather than work.
2. **A missing font is reported, not resolved.** The designer names the substituted families
   under the preview and the source logs them once, but nothing embeds or bundles a font, so
   a roll still renders differently on a machine that lacks one.
3. **Undo does not reach across Apply.** The stack is per-designer-window and starts empty
   each time the window opens; Cancel still discards everything since the last Apply.
4. **Style presets are per document.** There is no shared library across sources or scene
   collections — exporting the JSON and importing it elsewhere is the way to carry them.
5. **One ending action per source.** A roll that needs to do two things has to chain them
   through the `credits_finished` signal.

### Addressed since the first cut

- Rasterisation moved off the UI thread onto a shared render thread, for both the source and
  the designer preview.
- Drag-and-drop reordering in the section list.
- `HideSelf` now walks every scene rather than only the current one.
- Per-document style presets, with editing a bound style editing the preset.
- Undo/redo in the designer, over sections and presets.
- Missing fonts are surfaced in the designer and the log instead of silently substituted.
- Gradient fills, outlines and drop shadows on any style, preset or not.
- Bridges drawn from SVG art rather than from a string of characters, from a table of types
  that takes a new one without the renderer or the editor changing.

## Verifying changes

The plugin builds clean against libobs and Qt 6 with `-Wextra -Werror`. There is no test
target in the template yet; renderer and parser changes were validated with an offscreen
harness covering the `obs_data` round trip for all twelve section types, measure/render
agreement, tile contiguity and the tile-height cap, alpha format, hidden-section handling,
and the CSV parser's quoting/line-ending/delimiter-detection cases.

Bridge artwork was validated offscreen as well: every built-in type across all three fill
modes, checking that tiles stay inside the gap they were given, never overlap, and — for a
spreading type under `Stretch` and a scaling type under either filling mode — meet both ends of
it; measure/render agreement over a document holding every type; the `obs_data` round trip for
the new fields; a document written before the art types existed still loading as a text bridge
with its string intact and a thickness to fall back on; a custom tile sized from its own
viewBox, and a custom bridge with no file or a missing one drawing nothing rather than failing
the strip; a leader picking up the same gradient, outline and shadow as the text either side of
it; and a logo row bridged across to its text taking the same art on the same baseline.

Fills were validated the same way: each of solid, linear, radial, outline, hard and soft
shadow and all three together rendered offscreen and inspected; the `obs_data` round trip for
the new fields; a document written before fills existed still loading as plain solid text
with no effects and no bleed; a bridged row's leader picking up the same sweep and the same
outline as the text either side of it; and a 30 px blur straddling the 2048 px tile seam,
where the summed alpha of the rows either side of the boundary has to stay continuous rather
than dropping to zero on one side.

Two pieces can already be tested without libobs at all, which is the shape the rest of the
harness should take:

- `RenderThread` needs only Qt Core — post ordering, posting from several threads at once, a
  job posting its own follow-up, and jobs posted after `stopRenderThread()` being dropped.
- Row placement needs only Qt Gui, for real font metrics. Worth covering because the geometry
  is where these section types' behaviour lives: that the Bridged defaults still reproduce
  the old 50/50 layout, that the tab stop holds across rows of different lengths, that both
  edges are met whenever the mode says they should be, that overlong rows shrink in
  proportion, that a filled bridge covers the gap it was given, and — across the whole
  fill × sizing × one-sided matrix — that only `Natural` + `Fixed` leaves a row short of the
  full width. For logo rows: that `Hug` holds `logoGap` exactly across every alignment and
  side while `Edge` demonstrably does not, and that a `Bridged` logo row caps both ends with
  the leader padded off each.

Promoting all of it into a real CTest target is the obvious next infrastructure step.
