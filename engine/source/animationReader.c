#include "victoria/animationReader.h"

#include "utils/strings.h"
#include "victoria/resourceCollection.h"

#define MINIMUM_BLOCK_VERSION 6UL

#define SMALLEST_TARGET_BYTES 20UL
#define SMALLEST_CHANNEL_BYTES 20UL

typedef enum KeyframeDataType
{
    KEYFRAME_FIXED_8_7 = 0,
    KEYFRAME_FIXED_9_7,
    KEYFRAME_FIXED_5_10,
    KEYFRAME_FIXED_5_11,
    KEYFRAME_FIXED_3_12,
    KEYFRAME_FIXED_3_13,
    KEYFRAME_FLOAT32
} KeyframeDataType;

const char *animationReadResultGetName(AnimationReadResult result)
{
    switch (result)
    {
    case ANIMATION_READ_OK:
        return "read";
    case ANIMATION_READ_NOT_A_RESOURCE:
        return "not a scenegraph resource";
    case ANIMATION_READ_OLDER_COLLECTION:
        return "an older collection";
    case ANIMATION_READ_WRONG_TYPE:
        return "not an animation";
    case ANIMATION_READ_UNSUPPORTED_VERSION:
        return "a block version this reader has not seen";
    case ANIMATION_READ_TRUNCATED:
        return "stops part way";
    case ANIMATION_READ_NO_CHANNELS:
        return "drives nothing";
    case ANIMATION_READ_OUT_OF_ARENA:
        return "would not fit in the arena";
    case ANIMATION_READ_IMPLAUSIBLE_COUNT:
        return "a count the resource has no room for";
    default:
        return "an unnamed result";
    }
}

const char *animationAttributeGetName(Unsigned32 attribute)
{
    switch (attribute)
    {
    case ANIMATION_ATTRIBUTE_ROTATION:
        return "rotation";
    case ANIMATION_ATTRIBUTE_TRANSFORM:
        return "transform";
    case ANIMATION_ATTRIBUTE_MORPH_WEIGHT:
        return "morph weight";
    case ANIMATION_ATTRIBUTE_CONTACT_IK:
        return "inverse kinematics contact";
    case ANIMATION_ATTRIBUTE_WEIGHT_IK:
        return "inverse kinematics weight";
    default:
        return "an attribute with no name here";
    }
}

const char *animationChannelTypeGetName(Unsigned32 type)
{
    switch (type)
    {
    case ANIMATION_CHANNEL_FLOAT1:
        return "one float";
    case ANIMATION_CHANNEL_FLOAT3:
        return "three floats";
    case ANIMATION_CHANNEL_FLOAT4:
        return "four floats";
    case ANIMATION_CHANNEL_EULER_ROTATION:
        return "Euler rotation";
    case ANIMATION_CHANNEL_TRANSFORM_XYZ:
        return "transform";
    case ANIMATION_CHANNEL_FLOAT2:
        return "two floats";
    default:
        return "a channel type with no name here";
    }
}

static void readNullTerminatedString(ResourceCursor *cursor, char *destination, MemorySize capacity)
{
    MemorySize written = 0UL;

    for (;;)
    {
        Unsigned8 character = resourceCursorReadUnsigned8(cursor);

        if (cursor->overran || character == 0U)
        {
            break;
        }
        if (destination != NULL_POINTER && written + 1UL < capacity)
        {
            destination[written] = (char)character;
            written++;
        }
    }
    if (destination != NULL_POINTER && capacity > 0UL)
    {
        destination[written] = '\0';
    }
}

static void skipPadding(ResourceCursor *cursor, MemorySize runLength)
{
    MemorySize over = runLength % 4UL;

    if (over > 0UL)
    {
        (void)resourceCursorSkip(cursor, over);
    }
}

static KeyframeDataType decodeDataType(Unsigned8 packed, AnimationCurveType curveType,
                                       Boolean *understood)
{
    Unsigned32 topThreeBits;

    *understood = BOOLEAN_TRUE;
    if ((packed & 0x10U) != 0U)
    {
        return KEYFRAME_FLOAT32;
    }
    topThreeBits = ((Unsigned32)packed >> 2) & 0x7U;
    if (topThreeBits == 0U)
    {
        return (curveType == ANIMATION_CURVE_BAKED) ? KEYFRAME_FIXED_8_7 : KEYFRAME_FIXED_9_7;
    }
    if (topThreeBits == 1U)
    {
        return (curveType == ANIMATION_CURVE_BAKED) ? KEYFRAME_FIXED_5_10 : KEYFRAME_FIXED_5_11;
    }
    if (topThreeBits == 3U)
    {
        return (curveType == ANIMATION_CURVE_BAKED) ? KEYFRAME_FIXED_3_12 : KEYFRAME_FIXED_3_13;
    }
    *understood = BOOLEAN_FALSE;
    return KEYFRAME_FLOAT32;
}

