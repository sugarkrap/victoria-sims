#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "platform/linux/linuxPresenter.h"
#include "victoria/engineCore.h"
#include "platform/linux/linuxDiscStore.h"
#include "utils/strings.h"
#include "victoria/discReader.h"
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
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
    Unsigned64 profilerReportIntervalMicroseconds;
    Boolean shouldQuit;
} LinuxWindowState;

#define PROFILER_REPORT_DEFAULT_INTERVAL_MICROSECONDS 2000000ULL

/* Files an inspection will catalogue. The engine's own limit is the same order;
   this one is separate because inspecting a disc is not loading one, and a
   number chosen for a load should not quietly bound a diagnostic. */
#define INSPECT_FILE_LIMIT 4096U

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

/* Where a cached block lands: under the user's own cache directory, following
 * the same convention every other program on the machine does. Not beside the
 * executable, which may be read-only and is shared between users, and not in a
 * temporary directory, which is emptied by the thing that makes caching
 * pointless.
 *
 * Returns false rather than inventing somewhere when there is no home to put it
 * under. Caching is an optimisation and never a requirement: without it the
 * engine rasterizes a font on every run and is a second slower to start. */
static Boolean buildCachePath(const char *name, char *destination, MemorySize capacity)
{
    const char *base = getenv("XDG_CACHE_HOME");

    destination[0] = '\0';
    if (base == NULL_POINTER || base[0] != '/')
    {
        const char *home = getenv("HOME");

        if (home == NULL_POINTER || home[0] != '/')
        {
            return BOOLEAN_FALSE;
        }
        (void)stringAppend(destination, capacity, home);
        (void)stringAppend(destination, capacity, "/.cache");
    }
    else
    {
        (void)stringAppend(destination, capacity, base);
    }
    (void)stringAppend(destination, capacity, "/victoriaSims");
    /* Made if it is not there, ignored if it is. A failure here is caught by
       the open that follows, which has to be checked anyway. */
    (void)mkdir(destination, 0700);
    (void)stringAppend(destination, capacity, "/");

    /* The name is an identifier and not a path, and this is where that is
       enforced rather than trusted. A name carrying a slash or a pair of dots
       would write wherever it liked, and the engine composes these from things
       it read off a disc somebody else wrote. */
    while (*name != '\0')
    {
        char character = *name;
        char text[2];

        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '-' || character == '_'))
        {
            return BOOLEAN_FALSE;
        }
        text[0] = character;
        text[1] = '\0';
        (void)stringAppend(destination, capacity, text);
        name++;
    }
    return (stringLength(destination) + 1UL < capacity) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

