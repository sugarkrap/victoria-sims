#include "victoria/discSurvey.h"

#include "utils/strings.h"
#include "victoria/compression.h"

#define HEADER_SIZE 96UL
#define OFFSET_INDEX_ENTRY_COUNT 36UL
#define OFFSET_INDEX_OFFSET 40UL
#define OFFSET_INDEX_SIZE 44UL

#define ENTRY_SIZE_WITHOUT_INSTANCE_HIGH 20UL
#define ENTRY_SIZE_WITH_INSTANCE_HIGH 24UL

#define DEFAULT_WINDOW_BYTES (2UL * 1024UL * 1024UL)

#define SCRIPT_NAME_BYTES 64UL
#define SCRIPT_HEADER_BYTES 76UL
#define SCRIPT_FIRST_FORMAT 0x8000U
#define SCRIPT_CACHE_FLAG_FORMAT 0x8009U

#define FIRST_GLOBAL_OPCODE 0x0100U
#define FIRST_PRIVATE_OPCODE 0x1000U
#define FIRST_SEMIGLOBAL_OPCODE 0x2000U

typedef enum TypeStanding
{
    TYPE_READ_HERE = 0,
    TYPE_DECODED_IN_LEGACY,
    TYPE_NAMED_ONLY
} TypeStanding;

typedef struct KnownType
{
    Unsigned32 typeIdentifier;
    const char *name;
    TypeStanding standing;
} KnownType;

static const KnownType knownTypes[] = {
    { 0xE519C933UL, "CRES resource node", TYPE_READ_HERE },
    { 0xFC6EB1F7UL, "SHPE shape", TYPE_READ_HERE },
    { 0x7BA3838CUL, "GMND geometric node", TYPE_READ_HERE },
    { 0xAC4F8687UL, "GMDC geometry", TYPE_READ_HERE },
    { 0x49596978UL, "TXMT material", TYPE_READ_HERE },
    { 0x1C4A276CUL, "TXTR texture", TYPE_READ_HERE },
    { 0xED534136UL, "LIFO large texture level", TYPE_READ_HERE },
    { 0xFB00791EUL, "ANIM animation", TYPE_READ_HERE },
    { 0xE86B1EEFUL, "DIR compression directory", TYPE_READ_HERE },
    { 0xAC506764UL, "3IDR resource key list", TYPE_READ_HERE },
    { 0xEBCF3E27UL, "GZPS property set", TYPE_READ_HERE },
    { 0x856DDBACUL, "IMG image", TYPE_READ_HERE },
    { 0xABCB5DA4UL, "NHTR neighborhood terrain", TYPE_DECODED_IN_LEGACY },
    { 0xAC8A7A2EUL, "IDNO neighborhood info", TYPE_DECODED_IN_LEGACY },
    { 0xABD0DC63UL, "neighborhood decorations", TYPE_DECODED_IN_LEGACY },
    { 0x6D619378UL, "neighborhood object", TYPE_DECODED_IN_LEGACY },
    { 0x0BF999E7UL, "LTXT lot info", TYPE_DECODED_IN_LEGACY },
    { 0x6C589723UL, "base lot info", TYPE_DECODED_IN_LEGACY },
    { 0xFA1C39F7UL, "lot object", TYPE_DECODED_IN_LEGACY },
    { 0x6B943B43UL, "lot terrain", TYPE_DECODED_IN_LEGACY },
    { 0x4B58975BUL, "lot textures", TYPE_DECODED_IN_LEGACY },
    { 0xAB4BA572UL, "fence post layer", TYPE_DECODED_IN_LEGACY },
    { 0x0C900FDBUL, "pool", TYPE_DECODED_IN_LEGACY },
    { 0xAB9406AAUL, "roof", TYPE_DECODED_IN_LEGACY },
    { 0x0A284D0BUL, "wall graph", TYPE_DECODED_IN_LEGACY },
    { 0x8A84D7B0UL, "wall layer", TYPE_DECODED_IN_LEGACY },
    { 0xCAC4FC40UL, "string map", TYPE_DECODED_IN_LEGACY },
    { 0x2A51171BUL, "3D array", TYPE_DECODED_IN_LEGACY },
    { 0xAC598EACUL, "sim appearance", TYPE_DECODED_IN_LEGACY },
    { 0xAACE2EFBUL, "SDSC sim description", TYPE_DECODED_IN_LEGACY },
    { 0x53545223UL, "STR# string table", TYPE_DECODED_IN_LEGACY },
    { 0x6F626A74UL, "object save type table", TYPE_DECODED_IN_LEGACY },
    { 0x4F626A4DUL, "OBJM object module", TYPE_DECODED_IN_LEGACY },
    { 0x4F424A44UL, "OBJD object definition", TYPE_DECODED_IN_LEGACY },
    { 0x4F424A66UL, "OBJf object functions", TYPE_DECODED_IN_LEGACY },
    { 0x42484156UL, "BHAV behaviour tree", TYPE_DECODED_IN_LEGACY },
    { 0xCCA8E925UL, "catalog object", TYPE_DECODED_IN_LEGACY },
    { 0x2026960BUL, "audio", TYPE_DECODED_IN_LEGACY },
    { 0x8C3CE95AUL, "IMG2 image", TYPE_DECODED_IN_LEGACY },
    { 0xCC364C2AUL, "SREL sim relationship", TYPE_NAMED_ONLY },
    { 0x46414D49UL, "FAMI family", TYPE_NAMED_ONLY },
    { 0x8C870743UL, "FAMT family ties", TYPE_NAMED_ONLY },
    { 0xEBFEE33FUL, "SDNA sim genetics", TYPE_NAMED_ONLY },
    { 0x4E474248UL, "NGBH neighborhood memories", TYPE_NAMED_ONLY },
    { 0xCD95548EUL, "SWAF wants and fears", TYPE_NAMED_ONLY },
    { 0x104F6A6EUL, "BNFO business info", TYPE_NAMED_ONLY }
};

