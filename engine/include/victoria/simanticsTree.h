#ifndef VICTORIA_SIMANTICS_TREE_HEADER
#define VICTORIA_SIMANTICS_TREE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

#define SIMANTICS_TREE_NAME_LIMIT 64U
#define SIMANTICS_OPERAND_COUNT 16U

#define SIMANTICS_TARGET_ERROR 0xFFFCU
#define SIMANTICS_TARGET_TRUE 0xFFFDU
#define SIMANTICS_TARGET_FALSE 0xFFFEU

#define SIMANTICS_FIRST_GLOBAL_OPCODE 0x0100U
#define SIMANTICS_FIRST_PRIVATE_OPCODE 0x1000U
#define SIMANTICS_FIRST_SEMIGLOBAL_OPCODE 0x2000U

typedef struct SimanticsInstruction
{
    Unsigned16 opcode;
    Unsigned16 trueTarget;
    Unsigned16 falseTarget;
    Unsigned8 nodeVersion;
    Unsigned8 operands[SIMANTICS_OPERAND_COUNT];
} SimanticsInstruction;

typedef struct SimanticsTree
{
    char name[SIMANTICS_TREE_NAME_LIMIT];
    Unsigned16 format;
    Unsigned8 treeType;
    Unsigned8 argumentCount;
    Unsigned8 localCount;
    Unsigned8 headerFlags;
    Unsigned32 treeVersion;
    Unsigned16 instructionCount;
    const SimanticsInstruction *instructions;
} SimanticsTree;

typedef enum SimanticsTreeResult
{
    SIMANTICS_TREE_OK = 0,
    SIMANTICS_TREE_TOO_SHORT,
    SIMANTICS_TREE_UNKNOWN_FORMAT,
    SIMANTICS_TREE_TRUNCATED,
    SIMANTICS_TREE_OUT_OF_ROOM
} SimanticsTreeResult;

const char *simanticsTreeResultGetName(SimanticsTreeResult result);

MemorySize simanticsTreeGetInstructionSize(Unsigned16 format);

SimanticsTreeResult simanticsTreeRead(SimanticsTree *tree, const Unsigned8 *bytes, MemorySize size,
                                      MemoryArena *arena);

#endif
