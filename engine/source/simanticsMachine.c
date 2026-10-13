#include "victoria/simanticsMachine.h"

#define OBJECT_DATA_IDENTIFIER 0x0BU

#define CALL_ARGUMENT_FLAGS 12U
#define CALL_WIDE_ARGUMENT_COUNT 4U
#define CALL_NARROW_ARGUMENT_COUNT 8U
#define CALL_ARGUMENT_LIMIT 9U

#define CONSTANT_SCOPE_PRIVATE 0U
#define CONSTANT_SCOPE_SEMIGLOBAL 1U
#define CONSTANT_SCOPE_GLOBAL 2U

#define FIRST_FLAG 1
#define LAST_FLAG 16

#define RANDOM_MULTIPLIER 1103515245UL
#define RANDOM_INCREMENT 12345UL

typedef enum StepResult
{
    STEP_TRUE = 0,
    STEP_FALSE,
    STEP_FAILED,
    STEP_YIELD,
    STEP_CALLED
} StepResult;

const char *simanticsFailureGetName(SimanticsFailure failure)
{
    switch (failure)
    {
    case SIMANTICS_FAILURE_NONE:
        return "none";
    case SIMANTICS_FAILURE_ERROR_TARGET:
        return "the tree branched to its error exit";
    case SIMANTICS_FAILURE_BAD_TARGET:
        return "a branch points past the end of its tree";
    case SIMANTICS_FAILURE_UNSUPPORTED_PRIMITIVE:
        return "a primitive this machine cannot run yet";
    case SIMANTICS_FAILURE_UNSUPPORTED_OWNER:
        return "a data owner this machine cannot read yet";
    case SIMANTICS_FAILURE_UNSUPPORTED_OPERATOR:
        return "an expression operator this machine does not know";
    case SIMANTICS_FAILURE_UNSUPPORTED_CALL_FORMAT:
        return "a call whose arguments are laid out in a way this machine does not know";
    case SIMANTICS_FAILURE_UNSUPPORTED_SEARCH:
        return "a Set To Next search this machine cannot run yet";
    case SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE:
        return "an index or value outside what its owner holds";
    case SIMANTICS_FAILURE_READ_ONLY_OWNER:
        return "a write to an owner that can only be read";
    case SIMANTICS_FAILURE_NO_SUCH_OBJECT:
        return "an object that does not exist";
    case SIMANTICS_FAILURE_NO_SUCH_TREE:
        return "a call to a tree that is not loaded";
    case SIMANTICS_FAILURE_NO_SUCH_CONSTANT:
        return "a constant that is not loaded";
    case SIMANTICS_FAILURE_STACK_OVERFLOW:
        return "too many nested calls";
    case SIMANTICS_FAILURE_TOO_MANY_ITERATIONS:
        return "too many instructions without yielding";
    case SIMANTICS_FAILURE_BREAK_POINT:
        return "a break point fired";
    default:
        return "unknown";
    }
}

