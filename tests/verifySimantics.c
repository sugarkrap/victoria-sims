#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/memoryArena.h"
#include "victoria/simanticsMachine.h"
#include "victoria/simanticsTree.h"

#define ARENA_CAPACITY (1024UL * 1024UL)
#define TREE_BYTES 4096UL
#define NODE_LIMIT 32U
#define HOST_TREE_LIMIT 16U
#define HOST_CONSTANT_LIMIT 16U
#define TRACE_LIMIT 64U

#define PRIVATE_GROUP 0x7F000001UL
#define SEMIGLOBAL_GROUP 0x7F000002UL

#define TRUE_EXIT SIMANTICS_TARGET_TRUE
#define FALSE_EXIT SIMANTICS_TARGET_FALSE
#define ERROR_EXIT SIMANTICS_TARGET_ERROR

static Unsigned8 arenaStorage[ARENA_CAPACITY];
static MemoryArena arena;
static Integer32 failureCount = 0;

typedef struct Node
{
    Unsigned16 opcode;
    Unsigned16 trueTarget;
    Unsigned16 falseTarget;
    Unsigned8 nodeVersion;
    Unsigned8 operands[SIMANTICS_OPERAND_COUNT];
} Node;

typedef struct Builder
{
    Unsigned16 format;
    Unsigned8 argumentCount;
    Unsigned8 localCount;
    Node nodes[NODE_LIMIT];
    Unsigned32 count;
} Builder;

typedef struct HostTree
{
    Unsigned32 group;
    Unsigned16 instance;
    SimanticsTree tree;
} HostTree;

typedef struct HostConstant
{
    Unsigned32 group;
    Unsigned16 instance;
    Unsigned16 index;
    Integer16 value;
} HostConstant;

typedef struct TestHost
{
    HostTree trees[HOST_TREE_LIMIT];
    Unsigned32 treeCount;
    HostConstant constants[HOST_CONSTANT_LIMIT];
    Unsigned32 constantCount;
} TestHost;

typedef struct TraceLog
{
    Unsigned16 opcodes[TRACE_LIMIT];
    Unsigned16 targets[TRACE_LIMIT];
    Unsigned32 count;
} TraceLog;

static TestHost host;
static TraceLog traceLog;

static const SimanticsTree *testFindTree(void *context, Unsigned32 group, Unsigned16 instance)
{
    TestHost *testHost = (TestHost *)context;
    Unsigned32 which;

    for (which = 0U; which < testHost->treeCount; which++)
    {
        if (testHost->trees[which].group == group && testHost->trees[which].instance == instance)
        {
            return &testHost->trees[which].tree;
        }
    }
    return NULL_POINTER;
}

static Boolean testFindConstant(void *context, Unsigned32 group, Unsigned16 instance, Unsigned16 index,
                                Integer16 *value)
{
    TestHost *testHost = (TestHost *)context;
    Unsigned32 which;

    for (which = 0U; which < testHost->constantCount; which++)
    {
        const HostConstant *constant = &testHost->constants[which];

        if (constant->group == group && constant->instance == instance && constant->index == index)
        {
            *value = constant->value;
            return BOOLEAN_TRUE;
        }
    }
    return BOOLEAN_FALSE;
}

static void recordTrace(void *context, const SimanticsThread *thread, const SimanticsInstruction *instruction,
                        Unsigned16 target)
{
    TraceLog *log = (TraceLog *)context;

    (void)thread;
    if (log->count < TRACE_LIMIT)
    {
        log->opcodes[log->count] = instruction->opcode;
        log->targets[log->count] = target;
        log->count++;
    }
}

static void begin(Builder *builder, Unsigned8 argumentCount, Unsigned8 localCount)
{
    builder->format = 0x8007U;
    builder->argumentCount = argumentCount;
    builder->localCount = localCount;
    builder->count = 0U;
}

static Node *add(Builder *builder, Unsigned16 opcode, Unsigned16 trueTarget, Unsigned16 falseTarget)
{
    Node *node = &builder->nodes[builder->count];
    Unsigned32 which;

    builder->count++;
    node->opcode = opcode;
    node->trueTarget = trueTarget;
    node->falseTarget = falseTarget;
    node->nodeVersion = 0U;
    for (which = 0U; which < SIMANTICS_OPERAND_COUNT; which++)
    {
        node->operands[which] = 0U;
    }
    return node;
}

static void putWord(Unsigned8 *at, Unsigned16 value)
{
    at[0] = (Unsigned8)(value & 0xFFU);
    at[1] = (Unsigned8)(value >> 8);
}

static void expression(Builder *builder, Unsigned8 leftOwner, Unsigned16 leftData, Unsigned8 operator,
                       Unsigned8 rightOwner, Unsigned16 rightData, Unsigned16 trueTarget, Unsigned16 falseTarget)
{
    Node *node = add(builder, SIMANTICS_PRIMITIVE_EXPRESSION, trueTarget, falseTarget);

    putWord(node->operands, leftData);
    putWord(node->operands + 2, rightData);
    node->operands[5] = operator;
    node->operands[6] = leftOwner;
    node->operands[7] = rightOwner;
}

