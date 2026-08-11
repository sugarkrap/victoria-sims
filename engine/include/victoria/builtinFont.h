#ifndef VICTORIA_BUILTIN_FONT_HEADER
#define VICTORIA_BUILTIN_FONT_HEADER

#include "victoria/coreTypes.h"

#define BUILTIN_FONT_FIRST_CHARACTER 32U
#define BUILTIN_FONT_CHARACTER_COUNT 95U
#define BUILTIN_FONT_WIDTH 5U
#define BUILTIN_FONT_HEIGHT 7U

Boolean builtinFontHasInk(Unsigned32 codePoint, Unsigned32 column, Unsigned32 row);

#endif
