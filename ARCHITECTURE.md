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
| Playback | Starts on source visibility, plus start/pause/restart hotkeys, an optional loop, lead-in/lead-out padding, and a manual mode that parks the roll at a position instead of scrolling it |
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
    DividerArt.{hpp,cpp}   the table of divider shapes, and which of the three slots each serves
    EndingAction.{hpp,cpp} ending-action config and execution
  render/
    RenderThread.{hpp,cpp} the shared rasterisation thread and its job queue
    StripRenderer.{hpp,cpp} LogoCache, layout/measure, tiled QImage rasterisation
    SvgArt.{hpp,cpp}       the SVG tile cache, and painting a silhouette through a TextStyle's ink
    BridgeArtRenderer.{hpp,cpp} bridge tiling, on top of SvgArt
    DividerArtRenderer.{hpp,cpp} divider part sizing and arm tiling, on top of SvgArt
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
(`scrollSpeed`, `leadIn`, `leadOut`, `loop`, `startOnShow`, `startDelay`, `manualScroll`,
`scrollPosition`), an `EndingActionConfig`, a list of `StylePreset`s, and an ordered list of
`Section`s.

`Section` is a single struct covering all nineteen types rather than a class hierarchy. The
fields a given type actually uses are described by five predicates —
`sectionUsesText/Logos/Entries/Columns/Subtitles` — which drive both the layout switch and the
editor's field visibility, plus `sectionUsesSecondaryText`, derived from two of them rather than
tabulated because "it carries two texts" is true of a bridged row, a title/subtitle pair and a
heading with a line under it alike, however differently the three are laid out. One struct keeps serialisation trivial and
makes changing a section's type in the designer a non-destructive operation: nothing is thrown
away, the unused fields simply stop being read.

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
| `TitleWithSubtitle`, `HeaderWithSubtitle` | `text` over `secondaryText` | the list's stacked pair as a single heading; see below |
| `TitleWithLogo`, `HeaderWithLogo` | text + a logo beside it | `logoSide` picks the side, `logoPlacement` how the two relate; see below |
| `TitleWithSubtitleAndLogo`, `HeaderWithSubtitleAndLogo` | the stacked pair + a logo beside it | the same logo row, with a pair in the text column instead of a line |
| `LogoTitle`, `LogoHeader` | a logo, no text | for wordmarks used as the heading itself |
| `Bridged` | entry list of left/right pairs | joined by `bridge`, e.g. `Director . . . . . . Jane Doe`; see below |
| `TextList` | entry list, one column | |
| `TitleSubtitleList` | entry list of stacked text pairs | e.g. a position over the name that holds it; see below |
| `LogoList` | entry list of logos, one column | |
| `MultiTextList`, `MultiLogoList`, `MultiTitleSubtitleList` | entry list over `columns` columns | `fillAcross` picks row-major vs column-major |
| `SectionDivider` | no entries; a `dividerCentre` stack of its own | an ornamental rule composed from a cap, an arm and a centre; see below |
| `Spacer` | nothing | a blank run of `spacerHeight` px |

### Logo rows

`TitleWithLogo` and `HeaderWithLogo` place a logo against a line of text — and
`TitleWithSubtitleAndLogo` and `HeaderWithSubtitleAndLogo` against a stacked pair, through the
same branch. **`logoPlacement`** decides what "against" means:

| | |
|---|---|
| `Edge` | the logo is pinned to the section edge and the text is handed everything left over |
| `Hug` | logo, gap and text are measured as one group and placed as one |
| `Bridged` | the logo caps one end and the text the other, with the bridge running between |

`Edge` came first and has a trap in it that is worth naming, because it looks like a bug and
is really a layout consequence: the text is given the *entire* remaining column and then
aligns inside it, so a centred title ends up halfway across the frame from its own logo.
`logoGap` cannot pull them together, because under `Edge` it only ever sets the minimum
distance between two columns, never the distance drawn. `Hug` is the fix — measuring the
pair together and placing the group is what makes the gap the real separation — and it is
what `Section::makeDefault` now hands out. `Edge` remains the *load-time* fallback, so
documents written before the setting existed keep the layout they were built against.