static MemorySize serialize(const Builder *builder, Unsigned8 *bytes)
{
    MemorySize at = 76UL;
    MemorySize size = simanticsTreeGetInstructionSize(builder->format);
    Unsigned32 node;
    MemorySize which;

    for (which = 0UL; which < at; which++)
    {
        bytes[which] = 0U;
    }
    bytes[0] = 't';
    bytes[1] = 'e';
    bytes[2] = 's';
    bytes[3] = 't';
    putWord(bytes + 64, builder->format);
    putWord(bytes + 66, (Unsigned16)builder->count);
    bytes[69] = builder->argumentCount;
    bytes[70] = builder->localCount;
    if (builder->format >= 0x8009U)
    {
        bytes[at++] = 0U;
    }
    for (node = 0U; node < builder->count; node++, at += size)
    {
        const Node *source = &builder->nodes[node];
        MemorySize cursor = at + 2UL;

        putWord(bytes + at, source->opcode);
        if (builder->format < 0x8007U)
        {
            bytes[cursor++] = (Unsigned8)((source->trueTarget >= ERROR_EXIT) ? source->trueTarget - 0xFEFFU
                                                                             : source->trueTarget);
            bytes[cursor++] = (Unsigned8)((source->falseTarget >= ERROR_EXIT) ? source->falseTarget - 0xFEFFU
                                                                              : source->falseTarget);
        }
        else
        {
            putWord(bytes + cursor, source->trueTarget);
            putWord(bytes + cursor + 2UL, source->falseTarget);
            cursor += 4UL;
        }
        if (builder->format >= 0x8005U)
        {
            bytes[cursor++] = source->nodeVersion;
        }
        for (which = 0UL; which < size - (cursor - at); which++)
        {
            bytes[cursor + which] = source->operands[which];
        }
    }
    return at;
}

static Boolean build(const Builder *builder, SimanticsTree *tree)
{
    static Unsigned8 bytes[TREE_BYTES];
    MemorySize size = serialize(builder, bytes);

    return (simanticsTreeRead(tree, bytes, size, &arena) == SIMANTICS_TREE_OK) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static SimanticsTree *install(const Builder *builder, Unsigned32 group, Unsigned16 instance)
{
    HostTree *slot = &host.trees[host.treeCount];

    host.treeCount++;
    slot->group = group;
    slot->instance = instance;
    if (!build(builder, &slot->tree))
    {
        printf("FAIL  a test tree would not build\n");
        failureCount++;
    }
    return &slot->tree;
}

static void addConstant(Unsigned32 group, Unsigned16 instance, Unsigned16 index, Integer16 value)
{
    HostConstant *constant = &host.constants[host.constantCount];

    host.constantCount++;
    constant->group = group;
    constant->instance = instance;
    constant->index = index;
    constant->value = value;
}

static SimanticsMachine machine;
static SimanticsThread thread;
static Integer16 me;
static Integer16 other;

static void resetWorld(void)
{
    SimanticsHost description;

    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
    host.treeCount = 0U;
    host.constantCount = 0U;
    traceLog.count = 0U;
    description.context = &host;
    description.findTree = testFindTree;
    description.findConstant = testFindConstant;
    (void)simanticsMachineBegin(&machine, &description, &arena, 8U, 1U);
    machine.trace = recordTrace;
    machine.traceContext = &traceLog;
    me = simanticsMachineAddObject(&machine, PRIVATE_GROUP, SEMIGLOBAL_GROUP)->identifier;
    other = simanticsMachineAddObject(&machine, PRIVATE_GROUP, SEMIGLOBAL_GROUP)->identifier;
}

static SimanticsThreadStatus runTree(const Builder *builder, const Integer16 *arguments, Unsigned32 argumentCount)
{
    static SimanticsTree tree;
    Unsigned32 ticks = 0U;
    SimanticsThreadStatus status;

    if (!build(builder, &tree) || !simanticsThreadStart(&machine, &thread, me, &tree, arguments, argumentCount))
    {
        printf("FAIL  the tree would not start\n");
        failureCount++;
        return SIMANTICS_THREAD_FAILED;
    }
    do
    {
        status = simanticsThreadTick(&machine, &thread);
        ticks++;
    } while ((status == SIMANTICS_THREAD_SLEEPING || status == SIMANTICS_THREAD_RUNNING) && ticks < 1000U);
    return status;
}

static Integer16 evaluate(Unsigned8 operator, Integer16 left, Unsigned8 rightOwner, Integer16 right,
                          SimanticsThreadStatus *status)
{
    Builder builder;

    resetWorld();
    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL,
               (Unsigned16)left, 1U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, operator, rightOwner, (Unsigned16)right, TRUE_EXIT, FALSE_EXIT);
    *status = runTree(&builder, NULL_POINTER, 0U);
    return thread.temps[0];
}

