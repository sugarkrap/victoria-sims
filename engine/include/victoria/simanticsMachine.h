#ifndef VICTORIA_SIMANTICS_MACHINE_HEADER
#define VICTORIA_SIMANTICS_MACHINE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/simanticsTree.h"

#define SIMANTICS_GLOBAL_GROUP 0x7FD46CD0UL

#define SIMANTICS_TEMP_COUNT 20U
#define SIMANTICS_GLOBAL_COUNT 64U
#define SIMANTICS_ATTRIBUTE_COUNT 64U
#define SIMANTICS_OBJECT_DATA_COUNT 256U
#define SIMANTICS_PERSON_DATA_COUNT 256U
#define SIMANTICS_MOTIVE_COUNT 16U
#define SIMANTICS_SEMIGLOBAL_ATTRIBUTE_COUNT 64U
#define SIMANTICS_STACK_DEPTH 64U
#define SIMANTICS_FRAME_VALUE_LIMIT 32U
#define SIMANTICS_ITERATION_LIMIT 100000U

typedef enum SimanticsOwner
{
    SIMANTICS_OWNER_MY_ATTRIBUTE = 0x00,
    SIMANTICS_OWNER_STACK_OBJECT_ATTRIBUTE = 0x01,
    SIMANTICS_OWNER_MY_SEMIGLOBAL_ATTRIBUTE = 0x02,
    SIMANTICS_OWNER_MY_DATA = 0x03,
    SIMANTICS_OWNER_STACK_OBJECT_DATA = 0x04,
    SIMANTICS_OWNER_STACK_OBJECT_SEMIGLOBAL_ATTRIBUTE = 0x05,
    SIMANTICS_OWNER_GLOBAL = 0x06,
    SIMANTICS_OWNER_LITERAL = 0x07,
    SIMANTICS_OWNER_TEMP = 0x08,
    SIMANTICS_OWNER_PARAMETER = 0x09,
    SIMANTICS_OWNER_STACK_OBJECT_IDENTIFIER = 0x0A,
    SIMANTICS_OWNER_TEMP_BY_TEMP = 0x0B,
    SIMANTICS_OWNER_MY_MOTIVE = 0x0E,
    SIMANTICS_OWNER_STACK_OBJECT_MOTIVE = 0x0F,
    SIMANTICS_OWNER_STACK_OBJECT_MOTIVE_BY_TEMP = 0x11,
    SIMANTICS_OWNER_MY_PERSON_DATA = 0x12,
    SIMANTICS_OWNER_STACK_OBJECT_PERSON_DATA = 0x13,
    SIMANTICS_OWNER_STACK_OBJECT_ATTRIBUTE_BY_PARAMETER = 0x16,
    SIMANTICS_OWNER_LOCAL = 0x19,
    SIMANTICS_OWNER_CONSTANT = 0x1A,
    SIMANTICS_OWNER_MY_PERSON_DATA_BY_TEMP = 0x1E,
    SIMANTICS_OWNER_STACK_OBJECT_PERSON_DATA_BY_TEMP = 0x1F,
    SIMANTICS_OWNER_CONSTANT_BY_TEMP = 0x2F,
    SIMANTICS_OWNER_STACK_OBJECT_SEMIGLOBAL_ATTRIBUTE_BY_PARAMETER = 0x32
} SimanticsOwner;

typedef enum SimanticsOperator
{
    SIMANTICS_OPERATOR_GREATER = 0x00,
    SIMANTICS_OPERATOR_LESS = 0x01,
    SIMANTICS_OPERATOR_EQUAL = 0x02,
    SIMANTICS_OPERATOR_ADD = 0x03,
    SIMANTICS_OPERATOR_SUBTRACT = 0x04,
    SIMANTICS_OPERATOR_ASSIGN = 0x05,
    SIMANTICS_OPERATOR_MULTIPLY = 0x06,
    SIMANTICS_OPERATOR_DIVIDE = 0x07,
    SIMANTICS_OPERATOR_FLAG_IS_SET = 0x08,
    SIMANTICS_OPERATOR_SET_FLAG = 0x09,
    SIMANTICS_OPERATOR_CLEAR_FLAG = 0x0A,
    SIMANTICS_OPERATOR_INCREMENT_AND_LESS = 0x0B,
    SIMANTICS_OPERATOR_MODULO = 0x0C,
    SIMANTICS_OPERATOR_AND = 0x0D,
    SIMANTICS_OPERATOR_GREATER_OR_EQUAL = 0x0E,
    SIMANTICS_OPERATOR_LESS_OR_EQUAL = 0x0F,
    SIMANTICS_OPERATOR_NOT_EQUAL = 0x10,
    SIMANTICS_OPERATOR_DECREMENT_AND_GREATER = 0x11,
    SIMANTICS_OPERATOR_OR = 0x12,
    SIMANTICS_OPERATOR_EXCLUSIVE_OR = 0x13,
    SIMANTICS_OPERATOR_ABSOLUTE = 0x14,
    SIMANTICS_OPERATOR_ASSIGN_WIDE = 0x15
} SimanticsOperator;

typedef enum SimanticsPrimitive
{
    SIMANTICS_PRIMITIVE_SLEEP = 0x00,
    SIMANTICS_PRIMITIVE_EXPRESSION = 0x02,
    SIMANTICS_PRIMITIVE_RANDOM_NUMBER = 0x08,
    SIMANTICS_PRIMITIVE_BREAK_POINT = 0x0F,
    SIMANTICS_PRIMITIVE_SET_TO_NEXT = 0x1F
} SimanticsPrimitive;

