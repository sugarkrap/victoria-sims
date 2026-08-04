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
#include "victoria/profiler.h"

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
    Unsigned64 profilerReportIntervalMicroseconds;
    Boolean shouldQuit;
} LinuxWindowState;

#define PROFILER_REPORT_DEFAULT_INTERVAL_MICROSECONDS 2000000ULL

static LinuxWindowState windowState;

void platformLogMessage(const char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
}

Unsigned64 platformGetMicroseconds(void)
{
    struct timeval currentTime;

    gettimeofday(&currentTime, NULL_POINTER);
    return ((Unsigned64)currentTime.tv_sec * 1000000ULL) + (Unsigned64)currentTime.tv_usec;
}

static Real32 readMonotonicSeconds(void)
{
    static Boolean originIsRecorded = BOOLEAN_FALSE;
    static Unsigned64 originMicroseconds = 0ULL;
    Unsigned64 currentMicroseconds = platformGetMicroseconds();

    if (originIsRecorded == BOOLEAN_FALSE)
    {
        originMicroseconds = currentMicroseconds;
        originIsRecorded = BOOLEAN_TRUE;
    }

    return (Real32)(currentMicroseconds - originMicroseconds) / 1000000.0f;
}

/* Prints to the terminal on an interval. An on-screen overlay needs text
   rendering, which the engine does not have yet. */
static void printProfilerReportPeriodically(void)
{
    static Unsigned64 lastPrintMicroseconds = 0ULL;
    Unsigned64 nowMicroseconds = platformGetMicroseconds();

    if (windowState.profilerReportIntervalMicroseconds == 0ULL)
    {
        return;
    }

    if (nowMicroseconds - lastPrintMicroseconds < windowState.profilerReportIntervalMicroseconds)
    {
        return;
    }

    lastPrintMicroseconds = nowMicroseconds;
    platformLogMessage("");
    platformLogMessage(engineGetProfilerReportText());
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

/* Busy-waits rather than calling usleep, which under -std=c99 -pedantic would
   need a feature test macro. Spinning also exercises the same clock the
   profiler reads, which is the point of the check. */
static void spinForMicroseconds(Unsigned64 durationMicroseconds)
{
    Unsigned64 startMicroseconds = platformGetMicroseconds();

    while (platformGetMicroseconds() - startMicroseconds < durationMicroseconds)
    {
        /* Intentionally empty. */
    }
}

/* Runs without a display server, so continuous integration can still verify
   that the binary links, the budget is reachable, and the profiler actually
   measures elapsed time rather than merely compiling. */
static int runHeadlessSelfCheck(void)
{
    const Unsigned32 selfCheckFrameCount = 8U;
    const Unsigned32 spinMicrosecondsPerFrame = 2000U;
    MemoryArena *arena = memoryBudgetGetGlobalArena();
    ProfilerFrameSummary frameSummary;
    char reportText[VICTORIA_PROFILER_REPORT_CAPACITY];
    Unsigned32 frameIndex;

    if (profilerInitialize(arena) == BOOLEAN_FALSE)
    {
        platformLogMessage("platform: headless check failed, profiler unavailable");
        return 1;
    }

    for (frameIndex = 0U; frameIndex < selfCheckFrameCount; frameIndex += 1U)
    {
        engineBeginFrame();
        profilerBeginFrame();

        VICTORIA_PROFILE_ZONE_BEGIN("selfCheckOuter");
        VICTORIA_PROFILE_ZONE_BEGIN("selfCheckInner");
        spinForMicroseconds((Unsigned64)spinMicrosecondsPerFrame);
        VICTORIA_PROFILE_ZONE_END();
        VICTORIA_PROFILE_ZONE_END();

        profilerEndFrame();
    }

    profilerGetFrameSummary(&frameSummary);
    profilerWriteReport(reportText, sizeof(reportText));
    platformLogMessage(reportText);

#if VICTORIA_PROFILER_ENABLED
    if (frameSummary.frameIndex != (Unsigned64)selfCheckFrameCount)
    {
        platformLogMessage("platform: headless check failed, frame count wrong");
        return 1;
    }

    if (frameSummary.lastMicroseconds < (Unsigned64)spinMicrosecondsPerFrame)
    {
        platformLogMessage("platform: headless check failed, clock did not advance");
        return 1;
    }

    if (frameSummary.overflowCount != 0U)
    {
        platformLogMessage("platform: headless check failed, profiler overflowed");
        return 1;
    }
#else
    (void)frameSummary;
    (void)selfCheckFrameCount;
    (void)spinMicrosecondsPerFrame;
#endif

    if (memoryArenaAllocate(arena, memoryArenaGetRemainingBytes(arena), 1UL) == NULL_POINTER)
    {
        platformLogMessage("platform: headless check failed, budget unreachable");
        return 1;
    }

    platformLogMessage("platform: headless check passed");
    return 0;
}

int main(int argumentCount, char **argumentValues)
{
    Boolean runHeadlessCheck = BOOLEAN_FALSE;
    int argumentIndex;

    windowState.profilerReportIntervalMicroseconds = PROFILER_REPORT_DEFAULT_INTERVAL_MICROSECONDS;

    for (argumentIndex = 1; argumentIndex < argumentCount; argumentIndex += 1)
    {
        if (stringEquals(argumentValues[argumentIndex], "--check") == BOOLEAN_TRUE)
        {
            runHeadlessCheck = BOOLEAN_TRUE;
        }
        else if (stringEquals(argumentValues[argumentIndex], "--quiet") == BOOLEAN_TRUE)
        {
            windowState.profilerReportIntervalMicroseconds = 0ULL;
        }
    }

    if (runHeadlessCheck == BOOLEAN_TRUE)
    {
        return runHeadlessSelfCheck();
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
        engineBeginFrame();

        VICTORIA_PROFILE_ZONE_BEGIN("platformPumpEvents");
        pumpWindowEvents();
        VICTORIA_PROFILE_ZONE_END();

        engineRenderFrame(readMonotonicSeconds());

        /* Almost always the whole frame: with vertical sync on, this is where
           the wait for the display lands. */
        VICTORIA_PROFILE_ZONE_BEGIN("platformPresent");
        eglSwapBuffers(windowState.displayEGL, windowState.surfaceEGL);
        VICTORIA_PROFILE_ZONE_END();

        engineEndFrame();
        printProfilerReportPeriodically();
    }

    platformLogMessage("");
    platformLogMessage(engineGetProfilerReportText());
    engineShutdown();
    destroyWindowAndContext();
    return 0;
}
