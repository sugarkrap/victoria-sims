#include "victoria/engineText.h"

#include "utils/checksum.h"
#include "utils/strings.h"
#include "victoria/fontAtlas.h"
#include "victoria/glyphRaster.h"
#include "victoria/interfaceSurface.h"
#include "victoria/platformInterface.h"
#include "victoria/renderInterface.h"

/* The arena everything here is carved from, kept because the font is settled
   later than initialisation — a disc arrives when it arrives. */
static MemoryArena *textArena = NULL_POINTER;
/* The disc, for as long as a step is being taken against it. Held rather than
   passed down five calls: the step machine hands it in and everything below
   reads it, which is the same shape the rest of the engine uses. */
static VirtualFileSystem *fileSystem = NULL_POINTER;

/* ---- The text on the screen -------------------------------------------
 *
 * The debug menu used to be printed to a terminal, which is a fine place for it
 * right up until somebody runs the game the way a game is run. It is drawn now,
 * with the game's own font, read off the disc the way its meshes and textures
 * are.
 *
 * The order of preference is the whole of the design here:
 *
 *   1. a sheet of glyphs left behind by a previous run, read back from the
 *      platform's cache. Nothing is rasterized and the font file is not even
 *      opened. This is the case that matters for the slow end of the device
 *      ladder, where it turns a second of start-up into nothing at all.
 *   2. the disc's own font, rasterized once and then written to that cache so
 *      the next run takes route one.
 *   3. the font the engine carries with it, five pixels by seven, which is what
 *      draws before a disc is open and on a disc with no font on it.
 *
 * A browser has no cache to write to and says so, which lands it on route two
 * every session — and the sheet then lives in an arena for as long as the page
 * does, which is exactly the lifetime an arena is good at. */

#define TEXT_PIXEL_SIZE 14U
#define TEXT_SHEET_WIDTH 256U
#define TEXT_SHEET_HEIGHT 256U
/* Big enough for the menu at its largest and for the one-line hint at its
   smallest. Four bytes a pixel, so this is a megabyte and a half of the budget
   — the price of an interface that draws the same on a machine with shaders and
   one without. */
#define INTERFACE_LIMIT_WIDTH 768U
#define INTERFACE_LIMIT_HEIGHT 512U

/* All transient: taken from the arena while a font is being turned into pixels
   and given straight back. Only the sheet and the surface outlive the build. */
#define FONT_FILE_CAPACITY (320UL * 1024UL)
#define FONT_CACHE_CAPACITY (80UL * 1024UL)
#define FONT_POINT_LIMIT 1024U
#define FONT_CONTOUR_LIMIT 64U
#define GLYPH_EDGE_LIMIT 4096U
#define GLYPH_CROSSING_LIMIT 256U

static FontAtlas textAtlas;
static InterfaceSurface interfaceSurface;
static InterfaceMenuLayout menuLayout;
/* What the pointer is over, and what it was over last time the interface was
   drawn. A button that lights up under the pointer has to be redrawn when the
   pointer moves off it as well as onto it. */
static InterfaceMenuHit menuHovered;
static Integer32 pointerX = -1;
static Integer32 pointerY = -1;
/* Which composition the backend has been given, and what the menu said when it
   was made. Both compared rather than assumed, so a menu nobody is touching
   costs one comparison a frame instead of a layout and an upload. */
static Unsigned32 interfaceRevisionSent = 0U;
static char menuTextDrawn[2048];
static InterfaceMenuHit menuHoveredDrawn;
static Unsigned32 interfaceWindowWidth = 1280U;
static Unsigned32 interfaceWindowHeight = 720U;
static Boolean textIsReady = BOOLEAN_FALSE;
/* Whether the question "is there a font on this disc" has been answered. Asked
   once; the answer may well be no. */
static Boolean fontIsSettled = BOOLEAN_FALSE;

/* A number into a message. engineCore has its own copy of this; two lines
   duplicated is better than a header of shared log helpers nobody owns. */
static void appendUnsigned(char *destination, MemorySize capacity, Unsigned32 value)
{
    char digits[16];

    (void)stringWriteUnsigned(digits, sizeof(digits), (Unsigned64)value);
    (void)stringAppend(destination, capacity, digits);
}

/* The font the engine carries with it. Built before anything else so that a
   failure further down still leaves something able to say so. */
