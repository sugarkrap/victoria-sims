#ifndef VICTORIA_BUILTIN_FONT_HEADER
#define VICTORIA_BUILTIN_FONT_HEADER

#include "victoria/coreTypes.h"

/* A font the engine carries with it, so it can always say something.
 *
 * Everything else about text here goes through the disc: the game's own fonts
 * are read the way its meshes and textures are, which is the point of the
 * project. But a disc is not always there. The engine starts before one is
 * opened, a build with no disc at all still draws its placeholder, a disc may
 * turn out to have no fonts on it, and any of those states is one somebody
 * needs to be told about — in words, on the screen, which is exactly what
 * cannot happen if the only way to draw a letter is to have already found one.
 *
 * So: five pixels by seven, ninety-five characters, written out below as
 * pictures rather than as hex. It is authored here and owes nothing to anybody
 * — which is not a stylistic point but the provenance rule, since a font is as
 * much somebody's work as a mesh is and this one has to be ours.
 *
 * Five by seven is the smallest cell that holds a legible Latin alphabet with
 * distinct 'l', '1' and 'I', which is the bar a fallback has to clear. It is
 * not meant to be nice. When a real font arrives this is replaced wholesale. */

#define BUILTIN_FONT_FIRST_CHARACTER 32U
#define BUILTIN_FONT_CHARACTER_COUNT 95U
#define BUILTIN_FONT_WIDTH 5U
#define BUILTIN_FONT_HEIGHT 7U

/* Whether the given row and column of a character's cell has ink in it.
   Anything outside the range answers no, so a caller handed a byte off a disc
   does not have to filter it first. */
Boolean builtinFontHasInk(Unsigned32 codePoint, Unsigned32 column, Unsigned32 row);

#endif