static void checkOperators(void)
{
    SimanticsThreadStatus status;
    Integer16 value;

    printf("-- expression operators --\n");

    value = evaluate(SIMANTICS_OPERATOR_GREATER, 5, SIMANTICS_OWNER_LITERAL, 3, &status);
    checkThat(&failureCount, "5 > 3 takes the true branch", status == SIMANTICS_THREAD_RETURNED_TRUE);
    value = evaluate(SIMANTICS_OPERATOR_LESS, 5, SIMANTICS_OWNER_LITERAL, 3, &status);
    checkThat(&failureCount, "5 < 3 takes the false branch", status == SIMANTICS_THREAD_RETURNED_FALSE);
    value = evaluate(SIMANTICS_OPERATOR_EQUAL, -2, SIMANTICS_OWNER_LITERAL, (Integer16)0xFFFE, &status);
    checkThat(&failureCount, "literals are signed: -2 == 0xFFFE", status == SIMANTICS_THREAD_RETURNED_TRUE);
    value = evaluate(SIMANTICS_OPERATOR_GREATER_OR_EQUAL, 3, SIMANTICS_OWNER_LITERAL, 3, &status);
    checkThat(&failureCount, "3 >= 3", status == SIMANTICS_THREAD_RETURNED_TRUE);
    value = evaluate(SIMANTICS_OPERATOR_LESS_OR_EQUAL, 4, SIMANTICS_OWNER_LITERAL, 3, &status);
    checkThat(&failureCount, "not 4 <= 3", status == SIMANTICS_THREAD_RETURNED_FALSE);
    value = evaluate(SIMANTICS_OPERATOR_NOT_EQUAL, 4, SIMANTICS_OWNER_LITERAL, 3, &status);
    checkThat(&failureCount, "4 != 3", status == SIMANTICS_THREAD_RETURNED_TRUE);
    value = evaluate(SIMANTICS_OPERATOR_GREATER, -1, SIMANTICS_OWNER_LITERAL, 1, &status);
    checkThat(&failureCount, "comparisons are signed: not -1 > 1", status == SIMANTICS_THREAD_RETURNED_FALSE);

    value = evaluate(SIMANTICS_OPERATOR_ADD, 5, SIMANTICS_OWNER_LITERAL, 3, &status);
    checkThat(&failureCount, "5 += 3 is 8, and true", value == 8 && status == SIMANTICS_THREAD_RETURNED_TRUE);
    value = evaluate(SIMANTICS_OPERATOR_SUBTRACT, 5, SIMANTICS_OWNER_LITERAL, 8, &status);
    checkThat(&failureCount, "5 -= 8 is -3", value == -3);
    value = evaluate(SIMANTICS_OPERATOR_ASSIGN, 5, SIMANTICS_OWNER_LITERAL, 9, &status);
    checkThat(&failureCount, ":= 9 is 9", value == 9);
    value = evaluate(SIMANTICS_OPERATOR_MULTIPLY, -6, SIMANTICS_OWNER_LITERAL, 7, &status);
    checkThat(&failureCount, "-6 *= 7 is -42", value == -42);
    value = evaluate(SIMANTICS_OPERATOR_ADD, 32767, SIMANTICS_OWNER_LITERAL, 1, &status);
    checkThat(&failureCount, "32767 += 1 wraps to -32768", value == -32768);
    value = evaluate(SIMANTICS_OPERATOR_DIVIDE, -7, SIMANTICS_OWNER_LITERAL, 2, &status);
    checkThat(&failureCount, "-7 /= 2 truncates to -3", value == -3);
    value = evaluate(SIMANTICS_OPERATOR_DIVIDE, 7, SIMANTICS_OWNER_LITERAL, 0, &status);
    checkThat(&failureCount, "dividing by zero is false and leaves the value",
              value == 7 && status == SIMANTICS_THREAD_RETURNED_FALSE);
    value = evaluate(SIMANTICS_OPERATOR_MODULO, 7, SIMANTICS_OWNER_LITERAL, 3, &status);
    checkThat(&failureCount, "7 mod= 3 is 1", value == 1);
    value = evaluate(SIMANTICS_OPERATOR_MODULO, 7, SIMANTICS_OWNER_LITERAL, 0, &status);
    checkThat(&failureCount, "modulo by zero is false", status == SIMANTICS_THREAD_RETURNED_FALSE);
    value = evaluate(SIMANTICS_OPERATOR_AND, 0x0C, SIMANTICS_OWNER_LITERAL, 0x0A, &status);
    checkThat(&failureCount, "0x0C and= 0x0A is 0x08", value == 0x08);
    value = evaluate(SIMANTICS_OPERATOR_OR, 0x0C, SIMANTICS_OWNER_LITERAL, 0x0A, &status);
    checkThat(&failureCount, "0x0C or= 0x0A is 0x0E", value == 0x0E);
    value = evaluate(SIMANTICS_OPERATOR_EXCLUSIVE_OR, 0x0C, SIMANTICS_OWNER_LITERAL, 0x0A, &status);
    checkThat(&failureCount, "0x0C xor= 0x0A is 0x06", value == 0x06);
    value = evaluate(SIMANTICS_OPERATOR_ABSOLUTE, 1, SIMANTICS_OWNER_LITERAL, -12, &status);
    checkThat(&failureCount, ":= abs(-12) is 12", value == 12);

    value = evaluate(SIMANTICS_OPERATOR_SET_FLAG, 0, SIMANTICS_OWNER_LITERAL, 1, &status);
    checkThat(&failureCount, "setting flag 1 sets the lowest bit", value == 0x0001);
    value = evaluate(SIMANTICS_OPERATOR_SET_FLAG, 0, SIMANTICS_OWNER_LITERAL, 16, &status);
    checkThat(&failureCount, "setting flag 16 sets the sign bit", value == (Integer16)0x8000);
    value = evaluate(SIMANTICS_OPERATOR_CLEAR_FLAG, 0x0F, SIMANTICS_OWNER_LITERAL, 3, &status);
    checkThat(&failureCount, "clearing flag 3 clears bit 2", value == 0x0B);
    value = evaluate(SIMANTICS_OPERATOR_FLAG_IS_SET, 0x04, SIMANTICS_OWNER_LITERAL, 3, &status);
    checkThat(&failureCount, "flag 3 of 0x04 is set", status == SIMANTICS_THREAD_RETURNED_TRUE);
    value = evaluate(SIMANTICS_OPERATOR_FLAG_IS_SET, 0x04, SIMANTICS_OWNER_LITERAL, 2, &status);
    checkThat(&failureCount, "flag 2 of 0x04 is not", status == SIMANTICS_THREAD_RETURNED_FALSE);
    value = evaluate(SIMANTICS_OPERATOR_SET_FLAG, 0, SIMANTICS_OWNER_LITERAL, 0, &status);
    checkThat(&failureCount, "flag 0 does not exist",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE);

    value = evaluate(SIMANTICS_OPERATOR_INCREMENT_AND_LESS, 4, SIMANTICS_OWNER_LITERAL, 6, &status);
    checkThat(&failureCount, "++ and < : 4 becomes 5, which is < 6",
              value == 5 && status == SIMANTICS_THREAD_RETURNED_TRUE);
    value = evaluate(SIMANTICS_OPERATOR_INCREMENT_AND_LESS, 5, SIMANTICS_OWNER_LITERAL, 6, &status);
    checkThat(&failureCount, "++ and < : 5 becomes 6, which is not < 6",
              value == 6 && status == SIMANTICS_THREAD_RETURNED_FALSE);
    value = evaluate(SIMANTICS_OPERATOR_DECREMENT_AND_GREATER, 2, SIMANTICS_OWNER_LITERAL, 0, &status);
    checkThat(&failureCount, "-- and > : 2 becomes 1, which is > 0",
              value == 1 && status == SIMANTICS_THREAD_RETURNED_TRUE);

    value = evaluate(0x16U, 0, SIMANTICS_OWNER_LITERAL, 0, &status);
    checkThat(&failureCount, "an operator past the known 22 fails, and says so",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_UNSUPPORTED_OPERATOR);
    (void)value;
}