Boolean engineTextInitialize(MemoryArena *arena)
{
    Unsigned8 *sheet;
    Unsigned8 *pixels;

    textArena = arena;
    sheet = (Unsigned8 *)memoryArenaAllocate(
        textArena, (MemorySize)TEXT_SHEET_WIDTH * (MemorySize)TEXT_SHEET_HEIGHT, 16UL);
    pixels = (Unsigned8 *)memoryArenaAllocate(
        textArena,
        (MemorySize)INTERFACE_LIMIT_WIDTH * (MemorySize)INTERFACE_LIMIT_HEIGHT *
            (MemorySize)INTERFACE_BYTES_PER_PIXEL,
        16UL);

    fontAtlasBind(&textAtlas, sheet, TEXT_SHEET_WIDTH, TEXT_SHEET_HEIGHT);
    interfaceSurfaceBind(&interfaceSurface, pixels, INTERFACE_LIMIT_WIDTH,
                         INTERFACE_LIMIT_HEIGHT);
    menuHovered.target = INTERFACE_MENU_NOTHING;
    menuHovered.value = 0U;
    menuHoveredDrawn = menuHovered;
    if (sheet == NULL_POINTER || pixels == NULL_POINTER)
    {
        platformLogMessage("engine: no room for a glyph sheet, so nothing will be drawn in "
                           "words");
        return BOOLEAN_FALSE;
    }
    if (!fontAtlasBuildFromBuiltin(&textAtlas, 2U))
    {
        return BOOLEAN_FALSE;
    }
    textIsReady = BOOLEAN_TRUE;
    menuTextDrawn[0] = '\0';
    return BOOLEAN_TRUE;
}

/* Which file on this disc to read a font out of.
 *
 * Preferring the one the game's own FontStyle.ini names as its default, and
 * falling back to whichever turns up first. Both are guesses about a disc
 * somebody else laid out, so the name of what was chosen is logged — a wrong
 * guess should be readable rather than mysterious. */
static Integer32 findAFontOnThisDisc(void)
{
    Integer32 firstFound = -1;
    Unsigned32 index;

    for (index = 0U; index < fileSystem->entryCount; index++)
    {
        const VirtualFileEntry *entry = virtualFileSystemGetEntry(fileSystem, index);

        if (entry == NULL_POINTER || !stringEndsWithIgnoringCase(entry->path, ".mxf") ||
            entry->sizeInBytes == 0ULL || entry->sizeInBytes > (Unsigned64)FONT_FILE_CAPACITY)
        {
            continue;
        }
        if (stringContainsIgnoringCase(entry->path, "benguiat"))
        {
            return (Integer32)index;
        }
        if (firstFound < 0)
        {
            firstFound = (Integer32)index;
        }
    }
    return firstFound;
}

/* What a cached sheet was made from: which file, how long it is, and at what
 * size it was drawn.
 *
 * Not a checksum of the font's bytes, deliberately — that would mean reading a
 * hundred and eighty kilobytes off the disc before discovering that the answer
 * was already on hand, which is most of the cost this exists to avoid. A path
 * and a length identify a file on a pressed disc perfectly well. */
static Unsigned32 markForFont(const VirtualFileEntry *entry)
{
    Unsigned32 mark = checksumCrc32((const Unsigned8 *)entry->path, stringLength(entry->path));

    mark = (mark * 31U) + (Unsigned32)(entry->sizeInBytes & 0xFFFFFFFFULL);
    mark = (mark * 31U) + (Unsigned32)TEXT_PIXEL_SIZE;
    /* The sheet's shape too. A build compiled with a different one refuses the
       block anyway, but refusing it by the mark says why rather than looking
       like a damaged file. */
    mark = (mark * 31U) + (Unsigned32)TEXT_SHEET_WIDTH;
    mark = (mark * 31U) + (Unsigned32)TEXT_SHEET_HEIGHT;
    /* Nought is what an atlas built from the font we carry ourselves reports,
       and the two must never be mistaken for each other. */
    return (mark == 0U) ? 1U : mark;
}

static void nameTheCache(Unsigned32 mark, char *destination, MemorySize capacity)
{
    char digits[24];

    destination[0] = '\0';
    stringAppend(destination, capacity, "glyphs");
    (void)stringWriteHexadecimal(digits, sizeof(digits), (Unsigned64)mark, 8UL);
    stringAppend(destination, capacity, digits);
}

/* Rasterizes the font at the given index and keeps the result. Every array it
 * needs comes out of the arena and goes back into it before this returns; what
 * survives is the sheet, which was allocated once at start-up.
 *
 * Answers false when the bytes have not arrived yet, which on a browser is the
 * ordinary case and not a failure. */