static Unsigned32 bitsForDataType(KeyframeDataType type)
{
    switch (type)
    {
    case KEYFRAME_FIXED_8_7:
    case KEYFRAME_FIXED_5_10:
    case KEYFRAME_FIXED_3_12:
        return 15U;
    case KEYFRAME_FIXED_9_7:
    case KEYFRAME_FIXED_5_11:
    case KEYFRAME_FIXED_3_13:
        return 16U;
    default:
        return 0U;
    }
}

static Real32 divisorForDataType(KeyframeDataType type)
{
    switch (type)
    {
    case KEYFRAME_FIXED_8_7:
    case KEYFRAME_FIXED_9_7:
        return 128.0f;
    case KEYFRAME_FIXED_5_10:
        return 1024.0f;
    case KEYFRAME_FIXED_5_11:
        return 2048.0f;
    case KEYFRAME_FIXED_3_12:
        return 4096.0f;
    case KEYFRAME_FIXED_3_13:
        return 8192.0f;
    default:
        return 1.0f;
    }
}

static Real32 fixedPointToReal(KeyframeDataType type, Integer32 stored)
{
    Unsigned32 bits = bitsForDataType(type);
    Integer32 signBit = (Integer32)(1UL << bits);

    if (bits == 0U)
    {
        return 0.0f;
    }
    if ((stored & signBit) != 0)
    {
        stored -= (signBit << 1);
    }
    return (Real32)stored / divisorForDataType(type);
}

static Real32 readKeyframeValue(ResourceCursor *cursor, KeyframeDataType type,
                                Unsigned16 *precedingHalfword, Boolean *understood)
{
    Integer32 stored;

    if (type == KEYFRAME_FLOAT32)
    {
        return resourceCursorReadReal32(cursor);
    }
    if (bitsForDataType(type) == 16U)
    {
        if (precedingHalfword == NULL_POINTER)
        {
            *understood = BOOLEAN_FALSE;
            return 0.0f;
        }
        stored = (Integer32)resourceCursorReadUnsigned16(cursor);
        stored = (stored << 1) | (Integer32)((*precedingHalfword >> 15) & 1U);
        *precedingHalfword = (Unsigned16)(*precedingHalfword & 0x7FFFU);
        return fixedPointToReal(type, stored);
    }
    stored = (Integer32)resourceCursorReadUnsigned16(cursor);
    return fixedPointToReal(type, stored);
}

Real32 animationComponentSample(const AnimationComponent *component, Real32 tick)
{
    Unsigned32 index;

    if (component->keyframeCount == 0U || component->keyframes == NULL_POINTER)
    {
        return 0.0f;
    }
    if (tick <= component->keyframes[0].tick)
    {
        return component->keyframes[0].value;
    }
    for (index = 1U; index < component->keyframeCount; index++)
    {
        const AnimationKeyframe *before = &component->keyframes[index - 1U];
        const AnimationKeyframe *after = &component->keyframes[index];
        Real32 span;

        if (tick > after->tick)
        {
            continue;
        }
        span = after->tick - before->tick;
        if (span <= 0.0f)
        {
            return after->value;
        }
        return before->value + ((after->value - before->value) * ((tick - before->tick) / span));
    }
    return component->keyframes[component->keyframeCount - 1U].value;
}