A `Hug` group is placed by **`sectionAlign`**, not by the style's own alignment. `Edge` and
`Bridged` both consume the whole section box — the logo is pinned to one of its edges and the
text runs to the other — so the box, and with it the section's placement, is what decides where
they land. A `Hug` group is narrower than its box by construction, and aligning it by the text
left the one setting named after placing a section unable to move it: a header placed hard left
sat centred in that left-hand box because its title happened to be centred, which reads as the
placement being ignored rather than as two settings interacting. The style's alignment keeps its
own job inside the text column, which is where a wrapped or multi-line title needs it.

The four shapes differ in **what goes in the text column and in nothing else**, which is why
they share one layout branch rather than a second copy of the placement: `placeLogoRow` is
handed the width the column wants and whether there is anything in it, and a pair hands over
the wider of its two lines. The logo is centred against the whole block, so a subtitle makes
the row taller and moves the logo down with it rather than leaving it level with the title and
the pair hanging off the bottom. A single-line type is just the pair with an empty subtitle —
which the stack already draws as one line at one height, taking no gap with it — so the plain
shapes go through the same call and come out exactly where they always did.

`Bridged` reuses the Bridged section's machinery outright: `bridgeType`, `bridge` and
`bridgeFill` mean exactly what they mean there, and the leader is hung off the text's own
baseline, so it lands on the line the words sit on whatever it is drawn from. With a pair it is
the baseline of whichever line ends up **on top** — the title ordinarily, the subtitle when the
stack is flipped, and whichever one is actually filled in when only one is — so adding a
subtitle under a bridged heading leaves the leader exactly where it was. `bridgeSizing`,
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
rasterise them and fill the silhouette with a `TextStyle`, by default the section's own: the same
colour, the same gradient mapped over the same block, the same outline, the same shadow the words
either side get. That parity is the default because a leader belongs to the row rather than
sitting on it — and it is exactly what gets in the way when the leader is the part meant to stand
out, which is what `bridgeStyle` is for; see *The bridge's own ink* below. The one thing there is
no path left to stroke, so an outline grows the silhouette by a ring of offset copies instead,
dense enough that consecutive ones overlap within half a pixel. Only a custom file has colours of
its own worth keeping, so only there does `bridgeTint` get a say.

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

### The bridge's own ink

A bridge drawn in the section's style is a leader that belongs to its row, which is right up to
the moment the leader is the thing being designed: yellow dots under white names, a rule carrying
a sweep the words do not. `useBridgeStyle` turns the section's `bridgeStyle` on for that, and
`Document::effectiveBridgeStyle` is the one place the two are reconciled — nothing paints a bridge
without going through it, the same guarantee `effectiveStyle` gives the text.

**It merges rather than replaces, and it merges only the ink.** The fill, the gradient, the
outline and the shadow come from the bridge style; the family, size, weight, alignment and line
spacing stay the row's. That split is not a simplification, it is what makes the feature free:
every width, baseline and height a bridged row is built from is measured off the fields the merge
leaves alone, so a leader that has been recoloured reserves exactly the width it did before and
nothing else in the row moves for it. A text bridge is still set in the face the words either side
of it are, at their size — the string is what the user typed, drawn in a different colour.

It follows that a preset written for a run of headings can be pointed at a leader without dragging
a heading's font size across with it, which is why `bridgeStylePresetName` binds to the same
document presets everything else does rather than to a second, ink-only library. The designer's
editor for it hides the rows it does not use (`StyleEditor::setInkOnly`) and carries their values
straight through rather than reading them back off the hidden widgets, so editing a bound preset
from there cannot quietly rewrite the font of every section following it.

The override is resolved per section rather than hoisted beside `effectiveStyle`, because it
returns by value — a merge has nothing to return a reference to — and only the two bridged shapes
have any use for it. `effectBleed` is the exception and asks every section for it: a separately
inked bridge can carry a heavier shadow than either text beside it, and letting the bleed's
predicate disagree with the layout switch about which types draw bridges would clip that shadow at
a tile seam for exactly the sections it was wrong about.

The one place it does not reach is a custom tile with `bridgeTint` off, which is painted to the
strip in the colours it was authored with before any of this runs. The designer hides the whole
group in that case rather than offering settings that do nothing.