typedef struct ScriptFamilyMember
{
    Unsigned32 typeIdentifier;
    const char *name;
} ScriptFamilyMember;

static const ScriptFamilyMember scriptFamily[] = {
    { 0x42484156UL, "BHAV behaviour trees" },
    { 0x4F424A44UL, "OBJD object definitions" },
    { 0x4F424A66UL, "OBJf object function tables" },
    { 0x54544142UL, "TTAB interaction tables" },
    { 0x54544173UL, "TTAs interaction strings" },
    { 0x42434F4EUL, "BCON constants" },
    { 0x474C4F42UL, "GLOB semi-global links" },
    { 0x54505250UL, "TPRP tree parameter labels" },
    { 0x5452434EUL, "TRCN constant labels" },
    { 0x534C4F54UL, "SLOT routing slots" },
    { 0x43545353UL, "CTSS catalog strings" },
    { 0x53545223UL, "STR# string tables" },
    { 0x9012468AUL, "Lua global scripts" },
    { 0x9012468BUL, "Lua object scripts" }
};

static const char *const primitiveNames[] = {
    "Sleep", "Generic Sim Call", "Expression", "Find Best Interaction", "Grab", "Drop",
    "Change Suit or Accessory", "Refresh", "Random Number", "Burn", "Tutorial",
    "Get Distance To", "Get Direction To", "Push Interaction", "Find Best Object For Function",
    "Breakpoint", "Find Location For", "Idle For Input", "Remove Object Instance",
    "Make New Character", "Run Functional Tree", "Show String", "Look Towards",
    "Play Sound Event", "Old Relationship", "Alter Budget", "Relationship",
    "Go To Relative Position", "Run Tree By Name", "Set Motive Change", "Gosub Found Action",
    "Set To Next", "Test Object Type", "Find Five Worst Motives", "UI Effect", "Special Effect",
    "Dialog (private strings)", "Test Sim Interacting With", "Dialog (global strings)",
    "Dialog (semi-global strings)", "Online Jobs Call", "Set Balloon or Headline",
    "Create New Object Instance", "Drop Onto", "Animate Sim", "Go To Routing Slot", "Snap",
    "Reach", "Stop All Sounds", "Notify Stack Object Out Of Idle", "Add or Change Action String"
};

static Unsigned32 readUnsigned32(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned32)bytes[offset] | ((Unsigned32)bytes[offset + 1UL] << 8) |
           ((Unsigned32)bytes[offset + 2UL] << 16) | ((Unsigned32)bytes[offset + 3UL] << 24);
}

static Unsigned32 readUnsigned16(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned32)bytes[offset] | ((Unsigned32)bytes[offset + 1UL] << 8);
}

static void finishPackage(DiscSurvey *survey)
{
    if (survey->scripts != NULL_POINTER)
    {
        memoryArenaRewindToMarker(survey->scratch, survey->scriptsMarker);
    }
    survey->scripts = NULL_POINTER;
    survey->scriptCount = 0U;
    survey->scriptCursor = 0U;
    survey->stage = DISC_SURVEY_STAGE_HEADER;
    survey->nextFileIndex++;
}

static DiscSurveyNeighborhoodKind classifyNeighborhood(const char *path)
{
    if (stringContainsIgnoringCase(path, "character"))
    {
        return DISC_SURVEY_NEIGHBORHOOD_CHARACTER;
    }
    if (stringContainsIgnoringCase(path, "_lot") || stringContainsIgnoringCase(path, "lots"))
    {
        return DISC_SURVEY_NEIGHBORHOOD_LOT;
    }
    return DISC_SURVEY_NEIGHBORHOOD_MAIN;
}

static void tallyNeighborhoodType(DiscSurvey *survey, Unsigned32 typeIdentifier)
{
    Unsigned32 which;
    DiscSurveyTypeTally *tally;

    survey->neighborhoodResources++;
    for (which = 0U; which < survey->neighborhoodTypeCount; which++)
    {
        tally = &survey->neighborhoodTypes[which];
        if (tally->typeIdentifier == typeIdentifier)
        {
            tally->resourceCount++;
            if (tally->lastPackage != survey->nextFileIndex)
            {
                tally->lastPackage = survey->nextFileIndex;
                tally->packageCount++;
            }
            return;
        }
    }
    if (survey->neighborhoodTypeCount >= DISC_SURVEY_TYPE_LIMIT)
    {
        survey->neighborhoodTypeOverflow++;
        return;
    }
    tally = &survey->neighborhoodTypes[survey->neighborhoodTypeCount];
    tally->typeIdentifier = typeIdentifier;
    tally->resourceCount = 1U;
    tally->packageCount = 1U;
    tally->lastPackage = survey->nextFileIndex;
    survey->neighborhoodTypeCount++;
}

typedef char scriptFamilyFitsItsTallies
    [(VICTORIA_ARRAY_LENGTH(scriptFamily) <= DISC_SURVEY_FAMILY_LIMIT) ? 1 : -1];

