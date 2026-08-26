# Closing Time

An OBS Studio plugin that adds a **Credits Marquee** source: a rolling credits sequence you
design section by section, with logos, multi-column lists, and a configurable action that
fires once the roll clears the screen.

> Status: early. The source renders, scrolls and triggers; the designer window is usable.
> See [ARCHITECTURE.md](ARCHITECTURE.md) for the design and the list of known limitations.

## What it does

Add a **Credits Marquee** source to a scene, open **Credits Designer**, and build the roll
out of sections. The designer asks for a kind of section and then a couple of plain questions
about it — a heading, with a subtitle, with a logo; a list of lines, pairs or images over so many
columns — which between them cover every shape in the table below:

| Section type | What it is |
|---|---|
| Title / Header | A heading, at two default sizes |
| Title w/ Subtitle, Header w/ Subtitle | A heading with a second line under it, in a style of its own |
| Title w/ Logo, Header w/ Logo | A heading with a logo beside it, hugging the text, pinned to the edge, or bridged across to it |
| Title w/ Subtitle & Logo, Header w/ Subtitle & Logo | The same logo beside the stacked pair, centred against both lines |
| Logo Title, Logo Header | A heading that *is* an image — a wordmark, no text |
| Text to Text Bridged | `Director ······ Jane Doe` role-to-person pairs, joined by a leader of dots, dashes, diamonds, a rule, your own SVG, or plain text |
| Text List | A single column of names |
| Title and Subtitle List | Pairs stacked one over the other — a position with the name that holds it under it |
| Logo List | A single column of logos |
| Multi-List of Text | Names across a configurable number of columns |
| Title and Subtitle Multi-List | The same stacked pairs, across a configurable number of columns |
| Multi-List of Logos | Sponsor logos across a configurable number of columns |
| Section Divider | An ornamental rule between sections, composed from its two ends, the rule between them, and whatever sits in the middle |
| Spacer | A blank run, for pacing |
| Sticky Ending Block | Sections that hold still on the frame while the rest of the roll scrolls past behind them — a closing card that stays up, and can be the end of the roll |

Any section can carry a **background** — a colour, a gradient or an image, with rounded corners and
a border — behind the whole section or behind the individual things it draws. See *Backgrounds*
below.

A **title and subtitle** list is the other way to set what a bridged row sets on one line: the
position on top and the name under it, each with its own font, size, weight, colour and
alignment, so the pair reads as one item. The gap inside a pair is set separately from the gap
between pairs — that difference is what groups the two lines together — and the whole list can
be flipped to put the name on top instead without retyping anything. Leave one of the two blank
and the entry takes only the height of the line you did fill in, which is how a heading sits in
the middle of a list. Import a spreadsheet of roles and names straight into either shape.

The **w/ Subtitle** headings are that same pair as a single heading rather than a list — a film
title over its tagline, a section head over the line explaining it, a name under `Directed by`.
Everything the list offers applies: two independent styles, the gap inside the pair, and the
flip that puts the smaller line on top without retyping either field. Leave the subtitle blank
and the section is exactly a plain Title again, to the pixel. The logo variants put a logo
beside the whole pair — centred against both lines together, not just the title — and can hug
it, sit against the section edge, or bridge across to it, with the leader running along the
baseline of whichever line ends up on top.

Either side of a bridged row can carry a **subtitle** of its own — a note under the role on the
left, a company under the name on the right — each in a style of its own, and drawn only where you
put something. The leader stays on the top line of the row, so adding one leaves it where it was.

In a bridged row the **bridge** joining the two texts is drawn from artwork rather than from
characters, so a leader is a real shape at a thickness you set instead of whatever full stops
your font happens to draw: pick dots, dashes, diamonds, a line or a double rule, or point it at
an SVG of your own. It picks up the section's own colour, gradient, outline and shadow, so it
belongs to the row rather than sitting on it, and it can be lifted off the baseline to run a
rule through the middle of the text. Plain text is still one of the styles, for when a word —
`and`, `with` — is what belongs in the gap, and so is nothing at all, for two columns held apart
by a plain gap you set the width of.

Bridged rows are fully placeable on top of that: the bridge can be drawn once, repeated, or
spread to span whatever gap the two texts leave; the columns can be fixed (so the leader starts
at the same place on every row) or sized to the text (so the row reaches both edges); a short
row can hug either edge or sit centred; and a row with only one side filled in can run its
leader out to the far edge, for a heading inside an otherwise bridged list.