Tiles are parsed into a `BridgeArtCache` that lives for the length of one measure or render
rather than being kept between them, unlike `LogoCache`. A tile is a few hundred bytes of
markup, so the cache exists to avoid re-parsing once per *row*, not once per render — and
rebuilding it each time is what makes a custom SVG edited on disk show up in the next rebuild
with nothing having to watch the file.

### Section dividers

An ornamental rule is not one picture. Look at any sheet of them and the same anatomy repeats:
an **end cap**, an **arm** running inward from it, something in the **middle**, then the whole
thing mirrored. `SectionDivider` is built that way rather than as a list of finished dividers,
because three slots drawn from one library of two dozen shapes is a great many more dividers
than there are rows in the library — and adding a shape adds it to every divider that could use
it rather than to one.

`model/DividerArt.{hpp,cpp}` is a table on the same pattern as `BridgeArt`, with one field the
bridge table has no use for: **`roles`**, a mask of which slots a shape may be picked for. A
diamond is a perfectly good end cap *and* a perfectly good centrepiece, and saying so once beats
a second Diamond in a second table that has to be kept looking like the first. The pickers are
built by filtering the table on the slot they serve, so a shape is offered wherever it belongs
without anything enumerating the combinations.

Two things about the geometry are worth naming, because both are what let one number size a
whole divider:

**A cap is authored pointing outward along -x, and the right-hand end is that same tile
mirrored.** Mirroring is a painter transform on the artwork rather than a second tile, so a cap
cannot come out subtly different at the two ends of the same rule. `dividerMirrorEnds` is on by
default and true of every ornamental rule that is not an arrow pointing somewhere; switching it
off reveals `dividerEndCap`, which is *kept* either way so the toggle is non-destructive. The
same mirror applies to the right-hand **arm**, unconditionally: a taper running from a hairline
up to full thickness, or a rule ticked at one edge of each tile, would otherwise point the same
way on both sides and leave the divider lopsided. Mirroring a symmetric tile costs a transform
and changes nothing, which is why it is not a property each shape has to remember to declare.

**Every shape declares its own height as a multiple of the rule it belongs to.** `height` in the
table is that proportion — an arrowhead some four and a half times the rule, a centre diamond
three and a half — so `dividerThickness` scales all of it and an arrowhead stays an arrowhead
when a divider is made heavier. The one exception is the **arm**, whose height is *always* the
thickness whatever its row says: an arm is the rule, and a rule has no proportion to itself.

An arm has no fill setting, unlike a bridge. A bridge has three because the user is choosing how
a leader sits between two words; an arm has two fixed ends and only one sensible answer, so a
`Scale` shape covers its span exactly and a `Spread` shape lays whole tiles and shares the
leftover out *between* them rather than at the ends, meeting the cap outside it and the centre
inside it.

**The middle is a list, not a choice.** `dividerCentre` is a `QVector<DividerPiece>`, each an
ornament, a word or an image, drawn in order. The figures that actually turn up in the middle of
a rule are compounds — a diamond with a dot either side of it, `PART II` between two curls — and
offering a single slot would mean shipping every one of those as its own tile or making the user
draw it. An empty list is the ordinary case rather than a degenerate one: it is the unbroken
rule, and the two arms become one arm spanning the whole width, so a scaling shape is stretched
once across it instead of twice across two halves. A piece that measures to nothing — an empty
word, a logo that would not decode, an ornament whose file is missing — is dropped along with the
gap that would have sat beside it, rather than left as a hole in the rule.

A word and an image in the centre go through **exactly the helpers that draw a section's own
title and its own logo**, which is why they can be there at all: they pick up the style's
gradient, outline and shadow with no second implementation of any of it. That is also why the
composition lives in `StripRenderer.cpp` beside the other section geometry while
`DividerArtRenderer` handles only the artwork — the same split `prepareBridge`/`paintBridge`
already make.

`dividerRules` runs two or more rules in parallel about the midline, and the caps and the centre
are drawn **once** across the whole stack rather than once per rule: three lines broken by one
ornament is the deco figure, where three complete dividers touching is not. `dividerRuleInset`
makes each rule shorter than the one nearer the midline by a fixed amount, so a stack tapers to
a wedge; an even-numbered stack has no middle rule to measure from, so its two innermost sit half
a step out and the figure stays symmetric either way.