void animationMeasureTangentScale(const Animation *animation, Real32 *slopeToChange,
                                  Unsigned32 *intervalsCompared)
{
    Real32 slopeTotal = 0.0f;
    Real32 changeTotal = 0.0f;
    Unsigned32 compared = 0U;
    Unsigned32 index;

    *slopeToChange = 0.0f;
    *intervalsCompared = 0U;
    if (animation->channels == NULL_POINTER)
    {
        return;
    }

    for (index = 0U; index < animation->channelCount; index++)
    {
        const AnimationChannel *channel = &animation->channels[index];
        Unsigned32 part;

        for (part = 0U; part < channel->componentCount; part++)
        {
            const AnimationComponent *component = &channel->components[part];
            Unsigned32 which;

            if (component->curveType == ANIMATION_CURVE_BAKED || component->keyframes == NULL_POINTER)
            {
                continue;
            }
            for (which = 1U; which < component->keyframeCount; which++)
            {
                const AnimationKeyframe *before = &component->keyframes[which - 1U];
                const AnimationKeyframe *after = &component->keyframes[which];
                Real32 span = after->tick - before->tick;
                Real32 slope = before->tangentOut;
                Real32 change = after->value - before->value;

                if (span <= 0.0f)
                {
                    continue;
                }
                if (slope < 0.0f)
                {
                    slope = -slope;
                }
                if (change < 0.0f)
                {
                    change = -change;
                }
                slopeTotal += slope * span;
                changeTotal += change;
                compared++;
            }
        }
    }

    if (compared == 0U || changeTotal <= 0.0f)
    {
        return;
    }
    *slopeToChange = slopeTotal / changeTotal;
    *intervalsCompared = compared;
}

