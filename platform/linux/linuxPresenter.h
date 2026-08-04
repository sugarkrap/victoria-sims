#ifndef VICTORIA_LINUX_PRESENTER_HEADER
#define VICTORIA_LINUX_PRESENTER_HEADER

#include <X11/Xlib.h>

#include "victoria/coreTypes.h"

/* Gets a frame from the engine onto an X11 window. Two implementations, chosen
   at build time rather than at run time: a device with no usable OpenGL ES 2.0
   driver should not have to link libEGL and libGLESv2 at all, which is exactly
   the case on the older hardware in the device ladder. */

Boolean linuxPresenterCreate(Display *displayConnection, Window windowHandle,
                             Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void linuxPresenterResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void linuxPresenterPresent(void);
void linuxPresenterDestroy(void);

/* Named in the startup log so it is never ambiguous which one is running. */
const char *linuxPresenterGetName(void);

#endif