A **section divider** is the ornamental rule that separates one part of a roll from the next,
and it is *composed* rather than picked from a list of finished pictures: an end, a rule
running inward from it, and a middle — then the same again mirrored, so the two ends of a rule
cannot drift apart while you work on it. The ends and the middle are the same kind of thing and
draw on the same library of vector shapes — arrowheads, spearpoints, teardrops, scrolled ends,
deco steps and chevrons; diamonds, nested diamonds, four- and five-point stars, dots, hearts,
scrolls, filigree curls and deco interlocks; and the plain geometry under all of it — circles,
squares, rounded squares, triangles, pentagons, hexagons and octagons, each filled or as an
outline — so an arrowhead can break a rule as readily as it caps one, and any of them can instead
be an SVG of your own. The rule between them has a library
of its own: plain, double, tapered, dotted, dashed, diamond-chained and ticked.

Each end, and the middle, is a *list* rather than a single choice, because the figures that turn
up there are compounds: a diamond with a dot either side of it, `PART II` between two curls, a
monogram between a pair of scrolls, `MMXXVI` set against the end of the rule. Pieces can be
ornaments, a word set in the section's own font, or an image, each at a size and an angle of its
own and in whatever order you put them; leave a list empty and you get an end with nothing on it,
or an unbroken rule. A turned piece keeps the room along the rule that its untilted shape had, so
its neighbours stay where they are while you dial an angle in — a square set on its corner, a
triangle pointing up out of the line, a year set sideways — and the far end of a mirrored rule
leans the other way, so the two ends stay each other's reflection. Two
or three rules can run in parallel with a single ornament breaking all of them, and they can be
tapered so the stack forms a wedge. One thickness sizes the whole thing — every shape knows its
own proportion to the rule it belongs to, so an arrowhead stays an arrowhead when a divider is
made heavier.

Because the artwork is a stencil rather than a picture, a divider takes the section's own fill:
the gold sweep on your titles runs across the whole divider as one figure rather than
restarting at every diamond, and it carries the same outline and shadow. Its artwork can also
be inked separately from a word in its middle, the same way a bridge can be inked apart from
the names either side of it.

Each section carries its own font family, size, weight, colour, alignment and line spacing,
plus padding and side margins. It can also be **nudged sideways** — everything the section draws
slides left or right together, keeping the arrangement inside it, which is how a heading sits
deliberately off centre where a margin can only ever inset both edges at once. Within a list,
individual entries can be **tabbed in** by a step you set once for the list: a department over the
names under it, a sub-heading inside a run, or an entry hung back the other way. Text can be filled with a flat colour or with a **linear or
radial gradient** of as many colour stops as you like — a gold sweep down a title, a fade
across a name — and can carry an **outline** and a soft or hard **drop shadow**, which is
what keeps white credits legible over bright footage. The gradient runs across each block of
text rather than across the whole roll, so every name in a list gets the same sweep instead
of a different slice of one long one, and an outline or shadow never moves anything: it
paints outside the text without changing where a line sits or how long the roll runs.

**Backgrounds** put a panel behind a section — a flat colour, a linear or radial gradient, or an
image fitted by cover, contain, stretch or tile — with a corner radius on each of its four corners
and an optional border. A panel can sit behind the whole section or behind the individual things it
draws: the title, the subtitle, each logo, each row of a list, the leader joining a bridged row, or
a divider's artwork. A card behind a wordmark, a band running the full width of the frame behind a
header, a rounded chip behind a name. Give a list a panel for its rows and a second one for every
*other* row and it stripes; leave that alternate empty and every other row is bare instead.

The room inside a panel is the padding the section already has, and the **outset** is what takes it
further out — per side, so a band can run to one edge of the frame and stop short of the other. A
panel is painted, never laid out: switching one on, at any size, cannot move a section, change the
height of the roll or alter how long it runs, which means a roll can be given cards and bands after
it has been timed. Panels save as named presets and publish to the same machine-wide library the
text styles do, in a collection of their own — so a background called `Card` and a heading style
called `Card` are two different things.

Any style can instead be saved as a named **preset**: bind
every header to one and editing it once restyles all of them. Presets can also be published to a
**style library** shared by every roll on the machine — every source, every scene collection,
every profile — and a roll bound to one follows it, so editing the house style once restyles all
of them everywhere. The link is live: a preset edited in another OBS window shows up in the
preview as it is typed. A roll carries its own copy of every style it uses as well, so a scene
collection opened on a machine without the library still renders the way it was saved rather than
unstyled, and editing a shared style from a roll asks whether you mean *this roll* or *all of
them*. Renaming a shared style takes every roll bound to it along — the ones open at the time, and
the scene collections that were not, when they next load. Sections reorder by dragging, edits are undoable (Ctrl+Z), and lists can be typed in directly or
imported from a CSV/TSV file with a column-mapping step.