Boolean platformCacheStore(const char *name, const Unsigned8 *bytes, MemorySize byteCount)
{
    char path[512];
    FILE *handle;
    MemorySize written;

    if (name == NULL_POINTER || bytes == NULL_POINTER || byteCount == 0UL ||
        !buildCachePath(name, path, sizeof(path)))
    {
        return BOOLEAN_FALSE;
    }
    handle = fopen(path, "wb");
    if (handle == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }
    written = (MemorySize)fwrite(bytes, 1, (size_t)byteCount, handle);
    (void)fclose(handle);
    return (written == byteCount) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

MemorySize platformCacheLoad(const char *name, Unsigned8 *destination, MemorySize capacity)
{
    char path[512];
    FILE *handle;
    MemorySize read;

    if (name == NULL_POINTER || destination == NULL_POINTER || capacity == 0UL ||
        !buildCachePath(name, path, sizeof(path)))
    {
        return 0UL;
    }
    handle = fopen(path, "rb");
    if (handle == NULL_POINTER)
    {
        return 0UL;
    }
    read = (MemorySize)fread(destination, 1, (size_t)capacity, handle);
    (void)fclose(handle);
    return read;
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
    Window rootWindow;
    XSetWindowAttributes windowAttributes;

    windowState.displayConnection = XOpenDisplay(NULL_POINTER);
    if (windowState.displayConnection == NULL_POINTER)
    {
        platformLogMessage("platform: cannot open X display (is DISPLAY set?)");
        return BOOLEAN_FALSE;
    }

    rootWindow = DefaultRootWindow(windowState.displayConnection);
    /* Motion as well as buttons: a menu whose buttons light up under the
       pointer has to hear about the pointer moving, not only about it being
       pressed. LeaveWindow matters for the same reason in reverse — without it
       a button stays lit after the pointer has gone somewhere else entirely. */
    windowAttributes.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask |
                                  ButtonPressMask | PointerMotionMask | LeaveWindowMask;

    windowState.windowHandle = XCreateWindow(
        windowState.displayConnection, rootWindow,
        0, 0, WINDOW_INITIAL_WIDTH, WINDOW_INITIAL_HEIGHT, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWEventMask, &windowAttributes);

    XStoreName(windowState.displayConnection, windowState.windowHandle, "Victoria Sims");
    XMapWindow(windowState.displayConnection, windowState.windowHandle);

    windowState.deleteWindowAtom = XInternAtom(windowState.displayConnection, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(windowState.displayConnection, windowState.windowHandle, &windowState.deleteWindowAtom, 1);

    windowState.widthInPixels = WINDOW_INITIAL_WIDTH;
    windowState.heightInPixels = WINDOW_INITIAL_HEIGHT;

    return linuxPresenterCreate(windowState.displayConnection, windowState.windowHandle,
                                windowState.widthInPixels, windowState.heightInPixels);
}

static void destroyWindowAndContext(void)
{
    linuxPresenterDestroy();

    if (windowState.displayConnection != NULL_POINTER)
    {
        XDestroyWindow(windowState.displayConnection, windowState.windowHandle);
        XCloseDisplay(windowState.displayConnection);
        windowState.displayConnection = NULL_POINTER;
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
                linuxPresenterResize(windowState.widthInPixels, windowState.heightInPixels);
                engineResize(windowState.widthInPixels, windowState.heightInPixels);
            }
            break;

        case KeyPress:
            /* Keycode 9 is Escape on every X server we care about. */
            if (event.xkey.keycode == 9)
            {
                windowState.shouldQuit = BOOLEAN_TRUE;
            }
            else
            {
                /* The character rather than the keycode: the engine's menu is
                   driven by letters precisely so that a terminal and a browser
                   can agree without either learning the other's key names. */
                char typed[8];
                int written = XLookupString(&event.xkey, typed, (int)sizeof(typed) - 1,
                                            NULL, NULL);

                if (written > 0 && engineHandleMenuKey(typed[0]) == BOOLEAN_TRUE)
                {
                    /* Printed on change and not every frame. The menu shares the
                       terminal with the load's own log, so reprinting it
                       continuously would bury everything the engine says. */
                    platformLogMessage(engineGetMenuText());
                }
            }
            break;

        case MotionNotify:
            (void)engineHandlePointer(ENGINE_POINTER_MOVED, (Integer32)event.xmotion.x,
                                      (Integer32)event.xmotion.y);
            break;

        case ButtonPress:
            /* Button one only. Two and three are a paste and a context menu on
               this platform and mean nothing here; scrolling arrives as buttons
               four and five, which the pager buttons cover well enough for now. */
            if (event.xbutton.button == Button1 &&
                engineHandlePointer(ENGINE_POINTER_PRESSED, (Integer32)event.xbutton.x,
                                    (Integer32)event.xbutton.y) == BOOLEAN_TRUE)
            {
                platformLogMessage(engineGetMenuText());
            }
            break;

        case LeaveNotify:
            (void)engineHandlePointer(ENGINE_POINTER_LEFT, 0, 0);
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

/* Says what is on a disc and stops, without opening a window.
 *
 * A disc load normally ends in a mesh being handed to a renderer, which needs a
 * display, which a build machine or a remote shell does not have. But the
 * question "what files are on this disc" is answered entirely by the walk, and
 * the walk needs nothing but the store — so it can be asked anywhere, and by
 * anyone holding a copy of the game we are not allowed to ship. */
static int inspectDisc(const char *path)
{
    static DiscStore store;
    static VirtualFileSystem fileSystem;
    MemoryArena *arena = memoryBudgetGetGlobalArena();
    DiscReader reader;
    DiscReadStatus walk;
    char message[256];

    if (discStoreOpen(&store, &fileSystem, path, arena) == BOOLEAN_FALSE)
    {
        platformLogMessage("platform: cannot open that disc");
        return 1;
    }

    /* A folder arrives with its catalogue already filled; an image has to be
       walked first. */
    if (fileSystem.entryCount == 0U)
    {
        walk = discReaderBegin(&reader, &fileSystem, arena, INSPECT_FILE_LIMIT);
        while (walk == DISC_READ_PENDING)
        {
            walk = discReaderStep(&reader);
        }
        if (walk != DISC_READ_COMPLETE)
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message), "platform: ");
            stringAppend(message, sizeof(message), discReadStatusGetName(walk));
            platformLogMessage(message);
            discStoreClose(&store);
            return 1;
        }
    }

    message[0] = '\0';
    stringAppend(message, sizeof(message), "platform: ");
    stringAppend(message, sizeof(message), path);
    stringAppend(message, sizeof(message), " holds ");
    if (stringWriteUnsigned(message + stringLength(message), sizeof(message) - stringLength(message),
                            (Unsigned64)fileSystem.entryCount) > 0UL)
    {
        stringAppend(message, sizeof(message), " files");
    }
    platformLogMessage(message);

    engineReportDiscCatalogue(&fileSystem);
    discStoreClose(&store);
    return 0;
}

