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
    search->packagesWithShapes = 0U;
    search->modelsResolved = 0U;
    search->modelName[0] = '\0';
    search->foundThroughScenegraph = BOOLEAN_FALSE;
    search->geometryRefused = 0U;
    search->decompressionRefused = 0U;
    search->sawUnknownMark = BOOLEAN_FALSE;
    search->firstUnknownMark = 0U;
    search->largestElementCount = 0U;
    for (index = 0U; index < GEOMETRY_READ_RESULT_COUNT; index++)
    {
        search->refusalsByReason[index] = 0U;
    }
    for (index = 0U; index < DISC_CONTENT_VERSION_BUCKETS; index++)
    {
        search->versionsSeen[index] = 0U;
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

/* Follows shape to geometry node to container, and says whether it got there.
 *
 * A package with no shape is not a failure — plenty hold a container and
 * nothing else, and the caller falls back to taking the container directly.
 * What this buys is a mesh that was chosen: named, and known to be part of a
 * model rather than whatever the index happened to list first. */
static const PackageResource *findGeometryThroughScenegraph(DiscContentSearch *search,
                                                            const Package *package)
{
    const PackageResource *shapeResource =
        packageReaderFindFirstOfType(package, (Unsigned32)PACKAGE_TYPE_SHPE);
    MemorySize marker;
    const Unsigned8 *shapeBytes;
    MemorySize shapeSize;
    Boolean compressed;
    ShapeDescription shape;
    const PackageResource *geometry = NULL_POINTER;
    Unsigned32 index;

    if (shapeResource == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    marker = memoryArenaGetMarker(search->arena);
    shapeBytes = scenegraphReadResourceBytes(search->arena, package, shapeResource, &shapeSize, &compressed);
    if (shapeBytes == NULL_POINTER ||
        scenegraphReadShape(&shape, shapeBytes, shapeSize) != SCENEGRAPH_READ_OK)
    {
        memoryArenaRewindToMarker(search->arena, marker);
        return NULL_POINTER;
    }
    search->packagesWithShapes++;

    for (index = 0U; index < shape.storedMeshCount && geometry == NULL_POINTER; index++)
    {
        if (shape.meshNames[index][0] == '\0')
        {
            continue;
        }
        geometry = scenegraphFindGeometryNamed(search->arena, package, shape.meshNames[index]);
    }

    if (geometry != NULL_POINTER)
    {
        search->modelName[0] = '\0';
        stringAppend(search->modelName, RESOURCE_NAME_LIMIT, shape.resourceName);
    }
    /* The shape's own bytes are done with either way; the container is found by
     * index entry, which outlives them. */
    memoryArenaRewindToMarker(search->arena, marker);
    return geometry;
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

    /* Ask the scenegraph first. A shape names the meshes a model is built from,
     * so a container reached that way is one this engine chose rather than the
     * first the index happened to list. Packages that carry no shape fall back
     * to the blunt rule, which is how the engine has worked until now. */
    search->modelName[0] = '\0';
    geometry = findGeometryThroughScenegraph(search, &package);
    search->foundThroughScenegraph = (geometry != NULL_POINTER) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    if (geometry != NULL_POINTER)
    {
        search->modelsResolved++;
    }
    else
    {
        geometry = packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMDC);
    }
    if (geometry == NULL_POINTER)
    {
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_PENDING;
    }
    search->packagesWithGeometry++;

    {
        Boolean compressed = BOOLEAN_FALSE;

        geometryBytes = scenegraphReadResourceBytes(search->arena, &package, geometry, &geometrySize, &compressed);
        if (compressed)
        {
            search->packagesCompressed++;
        }
        if (geometryBytes == NULL_POINTER && compressed)
        {
            search->geometryRefused++;
            search->decompressionRefused++;
            memoryArenaRewindToMarker(search->arena, attemptMarker);
            return DISC_CONTENT_PENDING;
        }
    }

    if (geometryBytes == NULL_POINTER)
    {
        search->geometryRefused++;
        search->refusalsByReason[GEOMETRY_READ_TRUNCATED]++;
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_PENDING;
    }

    {
        GeometryReadResult readResult =
            geometryReaderOpen(&search->mesh, geometryBytes, geometrySize, search->arena);

        /* Recorded whether or not the read succeeded. What the disc holds does
           not depend on what this engine could do with it. */
        if (search->mesh.containerVersion != 0U)
        {
            Unsigned32 bucket = search->mesh.containerVersion;

            if (bucket >= DISC_CONTENT_VERSION_BUCKETS)
            {
                bucket = DISC_CONTENT_VERSION_BUCKETS - 1U;
            }
            search->versionsSeen[bucket]++;
        }
        if (search->mesh.versionMark != 0xFFFF0001UL && !search->sawUnknownMark)
        {
            search->sawUnknownMark = BOOLEAN_TRUE;
            search->firstUnknownMark = search->mesh.versionMark;
        }
        if (search->mesh.elementCount > search->largestElementCount)
        {
            search->largestElementCount = search->mesh.elementCount;
        }

        if (readResult != GEOMETRY_READ_OK)
        {
            search->geometryRefused++;
            if ((Unsigned32)readResult < GEOMETRY_READ_RESULT_COUNT)
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
