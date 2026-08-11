#ifndef VICTORIA_INTERFACE_SURFACE_HEADER
#define VICTORIA_INTERFACE_SURFACE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/fontAtlas.h"

#define INTERFACE_BYTES_PER_PIXEL 4U

typedef struct InterfaceColor
{
    Unsigned8 red;
    Unsigned8 green;
    Unsigned8 blue;
    Unsigned8 alpha;
} InterfaceColor;

typedef struct InterfaceSurface
{
    Unsigned8 *pixels;
    Unsigned32 maximumWidth;
    Unsigned32 maximumHeight;

    Unsigned32 width;
    Unsigned32 height;

    Unsigned32 revision;

    Unsigned32 charactersMissing;
    Unsigned32 drawsClipped;
} InterfaceSurface;

void interfaceSurfaceBind(InterfaceSurface *surface, Unsigned8 *pixels, Unsigned32 maximumWidth,
                          Unsigned32 maximumHeight);

Boolean interfaceSurfaceBegin(InterfaceSurface *surface, Unsigned32 width, Unsigned32 height);

void interfaceSurfaceEnd(InterfaceSurface *surface);

void interfaceSurfaceFill(InterfaceSurface *surface, Integer32 left, Integer32 top,
                          Unsigned32 width, Unsigned32 height, InterfaceColor color);

void interfaceSurfaceBorder(InterfaceSurface *surface, Integer32 left, Integer32 top,
                            Unsigned32 width, Unsigned32 height, Unsigned32 thickness,
                            InterfaceColor color);

Integer32 interfaceSurfaceText(InterfaceSurface *surface, const FontAtlas *atlas, Integer32 left,
                               Integer32 baselineY, const char *text, InterfaceColor color);

void interfaceSurfaceImage(InterfaceSurface *surface, Integer32 left, Integer32 top,
                           Unsigned32 width, Unsigned32 height, const Unsigned8 *rgbaPixels,
                           Unsigned32 sourceWidth, Unsigned32 sourceHeight);

#endif
