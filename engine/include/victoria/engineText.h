#ifndef VICTORIA_ENGINE_TEXT_HEADER
#define VICTORIA_ENGINE_TEXT_HEADER

#include "victoria/coreTypes.h"
#include "victoria/debugMenu.h"
#include "victoria/interfaceMenu.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

Boolean engineTextInitialize(MemoryArena *arena);

Boolean engineTextStepFont(VirtualFileSystem *fileSystem);
Boolean engineTextFontIsSettled(void);

void engineTextSetWindowSize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);

void engineTextDraw(DebugMenu *menu, const char *text);

void engineTextForget(void);

void engineTextSetPointer(Integer32 x, Integer32 y);
void engineTextForgetPointer(void);
InterfaceMenuHit engineTextGetHovered(void);
void engineTextSetHovered(InterfaceMenuHit hit);
InterfaceMenuHit engineTextHitTest(const DebugMenu *menu);

#endif