**Fonts travel with the roll.** A style names a font by family, and a family name means nothing on
a machine that does not have that font — so the roll carries the font files themselves, inside the
scene collection, and renders the same wherever the collection is opened. **Fonts...** in the
designer lists every family the roll is set in and where each one stands; for a font that cannot be
carried, because its licence says so or because it is not a file at all, you name the installed
family that stands in for it, and that choice renders the same everywhere rather than leaving each
machine to pick. Anything still unaccounted for is called out under the preview and in the OBS log
rather than quietly substituted. Carrying font files is redistributing them, so the switch can be
turned off per roll and the window says so where the switch is.

**Logos can move.** Any slot that takes a logo — a heading's, a wordmark, a cell in a sponsor
grid, an ornament in the middle of a divider — takes an animated GIF, APNG or animated WebP.
Video files are not logos and are not played. Each one sets whether it loops or plays
once, whether it starts with the roll or when it scrolls into frame, and how fast it runs against
the artwork's own timing. Animations move with the roll rather than with the clock, so a paused
roll is a still frame, and they carry the section's drop shadow like any other logo — from the
first frame by default, or recomputed per frame for artwork that changes shape. The designer's
preview shows the first frame until you press **Play animations**, so the pane holds still while
a roll is being written. Artwork is decoded whole and up front, which puts a ceiling on it: past
30 seconds a logo plays only its first 30, and the designer says so.

A **sticky ending block** is a run of sections that stops scrolling. It travels up the frame like
any other part of the roll, and when it reaches the place you pinned it to, it stays there while
the rest of the credits carries on past behind it. In the designer's section list a block's contents are drawn as
branches off it, with a count of what it holds and an arrow that folds them away while you work on
the rest of the roll. You pick which part of the block is pinned —
its top edge, its middle, its bottom — and where down the frame that lands, so "centred in the
frame" and "top edge a third of the way down" are both one setting rather than a number to work
out. It can hold for a set time or until something else stops the roll, and when the hold ends it
either stays put and ends the roll, carries on up and off the top, or does both at once. Give it a
background and the credits running past underneath will not read through its lettering — the same
panel every other section carries, so a closing card can have rounded corners and a border on it.

It holds whole sections, so a closing card is a title, a divider and a logo the way anything else
is; drag them under the block in the section list and they are inside it. Nothing above or below
moves for it, and you can have as many as you like — the same mechanism gives you a chapter
heading that holds while the section under it scrolls by.

The source properties cover canvas size, background colour, scroll speed, lead-in and
lead-out padding, start behaviour, looping, and the ending action.

## Playback

- Starts automatically when the source becomes visible (optional), after an optional delay.
- Per-source hotkeys for **start/resume**, **pause**, and **restart**.
- Optional looping, which wraps seamlessly.
- Lead-in and lead-out padding so the roll eases on and fully clears the frame.

## Ending actions

When the last pixel clears the top of the canvas, Closing Time can:

- switch scene, stop recording, stop streaming, stop the virtual camera, hide itself
  (everywhere it appears, nested scenes and groups included), or restart the roll;
- trigger any existing OBS hotkey by name;
- enable, disable or toggle a filter on any source — handy for kicking off a stinger or a
  fade to black;
- with an optional delay before it fires.

Regardless of the action chosen, the source emits a `credits_finished` signal, and a global
`closing_time_finished` signal fires too, so scripts and other plugins can react. Looping
and ending actions are mutually exclusive — a looping roll never ends.

## Building

Requires CMake 3.28+, a C++17 compiler, Qt 6 (Core, Gui, Widgets and Svg — Svg is what draws
the bridge and divider artwork), and the OBS Studio sources/dependencies. The plugin needs both
`ENABLE_QT` and `ENABLE_FRONTEND_API`, which are on by default.

```bash
cmake --preset ubuntu-x86_64      # or windows-x64 / macos
cmake --build --preset ubuntu-x86_64
```

See the [OBS plugin template wiki](https://github.com/obsproject/obs-plugintemplate/wiki)
for platform-specific build setup; this project keeps the template's CMake layout and CI
workflows unchanged.

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).
