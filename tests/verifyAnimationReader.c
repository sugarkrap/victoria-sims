
#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/animationReader.h"
#include "victoria/memoryArena.h"

#include <stdio.h>

#define ARENA_CAPACITY (1UL * 1024UL * 1024UL)
static Unsigned8 arenaStorage[ARENA_CAPACITY];

static Boolean nearly(Real32 value, Real32 expected)
{
    Real32 difference = value - expected;

    if (difference < 0.0f)
    {
        difference = -difference;
    }
    return difference < 0.001f ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

#define BUILDER_CAPACITY 8192UL

typedef struct Builder
{
    Unsigned8 bytes[BUILDER_CAPACITY];
    MemorySize length;
} Builder;

static void putUnsigned8(Builder *builder, Unsigned8 value)
{
    if (builder->length < BUILDER_CAPACITY)
    {
        builder->bytes[builder->length] = value;
        builder->length++;
    }
}

static void putUnsigned16(Builder *builder, Unsigned16 value)
{
    putUnsigned8(builder, (Unsigned8)(value & 0xFFU));
    putUnsigned8(builder, (Unsigned8)((value >> 8) & 0xFFU));
}

static void putUnsigned32(Builder *builder, Unsigned32 value)
{
    putUnsigned16(builder, (Unsigned16)(value & 0xFFFFUL));
    putUnsigned16(builder, (Unsigned16)((value >> 16) & 0xFFFFUL));
}

static void putReal32(Builder *builder, Real32 value)
{
    union
    {
        Real32 real;
        Unsigned32 word;
    } bits;

    bits.real = value;
    putUnsigned32(builder, bits.word);
}

static void putPrefixedString(Builder *builder, const char *text)
{
    MemorySize length = stringLength(text);
    MemorySize index;

    putUnsigned8(builder, (Unsigned8)length);
    for (index = 0UL; index < length; index++)
    {
        putUnsigned8(builder, (Unsigned8)text[index]);
    }
}

static void putTerminatedString(Builder *builder, const char *text)
{
    MemorySize length = stringLength(text);
    MemorySize index;

    for (index = 0UL; index < length; index++)
    {
        putUnsigned8(builder, (Unsigned8)text[index]);
    }
    putUnsigned8(builder, 0U);
}

static void putTypeInformation(Builder *builder, const char *name, Unsigned32 identifier,
                               Unsigned32 version)
{
    putPrefixedString(builder, name);
    putUnsigned32(builder, identifier);
    putUnsigned32(builder, version);
}

static void putPadding(Builder *builder, MemorySize runLength)
{
    MemorySize over = runLength % 4UL;
    MemorySize index;

    for (index = 0UL; index < over; index++)
    {
        putUnsigned8(builder, (Unsigned8)index);
    }
}

static Unsigned32 packChannelFlags(Unsigned32 durationTicks, Unsigned32 attribute, Unsigned32 type,
                                   Unsigned32 componentCount)
{
    return (durationTicks & 0x7FFFUL) | ((attribute & 0x1FUL) << 17) | ((type & 0x7UL) << 22) |
           (0xFUL << 25) | ((componentCount & 0x7UL) << 29);
}

#define CHANNEL_COUNT 2U

static void buildAnimation(Builder *builder)
{
    MemorySize runStart;

    putUnsigned32(builder, 0xFFFF0001UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 1U);
    putUnsigned32(builder, (Unsigned32)ANIMATION_TYPE_IDENTIFIER);

    putTypeInformation(builder, "cAnimResourceConst", (Unsigned32)ANIMATION_TYPE_IDENTIFIER, 6U);
    putTypeInformation(builder, "cSGResource", 0xFC6EB1F7UL, 2U);
    putPrefixedString(builder, "a-test-stand_anim");

    putUnsigned32(builder, 0U);
    putUnsigned16(builder, 300U);
    putUnsigned16(builder, 1U);
    putUnsigned16(builder, 0U);
    putUnsigned8(builder, 4U);
    putUnsigned8(builder, 6U);
    putUnsigned8(builder, 1U);
    putUnsigned8(builder, 0U);
    putUnsigned8(builder, 0U);
    putUnsigned8(builder, 6U);

    {
        Unsigned32 index;

        for (index = 0U; index < 4U; index++)
        {
            putUnsigned32(builder, 0U);
        }
        for (index = 0U; index < 9U; index++)
        {
            putReal32(builder, 0.0f);
        }
    }

    runStart = builder->length;
    putTerminatedString(builder, "auskel");
    putTerminatedString(builder, "data");
    putPadding(builder, builder->length - runStart);

    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0U);
    putUnsigned16(builder, 0U);
    putUnsigned16(builder, CHANNEL_COUNT);
    putUnsigned8(builder, 2U);
    putUnsigned8(builder, 0U);
    putUnsigned16(builder, 0U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0U);

    runStart = builder->length;
    putTerminatedString(builder, "auskel");
    putPadding(builder, builder->length - runStart);

    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0x11112222UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, packChannelFlags(300U, ANIMATION_ATTRIBUTE_ROTATION,
                                            ANIMATION_CHANNEL_EULER_ROTATION, 3U));
    putUnsigned32(builder, 0U);

    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0x33334444UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, packChannelFlags(300U, ANIMATION_ATTRIBUTE_TRANSFORM,
                                            ANIMATION_CHANNEL_TRANSFORM_XYZ, 3U));
    putUnsigned32(builder, 0U);

    runStart = builder->length;
    putTerminatedString(builder, "Head");
    putTerminatedString(builder, "spine");
    putPadding(builder, builder->length - runStart);

    {
        Unsigned32 which;

        for (which = 0U; which < 3U; which++)
        {
            putUnsigned16(builder, 2U);
            putUnsigned8(builder, 0x00U);
            putUnsigned8(builder, 0U);
            putUnsigned32(builder, 0U);
        }
        for (which = 0U; which < 3U; which++)
        {
            putUnsigned16(builder, 2U);
            putUnsigned8(builder, 0x05U);
            putUnsigned8(builder, 0U);
            putUnsigned32(builder, 0U);
        }
    }

    {
        Unsigned32 which;

        for (which = 0U; which < 3U; which++)
        {
            putUnsigned16(builder, 128U);
            putUnsigned16(builder, (Unsigned16)((Unsigned32)(-256) & 0xFFFFU));
        }
    }

    {
        Unsigned32 which;

        for (which = 0U; which < 3U; which++)
        {
            putUnsigned16(builder, 0x800AU);
            putUnsigned16(builder, 1024U);
            putUnsigned16(builder, 1024U);

            putUnsigned16(builder, 20U);
            putUnsigned16(builder, 0U);
            putUnsigned16(builder, 0U);
        }
    }
}

