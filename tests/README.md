# The offscreen test harness

Checks for the document model and the renderer, run without OBS, without a display and without
the designer window. The plugin itself still needs a real OBS install to load into — this is for
the half of the codebase that can be answered in a second and a half.

```sh
cmake -S . -B build -DENABLE_TESTS=ON
cmake --build build --target closing-time-tests
./build/tests/closing-time-tests
```

`ENABLE_TESTS` is off by default, so a plain plugin build is exactly what it was.

Needs Qt 6 (Core, Gui, Svg) and the libobs headers. On a fresh Claude Code on the web session the
`SessionStart` hook in `.claude/hooks/` installs both; locally, `qt6-base-dev qt6-svg-dev
libobs-dev` on Debian/Ubuntu.

## Running it

| | |
|---|---|
| `closing-time-tests` | everything; prints only the suites that failed |
| `--verbose` | prints every suite, passing or not |
| `--filter stack` | only the suites whose name contains `stack` |
| `--list` | the suites and the scenes, running nothing |
| `--artifacts out/` | writes a PNG of every scene into `out/` |

`--filter` is the one that matters day to day: the whole run is a few seconds, but a single suite
is tens of milliseconds, which is fast enough to sit in a loop with while a layout is being
worked out.

`ctest` runs the whole thing as one entry, since the suites share a process and a font database
and the harness already reports which of them failed.

## Looking at a graphical change

```sh
./build/tests/closing-time-tests --artifacts /tmp/before
# ... make the change, rebuild ...
./build/tests/closing-time-tests --artifacts /tmp/after
```

The scenes in `harness/Fixtures.cpp` are small documents chosen to show one thing each. Adding one
is three lines and gets you a rendered PNG next to all the others.

**There are deliberately no golden images.** Text rasterizes differently across font versions,
hinting settings and platforms, so a committed PNG would fail on machines where nothing is wrong.
The artifacts are for looking at; anything that must hold is written as a measurement.

## Writing a check

```cpp
CT_SUITE(stack_geometry, "Two lines stacked: the gap inside a pair")
{
        Section section = unpadded(SectionType::TitleWithSubtitle);
        const Document document = documentWith(section);

        const QVector<QRectF> lines = boxesOf(document, LayoutBox::Kind::Text);
        checkEq(lines.size(), 2, "a filled pair places two lines");
        if (lines.size() != 2)
                return;

        checkNear(lines.at(1).top() - lines.at(0).bottom(), section.subtitleGap, 0.5,
                  "the two lines are separated by subtitleGap and nothing else");
}
```

`CT_SUITE` registers the suite before `main` runs; the file only has to be listed in
`tests/CMakeLists.txt`. Every assertion records and returns rather than aborting, so one run
reports everything that moved — which is usually the useful part of a layout change. Guard an
indexed access behind its own `checkEq` and bail, as above, so a broken build reports failures
instead of dying halfway through.

### Boxes or ink?

The two questions the harness answers are not interchangeable, and picking the wrong one is the
main way a check here passes while the thing it names is broken.

- **`boxesOf` / `boxOf`** — where the layout *put* something: the column a run of text was given,
  the rectangle a logo was fitted into. Use for "this column is the full content width".
- **`inkOf`** — where pixels actually landed. Use for "the leader reaches the words": a centered
  title in a wide column inks the middle of it, and only the ink knows that.

### Sweeps

A configuration matrix is one call rather than eight nested loops. Each combination pushes a
`Context`, so a failure out of thousands says which combination raised it:

```cpp
const QVector<Axis> axes = {
        axis<LogoPlacement>("place", {{"edge", LogoPlacement::Edge}, {"hug", LogoPlacement::Hug}},
                            [](Section &s, LogoPlacement v) { s.logoPlacement = v; }),
        axis<HAlign>("align", {{"left", HAlign::Left}, {"center", HAlign::Center}},
                     [](Section &s, HAlign v) { s.style.align = v; }),
};

sweep(base, axes, [&](const Section &section) { ... });
```

The value type is written out (`axis<HAlign>`) because a braced list is not a deduced context.

## Before trusting a new check

**Break the thing it is meant to catch, and watch it fail.** A check that has never failed is a
check that might be asserting nothing — comparing two values that are equal for a reason other
than the one named, or measuring a box when it meant ink. Every suite here was written against a
deliberately broken build first:

```sh
# make the renderer wrong on purpose, rebuild, run, confirm the right suite fails, revert
```

The failures should name the defect. If a break produces no failure, or produces one whose message
does not describe what was broken, the check is not finished.