static void tallyFamily(DiscSurvey *survey, Unsigned32 typeIdentifier, Boolean *seenInPackage)
{
    MemorySize which;

    for (which = 0UL; which < VICTORIA_ARRAY_LENGTH(scriptFamily); which++)
    {
        if (scriptFamily[which].typeIdentifier == typeIdentifier)
        {
            survey->familyResources[which]++;
            if (!seenInPackage[which])
            {
                seenInPackage[which] = BOOLEAN_TRUE;
                survey->familyPackages[which]++;
            }
            return;
        }
    }
}

void discSurveyBegin(DiscSurvey *survey, VirtualFileSystem *fileSystem, MemoryArena *scratch)
{
    MemorySize byteIndex;
    Unsigned8 *raw = (Unsigned8 *)survey;

    for (byteIndex = 0UL; byteIndex < sizeof(*survey); byteIndex++)
    {
        raw[byteIndex] = 0U;
    }
    survey->fileSystem = fileSystem;
    survey->scratch = scratch;
    survey->windowBytes = DEFAULT_WINDOW_BYTES;
    survey->stage = DISC_SURVEY_STAGE_HEADER;
    survey->scripts = NULL_POINTER;
}

Boolean discSurveyCountScript(DiscSurvey *survey, const Unsigned8 *bytes, MemorySize size)
{
    Unsigned32 format;
    Unsigned32 instructionCount;
    MemorySize nodeSize;
    MemorySize at;
    Unsigned32 node;
    Unsigned8 seen[DISC_SURVEY_PRIMITIVE_LIMIT];
    Unsigned32 which;

    if (size < SCRIPT_HEADER_BYTES)
    {
        survey->scriptsMalformed++;
        return BOOLEAN_FALSE;
    }
    format = readUnsigned16(bytes, SCRIPT_NAME_BYTES);
    instructionCount = readUnsigned16(bytes, SCRIPT_NAME_BYTES + 2UL);
    if (format < SCRIPT_FIRST_FORMAT || format >= SCRIPT_FIRST_FORMAT + DISC_SURVEY_FORMAT_LIMIT)
    {
        survey->formatOther++;
        survey->scriptsMalformed++;
        return BOOLEAN_FALSE;
    }

    if (format <= 0x8002U)
    {
        nodeSize = 12UL;
    }
    else if (format <= 0x8004U)
    {
        nodeSize = 20UL;
    }
    else if (format <= 0x8006U)
    {
        nodeSize = 21UL;
    }
    else
    {
        nodeSize = 23UL;
    }
    at = SCRIPT_HEADER_BYTES + ((format >= SCRIPT_CACHE_FLAG_FORMAT) ? 1UL : 0UL);
    if (at + (MemorySize)instructionCount * nodeSize > size)
    {
        survey->scriptsMalformed++;
        return BOOLEAN_FALSE;
    }

    survey->formatCounts[format - SCRIPT_FIRST_FORMAT]++;
    survey->scriptsRead++;
    survey->instructions += instructionCount;
    if (instructionCount > survey->largestScript)
    {
        survey->largestScript = instructionCount;
    }
    for (which = 0U; which < DISC_SURVEY_PRIMITIVE_LIMIT; which++)
    {
        seen[which] = 0U;
    }

    for (node = 0U; node < instructionCount; node++, at += nodeSize)
    {
        Unsigned32 opcode = readUnsigned16(bytes, at);

        if (opcode < FIRST_GLOBAL_OPCODE)
        {
            survey->primitiveUses[opcode]++;
            if (seen[opcode] == 0U)
            {
                seen[opcode] = 1U;
                survey->primitiveScripts[opcode]++;
            }
        }
        else if (opcode < FIRST_PRIVATE_OPCODE)
        {
            survey->globalCalls++;
        }
        else if (opcode < FIRST_SEMIGLOBAL_OPCODE)
        {
            survey->privateCalls++;
        }
        else
        {
            survey->semiGlobalCalls++;
        }
    }
    return BOOLEAN_TRUE;
}

static DiscSurveyStatus readHeader(DiscSurvey *survey, const VirtualFileEntry *entry)
{
    Unsigned8 header[HEADER_SIZE];
    VirtualReadResult read =
        virtualFileSystemReadFile(survey->fileSystem, survey->nextFileIndex, 0U, HEADER_SIZE, header);
    MemorySize indexEnd;

    if (read == VIRTUAL_READ_PENDING)
    {
        return DISC_SURVEY_WORKING;
    }
    if (read != VIRTUAL_READ_OK || header[0] != 'D' || header[1] != 'B' || header[2] != 'P' ||
        header[3] != 'F')
    {
        survey->packagesRefused++;
        finishPackage(survey);
        return DISC_SURVEY_WORKING;
    }

    survey->pendingEntryCount = readUnsigned32(header, OFFSET_INDEX_ENTRY_COUNT);
    survey->pendingIndexOffset = readUnsigned32(header, OFFSET_INDEX_OFFSET);
    survey->pendingIndexSize = readUnsigned32(header, OFFSET_INDEX_SIZE);
    indexEnd = (MemorySize)survey->pendingIndexOffset + (MemorySize)survey->pendingIndexSize;
    if (survey->pendingEntryCount == 0U || survey->pendingIndexSize == 0U ||
        indexEnd < (MemorySize)survey->pendingIndexOffset || (Unsigned64)indexEnd > entry->sizeInBytes)
    {
        survey->packagesRefused++;
        finishPackage(survey);
        return DISC_SURVEY_WORKING;
    }
    survey->stage = DISC_SURVEY_STAGE_INDEX;
    return DISC_SURVEY_WORKING;
}

