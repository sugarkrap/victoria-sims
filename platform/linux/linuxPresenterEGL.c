#include <EGL/egl.h>
#include <X11/Xlib.h>

#include "platform/linux/linuxPresenter.h"
#include "victoria/platformInterface.h"

/* Presents through EGL with an OpenGL ES 2.0 context. The renderer has already
   drawn into the back buffer by the time present is called, so this is just
   the buffer swap. */

typedef struct PresenterStateEGL
{
    EGLDisplay displayEGL;
    EGLSurface surfaceEGL;
    EGLContext contextEGL;
} PresenterStateEGL;

static PresenterStateEGL presenterState;

const char *linuxPresenterGetName(void)
{
    return "OpenGL ES 2.0 (EGL)";
}

Boolean linuxPresenterCreate(Display *displayConnection, Window windowHandle,
                             Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    static const EGLint configurationAttributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    static const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    EGLConfig chosenConfiguration;
    EGLint matchingConfigurationCount = 0;

    (void)widthInPixels;
    (void)heightInPixels;

    presenterState.displayEGL = eglGetDisplay((EGLNativeDisplayType)displayConnection);
    if (presenterState.displayEGL == EGL_NO_DISPLAY)
    {
        platformLogMessage("platform: no EGL display available");
        return BOOLEAN_FALSE;
    }

    if (eglInitialize(presenterState.displayEGL, NULL_POINTER, NULL_POINTER) == EGL_FALSE)
    {
        platformLogMessage("platform: eglInitialize failed");
        return BOOLEAN_FALSE;
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    if (eglChooseConfig(presenterState.displayEGL, configurationAttributes, &chosenConfiguration, 1,
                        &matchingConfigurationCount) == EGL_FALSE ||
        matchingConfigurationCount == 0)
    {
        platformLogMessage("platform: no matching EGL configuration");
        return BOOLEAN_FALSE;
    }

    presenterState.surfaceEGL = eglCreateWindowSurface(presenterState.displayEGL, chosenConfiguration,
                                                       (EGLNativeWindowType)windowHandle, NULL_POINTER);
    if (presenterState.surfaceEGL == EGL_NO_SURFACE)
    {
        platformLogMessage("platform: eglCreateWindowSurface failed");
        return BOOLEAN_FALSE;
    }

    presenterState.contextEGL = eglCreateContext(presenterState.displayEGL, chosenConfiguration,
                                                 EGL_NO_CONTEXT, contextAttributes);
    if (presenterState.contextEGL == EGL_NO_CONTEXT)
    {
        platformLogMessage("platform: eglCreateContext failed");
        return BOOLEAN_FALSE;
    }

    if (eglMakeCurrent(presenterState.displayEGL, presenterState.surfaceEGL, presenterState.surfaceEGL,
                       presenterState.contextEGL) == EGL_FALSE)
    {
        platformLogMessage("platform: eglMakeCurrent failed");
        return BOOLEAN_FALSE;
    }

    /* The render loop presents every frame assuming vertical sync paces it,
       but that is only true once asked for: the interval EGL starts with is
       implementation-defined, and left unset some drivers hand back a buffer
       still in flight, which is what a "not idle" assertion in the platform's
       own present path further down the stack turns into. */
    eglSwapInterval(presenterState.displayEGL, 1);

    return BOOLEAN_TRUE;
}

void linuxPresenterResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    /* EGL tracks the window size itself; the renderer's own viewport update is
       all that is needed. */
    (void)widthInPixels;
    (void)heightInPixels;
}

void linuxPresenterPresent(void)
{
    eglSwapBuffers(presenterState.displayEGL, presenterState.surfaceEGL);
}

void linuxPresenterDestroy(void)
{
    if (presenterState.displayEGL == EGL_NO_DISPLAY)
    {
        return;
    }

    eglMakeCurrent(presenterState.displayEGL, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (presenterState.contextEGL != EGL_NO_CONTEXT)
    {
        eglDestroyContext(presenterState.displayEGL, presenterState.contextEGL);
    }
    if (presenterState.surfaceEGL != EGL_NO_SURFACE)
    {
        eglDestroySurface(presenterState.displayEGL, presenterState.surfaceEGL);
    }
    eglTerminate(presenterState.displayEGL);
    presenterState.displayEGL = EGL_NO_DISPLAY;
}