static void checkOwners(void)
{
    Builder builder;
    SimanticsThreadStatus status;
    SimanticsObject *mine;
    SimanticsObject *theirs;
    Integer16 arguments[2] = { 11, 22 };

    printf("\n-- data owners --\n");

    resetWorld();
    mine = simanticsMachineFindObject(&machine, me);
    theirs = simanticsMachineFindObject(&machine, other);
    mine->attributes[3] = 30;
    theirs->attributes[4] = 40;
    theirs->data[0x10] = 160;
    begin(&builder, 2U, 2U);
    expression(&builder, SIMANTICS_OWNER_LOCAL, 1U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_PARAMETER, 1U, 1U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LOCAL, 1U, 2U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 1U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 5U, 3U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP_BY_TEMP, 1U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_PARAMETER, 0U,
               4U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_STACK_OBJECT_IDENTIFIER, 0U, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_LITERAL, (Unsigned16)other, 5U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 2U, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_STACK_OBJECT_ATTRIBUTE, 4U, 6U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 3U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_MY_ATTRIBUTE, 3U, 7U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 4U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_STACK_OBJECT_DATA,
               0x10U, 8U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_MY_DATA, 0x20U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 77U,
               9U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_GLOBAL, 7U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 99U, 10U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 6U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_STACK_OBJECT_DATA,
               0x0BU, 11U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 7U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 4U, 12U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_STACK_OBJECT_ATTRIBUTE_BY_PARAMETER, 1U, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_LITERAL, 123U, TRUE_EXIT, ERROR_EXIT);
    arguments[1] = 4;
    status = runTree(&builder, arguments, 2U);

    checkThat(&failureCount, "the walk through every owner finishes", status == SIMANTICS_THREAD_RETURNED_TRUE);
    checkThat(&failureCount, "a parameter copies into a local and on into a temp", thread.temps[0] == 4);
    checkThat(&failureCount, "temp by temp writes the temp another temp names", thread.temps[5] == 11);
    checkThat(&failureCount, "the stack object is set by its identifier", thread.stackObjectIdentifier == other);
    checkThat(&failureCount, "the stack object's attribute is read", thread.temps[2] == 40);
    checkThat(&failureCount, "my own attribute is read", thread.temps[3] == 30);
    checkThat(&failureCount, "the stack object's data is read", thread.temps[4] == 160);
    checkThat(&failureCount, "my own data is written", mine->data[0x20] == 77);
    checkThat(&failureCount, "a global is written", machine.globals[7] == 99);
    checkThat(&failureCount, "object data 0x0B is the object's identifier", thread.temps[6] == other);
    checkThat(&failureCount, "an attribute is addressed through a parameter", theirs->attributes[4] == 123);

    resetWorld();
    addConstant(PRIVATE_GROUP, 0x1005U, 3U, 301);
    addConstant(SEMIGLOBAL_GROUP, 0x2045U, 2U, 202);
    addConstant(SIMANTICS_GLOBAL_GROUP, 0x0101U, 1U, 101);
    addConstant(PRIVATE_GROUP, 0x1007U, 6U, 706);
    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_CONSTANT,
               (Unsigned16)((5U << 7) | 3U), 1U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 1U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_CONSTANT,
               (Unsigned16)((1U << 13) | (1U << 15) | (5U << 7) | 2U), 2U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 2U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_CONSTANT,
               (Unsigned16)((2U << 13) | (1U << 7) | 1U), 3U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 5U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 6U, 4U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 3U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_CONSTANT_BY_TEMP,
               (Unsigned16)((7U << 3) | 5U), 5U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 4U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_CONSTANT,
               (Unsigned16)((9U << 7) | 1U), TRUE_EXIT, ERROR_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "a private constant is read from table 0x1005", thread.temps[0] == 301);
    checkThat(&failureCount, "a semi-global constant, its table's high bit from bit 15", thread.temps[1] == 202);
    checkThat(&failureCount, "a global constant is read from the global group", thread.temps[2] == 101);
    checkThat(&failureCount, "a constant indexed by a temp", thread.temps[3] == 706);
    checkThat(&failureCount, "a constant nobody loaded fails, and says so",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_NO_SUCH_CONSTANT);

    resetWorld();
    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_LITERAL, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 1U,
               TRUE_EXIT, ERROR_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "a literal cannot be written",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_READ_ONLY_OWNER);

    resetWorld();
    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, 0x0DU, 0U, TRUE_EXIT, ERROR_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "an owner not built yet fails, naming the owner",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_UNSUPPORTED_OWNER &&
                  thread.failureDetail == 0x0DU);

    resetWorld();
    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_STACK_OBJECT_DATA,
               0U, TRUE_EXIT, ERROR_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "reading through an empty stack object fails",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_NO_SUCH_OBJECT);

    resetWorld();
    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, SIMANTICS_TEMP_COUNT, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_LITERAL, 1U, TRUE_EXIT, ERROR_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "a temp past the last fails",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE);
}

