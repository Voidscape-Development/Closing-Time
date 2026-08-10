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
    ToolButtons.{hpp,cpp}    the compact button shapes a list's controls are built from
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

### The section box

Every section is laid out inside a box rather than across the canvas: `sectionWidth` is the
share of the canvas width it occupies, `sectionAlign` where that share sits, and `marginX` an
inset taken off each of the box's own edges. At the defaults — the full width, centred — the box
*is* the canvas and the result is the plain inset from both edges that a margin has always given.

The box exists because a margin alone can only ever centre things: it insets both sides equally,
so a margin large enough to push a block towards one edge pushes it in from the other by exactly
as much. Splitting "how wide" from "where" is what lets a run of credits sit hard against one
side of the frame while still keeping a margin's worth of clear space off that side.

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
stop — and a tab stop is a *starting* position, not a cap: a left text that overruns it takes
the width it needs (up to whatever the right column leaves) and pushes the bridge along, rather
than wrapping inside its column with most of the row sitting empty beside it. Rows that do fit
still start their bridge at the same x, which is the whole point of the setting. Under a `Fixed`
bridge the split is a real division of reserved space, so it stays a cap there. Under `Natural`,
two texts that together overflow the row shrink in proportion rather than letting whichever comes
first swallow the row and wrap the other out of existence.

Column widths are measured through the same `QTextLayout` the paint pass lays the run out with,
and rounded up to a whole pixel. A column sized from a natural width is handed that width straight
back as its line width, and a measurement landing a fraction of a pixel under what the layout then
asks for is enough to break the line — text wrapping inside a column measured to fit it.

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

The baseline is taken from the **laid-out line**, not from `QFontMetricsF::ascent()`. The two
part company as soon as anything other than the family's own engine supplies a glyph — one
character falling back to another font is enough, and the gap runs from one pixel to eight
depending on the families involved — and a row measured against one ascent while its glyphs are
drawn against another sits its two sides visibly apart for no reason the document can explain.
Measuring it the way the paint pass positions it is the same rule the rest of the layout follows.

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
instead of being cut off at the seam. A section that only places logos counts for that too —
see below — so the bleed is taken over every visible section that sets text *or* draws artwork.

**A logo casts the style's shadow as well.** A section's `TextStyle` is what says a shadow is
wanted, and a wordmark used as a heading is that section's content as much as a line of type
would be; leaving it flat while the headings either side of it were lifted off the background
made the setting look broken rather than deliberate. The artwork is its own silhouette — its
alpha is the shape — so there is no path to offset, only the image recoloured to the shadow's
ink at the size it is about to be drawn, softened by the same box passes as everything else.
The outline is deliberately not carried across: a stroke around a photograph's rectangle is a
frame, which is a different feature wanting its own controls, not this one applied to art.

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
scene item's transform — each tile is drawn as the part of itself that actually falls inside
the canvas. The background is a solid quad drawn underneath.

That clipped quad is built vertex by vertex rather than through `gs_draw_sprite_subregion`,
because the scroll position is fractional and that call is not: it takes whole texels, so the
visible height has to be rounded down and the quad ends up short of the canvas edge by the
fraction that was dropped. What is left is a sliver along the bottom that never gets painted —
a pixel or two once bilinear sampling has softened the last row — which content entering from
below appears to pop through rather than slide into, and which is most obvious on a block of
solid artwork such as a row of logos. Four vertices carrying the fraction in both the positions
and the texture coordinates cost nothing per frame and land the edge exactly.

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

The section list opens wider than its stretch factor alone would give it, and folds away to a
button in its own header rather than to a splitter drag. Dragging a pane shut is easy to do by
accident and leaves nothing on screen saying how to undo it, so the splitter no longer collapses
its children at all: the button is the one way in, and because it stays put when the pane folds,
the one way back out.

The editor keeps one widget set and hides the rows that do not apply to the selected type
(`QFormLayout::setRowVisible`, Qt 6.4+) rather than rebuilding, which keeps focus and scroll
position stable while clicking down the section list. A trailing spacer takes whatever height
is left over: a `QVBoxLayout` with nothing to give its slack to shares it out between the items
it has, which spread a short type's handful of rows down the pane with gaps between them. The
entry table is the one thing worth growing, so it takes the slack instead whenever the selected
type has one, and asks for enough height to read a run of entries at a glance.

The same rule reaches past the form rows: *Set Logo* is hidden for the types whose entries are
lines of text rather than paths, and a logo list hands its width to the path column instead of to
the three-digit pixel height beside it. The gradient stop table sizes itself the same way, from
the stops it actually holds up to a cap, rather than at a fixed height that turned every sweep
past four stops into a four-row window and gave a two-stop one empty rows it had no use for.

Preview re-renders are debounced by
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

### Ways in

The properties window's button was the only way to reach the designer, which meant opening — and
then closing — a window that has nothing to do with what is being edited. There are now four:

| | |
|---|---|
| The properties window's button | the original, unchanged |
| **Tools ▸ Credits Designer…** | whatever credit roll is selected in the current scene, else the only one, else a picker |
| A **per-source hotkey** | `ClosingTime.Designer`, alongside start/pause/restart |
| The source's **Interact** button | see below |