Ink comes from the **bridge's** override. `useBridgeStyle`/`bridgeStyle` are reused outright
rather than duplicated under a divider name, because "colour the artwork apart from the text" is
one want with two names: a divider whose rule carries the title's gold sweep while its own label
stays white is the same edit as yellow leader dots under white names. The designer retitles the
group by section type, the way it already retitles the secondary style group.

Every tinted part of a divider is rasterised into **one** silhouette and inked once — not part by
part — so an outline surrounds the divider rather than each diamond in it, and one gradient runs
the whole way across instead of restarting at every piece. That is what `render/SvgArt.{hpp,cpp}`
exists for: the tile cache and the paint-through-ink path (shadow, grown outline, gradient
stencil) were lifted out of `BridgeArtRenderer` so both callers ink identically rather than by
two implementations that agree today. `paintInkedArt` takes a callback that draws the silhouette,
which is what lets the bridge hand over a run of tiles and the divider hand over a whole figure
made of different artwork.

Every load-time fallback is `None` — a divider whose keys a document does not carry draws the
plainest thing the library can, rather than whatever happens to sit first in the table — except
the arm, where `None` would mean a divider with no rule in it at all, so that one falls back to
`Rule`.

### Title and subtitle rows

A `TitleSubtitleList` entry is two texts stacked rather than two texts on a line: `text` above
`secondaryText`, drawn in the section's primary and secondary styles respectively. It is the
same pair of fields a `Bridged` row carries, and deliberately so — the two types are the two
ways the same content wants to be set, so switching between them keeps every entry intact,
which is the whole reason `Section` is one struct. `MultiTitleSubtitleList` is that entry
placed into columns, sharing the multi-list's `columns`, `columnGap` and `fillAcross` and
folded into the same layout branch; a row is as tall as the tallest pair in it, so a title
that wraps pushes the next row down rather than overlapping the pair beneath it.

`TitleWithSubtitle` and `HeaderWithSubtitle` are that same stack as a **single heading**: the
pair comes off the section's own `text` and `secondaryText` rather than off an entry, and goes
through the very same `layoutTitleSubtitle`. That is why the helper takes the two strings rather
than the thing holding them — a heading and a list entry keep them in different places, and
everything that *shapes* the stack (`subtitleGap`, `subtitleFirst`, both styles) lives on the
section either way. The alternative, a one-entry list, was rejected on both ends: it would put
an entry table in the editor for a section that can only ever hold one pair, and would leave a
second entry to be silently drawn by a type with nowhere to put it.

`secondaryText` is kept whatever the type, like every other field on the struct, so a heading
switched to something else and back keeps its subtitle. A type that does not stack one never
reads it — the layout branch passes an empty string in its place rather than testing the field —
which is what makes a `TitleWithLogo` with a stray subtitle on it measure and draw identically
to one without.

Three things decide how a pair reads:

**`subtitleGap` is a separate number from `entryGap`,** because the two say opposite things:
one binds the two lines of an entry together, the other holds consecutive entries apart. A list
where they are equal is not a list of pairs at all — it is a single run of alternating lines,
with nothing but the styles saying which line belongs to which. That is also why
`makeDefault` hands out a wider `entryGap` than the other list types get, and turns
`useSecondaryStyle` on with a smaller size behind it: a pair drawn in one style at one spacing
is a `TextList` with twice as many rows.

**`subtitleFirst` moves the placement and nothing else.** `text` is still the title in
`style` and `secondaryText` still the subtitle in `secondaryStyle` when it is set, so flipping
the order never migrates content between the two fields and never relabels the columns of the
editor's table. Name-over-role and role-over-name are the same document with one flag between
them.

**Each line is aligned by its own style,** within the full section (or column) width, rather
than the two being measured and placed as one group the way a `Hug` logo row is. A pair is
already the full width of what it is given — unlike that logo row, which is narrower than its
box by construction — so there is no group left to place, and aligning them separately is what
lets a title sit hard left with its subtitle under the right-hand end of it. `sectionAlign`
still moves the whole list, since it moves the box both lines are laid out in. A `Hug` logo row
is the one case where a pair is *not* given the full width: there the group is sized from the
wider of the two lines and both are laid out into that one column, so the two still align
against each other rather than against whatever the section left over.