static void checkControlFlow(void)
{
    Builder builder;
    SimanticsThreadStatus status;

    printf("\n-- control flow --\n");

    resetWorld();
    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 1U, SIMANTICS_OPERATOR_ADD, SIMANTICS_OWNER_LITERAL, 3U, 1U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_INCREMENT_AND_LESS, SIMANTICS_OWNER_LITERAL,
               10U, 0U, TRUE_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "a loop runs ten times and leaves through its false branch",
              status == SIMANTICS_THREAD_RETURNED_TRUE && thread.temps[0] == 10 && thread.temps[1] == 30);
    checkThat(&failureCount, "every step is traced", traceLog.count == 20U);
    checkThat(&failureCount, "the last trace is the exit",
              traceLog.count > 0U && traceLog.targets[traceLog.count - 1U] == TRUE_EXIT);

    resetWorld();
    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_EQUAL, SIMANTICS_OWNER_LITERAL, 1U, TRUE_EXIT,
               ERROR_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "the error exit fails the thread at that node",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_ERROR_TARGET &&
                  thread.failureNode == 0U);

    resetWorld();
    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 1U, 5U,
               ERROR_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "a branch past the end fails",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_BAD_TARGET);

    resetWorld();
    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ADD, SIMANTICS_OWNER_LITERAL, 1U, 0U,
               ERROR_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "an endless loop is stopped",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_TOO_MANY_ITERATIONS);

    resetWorld();
    begin(&builder, 0U, 0U);
    add(&builder, 0x0013U, TRUE_EXIT, FALSE_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "a primitive not built yet fails, naming the opcode",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_UNSUPPORTED_PRIMITIVE &&
                  thread.failureDetail == 0x13U);
}

static void checkCalls(void)
{
    Builder callee;
    Builder caller;
    Node *call;
    SimanticsThreadStatus status;
    Integer16 arguments[2] = { 7, 8 };

    printf("\n-- calls --\n");

    resetWorld();
    begin(&callee, 2U, 1U);
    expression(&callee, SIMANTICS_OWNER_LOCAL, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_PARAMETER, 0U, 1U,
               ERROR_EXIT);
    expression(&callee, SIMANTICS_OWNER_LOCAL, 0U, SIMANTICS_OPERATOR_ADD, SIMANTICS_OWNER_PARAMETER, 1U, 2U,
               ERROR_EXIT);
    expression(&callee, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LOCAL, 0U, 3U,
               ERROR_EXIT);
    expression(&callee, SIMANTICS_OWNER_LOCAL, 0U, SIMANTICS_OPERATOR_GREATER, SIMANTICS_OWNER_LITERAL, 10U,
               TRUE_EXIT, FALSE_EXIT);
    (void)install(&callee, PRIVATE_GROUP, 0x1001U);
    (void)install(&callee, SIMANTICS_GLOBAL_GROUP, 0x0101U);
    (void)install(&callee, SEMIGLOBAL_GROUP, 0x2001U);

    begin(&caller, 2U, 1U);
    call = add(&caller, 0x1001U, 1U, ERROR_EXIT);
    putWord(call->operands, 4U);
    putWord(call->operands + 2, 9U);
    expression(&caller, SIMANTICS_OWNER_TEMP, 9U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_TEMP, 0U, 2U,
               ERROR_EXIT);
    expression(&caller, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 2U, 3U,
               ERROR_EXIT);
    expression(&caller, SIMANTICS_OWNER_TEMP, 1U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 3U, 4U,
               ERROR_EXIT);
    call = add(&caller, 0x0101U, ERROR_EXIT, 5U);
    {
        Unsigned32 which;

        for (which = 0U; which < 8U; which++)
        {
            call->operands[which] = 0xFFU;
        }
    }
    expression(&caller, SIMANTICS_OWNER_TEMP, 2U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_TEMP, 0U, 6U,
               ERROR_EXIT);
    call = add(&caller, 0x2001U, 7U, ERROR_EXIT);
    call->nodeVersion = 1U;
    call->operands[12] = 0x02U;
    expression(&caller, SIMANTICS_OWNER_TEMP, 3U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_TEMP, 0U, 8U,
               ERROR_EXIT);
    expression(&caller, SIMANTICS_OWNER_LOCAL, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 50U, 9U,
               ERROR_EXIT);
    call = add(&caller, 0x1001U, 10U, ERROR_EXIT);
    call->operands[12] = 0x01U;
    call->operands[0] = SIMANTICS_OWNER_LOCAL;
    putWord(call->operands + 1, 0U);
    call->operands[3] = SIMANTICS_OWNER_LITERAL;
    putWord(call->operands + 4, 6U);
    expression(&caller, SIMANTICS_OWNER_TEMP, 4U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_TEMP, 0U, TRUE_EXIT,
               ERROR_EXIT);

    status = runTree(&caller, arguments, 2U);
    checkThat(&failureCount, "the caller finishes", status == SIMANTICS_THREAD_RETURNED_TRUE);
    checkThat(&failureCount, "literal arguments: 4 + 9, and the callee's true comes back", thread.temps[9] == 13);
    checkThat(&failureCount, "temps as arguments to a global tree: 2 + 3, and its false comes back", thread.temps[2] == 5);
    checkThat(&failureCount, "the caller's own parameters to a semi-global tree: 7 + 8",
              thread.temps[3] == 15);
    checkThat(&failureCount, "arguments read through owners: local 50 + literal 6", thread.temps[4] == 56);
    checkThat(&failureCount, "the caller's local is its own, untouched by the callee's",
              thread.frames[0].locals[0] == 50);

    resetWorld();
    begin(&callee, 0U, 0U);
    add(&callee, 0x1001U, TRUE_EXIT, FALSE_EXIT);
    (void)install(&callee, PRIVATE_GROUP, 0x1001U);
    status = runTree(&callee, NULL_POINTER, 0U);
    checkThat(&failureCount, "a tree calling itself forever overflows the stack",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_STACK_OVERFLOW &&
                  thread.depth == SIMANTICS_STACK_DEPTH);

    resetWorld();
    begin(&caller, 0U, 0U);
    add(&caller, 0x1077U, TRUE_EXIT, FALSE_EXIT);
    status = runTree(&caller, NULL_POINTER, 0U);
    checkThat(&failureCount, "a call to a tree nobody loaded fails, naming it",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_NO_SUCH_TREE &&
                  thread.failureDetail == 0x1077U);
}

