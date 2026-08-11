#ifndef VICTORIA_LINUX_PRESENTER_HEADER
#define VICTORIA_LINUX_PRESENTER_HEADER

#include <X11/Xlib.h>

#include "victoria/coreTypes.h"

Boolean linuxPresenterCreate(Display *displayConnection, Window windowHandle,
                             Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void linuxPresenterResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void linuxPresenterPresent(void);
void linuxPresenterDestroy(void);

const char *linuxPresenterGetName(void);

#endif