Boolean simanticsMachineBegin(SimanticsMachine *machine, const SimanticsHost *host, MemoryArena *arena,
                              Unsigned32 objectCapacity, Unsigned32 randomSeed)
{
    Unsigned32 which;

    machine->host = *host;
    machine->objectCount = 0U;
    machine->objectCapacity = objectCapacity;
    machine->randomState = randomSeed;
    machine->trace = NULL_POINTER;
    machine->traceContext = NULL_POINTER;
    for (which = 0U; which < SIMANTICS_GLOBAL_COUNT; which++)
    {
        machine->globals[which] = 0;
    }
    machine->objects = (SimanticsObject *)memoryArenaAllocate(
        arena, (MemorySize)objectCapacity * sizeof(SimanticsObject), sizeof(Unsigned32));
    return (machine->objects != NULL_POINTER) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

SimanticsObject *simanticsMachineAddObject(SimanticsMachine *machine, Unsigned32 privateGroup,
                                           Unsigned32 semiGlobalGroup)
{
    SimanticsObject *object;
    Unsigned32 which;

    if (machine->objectCount >= machine->objectCapacity)
    {
        return NULL_POINTER;
    }
    object = &machine->objects[machine->objectCount];
    machine->objectCount++;
    object->identifier = (Integer16)machine->objectCount;
    object->privateGroup = privateGroup;
    object->semiGlobalGroup = semiGlobalGroup;
    for (which = 0U; which < SIMANTICS_ATTRIBUTE_COUNT; which++)
    {
        object->attributes[which] = 0;
    }
    for (which = 0U; which < SIMANTICS_OBJECT_DATA_COUNT; which++)
    {
        object->data[which] = 0;
    }
    for (which = 0U; which < SIMANTICS_PERSON_DATA_COUNT; which++)
    {
        object->personData[which] = 0;
    }
    for (which = 0U; which < SIMANTICS_SEMIGLOBAL_ATTRIBUTE_COUNT; which++)
    {
        object->semiGlobalAttributes[which] = 0;
    }
    for (which = 0U; which < SIMANTICS_MOTIVE_COUNT; which++)
    {
        object->motives[which] = 0;
    }
    object->guid = 0U;
    object->isPerson = BOOLEAN_FALSE;
    object->data[OBJECT_DATA_IDENTIFIER] = object->identifier;
    return object;
}

SimanticsObject *simanticsMachineFindObject(SimanticsMachine *machine, Integer16 identifier)
{
    if (identifier <= 0 || (Unsigned32)identifier > machine->objectCount)
    {
        return NULL_POINTER;
    }
    return &machine->objects[identifier - 1];
}

static SimanticsFrame *currentFrame(SimanticsThread *thread)
{
    return &thread->frames[thread->depth - 1U];
}

static StepResult fail(SimanticsThread *thread, SimanticsFailure failure, Unsigned32 detail)
{
    const SimanticsFrame *frame = currentFrame(thread);

    thread->status = SIMANTICS_THREAD_FAILED;
    thread->failure = failure;
    thread->failureDetail = detail;
    thread->failureTree = frame->tree;
    thread->failureNode = frame->node;
    return STEP_FAILED;
}

static Integer16 wrap(Integer32 value)
{
    return (Integer16)(Unsigned16)(Unsigned32)value;
}

static Unsigned16 readOperandWord(const Unsigned8 *operands, Unsigned32 at)
{
    return (Unsigned16)((Unsigned16)operands[at] | (Unsigned16)((Unsigned16)operands[at + 1U] << 8));
}

static Unsigned32 constantGroup(const SimanticsObject *me, Unsigned32 scope)
{
    switch (scope)
    {
    case CONSTANT_SCOPE_PRIVATE:
        return me->privateGroup;
    case CONSTANT_SCOPE_SEMIGLOBAL:
        return me->semiGlobalGroup;
    default:
        return SIMANTICS_GLOBAL_GROUP;
    }
}

static Unsigned16 constantInstanceBase(Unsigned32 scope)
{
    switch (scope)
    {
    case CONSTANT_SCOPE_PRIVATE:
        return (Unsigned16)SIMANTICS_FIRST_PRIVATE_OPCODE;
    case CONSTANT_SCOPE_SEMIGLOBAL:
        return (Unsigned16)SIMANTICS_FIRST_SEMIGLOBAL_OPCODE;
    default:
        return (Unsigned16)SIMANTICS_FIRST_GLOBAL_OPCODE;
    }
}

static StepResult readConstant(SimanticsMachine *machine, SimanticsThread *thread, Unsigned16 encoded,
                               Boolean indexedByTemp, Integer16 *value)
{
    SimanticsObject *me = simanticsMachineFindObject(machine, thread->objectIdentifier);
    Unsigned32 scope = ((Unsigned32)encoded >> 13) & 0x03U;
    Unsigned32 table;
    Unsigned32 index;

    if (me == NULL_POINTER)
    {
        return fail(thread, SIMANTICS_FAILURE_NO_SUCH_OBJECT, (Unsigned32)(Unsigned16)thread->objectIdentifier);
    }
    if (scope > CONSTANT_SCOPE_GLOBAL)
    {
        return fail(thread, SIMANTICS_FAILURE_NO_SUCH_CONSTANT, encoded);
    }
    if (indexedByTemp)
    {
        table = (((Unsigned32)encoded >> 5) & 0x0400U) | (((Unsigned32)encoded >> 3) & 0x03FFU);
        index = (Unsigned32)(Unsigned16)thread->temps[encoded & 0x07U];
    }
    else
    {
        table = (((Unsigned32)encoded >> 9) & 0x0040U) | (((Unsigned32)encoded >> 7) & 0x003FU);
        index = (Unsigned32)encoded & 0x7FU;
    }
    if (machine->host.findConstant == NULL_POINTER ||
        !machine->host.findConstant(machine->host.context, constantGroup(me, scope),
                                    (Unsigned16)(constantInstanceBase(scope) + table), (Unsigned16)index, value))
    {
        return fail(thread, SIMANTICS_FAILURE_NO_SUCH_CONSTANT, encoded);
    }
    return STEP_TRUE;
}

typedef enum ObjectField
{
    OBJECT_FIELD_NONE = 0,
    OBJECT_FIELD_ATTRIBUTE,
    OBJECT_FIELD_SEMIGLOBAL_ATTRIBUTE,
    OBJECT_FIELD_DATA,
    OBJECT_FIELD_PERSON_DATA,
    OBJECT_FIELD_MOTIVE
} ObjectField;

typedef enum IndexSource
{
    INDEX_AS_GIVEN = 0,
    INDEX_FROM_TEMP,
    INDEX_FROM_PARAMETER
} IndexSource;

typedef struct OwnerShape
{
    Unsigned8 owner;
    ObjectField field;
    Boolean mine;
    IndexSource index;
} OwnerShape;

static const OwnerShape ownerShapes[] = {
    { SIMANTICS_OWNER_MY_ATTRIBUTE, OBJECT_FIELD_ATTRIBUTE, BOOLEAN_TRUE, INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_STACK_OBJECT_ATTRIBUTE, OBJECT_FIELD_ATTRIBUTE, BOOLEAN_FALSE, INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_MY_SEMIGLOBAL_ATTRIBUTE, OBJECT_FIELD_SEMIGLOBAL_ATTRIBUTE, BOOLEAN_TRUE, INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_MY_DATA, OBJECT_FIELD_DATA, BOOLEAN_TRUE, INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_STACK_OBJECT_DATA, OBJECT_FIELD_DATA, BOOLEAN_FALSE, INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_STACK_OBJECT_SEMIGLOBAL_ATTRIBUTE, OBJECT_FIELD_SEMIGLOBAL_ATTRIBUTE, BOOLEAN_FALSE,
      INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_TEMP, OBJECT_FIELD_NONE, BOOLEAN_FALSE, INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_TEMP_BY_TEMP, OBJECT_FIELD_NONE, BOOLEAN_FALSE, INDEX_FROM_TEMP },
    { SIMANTICS_OWNER_MY_MOTIVE, OBJECT_FIELD_MOTIVE, BOOLEAN_TRUE, INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_STACK_OBJECT_MOTIVE, OBJECT_FIELD_MOTIVE, BOOLEAN_FALSE, INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_STACK_OBJECT_MOTIVE_BY_TEMP, OBJECT_FIELD_MOTIVE, BOOLEAN_FALSE, INDEX_FROM_TEMP },
    { SIMANTICS_OWNER_MY_PERSON_DATA, OBJECT_FIELD_PERSON_DATA, BOOLEAN_TRUE, INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_STACK_OBJECT_PERSON_DATA, OBJECT_FIELD_PERSON_DATA, BOOLEAN_FALSE, INDEX_AS_GIVEN },
    { SIMANTICS_OWNER_STACK_OBJECT_ATTRIBUTE_BY_PARAMETER, OBJECT_FIELD_ATTRIBUTE, BOOLEAN_FALSE,
      INDEX_FROM_PARAMETER },
    { SIMANTICS_OWNER_MY_PERSON_DATA_BY_TEMP, OBJECT_FIELD_PERSON_DATA, BOOLEAN_TRUE, INDEX_FROM_TEMP },
    { SIMANTICS_OWNER_STACK_OBJECT_PERSON_DATA_BY_TEMP, OBJECT_FIELD_PERSON_DATA, BOOLEAN_FALSE, INDEX_FROM_TEMP },
    { SIMANTICS_OWNER_STACK_OBJECT_SEMIGLOBAL_ATTRIBUTE_BY_PARAMETER, OBJECT_FIELD_SEMIGLOBAL_ATTRIBUTE,
      BOOLEAN_FALSE, INDEX_FROM_PARAMETER }
};

static const OwnerShape *findOwnerShape(Unsigned8 owner)
{
    MemorySize which;

    for (which = 0UL; which < VICTORIA_ARRAY_LENGTH(ownerShapes); which++)
    {
        if (ownerShapes[which].owner == owner)
        {
            return &ownerShapes[which];
        }
    }
    return NULL_POINTER;
}

static Integer16 *objectField(SimanticsObject *object, ObjectField field, Unsigned32 *count)
{
    switch (field)
    {
    case OBJECT_FIELD_ATTRIBUTE:
        *count = SIMANTICS_ATTRIBUTE_COUNT;
        return object->attributes;
    case OBJECT_FIELD_SEMIGLOBAL_ATTRIBUTE:
        *count = SIMANTICS_SEMIGLOBAL_ATTRIBUTE_COUNT;
        return object->semiGlobalAttributes;
    case OBJECT_FIELD_DATA:
        *count = SIMANTICS_OBJECT_DATA_COUNT;
        return object->data;
    case OBJECT_FIELD_PERSON_DATA:
        *count = SIMANTICS_PERSON_DATA_COUNT;
        return object->personData;
    default:
        *count = SIMANTICS_MOTIVE_COUNT;
        return object->motives;
    }
}

static StepResult locate(SimanticsMachine *machine, SimanticsThread *thread, Unsigned8 owner, Unsigned16 data,
                         Integer16 **slot)
{
    SimanticsFrame *frame = currentFrame(thread);
    const OwnerShape *shape = findOwnerShape(owner);
    Unsigned32 index = data;

    *slot = NULL_POINTER;
    if (shape != NULL_POINTER && shape->index == INDEX_FROM_TEMP)
    {
        if (index >= SIMANTICS_TEMP_COUNT)
        {
            return fail(thread, SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE, index);
        }
        index = (Unsigned32)(Unsigned16)thread->temps[index];
    }
    else if (shape != NULL_POINTER && shape->index == INDEX_FROM_PARAMETER)
    {
        if (index >= SIMANTICS_FRAME_VALUE_LIMIT)
        {
            return fail(thread, SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE, index);
        }
        index = (Unsigned32)(Unsigned16)frame->parameters[index];
    }

    if (shape != NULL_POINTER && shape->field != OBJECT_FIELD_NONE)
    {
        Integer16 identifier = shape->mine ? thread->objectIdentifier : thread->stackObjectIdentifier;
        SimanticsObject *object = simanticsMachineFindObject(machine, identifier);
        Unsigned32 count;
        Integer16 *values;

        if (object == NULL_POINTER)
        {
            return fail(thread, SIMANTICS_FAILURE_NO_SUCH_OBJECT, (Unsigned32)(Unsigned16)identifier);
        }
        values = objectField(object, shape->field, &count);
        if (index >= count)
        {
            return fail(thread, SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE, index);
        }
        *slot = &values[index];
        return STEP_TRUE;
    }

    switch (owner)
    {
    case SIMANTICS_OWNER_TEMP:
    case SIMANTICS_OWNER_TEMP_BY_TEMP:
        if (index >= SIMANTICS_TEMP_COUNT)
        {
            return fail(thread, SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE, index);
        }
        *slot = &thread->temps[index];
        return STEP_TRUE;
    case SIMANTICS_OWNER_PARAMETER:
    case SIMANTICS_OWNER_LOCAL:
        if (index >= SIMANTICS_FRAME_VALUE_LIMIT)
        {
            return fail(thread, SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE, index);
        }
        *slot = (owner == SIMANTICS_OWNER_PARAMETER) ? &frame->parameters[index] : &frame->locals[index];
        return STEP_TRUE;
    case SIMANTICS_OWNER_GLOBAL:
        if (index >= SIMANTICS_GLOBAL_COUNT)
        {
            return fail(thread, SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE, index);
        }
        *slot = &machine->globals[index];
        return STEP_TRUE;
    case SIMANTICS_OWNER_STACK_OBJECT_IDENTIFIER:
        *slot = &thread->stackObjectIdentifier;
        return STEP_TRUE;
    case SIMANTICS_OWNER_LITERAL:
    case SIMANTICS_OWNER_CONSTANT:
    case SIMANTICS_OWNER_CONSTANT_BY_TEMP:
        return fail(thread, SIMANTICS_FAILURE_READ_ONLY_OWNER, owner);
    default:
        return fail(thread, SIMANTICS_FAILURE_UNSUPPORTED_OWNER, owner);
    }
}

static StepResult readValue(SimanticsMachine *machine, SimanticsThread *thread, Unsigned8 owner, Unsigned16 data,
                            Integer16 *value)
{
    Integer16 *slot;
    StepResult result;

    switch (owner)
    {
    case SIMANTICS_OWNER_LITERAL:
        *value = (Integer16)data;
        return STEP_TRUE;
    case SIMANTICS_OWNER_CONSTANT:
        return readConstant(machine, thread, data, BOOLEAN_FALSE, value);
    case SIMANTICS_OWNER_CONSTANT_BY_TEMP:
        return readConstant(machine, thread, data, BOOLEAN_TRUE, value);
    default:
        result = locate(machine, thread, owner, data, &slot);
        if (result == STEP_TRUE)
        {
            *value = *slot;
        }
        return result;
    }
}

static StepResult writeValue(SimanticsMachine *machine, SimanticsThread *thread, Unsigned8 owner, Unsigned16 data,
                             Integer16 value)
{
    Integer16 *slot;
    StepResult result = locate(machine, thread, owner, data, &slot);

    if (result == STEP_TRUE)
    {
        *slot = value;
    }
    return result;
}

static StepResult flagMask(SimanticsThread *thread, Integer16 flag, Integer32 *mask)
{
    if (flag < FIRST_FLAG || flag > LAST_FLAG)
    {
        return fail(thread, SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE, (Unsigned32)(Unsigned16)flag);
    }
    *mask = (Integer32)(1L << (flag - FIRST_FLAG));
    return STEP_TRUE;
}

static StepResult compare(Unsigned8 operator, Integer32 left, Integer32 right)
{
    Boolean holds;

    switch (operator)
    {
    case SIMANTICS_OPERATOR_GREATER:
        holds = (Boolean)(left > right);
        break;
    case SIMANTICS_OPERATOR_LESS:
        holds = (Boolean)(left < right);
        break;
    case SIMANTICS_OPERATOR_EQUAL:
        holds = (Boolean)(left == right);
        break;
    case SIMANTICS_OPERATOR_GREATER_OR_EQUAL:
        holds = (Boolean)(left >= right);
        break;
    case SIMANTICS_OPERATOR_LESS_OR_EQUAL:
        holds = (Boolean)(left <= right);
        break;
    default:
        holds = (Boolean)(left != right);
        break;
    }
    return holds ? STEP_TRUE : STEP_FALSE;
}

static StepResult assignWide(SimanticsMachine *machine, SimanticsThread *thread, Unsigned8 leftOwner,
                             Unsigned16 leftData, Unsigned8 rightOwner, Unsigned16 rightData)
{
    Integer16 low;
    Integer16 high;
    StepResult result = readValue(machine, thread, rightOwner, rightData, &low);

    if (result == STEP_TRUE)
    {
        result = readValue(machine, thread, rightOwner, (Unsigned16)(rightData + 1U), &high);
    }
    if (result == STEP_TRUE)
    {
        result = writeValue(machine, thread, leftOwner, leftData, low);
    }
    if (result == STEP_TRUE)
    {
        result = writeValue(machine, thread, leftOwner, (Unsigned16)(leftData + 1U), high);
    }
    return result;
}

static StepResult runExpression(SimanticsMachine *machine, SimanticsThread *thread,
                                const SimanticsInstruction *instruction)
{
    const Unsigned8 *operands = instruction->operands;
    Unsigned16 leftData = readOperandWord(operands, 0U);
    Unsigned16 rightData = readOperandWord(operands, 2U);
    Unsigned8 operator = operands[5];
    Unsigned8 leftOwner = operands[6];
    Unsigned8 rightOwner = operands[7];
    Integer16 left = 0;
    Integer16 right;
    Integer32 outcome;
    Integer32 mask;
    StepResult result;

    if (operator > SIMANTICS_OPERATOR_ASSIGN_WIDE)
    {
        return fail(thread, SIMANTICS_FAILURE_UNSUPPORTED_OPERATOR, operator);
    }
    if (operator == SIMANTICS_OPERATOR_ASSIGN_WIDE)
    {
        return assignWide(machine, thread, leftOwner, leftData, rightOwner, rightData);
    }

    result = readValue(machine, thread, rightOwner, rightData, &right);
    if (result != STEP_TRUE)
    {
        return result;
    }
    if (operator != SIMANTICS_OPERATOR_ASSIGN && operator != SIMANTICS_OPERATOR_ABSOLUTE)
    {
        result = readValue(machine, thread, leftOwner, leftData, &left);
        if (result != STEP_TRUE)
        {
            return result;
        }
    }

    switch (operator)
    {
    case SIMANTICS_OPERATOR_GREATER:
    case SIMANTICS_OPERATOR_LESS:
    case SIMANTICS_OPERATOR_EQUAL:
    case SIMANTICS_OPERATOR_GREATER_OR_EQUAL:
    case SIMANTICS_OPERATOR_LESS_OR_EQUAL:
    case SIMANTICS_OPERATOR_NOT_EQUAL:
        return compare(operator, left, right);
    case SIMANTICS_OPERATOR_FLAG_IS_SET:
        result = flagMask(thread, right, &mask);
        if (result != STEP_TRUE)
        {
            return result;
        }
        return (((Integer32)left & mask) != 0) ? STEP_TRUE : STEP_FALSE;
    case SIMANTICS_OPERATOR_SET_FLAG:
    case SIMANTICS_OPERATOR_CLEAR_FLAG:
        result = flagMask(thread, right, &mask);
        if (result != STEP_TRUE)
        {
            return result;
        }
        outcome = (operator == SIMANTICS_OPERATOR_SET_FLAG) ? ((Integer32)left | mask) : ((Integer32)left & ~mask);
        return writeValue(machine, thread, leftOwner, leftData, wrap(outcome));
    case SIMANTICS_OPERATOR_INCREMENT_AND_LESS:
    case SIMANTICS_OPERATOR_DECREMENT_AND_GREATER:
        left = wrap((Integer32)left + ((operator == SIMANTICS_OPERATOR_INCREMENT_AND_LESS) ? 1 : -1));
        result = writeValue(machine, thread, leftOwner, leftData, left);
        if (result != STEP_TRUE)
        {
            return result;
        }
        return compare((operator == SIMANTICS_OPERATOR_INCREMENT_AND_LESS) ? (Unsigned8)SIMANTICS_OPERATOR_LESS
                                                                         : (Unsigned8)SIMANTICS_OPERATOR_GREATER,
                       left, right);
    case SIMANTICS_OPERATOR_DIVIDE:
    case SIMANTICS_OPERATOR_MODULO:
        if (right == 0)
        {
            return STEP_FALSE;
        }
        outcome = (operator == SIMANTICS_OPERATOR_DIVIDE) ? ((Integer32)left / (Integer32)right)
                                                          : ((Integer32)left % (Integer32)right);
        return writeValue(machine, thread, leftOwner, leftData, wrap(outcome));
    case SIMANTICS_OPERATOR_ADD:
        outcome = (Integer32)left + (Integer32)right;
        break;
    case SIMANTICS_OPERATOR_SUBTRACT:
        outcome = (Integer32)left - (Integer32)right;
        break;
    case SIMANTICS_OPERATOR_MULTIPLY:
        outcome = (Integer32)left * (Integer32)right;
        break;
    case SIMANTICS_OPERATOR_AND:
        outcome = (Integer32)left & (Integer32)right;
        break;
    case SIMANTICS_OPERATOR_OR:
        outcome = (Integer32)left | (Integer32)right;
        break;
    case SIMANTICS_OPERATOR_EXCLUSIVE_OR:
        outcome = (Integer32)left ^ (Integer32)right;
        break;
    case SIMANTICS_OPERATOR_ABSOLUTE:
        outcome = (right < 0) ? -(Integer32)right : (Integer32)right;
        break;
    default:
        outcome = (Integer32)right;
        break;
    }
    return writeValue(machine, thread, leftOwner, leftData, wrap(outcome));
}

static StepResult runRandomNumber(SimanticsMachine *machine, SimanticsThread *thread,
                                  const SimanticsInstruction *instruction)
{
    const Unsigned8 *operands = instruction->operands;
    Integer16 range;
    StepResult result = readValue(machine, thread, operands[6], readOperandWord(operands, 4U), &range);

    if (result != STEP_TRUE)
    {
        return result;
    }
    if (range <= 0)
    {
        return fail(thread, SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE, (Unsigned32)(Unsigned16)range);
    }
    machine->randomState = machine->randomState * RANDOM_MULTIPLIER + RANDOM_INCREMENT;
    return writeValue(machine, thread, operands[2], readOperandWord(operands, 0U),
                      (Integer16)((machine->randomState >> 16) % (Unsigned32)range));
}

static StepResult runBreakPoint(SimanticsMachine *machine, SimanticsThread *thread,
                                const SimanticsInstruction *instruction)
{
    const Unsigned8 *operands = instruction->operands;
    Integer16 condition;
    StepResult result;

    if ((operands[4] & 0x01U) != 0U)
    {
        return STEP_TRUE;
    }
    result = readValue(machine, thread, operands[2], readOperandWord(operands, 0U), &condition);
    if (result != STEP_TRUE)
    {
        return result;
    }
    return (condition != 0) ? fail(thread, SIMANTICS_FAILURE_BREAK_POINT, (Unsigned32)(Unsigned16)condition)
                            : STEP_TRUE;
}

static StepResult runSleep(SimanticsMachine *machine, SimanticsThread *thread, const SimanticsInstruction *instruction)
{
    Integer16 ticks;
    StepResult result = readValue(machine, thread, SIMANTICS_OWNER_PARAMETER,
                                  readOperandWord(instruction->operands, 0U), &ticks);

    if (result != STEP_TRUE)
    {
        return result;
    }
    thread->sleepTicks = (ticks > 0) ? (Unsigned32)ticks : 1U;
    return STEP_YIELD;
}

static Boolean matchesSearch(const SimanticsObject *object, Unsigned32 search, const Unsigned8 *operands,
                             Boolean filtered)
{
    Unsigned32 label = (Unsigned32)readOperandWord(operands, 9U);
    Boolean kind;

    switch (search)
    {
    case SIMANTICS_SEARCH_PERSON:
    case SIMANTICS_SEARCH_SIM:
        kind = object->isPerson;
        break;
    case SIMANTICS_SEARCH_NON_PERSON:
        kind = object->isPerson ? BOOLEAN_FALSE : BOOLEAN_TRUE;
        break;
    case SIMANTICS_SEARCH_OBJECT_OF_TYPE:
        kind = (Boolean)(object->guid == ((Unsigned32)readOperandWord(operands, 0U) |
                                          ((Unsigned32)readOperandWord(operands, 2U) << 16)));
        break;
    default:
        kind = BOOLEAN_TRUE;
        break;
    }
    if (kind && filtered)
    {
        kind = (Boolean)(label < SIMANTICS_OBJECT_DATA_COUNT &&
                         object->data[label] == (Integer16)readOperandWord(operands, 11U));
    }
    return kind;
}

static StepResult runSetToNext(SimanticsMachine *machine, SimanticsThread *thread,
                               const SimanticsInstruction *instruction)
{
    const Unsigned8 *operands = instruction->operands;
    Unsigned32 search = operands[4] & 0x7FU;
    Boolean intoOwner = (Boolean)((operands[4] & 0x80U) != 0U);
    Unsigned8 owner = intoOwner ? operands[5] : (Unsigned8)SIMANTICS_OWNER_STACK_OBJECT_IDENTIFIER;
    Unsigned16 data = intoOwner ? (Unsigned16)operands[7] : 0U;
    Boolean filtered = (Boolean)(instruction->nodeVersion > 0U && (operands[8] & 0x02U) != 0U);
    const SimanticsObject *current;
    Integer16 from;
    Unsigned32 candidate;
    StepResult result = readValue(machine, thread, owner, data, &from);

    if (result != STEP_TRUE)
    {
        return result;
    }
    switch (search)
    {
    case SIMANTICS_SEARCH_VERIFY_STACK_OBJECT:
    case SIMANTICS_SEARCH_VERIFY_SIM_IN_STACK_OBJECT:
        current = simanticsMachineFindObject(machine, from);
        return (current != NULL_POINTER &&
                (search == SIMANTICS_SEARCH_VERIFY_STACK_OBJECT || current->isPerson))
                   ? STEP_TRUE
                   : STEP_FALSE;
    case SIMANTICS_SEARCH_OBJECT:
    case SIMANTICS_SEARCH_PERSON:
    case SIMANTICS_SEARCH_NON_PERSON:
    case SIMANTICS_SEARCH_OBJECT_OF_TYPE:
    case SIMANTICS_SEARCH_SIM:
        break;
    default:
        return fail(thread, SIMANTICS_FAILURE_UNSUPPORTED_SEARCH, search);
    }
    for (candidate = (from > 0) ? (Unsigned32)from + 1U : 1U; candidate <= machine->objectCount; candidate++)
    {
        if (matchesSearch(&machine->objects[candidate - 1U], search, operands, filtered))
        {
            return writeValue(machine, thread, owner, data, (Integer16)candidate);
        }
    }
    return STEP_FALSE;
}

static Unsigned32 callGroup(const SimanticsObject *me, Unsigned16 opcode)
{
    if (opcode < SIMANTICS_FIRST_PRIVATE_OPCODE)
    {
        return SIMANTICS_GLOBAL_GROUP;
    }
    if (opcode < SIMANTICS_FIRST_SEMIGLOBAL_OPCODE)
    {
        return me->privateGroup;
    }
    return me->semiGlobalGroup;
}

static StepResult gatherArguments(SimanticsMachine *machine, SimanticsThread *thread,
                                  const SimanticsInstruction *instruction, Unsigned32 wanted, Integer16 *arguments)
{
    const Unsigned8 *operands = instruction->operands;
    const SimanticsFrame *caller = currentFrame(thread);
    Unsigned8 flags = operands[CALL_ARGUMENT_FLAGS];
    Unsigned32 which;
    Boolean allUnset = BOOLEAN_TRUE;

    for (which = 0U; which < wanted; which++)
    {
        arguments[which] = 0;
    }
    if (instruction->nodeVersion > 0U && (flags & 0x03U) == 0x02U)
    {
        for (which = 0U; which < wanted && which < (Unsigned32)caller->tree->argumentCount; which++)
        {
            arguments[which] = caller->parameters[which];
        }
        return STEP_TRUE;
    }
    if ((flags & 0x01U) != 0U && !(instruction->nodeVersion > 0U && flags == 0xFFU))
    {
        if (wanted >= CALL_ARGUMENT_LIMIT)
        {
            return STEP_TRUE;
        }
        for (which = 0U; which < wanted && which < CALL_WIDE_ARGUMENT_COUNT; which++)
        {
            StepResult result = readValue(machine, thread, operands[which * 3U],
                                          readOperandWord(operands, which * 3U + 1U), &arguments[which]);

            if (result != STEP_TRUE)
            {
                return result;
            }
        }
        return STEP_TRUE;
    }
    for (which = 0U; which < CALL_NARROW_ARGUMENT_COUNT; which++)
    {
        if (operands[which] != 0xFFU)
        {
            allUnset = BOOLEAN_FALSE;
        }
    }
    for (which = 0U; which < wanted && which < CALL_NARROW_ARGUMENT_COUNT; which++)
    {
        if (allUnset)
        {
            arguments[which] = thread->temps[which];
        }
        else if (which == 6U && instruction->nodeVersion == 0U)
        {
            arguments[which] = (Integer16)(Unsigned16)((Unsigned16)operands[13] << 8);
        }
        else
        {
            arguments[which] = (Integer16)readOperandWord(operands, which * 2U);
        }
    }
    return STEP_TRUE;
}

static void pushFrame(SimanticsThread *thread, const SimanticsTree *tree, const Integer16 *arguments)
{
    SimanticsFrame *frame = &thread->frames[thread->depth];
    Unsigned32 which;

    thread->depth++;
    frame->tree = tree;
    frame->node = 0U;
    for (which = 0U; which < SIMANTICS_FRAME_VALUE_LIMIT; which++)
    {
        frame->parameters[which] = (which < (Unsigned32)tree->argumentCount) ? arguments[which] : 0;
        frame->locals[which] = 0;
    }
}

static StepResult runCall(SimanticsMachine *machine, SimanticsThread *thread, const SimanticsInstruction *instruction)
{
    SimanticsObject *me = simanticsMachineFindObject(machine, thread->objectIdentifier);
    const SimanticsTree *callee;
    Integer16 arguments[SIMANTICS_FRAME_VALUE_LIMIT];
    StepResult result;

    if (me == NULL_POINTER)
    {
        return fail(thread, SIMANTICS_FAILURE_NO_SUCH_OBJECT, (Unsigned32)(Unsigned16)thread->objectIdentifier);
    }
    callee = (machine->host.findTree == NULL_POINTER)
                 ? NULL_POINTER
                 : machine->host.findTree(machine->host.context, callGroup(me, instruction->opcode),
                                          instruction->opcode);
    if (callee == NULL_POINTER)
    {
        return fail(thread, SIMANTICS_FAILURE_NO_SUCH_TREE, instruction->opcode);
    }
    if ((Unsigned32)callee->argumentCount > SIMANTICS_FRAME_VALUE_LIMIT ||
        (Unsigned32)callee->localCount > SIMANTICS_FRAME_VALUE_LIMIT)
    {
        return fail(thread, SIMANTICS_FAILURE_VALUE_OUT_OF_RANGE, callee->argumentCount);
    }
    if (thread->depth >= SIMANTICS_STACK_DEPTH)
    {
        return fail(thread, SIMANTICS_FAILURE_STACK_OVERFLOW, thread->depth);
    }
    result = gatherArguments(machine, thread, instruction, callee->argumentCount, arguments);
    if (result != STEP_TRUE)
    {
        return result;
    }
    pushFrame(thread, callee, arguments);
    return STEP_CALLED;
}

static StepResult runInstruction(SimanticsMachine *machine, SimanticsThread *thread,
                                 const SimanticsInstruction *instruction)
{
    if (instruction->opcode >= SIMANTICS_FIRST_GLOBAL_OPCODE)
    {
        return runCall(machine, thread, instruction);
    }
    switch (instruction->opcode)
    {
    case SIMANTICS_PRIMITIVE_SLEEP:
        return runSleep(machine, thread, instruction);
    case SIMANTICS_PRIMITIVE_EXPRESSION:
        return runExpression(machine, thread, instruction);
    case SIMANTICS_PRIMITIVE_RANDOM_NUMBER:
        return runRandomNumber(machine, thread, instruction);
    case SIMANTICS_PRIMITIVE_BREAK_POINT:
        return runBreakPoint(machine, thread, instruction);
    case SIMANTICS_PRIMITIVE_SET_TO_NEXT:
        return runSetToNext(machine, thread, instruction);
    default:
        return fail(thread, SIMANTICS_FAILURE_UNSUPPORTED_PRIMITIVE, instruction->opcode);
    }
}

static StepResult enterNode(SimanticsThread *thread)
{
    const SimanticsFrame *frame = currentFrame(thread);

    if (frame->node >= frame->tree->instructionCount)
    {
        return fail(thread, SIMANTICS_FAILURE_BAD_TARGET, frame->node);
    }
    return STEP_TRUE;
}

static StepResult follow(SimanticsMachine *machine, SimanticsThread *thread, Boolean outcome)
{
    while (thread->depth > 0U)
    {
        SimanticsFrame *frame = currentFrame(thread);
        const SimanticsInstruction *instruction = &frame->tree->instructions[frame->node];
        Unsigned16 target = outcome ? instruction->trueTarget : instruction->falseTarget;

        if (machine->trace != NULL_POINTER)
        {
            machine->trace(machine->traceContext, thread, instruction, target);
        }
        if (target == SIMANTICS_TARGET_ERROR)
        {
            return fail(thread, SIMANTICS_FAILURE_ERROR_TARGET, frame->node);
        }
        if (target != SIMANTICS_TARGET_TRUE && target != SIMANTICS_TARGET_FALSE)
        {
            frame->node = target;
            return enterNode(thread);
        }
        outcome = (target == SIMANTICS_TARGET_TRUE) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
        if (thread->depth == 1U)
        {
            thread->status = outcome ? SIMANTICS_THREAD_RETURNED_TRUE : SIMANTICS_THREAD_RETURNED_FALSE;
            return outcome ? STEP_TRUE : STEP_FALSE;
        }
        thread->depth--;
    }
    return STEP_FAILED;
}

Boolean simanticsThreadStart(SimanticsMachine *machine, SimanticsThread *thread, Integer16 objectIdentifier,
                             const SimanticsTree *tree, const Integer16 *arguments, Unsigned32 argumentCount)
{
    Integer16 padded[SIMANTICS_FRAME_VALUE_LIMIT];
    Unsigned32 which;

    if (tree == NULL_POINTER || simanticsMachineFindObject(machine, objectIdentifier) == NULL_POINTER ||
        (Unsigned32)tree->argumentCount > SIMANTICS_FRAME_VALUE_LIMIT ||
        argumentCount > SIMANTICS_FRAME_VALUE_LIMIT)
    {
        return BOOLEAN_FALSE;
    }
    for (which = 0U; which < SIMANTICS_FRAME_VALUE_LIMIT; which++)
    {
        padded[which] = (which < argumentCount) ? arguments[which] : 0;
    }
    for (which = 0U; which < SIMANTICS_TEMP_COUNT; which++)
    {
        thread->temps[which] = 0;
    }
    thread->objectIdentifier = objectIdentifier;
    thread->stackObjectIdentifier = 0;
    thread->depth = 0U;
    thread->sleepTicks = 0U;
    thread->failure = SIMANTICS_FAILURE_NONE;
    thread->failureDetail = 0U;
    thread->failureTree = NULL_POINTER;
    thread->failureNode = 0U;
    thread->status = SIMANTICS_THREAD_RUNNING;
    pushFrame(thread, tree, padded);
    (void)enterNode(thread);
    return BOOLEAN_TRUE;
}

SimanticsThreadStatus simanticsThreadTick(SimanticsMachine *machine, SimanticsThread *thread)
{
    Unsigned32 iterations = 0U;

    if (thread->status == SIMANTICS_THREAD_SLEEPING)
    {
        thread->sleepTicks--;
        if (thread->sleepTicks > 0U)
        {
            return thread->status;
        }
        thread->status = SIMANTICS_THREAD_RUNNING;
        if (follow(machine, thread, BOOLEAN_TRUE) == STEP_FAILED || thread->status != SIMANTICS_THREAD_RUNNING)
        {
            return thread->status;
        }
    }

    while (thread->status == SIMANTICS_THREAD_RUNNING)
    {
        SimanticsFrame *frame = currentFrame(thread);
        StepResult result;

        if (iterations >= SIMANTICS_ITERATION_LIMIT)
        {
            (void)fail(thread, SIMANTICS_FAILURE_TOO_MANY_ITERATIONS, iterations);
            break;
        }
        iterations++;

        result = runInstruction(machine, thread, &frame->tree->instructions[frame->node]);
        switch (result)
        {
        case STEP_TRUE:
        case STEP_FALSE:
            (void)follow(machine, thread, (result == STEP_TRUE) ? BOOLEAN_TRUE : BOOLEAN_FALSE);
            break;
        case STEP_CALLED:
            (void)enterNode(thread);
            break;
        case STEP_YIELD:
            thread->status = SIMANTICS_THREAD_SLEEPING;
            break;
        default:
            break;
        }
    }
    return thread->status;
}
