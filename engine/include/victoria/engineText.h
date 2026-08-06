#ifndef VICTORIA_ENGINE_TEXT_HEADER
#define VICTORIA_ENGINE_TEXT_HEADER

#include "victoria/coreTypes.h"
#include "victoria/debugMenu.h"
#include "victoria/interfaceMenu.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

/* Everything between a font on a disc and words on the screen.
 *
 * The pieces underneath are each their own module already — fontReader.h reads
 * the container, glyphRaster.h turns outlines into coverage, fontAtlas.h keeps
 * the result, interfaceSurface.h holds the pixels and interfaceMenu.h lays the
 * menu out. This is the part that knows the ORDER: try the cache, then the
 * disc, then the font we carry ourselves, and redraw when what would be shown
 * has changed.
 *
 * It lives out here rather than in engineCore because none of that order has
 * anything to do with assembling a Sim, and because what it needs from the rest
 * of the engine is four things it can be handed: an arena, a file system, the
 * menu's state, and the words. Everything else it owns.
 *
 * Nothing here is required to work. A machine with no cache rasterizes on every
 * run; a disc with no font on it draws with the built-in one; an arena with no
 * room for a glyph sheet draws nothing and says so to the log. */

/* Reserves the glyph sheet and the interface surface, and builds the font the
   engine carries with it — so that a failure anywhere later can still be
   reported in words on the screen. False when there was no room, and every
   call below then does nothing rather than needing to be guarded. */
Boolean engineTextInitialize(MemoryArena *arena);

/* One step of finding a font on the disc.
 *
 * Answers false while a read is outstanding, which on a browser is the ordinary
 * case: the caller comes back next step. Answers true when the question is
 * settled, however it settled — a cached sheet, a rasterized one, or no font on
 * this disc at all. */
Boolean engineTextStepFont(VirtualFileSystem *fileSystem);
Boolean engineTextFontIsSettled(void);

/* The window the menu sizes itself against. A menu that filled the screen would
   be a screen with no Sim on it, which is the thing being looked at. */
void engineTextSetWindowSize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);

/* Redraws when what would be shown has changed, and hands the result to the
 * backend when it has. Three things can change it — the menu's words, what the
 * pointer is over, the size of the window — and all three are compared rather
 * than tracked by a flag, because a flag set in one of a dozen places is a flag
 * that will one day not be set. */
/* Not const: drawing settles how the tiles are arranged, and the keys have to
   be told, or the cursor walks the list in reading order while the eye follows
   a grid. */
void engineTextDraw(DebugMenu *menu, const char *text);

/* Forces the next draw, for a change the comparison cannot see. */
void engineTextForget(void);

/* What the pointer is over, and where it is. The engine decides what a click
   MEANS; this only says what was under it. */
void engineTextSetPointer(Integer32 x, Integer32 y);
void engineTextForgetPointer(void);
InterfaceMenuHit engineTextGetHovered(void);
void engineTextSetHovered(InterfaceMenuHit hit);
InterfaceMenuHit engineTextHitTest(const DebugMenu *menu);

#endif