static void checkOtherPrimitives(void)
{
    Builder builder;
    Node *node;
    SimanticsThreadStatus status;
    SimanticsTree tree;
    Integer16 arguments[1] = { 3 };
    Unsigned32 ticks = 0U;
    Unsigned32 which;
    Boolean inRange = BOOLEAN_TRUE;

    printf("\n-- sleep, random number, break point --\n");

    resetWorld();
    begin(&builder, 1U, 0U);
    node = add(&builder, SIMANTICS_PRIMITIVE_SLEEP, 1U, ERROR_EXIT);
    putWord(node->operands, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 1U, TRUE_EXIT,
               ERROR_EXIT);
    (void)build(&builder, &tree);
    (void)simanticsThreadStart(&machine, &thread, me, &tree, arguments, 1U);
    status = simanticsThreadTick(&machine, &thread);
    checkThat(&failureCount, "sleep yields on the tick it runs", status == SIMANTICS_THREAD_SLEEPING);
    while (status == SIMANTICS_THREAD_SLEEPING && ticks < 10U)
    {
        status = simanticsThreadTick(&machine, &thread);
        ticks++;
    }
    checkThat(&failureCount, "and wakes after the ticks its parameter names", ticks == 3U && thread.temps[0] == 1);

    resetWorld();
    begin(&builder, 0U, 0U);
    for (which = 0U; which < 20U; which++)
    {
        node = add(&builder, SIMANTICS_PRIMITIVE_RANDOM_NUMBER, (Unsigned16)(which + 1U), ERROR_EXIT);
        putWord(node->operands, (Unsigned16)(which % SIMANTICS_TEMP_COUNT));
        node->operands[2] = SIMANTICS_OWNER_TEMP;
        putWord(node->operands + 4, 6U);
        node->operands[6] = SIMANTICS_OWNER_LITERAL;
    }
    builder.nodes[builder.count - 1U].trueTarget = TRUE_EXIT;
    status = runTree(&builder, NULL_POINTER, 0U);
    for (which = 0U; which < 20U; which++)
    {
        if (thread.temps[which] < 0 || thread.temps[which] >= 6)
        {
            inRange = BOOLEAN_FALSE;
        }
    }
    checkThat(&failureCount, "random numbers land in 0 .. < 6", status == SIMANTICS_THREAD_RETURNED_TRUE && inRange);

    resetWorld();
    begin(&builder, 0U, 0U);
    node = add(&builder, SIMANTICS_PRIMITIVE_BREAK_POINT, 1U, ERROR_EXIT);
    node->operands[2] = SIMANTICS_OWNER_LITERAL;
    putWord(node->operands, 1U);
    node->operands[4] = 0x01U;
    node = add(&builder, SIMANTICS_PRIMITIVE_BREAK_POINT, TRUE_EXIT, ERROR_EXIT);
    node->operands[2] = SIMANTICS_OWNER_TEMP;
    putWord(node->operands, 0U);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "a break point marked release-ignored, and one whose condition is zero, pass",
              status == SIMANTICS_THREAD_RETURNED_TRUE);

    resetWorld();
    begin(&builder, 0U, 0U);
    node = add(&builder, SIMANTICS_PRIMITIVE_BREAK_POINT, TRUE_EXIT, ERROR_EXIT);
    node->operands[2] = SIMANTICS_OWNER_LITERAL;
    putWord(node->operands, 1U);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "a break point whose condition holds fails the thread",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_BREAK_POINT);
}

