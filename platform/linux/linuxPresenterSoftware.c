#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "platform/linux/linuxPresenter.h"
#include "victoria/platformInterface.h"
#include "victoria/softwareSurface.h"

typedef struct SoftwarePresenterState
{
    Display *displayConnection;
    Window windowHandle;
    GC graphicsContext;
    XImage *image;
    Visual *visual;
    int depth;
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
} SoftwarePresenterState;

static SoftwarePresenterState presenterState;

const char *linuxPresenterGetName(void)
{
    return "software (XPutImage)";
}

Boolean linuxPresenterCreate(Display *displayConnection, Window windowHandle,
                             Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    int screenNumber = DefaultScreen(displayConnection);
    Visual *visual = DefaultVisual(displayConnection, screenNumber);
    int depth = DefaultDepth(displayConnection, screenNumber);

    if (depth < 24)
    {
        platformLogMessage("platform: software presenter needs a 24-bit or deeper visual");
        return BOOLEAN_FALSE;
    }

    presenterState.displayConnection = displayConnection;
    presenterState.windowHandle = windowHandle;
    presenterState.widthInPixels = widthInPixels;
    presenterState.heightInPixels = heightInPixels;
    presenterState.graphicsContext = XCreateGC(displayConnection, windowHandle, 0, NULL_POINTER);
    presenterState.visual = visual;
    presenterState.depth = depth;

    return BOOLEAN_TRUE;
}

static Boolean ensureImage(void)
{
    const SoftwareSurface *surface = renderSoftwareGetSurface();

    if (presenterState.image != NULL_POINTER)
    {
        return BOOLEAN_TRUE;
    }
    if (surface->pixels == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }

    presenterState.image = XCreateImage(presenterState.displayConnection, presenterState.visual,
                                        (unsigned int)presenterState.depth, ZPixmap, 0,
                                        (char *)surface->pixels,
                                        surface->pitchInPixels, VICTORIA_SOFTWARE_MAXIMUM_HEIGHT,
                                        32, (int)(surface->pitchInPixels * sizeof(Unsigned32)));
    if (presenterState.image == NULL_POINTER)
    {
        platformLogMessage("platform: XCreateImage failed");
        return BOOLEAN_FALSE;
    }
    return BOOLEAN_TRUE;
}

void linuxPresenterResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    presenterState.widthInPixels = widthInPixels;
    presenterState.heightInPixels = heightInPixels;
}

void linuxPresenterPresent(void)
{
    const SoftwareSurface *surface = renderSoftwareGetSurface();

    if (ensureImage() == BOOLEAN_FALSE)
    {
        return;
    }

    XPutImage(presenterState.displayConnection, presenterState.windowHandle,
              presenterState.graphicsContext, presenterState.image,
              0, 0, 0, 0, surface->widthInPixels, surface->heightInPixels);
    XFlush(presenterState.displayConnection);
}

void linuxPresenterDestroy(void)
{
    if (presenterState.image != NULL_POINTER)
    {
        presenterState.image->data = NULL_POINTER;
        XDestroyImage(presenterState.image);
        presenterState.image = NULL_POINTER;
    }
    if (presenterState.graphicsContext != NULL_POINTER)
    {
        XFreeGC(presenterState.displayConnection, presenterState.graphicsContext);
        presenterState.graphicsContext = NULL_POINTER;
    }
}
