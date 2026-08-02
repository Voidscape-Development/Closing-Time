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
| Rendering | Qt/QPainter composites the whole roll into one tall image, uploaded as GPU textures and scrolled by offset |
| Persistence | The document lives in the source's `obs_data` settings, so it saves with the scene collection |
| Ending actions | Built-in actions, named-hotkey trigger, filter toggle, and a signal/frontend event — all four |
| Logo variants | "… w/ Logo" = text **with a logo beside it**; "Logo …" = the heading **is** an image, no text |
| Playback | Starts on source visibility, plus start/pause/restart hotkeys, an optional loop, and lead-in/lead-out padding |
| Import | Delimited files (CSV/TSV) with a preview and per-column field mapping |

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
    CreditsModel.{hpp,cpp} TextStyle, LogoRef, Entry, Section, Document + obs_data (de)serialisation
    EndingAction.{hpp,cpp} ending-action config and execution
  render/
    StripRenderer.{hpp,cpp} LogoCache, layout/measure, tiled QImage rasterisation
  source/
    CreditsSource.{hpp,cpp} obs_source_info: playback, GPU upload, draw, hotkeys, properties
  ui/
    DesignerDialog.{hpp,cpp} three-pane designer window
    SectionEditor.{hpp,cpp}  per-section editor + StyleEditor
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
`EndingActionConfig`, and an ordered list of `Section`s.

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
| `TitleWithLogo`, `HeaderWithLogo` | text + a logo beside it | `logoSide` picks the side, `logoGap` the spacing |
| `LogoTitle`, `LogoHeader` | a logo, no text | for wordmarks used as the heading itself |
| `Bridged` | entry list of left/right pairs | joined by `bridge`, e.g. `Director . . . . . . Jane Doe` |
| `TextList` | entry list, one column | |
| `LogoList` | entry list of logos, one column | |
| `MultiTextList`, `MultiLogoList` | entry list over `columns` columns | `fillAcross` picks row-major vs column-major |
| `Spacer` | nothing | a blank run of `spacerHeight` px |

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
                (UI thread)                        │
                                                   ├──> PreviewWidget  (designer, scaled)
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
- **UI thread** owns rasterisation. `QPainter`/`QFont` want a thread with Qt behind them,
  and the designer preview shares the same renderer, so every rebuild is dispatched with
  `obs_queue_task(OBS_TASK_UI, …)`. Rebuilds coalesce: while one is in flight, further
  edits set a re-run flag instead of queueing another task.
- **Either** may touch playback state — hotkeys and proc handlers arrive on the UI thread,
  `video_tick` on the graphics thread — so phase, offset and the pause flag sit behind
  `stateMutex`. The finished rendering crosses threads through `pendingStrip` under
  `handoffMutex`.

Ending actions are decided on the graphics thread but touch the frontend and the scene
graph, so `EndingActionConfig::execute` always hands the work to the UI queue, holding a
strong source reference so the source cannot be destroyed underneath it.

`LogoCache` is **not** shared: the source and each designer window own one. They are only
ever used from the UI thread, but keeping them separate avoids any question about it.

## Ending actions

One action per source, selected by type, plus a delay. Whatever is selected — including
`None` — the source also emits:

- `credits_finished(ptr source)` on the source's own signal handler, and
- `closing_time_finished(ptr source)` on the global signal handler, so a script can listen
  in one place for any roll finishing.

Loop and ending actions are mutually exclusive by construction: a looping roll never
reaches the finished phase.

`FireHotkey` works by looking the hotkey up by its registration name (plus the registering
source's name, for source hotkeys, since names like "Show"/"Hide" repeat) and calling
`obs_hotkey_trigger_routed_callback`. The OBS frontend enables callback rerouting, so this
is the supported way to make a hotkey fire from code. Both the press and the release edge
are delivered, because toggle-style hotkeys act on one and reset on the other.

## Designer window

Three panes in a splitter: section list, editor for the selected section, live preview.

Ownership is split cleanly with the properties dialog — **the designer owns content
(sections), the properties dialog owns canvas, playback and the ending action**. On Apply,
the designer re-reads the live settings and writes back only `sections`, so edits made in
the properties window while the designer was open are not clobbered.

The editor keeps one widget set and hides the rows that do not apply to the selected type
(`QFormLayout::setRowVisible`, Qt 6.4+) rather than rebuilding, which keeps focus and scroll
position stable while clicking down the section list. Preview re-renders are debounced by
250 ms so typing does not re-rasterise the strip on every keystroke.

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

These are deliberate for this first cut, listed roughly in the order worth addressing:

1. **Rasterisation blocks the UI thread.** A very long roll will visibly hitch OBS while it
   re-renders. The fix is a worker thread — the handoff buffer and coalescing logic are
   already built for it; only the `OBS_TASK_UI` dispatch needs to change. Qt supports
   `QPainter` on a `QImage` off the GUI thread, so this is a contained change.
2. **Logos are static.** No animated GIF/WebM logos. *Hybrid logos*: the strip stays as-is
   for text, and logo slots become separate textured quads fed by libobs image sources, at
   the cost of a second draw path with its own transform bookkeeping.
3. **Section reordering is buttons only** — no drag-and-drop in the section list.
4. **`HideSelf` only searches the current scene**, and does not recurse beyond one level of
   groups.
5. **No per-document style presets.** Every section carries its own full `TextStyle`; there
   is no "apply this font to all headers" affordance yet.
6. **The designer has no undo stack.** Cancel discards everything since the last Apply.
7. **Fonts are referenced by family name.** A scene collection moved to a machine without
   that font falls back silently rather than warning.

## Verifying changes

The plugin builds clean against libobs and Qt 6 with `-Wextra -Werror`. There is no test
target in the template yet; renderer and parser changes were validated with an offscreen
harness covering the `obs_data` round trip for all twelve section types, measure/render
agreement, tile contiguity and the tile-height cap, alpha format, hidden-section handling,
and the CSV parser's quoting/line-ending/delimiter-detection cases. Promoting that harness
into a real CTest target is the obvious next infrastructure step.