int main(int argumentCount, char **argumentValues)
{
    EngineConfiguration configuration;
    Boolean runHeadlessCheck = BOOLEAN_FALSE;
    Boolean cameraIsStill = BOOLEAN_FALSE;
    Boolean menuIsOpen = BOOLEAN_FALSE;
    Unsigned32 menuPage = 0U;
    /* Half a turn, which is a Sim's front. Nought is its back. */
    Real32 cameraAngleDegrees = 180.0f;
    Boolean poseIsHeld = BOOLEAN_FALSE;
    Real32 poseHeldTick = 0.0f;
    Unsigned32 heldMorphChannel = 0U;
    const char *wornName = NULL_POINTER;
    const char *simArchetype = NULL_POINTER;
    const char *inspectPath = NULL_POINTER;
    MemorySize graphicsMemoryLimitBytes = 0UL;
    const char *discPath = NULL_POINTER;
    static DiscStore discStore;
    static VirtualFileSystem discFileSystem;
    int argumentIndex;

    windowState.profilerReportIntervalMicroseconds = PROFILER_REPORT_DEFAULT_INTERVAL_MICROSECONDS;

    for (argumentIndex = 1; argumentIndex < argumentCount; argumentIndex += 1)
    {
        const char *argument = argumentValues[argumentIndex];

        if (stringEquals(argument, "--check") == BOOLEAN_TRUE)
        {
            runHeadlessCheck = BOOLEAN_TRUE;
        }
        else if (stringEquals(argument, "--quiet") == BOOLEAN_TRUE)
        {
            windowState.profilerReportIntervalMicroseconds = 0ULL;
        }
        else if (stringEquals(argument, "--still-camera") == BOOLEAN_TRUE)
        {
            /* For judging one frame against another. See the note on
               EngineConfiguration.cameraIsStill. */
            cameraIsStill = BOOLEAN_TRUE;
        }
        else if (stringStartsWith(argument, "--morph=") == BOOLEAN_TRUE)
        {
            /* One deformation channel, held at full. The run's own log lists
               the channels and their numbers. */
            heldMorphChannel = (Unsigned32)stringParseUnsigned(argument + stringLength("--morph="));
        }
        else if (stringStartsWith(argument, "--menu=") == BOOLEAN_TRUE)
        {
            /* Which page to open on, for the same reason --menu exists at all:
               a page nobody can click their way to is a page nobody can
               capture. */
            const char *page = argument + stringLength("--menu=");

            menuIsOpen = BOOLEAN_TRUE;
            menuPage = stringEqualsIgnoringCase(page, "clothing")
                           ? 1U
                           : (stringEqualsIgnoringCase(page, "animation") ? 2U : 0U);
        }
        else if (stringEquals(argument, "--menu") == BOOLEAN_TRUE)
        {
            /* Opens the menu at start rather than waiting for an m.
             *
             * A menu is the one part of an engine that cannot be judged from a
             * log, and a machine with no way to synthesise a keystroke — a
             * headless capture, a build runner, whatever is taking the
             * screenshot — otherwise cannot see it at all. */
            menuIsOpen = BOOLEAN_TRUE;
        }
        else if (stringStartsWith(argument, "--sim=") == BOOLEAN_TRUE)
        {
            /* An age and a gender — am, af, cm, tf, em. The run says which
               archetypes the disc actually carries, so a name to try comes out
               of the last run's output. */
            simArchetype = argument + stringLength("--sim=");
        }
        else if (stringStartsWith(argument, "--wear=") == BOOLEAN_TRUE)
        {
            /* Any part of a catalogue entry's name. The run's own log names
               every entry the wardrobe was offered, so the next run's argument
               comes out of the last one's output. */
            wornName = argument + stringLength("--wear=");
        }
        else if (stringEquals(argument, "--still-pose") == BOOLEAN_TRUE)
        {
            poseIsHeld = BOOLEAN_TRUE;
        }
        else if (stringStartsWith(argument, "--still-pose=") == BOOLEAN_TRUE)
        {
            /* A tick to hold on, for looking at a moment other than the first. */
            poseIsHeld = BOOLEAN_TRUE;
            poseHeldTick = (Real32)stringParseUnsigned(argument + stringLength("--still-pose="));
        }
        else if (stringStartsWith(argument, "--still-camera=") == BOOLEAN_TRUE)
        {
            /* An angle in degrees, for looking at something other than the
               front. Whole degrees are enough for pointing a camera. */
            cameraIsStill = BOOLEAN_TRUE;
            cameraAngleDegrees =
                (Real32)stringParseUnsigned(argument + stringLength("--still-camera="));
        }
        else if (stringStartsWith(argument, "--inspect-disc=") == BOOLEAN_TRUE)
        {
            inspectPath = argument + stringLength("--inspect-disc=");
        }
        else if (stringStartsWith(argument, "--disc=") == BOOLEAN_TRUE)
        {
            /* An image or a directory; which it is decided by looking, not by
               the shape of the path, so a mounted CD and a rip both work. */
            discPath = argument + stringLength("--disc=");
        }
        else if (stringStartsWith(argument, "--graphics-memory-mebibytes=") == BOOLEAN_TRUE)
        {
            /* Overrides whatever the driver claims, so a small-memory device
               can be simulated on a machine that has plenty. */
            graphicsMemoryLimitBytes =
                (MemorySize)stringParseUnsigned(argument + stringLength("--graphics-memory-mebibytes=")) *
                1024UL * 1024UL;
        }
    }

    if (inspectPath != NULL_POINTER)
    {
        return inspectDisc(inspectPath);
    }

    if (runHeadlessCheck == BOOLEAN_TRUE)
    {
        return runHeadlessSelfCheck();
    }

    windowState.displayConnection = NULL_POINTER;
    windowState.shouldQuit = BOOLEAN_FALSE;

    if (createWindowAndContext() == BOOLEAN_FALSE)
    {
        destroyWindowAndContext();
        return 1;
    }

    configuration.widthInPixels = windowState.widthInPixels;
    configuration.heightInPixels = windowState.heightInPixels;
    configuration.graphicsMemoryLimitBytes = graphicsMemoryLimitBytes;
    configuration.fileSystem = NULL_POINTER;
    configuration.cameraIsStill = cameraIsStill;
    configuration.cameraAngleDegrees = cameraAngleDegrees;
    configuration.poseIsHeld = poseIsHeld;
    configuration.poseHeldTick = poseHeldTick;
    configuration.heldMorphChannel = heldMorphChannel;
    configuration.wornName = wornName;
    configuration.simArchetype = simArchetype;
    configuration.menuIsOpen = menuIsOpen;
    configuration.menuPage = menuPage;

    if (discPath != NULL_POINTER)
    {
        /* The budget's arena rather than the engine's: the engine has not been
           initialized yet, so its own pointer is still null, and a folder disc
           allocates its catalogue right here. They are the same arena either
           way — the budget hands out the one and only one. */
        if (discStoreOpen(&discStore, &discFileSystem, discPath, memoryBudgetGetGlobalArena()) ==
            BOOLEAN_TRUE)
        {
            char opened[256];

            opened[0] = '\0';
            stringAppend(opened, sizeof(opened), "platform: opened ");
            stringAppend(opened, sizeof(opened), discPath);
            stringAppend(opened, sizeof(opened), " as a ");
            stringAppend(opened, sizeof(opened), discStoreDescribe(&discStore));
            platformLogMessage(opened);
            configuration.fileSystem = &discFileSystem;
        }
        else
        {
            platformLogMessage("platform: cannot open that disc, starting without one");
        }
    }

    platformLogMessage(linuxPresenterGetName());

    if (engineInitialize(&configuration) == BOOLEAN_FALSE)
    {
        destroyWindowAndContext();
        return 1;
    }

    /* Said once the load has had its say, so it is the last thing on the
       terminal rather than the first. */
    platformLogMessage(engineGetMenuText());

    while (windowState.shouldQuit == BOOLEAN_FALSE)
    {
        engineBeginFrame();

        VICTORIA_PROFILE_ZONE_BEGIN("platformPumpEvents");
        pumpWindowEvents();
        VICTORIA_PROFILE_ZONE_END();

        /* Kept stepping after the load has finished, because the menu can ask
           it a second question — a different Sim restarts the assembly, and the
           animation names are read after the Sim is on screen. Once there is
           nothing to do the first call returns immediately.
         *
           A batch and not one, because a native read never pends: one step a
           frame would spend five thousand frames on a list that takes a moment,
           and the whole point of doing it out here is that the window stays
           alive while it happens. */
        {
            Unsigned32 steps = 0U;

            while (steps < 256U && engineStepDiscLoad() == ENGINE_DISC_WORKING)
            {
                steps++;
            }
        }

        engineRenderFrame(readMonotonicSeconds());

        /* Almost always the whole frame: with vertical sync on, this is where
           the wait for the display lands. */
        VICTORIA_PROFILE_ZONE_BEGIN("platformPresent");
        linuxPresenterPresent();
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
