#include <EGL/egl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

#include "victoria/engineCore.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/memoryBudget.h"
#include "victoria/platformInterface.h"

#define WINDOW_INITIAL_WIDTH 1024u
#define WINDOW_INITIAL_HEIGHT 576u

typedef struct LinuxWindowState
{
    Display *displayConnection;
    Window windowHandle;
    Atom deleteWindowAtom;
    EGLDisplay displayEGL;
    EGLSurface surfaceEGL;
    EGLContext contextEGL;
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
    Boolean shouldQuit;
} LinuxWindowState;

static LinuxWindowState windowState;

void platformLogMessage(const char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
}

static Real32 readMonotonicSeconds(void)
{
    /* gettimeofday rather than clock_gettime: it is present on the 2.4-era
       kernels and C libraries this target has to run on. */
    static Boolean originIsRecorded = BOOLEAN_FALSE;
    static struct timeval originTime;
    struct timeval currentTime;

    gettimeofday(&currentTime, NULL_POINTER);
    if (originIsRecorded == BOOLEAN_FALSE)
    {
        originTime = currentTime;
        originIsRecorded = BOOLEAN_TRUE;
    }

    return (Real32)(currentTime.tv_sec - originTime.tv_sec) +
           ((Real32)(currentTime.tv_usec - originTime.tv_usec) / 1000000.0f);
}