static Boolean buildTheAtlasFromDisc(Unsigned32 fileIndex, Unsigned32 mark)
{
    MemorySize marker = memoryArenaGetMarker(textArena);
    const VirtualFileEntry *entry = virtualFileSystemGetEntry(fileSystem, fileIndex);
    MemorySize size = (MemorySize)entry->sizeInBytes;
    Unsigned8 *bytes = (Unsigned8 *)memoryArenaAllocate(textArena, size, 8UL);
    GlyphRasterEdge *edges = (GlyphRasterEdge *)memoryArenaAllocate(
        textArena, (MemorySize)GLYPH_EDGE_LIMIT * sizeof(GlyphRasterEdge), 8UL);
    Integer32 *crossingX = (Integer32 *)memoryArenaAllocate(
        textArena, (MemorySize)GLYPH_CROSSING_LIMIT * sizeof(Integer32), 4UL);
    Integer32 *crossingDirection = (Integer32 *)memoryArenaAllocate(
        textArena, (MemorySize)GLYPH_CROSSING_LIMIT * sizeof(Integer32), 4UL);
    Unsigned16 *rowCoverage = (Unsigned16 *)memoryArenaAllocate(
        textArena, (MemorySize)TEXT_SHEET_WIDTH * sizeof(Unsigned16), 2UL);
    Integer32 *pointX = (Integer32 *)memoryArenaAllocate(
        textArena, (MemorySize)FONT_POINT_LIMIT * sizeof(Integer32), 4UL);
    Integer32 *pointY = (Integer32 *)memoryArenaAllocate(
        textArena, (MemorySize)FONT_POINT_LIMIT * sizeof(Integer32), 4UL);
    Unsigned8 *onCurve = (Unsigned8 *)memoryArenaAllocate(textArena, FONT_POINT_LIMIT, 1UL);
    Unsigned32 *contourEnds = (Unsigned32 *)memoryArenaAllocate(
        textArena, (MemorySize)FONT_CONTOUR_LIMIT * sizeof(Unsigned32), 4UL);
    FontReader font;
    FontGlyphOutline outline;
    GlyphRasterizer rasterizer;
    VirtualReadResult read;
    char message[512];

    if (bytes == NULL_POINTER || edges == NULL_POINTER || crossingX == NULL_POINTER ||
        crossingDirection == NULL_POINTER || rowCoverage == NULL_POINTER ||
        pointX == NULL_POINTER || pointY == NULL_POINTER || onCurve == NULL_POINTER ||
        contourEnds == NULL_POINTER)
    {
        memoryArenaRewindToMarker(textArena, marker);
        platformLogMessage("engine: no room to rasterize a font, keeping the built-in one");
        return BOOLEAN_TRUE;
    }

    read = virtualFileSystemReadFile(fileSystem, fileIndex, 0ULL, size, bytes);
    if (read == VIRTUAL_READ_PENDING)
    {
        /* Given back and asked for again next step. The arena is a stack, and
           leaving this allocated across a pend would strand it. */
        memoryArenaRewindToMarker(textArena, marker);
        return BOOLEAN_FALSE;
    }
    if (read != VIRTUAL_READ_OK || !fontReaderOpen(&font, bytes, size))
    {
        memoryArenaRewindToMarker(textArena, marker);
        platformLogMessage("engine: that font would not read, keeping the built-in one");
        return BOOLEAN_TRUE;
    }

    glyphRasterizerBind(&rasterizer, edges, GLYPH_EDGE_LIMIT, crossingX, crossingDirection,
                        GLYPH_CROSSING_LIMIT, rowCoverage, TEXT_SHEET_WIDTH);
    fontOutlineBind(&outline, pointX, pointY, onCurve, contourEnds, FONT_POINT_LIMIT,
                    FONT_CONTOUR_LIMIT);

    if (!fontAtlasBuildFromFont(&textAtlas, &font, &rasterizer, &outline, TEXT_PIXEL_SIZE, mark))
    {
        memoryArenaRewindToMarker(textArena, marker);
        (void)fontAtlasBuildFromBuiltin(&textAtlas, 2U);
        platformLogMessage("engine: that font would not rasterize, keeping the built-in one");
        return BOOLEAN_TRUE;
    }

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: reading text from ");
    stringAppend(message, sizeof(message), entry->path);
    stringAppend(message, sizeof(message), " — ");
    appendUnsigned(message, sizeof(message), font.glyphCount);
    stringAppend(message, sizeof(message), " glyphs in the file, ");
    appendUnsigned(message, sizeof(message),
                   (Unsigned32)FONT_ATLAS_CHARACTER_COUNT - textAtlas.glyphsRefused);
    stringAppend(message, sizeof(message), " drawn at ");
    appendUnsigned(message, sizeof(message), TEXT_PIXEL_SIZE);
    stringAppend(message, sizeof(message), " pixels");
    platformLogMessage(message);

    /* Left for the next run. A platform with nowhere to put it says so, and the
       only consequence is that the next run does this again. */
    {
        Unsigned8 *block =
            (Unsigned8 *)memoryArenaAllocate(textArena, FONT_CACHE_CAPACITY, 8UL);
        MemorySize written = 0UL;
        char name[64];

        if (block != NULL_POINTER)
        {
            written = fontAtlasStore(&textAtlas, block, FONT_CACHE_CAPACITY);
        }
        nameTheCache(mark, name, sizeof(name));
        if (written > 0UL && platformCacheStore(name, block, written))
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: kept those glyphs as ");
            stringAppend(message, sizeof(message), name);
            stringAppend(message, sizeof(message), ", so the next run draws them without "
                                                   "rasterizing anything");
            platformLogMessage(message);
        }
        else
        {
            platformLogMessage("engine: nowhere to keep the glyphs, so every run will "
                               "rasterize them again");
        }
    }

    memoryArenaRewindToMarker(textArena, marker);
    return BOOLEAN_TRUE;
}

