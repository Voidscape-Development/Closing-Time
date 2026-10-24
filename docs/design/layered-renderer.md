# Design: the layered renderer

Status: implemented (PR #26). `ARCHITECTURE.md` describes the result; this document records why the
renderer was rebuilt, what it was rebuilt into, the calls made along the way -- including where the
build departs from the first proposal -- and what it measured afterwards.

## Why

The plugin rasterized the whole roll into one tall picture, cut it into tiles and scrolled them.
Almost everything difficult about the render path came from that one decision. These are
measurements from the offscreen harness on a 2,000-name roll (200 lists of 10, 108,200 px tall)
carrying two 4 MB bundled fonts:

| Operation | Cost | Where it ran |
|---|---|---|
| Rasterize the roll | 4.8 – 5.7 s | render thread, on **every** content edit |
| Lay the roll out (no pixels) | 13 – 25 ms | render thread |
| `Document::load` in `update()` | ~105 ms, of which ~97 ms is base64-decoding the fonts | **graphics thread**, on every `update()` |
| `renderKey()` in `update()` | ~77 ms (41 ms saving, 54 ms producing 1.2 MB of JSON) | **graphics thread**, on every `update()` |

`update()` is deferred to the graphics thread for video sources, and a slider drag sends one per
frame. So a large roll cost roughly 180 ms of OBS's compositing thread per frame of a drag. That was
the worst hitch in the plugin, and it was not in the render path at all.

The rest of the problems follow from the single picture:

- **Rebuild cost grows with roll length.** Changing one name re-rasterized every name.
- **VRAM grows with roll length.** Every tile stayed resident for the whole roll: about 1.5 GB at
  the 200,000 px cap and a 1920 px canvas.
- **Swapping a rebuilt strip in is a hitch.** Spreading the uploads (PR #26) helped, but every
  tile still had to go up again, because nothing tied a new tile to the old one that held the same
  pixels.
- **Special cases.** Anything that does not scroll with the roll (animated logos, sticky blocks) had
  to be punched out of the picture as a hole and drawn over it. That trick is the direct cause of
  documented limitations, such as an animated logo inside a sticky block showing only its first
  frame.
- **Playback was an untested state machine.** Phases, a `StickyClock`, roll epochs and a per-block
  state were advanced tick by tick in `CreditsSource.cpp`, which the harness does not compile. A
  scrubbed roll's appearance depended on where it had been scrubbed from.
- **Two compositors.** The source composited on the GPU and the designer preview re-implemented
  the same composition in QPainter. The preview also rasterized at full resolution and then scaled
  down to a pane a quarter of the size.
- **Legacy color.** The source did not declare itself sRGB-aware, so OBS blended it in gamma space
  rather than the linear space the rest of a modern OBS canvas uses, including HDR canvases.

## The target

```
Document ──layoutRoll()──────────▶ RollLayout ──buildTimeline()──▶ RollTimeline   (pure)
          (render thread; costs        │   layers, boxes,
           about as much as measure)   │   timeline inputs
                                       │
                 rasterizeChunk(layer, chunk)  ◀── requested on demand, cached by content key
                 (render thread)       │
                                       ▼
 each frame:   timeline.evaluate(t) ─▶ FrameState ─▶ composeFrame(layout, state, view) ─▶ DrawList
                                                                                         │
                                                       GS backend (source) ◀─────────────┤
                                                       QPainter backend (preview, tests) ◀┘
```

### Layers

The roll becomes a list of **layers**, each with a picture of its own and a rule for where it goes:

- **Section layer.** One top-level section, scrolling with the roll. Its picture covers the
  section's box plus whatever it paints outside it (shadows, a panel's outset), clipped to the
  roll's own extent.
- **Sticky layer.** A sticky block's picture, positioned and faded by the timeline.
- **Animated logo layer.** A logo whose artwork changes, positioned relative to the layer that
  holds it. That layer is the roll for a logo in an ordinary section, or the block for a logo inside
  a sticky block. The second case is new: logos in a block now play.

Composited in document order with premultiplied over, separate section pictures give the same
pixels as painting every section into one shared tile. Over is associative, so the only difference
is 8-bit rounding where two sections' bleeds overlap. The harness checks this by comparing a
flattened layered render against painting sections into one image.

Pictures larger than a GPU wants to take in one upload are cut into **chunks** of at most 1024 px.
A chunk is the unit of rasterization, caching, upload and eviction.

### Content keys

Every layer carries a **content key**: a hash of everything its pixels depend on. That means the
section with its styles and panels resolved through their presets (so editing a preset invalidates
only the sections bound to it), the canvas width, the fonts after substitution, the bundled fonts'
hashes, the picture's bounds, the scale it is drawn at, and whether animated logos are left as
holes.

Files on disk (logos, panel images, custom SVG art) are tracked by **recording** them rather than
by listing them. Every loader reports the paths it reads during a raster, and a cached chunk is
reused only while those files still have the size and modification time they had. A field added to
the model later that names a file is covered without anyone remembering to add it to a list.

Keys are what make edits cheap at every level. The render thread reuses cached pictures, and the
source reuses uploaded textures across a rebuild. Editing one name re-rasterizes and re-uploads
one section.

### The timeline

Where everything is on screen becomes a **pure function of roll time**. `buildTimeline` runs once
per layout. It simulates the sticky blocks event by event (pin, fade-in end, release, leaving the
frame, the roll clearing, the finish) and records each transition time. `evaluate(t)` then answers
by comparing `t` against those times, with no state carried between frames.

- Roll time `t` advances by the video's frame interval while the roll is running and not paused.
  This is the same clock as before, including the jitter handling.
- The strip's offset is `speed · min(t, T_finish, T_travel)`.
- A sticky block's position and opacity are piecewise in `t`.
- Holds count down only while the roll runs. A fade or a release that is already under way carries
  on after the finish, which is the old "coasting" rule.
- The finish time and the ending action's time (finish plus delay) are known up front. They can be
  infinite: a hold that lasts forever, or a loop.
- A loop is `t` modulo the travel time, which restarts every block and every animation.
- Animated logo frames come from `t`: from the start of the roll, or from the moment the logo
  enters the frame. They freeze at the finish.
- `evaluateScrubbed(offset)` is manual scroll. Holds never run out, fades are complete, and
  everything shows the state the position alone implies.

Behavior changes this brings, all deliberate:

1. **Back-to-back "after the roll" blocks now follow one another.** Before, two such blocks
   became eligible in the same tick and faded up together, which defeated the purpose of the
   entrance. The simulation applies events one at a time, so the first card up blocks the second
   until it leaves.
2. **A scrubbed roll no longer depends on where it was scrubbed from.**
3. **A scrubbed roll's animated logos show the frame for that position** rather than the frame
   they were parked on.
4. **The designer shows the roll's true running time**, including holds, or says that it loops or
   holds forever.
5. **Changing scroll speed mid-roll keeps the strip's position.** Roll time is rescaled to the new
   speed, so the strip stays where it was; a block's pin and hold are rescaled along with it.

### The source: residency and swaps

The graphics thread keeps the layout, the timeline and a map from **chunk key to texture**. Each
tick it works out which chunks are wanted:

- **Now:** on screen at the current `t`.
- **Soon:** in the strip window from one canvas below the frame out to four seconds of travel or
  one canvas, whichever is further. Also anything a sticky block will show within the next few
  seconds (found by evaluating the timeline ahead), and the opening of the roll when a loop is
  about to wrap.

Wanted chunks that are not cached are requested from the render thread. Results come back through
an inbox. Uploads follow the per-video-frame budget from PR #26: one chunk per frame, unless the
chunk is on screen now, in which case it goes up regardless. Chunks more than one canvas behind
the roll are evicted.

A rebuilt layout is **staged**. It replaces the old one in the first frame where every chunk it
would show is resident, with a half-second backstop. Because textures are keyed by content, almost
all of those chunks are usually resident already.

If a chunk is on screen but its picture has not arrived yet, the frame draws without it and the
source logs a single "fell behind" line. That is the visible failure mode of lazy rasterization.
Lookahead and queue priority exist to keep it from happening, and the log line is there so it can
be noticed if it does.

### The render queue

One thread, as before, but with two **priority lanes**. Rasters a live source has requested go
ahead of everything else; layout jobs and the designer's work wait behind them. Each consumer's
chunks are drawn from a ranked list it replaces as often as it likes (`ChunkPipeline`), one chunk per
job, so a chunk nobody wants any more is never drawn and an urgent one never waits behind a backlog.
Layouts carry the generation of the edit they were started for and check it between sections, so an
edit that has been overtaken is abandoned part-way through.

### The compositor

`composeFrame(layout, state, view)` decides what is drawn, in what order, where, at what opacity
and clipped to what. It returns a `DrawList`. Two backends execute it:

- **GS (the source):** one effect for every draw, which samples a premultiplied gamma-space
  texture, converts it to linear, re-premultiplies, applies the layer's opacity and writes to an
  sRGB framebuffer.
- **QPainter:** the designer preview and the harness's flattened images.

The preview's view is the roll end to end with blocks drawn back into their slots. The source's
view is the canvas at time `t`. The decision about what to draw lives in one place.

### Color

The source declares `OBS_SOURCE_SRGB` and blends in linear space, the same as OBS's own sources.
That also lets OBS convert it correctly onto an HDR canvas. The background color is linearized
before it is drawn. QPainter still blends in gamma space inside each picture, which is what the
designer sees. The one visible difference is that an antialiased edge composited over other
content on the canvas reads very slightly heavier, the same way every other sRGB-aware OBS source
does.

### Persistence

- **Schema version.** The document records `schema_version`. Version 1 is everything written
  before this change and loads exactly as it did. Version 2 adds the font encoding below. Loading
  runs a migration per version step.
- **Fonts** stay inside the scene collection, because a collection that can be copied to another
  machine and render identically there is the feature. They are written zlib-compressed with their
  SHA-256 beside them. Loading looks the hash up in a process-wide store of decoded fonts and skips
  the decode when it is already held, which turns the ~97 ms per `update()` into a lookup.
  Version 1 base64 fonts still load, are decoded once and are then held the same way.
- **Change detection** is structural equality (`operator==`, defaulted in C++20) over the content
  half of the document, not a JSON key. A field added to the model is compared without anyone
  having to add it, which was the one real virtue of the JSON key.

### The designer

- The preview is built from the same layout and compositor. It rasterizes at the pane's scale times
  the device pixel ratio rather than at full resolution.
- Clicking in the preview selects the section under the cursor, found from the layout boxes.
- The duration label comes from the timeline.

### obs-websocket

The plugin registers a vendor, `closing-time`, when obs-websocket is present:

- Requests: `Start`, `Pause`, `Resume`, `Restart` and `GetState`, each taking a `sourceName`.
  `GetState` returns the phase, the roll time, the running time and whether the roll has finished.
- Event: `RollFinished`, emitted alongside the existing `credits_finished` signal.

## Decisions and departures from the first proposal

- **No drawing-command list inside a section.** The proposal was for layout to produce a list of
  drawing commands for the painter to execute. Building that means re-expressing about 1,500 lines
  of text, gradient, outline, shadow and SVG painting as data. Nothing would consume it that the
  layers and boxes do not already serve, and it would put the renderer's fidelity at risk across
  6,000 checks. The split is made at the section boundary instead: layout produces placed layers
  and boxes, and painting consumes them one layer at a time. Inside a section, measuring and
  painting still run through the same code. That shared path is what guarantees the two cannot
  disagree, and it is kept on purpose.
- **Fonts are not moved to a side store.** The proposal suggested storing them by hash outside the
  collection. That would break the collection's portability, which is why the bundle exists. The
  cost the side store was meant to remove (bloat and decode time) is removed by compression and
  the decode cache instead.
- **Limitations that remain:** a block still cannot hold a block (that is a second set of pinning
  rules, not a rendering problem); a panel's outset is still cut at the ends of the roll (drawing
  past them would put a section's panel on screen before the roll has started); and a panel image
  is still a still.
- **C++20.** It is needed for defaulted comparisons. The OBS plugin template already handles it on
  all three platforms.

## Results

On the same 2,000-name roll, offscreen:

| Operation | Before | After |
|---|---|---|
| An edit, until the frame can show it | ~5 s (rasterize the roll) | ~100 ms layout + ~3 ms per section on screen |
| `update()` on the graphics thread | ~180 ms | ~11 ms (most of it parsing 2,000 entries out of `obs_data`) |
| GPU memory held | the whole roll | what is within a few seconds of the frame |

In a real libobs (the live harness, on Mesa's software renderer under Xvfb): a 300-section roll at
3,000 px/s shows no empty frame, a jump across a parked long roll fills in within one frame, an
edit mid-roll never shows a blank frame, and the roll moves exactly its scroll speed per second.

Two things the build turned up that the design did not anticipate:

- **One request per chunk was the wrong shape.** The first version posted a render job per chunk;
  under load, a chunk needed on screen now waited behind a long tail of prefetch. Replacing a ranked
  list of wanted chunks and drawing the front of it one job at a time (`ChunkPipeline`) is what made a
  fast roll and a scrub jump reliable.
- **Qt draws a run of glyphs slightly differently the first time it meets them.** The layered and
  one-picture renders agree exactly once that is warmed up; the harness's equivalence check says so.

## Verification

- The timeline is tested directly: every entrance against every release, holds, holds forever,
  loops, finishes that come from a block and finishes that come from the roll, scrubbing, speed
  changes and animated logo frames.
- Layered output is compared against one-picture output: the same pixels within rounding, chunk
  seams included.
- Content keys: a section re-rendered at a different position gives an identical picture; editing
  one section changes only its key; editing a preset changes only the keys of the sections bound
  to it; touching a logo file on disk invalidates only the sections that read it.
- Persistence: version 1 documents load unchanged; fonts round-trip through compression; the
  decode store hands back the same bytes; structural equality ignores playback fields and sees
  every content field.
- The source, the GPU path and the designer's preview are checked by the live harness (`tests/live`):
  libobs on a real graphics device, the plugin module loaded, frames read back. The websocket
  vendor's requests go through the same proc handlers the harness drives; the vendor registration
  itself needs obs-websocket installed and has only been exercised as far as its absence.