static void checkObjectStores(void)
{
    Builder builder;
    SimanticsThreadStatus status;
    SimanticsObject *mine;
    SimanticsObject *theirs;
    Integer16 arguments[1] = { 9 };

    printf("\n-- person data, motives and semi-global attributes --\n");

    resetWorld();
    mine = simanticsMachineFindObject(&machine, me);
    theirs = simanticsMachineFindObject(&machine, other);
    theirs->personData[0x40] = 64;
    theirs->motives[5] = -50;
    mine->personData[0x22] = 34;
    begin(&builder, 1U, 0U);
    expression(&builder, SIMANTICS_OWNER_STACK_OBJECT_IDENTIFIER, 0U, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_LITERAL, (Unsigned16)other, 1U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_STACK_OBJECT_PERSON_DATA, 0x40U, 2U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 1U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 0x22U, 3U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 2U, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_MY_PERSON_DATA_BY_TEMP, 1U, 4U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 3U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 5U, 5U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_TEMP, 4U, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_STACK_OBJECT_MOTIVE_BY_TEMP, 3U, 6U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_MY_MOTIVE, 2U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 75U, 7U,
               ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_MY_SEMIGLOBAL_ATTRIBUTE, 3U, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_LITERAL, 33U, 8U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_STACK_OBJECT_SEMIGLOBAL_ATTRIBUTE_BY_PARAMETER, 0U,
               SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 44U, 9U, ERROR_EXIT);
    expression(&builder, SIMANTICS_OWNER_STACK_OBJECT_PERSON_DATA_BY_TEMP, 1U, SIMANTICS_OPERATOR_ADD,
               SIMANTICS_OWNER_LITERAL, 1U, TRUE_EXIT, ERROR_EXIT);
    status = runTree(&builder, arguments, 1U);

    checkThat(&failureCount, "the walk finishes", status == SIMANTICS_THREAD_RETURNED_TRUE);
    checkThat(&failureCount, "the stack object's person data is read", thread.temps[0] == 64);
    checkThat(&failureCount, "my person data is read at the index a temp holds", thread.temps[2] == 34);
    checkThat(&failureCount, "the stack object's motive is read at the index a temp holds", thread.temps[4] == -50);
    checkThat(&failureCount, "my motive is written", mine->motives[2] == 75);
    checkThat(&failureCount, "my semi-global attribute is written, apart from my attributes",
              mine->semiGlobalAttributes[3] == 33 && mine->attributes[3] == 0);
    checkThat(&failureCount, "the stack object's semi-global attribute is addressed through a parameter",
              theirs->semiGlobalAttributes[9] == 44);
    checkThat(&failureCount, "the stack object's person data is changed in place", theirs->personData[0x22] == 1);
}

static Node *setToNext(Builder *builder, Unsigned8 search, Unsigned16 trueTarget, Unsigned16 falseTarget)
{
    Node *node = add(builder, SIMANTICS_PRIMITIVE_SET_TO_NEXT, trueTarget, falseTarget);

    node->operands[4] = search;
    return node;
}