An empty line takes neither height nor gap with it, so an entry carrying only one of its two
texts occupies exactly what that one line does. That is what lets a heading row sit in an
otherwise paired list without reserving a blank line's worth of space for the text that is
not there — the same courtesy `bridgeSpanEmpty` extends to a bridged row, arrived at without
needing a setting because a stack has nothing to redistribute.

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
(`stylePresetName`, and `secondaryStylePresetName` for whichever second text the type carries
— the right-hand side of a `Bridged` row, or the subtitle of a title/subtitle list);
everything that lays text out goes through `Document::effectiveStyle` /
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

The distance travelled per tick comes from the **video's own frame interval**, not from the
gap `video_tick` reports. That gap is wall-clock and jitters — a percent or two either side of
the interval on an idle machine, more when anything else on the system takes a moment — while
frames are composited on a fixed cadence regardless. Feeding the measured gap straight in moves
the roll a slightly different distance in each equally-spaced frame, and on a page of type that
reads as the roll catching: appearing to stall for an instant and then carry on, because the eye
tracks the text and sees the spacing between successive positions change rather than the clock
those positions came from. A gap close enough to the interval to be that jitter is therefore
taken *as* the interval; one well outside the band is a real stall — a dropped frame, a scene
collection loading — and is used as measured, so the roll keeps its timing over anything long
enough to be worth keeping it over.

**Manual scrolling.** `manualScroll` parks the roll at `scrollPosition` instead of advancing it,
for looking at a section in the middle of a long roll without waiting for the roll to scroll
there. `video_tick` scrubs and returns before any of the playback machinery, so nothing moves
while it is on: the roll cannot reach the finished phase, the ending action cannot fire, and the
start and pause hotkeys have nothing to act on. The phase itself is left exactly as playback set
it, so switching the mode back off resumes from a state `update()` already knows how to re-arm.

The position is a **share of the full travel**, not a pixel offset and not a number of seconds.
Pixels would be re-scaled by every content edit, and OBS cannot re-range a slider without the
properties window being reopened; seconds would move under a change of scroll speed. A percentage
of `canvasHeight + stripHeight` means the same thing after either. It is re-applied every tick
rather than once when the setting changes, because a rebuild finishing or a canvas resize changes
the travel underneath it, and a roll parked halfway through should stay halfway through.

It saves with the scene collection like every other setting here, so it is possible to leave it on
and go live with a roll that never moves. The properties window says so in a warning beside the
slider, and that is the whole of the guard: turning the setting off at some later moment of the
plugin's choosing would be its own surprise, and would mean the source watching frontend events it
otherwise has no use for.

**Rebuilds are gated on content.** Every setting reaches the source through the same `update()`,
and scrubbing sends one per frame of the drag. `renderKey()` reduces the document to everything
the strip is rasterised from — by blanking the playback fields rather than by listing the content
ones, so a field added later counts towards it by default — and a rebuild is queued only when that
string changes. Wrong in the direction the default takes, that costs a redundant rebuild; wrong
the other way it would leave a stale strip on screen. The same comparison decides whether a
running roll restarts, which is what the restart was always for: content moved under the roll, so
its position no longer means anything. A playback setting changing is not that, and a roll now
keeps its place when the scroll speed is adjusted under it.

**Layout boxes.** `render()` optionally fills a `LayoutBoxes` with the rectangle every section,
content area, text block, logo and bridge was placed in, for the designer's layout overlay. They
are collected in the measure pass, which each section goes through exactly once, so they cannot
disagree with what was painted and a section straddling a tile seam is reported once rather than
once per tile. The source passes nothing and pays for none of it.

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

Within that table a row is sized from the **position spin box** and nothing else, and the colour
swatch beside it takes whatever height the row comes out at. A push button asks for more height
than a spin box — several pixels more under the themes OBS ships — and sizing the row against
the taller of the two made every row taller than the value it exists to sit beside, which cost a
stop or two off the bottom of the table and gained nothing that was any easier to read. The
position column is sized from that spin box too: `resizeColumnToContents` measures items, and
every cell here holds a widget instead, so the column was coming out the width of its own header
and clipping the value it was showing.

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
then closing — a window that has nothing to do with what is being edited. There are now three:

