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
| Text to Text Bridged | `Director . . . . . . Jane Doe` role-to-person pairs, with leaders that can repeat or stretch to fill the row |
| Text List | A single column of names |
| Logo List | A single column of logos |
| Multi-List of Text | Names across a configurable number of columns |
| Multi-List of Logos | Sponsor logos across a configurable number of columns |
| Spacer | A blank run, for pacing |

Bridged rows are fully placeable: the bridge can be drawn once, repeated, or stretched to
span whatever gap the two texts leave; the columns can be fixed (so the leader starts at the
same place on every row) or sized to the text (so the row reaches both edges); a short row
can hug either edge or sit centred; and a row with only one side filled in can run its
leader out to the far edge, for a heading inside an otherwise bridged list.

Each section carries its own font family, size, weight, colour, alignment and line spacing,
plus padding and side margins. Any style can instead be saved as a named **preset**: bind
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

Requires CMake 3.28+, a C++17 compiler, Qt 6, and the OBS Studio sources/dependencies. The
plugin needs both `ENABLE_QT` and `ENABLE_FRONTEND_API`, which are on by default.

```bash
cmake --preset ubuntu-x86_64      # or windows-x64 / macos
cmake --build --preset ubuntu-x86_64
```

See the [OBS plugin template wiki](https://github.com/obsproject/obs-plugintemplate/wiki)
for platform-specific build setup; this project keeps the template's CMake layout and CI
workflows unchanged.

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).
