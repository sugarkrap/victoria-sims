#ifndef VICTORIA_SOFTWARE_SURFACE_HEADER
#define VICTORIA_SOFTWARE_SURFACE_HEADER

#include "victoria/coreTypes.h"

typedef struct SoftwareSurface
{
    Unsigned32 *pixels;
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
    Unsigned32 pitchInPixels;
} SoftwareSurface;

#ifndef VICTORIA_SOFTWARE_MAXIMUM_WIDTH
#define VICTORIA_SOFTWARE_MAXIMUM_WIDTH 1280U
#endif
#ifndef VICTORIA_SOFTWARE_MAXIMUM_HEIGHT
#define VICTORIA_SOFTWARE_MAXIMUM_HEIGHT 1024U
#endif

const SoftwareSurface *renderSoftwareGetSurface(void);

#endif
