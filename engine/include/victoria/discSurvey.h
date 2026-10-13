#ifndef VICTORIA_DISC_SURVEY_HEADER
#define VICTORIA_DISC_SURVEY_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

#define DISC_SURVEY_TYPE_LIMIT 192U
#define DISC_SURVEY_PRIMITIVE_LIMIT 256U
#define DISC_SURVEY_FORMAT_LIMIT 10U
#define DISC_SURVEY_SAMPLE_LIMIT 4U
#define DISC_SURVEY_SCRIPT_PACKAGE_LIMIT 12U
#define DISC_SURVEY_FAMILY_LIMIT 16U

#define DISC_SURVEY_BHAV_TYPE 0x42484156UL
#define DISC_SURVEY_GLOBAL_GROUP 0x7FD46CD0UL

typedef enum DiscSurveyStatus
{
    DISC_SURVEY_COMPLETE = 0,
    DISC_SURVEY_WORKING
} DiscSurveyStatus;

typedef enum DiscSurveyNeighborhoodKind
{
    DISC_SURVEY_NEIGHBORHOOD_MAIN = 0,
    DISC_SURVEY_NEIGHBORHOOD_CHARACTER,
    DISC_SURVEY_NEIGHBORHOOD_LOT,
    DISC_SURVEY_NEIGHBORHOOD_KIND_COUNT
} DiscSurveyNeighborhoodKind;

typedef struct DiscSurveyTypeTally
{
    Unsigned32 typeIdentifier;
    Unsigned32 resourceCount;
    Unsigned32 packageCount;
    Unsigned32 lastPackage;
} DiscSurveyTypeTally;

typedef struct DiscSurveyScriptPackage
{
    Unsigned32 fileIndex;
    Unsigned32 scriptCount;
} DiscSurveyScriptPackage;

typedef struct DiscSurveyPending
{
    Unsigned32 groupIdentifier;
    Unsigned32 offsetInBytes;
    Unsigned32 sizeInBytes;
} DiscSurveyPending;

typedef enum DiscSurveyStage
{
    DISC_SURVEY_STAGE_HEADER = 0,
    DISC_SURVEY_STAGE_INDEX,
    DISC_SURVEY_STAGE_SCRIPTS
} DiscSurveyStage;

typedef struct DiscSurvey
{
    VirtualFileSystem *fileSystem;
    MemoryArena *scratch;
    MemorySize windowBytes;

    Unsigned32 nextFileIndex;
    DiscSurveyStage stage;
    Unsigned32 pendingEntryCount;
    Unsigned32 pendingIndexOffset;
    Unsigned32 pendingIndexSize;
    Boolean currentIsNeighborhood;

    DiscSurveyPending *scripts;
    Unsigned32 scriptCount;
    Unsigned32 scriptCursor;
    MemorySize scriptsMarker;

    Unsigned32 packagesSeen;
    Unsigned32 packagesRefused;
    Unsigned32 packagesTooLarge;
    Unsigned32 resourcesSeen;

    Unsigned32 neighborhoodPackages[DISC_SURVEY_NEIGHBORHOOD_KIND_COUNT];
    Unsigned32 neighborhoodResources;
    Unsigned32 neighborhoodSamples[DISC_SURVEY_SAMPLE_LIMIT];
    Unsigned32 neighborhoodSampleCount;
    DiscSurveyTypeTally neighborhoodTypes[DISC_SURVEY_TYPE_LIMIT];
    Unsigned32 neighborhoodTypeCount;
    Unsigned32 neighborhoodTypeOverflow;

    Unsigned32 familyResources[DISC_SURVEY_FAMILY_LIMIT];
    Unsigned32 familyPackages[DISC_SURVEY_FAMILY_LIMIT];

    DiscSurveyScriptPackage scriptPackages[DISC_SURVEY_SCRIPT_PACKAGE_LIMIT];
    Unsigned32 scriptPackageCount;
    Unsigned32 scriptPackagesUnlisted;

    Unsigned32 scriptsListed;
    Unsigned32 scriptReads;
    Unsigned32 scriptsRead;
    Unsigned32 scriptsCompressed;
    Unsigned32 scriptsUnreadable;
    Unsigned32 scriptsMalformed;
    Unsigned32 scriptsInGlobalGroup;
    Unsigned32 formatCounts[DISC_SURVEY_FORMAT_LIMIT];
    Unsigned32 formatOther;

    Unsigned32 instructions;
    Unsigned32 primitiveUses[DISC_SURVEY_PRIMITIVE_LIMIT];
    Unsigned32 primitiveScripts[DISC_SURVEY_PRIMITIVE_LIMIT];
    Unsigned32 globalCalls;
    Unsigned32 privateCalls;
    Unsigned32 semiGlobalCalls;
    Unsigned32 largestScript;
} DiscSurvey;

void discSurveyBegin(DiscSurvey *survey, VirtualFileSystem *fileSystem, MemoryArena *scratch);

DiscSurveyStatus discSurveyStep(DiscSurvey *survey);

Boolean discSurveyCountScript(DiscSurvey *survey, const Unsigned8 *bytes, MemorySize size);

typedef void (*DiscSurveyLog)(const char *line);

void discSurveyReport(const DiscSurvey *survey, DiscSurveyLog log);

#endif