const AnimationChannel *animationFindChannel(const Animation *animation, const char *name)
{
    Unsigned32 index;

    if (animation->channels == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    for (index = 0U; index < animation->channelCount; index++)
    {
        if (stringEqualsIgnoringCase(animation->channels[index].name, name))
        {
            return &animation->channels[index];
        }
    }
    return NULL_POINTER;
}

AnimationReadResult animationReaderOpen(Animation *animation, const Unsigned8 *bytes,
                                        MemorySize sizeInBytes, MemoryArena *arena)
{
    ResourceCursor cursor;
    ResourceCollection collection;
    ResourceCollectionResult collectionResult;
    PersistTypeInfo blockType;
    AnimationChannel *channels;
    Unsigned32 *channelsPerTarget;
    Unsigned32 targetCount;
    Unsigned32 channelTotal = 0U;
    Unsigned32 index;
    Unsigned32 inner;
    MemorySize runStart;
    Unsigned8 skeletonTagLength;
    Unsigned8 dataStringLength;

    animation->resourceName[0] = '\0';
    animation->skeletonTag[0] = '\0';
    animation->blockVersion = 0U;
    animation->durationTicks = 0U;
    animation->targetCount = 0U;
    animation->channels = NULL_POINTER;
    animation->channelCount = 0U;
    animation->chainCount = 0U;

    collectionResult = resourceCollectionOpen(&collection, &cursor, bytes, sizeInBytes);
    if (collectionResult != RESOURCE_COLLECTION_OK)
    {
        switch (collectionResult)
        {
        case RESOURCE_COLLECTION_NOT_A_RESOURCE:
            return ANIMATION_READ_NOT_A_RESOURCE;
        case RESOURCE_COLLECTION_OLDER:
            return ANIMATION_READ_OLDER_COLLECTION;
        default:
            return ANIMATION_READ_TRUNCATED;
        }
    }

    resourceCursorReadTypeInformation(&cursor, &blockType);
    if (cursor.overran)
    {
        return ANIMATION_READ_TRUNCATED;
    }
    if (blockType.typeIdentifier != (Unsigned32)ANIMATION_TYPE_IDENTIFIER)
    {
        return ANIMATION_READ_WRONG_TYPE;
    }
    animation->blockVersion = blockType.version;
    if (blockType.version < MINIMUM_BLOCK_VERSION)
    {
        return ANIMATION_READ_UNSUPPORTED_VERSION;
    }

    resourceCursorReadTypeInformation(&cursor, NULL_POINTER);
    resourceCursorReadString(&cursor, animation->resourceName, ANIMATION_NAME_LIMIT);

    (void)resourceCursorReadUnsigned32(&cursor);

    animation->durationTicks = (Unsigned32)resourceCursorReadUnsigned16(&cursor);
    targetCount = (Unsigned32)resourceCursorReadUnsigned16(&cursor);
    (void)resourceCursorReadUnsigned16(&cursor);
    dataStringLength = resourceCursorReadUnsigned8(&cursor);
    (void)resourceCursorReadUnsigned8(&cursor);
    (void)resourceCursorReadUnsigned8(&cursor);
    (void)resourceCursorReadUnsigned8(&cursor);
    (void)resourceCursorReadUnsigned8(&cursor);
    skeletonTagLength = resourceCursorReadUnsigned8(&cursor);

    (void)resourceCursorSkip(&cursor, 4UL * 4UL);
    (void)resourceCursorSkip(&cursor, 9UL * 4UL);

    if (cursor.overran)
    {
        return ANIMATION_READ_TRUNCATED;
    }
    animation->targetCount = targetCount;
    if (targetCount == 0U)
    {
        return ANIMATION_READ_NO_CHANNELS;
    }
    if ((MemorySize)targetCount > sizeInBytes / SMALLEST_TARGET_BYTES)
    {
        return ANIMATION_READ_IMPLAUSIBLE_COUNT;
    }

    runStart = cursor.position;
    readNullTerminatedString(&cursor, animation->skeletonTag, ANIMATION_NAME_LIMIT);
    readNullTerminatedString(&cursor, NULL_POINTER, 0UL);
    skipPadding(&cursor, cursor.position - runStart);
    if (cursor.overran)
    {
        return ANIMATION_READ_TRUNCATED;
    }
    if (stringLength(animation->skeletonTag) != (MemorySize)skeletonTagLength &&
        skeletonTagLength < ANIMATION_NAME_LIMIT)
    {
        return ANIMATION_READ_TRUNCATED;
    }
    (void)dataStringLength;

    channelsPerTarget = (Unsigned32 *)memoryArenaAllocate(
        arena, (MemorySize)targetCount * sizeof(Unsigned32), sizeof(Unsigned32));
    if (channelsPerTarget == NULL_POINTER)
    {
        return ANIMATION_READ_OUT_OF_ARENA;
    }

    for (index = 0U; index < targetCount; index++)
    {
        (void)resourceCursorSkip(&cursor, 4UL * 2UL);
        (void)resourceCursorReadUnsigned16(&cursor);
        channelsPerTarget[index] = (Unsigned32)resourceCursorReadUnsigned16(&cursor);
        animation->chainCount += (Unsigned32)resourceCursorReadUnsigned8(&cursor);
        (void)resourceCursorReadUnsigned8(&cursor);
        (void)resourceCursorReadUnsigned16(&cursor);
        (void)resourceCursorSkip(&cursor, 4UL * 3UL);
        channelTotal += channelsPerTarget[index];
    }
    if (cursor.overran)
    {
        return ANIMATION_READ_TRUNCATED;
    }
    if (channelTotal == 0U)
    {
        return ANIMATION_READ_NO_CHANNELS;
    }
    if ((MemorySize)channelTotal > sizeInBytes / SMALLEST_CHANNEL_BYTES)
    {
        return ANIMATION_READ_IMPLAUSIBLE_COUNT;
    }

    runStart = cursor.position;
    for (index = 0U; index < targetCount; index++)
    {
        readNullTerminatedString(&cursor, NULL_POINTER, 0UL);
    }
    skipPadding(&cursor, cursor.position - runStart);

    channels = (AnimationChannel *)memoryArenaAllocate(
        arena, (MemorySize)channelTotal * sizeof(AnimationChannel), sizeof(MemorySize));
    if (channels == NULL_POINTER)
    {
        return ANIMATION_READ_OUT_OF_ARENA;
    }

    for (index = 0U; index < channelTotal; index++)
    {
        Unsigned32 flags;

        (void)resourceCursorSkip(&cursor, 4UL * 2UL);
        (void)resourceCursorReadUnsigned32(&cursor);
        (void)resourceCursorReadUnsigned32(&cursor);
        flags = resourceCursorReadUnsigned32(&cursor);
        (void)resourceCursorReadUnsigned32(&cursor);

        channels[index].name[0] = '\0';
        channels[index].flags = flags;
        channels[index].durationTicks = flags & 0x7FFFU;
        channels[index].attribute = (AnimationAttribute)((flags >> 17) & 0x1FU);
        channels[index].type = (AnimationChannelType)((flags >> 22) & 0x7U);
        channels[index].componentCount = (flags >> 29) & 0x7U;
        if (channels[index].componentCount > 4U)
        {
            channels[index].componentCount = 4U;
        }
        for (inner = 0U; inner < 4U; inner++)
        {
            channels[index].components[inner].curveType = ANIMATION_CURVE_BAKED;
            channels[index].components[inner].keyframeCount = 0U;
            channels[index].components[inner].keyframes = NULL_POINTER;
        }
    }
    if (cursor.overran)
    {
        return ANIMATION_READ_TRUNCATED;
    }

    runStart = cursor.position;
    for (index = 0U; index < channelTotal; index++)
    {
        readNullTerminatedString(&cursor, channels[index].name, ANIMATION_NAME_LIMIT);
    }
    skipPadding(&cursor, cursor.position - runStart);
    if (cursor.overran)
    {
        return ANIMATION_READ_TRUNCATED;
    }

    {
        KeyframeDataType *dataTypes = (KeyframeDataType *)memoryArenaAllocate(
            arena, (MemorySize)channelTotal * 4UL * sizeof(KeyframeDataType),
            sizeof(MemorySize));

        if (dataTypes == NULL_POINTER)
        {
            return ANIMATION_READ_OUT_OF_ARENA;
        }
        for (index = 0U; index < channelTotal; index++)
        {
            for (inner = 0U; inner < channels[index].componentCount; inner++)
            {
                AnimationComponent *component = &channels[index].components[inner];
                Unsigned32 keyframeCount = (Unsigned32)resourceCursorReadUnsigned16(&cursor);
                Unsigned8 packed = resourceCursorReadUnsigned8(&cursor);
                Boolean understood = BOOLEAN_TRUE;

                (void)resourceCursorSkip(&cursor, 1UL + 4UL);

                component->curveType = (AnimationCurveType)(packed & 0x3U);
                component->keyframeCount = keyframeCount;
                dataTypes[index * 4U + inner] =
                    decodeDataType(packed, component->curveType, &understood);
                if (!understood || (packed & 0x3U) > 2U)
                {
                    component->keyframeCount = 0U;
                }
            }
        }
        if (cursor.overran)
        {
            return ANIMATION_READ_TRUNCATED;
        }

        for (index = 0U; index < channelTotal; index++)
        {
            for (inner = 0U; inner < channels[index].componentCount; inner++)
            {
                AnimationComponent *component = &channels[index].components[inner];
                KeyframeDataType dataType = dataTypes[index * 4U + inner];
                AnimationKeyframe *keyframes;
                Unsigned32 which;

                if (component->keyframeCount == 0U)
                {
                    continue;
                }
                if ((MemorySize)component->keyframeCount > sizeInBytes / 2UL)
                {
                    return ANIMATION_READ_IMPLAUSIBLE_COUNT;
                }
                keyframes = (AnimationKeyframe *)memoryArenaAllocate(
                    arena, (MemorySize)component->keyframeCount * sizeof(AnimationKeyframe),
                    sizeof(Real32));
                if (keyframes == NULL_POINTER)
                {
                    return ANIMATION_READ_OUT_OF_ARENA;
                }
                for (which = 0U; which < component->keyframeCount; which++)
                {
                    Boolean understood = BOOLEAN_TRUE;

                    keyframes[which].tangentIn = 0.0f;
                    keyframes[which].tangentOut = 0.0f;
                    if (component->curveType == ANIMATION_CURVE_BAKED)
                    {
                        keyframes[which].tick =
                            (Real32)channels[index].durationTicks *
                            ((Real32)which / (Real32)component->keyframeCount);
                        keyframes[which].value =
                            readKeyframeValue(&cursor, dataType, NULL_POINTER, &understood);
                    }
                    else
                    {
                        Unsigned16 time = resourceCursorReadUnsigned16(&cursor);
                        Real32 value = readKeyframeValue(&cursor, dataType, &time, &understood);

                        keyframes[which].tick = (Real32)time;
                        keyframes[which].value = value;

                        if (component->curveType == ANIMATION_CURVE_DISCONTINUOUS)
                        {
                            keyframes[which].tangentIn = fixedPointToReal(
                                KEYFRAME_FIXED_5_10,
                                (Integer32)resourceCursorReadUnsigned16(&cursor));
                            keyframes[which].tangentOut = fixedPointToReal(
                                KEYFRAME_FIXED_5_10,
                                (Integer32)resourceCursorReadUnsigned16(&cursor));
                        }
                        else
                        {
                            keyframes[which].tangentIn = fixedPointToReal(
                                KEYFRAME_FIXED_5_10,
                                (Integer32)resourceCursorReadUnsigned16(&cursor));
                            keyframes[which].tangentOut = keyframes[which].tangentIn;
                        }
                    }
                    if (!understood)
                    {
                        return ANIMATION_READ_TRUNCATED;
                    }
                }
                if (cursor.overran)
                {
                    return ANIMATION_READ_TRUNCATED;
                }
                component->keyframes = keyframes;
            }
        }
    }

    animation->channels = channels;
    animation->channelCount = channelTotal;
    return ANIMATION_READ_OK;
}