| | |
|---|---|
| The properties window's button | the original, unchanged |
| **Tools ▸ Credits Designer** | a submenu of every roll in the collection, by name |
| A **per-source hotkey** | `ClosingTime.Designer`, alongside start/pause/restart |

The Tools entry is a submenu rather than something to click, built through
`obs_frontend_add_tools_menu_qaction` so a `QMenu` can be hung off the action. Picking a name
opens that roll's designer, and the list covers the whole scene collection rather than the
current scene: `obs_enum_sources` walks the collection's inputs, so a roll parked in a scene the
user is not looking at is one hover away rather than unreachable without switching scenes first.
A modal picker would have done the same job and been a worse way to do it — a list of names is a
menu's own work.

The list is rebuilt on `aboutToShow`, which is what keeps it in step with sources being added,
renamed and removed without anything having to watch for any of that, and entries hold **weak**
references, so a roll deleted while the menu happens to be open opens nothing rather than being
kept alive by the menu holding it. It is also filled once at registration, even though nothing is
loaded that early: the macOS menu bar draws an empty submenu as an unusable one, and the
"no sources" placeholder is enough to keep it a submenu until the first hover replaces it.

The source's own **Interact** button was considered and left alone. `OBS_SOURCE_INTERACTION` is
what puts one there, but the button belongs to the frontend: it opens OBS's interaction window,
and no plugin hook exists to put a different window there instead. The nearest thing available is
to open the designer from the focus event that window raises as it appears — which leaves the
interaction window sitting open behind it, with no way for the plugin to close it. A stray window
nobody asked for is a worse trade than one menu entry.

The hotkey arrives on the hotkey thread, and a window can only be opened on the UI one, so it
goes through `openDesignerForAsync`, which queues onto the UI thread holding a weak reference. A
source destroyed between the two ends of that queue makes the task a no-op rather than a crash.

### Preview

The preview pane is the whole roll end to end, at whatever scale fits the canvas across it, with
the wheel moving through it. The dashed frame is the canvas: one screenful, held at the top of
the pane, so the roll runs up through it the way it will on air, and everything below the frame
is dimmed as content that has not reached the screen yet.

Scrolling therefore stops when the last pixel of the roll reaches the **bottom of that frame**,
not the bottom of the pane. The frame is the only part of the pane that says anything about what
goes to air; stopping at the pane's own edge left the end of the roll parked in the dimmed run
below it and refused to bring it any further, so the closing sections could be looked at but
never seen in the frame they are being designed for — and a roll shorter than the pane could not
be scrolled at all.

### Layout overlay

A checkbox under the preview draws the layout's own rectangles over the roll: each section's
box, the content area left inside its margins and padding, and every block of text, logo and
bridge placed within it. It is the answer to "why is this section *there*" — a section box that
turns out to be half the canvas, or a text column that turns out to be the width of its own
words rather than the width of the section, names the setting responsible immediately, where the
rendered pixels alone only show the result.

It is a debugging view rather than part of the design, so it says as much as it can while
getting in the way as little as it can: hairlines rather than fills, the section box dashed and
the content area dotted so both read as bounds rather than as something drawn, and everything
outside the selected section dimmed to a quarter strength. The boxes come back with **every**
preview render rather than only while the overlay is showing, which is what lets it be switched
on over the strip already on screen instead of waiting on a rebuild; the cost is a handful of
rectangles per section against a full rasterisation.

Both halves of the framing below are corrections to an outline that had stopped saying anything. The strip
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
Bridged, title/subtitle for a title/subtitle list, path/height for logo lists, text otherwise
— defaulting to mapping columns onto those fields in order. The two-text types share one
branch off `sectionUsesSecondaryText` and differ only in what the fields are called, which is
what makes a spreadsheet of roles and names import into either shape unchanged. Import
replaces or appends, per a checkbox.

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
- Two more ways into the designer — the Tools menu and a per-source hotkey — so it is no longer
  reached only through the properties window.
- The preview's canvas frame means something again: the canvas is inset from the pane's edges and
  the roll below one screenful is dimmed.