The last one is a compromise worth naming. `OBS_SOURCE_INTERACTION` is what puts an Interact
button on a source, but the button belongs to the frontend: it opens OBS's own interaction
window, and no plugin hook exists to put a different window there instead. What the source does
get is the events that window sends it, the first being a focus event raised as it appears — so
the designer opens on that, and on a click inside the window as a backstop. The interaction
window stays open behind the designer; closing it is the frontend's to do.

Two of these arrive off the UI thread — a hotkey runs on the hotkey thread, and the interaction
callbacks run inside an event still being delivered — so both go through `openDesignerForAsync`,
which queues onto the UI thread holding a weak reference. A source destroyed between the two ends
of that queue makes the task a no-op rather than a crash.

### Preview

The preview pane is the whole roll end to end, at whatever scale fits the canvas across it, with
the wheel moving through it. The dashed frame is the canvas: one screenful, held at the top of
the pane, so the roll runs up through it the way it will on air, and everything below the frame
is dimmed as content that has not reached the screen yet.

Both halves of that are corrections to an outline that had stopped saying anything. The strip
was drawn edge to edge, which put the canvas's left and right edges exactly on the pane's own
border — an outline with nothing on the far side of it to mark it off against — and the dimming
is what makes the frame's bottom edge a boundary rather than a line lying across the middle of
the roll for no visible reason. The scale now comes from the canvas's width rather than the
pane's, and the few pixels of surround that buys is the whole difference between a frame around
the canvas and a border around a pane.

### Controls on a list

The section list's own controls, and the entry table's, are the compact buttons OBS uses for the
same job: a square carrying a glyph or an arrow, with the word it used to show moved to its
tooltip (`ui/ToolButtons.hpp`). That is a legibility decision second and a width decision first —
five labelled buttons held the section pane open at over 400 px whether or not the user had any
use for the space, and a splitter cannot be dragged past what the widgets underneath it demand.
The same row of glyphs comes to under half of that, so the pane now goes as narrow as the list
itself is useful at. Duplicate keeps its word: no glyph says it without a theme icon behind it.

A colour swatch is **painted**, not set as a stylesheet background. A stylesheet does not stop at
the widget it is set on — it reaches everything beneath that widget in the object tree, and a
dialog parented to a widget is beneath it for styling as much as for stacking, so the colour
dialog these buttons open was being painted in the very colour it had been opened to change.
Painting the swatch and hanging the dialog off the window rather than the button leaves nothing
to cascade, and a painted rectangle has no fixed resolution to lose at high DPI, which was the
reason a stylesheet was preferred to an icon in the first place.

## CSV import

`parseCsv` handles quoted fields, doubled quotes, embedded newlines, and LF/CRLF/CR line
endings. `guessDelimiter` scores comma/tab/semicolon/pipe by how many rows agree on the most
common column count, weighted by that count, so a separator that never actually splits
anything loses to one that yields a consistent table.

The dialog offers the fields that make sense for the target section type — left/right for
Bridged, path/height for logo lists, text otherwise — defaulting to mapping columns onto
those fields in order. Import replaces or appends, per a checkbox.

The mapping sits in a panel beside the preview rather than in a strip above it: one row per
column, scrolling down as far as the file is wide, so a dozen-column spreadsheet needs no
sideways scrolling to reach its last column. Each row is labelled with the column's own name
from the header, which is also what the preview's header shows, falling back to `Column N`
when there is no header row or the name is blank. Toggling the header checkbox re-labels the
rows in place rather than rebuilding them, so a mapping the user has already set survives.

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
- A section box — a width and a placement — so a section can sit against one edge of the canvas
  with its margin still holding it clear of that edge.
- Logos cast the style's drop shadow, the same as text and bridge artwork.
- Three more ways into the designer — the Tools menu, a per-source hotkey, and the source's
  Interact button — so it is no longer reached only through the properties window.
- The preview's canvas frame means something again: the canvas is inset from the pane's edges and
  the roll below one screenful is dimmed.
- Colour buttons paint their swatch instead of carrying a stylesheet that leaked into the colour
  dialog they opened.

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

Row geometry and the section box were checked the same way, against the previous build as well
as the new one so that each case is known to fail before it passes: every bridged row staying on
one line across both sizing modes and both filling modes, including a left text that overruns its
tab stop with the rest of the row free; both sides of a row keeping a shared baseline when one of
them carries a character the family cannot supply and the line is really laid out against a
fallback engine's ascent; a half-width section placed left, right and centred keeping its ink
inside the box it was given while a full-width one still spans margin to margin; measure/render
agreement over a document holding every section type; the `obs_data` round trip for the two new
fields and a document written before them loading full width and centred; and a logo's shadow
landing at its offset without changing the strip's height.

The designer's layout was checked offscreen too, by giving the editor more height than it needs
and measuring the largest run of empty space between its visible controls — 6 px for every
section type with the trailing spacer, against 22 px for a Title and 123 px for a Logo Title
without it — along with the entry table holding its minimum height and the section box surviving
a round trip through the editor's widgets.

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