static Boolean createWindowAndContext(void)
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
    Window rootWindow;
    XSetWindowAttributes windowAttributes;

    windowState.displayConnection = XOpenDisplay(NULL_POINTER);
    if (windowState.displayConnection == NULL_POINTER)
    {
        platformLogMessage("platform: cannot open X display (is DISPLAY set?)");
        return BOOLEAN_FALSE;
    }

    rootWindow = DefaultRootWindow(windowState.displayConnection);
    windowAttributes.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;

    windowState.windowHandle = XCreateWindow(
        windowState.displayConnection, rootWindow,
        0, 0, WINDOW_INITIAL_WIDTH, WINDOW_INITIAL_HEIGHT, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWEventMask, &windowAttributes);

    XStoreName(windowState.displayConnection, windowState.windowHandle, "Victoria Sims");
    XMapWindow(windowState.displayConnection, windowState.windowHandle);

    windowState.deleteWindowAtom = XInternAtom(windowState.displayConnection, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(windowState.displayConnection, windowState.windowHandle, &windowState.deleteWindowAtom, 1);

    windowState.displayEGL = eglGetDisplay((EGLNativeDisplayType)windowState.displayConnection);
    if (windowState.displayEGL == EGL_NO_DISPLAY)
    {
        platformLogMessage("platform: no EGL display available");
        return BOOLEAN_FALSE;
    }

    if (eglInitialize(windowState.displayEGL, NULL_POINTER, NULL_POINTER) == EGL_FALSE)
    {
        platformLogMessage("platform: eglInitialize failed");
        return BOOLEAN_FALSE;
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    if (eglChooseConfig(windowState.displayEGL, configurationAttributes, &chosenConfiguration, 1,
                        &matchingConfigurationCount) == EGL_FALSE ||
        matchingConfigurationCount == 0)
    {
        platformLogMessage("platform: no matching EGL configuration");
        return BOOLEAN_FALSE;
    }

    windowState.surfaceEGL = eglCreateWindowSurface(windowState.displayEGL, chosenConfiguration,
                                                    (EGLNativeWindowType)windowState.windowHandle,
                                                    NULL_POINTER);
    if (windowState.surfaceEGL == EGL_NO_SURFACE)
    {
        platformLogMessage("platform: eglCreateWindowSurface failed");
        return BOOLEAN_FALSE;
    }

    windowState.contextEGL = eglCreateContext(windowState.displayEGL, chosenConfiguration, EGL_NO_CONTEXT,
                                              contextAttributes);
    if (windowState.contextEGL == EGL_NO_CONTEXT)
    {
        platformLogMessage("platform: eglCreateContext failed");
        return BOOLEAN_FALSE;
    }

    if (eglMakeCurrent(windowState.displayEGL, windowState.surfaceEGL, windowState.surfaceEGL,
                       windowState.contextEGL) == EGL_FALSE)
    {
        platformLogMessage("platform: eglMakeCurrent failed");
        return BOOLEAN_FALSE;
    }

    windowState.widthInPixels = WINDOW_INITIAL_WIDTH;
    windowState.heightInPixels = WINDOW_INITIAL_HEIGHT;
    return BOOLEAN_TRUE;
}

static void destroyWindowAndContext(void)
{
    if (windowState.displayEGL != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(windowState.displayEGL, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (windowState.contextEGL != EGL_NO_CONTEXT)
        {
            eglDestroyContext(windowState.displayEGL, windowState.contextEGL);
        }
        if (windowState.surfaceEGL != EGL_NO_SURFACE)
        {
            eglDestroySurface(windowState.displayEGL, windowState.surfaceEGL);
        }
        eglTerminate(windowState.displayEGL);
    }

    if (windowState.displayConnection != NULL_POINTER)
    {
        XDestroyWindow(windowState.displayConnection, windowState.windowHandle);
        XCloseDisplay(windowState.displayConnection);
    }
}

static void pumpWindowEvents(void)
{
    XEvent event;

    while (XPending(windowState.displayConnection) > 0)
    {
        XNextEvent(windowState.displayConnection, &event);

        switch (event.type)
        {
        case ConfigureNotify:
            if ((Unsigned32)event.xconfigure.width != windowState.widthInPixels ||
                (Unsigned32)event.xconfigure.height != windowState.heightInPixels)
            {
                windowState.widthInPixels = (Unsigned32)event.xconfigure.width;
                windowState.heightInPixels = (Unsigned32)event.xconfigure.height;
                engineResize(windowState.widthInPixels, windowState.heightInPixels);
            }
            break;

        case KeyPress:
            /* Keycode 9 is Escape on every X server we care about. */
            if (event.xkey.keycode == 9)
            {
                windowState.shouldQuit = BOOLEAN_TRUE;
            }
            break;

        case ClientMessage:
            if ((Atom)event.xclient.data.l[0] == windowState.deleteWindowAtom)
            {
                windowState.shouldQuit = BOOLEAN_TRUE;
            }
            break;

        default:
            break;
        }
    }
}

int main(int argumentCount, char **argumentValues)
{
    Boolean runHeadlessCheck = BOOLEAN_FALSE;
    int argumentIndex;

    for (argumentIndex = 1; argumentIndex < argumentCount; argumentIndex += 1)
    {
        if (stringEquals(argumentValues[argumentIndex], "--check") == BOOLEAN_TRUE)
        {
            runHeadlessCheck = BOOLEAN_TRUE;
        }
    }

    if (runHeadlessCheck == BOOLEAN_TRUE)
    {
        /* Proves the binary links and the memory budget is actually reachable
           without needing a display server, which is all continuous
           integration can verify. */
        MemoryArena *arena = memoryBudgetGetGlobalArena();
        void *wholeBudget = memoryArenaAllocate(arena, VICTORIA_MEMORY_BUDGET_BYTES, 16UL);

        if (wholeBudget == NULL_POINTER)
        {
            platformLogMessage("platform: headless check failed, budget unreachable");
            return 1;
        }

        platformLogMessage("platform: headless check passed");
        return 0;
    }

    windowState.displayConnection = NULL_POINTER;
    windowState.displayEGL = EGL_NO_DISPLAY;
    windowState.surfaceEGL = EGL_NO_SURFACE;
    windowState.contextEGL = EGL_NO_CONTEXT;
    windowState.shouldQuit = BOOLEAN_FALSE;

    if (createWindowAndContext() == BOOLEAN_FALSE)
    {
        destroyWindowAndContext();
        return 1;
    }

    if (engineInitialize(windowState.widthInPixels, windowState.heightInPixels) == BOOLEAN_FALSE)
    {
        destroyWindowAndContext();
        return 1;
    }

    while (windowState.shouldQuit == BOOLEAN_FALSE)
    {
        pumpWindowEvents();
        engineRenderFrame(readMonotonicSeconds());
        eglSwapBuffers(windowState.displayEGL, windowState.surfaceEGL);
    }

    engineShutdown();
    destroyWindowAndContext();
    return 0;
}
