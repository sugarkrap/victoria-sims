#include "victoria/memoryArena.h"
#include "victoria/platformInterface.h"
#include "victoria/virtualFileSystem.h"

#define DELIVERY_CAPACITY (24UL * 1024UL * 1024UL + 256UL * 1024UL)

Boolean webDiscStoreOpen(VirtualFileSystem *fileSystem, Unsigned64 sizeInBytes, MemoryArena *arena);
Real32 webDiscStoreGetWantedLength(void);
double webDiscStoreGetWantedOffset(void);
Unsigned32 webDiscStoreGetDeliveryPointer(void);
void webDiscStoreDeliver(void);

static Unsigned8 *deliveryBuffer = NULL_POINTER;

static Unsigned64 wantedOffset = 0U;
static MemorySize wantedLength = 0UL;
static Boolean isWaiting = BOOLEAN_FALSE;

static Unsigned64 heldOffset = 0U;
static MemorySize heldLength = 0UL;
static Boolean isHolding = BOOLEAN_FALSE;

static VirtualReadResult webDiscRead(void *context, Unsigned64 offsetInBytes, MemorySize sizeInBytes,
                                     Unsigned8 *destination)
{
    MemorySize index;

    (void)context;

    if (isHolding && heldOffset == offsetInBytes && heldLength == sizeInBytes)
    {
        for (index = 0UL; index < sizeInBytes; index++)
        {
            destination[index] = deliveryBuffer[index];
        }
        isHolding = BOOLEAN_FALSE;
        isWaiting = BOOLEAN_FALSE;
        return VIRTUAL_READ_OK;
    }

    if (sizeInBytes > DELIVERY_CAPACITY)
    {
        return VIRTUAL_READ_FAILED;
    }

    wantedOffset = offsetInBytes;
    wantedLength = sizeInBytes;
    isWaiting = BOOLEAN_TRUE;
    return VIRTUAL_READ_PENDING;
}

Boolean webDiscStoreOpen(VirtualFileSystem *fileSystem, Unsigned64 sizeInBytes, MemoryArena *arena)
{
    if (deliveryBuffer == NULL_POINTER)
    {
        deliveryBuffer = (Unsigned8 *)memoryArenaAllocate(arena, DELIVERY_CAPACITY, 8UL);
    }
    if (deliveryBuffer == NULL_POINTER)
    {
        platformLogMessage("platform: not enough arena for the disc delivery buffer");
        return BOOLEAN_FALSE;
    }

    isWaiting = BOOLEAN_FALSE;
    isHolding = BOOLEAN_FALSE;
    virtualFileSystemInitialize(fileSystem, webDiscRead, NULL_POINTER, sizeInBytes);
    return BOOLEAN_TRUE;
}

Real32 webDiscStoreGetWantedLength(void)
{
    return isWaiting ? (Real32)wantedLength : 0.0f;
}

double webDiscStoreGetWantedOffset(void)
{
    return isWaiting ? (double)wantedOffset : 0.0;
}

Unsigned32 webDiscStoreGetDeliveryPointer(void)
{
    return (Unsigned32)(MemorySize)deliveryBuffer;
}

void webDiscStoreDeliver(void)
{
    heldOffset = wantedOffset;
    heldLength = wantedLength;
    isHolding = BOOLEAN_TRUE;
}
