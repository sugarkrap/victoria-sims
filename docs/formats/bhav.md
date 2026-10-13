# BHAV behaviour trees, and the machine that runs them

What `engine/source/simanticsTree.c` reads and `engine/source/simanticsMachine.c`
runs. As elsewhere in these documents, **verified** means checked against real
data by this project, *unverified* means taken from the community record and
not yet checked against the game.

The community record here is PJSE, the SimAntics editor inside SimPE
(github.com/frankwilco/PJSE), and its disassembler notes. It is an editor, not
an interpreter: it says how instructions are laid out and named, never what the
game does when it runs them. Everything about behaviour below is therefore
*unverified* until a probe against the real game confirms it.

## Header

76 bytes, little-endian, then one more byte in format 0x8009.

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 64 | Name, NUL-terminated |
| 0x40 | 2 | Format, 0x8000 to 0x8009 |
| 0x42 | 2 | Instruction count |
| 0x44 | 1 | Tree type |
| 0x45 | 1 | Argument count |
| 0x46 | 1 | Local count |
| 0x47 | 1 | Header flags |
| 0x48 | 4 | Tree version |
| 0x4C | 1 | Cache flags, format 0x8009 only |

**Verified**: the disc survey decoded all 44,792 trees on a retail disc with this
layout and none came out malformed. Formats seen: 0x8000 to 0x8007.

## Instructions

| Format | Size | Layout |
| --- | --- | --- |
| 0x8000 to 0x8002 | 12 | opcode (2), true (1), false (1), operands (8) |
| 0x8003 to 0x8004 | 20 | opcode (2), true (1), false (1), operands (16) |
| 0x8005 to 0x8006 | 21 | opcode (2), true (1), false (1), node version (1), operands (16) |
| 0x8007 to 0x8009 | 23 | opcode (2), true (2), false (2), node version (1), operands (16) |

Branch targets are instruction indices, except three exits. The reader widens
the one-byte form, so the machine only ever sees the wide one.

| Exit | One byte | Two bytes |
| --- | --- | --- |
| Error | 0xFD | 0xFFFC |
| Return true | 0xFE | 0xFFFD |
| Return false | 0xFF | 0xFFFE |

Opcodes below 0x0100 are primitives. 0x0100 to 0x0FFF call a tree in the
global group 0x7FD46CD0, 0x1000 to 0x1FFF one in the object's own group, and
0x2000 and above one in the object's semi-global group.

## Calls

The callee's argument count says how many arguments it takes. The operands say
where they come from, in one of four layouts, chosen in this order:

1. **The caller's parameters**: node version above zero and `operands[12] & 3`
   equal to 2.
2. **Four owner and value pairs**: `operands[12] & 1` set, unless the node
   version is above zero and `operands[12]` is 0xFF. Each pair is an owner byte
   and a 16-bit value, three bytes apart. Only for callees taking fewer than
   nine arguments.
3. **Eight 16-bit literals**: any of the first eight operand bytes is not 0xFF.
   With node version zero, the seventh argument's low byte reads as zero.
4. **Temps 0 to 7**: the first eight operand bytes are all 0xFF.

Arguments the operands do not supply are zero.

## Expression, primitive 0x02

| Operand | Meaning |
| --- | --- |
| 0 to 1 | Left value |
| 2 to 3 | Right value |
| 5 | Operator |
| 6 | Left owner |
| 7 | Right owner |

Operators, by number: `>`, `<`, `==`, `+=`, `-=`, `:=`, `*=`, `/=`, flag set?,
set flag, clear flag, `++` and `<`, `mod=`, `and=`, `>=`, `<=`, `!=`, `--` and
`>`, `or=`, `xor=`, `:= abs()`, and a 32-bit assignment that copies two
neighbouring values at once.

Values are signed 16-bit. Flags are numbered from 1. Division or modulo by zero
returns false, per PJSE's disassembler note that non-comparison operators
"return false if error".

## Data owners

PJSE names 52. The machine reads these so far:

| Owners | What |
| --- | --- |
| 0x00, 0x01, 0x16 | My and the stack object's attributes, the latter also at an index a parameter holds |
| 0x02, 0x05, 0x32 | My and the stack object's semi-global attributes, the latter also through a parameter |
| 0x03, 0x04 | My and the stack object's object data |
| 0x06, 0x07, 0x08, 0x09, 0x19 | Globals, literals, temps, parameters, locals |
| 0x0A | The stack object's identifier |
| 0x0B | A temp chosen by a temp |
| 0x0E, 0x0F, 0x11 | My and the stack object's motives, the latter also at an index a temp holds |
| 0x12, 0x13, 0x1E, 0x1F | My and the stack object's person data, also at an index a temp holds |
| 0x1A, 0x2F | Constants, the latter indexed by a temp |

Any other owner stops the thread and names itself, so a real tree reports
exactly what it needs next.

A constant's 16-bit value packs scope, table and index:

| Bits | Constant (0x1A) | Constant by temp (0x2F) |
| --- | --- | --- |
| 13 to 14 | Scope: 0 private, 1 semi-global, 2 global | Same |
| Index | Bits 0 to 6 | The temp named by bits 0 to 2 |
| Table | Bits 7 to 12, and bit 15 as its seventh bit | Bits 3 to 12, and bit 15 as its eleventh |

The table is added to 0x1000, 0x2000 or 0x0100 by scope to give the BCON
instance.

## Other primitives so far

| Opcode | Name | Operands |
| --- | --- | --- |
| 0x00 | Sleep | 0 to 1: the parameter holding the tick count |
| 0x08 | Random Number | 0 to 1 and 2: the destination value and owner; 4 to 5 and 6: the range value and owner, giving 0 to just under it |
| 0x0F | Break Point | 4 bit 0: ignored in release; otherwise breaks when the value at 0 to 1, owner 2, is not zero |
| 0x1F | Set To Next | 4: search type in bits 0 to 6, and bit 7 to write the result to the owner in 5 and the value in 7 rather than the stack object; 0 to 3: a GUID for searches by type; with a node version, 8 bit 1 filters on object data label 9 to 10 equal to 11 to 12. False when nothing comes next |

Set To Next searches built so far: any object (0x00), person (0x01), non-person
(0x02), object of a GUID (0x04), verify the stack object (0x1A), verify a Sim in
the stack object (0x1E), and Sim (0x28, pets not yet told apart). Any other
search stops the thread and names its type.

## Unverified guesses the machine makes

These are choices the machine had to make that nothing here confirms. They are
the first things a probe against the real game should settle.

* **Temps** are shared by every frame of a thread. There are 20, a number taken
  from FreeSO's Sims Online machine, not from The Sims 2.
* **The stack object** belongs to the thread, not the frame, so a callee that
  changes it changes it for its caller.
* **Sleep** always yields at least one tick, even for zero.
* **Arithmetic** wraps at 16 bits rather than clamping.
* **Limits**: 64 nested calls and 100,000 instructions without yielding before
  the thread is stopped.
* **Object data 0x0B** holds the object's own identifier.
* **Set To Next** walks objects in identifier order, starting after the value
  it is given, and treats the two verify searches as a check of that value
  alone. Whether disabled objects are skipped is ignored, since nothing is
  disabled yet.
* **Semi-global attributes** live on each object, apart from its attributes.
