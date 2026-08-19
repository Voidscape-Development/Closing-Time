# Closing Time

An OBS Studio plugin that adds a **Credits Marquee** source: a rolling credits sequence you
design section by section, with logos, multi-column lists, and a configurable action that
fires once the roll clears the screen.

> Status: early. The source renders, scrolls and triggers; the designer window is usable.
> See [ARCHITECTURE.md](ARCHITECTURE.md) for the design and the list of known limitations.

## What it does

Add a **Credits Marquee** source to a scene, open **Credits Designer**, and build the roll
out of sections:

| Section type | What it is |
|---|---|
| Title / Header | A heading, at two default sizes |
| Title w/ Logo, Header w/ Logo | A heading with a logo beside it, hugging the text, pinned to the edge, or bridged across to it |
| Logo Title, Logo Header | A heading that *is* an image — a wordmark, no text |
| Text to Text Bridged | `Director ······ Jane Doe` role-to-person pairs, joined by a leader of dots, dashes, diamonds, a rule, your own SVG, or plain text |
| Text List | A single column of names |
| Title and Subtitle List | Pairs stacked one over the other — a position with the name that holds it under it |
| Logo List | A single column of logos |
| Multi-List of Text | Names across a configurable number of columns |
| Title and Subtitle Multi-List | The same stacked pairs, across a configurable number of columns |
| Multi-List of Logos | Sponsor logos across a configurable number of columns |
| Section Divider | An ornamental rule between sections, composed from end caps, a rule and a centrepiece |
| Spacer | A blank run, for pacing |

A **title and subtitle** list is the other way to set what a bridged row sets on one line: the
position on top and the name under it, each with its own font, size, weight, colour and
alignment, so the pair reads as one item. The gap inside a pair is set separately from the gap
between pairs — that difference is what groups the two lines together — and the whole list can
be flipped to put the name on top instead without retyping anything. Leave one of the two blank
and the entry takes only the height of the line you did fill in, which is how a heading sits in
the middle of a list. Import a spreadsheet of roles and names straight into either shape.

In a bridged row the **bridge** joining the two texts is drawn from artwork rather than from
characters, so a leader is a real shape at a thickness you set instead of whatever full stops
your font happens to draw: pick dots, dashes, diamonds, a line or a double rule, or point it at
an SVG of your own. It picks up the section's own colour, gradient, outline and shadow, so it
belongs to the row rather than sitting on it, and it can be lifted off the baseline to run a
rule through the middle of the text. Plain text is still one of the styles, for when a word —
`and`, `with` — is what belongs in the gap.

Bridged rows are fully placeable on top of that: the bridge can be drawn once, repeated, or
spread to span whatever gap the two texts leave; the columns can be fixed (so the leader starts
at the same place on every row) or sized to the text (so the row reaches both edges); a short
row can hug either edge or sit centred; and a row with only one side filled in can run its
leader out to the far edge, for a heading inside an otherwise bridged list.

A **section divider** is the ornamental rule that separates one part of a roll from the next,
and it is *composed* rather than picked from a list of finished pictures: an end cap, a rule
running inward from it, and a centre — then the same again mirrored, so the two ends of a rule
cannot drift apart while you work on it. Each of the three is drawn from a small library of
vector shapes — arrowheads, spearpoints, teardrops, scrolled ends and deco steps for the ends;
plain, double, tapered, dotted, dashed, diamond-chained and ticked rules; diamonds, nested
diamonds, four- and five-point stars, hearts, scrolls, filigree curls and deco interlocks for
the middle — and any of the three can instead be an SVG of your own. Two dozen shapes across
three slots is a great many more dividers than there are shapes.

The middle is a *list* rather than a single choice, because the figures that turn up there are
compounds: a diamond with a dot either side of it, `PART II` between two curls, a monogram
between a pair of scrolls. Pieces can be ornaments, a word set in the section's own font, or an
image, in whatever order you put them; leave the list empty and you get an unbroken rule. Two
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
plus padding and side margins. Text can be filled with a flat colour or with a **linear or
radial gradient** of as many colour stops as you like — a gold sweep down a title, a fade
across a name — and can carry an **outline** and a soft or hard **drop shadow**, which is
what keeps white credits legible over bright footage. The gradient runs across each block of
text rather than across the whole roll, so every name in a list gets the same sweep instead
of a different slice of one long one, and an outline or shadow never moves anything: it
paints outside the text without changing where a line sits or how long the roll runs.

Any style can instead be saved as a named **preset**: bind
every header to one and editing it once restyles all of them. Sections reorder by dragging,
edits are undoable (Ctrl+Z), and a font the machine does not have is called out under the
preview rather than quietly substituted. Lists can be typed in directly or imported from a
CSV/TSV file with a column-mapping step.

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
