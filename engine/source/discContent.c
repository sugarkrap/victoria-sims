#include "victoria/discContent.h"

#include "utils/strings.h"
#include "victoria/compression.h"
#include "victoria/packageReader.h"

/* Files larger than this are not opened looking for geometry. A retail
 * objects.package is forty megabytes and holds no mesh worth drawing on its
 * own; the ceiling keeps a search from spending the whole budget on one file it
 * was never going to use. */
#define LARGEST_PACKAGE_BYTES (24UL * 1024UL * 1024UL)

const char *discContentStatusGetName(DiscContentStatus status)
{
    switch (status)
    {
    case DISC_CONTENT_FOUND:
        return "found geometry";
    case DISC_CONTENT_PENDING:
        return "still looking";
    case DISC_CONTENT_NONE_FOUND:
        return "no readable geometry on this disc";
    case DISC_CONTENT_OUT_OF_ARENA:
        return "not enough arena space to open a package";
    default:
        return "unknown";
    }
}

void discContentBegin(DiscContentSearch *search, VirtualFileSystem *fileSystem, MemoryArena *arena)
{
    Unsigned32 index;

    search->fileSystem = fileSystem;
    search->arena = arena;
    search->arenaMarker = memoryArenaGetMarker(arena);
    search->nextIndex = 0U;
    search->packagePath[0] = '\0';
    search->packagesOpened = 0U;
    search->packagesCompressed = 0U;
    search->packagesWithGeometry = 0U;
    search->geometryRefused = 0U;
    search->decompressionRefused = 0U;
    for (index = 0U; index < 8U; index++)
    {
        search->refusalsByReason[index] = 0U;
    }
}

static Boolean endsWithPackage(const char *path)
{
    MemorySize length = stringLength(path);
    MemorySize index;
    const char *suffix = ".package";
    MemorySize suffixLength = 8UL;

    if (length < suffixLength)
    {
        return BOOLEAN_FALSE;
    }
    for (index = 0UL; index < suffixLength; index++)
    {
        if (characterToLowerCase(path[length - suffixLength + index]) != suffix[index])
        {
            return BOOLEAN_FALSE;
        }
    }
    return BOOLEAN_TRUE;
}

DiscContentStatus discContentStep(DiscContentSearch *search)
{
    const VirtualFileEntry *entry;
    Unsigned8 *bytes;
    MemorySize sizeInBytes;
    MemorySize attemptMarker;
    Package package;
    const PackageResource *geometry;
    const Unsigned8 *geometryBytes;
    MemorySize geometrySize;
    VirtualReadResult read;

    if (search->nextIndex >= search->fileSystem->entryCount)
    {
        return DISC_CONTENT_NONE_FOUND;
    }

    entry = virtualFileSystemGetEntry(search->fileSystem, search->nextIndex);
    if (entry == NULL_POINTER || !endsWithPackage(entry->path) ||
        entry->sizeInBytes > (Unsigned64)LARGEST_PACKAGE_BYTES || entry->sizeInBytes == 0U)
    {
        search->nextIndex++;
        return DISC_CONTENT_PENDING;
    }

    /* Everything this attempt allocates is given back unless it succeeds, so a
     * disc full of packages that turn out to hold nothing costs the arena
     * nothing at all. */
    attemptMarker = memoryArenaGetMarker(search->arena);
    sizeInBytes = (MemorySize)entry->sizeInBytes;
    bytes = (Unsigned8 *)memoryArenaAllocate(search->arena, sizeInBytes, 8UL);
    if (bytes == NULL_POINTER)
    {
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_OUT_OF_ARENA;
    }

    read = virtualFileSystemReadFile(search->fileSystem, search->nextIndex, 0U, sizeInBytes, bytes);
    if (read == VIRTUAL_READ_PENDING)
    {
        /* The bytes are not here yet. Give the space back and come again; the
         * index is untouched, so the next step retries this same package. */
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_PENDING;
    }

    search->nextIndex++;
    if (read != VIRTUAL_READ_OK ||
        packageReaderOpen(&package, bytes, sizeInBytes, search->arena) != PACKAGE_READ_OK)
    {
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_PENDING;
    }
    search->packagesOpened++;

    geometry = packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMDC);
    if (geometry == NULL_POINTER)
    {
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_PENDING;
    }
    search->packagesWithGeometry++;

    geometryBytes = packageReaderGetResourceBytes(&package, geometry);
    geometrySize = (MemorySize)geometry->sizeInBytes;

    /* Almost everything on a retail disc is stored compressed, so the bytes the
       index points at are usually not the resource — they are a RefPack stream
       that unpacks into it. Detected from the stream's own header rather than
       from the package's directory resource: the header is on the thing being
       read, which is the harder of the two to be wrong about. */
    if (geometryBytes != NULL_POINTER && compressionLooksLikeRefPack(geometryBytes, geometrySize))
    {
        MemorySize decompressedSize = compressionGetDecompressedSize(geometryBytes, geometrySize);
        Unsigned8 *unpacked =
            (Unsigned8 *)memoryArenaAllocate(search->arena, decompressedSize, 8UL);
        CompressionResult unpackResult;

        search->packagesCompressed++;
        if (unpacked == NULL_POINTER)
        {
            memoryArenaRewindToMarker(search->arena, attemptMarker);
            return DISC_CONTENT_OUT_OF_ARENA;
        }
        unpackResult = compressionDecompressRefPack(unpacked, decompressedSize, geometryBytes,
                                                    geometrySize, &decompressedSize);
        if (unpackResult != COMPRESSION_OK)
        {
            search->geometryRefused++;
            search->decompressionRefused++;
            memoryArenaRewindToMarker(search->arena, attemptMarker);
            return DISC_CONTENT_PENDING;
        }
        geometryBytes = unpacked;
        geometrySize = decompressedSize;
    }

    {
        GeometryReadResult readResult =
            (geometryBytes == NULL_POINTER)
                ? GEOMETRY_READ_TRUNCATED
                : geometryReaderOpen(&search->mesh, geometryBytes, geometrySize, search->arena);

        if (readResult != GEOMETRY_READ_OK)
        {
            search->geometryRefused++;
            if ((Unsigned32)readResult < 8U)
            {
                search->refusalsByReason[(Unsigned32)readResult]++;
            }
            memoryArenaRewindToMarker(search->arena, attemptMarker);
            return DISC_CONTENT_PENDING;
        }
    }

    search->packagePath[0] = '\0';
    stringAppend(search->packagePath, DISC_CONTENT_PATH_LIMIT, entry->path);
    return DISC_CONTENT_FOUND;
}

DiscContentStatus discContentRunToCompletion(DiscContentSearch *search)
{
    /* Bounded so a store that answers PENDING forever cannot hang a caller that
     * should not have used this. */
    Unsigned32 remaining = 1000000U;

    for (;;)
    {
        DiscContentStatus status = discContentStep(search);

        if (status != DISC_CONTENT_PENDING)
        {
            return status;
        }
        if (remaining == 0U)
        {
            return DISC_CONTENT_NONE_FOUND;
        }
        remaining--;
    }
}
