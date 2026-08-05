#ifndef VICTORIA_ANIMATION_READER_HEADER
#define VICTORIA_ANIMATION_READER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

/* Reads an ANIM — a cAnimResourceConst block — into something a pose can be
 * sampled out of.
 *
 * An animation names the bones it drives by string, not by number: a channel
 * carries a name like "l_forearm" and the tree carries a node of that name. So
 * nothing here needs the bone identifiers a mesh's primitives use, and matching
 * an animation to a skeleton is a name lookup rather than an index.
 *
 * What is read: the targets, their channels, each channel's components, and the
 * keyframes in them. What is deliberately not read:
 *
 *   - IK chains. They sit at the end of the block, after everything here, and
 *     posing a mesh does not need them. Their presence is counted so a caller
 *     can see an animation is carrying them.
 *   - Event keys, and the locomotion figures beyond the few kept below.
 *
 * The file stores keyframe values in seven different encodings — six fixed
 * point layouts and plain floats — and two of the fixed point ones steal a bit
 * from the halfword before them. All of that is undone here, so a caller sees
 * floats and never a raw fixed point number. */

#define ANIMATION_TYPE_IDENTIFIER 0xFB00791EUL

/* The format's own conversion between a tick and a frame. Kept because a
   duration in ticks is meaningless without it. */
#define ANIMATION_FRAMES_PER_TICK 0.03f

/* And from a tick to a second: the frames above over the game's twenty four a
   second, so a tick is an eight hundredth of a second. Spelled as the division
   rather than as 0.00125 so it stays visibly derived from the two numbers it
   comes from. */
#define ANIMATION_TICK_SECONDS (ANIMATION_FRAMES_PER_TICK / 24.0f)

#define ANIMATION_NAME_LIMIT 64UL

/* What a channel's numbers mean. The names are the format's own. */
typedef enum AnimationChannelType
{
    ANIMATION_CHANNEL_FLOAT1 = 0,
    ANIMATION_CHANNEL_FLOAT3 = 1,
    ANIMATION_CHANNEL_FLOAT4 = 2,
    /* Called a quaternion channel by the format, and stored as Euler angles in
       degrees regardless. Reading it as a quaternion yields a rotation that is
       wrong in a way that still looks like a rotation. */
    ANIMATION_CHANNEL_EULER_ROTATION = 3,
    ANIMATION_CHANNEL_TRANSFORM_XYZ = 4,
    ANIMATION_CHANNEL_FLOAT2 = 5
} AnimationChannelType;

/* Which property of the bone a channel drives. Only the first two matter to a
   pose; the rest are kept so a channel that is skipped can say what it was. */
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

/* How a component's keyframes carry their tangents, which also decides whether
   they carry a time of their own. */
typedef enum AnimationCurveType
{
    /* One frame per step, evenly spread across the channel's duration. No time
       is stored, because the position in the array is the time. */
    ANIMATION_CURVE_BAKED = 0,
    ANIMATION_CURVE_CONTINUOUS = 1,
    ANIMATION_CURVE_DISCONTINUOUS = 2
} AnimationCurveType;

typedef struct AnimationKeyframe
{
    /* In ticks. Derived from the position in the array for a baked curve, and
       read from the file for the other two. */
    Real32 tick;
    Real32 value;
    /* The slopes the curve leaves the previous keyframe with and arrives at
     * this one with, in value per tick, both stored as 5.10 fixed point.
     *
     * A continuous curve stores one and means both — that is what makes it
     * continuous — and a discontinuous one stores them separately so the curve
     * can break at the keyframe. A baked curve stores neither: its frames are
     * one per step already, so there is nothing to interpolate along and both
     * are left at nought. */
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
    /* The name of the bone this drives, which is a node's name in the tree. */
    char name[ANIMATION_NAME_LIMIT];
    Unsigned32 flags;
    Unsigned32 durationTicks;
    AnimationAttribute attribute;
    AnimationChannelType type;
    /* One to four. Euler rotation and transform channels both carry three. */
    Unsigned32 componentCount;
    AnimationComponent components[4];
} AnimationChannel;