static void keepOnlyTheScriptList(DiscSurvey *survey, MemorySize listMarker, DiscSurveyPending *list,
                                  Unsigned32 listCount)
{
    memoryArenaRewindToMarker(survey->scratch, listMarker);
    survey->scriptsMarker = listMarker;
    survey->scripts = (DiscSurveyPending *)memoryArenaAllocate(
        survey->scratch, (MemorySize)listCount * sizeof(DiscSurveyPending), 4UL);
    if (survey->scripts != list)
    {
        Unsigned32 which;

        for (which = 0U; which < listCount; which++)
        {
            survey->scripts[which] = list[which];
        }
    }
    survey->scriptCount = listCount;
}

static DiscSurveyStatus readIndex(DiscSurvey *survey)
{
    MemorySize marker = memoryArenaGetMarker(survey->scratch);
    MemorySize entrySize;
    MemorySize scriptsBytes;
    DiscSurveyPending *scripts;
    Unsigned8 *indexBytes;
    VirtualReadResult read;
    Unsigned32 which;
    Unsigned32 scriptCount = 0U;
    Boolean familySeen[DISC_SURVEY_FAMILY_LIMIT];

    entrySize = (MemorySize)survey->pendingIndexSize / (MemorySize)survey->pendingEntryCount;
    if ((entrySize != ENTRY_SIZE_WITHOUT_INSTANCE_HIGH && entrySize != ENTRY_SIZE_WITH_INSTANCE_HIGH) ||
        entrySize * (MemorySize)survey->pendingEntryCount != (MemorySize)survey->pendingIndexSize)
    {
        survey->packagesRefused++;
        finishPackage(survey);
        return DISC_SURVEY_WORKING;
    }

    scriptsBytes = (MemorySize)survey->pendingEntryCount * sizeof(DiscSurveyPending);
    scripts = (DiscSurveyPending *)memoryArenaAllocate(survey->scratch, scriptsBytes, 4UL);
    indexBytes = (scripts == NULL_POINTER)
                     ? NULL_POINTER
                     : (Unsigned8 *)memoryArenaAllocate(survey->scratch,
                                                        (MemorySize)survey->pendingIndexSize, 4UL);
    if (indexBytes == NULL_POINTER)
    {
        memoryArenaRewindToMarker(survey->scratch, marker);
        survey->packagesTooLarge++;
        finishPackage(survey);
        return DISC_SURVEY_WORKING;
    }

    read = virtualFileSystemReadFile(survey->fileSystem, survey->nextFileIndex,
                                     (Unsigned64)survey->pendingIndexOffset,
                                     (MemorySize)survey->pendingIndexSize, indexBytes);
    if (read == VIRTUAL_READ_PENDING)
    {
        memoryArenaRewindToMarker(survey->scratch, marker);
        return DISC_SURVEY_WORKING;
    }
    if (read != VIRTUAL_READ_OK)
    {
        memoryArenaRewindToMarker(survey->scratch, marker);
        survey->packagesRefused++;
        finishPackage(survey);
        return DISC_SURVEY_WORKING;
    }

    for (which = 0U; which < DISC_SURVEY_FAMILY_LIMIT; which++)
    {
        familySeen[which] = BOOLEAN_FALSE;
    }

    survey->packagesSeen++;
    if (survey->currentIsNeighborhood)
    {
        const VirtualFileEntry *entry =
            virtualFileSystemGetEntry(survey->fileSystem, survey->nextFileIndex);

        survey->neighborhoodPackages[classifyNeighborhood(entry->path)]++;
        if (survey->neighborhoodSampleCount < DISC_SURVEY_SAMPLE_LIMIT)
        {
            survey->neighborhoodSamples[survey->neighborhoodSampleCount++] = survey->nextFileIndex;
        }
    }

    for (which = 0U; which < survey->pendingEntryCount; which++)
    {
        MemorySize at = (MemorySize)which * entrySize;
        Unsigned32 typeIdentifier = readUnsigned32(indexBytes, at);
        MemorySize locationAt = at + ((entrySize == ENTRY_SIZE_WITH_INSTANCE_HIGH) ? 16UL : 12UL);

        survey->resourcesSeen++;
        tallyFamily(survey, typeIdentifier, familySeen);
        if (survey->currentIsNeighborhood)
        {
            tallyNeighborhoodType(survey, typeIdentifier);
        }
        if (typeIdentifier != DISC_SURVEY_BHAV_TYPE)
        {
            continue;
        }
        scripts[scriptCount].groupIdentifier = readUnsigned32(indexBytes, at + 4UL);
        scripts[scriptCount].offsetInBytes = readUnsigned32(indexBytes, locationAt);
        scripts[scriptCount].sizeInBytes = readUnsigned32(indexBytes, locationAt + 4UL);
        scriptCount++;
    }

    if (scriptCount == 0U)
    {
        memoryArenaRewindToMarker(survey->scratch, marker);
        finishPackage(survey);
        return DISC_SURVEY_WORKING;
    }
    keepOnlyTheScriptList(survey, marker, scripts, scriptCount);
    survey->scriptCursor = 0U;
    survey->scriptsListed += scriptCount;

    if (survey->scriptPackageCount < DISC_SURVEY_SCRIPT_PACKAGE_LIMIT)
    {
        survey->scriptPackages[survey->scriptPackageCount].fileIndex = survey->nextFileIndex;
        survey->scriptPackages[survey->scriptPackageCount].scriptCount = scriptCount;
        survey->scriptPackageCount++;
    }
    else
    {
        survey->scriptPackagesUnlisted++;
    }
    survey->stage = DISC_SURVEY_STAGE_SCRIPTS;
    return DISC_SURVEY_WORKING;
}