static void checkSetToNext(void)
{
    Builder builder;
    Node *node;
    SimanticsThreadStatus status;
    SimanticsObject *chair;
    SimanticsObject *lamp;
    SimanticsObject *guest;

    printf("\n-- set to next --\n");

    resetWorld();
    simanticsMachineFindObject(&machine, me)->isPerson = BOOLEAN_TRUE;
    chair = simanticsMachineFindObject(&machine, other);
    chair->guid = 0xC0FFEE01UL;
    chair->data[0x30] = 7;
    lamp = simanticsMachineAddObject(&machine, PRIVATE_GROUP, SEMIGLOBAL_GROUP);
    lamp->guid = 0xC0FFEE02UL;
    guest = simanticsMachineAddObject(&machine, PRIVATE_GROUP, SEMIGLOBAL_GROUP);
    guest->isPerson = BOOLEAN_TRUE;
    lamp->data[0x30] = 7;

    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ADD, SIMANTICS_OWNER_LITERAL, 1U, 1U,
               ERROR_EXIT);
    (void)setToNext(&builder, SIMANTICS_SEARCH_OBJECT, 0U, TRUE_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "walking every object visits all four, then answers false",
              status == SIMANTICS_THREAD_RETURNED_TRUE && thread.temps[0] == 5 &&
                  thread.stackObjectIdentifier == guest->identifier);

    begin(&builder, 0U, 0U);
    expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ADD, SIMANTICS_OWNER_LITERAL, 1U, 1U,
               ERROR_EXIT);
    (void)setToNext(&builder, SIMANTICS_SEARCH_PERSON, 0U, TRUE_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "walking the people visits two", thread.temps[0] == 3);

    begin(&builder, 0U, 0U);
    (void)setToNext(&builder, SIMANTICS_SEARCH_NON_PERSON, TRUE_EXIT, FALSE_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "the first thing that is not a person is the chair",
              status == SIMANTICS_THREAD_RETURNED_TRUE && thread.stackObjectIdentifier == chair->identifier);

    begin(&builder, 0U, 0U);
    node = setToNext(&builder, SIMANTICS_SEARCH_OBJECT_OF_TYPE, TRUE_EXIT, FALSE_EXIT);
    putWord(node->operands, 0xEE02U);
    putWord(node->operands + 2, 0xC0FFU);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "an object of a type is found by its GUID",
              status == SIMANTICS_THREAD_RETURNED_TRUE && thread.stackObjectIdentifier == lamp->identifier);

    begin(&builder, 0U, 0U);
    node = setToNext(&builder, (Unsigned8)(SIMANTICS_SEARCH_NON_PERSON | 0x80U), 1U, FALSE_EXIT);
    node->operands[5] = SIMANTICS_OWNER_TEMP;
    node->operands[7] = 6U;
    node->nodeVersion = 1U;
    node->operands[8] = 0x02U;
    putWord(node->operands + 9, 0x30U);
    putWord(node->operands + 11, 7U);
    node = setToNext(&builder, (Unsigned8)(SIMANTICS_SEARCH_NON_PERSON | 0x80U), FALSE_EXIT, TRUE_EXIT);
    node->operands[5] = SIMANTICS_OWNER_TEMP;
    node->operands[7] = 6U;
    node->nodeVersion = 1U;
    node->operands[8] = 0x02U;
    putWord(node->operands + 9, 0x30U);
    putWord(node->operands + 11, 7U);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "a search can write into a temp, and filter by object data",
              status == SIMANTICS_THREAD_RETURNED_FALSE && thread.temps[6] == lamp->identifier &&
                  thread.stackObjectIdentifier == 0);

    begin(&builder, 0U, 0U);
    (void)setToNext(&builder, SIMANTICS_SEARCH_VERIFY_SIM_IN_STACK_OBJECT, FALSE_EXIT, 1U);
    expression(&builder, SIMANTICS_OWNER_STACK_OBJECT_IDENTIFIER, 0U, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_LITERAL, (Unsigned16)guest->identifier, 2U, ERROR_EXIT);
    (void)setToNext(&builder, SIMANTICS_SEARCH_VERIFY_SIM_IN_STACK_OBJECT, 3U, FALSE_EXIT);
    expression(&builder, SIMANTICS_OWNER_STACK_OBJECT_IDENTIFIER, 0U, SIMANTICS_OPERATOR_ASSIGN,
               SIMANTICS_OWNER_LITERAL, (Unsigned16)chair->identifier, 4U, ERROR_EXIT);
    (void)setToNext(&builder, SIMANTICS_SEARCH_VERIFY_SIM_IN_STACK_OBJECT, FALSE_EXIT, 5U);
    (void)setToNext(&builder, SIMANTICS_SEARCH_VERIFY_STACK_OBJECT, TRUE_EXIT, FALSE_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "verifying: no stack object, a person, then an object that is no sim",
              status == SIMANTICS_THREAD_RETURNED_TRUE);

    begin(&builder, 0U, 0U);
    (void)setToNext(&builder, 0x0DU, TRUE_EXIT, FALSE_EXIT);
    status = runTree(&builder, NULL_POINTER, 0U);
    checkThat(&failureCount, "a search not built yet fails, naming its type",
              status == SIMANTICS_THREAD_FAILED && thread.failure == SIMANTICS_FAILURE_UNSUPPORTED_SEARCH &&
                  thread.failureDetail == 0x0DU);
}

static void checkFormats(void)
{
    Builder builder;
    SimanticsTree tree;
    static Unsigned8 bytes[TREE_BYTES];
    MemorySize size;
    Unsigned16 formats[4] = { 0x8002U, 0x8004U, 0x8006U, 0x8009U };
    Unsigned32 which;

    printf("\n-- reading every tree format --\n");

    for (which = 0U; which < 4U; which++)
    {
        resetWorld();
        begin(&builder, 0U, 0U);
        builder.format = formats[which];
        expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_LITERAL, 42U, 1U,
                   ERROR_EXIT);
        expression(&builder, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_EQUAL, SIMANTICS_OWNER_LITERAL, 42U,
                   TRUE_EXIT, FALSE_EXIT);
        size = serialize(&builder, bytes);
        checkThat(&failureCount, "the tree reads",
                  simanticsTreeRead(&tree, bytes, size, &arena) == SIMANTICS_TREE_OK);
        checkThat(&failureCount, "its exits come out in the wide encoding",
                  tree.instructions[1].trueTarget == TRUE_EXIT && tree.instructions[1].falseTarget == FALSE_EXIT &&
                      tree.instructions[0].falseTarget == ERROR_EXIT);
        checkThat(&failureCount, "and it runs",
                  simanticsThreadStart(&machine, &thread, me, &tree, NULL_POINTER, 0U) &&
                      simanticsThreadTick(&machine, &thread) == SIMANTICS_THREAD_RETURNED_TRUE &&
                      thread.temps[0] == 42);
    }

    checkThat(&failureCount, "a tree cut short is refused",
              simanticsTreeRead(&tree, bytes, size - 1UL, &arena) == SIMANTICS_TREE_TRUNCATED);
    bytes[64] = 0x0AU;
    bytes[65] = 0x80U;
    checkThat(&failureCount, "an unknown format is refused",
              simanticsTreeRead(&tree, bytes, size, &arena) == SIMANTICS_TREE_UNKNOWN_FORMAT);
    checkThat(&failureCount, "a header alone is too short",
              simanticsTreeRead(&tree, bytes, 40UL, &arena) == SIMANTICS_TREE_TOO_SHORT);
}

int main(void)
{
    checkOperators();
    checkOwners();
    checkControlFlow();
    checkCalls();
    checkOtherPrimitives();
    checkObjectStores();
    checkSetToNext();
    checkFormats();
    return (int)checkSummarize(failureCount, "SimAntics");
}