- Colour buttons paint their swatch instead of carrying a stylesheet that leaked into the colour
  dialog they opened.
- A layout overlay in the designer, drawing every section box, content area, text block, logo and
  bridge the layout placed.
- A logo row that moves with its text is placed by the section's own placement, so the setting
  named after placing a section can move one.
- The roll advances by the video's frame interval rather than by a jittering wall-clock delta,
  which is what made it appear to catch every few seconds.
- The preview scrolls until the end of the roll reaches the canvas frame rather than the bottom
  of the pane.
- Gradient stop rows are sized from the value they hold rather than from the button beside it.
- Title/subtitle lists, in one column and over several, for the run of pairs — a position over
  the name that holds it — that the bridged row was the only way to set.
- A bridge can be inked separately from the text either side of it — colour, gradient, outline and
  shadow — without any of the row's geometry moving for it.
- A manual scroll mode on the source, parking the roll at a position on a slider instead of
  playing it, so a section in the middle of a long roll can be looked at while it is being written.
- Rebuilds are queued only when the document's *content* changes, rather than on every settings
  edit, so a slider drag no longer re-rasterises the whole strip once a frame.
- A Section Divider type: an ornamental rule composed from an end cap, a rule and a list of
  centrepieces, drawn from a shape library that takes a new shape without the renderer, the
  editor or the persistence format changing.
- Title and Header types that carry a subtitle of their own, with and without a logo, so the
  stacked pair the lists have always offered can be set as a single heading.

## Verifying changes

The plugin builds clean against libobs and Qt 6 with `-Wextra -Werror`. There is no test
target in the template yet; renderer and parser changes were validated with an offscreen
harness covering the `obs_data` round trip for all nineteen section types, measure/render
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

Section dividers were validated offscreen the same way, over roughly four thousand checks: the
`obs_data` round trip for every new field and every kind of centre piece, including through the
designer's JSON export, and with a stored zero for each of the three gaps told apart from a
missing key; a section written before dividers existed loading with `None` in both end slots,
`Rule` in the arm, mirrored ends, one rule, an empty centre and a drawable thickness; every
built-in shape in every slot it serves, crossed against every other, checking measure/render
agreement and that nothing is drawn outside the content box or into the padding; a stack of one
to five rules drawing one run of rows per rule, symmetric about its midline and never widening
outward; custom artwork taking the section's colour with `dividerTint` on and keeping its own
with it off, and a missing file leaving the rest of the divider drawn; and the degenerate cases
— a divider with no cap, no arm and no centre, one narrower than its own parts, one whose only
centre piece is an empty word, and a hidden one taking no height at all.

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

The layout overlay, the logo row's placement and the two designer sizing fixes were checked
offscreen against the previous build as well as the new one, so each case is known to fail before
it passes: a Hug logo row landing hard left, centred and hard right as the section placement says,
at full width as well as half; every combination of placement, logo side, alignment, margin and
overlong text keeping its ink inside the section box it was given; one section box and one content
box per visible section, contiguous from the lead-in to the strip's end, with every text, logo and
bridge box inside its own section's content area, nothing reported for a hidden section, and a
render that asks for no boxes coming out identical to one that does; the end of the roll reaching
the canvas frame at full scroll for a strip longer than the pane and for one shorter than it,
where the pane previously would not scroll at all; and a stop row staying the height of its
position spin box under a theme whose buttons are taller than one, with the swatch filling that
row and neither scroll bar appearing.

Title/subtitle lists were checked the same way, each case against the previous build as well as
the new one so that it is known to fail before it passes: the two lines of an entry separated by
`subtitleGap` exactly and consecutive entries by `entryGap`, over a list whose two styles are set
at different sizes; every line staying inside the content area it was given; `subtitleFirst`
swapping both which style is drawn on top and which *text* is — checked separately, since a list
whose styles swap without its texts measures identically to one where both do, and only a title
wrapped to two lines against a single-line subtitle at the same size tells them apart — while
leaving the section's height unchanged; an entry with no subtitle measuring exactly as tall as
the same text in a plain `TextList`, and one with no title drawing a single line; a title set
left against a subtitle set right inking opposite halves of the column, read off the rendered
pixels because the layout boxes report the column rather than the ink; a multi-list filling
across and filling down putting the same entries in demonstrably different columns, with each row
clearing the tallest pair above it; the `obs_data` round trip for both new fields across every
section type; a document written before either existed loading title-first with the default gap,
and a stored gap of zero surviving as zero rather than being read back as that default; and
measure/render agreement plus tile contiguity over a document holding all nineteen types.