static void countOneScript(DiscSurvey *survey, const DiscSurveyPending *script, const Unsigned8 *bytes)
{
    MemorySize size = (MemorySize)script->sizeInBytes;

    if (script->groupIdentifier == DISC_SURVEY_GLOBAL_GROUP)
    {
        survey->scriptsInGlobalGroup++;
    }
    if (compressionLooksLikeRefPack(bytes, size))
    {
        MemorySize marker = memoryArenaGetMarker(survey->scratch);
        MemorySize unpackedSize = compressionGetDecompressedSize(bytes, size);
        Unsigned8 *unpacked = (Unsigned8 *)memoryArenaAllocate(survey->scratch, unpackedSize, 4UL);

        survey->scriptsCompressed++;
        if (unpacked == NULL_POINTER ||
            compressionDecompressRefPack(unpacked, unpackedSize, bytes, size, &unpackedSize) !=
                COMPRESSION_OK)
        {
            survey->scriptsUnreadable++;
        }
        else
        {
            (void)discSurveyCountScript(survey, unpacked, unpackedSize);
        }
        memoryArenaRewindToMarker(survey->scratch, marker);
        return;
    }
    (void)discSurveyCountScript(survey, bytes, size);
}

static DiscSurveyStatus readScripts(DiscSurvey *survey)
{
    const DiscSurveyPending *first = &survey->scripts[survey->scriptCursor];
    Unsigned32 windowStart = first->offsetInBytes;
    Unsigned32 windowEnd = first->offsetInBytes + first->sizeInBytes;
    Unsigned32 last = survey->scriptCursor + 1U;
    MemorySize marker = memoryArenaGetMarker(survey->scratch);
    Unsigned8 *window;
    VirtualReadResult read;
    Unsigned32 which;

    if (windowEnd < windowStart)
    {
        survey->scriptsUnreadable++;
        survey->scriptCursor++;
        if (survey->scriptCursor >= survey->scriptCount)
        {
            finishPackage(survey);
        }
        return DISC_SURVEY_WORKING;
    }

    while (last < survey->scriptCount)
    {
        const DiscSurveyPending *next = &survey->scripts[last];
        Unsigned32 nextEnd = next->offsetInBytes + next->sizeInBytes;

        if (next->offsetInBytes < windowStart || nextEnd < next->offsetInBytes ||
            (MemorySize)(((nextEnd > windowEnd) ? nextEnd : windowEnd) - windowStart) >
                survey->windowBytes)
        {
            break;
        }
        if (nextEnd > windowEnd)
        {
            windowEnd = nextEnd;
        }
        last++;
    }

    window = (Unsigned8 *)memoryArenaAllocate(survey->scratch, (MemorySize)(windowEnd - windowStart),
                                              8UL);
    if (window == NULL_POINTER)
    {
        survey->scriptsUnreadable += last - survey->scriptCursor;
        survey->scriptCursor = last;
        if (survey->scriptCursor >= survey->scriptCount)
        {
            finishPackage(survey);
        }
        return DISC_SURVEY_WORKING;
    }

    read = virtualFileSystemReadFile(survey->fileSystem, survey->nextFileIndex, (Unsigned64)windowStart,
                                     (MemorySize)(windowEnd - windowStart), window);
    if (read == VIRTUAL_READ_PENDING)
    {
        memoryArenaRewindToMarker(survey->scratch, marker);
        return DISC_SURVEY_WORKING;
    }
    survey->scriptReads++;
    if (read != VIRTUAL_READ_OK)
    {
        survey->scriptsUnreadable += last - survey->scriptCursor;
    }
    else
    {
        for (which = survey->scriptCursor; which < last; which++)
        {
            const DiscSurveyPending *script = &survey->scripts[which];

            countOneScript(survey, script, window + (script->offsetInBytes - windowStart));
        }
    }
    memoryArenaRewindToMarker(survey->scratch, marker);

    survey->scriptCursor = last;
    if (survey->scriptCursor >= survey->scriptCount)
    {
        finishPackage(survey);
    }
    return DISC_SURVEY_WORKING;
}

DiscSurveyStatus discSurveyStep(DiscSurvey *survey)
{
    const VirtualFileEntry *entry;

    if (survey->nextFileIndex >= survey->fileSystem->entryCount)
    {
        return DISC_SURVEY_COMPLETE;
    }

    if (survey->stage == DISC_SURVEY_STAGE_SCRIPTS)
    {
        return readScripts(survey);
    }
    if (survey->stage == DISC_SURVEY_STAGE_INDEX)
    {
        return readIndex(survey);
    }

    entry = virtualFileSystemGetEntry(survey->fileSystem, survey->nextFileIndex);
    if (entry == NULL_POINTER || !stringEndsWithIgnoringCase(entry->path, ".package") ||
        entry->sizeInBytes < (Unsigned64)HEADER_SIZE)
    {
        survey->nextFileIndex++;
        return DISC_SURVEY_WORKING;
    }
    survey->currentIsNeighborhood = stringContainsIgnoringCase(entry->path, "neighborhood");
    return readHeader(survey, entry);
}

#define LINE_CAPACITY 320UL

