#include "victoria/simanticsTree.h"

#define HEADER_BYTES 76UL
#define OFFSET_FORMAT 64UL
#define OFFSET_INSTRUCTION_COUNT 66UL
#define OFFSET_TREE_TYPE 68UL
#define OFFSET_ARGUMENT_COUNT 69UL
#define OFFSET_LOCAL_COUNT 70UL
#define OFFSET_HEADER_FLAGS 71UL
#define OFFSET_TREE_VERSION 72UL

#define FIRST_FORMAT 0x8000U
#define LAST_FORMAT 0x8009U
#define FIRST_FORMAT_WITH_WIDE_OPERANDS 0x8003U
#define FIRST_FORMAT_WITH_NODE_VERSION 0x8005U
#define FIRST_FORMAT_WITH_WIDE_TARGETS 0x8007U
#define FIRST_FORMAT_WITH_CACHE_FLAGS 0x8009U

#define NARROW_OPERAND_COUNT 8UL
#define NARROW_TARGET_ERROR 0x00FDU
#define NARROW_TARGET_TRUE 0x00FEU
#define NARROW_TARGET_FALSE 0x00FFU

const char *simanticsTreeResultGetName(SimanticsTreeResult result)
{
    switch (result)
    {
    case SIMANTICS_TREE_OK:
        return "ok";
    case SIMANTICS_TREE_TOO_SHORT:
        return "shorter than a tree header";
    case SIMANTICS_TREE_UNKNOWN_FORMAT:
        return "a tree format this reader does not know";
    case SIMANTICS_TREE_TRUNCATED:
        return "the instructions run past the end of the resource";
    case SIMANTICS_TREE_OUT_OF_ROOM:
        return "not enough arena for the instructions";
    default:
        return "unknown";
    }
}

static Unsigned16 readUnsigned16(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned16)((Unsigned16)bytes[offset] | (Unsigned16)((Unsigned16)bytes[offset + 1UL] << 8));
}

static Unsigned32 readUnsigned32(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned32)readUnsigned16(bytes, offset) |
           ((Unsigned32)readUnsigned16(bytes, offset + 2UL) << 16);
}

static Unsigned16 widenNarrowTarget(Unsigned8 target)
{
    switch (target)
    {
    case NARROW_TARGET_ERROR:
        return SIMANTICS_TARGET_ERROR;
    case NARROW_TARGET_TRUE:
        return SIMANTICS_TARGET_TRUE;
    case NARROW_TARGET_FALSE:
        return SIMANTICS_TARGET_FALSE;
    default:
        return (Unsigned16)target;
    }
}

MemorySize simanticsTreeGetInstructionSize(Unsigned16 format)
{
    if (format < FIRST_FORMAT || format > LAST_FORMAT)
    {
        return 0UL;
    }
    if (format < FIRST_FORMAT_WITH_WIDE_OPERANDS)
    {
        return 4UL + NARROW_OPERAND_COUNT;
    }
    if (format < FIRST_FORMAT_WITH_NODE_VERSION)
    {
        return 4UL + SIMANTICS_OPERAND_COUNT;
    }
    if (format < FIRST_FORMAT_WITH_WIDE_TARGETS)
    {
        return 5UL + SIMANTICS_OPERAND_COUNT;
    }
    return 7UL + SIMANTICS_OPERAND_COUNT;
}

static void readInstruction(SimanticsInstruction *instruction, const Unsigned8 *bytes, Unsigned16 format)
{
    MemorySize at = 2UL;
    MemorySize operandCount =
        (format < FIRST_FORMAT_WITH_WIDE_OPERANDS) ? NARROW_OPERAND_COUNT : (MemorySize)SIMANTICS_OPERAND_COUNT;
    MemorySize which;

    instruction->opcode = readUnsigned16(bytes, 0UL);
    if (format < FIRST_FORMAT_WITH_WIDE_TARGETS)
    {
        instruction->trueTarget = widenNarrowTarget(bytes[at]);
        instruction->falseTarget = widenNarrowTarget(bytes[at + 1UL]);
        at += 2UL;
    }
    else
    {
        instruction->trueTarget = readUnsigned16(bytes, at);
        instruction->falseTarget = readUnsigned16(bytes, at + 2UL);
        at += 4UL;
    }
    instruction->nodeVersion = 0U;
    if (format >= FIRST_FORMAT_WITH_NODE_VERSION)
    {
        instruction->nodeVersion = bytes[at];
        at += 1UL;
    }
    for (which = 0UL; which < (MemorySize)SIMANTICS_OPERAND_COUNT; which++)
    {
        instruction->operands[which] = (which < operandCount) ? bytes[at + which] : 0U;
    }
}

SimanticsTreeResult simanticsTreeRead(SimanticsTree *tree, const Unsigned8 *bytes, MemorySize size,
                                      MemoryArena *arena)
{
    MemorySize instructionSize;
    MemorySize at;
    MemorySize which;
    SimanticsInstruction *instructions;

    if (size < HEADER_BYTES)
    {
        return SIMANTICS_TREE_TOO_SHORT;
    }
    tree->format = readUnsigned16(bytes, OFFSET_FORMAT);
    instructionSize = simanticsTreeGetInstructionSize(tree->format);
    if (instructionSize == 0UL)
    {
        return SIMANTICS_TREE_UNKNOWN_FORMAT;
    }

    for (which = 0UL; which < (MemorySize)SIMANTICS_TREE_NAME_LIMIT - 1UL && bytes[which] != 0U; which++)
    {
        tree->name[which] = (char)bytes[which];
    }
    tree->name[which] = '\0';
    tree->instructionCount = readUnsigned16(bytes, OFFSET_INSTRUCTION_COUNT);
    tree->treeType = bytes[OFFSET_TREE_TYPE];
    tree->argumentCount = bytes[OFFSET_ARGUMENT_COUNT];
    tree->localCount = bytes[OFFSET_LOCAL_COUNT];
    tree->headerFlags = bytes[OFFSET_HEADER_FLAGS];
    tree->treeVersion = readUnsigned32(bytes, OFFSET_TREE_VERSION);
    tree->instructions = NULL_POINTER;

    at = HEADER_BYTES + ((tree->format >= FIRST_FORMAT_WITH_CACHE_FLAGS) ? 1UL : 0UL);
    if (at + (MemorySize)tree->instructionCount * instructionSize > size)
    {
        return SIMANTICS_TREE_TRUNCATED;
    }
    if (tree->instructionCount == 0U)
    {
        return SIMANTICS_TREE_OK;
    }

    instructions = (SimanticsInstruction *)memoryArenaAllocate(
        arena, (MemorySize)tree->instructionCount * sizeof(SimanticsInstruction), sizeof(Unsigned32));
    if (instructions == NULL_POINTER)
    {
        return SIMANTICS_TREE_OUT_OF_ROOM;
    }
    for (which = 0UL; which < (MemorySize)tree->instructionCount; which++, at += instructionSize)
    {
        readInstruction(&instructions[which], bytes + at, tree->format);
    }
    tree->instructions = instructions;
    return SIMANTICS_TREE_OK;
}