typedef enum SimanticsSearch
{
    SIMANTICS_SEARCH_OBJECT = 0x00,
    SIMANTICS_SEARCH_PERSON = 0x01,
    SIMANTICS_SEARCH_NON_PERSON = 0x02,
    SIMANTICS_SEARCH_OBJECT_OF_TYPE = 0x04,
    SIMANTICS_SEARCH_VERIFY_STACK_OBJECT = 0x1A,
    SIMANTICS_SEARCH_VERIFY_SIM_IN_STACK_OBJECT = 0x1E,
    SIMANTICS_SEARCH_SIM = 0x28
} SimanticsSearch;

typedef enum SimanticsFailure
{
    SIMANTICS_FAILURE_NONE = 0,
    SIMANTICS_FAILURE_ERROR_TARGET,
    SIMANTICS_FAILURE_BAD_TARGET,
    SIMANTICS_FAILURE_UNSUPPORTED_PRIMITIVE,
    SIMANTICS_FAILURE_UNSUPPORTED_OWNER,
    SIMANTICS_FAILURE_UNSUPPORTED_OPERATOR,
    SIMANTICS_FAILURE_UNSUPPORTED_CALL_FORMAT,
    SIMANTICS_FAILURE_UNSUPPORTED_SEARCH,
    SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE,
    SIMANTICS_FAILURE_READ_ONLY_OWNER,
    SIMANTICS_FAILURE_NO_SUCH_OBJECT,
    SIMANTICS_FAILURE_NO_SUCH_TREE,
    SIMANTICS_FAILURE_NO_SUCH_CONSTANT,
    SIMANTICS_FAILURE_STACK_OVERFLOW,
    SIMANTICS_FAILURE_TOO_MANY_ITERATIONS,
    SIMANTICS_FAILURE_BREAK_POINT,
    SIMANTICS_FAILURE_COUNT
} SimanticsFailure;

const char *simanticsFailureGetName(SimanticsFailure failure);

typedef struct SimanticsHost
{
    void *context;
    const SimanticsTree *(*findTree)(void *context, Unsigned32 groupIdentifier, Unsigned16 instance);
    Boolean (*findConstant)(void *context, Unsigned32 groupIdentifier, Unsigned16 instance, Unsigned16 index,
                            Integer16 *value);
} SimanticsHost;

typedef struct SimanticsObject
{
    Integer16 identifier;
    Unsigned32 privateGroup;
    Unsigned32 semiGlobalGroup;
    Unsigned32 guid;
    Boolean isPerson;
    Integer16 attributes[SIMANTICS_ATTRIBUTE_COUNT];
    Integer16 semiGlobalAttributes[SIMANTICS_SEMIGLOBAL_ATTRIBUTE_COUNT];
    Integer16 data[SIMANTICS_OBJECT_DATA_COUNT];
    Integer16 personData[SIMANTICS_PERSON_DATA_COUNT];
    Integer16 motives[SIMANTICS_MOTIVE_COUNT];
} SimanticsObject;

typedef struct SimanticsFrame
{
    const SimanticsTree *tree;
    Unsigned16 node;
    Integer16 parameters[SIMANTICS_FRAME_VALUE_LIMIT];
    Integer16 locals[SIMANTICS_FRAME_VALUE_LIMIT];
} SimanticsFrame;

typedef enum SimanticsThreadStatus
{
    SIMANTICS_THREAD_IDLE = 0,
    SIMANTICS_THREAD_RUNNING,
    SIMANTICS_THREAD_SLEEPING,
    SIMANTICS_THREAD_RETURNED_TRUE,
    SIMANTICS_THREAD_RETURNED_FALSE,
    SIMANTICS_THREAD_FAILED
} SimanticsThreadStatus;

typedef struct SimanticsThread
{
    Integer16 objectIdentifier;
    Integer16 stackObjectIdentifier;
    Integer16 temps[SIMANTICS_TEMP_COUNT];
    SimanticsFrame frames[SIMANTICS_STACK_DEPTH];
    Unsigned32 depth;
    Unsigned32 sleepTicks;
    SimanticsThreadStatus status;
    SimanticsFailure failure;
    Unsigned32 failureDetail;
    const SimanticsTree *failureTree;
    Unsigned16 failureNode;
} SimanticsThread;

typedef struct SimanticsMachine SimanticsMachine;

typedef void (*SimanticsTrace)(void *context, const SimanticsThread *thread, const SimanticsInstruction *instruction,
                               Unsigned16 target);

struct SimanticsMachine
{
    SimanticsHost host;
    SimanticsObject *objects;
    Unsigned32 objectCount;
    Unsigned32 objectCapacity;
    Integer16 globals[SIMANTICS_GLOBAL_COUNT];
    Unsigned32 randomState;
    SimanticsTrace trace;
    void *traceContext;
};

Boolean simanticsMachineBegin(SimanticsMachine *machine, const SimanticsHost *host, MemoryArena *arena,
                              Unsigned32 objectCapacity, Unsigned32 randomSeed);

SimanticsObject *simanticsMachineAddObject(SimanticsMachine *machine, Unsigned32 privateGroup,
                                           Unsigned32 semiGlobalGroup);

SimanticsObject *simanticsMachineFindObject(SimanticsMachine *machine, Integer16 identifier);

Boolean simanticsThreadStart(SimanticsMachine *machine, SimanticsThread *thread, Integer16 objectIdentifier,
                             const SimanticsTree *tree, const Integer16 *arguments, Unsigned32 argumentCount);

SimanticsThreadStatus simanticsThreadTick(SimanticsMachine *machine, SimanticsThread *thread);

#endif