static void appendNumber(char *line, Unsigned64 value)
{
    MemorySize used = stringLength(line);

    (void)stringWriteUnsigned(line + used, LINE_CAPACITY - used, value);
}

static void appendHex(char *line, Unsigned64 value, MemorySize digits)
{
    MemorySize used = stringLength(line);

    (void)stringWriteHexadecimal(line + used, LINE_CAPACITY - used, value, digits);
}

static void appendPercent(char *line, Unsigned32 part, Unsigned32 whole)
{
    Unsigned64 tenths = (whole == 0U) ? 0ULL : ((Unsigned64)part * 1000ULL + whole / 2U) / whole;

    appendNumber(line, tenths / 10ULL);
    stringAppend(line, LINE_CAPACITY, ".");
    appendNumber(line, tenths % 10ULL);
    stringAppend(line, LINE_CAPACITY, "%");
}

static const KnownType *findKnownType(Unsigned32 typeIdentifier)
{
    MemorySize which;

    for (which = 0UL; which < VICTORIA_ARRAY_LENGTH(knownTypes); which++)
    {
        if (knownTypes[which].typeIdentifier == typeIdentifier)
        {
            return &knownTypes[which];
        }
    }
    return NULL_POINTER;
}

static void appendFourCharacterCode(char *line, Unsigned32 typeIdentifier)
{
    char code[5];
    Unsigned32 which;

    for (which = 0U; which < 4U; which++)
    {
        char character = (char)((typeIdentifier >> (24U - which * 8U)) & 0xFFU);

        if (character < 0x20 || character > 0x7E)
        {
            return;
        }
        code[which] = character;
    }
    code[4] = '\0';
    stringAppend(line, LINE_CAPACITY, " '");
    stringAppend(line, LINE_CAPACITY, code);
    stringAppend(line, LINE_CAPACITY, "'");
}

static Boolean rankedTally(const DiscSurveyTypeTally *tallies, Unsigned32 count, Unsigned32 rank,
                           const DiscSurveyTypeTally **result)
{
    Unsigned32 which;

    for (which = 0U; which < count; which++)
    {
        Unsigned32 ahead = 0U;
        Unsigned32 other;

        for (other = 0U; other < count; other++)
        {
            if (tallies[other].resourceCount > tallies[which].resourceCount ||
                (tallies[other].resourceCount == tallies[which].resourceCount && other < which))
            {
                ahead++;
            }
        }
        if (ahead == rank)
        {
            *result = &tallies[which];
            return BOOLEAN_TRUE;
        }
    }
    return BOOLEAN_FALSE;
}

static void reportNeighborhoods(const DiscSurvey *survey, DiscSurveyLog log)
{
    char line[LINE_CAPACITY];
    Unsigned32 totalPackages = survey->neighborhoodPackages[DISC_SURVEY_NEIGHBORHOOD_MAIN] +
                               survey->neighborhoodPackages[DISC_SURVEY_NEIGHBORHOOD_CHARACTER] +
                               survey->neighborhoodPackages[DISC_SURVEY_NEIGHBORHOOD_LOT];
    Unsigned32 standingResources[3] = { 0U, 0U, 0U };
    Unsigned32 standingTypes[3] = { 0U, 0U, 0U };
    Unsigned32 unknownResources = 0U;
    Unsigned32 unknownTypes = 0U;
    Unsigned32 rank;

    log("survey: ---- neighborhoods ----");
    if (totalPackages == 0U)
    {
        log("survey: no package with \"neighborhood\" in its path was readable on this disc. "
            "If the neighborhoods ship inside a compressed installer archive they are not mounted "
            "yet, and this census has nothing to count.");
        return;
    }

    line[0] = '\0';
    stringAppend(line, sizeof(line), "survey: ");
    appendNumber(line, totalPackages);
    stringAppend(line, sizeof(line), " neighborhood package(s) — ");
    appendNumber(line, survey->neighborhoodPackages[DISC_SURVEY_NEIGHBORHOOD_MAIN]);
    stringAppend(line, sizeof(line), " neighborhood, ");
    appendNumber(line, survey->neighborhoodPackages[DISC_SURVEY_NEIGHBORHOOD_CHARACTER]);
    stringAppend(line, sizeof(line), " character, ");
    appendNumber(line, survey->neighborhoodPackages[DISC_SURVEY_NEIGHBORHOOD_LOT]);
    stringAppend(line, sizeof(line), " lot — holding ");
    appendNumber(line, survey->neighborhoodResources);
    stringAppend(line, sizeof(line), " resource(s) of ");
    appendNumber(line, survey->neighborhoodTypeCount);
    stringAppend(line, sizeof(line), " type(s)");
    log(line);

    for (rank = 0U; rank < survey->neighborhoodSampleCount; rank++)
    {
        const VirtualFileEntry *entry =
            virtualFileSystemGetEntry(survey->fileSystem, survey->neighborhoodSamples[rank]);

        line[0] = '\0';
        stringAppend(line, sizeof(line), "survey:   e.g. ");
        stringAppend(line, sizeof(line), (entry != NULL_POINTER) ? entry->path : "?");
        log(line);
    }

    log("survey:   type  resources  packages  standing  name");
    for (rank = 0U; rank < survey->neighborhoodTypeCount; rank++)
    {
        const DiscSurveyTypeTally *tally;
        const KnownType *known;
        const char *standing;

        if (!rankedTally(survey->neighborhoodTypes, survey->neighborhoodTypeCount, rank, &tally))
        {
            break;
        }
        known = findKnownType(tally->typeIdentifier);
        if (known == NULL_POINTER)
        {
            standing = "UNKNOWN";
            unknownResources += tally->resourceCount;
            unknownTypes++;
        }
        else
        {
            standingResources[known->standing] += tally->resourceCount;
            standingTypes[known->standing]++;
            standing = (known->standing == TYPE_READ_HERE)          ? "read here"
                       : (known->standing == TYPE_DECODED_IN_LEGACY) ? "legacy codec"
                                                                     : "name only";
        }

        line[0] = '\0';
        stringAppend(line, sizeof(line), "survey:   ");
        appendHex(line, tally->typeIdentifier, 8UL);
        stringAppend(line, sizeof(line), "  x");
        appendNumber(line, tally->resourceCount);
        stringAppend(line, sizeof(line), "  in ");
        appendNumber(line, tally->packageCount);
        stringAppend(line, sizeof(line), "  ");
        stringAppend(line, sizeof(line), standing);
        if (known != NULL_POINTER)
        {
            stringAppend(line, sizeof(line), "  ");
            stringAppend(line, sizeof(line), known->name);
        }
        else
        {
            appendFourCharacterCode(line, tally->typeIdentifier);
        }
        log(line);
    }
    if (survey->neighborhoodTypeOverflow > 0U)
    {
        line[0] = '\0';
        stringAppend(line, sizeof(line), "survey:   (");
        appendNumber(line, survey->neighborhoodTypeOverflow);
        stringAppend(line, sizeof(line), " resource(s) of types past the table's room)");
        log(line);
    }

    line[0] = '\0';
    stringAppend(line, sizeof(line), "survey: neighborhood resources — read here ");
    appendPercent(line, standingResources[TYPE_READ_HERE], survey->neighborhoodResources);
    stringAppend(line, sizeof(line), " (");
    appendNumber(line, standingTypes[TYPE_READ_HERE]);
    stringAppend(line, sizeof(line), " types), legacy codec ");
    appendPercent(line, standingResources[TYPE_DECODED_IN_LEGACY], survey->neighborhoodResources);
    stringAppend(line, sizeof(line), " (");
    appendNumber(line, standingTypes[TYPE_DECODED_IN_LEGACY]);
    stringAppend(line, sizeof(line), "), name only ");
    appendPercent(line, standingResources[TYPE_NAMED_ONLY], survey->neighborhoodResources);
    stringAppend(line, sizeof(line), " (");
    appendNumber(line, standingTypes[TYPE_NAMED_ONLY]);
    stringAppend(line, sizeof(line), "), unknown ");
    appendPercent(line, unknownResources, survey->neighborhoodResources);
    stringAppend(line, sizeof(line), " (");
    appendNumber(line, unknownTypes);
    stringAppend(line, sizeof(line), ")");
    log(line);
}

