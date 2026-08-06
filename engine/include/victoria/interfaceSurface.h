#ifndef VICTORIA_INTERFACE_SURFACE_HEADER
#define VICTORIA_INTERFACE_SURFACE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/fontAtlas.h"

/* The one image the engine draws its interface into, and the only thing the
 * three backends are asked to put on screen.
 *
 * This was two channels — a panel and some ink — which was exactly enough for a
 * menu made of text and not enough for a menu made of anything else. Buttons
 * want a fill and a border, a thumbnail wants a picture, and a picture wants
 * colour. So it is four bytes a pixel now, and the trick that let two channels
 * composite in one pass is replaced by the ordinary thing everybody does.
 *
 * Premultiplied alpha: each of red, green and blue is already multiplied by the
 * alpha beside it. Every backend then composites with
 *
 *     out = source + destination * (1 - sourceAlpha)
 *
 * which is one standard blend mode, available on hardware older than this
 * project's floor, and correct when two translucent things are drawn over each
 * other — which straight alpha is not, and which a menu with a panel under a
 * button under a letter does constantly.
 *
 * Everything about layout, fonts, baselines and hit testing happens above this
 * file. Everything about pixels happens in it. A backend that can copy an image
 * can show the whole interface. */

#define INTERFACE_BYTES_PER_PIXEL 4U

typedef struct InterfaceColor
{
    Unsigned8 red;
    Unsigned8 green;
    Unsigned8 blue;
    /* Straight, not premultiplied. The multiplication happens on the way into
       the surface, so callers write the colour they mean. */
    Unsigned8 alpha;
} InterfaceColor;

typedef struct InterfaceSurface
{
    Unsigned8 *pixels;
    Unsigned32 maximumWidth;
    Unsigned32 maximumHeight;

    /* What the current frame's interface actually fills. Nought by nought means
       there is nothing to show, which a backend treats as "take it away". */
    Unsigned32 width;
    Unsigned32 height;

    /* Bumped whenever the pixels change, so a backend uploads on change rather
       than every frame. Nothing else should read it. */
    Unsigned32 revision;

    /* Characters the atlas had no glyph for, and draws that fell outside the
       surface. Both are silent by nature: a missing letter looks like a word
       nobody wrote, and a button drawn off the edge looks like a button that
       was never there. */
    Unsigned32 charactersMissing;
    Unsigned32 drawsClipped;
} InterfaceSurface;

void interfaceSurfaceBind(InterfaceSurface *surface, Unsigned8 *pixels, Unsigned32 maximumWidth,
                          Unsigned32 maximumHeight);

/* Starts a frame at the given size, clearing it to nothing at all. Returns
   false when that will not fit, and leaves the surface empty rather than the
   size it could not have. */
Boolean interfaceSurfaceBegin(InterfaceSurface *surface, Unsigned32 width, Unsigned32 height);

/* Ends it, moving the revision so a backend knows to upload. Separate from
   begin so that a frame drawing nothing still says the interface is gone. */
void interfaceSurfaceEnd(InterfaceSurface *surface);

/* Everything below clips to the surface rather than refusing, so a caller
   laying out a menu wider than the window gets the part that fits. */
void interfaceSurfaceFill(InterfaceSurface *surface, Integer32 left, Integer32 top,
                          Unsigned32 width, Unsigned32 height, InterfaceColor color);

/* A rectangle's edge, drawn inwards from the bounds given, so a border and the
   fill it surrounds take the same four numbers. */
void interfaceSurfaceBorder(InterfaceSurface *surface, Integer32 left, Integer32 top,
                            Unsigned32 width, Unsigned32 height, Unsigned32 thickness,
                            InterfaceColor color);

/* One line of text, with the pen starting at left and the baseline at
   baselineY. Returns where the pen ended, so a caller drawing two colours in
   one line does not have to measure the first half. */
Integer32 interfaceSurfaceText(InterfaceSurface *surface, const FontAtlas *atlas, Integer32 left,
                               Integer32 baselineY, const char *text, InterfaceColor color);

/* An image, scaled to the rectangle by nearest neighbour.
 *
 * The source is STRAIGHT RGBA — which is what a texture off a disc is — and is
 * premultiplied on the way in. Nearest and not smooth: a thumbnail of a garment
 * at a twentieth of its size is being asked what colour something is, not what
 * shape, and a cheap answer to that is the right answer on the hardware at the
 * bottom of the ladder. */
void interfaceSurfaceImage(InterfaceSurface *surface, Integer32 left, Integer32 top,
                           Unsigned32 width, Unsigned32 height, const Unsigned8 *rgbaPixels,
                           Unsigned32 sourceWidth, Unsigned32 sourceHeight);

#endif