The subtitle headings were validated offscreen the same way, each case against a deliberately
broken build as well as the working one so that it is known to fail before it passes — a logo
centred on the title line rather than on the block, a leader pinned to the title's baseline
whatever the stacking order says, a `Section::save` that drops the new field, and a `Hug` group
measured from the title alone. Over 311 checks: every section type's id round-tripping and
staying unique, with the four new ids checked by name since a scene collection carries them; the
`obs_data` round trip for `secondaryText` across every type and through the designer's JSON
export, a section written before the type existed loading without a subtitle, and a stored
`subtitleGap` of zero surviving as zero; the defaults, including the logo variants inheriting the
`Hug` placement and the drawn bridge every logo type is handed, and the plain `Title` keeping the
defaults it had; a heading with its subtitle blank measuring *exactly* as tall as a plain
`Title`; the two lines separated by `subtitleGap` and by nothing else, both laid out into the
full content column, with flipping the stack swapping which line is on top and leaving the height
alone; nothing drawn into either padding; the logo row's block centred against the logo rather
than the title line, `logoGap` holding exactly, the hugged column widening for a subtitle wider
than the title with both lines sharing it, and a short logo leaving the pair deciding the row's
height; a `Bridged` logo row's leader staying put when a subtitle is added under it and moving
when the stack is flipped, and an empty heading still placing a leader rather than failing; and
measure/render agreement, tile contiguity and every reported box staying inside its own section's
box over a document holding all nineteen types at once.

The designer's layout was checked offscreen too, by giving the editor more height than it needs
and measuring the largest run of empty space between its visible controls — 6 px for every
section type with the trailing spacer, against 22 px for a Title and 123 px for a Logo Title
without it — along with the entry table holding its minimum height and the section box surviving
a round trip through the editor's widgets.

The bridge's own ink and the manual scroll mode were checked offscreen the same way, each case
against a deliberately broken build as well as the working one so that it is known to fail before
it passes — a merge that replaces the whole style, a renderer still passing the section's style to
`paintBridge`, a `renderKey` that does not blank the scrub fields, and a `video_tick` that falls
through to playback anyway.

For the ink: that switching `useBridgeStyle` on changes **no** measurement anywhere — `measure()`,
every layout box and the strip's own tiling all identical — against a bridge style deliberately
unlike the row's in family, size, weight, alignment, line spacing, fill, gradient, outline and
shadow at once, over the whole bridge type × fill × sizing × one-sided matrix and for a `Bridged`
logo row, since that invariant is the entire justification for merging only the ink; that the
leader really is recoloured, read off the rendered pixels, while the pixels of the text either side
of it come out byte-identical, for a text bridge as well as for dots and diamonds; that
`effectiveBridgeStyle` takes each ink field from the override and each layout field from the row;
that a preset bound to the bridge contributes its colour and not its font size, and that deleting
that preset unbinds the bridge rather than leaving it dangling; that the bridge style's own bleed
is the one `effectBleed` reports when it is the heaviest, with a fixture long enough to be tiled
keeping ink either side of the seam; and the `obs_data` round trip for all three new fields, with a
section written before they existed loading with the override off and its bridge style seeded from
the section's own.

For manual scrolling: that `renderKey()` is unchanged by every one of the playback fields and
changed by every content one, which is the property the whole rebuild gate rests on; that the
parked offset is the right share of the travel at each end and the middle, clamps outside 0–100,
and re-resolves against a strip that has since been rebuilt taller; and that a hundred and twenty
ticks in manual mode leave the offset, the phase and the pending action exactly as they were, with
a control fixture that advances under the same ticks.

What is still unverified: the designer's own wiring, since nothing here drives Qt Widgets, and
`update()`'s re-arm and park paths, which want a live `obs_source_t` rather than a document.

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