typedef enum AnimationReadResult
{
    ANIMATION_READ_OK = 0,
    ANIMATION_READ_NOT_A_RESOURCE,
    ANIMATION_READ_OLDER_COLLECTION,
    ANIMATION_READ_WRONG_TYPE,
    /* The block declares a version this reader has not seen. Distinguished from
       a wrong type because the two call for opposite responses. */
    ANIMATION_READ_UNSUPPORTED_VERSION,
    ANIMATION_READ_TRUNCATED,
    ANIMATION_READ_NO_CHANNELS,
    ANIMATION_READ_OUT_OF_ARENA,
    /* A count larger than the resource has bytes to describe, which means the
       count is not a count. */
    ANIMATION_READ_IMPLAUSIBLE_COUNT
} AnimationReadResult;

#define ANIMATION_READ_RESULT_COUNT 9U

const char *animationReadResultGetName(AnimationReadResult result);

typedef struct Animation
{
    char resourceName[ANIMATION_NAME_LIMIT];
    /* The skeleton this was authored against — "auskel" for a Sim. A pose built
       from an animation whose tag does not match the model's is not wrong so
       much as meaningless, so this is kept for a caller to check. */
    char skeletonTag[ANIMATION_NAME_LIMIT];

    Unsigned32 blockVersion;
    Unsigned32 durationTicks;
    Unsigned32 targetCount;
    /* Every target's channels, flattened. A target is a bone or a morph, and
       which target a channel came from does not change what it drives — the
       channel's own name does. */
    const AnimationChannel *channels;
    Unsigned32 channelCount;
    /* Counted and stepped over rather than read. An animation carrying them is
       doing something this cannot reproduce, and that should be visible. */
    Unsigned32 chainCount;
} Animation;

AnimationReadResult animationReaderOpen(Animation *animation, const Unsigned8 *bytes,
                                        MemorySize sizeInBytes, MemoryArena *arena);

/* A component's value at a tick, interpolated linearly between the two
 * keyframes either side of it.
 *
 * Linear, not along the curve's tangents. The tangents are in the file and are
 * read, but a straight line between keyframes is right at every keyframe and
 * close between them, whereas a wrong curve is wrong everywhere. Sampling on
 * the tangents is worth doing and is not done here.
 *
 * A component with no keyframes yields zero. */
Real32 animationComponentSample(const AnimationComponent *component, Real32 tick);

/* Measures what unit the keyframe tangents are in.
 *
 * A tangent is a slope, so the value it accounts for across an interval is
 * slope times that interval's length. For a curve that means anything, that
 * quantity is the same order as the interval's actual change in value — a
 * slope which claims a bone rotates a thousand degrees across a gap it
 * actually rotates two over is not a shape, it is a unit error.
 *
 * So this totals both over every interval of every channel and hands back the
 * ratio. Near one and the tangents are per tick, which is what the span is
 * measured in. Near eight hundred and they are per second, because that is how
 * many ticks a second holds — and the curve then needs the span converted
 * before it is applied.
 *
 * Totals rather than a worst case: a single interval with a near-zero change
 * gives an enormous ratio however right the unit is, and one outlier deciding
 * this is how it went wrong the first time.
 *
 * Writes the number of intervals it managed to compare. Nought means every
 * tangent was flat and the question cannot be answered from this animation. */
void animationMeasureTangentScale(const Animation *animation, Real32 *slopeToChange,
                                  Unsigned32 *intervalsCompared);

/* The channel named, or null when the animation drives no bone of that name.
   Compared without regard to case, as the format's own names are not consistent
   about it. */
const AnimationChannel *animationFindChannel(const Animation *animation, const char *name);

#endif
