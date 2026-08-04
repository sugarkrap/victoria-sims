#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "platform/linux/linuxPresenter.h"
#include "victoria/platformInterface.h"
#include "victoria/softwareSurface.h"

/* Presents the software framebuffer with plain XPutImage. No EGL, no GLES, no
   shared memory extension: the point of this path is to work on a machine
   whose graphics stack is a framebuffer and nothing more. */

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

/* Deferred to first present because the framebuffer does not exist yet when
   the presenter is created: the window has to exist before the engine starts
   (the OpenGL ES path needs a context to initialise against), and the engine
   is what allocates the framebuffer. Binding early would capture a null
   pointer and present nothing, silently. */
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

    /* The image borrows the engine's framebuffer rather than owning a copy, so
       presenting is one blit and no conversion. It must therefore never be
       freed with XDestroyImage while still holding that pointer. */
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
        /* Detached first: the pixels belong to the arena, and XDestroyImage
           would otherwise try to free them. */
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
