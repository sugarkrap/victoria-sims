# Test assets

Fixtures for the package reader. **None of this is retail game data**, and
nothing derived from a retail install may ever be added here.

Everything in `scenegraph/` came from the upstream
[OpenTS2](https://github.com/LazyDuchess/OpenTS2) project's `TestAssets`
directory, whose own README states these were "resources made specifically for
unit testing". The content bears that out: the model is the Utah teapot, and
the texture is the Creative Commons Zero mark — an asset chosen precisely
because it carries no rights. The source assets are kept alongside the packages
they were built from, so the provenance of each is checkable rather than taken
on trust.

| File | Resources | Built from |
| --- | --- | --- |
| `teapot_model.package` | CRES, SHPE, GMND, GMDC | `teapot.stl` |
| `textures.package` | TXTR ×4, LIFO | `cc0-logo.png`, `brick-texture.png` |
| `material_definition.package` | TXMT | — |
| `animation.package` | ANIM ×3 | — |

Together those cover the whole scenegraph chain the renderer has to walk:
CRES → SHPE → GMND → GMDC, plus TXMT and TXTR for materials. That is enough to
test the reader end to end without anyone needing a copy of the game.

Upstream's filenames are kept as-is rather than renamed to this project's
conventions, so a file here can still be diffed against the version it came
from.

## What is deliberately absent

Upstream also ships `TestAssets/Codecs/` — effects, lot info, lot objects,
neighbourhood decorations, object codecs, sim data. Those are not here. A
273 KB effects blob is not something anyone hand-authored for a unit test, and
their provenance cannot be established, so they are treated as retail-derived.

## Adding to this directory

`tools/checkNoGameData.sh` pins every file here by SHA-256 against
`manifest.sha256`, and continuous integration runs it. Adding or changing a
fixture means updating that manifest deliberately — a retail package cannot be
dropped in quietly, and neither can a fixture be swapped for one.
