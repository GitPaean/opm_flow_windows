# flow-gui application icon

An anticline: arched strata under a seal, with a well into the crest — the trap
geometry a reservoir simulator exists to model. Chosen over a plot or a chart
glyph because a dozen programs draw curves and only this one is about what is
under the ground.

## Files

| | |
|---|---|
| `flow-gui-<n>.png` | 16, 24, 32, 48, 64, 128, 256 px. Compiled into the binary (`qt_add_resources` in `../CMakeLists.txt`) and installed into `share/icons/hicolor/<n>x<n>/apps/flow-gui.png` on Unix. |
| `flow-gui.ico` | The same seven sizes in one file, referenced by `../flow-gui.rc` for the Windows build. |
| `make-icons.cpp` | Draws the PNGs. Not part of the build — see the header comment to rebuild them. |

Each size is drawn at its own size rather than scaled down from the 256, which
is what keeps the bands a whole pixel or two thick at 16px instead of the smear
a downscale gives.

## Colours

The frame is SINTEF's brand blue, read from the department site's stylesheet
(`www.sintef.no/dist/main.2a835d91.css`, where `#003c65` is the dominant colour
and drives `--accordion-head-color`). The strata tones follow the department's
own geomechanics figure, and the well is the SINTEF green from the same sheet.

| | |
|---|---|
| `#003c65` | frame — SINTEF blue |
| `#8c3f3b` | overburden |
| `#f2d08a` | seal |
| `#e69f00` | reservoir |
| `#5a2b2a` | basement |
| `#14b978` | well — SINTEF green |

Deliberately **not** the SINTEF logo. This is an unofficial build (see the
top-level packaging notes), and carrying the mark would say otherwise; the
palette places it without making that claim.
