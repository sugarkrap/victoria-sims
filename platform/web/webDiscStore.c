#include "victoria/memoryArena.h"
#include "victoria/platformInterface.h"
#include "victoria/virtualFileSystem.h"

/* The browser's side of a disc.
 *
 * A File is a handle, not bytes: the browser reads ranges off disk when asked,
 * and only asynchronously. So a read here cannot answer on the spot. It records
 * what it wants and answers PENDING; the page fetches that range, writes it
 * into the delivery buffer, and the engine asks again — and the second time the
 * answer is there.
 *
 * That is the whole reason virtualFileSystemRead has a PENDING at all. Nothing
 * about this is a workaround; it is what the platform is.
 *
 * One range is in flight at a time. The engine only ever waits on one, and a
 * queue would be machinery in service of nothing. */

/* Big enough for the largest package the content search will open, plus room
   for a directory extent. Claimed from the arena, so it is inside the budget
   rather than beside it. */
#define DELIVERY_CAPACITY (24UL * 1024UL * 1024UL + 256UL * 1024UL)

/* Declared here rather than in a header: the web entry point is the only
   caller, and a header for four functions used once would be ceremony. */
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
        /* Nothing on a disc should be this big, and asking the page for it
           would only fail later and less clearly. */
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

/* What the page should fetch next, as doubles: an image is larger than a
   32-bit offset addresses, and every value here is far inside what a double
   represents exactly. Length is zero when nothing is wanted. */
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

/* Called once the page has written the range into the delivery buffer. */
void webDiscStoreDeliver(void)
{
    heldOffset = wantedOffset;
    heldLength = wantedLength;
    isHolding = BOOLEAN_TRUE;
}
