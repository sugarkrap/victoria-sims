#ifndef VICTORIA_ANIMATION_READER_HEADER
#define VICTORIA_ANIMATION_READER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

#define ANIMATION_TYPE_IDENTIFIER 0xFB00791EUL

#define ANIMATION_FRAMES_PER_TICK 0.03f

#define ANIMATION_TICK_SECONDS (ANIMATION_FRAMES_PER_TICK / 24.0f)

#define ANIMATION_NAME_LIMIT 64UL

typedef enum AnimationChannelType
{
    ANIMATION_CHANNEL_FLOAT1 = 0,
    ANIMATION_CHANNEL_FLOAT3 = 1,
    ANIMATION_CHANNEL_FLOAT4 = 2,
    ANIMATION_CHANNEL_EULER_ROTATION = 3,
    ANIMATION_CHANNEL_TRANSFORM_XYZ = 4,
    ANIMATION_CHANNEL_FLOAT2 = 5
} AnimationChannelType;

typedef enum AnimationAttribute
{
    ANIMATION_ATTRIBUTE_ROTATION = 0,
    ANIMATION_ATTRIBUTE_TRANSFORM = 1,
    ANIMATION_ATTRIBUTE_MORPH_WEIGHT = 2,
    ANIMATION_ATTRIBUTE_CONTACT_IK = 3,
    ANIMATION_ATTRIBUTE_WEIGHT_IK = 4
} AnimationAttribute;

const char *animationAttributeGetName(Unsigned32 attribute);
const char *animationChannelTypeGetName(Unsigned32 type);

typedef enum AnimationCurveType
{
    ANIMATION_CURVE_BAKED = 0,
    ANIMATION_CURVE_CONTINUOUS = 1,
    ANIMATION_CURVE_DISCONTINUOUS = 2
} AnimationCurveType;

typedef struct AnimationKeyframe
{
    Real32 tick;
    Real32 value;
    Real32 tangentIn;
    Real32 tangentOut;
} AnimationKeyframe;

typedef struct AnimationComponent
{
    AnimationCurveType curveType;
    Unsigned32 keyframeCount;
    const AnimationKeyframe *keyframes;
} AnimationComponent;

typedef struct AnimationChannel
{
    char name[ANIMATION_NAME_LIMIT];
    Unsigned32 flags;
    Unsigned32 durationTicks;
    AnimationAttribute attribute;
    AnimationChannelType type;
    Unsigned32 componentCount;
    AnimationComponent components[4];
} AnimationChannel;

typedef enum AnimationReadResult
{
    ANIMATION_READ_OK = 0,
    ANIMATION_READ_NOT_A_RESOURCE,
    ANIMATION_READ_OLDER_COLLECTION,
    ANIMATION_READ_WRONG_TYPE,
    ANIMATION_READ_UNSUPPORTED_VERSION,
    ANIMATION_READ_TRUNCATED,
    ANIMATION_READ_NO_CHANNELS,
    ANIMATION_READ_OUT_OF_ARENA,
    ANIMATION_READ_IMPLAUSIBLE_COUNT
} AnimationReadResult;

#define ANIMATION_READ_RESULT_COUNT 9U

const char *animationReadResultGetName(AnimationReadResult result);

typedef struct Animation
{
    char resourceName[ANIMATION_NAME_LIMIT];
    char skeletonTag[ANIMATION_NAME_LIMIT];

    Unsigned32 blockVersion;
    Unsigned32 durationTicks;
    Unsigned32 targetCount;
    const AnimationChannel *channels;
    Unsigned32 channelCount;
    Unsigned32 chainCount;
} Animation;

AnimationReadResult animationReaderOpen(Animation *animation, const Unsigned8 *bytes,
                                        MemorySize sizeInBytes, MemoryArena *arena);

Real32 animationComponentSample(const AnimationComponent *component, Real32 tick);

void animationMeasureTangentScale(const Animation *animation, Real32 *slopeToChange,
                                  Unsigned32 *intervalsCompared);

const AnimationChannel *animationFindChannel(const Animation *animation, const char *name);

#endif
