#ifndef VICTORIA_SOFTWARE_SURFACE_HEADER
#define VICTORIA_SOFTWARE_SURFACE_HEADER

#include "victoria/coreTypes.h"

/* The target the software backend draws into, and the thing the platform
   layer hands to the window system. Pixels are 32-bit 0x00RRGGBB in host byte
   order, which is what X11 wants for a 24-bit TrueColor visual and avoids a
   conversion pass on the way out. */
typedef struct SoftwareSurface
{
    Unsigned32 *pixels;
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
    /* In pixels, not bytes. Equal to the width today, but kept separate so a
       backing store wider than the visible area does not need every loop
       rewritten. */
    Unsigned32 pitchInPixels;
} SoftwareSurface;

/* Bounds the arena reservation. A backing store is allocated once at this size
   and never grows, so a resize past it is clamped rather than reallocated —
   there is no allocator to reallocate with. */
#ifndef VICTORIA_SOFTWARE_MAXIMUM_WIDTH
#define VICTORIA_SOFTWARE_MAXIMUM_WIDTH 1280U
#endif
#ifndef VICTORIA_SOFTWARE_MAXIMUM_HEIGHT
#define VICTORIA_SOFTWARE_MAXIMUM_HEIGHT 1024U
#endif

/* Valid only after renderInitialize has succeeded. */
const SoftwareSurface *renderSoftwareGetSurface(void);

#endif