/* One step of finding a font. Answers false while a read is outstanding, which
 * is what makes it safe to drive from the same loop as everything else on a
 * store that cannot answer on the spot. */
Boolean engineTextFontIsSettled(void)
{
    return fontIsSettled;
}

void engineTextSetWindowSize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    interfaceWindowWidth = (widthInPixels > 0U) ? widthInPixels : 1U;
    interfaceWindowHeight = (heightInPixels > 0U) ? heightInPixels : 1U;
    /* Forces the next draw. Nothing else would: the menu says the same words in
       a window of any size. */
    menuTextDrawn[0] = '\0';
}

void engineTextForget(void)
{
    menuTextDrawn[0] = '\0';
}

void engineTextSetPointer(Integer32 x, Integer32 y)
{
    pointerX = x;
    pointerY = y;
}

void engineTextForgetPointer(void)
{
    pointerX = -1;
    pointerY = -1;
    menuHovered.target = INTERFACE_MENU_NOTHING;
    menuHovered.value = 0U;
}

InterfaceMenuHit engineTextGetHovered(void)
{
    return menuHovered;
}

void engineTextSetHovered(InterfaceMenuHit hit)
{
    menuHovered = hit;
}

Boolean engineTextStepFont(VirtualFileSystem *store)
{
    Integer32 fileIndex;
    const VirtualFileEntry *entry;
    Unsigned32 mark;
    char name[64];

    fileSystem = store;
    if (!textIsReady || fileSystem == NULL_POINTER)
    {
        fontIsSettled = BOOLEAN_TRUE;
        return BOOLEAN_TRUE;
    }
    fileIndex = findAFontOnThisDisc();
    if (fileIndex < 0)
    {
        fontIsSettled = BOOLEAN_TRUE;
        platformLogMessage("engine: no font on this disc, so text is drawn with the one the "
                           "engine carries");
        return BOOLEAN_TRUE;
    }

    entry = virtualFileSystemGetEntry(fileSystem, (Unsigned32)fileIndex);
    mark = markForFont(entry);
    nameTheCache(mark, name, sizeof(name));

    /* Route one: a sheet from a previous run. The font file is never opened. */
    {
        MemorySize marker = memoryArenaGetMarker(textArena);
        Unsigned8 *block =
            (Unsigned8 *)memoryArenaAllocate(textArena, FONT_CACHE_CAPACITY, 8UL);
        MemorySize read = 0UL;

        if (block != NULL_POINTER)
        {
            read = platformCacheLoad(name, block, FONT_CACHE_CAPACITY);
        }
        if (read > 0UL && fontAtlasRestore(&textAtlas, block, read, mark))
        {
            memoryArenaRewindToMarker(textArena, marker);
            fontIsSettled = BOOLEAN_TRUE;
            menuTextDrawn[0] = '\0';
            platformLogMessage("engine: read this disc's glyphs back from the cache, so "
                               "nothing had to be rasterized");
            return BOOLEAN_TRUE;
        }
        memoryArenaRewindToMarker(textArena, marker);
    }

    if (!buildTheAtlasFromDisc((Unsigned32)fileIndex, mark))
    {
        return BOOLEAN_FALSE;
    }
    fontIsSettled = BOOLEAN_TRUE;
    menuTextDrawn[0] = '\0';
    return BOOLEAN_TRUE;
}

