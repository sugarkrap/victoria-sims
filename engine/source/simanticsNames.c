#include "victoria/simanticsNames.h"

static const char *const primitiveNames[] = {
    "Sleep", "Generic Sims Call", "Expression", "Find Best Interaction", "Grab (obsolete)", "Drop (obsolete)",
    "Change Suit (obsolete)", "Refresh", "Random Number", "Burn (obsolete)", "Tutorial", "Get Distance To",
    "Get Direction To", "Push Interaction", "Find Best Object For Function", "Break Point", "Find Location For",
    "Idle For Input", "Remove Object Instance", "Make New Character", "Run Functional Tree",
    "Show String (unused)", "Turn Body", "Play or Stop Sound Event", "Old Relationship (unused)", "Alter Budget",
    "Relationship", "Go To Relative Position", "Run Tree By Name", "Set Motive Change", "Gosub Found Action",
    "Set To Next", "Test Object Type", "Find Five Worst Motives", "UI Effect", "Camera Control", "Dialog",
    "Test Sim Interacting With", "(unused)", "(unused)", "(unused)", "Set Balloon or Headline (obsolete)",
    "Create New Object Instance", "Drop Onto (obsolete)", "Animate Sim (obsolete)", "Go To Routing Slot", "Snap",
    "Reach (obsolete)", "Stop All Sounds", "Notify Stack Object Out Of Idle", "Add or Change Action String",
    "Manage Inventory"
};

static const char *const laterPrimitiveNames[] = {
    "Animate Object", "Animate Sim", "Animate Overlay", "Animate Stop", "Change Material", "Look At",
    "Change Light", "Effect Stop or Start", "Snap Into", "Assign Locomotion Animations", "Debug", "Reach or Put",
    "Age", "Array Operation", "Message", "Ray Trace", "Change Outfit", "On Timer", "Cinematic", "Want Satisfy",
    "Follow Sim", "Lua"
};

#define FIRST_LATER_PRIMITIVE 0x69U

static const char *const ownerNames[] = {
    "my attribute", "stack object's attribute", "my semi-global attribute", "my object data",
    "stack object's object data", "stack object's semi-global attribute", "global", "literal", "temp", "parameter",
    "stack object identifier", "temp by temp", "tree advert range", "stack object's temp", "my motive",
    "stack object's motive", "stack object's slot", "stack object's motive by temp", "my person data",
    "stack object's person data", "my slot", "stack object's definition", "stack object's attribute by parameter",
    "room by temp 0", "neighbor in stack object", "local", "constant", "(unused)", "tree advert personality variance",
    "tree advert minimum", "my person data by temp", "stack object's person data by temp", "neighbor's person data",
    "job data", "neighborhood data", "stack object's function", "my type attribute", "stack object's type attribute",
    "neighbor's object definition", "my temporary token", "stack object's temporary token",
    "my object array iterator index", "stack object's object array iterator index", "my object array iterator data",
    "stack object's object array iterator data", "my object array element at temp 0",
    "stack object's object array element at temp 0", "constant by temp", "my slot by temp",
    "stack object's slot by temp", "stack object's semi-global attribute by parameter",
    "stack object's master definition"
};

static const char *const searchNames[] = {
    "object",
    "person",
    "non-person",
    "part of a multi-tile object",
    "object of type",
    "neighbor id",
    "object with category == param 0",
    "neighbor of type",
    "object on same tile",
    "object adjacent to object in local",
    "career",
    "lot id",
    "object with category == temp 0",
    "person closest to person in temp 0",
    "job object instance from job object GUID in sim in stack object",
    "job object instance from job object GUID from neighbor id in stack object",
    "vehicle",
    "neighbor of family in temp 0",
    "object of type GUID in temp 0 and 1",
    "group member of object in temp 0",
    "group member in reverse order of object in temp 0",
    "family member parent",
    "family member sibling",
    "family member child",
    "visible object in my view within distance in temp 0",
    "object in room in temp 0",
    "verify stack object",
    "utility using object",
    "neighbor with non-zero var3 relationship to neighbor id in temp 0",
    "verify neighbor id in stack object",
    "verify sim in stack object",
    "family member spouse",
    "owner of object",
    "hauntable object",
    "object within one tile of object in local",
    "neighbor id with a STR value greater than temp 1 to me",
    "neighbor id with rel var 1 and temp 1",
    "object with wants category equal to temp 0",
    "influence",
    "neighbor with no relationship to neighbor id in temp 0",
    "sim (pets, humans)",
    "pet",
    "dog",
    "large dog",
    "small dog",
    "cat",
    "sim neighbor id (humans, pets)",
    "pet neighbor id",
    "dog neighbor id",
    "large dog neighbor id",
    "small dog neighbor id",
    "cat neighbor id",
    "jaguar",
    "orangutan",
    "jaguar neighbor",
    "orangutan neighbor"
};

const char *simanticsPrimitiveGetName(Unsigned32 opcode)
{
    if (opcode < VICTORIA_ARRAY_LENGTH(primitiveNames))
    {
        return primitiveNames[opcode];
    }
    if (opcode >= FIRST_LATER_PRIMITIVE && opcode - FIRST_LATER_PRIMITIVE < VICTORIA_ARRAY_LENGTH(laterPrimitiveNames))
    {
        return laterPrimitiveNames[opcode - FIRST_LATER_PRIMITIVE];
    }
    return "(no name known)";
}

const char *simanticsOwnerGetName(Unsigned32 owner)
{
    return (owner < VICTORIA_ARRAY_LENGTH(ownerNames)) ? ownerNames[owner] : "(no name known)";
}

const char *simanticsSearchGetName(Unsigned32 search)
{
    return (search < VICTORIA_ARRAY_LENGTH(searchNames)) ? searchNames[search] : "(no name known)";
}
