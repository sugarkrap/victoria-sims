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
#include "victoria/memoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "victoria/virtualFileSystem.h"

#define WINDOW_INITIAL_WIDTH 1024u
#define WINDOW_INITIAL_HEIGHT 768u

typedef struct LinuxGameWindowState
{
    Display *displayConnection;
    Window windowHandle;
    Atom deleteWindowAtom;
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
    Boolean shouldQuit;
} LinuxGameWindowState;

static LinuxGameWindowState windowState;

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
    (void)mkdir(destination, 0700);
    (void)stringAppend(destination, capacity, "/");

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

static Boolean createWindowAndContext(void)
{
    Window rootWindow;
    XSetWindowAttributes windowAttributes;

    windowState.displayConnection = XOpenDisplay(NULL_POINTER);
    if (windowState.displayConnection == NULL_POINTER)
    {
        platformLogMessage("victoriaSims: cannot open X display (is DISPLAY set?)");
        return BOOLEAN_FALSE;
    }

    rootWindow = DefaultRootWindow(windowState.displayConnection);
    windowAttributes.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask |
                                  ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                                  LeaveWindowMask;

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
            if (event.xkey.keycode == 9)
            {
                windowState.shouldQuit = BOOLEAN_TRUE;
            }
            break;

        case MotionNotify:
            (void)engineHandleGamePointer(ENGINE_POINTER_MOVED, (Integer32)event.xmotion.x,
                                          (Integer32)event.xmotion.y);
            break;

        case ButtonPress:
            if (event.xbutton.button == Button1)
            {
                (void)engineHandleGamePointer(ENGINE_POINTER_PRESSED, (Integer32)event.xbutton.x,
                                              (Integer32)event.xbutton.y);
            }
            break;

        case ButtonRelease:
            if (event.xbutton.button == Button1)
            {
                (void)engineHandleGamePointer(ENGINE_POINTER_RELEASED, (Integer32)event.xbutton.x,
                                              (Integer32)event.xbutton.y);
            }
            break;

        case LeaveNotify:
            (void)engineHandleGamePointer(ENGINE_POINTER_LEFT, 0, 0);
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
    static DiscStore discStore;
    static VirtualFileSystem discFileSystem;

    if (argumentCount != 2)
    {
        fputs("usage: victoriaSims <path to a Sims 2 disc image or install folder>\n", stderr);
        return 1;
    }

    if (discStoreOpen(&discStore, &discFileSystem, argumentValues[1],
                      memoryBudgetGetGlobalArena()) == BOOLEAN_FALSE)
    {
        fputs("victoriaSims: cannot open that disc\n", stderr);
        return 1;
    }

    windowState.displayConnection = NULL_POINTER;
    windowState.shouldQuit = BOOLEAN_FALSE;

    if (createWindowAndContext() == BOOLEAN_FALSE)
    {
        destroyWindowAndContext();
        return 1;
    }

    if (engineInitializeGame(&discFileSystem, windowState.widthInPixels,
                             windowState.heightInPixels, 0UL) == BOOLEAN_FALSE)
    {
        destroyWindowAndContext();
        return 1;
    }

    while (windowState.shouldQuit == BOOLEAN_FALSE)
    {
        engineBeginFrame();
        pumpWindowEvents();
        engineRenderFrame(readMonotonicSeconds());
        linuxPresenterPresent();
        engineEndFrame();
    }

    engineShutdown();
    destroyWindowAndContext();
    return 0;
}