/* A picture for one tile, if there is one.
 *
 * There is not, yet, for any page: body types and animations have nothing that
 * could be a thumbnail, and the clothing page is a list the load does not build
 * yet. The hook is here rather than later because it is what decides the shape
 * of the drawing code, and because the answer for clothing is already sitting
 * in the engine — the garment textures are read to paint the Sim, and a
 * thumbnail is one of them scaled down. */
static Boolean thumbnailForRow(void *context, DebugMenuPage page, Unsigned32 row,
                               const Unsigned8 **rgbaPixels, Unsigned32 *width,
                               Unsigned32 *height)
{
    (void)context;
    (void)page;
    (void)row;
    (void)rgbaPixels;
    (void)width;
    (void)height;
    return BOOLEAN_FALSE;
}

/* Redraws the interface when what it would show has changed.
 *
 * Three things can change it: what the menu says, what the pointer is over, and
 * the size of the window. All three are compared rather than tracked by flags —
 * a flag set in one of the dozen places that can move the menu is a flag that
 * will one day not be set. */
void engineTextDraw(DebugMenu *menu, const char *text)
{
    const char *wanted = text;
    Boolean sameText;

    if (!textIsReady || text == NULL_POINTER)
    {
        return;
    }
    sameText = stringEquals(wanted, menuTextDrawn);
    if (sameText && menuHovered.target == menuHoveredDrawn.target &&
        menuHovered.value == menuHoveredDrawn.value)
    {
        return;
    }
    menuTextDrawn[0] = '\0';
    (void)stringAppend(menuTextDrawn, sizeof(menuTextDrawn), wanted);
    menuHoveredDrawn = menuHovered;

    if (!debugMenuIsOpen(menu))
    {
        Unsigned32 width = 0U;
        Unsigned32 height = 0U;

        /* Shut, and still saying so. A debug feature nobody can discover is a
           debug feature nobody has. */
        if (interfaceMenuMeasureHint(&textAtlas, "menu: press m", &width, &height) &&
            interfaceSurfaceBegin(&interfaceSurface, width, height))
        {
            interfaceMenuDrawHint(&interfaceSurface, &textAtlas, "menu: press m");
        }
        interfaceSurfaceEnd(&interfaceSurface);
    }
    else
    {
        interfaceMenuLayout(&menuLayout, interfaceWindowWidth, interfaceWindowHeight);
        if (menuLayout.width > interfaceSurface.maximumWidth)
        {
            menuLayout.width = interfaceSurface.maximumWidth;
        }
        if (menuLayout.height > interfaceSurface.maximumHeight)
        {
            menuLayout.height = interfaceSurface.maximumHeight;
        }
        /* The keys are told how the tiles are arranged, so down is a row of the
           grid rather than one tile along it — otherwise the cursor walks the
           list in reading order while the eye follows a grid. */
        debugMenuSetGrid(menu, debugMenuGetPage(menu), menuLayout.columns,
                         INTERFACE_MENU_PER_PAGE(&menuLayout));
        if (interfaceSurfaceBegin(&interfaceSurface, menuLayout.width, menuLayout.height))
        {
            interfaceMenuDraw(&interfaceSurface, &menuLayout, menu, &textAtlas, menuHovered,
                              thumbnailForRow, NULL_POINTER);
        }
        interfaceSurfaceEnd(&interfaceSurface);
    }

    if (interfaceSurface.revision != interfaceRevisionSent)
    {
        renderSetOverlay(interfaceSurface.pixels, interfaceSurface.width,
                         interfaceSurface.height);
        interfaceRevisionSent = interfaceSurface.revision;
    }
}

/* Where the pointer is, translated into the menu's own coordinates.
 *
 * The interface is drawn in the top left corner at one pixel to one, so window
 * coordinates and surface coordinates are the same numbers. That is worth
 * writing down rather than relying on: the day the interface is drawn anywhere
 * else, this is the function that has to change and the only one. */
InterfaceMenuHit engineTextHitTest(const DebugMenu *menu)
{
    InterfaceMenuHit nothing;

    nothing.target = INTERFACE_MENU_NOTHING;
    nothing.value = 0U;
    if (!textIsReady || !debugMenuIsOpen(menu) || pointerX < 0 || pointerY < 0)
    {
        return nothing;
    }
    return interfaceMenuHitTest(&menuLayout, menu, pointerX, pointerY);
}