static void reportScripts(const DiscSurvey *survey, DiscSurveyLog log)
{
    char line[LINE_CAPACITY];
    Unsigned32 which;
    Unsigned32 primitiveTotal = 0U;
    Unsigned32 distinct = 0U;
    Unsigned32 rank;
    Unsigned32 callTotal;

    log("survey: ---- SimAntics ----");

    log("survey: resources the machine reads, across every package:");
    for (which = 0U; which < VICTORIA_ARRAY_LENGTH(scriptFamily); which++)
    {
        line[0] = '\0';
        stringAppend(line, sizeof(line), "survey:   ");
        stringAppend(line, sizeof(line), scriptFamily[which].name);
        stringAppend(line, sizeof(line), " x");
        appendNumber(line, survey->familyResources[which]);
        stringAppend(line, sizeof(line), " in ");
        appendNumber(line, survey->familyPackages[which]);
        stringAppend(line, sizeof(line), " package(s)");
        log(line);
    }

    if (survey->scriptsListed == 0U)
    {
        log("survey: no BHAV resource was readable on this disc. If Objects.package ships inside a "
            "compressed installer archive it is not mounted yet, and there is nothing to count.");
        return;
    }

    for (which = 0U; which < survey->scriptPackageCount; which++)
    {
        const VirtualFileEntry *entry =
            virtualFileSystemGetEntry(survey->fileSystem, survey->scriptPackages[which].fileIndex);

        line[0] = '\0';
        stringAppend(line, sizeof(line), "survey:   ");
        appendNumber(line, survey->scriptPackages[which].scriptCount);
        stringAppend(line, sizeof(line), " BHAV in ");
        stringAppend(line, sizeof(line), (entry != NULL_POINTER) ? entry->path : "?");
        log(line);
    }
    if (survey->scriptPackagesUnlisted > 0U)
    {
        line[0] = '\0';
        stringAppend(line, sizeof(line), "survey:   and ");
        appendNumber(line, survey->scriptPackagesUnlisted);
        stringAppend(line, sizeof(line), " more package(s)");
        log(line);
    }

    line[0] = '\0';
    stringAppend(line, sizeof(line), "survey: ");
    appendNumber(line, survey->scriptsListed);
    stringAppend(line, sizeof(line), " BHAV listed, ");
    appendNumber(line, survey->scriptsRead);
    stringAppend(line, sizeof(line), " decoded (");
    appendNumber(line, survey->scriptsCompressed);
    stringAppend(line, sizeof(line), " were compressed), ");
    appendNumber(line, survey->scriptsMalformed);
    stringAppend(line, sizeof(line), " malformed, ");
    appendNumber(line, survey->scriptsUnreadable);
    stringAppend(line, sizeof(line), " unreadable; ");
    appendNumber(line, survey->scriptsInGlobalGroup);
    stringAppend(line, sizeof(line), " in the global group; ");
    appendNumber(line, survey->scriptReads);
    stringAppend(line, sizeof(line), " read request(s)");
    log(line);

    line[0] = '\0';
    stringAppend(line, sizeof(line), "survey: formats —");
    for (which = 0U; which < DISC_SURVEY_FORMAT_LIMIT; which++)
    {
        if (survey->formatCounts[which] == 0U)
        {
            continue;
        }
        stringAppend(line, sizeof(line), " ");
        appendHex(line, SCRIPT_FIRST_FORMAT + which, 4UL);
        stringAppend(line, sizeof(line), " x");
        appendNumber(line, survey->formatCounts[which]);
    }
    if (survey->formatOther > 0U)
    {
        stringAppend(line, sizeof(line), "; other x");
        appendNumber(line, survey->formatOther);
    }
    log(line);

    for (which = 0U; which < DISC_SURVEY_PRIMITIVE_LIMIT; which++)
    {
        primitiveTotal += survey->primitiveUses[which];
        if (survey->primitiveUses[which] > 0U)
        {
            distinct++;
        }
    }
    callTotal = survey->globalCalls + survey->privateCalls + survey->semiGlobalCalls;

    line[0] = '\0';
    stringAppend(line, sizeof(line), "survey: ");
    appendNumber(line, survey->instructions);
    stringAppend(line, sizeof(line), " instruction(s), largest tree ");
    appendNumber(line, survey->largestScript);
    stringAppend(line, sizeof(line), " — primitives ");
    appendNumber(line, primitiveTotal);
    stringAppend(line, sizeof(line), ", calls to other trees ");
    appendNumber(line, callTotal);
    stringAppend(line, sizeof(line), " (global ");
    appendNumber(line, survey->globalCalls);
    stringAppend(line, sizeof(line), ", private ");
    appendNumber(line, survey->privateCalls);
    stringAppend(line, sizeof(line), ", semi-global ");
    appendNumber(line, survey->semiGlobalCalls);
    stringAppend(line, sizeof(line), ")");
    log(line);

    line[0] = '\0';
    stringAppend(line, sizeof(line), "survey: ");
    appendNumber(line, distinct);
    stringAppend(line, sizeof(line),
                 " distinct primitive(s) in use, most used first. Names are The Sims 1's "
                 "numbering — leads, not facts, for The Sims 2.");
    log(line);
    log("survey:   opcode  uses  share  in-trees  cumulative  name");

    {
        Unsigned32 cumulative = 0U;
        Boolean used[DISC_SURVEY_PRIMITIVE_LIMIT];

        for (which = 0U; which < DISC_SURVEY_PRIMITIVE_LIMIT; which++)
        {
            used[which] = BOOLEAN_FALSE;
        }
        for (rank = 0U; rank < distinct; rank++)
        {
            Unsigned32 best = DISC_SURVEY_PRIMITIVE_LIMIT;

            for (which = 0U; which < DISC_SURVEY_PRIMITIVE_LIMIT; which++)
            {
                if (used[which] || survey->primitiveUses[which] == 0U)
                {
                    continue;
                }
                if (best == DISC_SURVEY_PRIMITIVE_LIMIT ||
                    survey->primitiveUses[which] > survey->primitiveUses[best])
                {
                    best = which;
                }
            }
            if (best == DISC_SURVEY_PRIMITIVE_LIMIT)
            {
                break;
            }
            used[best] = BOOLEAN_TRUE;
            cumulative += survey->primitiveUses[best];

            line[0] = '\0';
            stringAppend(line, sizeof(line), "survey:   ");
            appendHex(line, best, 2UL);
            stringAppend(line, sizeof(line), "  x");
            appendNumber(line, survey->primitiveUses[best]);
            stringAppend(line, sizeof(line), "  ");
            appendPercent(line, survey->primitiveUses[best], primitiveTotal);
            stringAppend(line, sizeof(line), "  ");
            appendNumber(line, survey->primitiveScripts[best]);
            stringAppend(line, sizeof(line), "  ");
            appendPercent(line, cumulative, primitiveTotal);
            stringAppend(line, sizeof(line), "  ");
            stringAppend(line, sizeof(line),
                         (best < VICTORIA_ARRAY_LENGTH(primitiveNames)) ? primitiveNames[best]
                                                                        : "(no name known)");
            log(line);
        }
    }
}

void discSurveyReport(const DiscSurvey *survey, DiscSurveyLog log)
{
    char line[LINE_CAPACITY];

    line[0] = '\0';
    stringAppend(line, sizeof(line), "survey: surveyed ");
    appendNumber(line, survey->packagesSeen);
    stringAppend(line, sizeof(line), " package(s), ");
    appendNumber(line, survey->resourcesSeen);
    stringAppend(line, sizeof(line), " resource(s)");
    if (survey->packagesRefused > 0U)
    {
        stringAppend(line, sizeof(line), "; ");
        appendNumber(line, survey->packagesRefused);
        stringAppend(line, sizeof(line), " would not be read");
    }
    if (survey->packagesTooLarge > 0U)
    {
        stringAppend(line, sizeof(line), "; ");
        appendNumber(line, survey->packagesTooLarge);
        stringAppend(line, sizeof(line), " had an index too large for the survey's scratch memory");
    }
    log(line);

    reportNeighborhoods(survey, log);
    reportScripts(survey, log);
    log("survey: done");
}
