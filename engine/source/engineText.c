#include "victoria/engineText.h"
#include "victoria/engineCore.h"

#include "utils/checksum.h"
#include "utils/strings.h"
#include "victoria/fontAtlas.h"
#include "victoria/glyphRaster.h"
#include "victoria/interfaceSurface.h"
#include "victoria/platformInterface.h"
#include "victoria/renderInterface.h"

static MemoryArena *textArena = NULL_POINTER;
static VirtualFileSystem *fileSystem = NULL_POINTER;

#define TEXT_PIXEL_SIZE 14U
#define TEXT_SHEET_WIDTH 256U
#define TEXT_SHEET_HEIGHT 256U
#define INTERFACE_LIMIT_WIDTH 768U
#define INTERFACE_LIMIT_HEIGHT 512U

#define FONT_FILE_CAPACITY (320UL * 1024UL)
#define FONT_CACHE_CAPACITY (80UL * 1024UL)
#define FONT_POINT_LIMIT 1024U
#define FONT_CONTOUR_LIMIT 64U
#define GLYPH_EDGE_LIMIT 4096U
#define GLYPH_CROSSING_LIMIT 256U

static FontAtlas textAtlas;
static InterfaceSurface interfaceSurface;
static InterfaceMenuLayout menuLayout;
static InterfaceMenuHit menuHovered;
static Integer32 pointerX = -1;
static Integer32 pointerY = -1;
static Unsigned32 interfaceRevisionSent = 0U;
static char menuTextDrawn[2048];
static InterfaceMenuHit menuHoveredDrawn;
static Unsigned32 interfaceWindowWidth = 1280U;
static Unsigned32 interfaceWindowHeight = 720U;
static Boolean textIsReady = BOOLEAN_FALSE;
static Boolean fontIsSettled = BOOLEAN_FALSE;

static void appendUnsigned(char *destination, MemorySize capacity, Unsigned32 value)
{
    char digits[16];

    (void)stringWriteUnsigned(digits, sizeof(digits), (Unsigned64)value);
    (void)stringAppend(destination, capacity, digits);
}

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

static Unsigned32 markForFont(const VirtualFileEntry *entry)
{
    Unsigned32 mark = checksumCrc32((const Unsigned8 *)entry->path, stringLength(entry->path));

    mark = (mark * 31U) + (Unsigned32)(entry->sizeInBytes & 0xFFFFFFFFULL);
    mark = (mark * 31U) + (Unsigned32)TEXT_PIXEL_SIZE;
    mark = (mark * 31U) + (Unsigned32)TEXT_SHEET_WIDTH;
    mark = (mark * 31U) + (Unsigned32)TEXT_SHEET_HEIGHT;
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

Boolean engineTextFontIsSettled(void)
{
    return fontIsSettled;
}

void engineTextSetWindowSize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    interfaceWindowWidth = (widthInPixels > 0U) ? widthInPixels : 1U;
    interfaceWindowHeight = (heightInPixels > 0U) ? heightInPixels : 1U;
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

static Boolean thumbnailForRow(void *context, DebugMenuPage page, Unsigned32 row,
                               const Unsigned8 **rgbaPixels, Unsigned32 *width,
                               Unsigned32 *height)
{
    (void)context;
    if (page != DEBUG_MENU_PAGE_CLOTHING)
    {
        return BOOLEAN_FALSE;
    }
    return engineGetThumbnailPixels(row, rgbaPixels, width, height);
}

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