int main(void)
{
    MemoryArena arena;
    Builder builder;
    Animation animation;
    AnimationReadResult result;
    Integer32 failureCount = 0;

    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
    builder.length = 0UL;
    buildAnimation(&builder);

    printf("-- reading an animation --\n");
    result = animationReaderOpen(&animation, builder.bytes, builder.length, &arena);
    checkThat(&failureCount, "the reader accepts it", result == ANIMATION_READ_OK);
    if (result != ANIMATION_READ_OK)
    {
        printf("  result: %s\n", animationReadResultGetName(result));
        return checkSummarize(failureCount, "animation reader");
    }

    checkThat(&failureCount, "names the resource", stringEquals(animation.resourceName, "a-test-stand_anim"));
    checkThat(&failureCount, "reports its duration", animation.durationTicks == 300U);
    checkThat(&failureCount, "finds the one target", animation.targetCount == 1U);
    checkThat(&failureCount, "and both of its channels", animation.channelCount == CHANNEL_COUNT);

    printf("\n-- are the padded string runs walked correctly --\n");
    checkThat(&failureCount, "the skeleton it was authored against",
              stringEquals(animation.skeletonTag, "auskel"));
    checkThat(&failureCount, "the first channel's name survives the run after it",
              stringEquals(animation.channels[0].name, "Head"));
    checkThat(&failureCount, "and so does the second's",
              stringEquals(animation.channels[1].name, "spine"));

    printf("\n-- is the packed flag word taken apart --\n");
    checkThat(&failureCount, "the duration comes out of the bottom fifteen bits",
              animation.channels[0].durationTicks == 300U);
    checkThat(&failureCount, "the attribute says the first channel rotates",
              animation.channels[0].attribute == ANIMATION_ATTRIBUTE_ROTATION);
    checkThat(&failureCount, "and that the second translates",
              animation.channels[1].attribute == ANIMATION_ATTRIBUTE_TRANSFORM);
    checkThat(&failureCount, "the type says Euler angles and not a quaternion",
              animation.channels[0].type == ANIMATION_CHANNEL_EULER_ROTATION);
    checkThat(&failureCount, "and a transform for the second",
              animation.channels[1].type == ANIMATION_CHANNEL_TRANSFORM_XYZ);
    checkThat(&failureCount, "both carry three components",
              animation.channels[0].componentCount == 3U &&
                  animation.channels[1].componentCount == 3U);
    checkThat(&failureCount, "and the chains it does not follow are counted",
              animation.chainCount == 2U);

    printf("\n-- are baked keyframes read and spread over the duration --\n");
    checkThat(&failureCount, "both frames arrive",
              animation.channels[0].components[0].keyframeCount == 2U);
    checkThat(&failureCount, "the first sits at the start",
              nearly(animation.channels[0].components[0].keyframes[0].tick, 0.0f));
    checkThat(&failureCount, "and the second half way through the duration",
              nearly(animation.channels[0].components[0].keyframes[1].tick, 150.0f));
    checkThat(&failureCount, "eight-seven fixed point scales by a hundred and twenty eight",
              nearly(animation.channels[0].components[0].keyframes[0].value, 1.0f));
    checkThat(&failureCount, "and sign extends from its own width, not the machine's",
              nearly(animation.channels[0].components[0].keyframes[1].value, -2.0f));

    printf("\n-- does a sixteen bit layout steal its bit from the time --\n");
    checkThat(&failureCount, "the time has the stolen bit masked back out",
              nearly(animation.channels[1].components[0].keyframes[0].tick, 10.0f));
    checkThat(&failureCount, "and the value has it put back in",
              nearly(animation.channels[1].components[0].keyframes[0].value, 2049.0f / 2048.0f));

    printf("\n-- sampling between keyframes --\n");
    {
        const AnimationComponent *baked = &animation.channels[0].components[0];

        checkThat(&failureCount, "before the first keyframe holds at its value",
                  nearly(animationComponentSample(baked, -10.0f), 1.0f));
        checkThat(&failureCount, "on a keyframe is that keyframe",
                  nearly(animationComponentSample(baked, 150.0f), -2.0f));
        checkThat(&failureCount, "half way between is half way along",
                  nearly(animationComponentSample(baked, 75.0f), -0.5f));
        checkThat(&failureCount, "and past the last holds rather than running on",
                  nearly(animationComponentSample(baked, 10000.0f), -2.0f));
    }

    printf("\n-- are the tangents read, and deliberately not followed --\n");
    {
        const AnimationComponent *curved = &animation.channels[1].components[0];

        checkThat(&failureCount, "a continuous curve stores one tangent and means it both ways",
                  nearly(curved->keyframes[0].tangentIn, curved->keyframes[0].tangentOut));
        checkThat(&failureCount, "and 5.10 puts a stored 1024 at a slope of one",
                  nearly(curved->keyframes[0].tangentOut, 1.0f));
        checkThat(&failureCount, "while the keyframe after it is flat",
                  nearly(curved->keyframes[1].tangentOut, 0.0f));

        {
            Real32 straightLine = curved->keyframes[0].value +
                                  ((curved->keyframes[1].value - curved->keyframes[0].value) * 0.5f);
            Real32 spanInSeconds = (curved->keyframes[1].tick - curved->keyframes[0].tick) *
                                   ANIMATION_TICK_SECONDS;
            Real32 hermite = straightLine +
                             (0.125f * spanInSeconds * curved->keyframes[0].tangentOut);

            checkThat(&failureCount, "midway between keyframes is midway in value",
                      nearly(animationComponentSample(curved, 15.0f), straightLine));
            checkThat(&failureCount, "and not along the tangents, at any scale",
                      !nearly(animationComponentSample(curved, 15.0f), hermite));
        }
        checkThat(&failureCount, "every keyframe is still hit exactly",
                  nearly(animationComponentSample(curved, curved->keyframes[0].tick),
                         curved->keyframes[0].value) &&
                      nearly(animationComponentSample(curved, curved->keyframes[1].tick),
                             curved->keyframes[1].value));

        {
            const AnimationComponent *baked = &animation.channels[0].components[0];

            checkThat(&failureCount, "a baked curve carries no tangents and stays a straight line",
                      nearly(animationComponentSample(baked, 37.5f), 0.25f));
        }
    }

    printf("\n-- measuring what unit the tangents are in --\n");
    {
        Real32 slopeToChange = 0.0f;
        Unsigned32 intervals = 0U;

        animationMeasureTangentScale(&animation, &slopeToChange, &intervals);
        checkThat(&failureCount, "it compares the intervals that carry tangents", intervals == 3U);
        checkThat(&failureCount, "and reports slope times span against the change spanned",
                  nearly(slopeToChange, 10.0f / (2049.0f / 2048.0f)));
    }

    printf("\n-- finding a channel by the name a tree spells --\n");
    checkThat(&failureCount, "the case the tree uses still finds it",
              animationFindChannel(&animation, "head") == &animation.channels[0]);
    checkThat(&failureCount, "as does the case the animation uses",
              animationFindChannel(&animation, "Head") == &animation.channels[0]);
    checkThat(&failureCount, "and a bone it does not drive finds nothing",
              animationFindChannel(&animation, "l_hand") == NULL_POINTER);

    printf("\n-- refusing what it should --\n");
    {
        Animation other;
        static const Unsigned8 notAResource[16] = { 0 };

        checkThat(&failureCount, "rejects bytes that are not a collection",
                  animationReaderOpen(&other, notAResource, sizeof(notAResource), &arena) ==
                      ANIMATION_READ_NOT_A_RESOURCE);
        checkThat(&failureCount, "rejects a resource that stops part way",
                  animationReaderOpen(&other, builder.bytes, 48UL, &arena) != ANIMATION_READ_OK);
    }

    return checkSummarize(failureCount, "animation reader");
}
