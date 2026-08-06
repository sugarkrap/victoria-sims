#!/usr/bin/env python3
"""Writes testAssets/scenegraph/sim_fixture.package.

A Sim, authored from nothing.

The engine's whole-Sim path — find four resources by name, walk each to a
container, join them, hang them on a skeleton, then dress them out of the
catalogue — has never had a fixture. The test disc carries a teapot, which is
one rigid model with no bones, no skeleton and no catalogue entry, so `make
verify` could not catch a regression in any of it. Three defects reached a
screen because of that: a Sim's head left behind by a merge, part textures lost
on the first pose, and a face painted green because a part index was handed to
something that wanted a primitive index.

Nothing here may come from a retail disc, so none of it does. Every byte below
is written out from the format the engine's own readers expect, which is why
this file is long: it is a writer for seven formats that only had readers.

That has a second use. A reader can only be checked against files somebody else
wrote; a writer states the layout in one place, in the open, where a
disagreement with the reader shows up as a fixture that will not load rather
than as a retail disc that mysteriously does not work.

The Sim is boxes. It is not meant to look like anything — it is meant to have
the STRUCTURE a Sim has: several parts, one of them with two primitives and two
materials, all of them weighted to one skeleton, and catalogue entries that name
replacements for them.
"""

import struct
import sys
import os

# --------------------------------------------------------------------------
# The names the engine looks things up by, and the hash it looks them up with.
# --------------------------------------------------------------------------

TYPE_CRES = 0xE519C933
TYPE_SHPE = 0xFC6EB1F7
TYPE_GMND = 0x7BA3838C
TYPE_GMDC = 0xAC4F8687
TYPE_TXMT = 0x49596978
TYPE_TXTR = 0x1C4A276C
TYPE_SKIN_ENTRY = 0xEBCF3E27
TYPE_KEY_LIST = 0xAC506764

BLOCK_RESOURCE_NODE = 0xE519C933
BLOCK_TRANSFORM_NODE = 0x65246462
BLOCK_SHAPE_REFERENCE_NODE = 0x65245517

COLLECTION_MARK = 0xFFFF0001

CRC24_POLYNOMIAL = 0x864CFB
CRC24_INITIAL = 0xB704CE
CRC32_POLYNOMIAL = 0x04C11DB7
CRC32_INITIAL = 0xFFFFFFFF


def crc24(name):
    """The CRC24 utils/resourceHash.c computes, lower-cased the same way."""
    remainder = CRC24_INITIAL
    for character in name.lower():
        remainder ^= (ord(character) & 0xFF) << 16
        for _ in range(8):
            remainder = (remainder << 1) & 0xFFFFFFFF
            if remainder & 0x01000000:
                remainder ^= CRC24_POLYNOMIAL
    return remainder & 0x00FFFFFF


def crc32Mpeg2(name):
    remainder = CRC32_INITIAL
    for character in name.lower():
        remainder ^= (ord(character) & 0xFF) << 24
        remainder &= 0xFFFFFFFF
        for _ in range(8):
            if remainder & 0x80000000:
                remainder = ((remainder << 1) ^ CRC32_POLYNOMIAL) & 0xFFFFFFFF
            else:
                remainder = (remainder << 1) & 0xFFFFFFFF
    return remainder


def instanceOf(name):
    return crc24(name) | 0xFF000000


def instanceHighOf(name):
    return crc32Mpeg2(name)


# The group every fixture resource is filed under. A scenegraph lookup by name
# ignores the group entirely — the name hashes to the instance words — so this
# only has to be consistent. The catalogue's sidecar lookup does NOT ignore it:
# a key list is matched on group AND instance, so an entry and its list have to
# agree, and that agreement is one of the things worth having a fixture for.
GROUP = 0x1F000000


# --------------------------------------------------------------------------
# Writing primitives, in the two string conventions the formats use.
# --------------------------------------------------------------------------


def u8(value):
    return struct.pack("<B", value & 0xFF)


def u16(value):
    return struct.pack("<H", value & 0xFFFF)


def u32(value):
    return struct.pack("<I", value & 0xFFFFFFFF)


def f32(value):
    return struct.pack("<f", value)


def sgString(text):
    """A scenegraph string: one to five length bytes with a continuation bit.

    NOT the same as a property set's string, which is a flat four-byte length.
    Either rule applied to the other's stream yields a plausible length, which
    is exactly why both are spelled out here rather than shared."""
    data = text.encode("ascii")
    length = len(data)
    out = b""
    while True:
        byte = length & 0x7F
        length >>= 7
        if length:
            out += u8(byte | 0x80)
        else:
            out += u8(byte)
            break
    return out + data


def cpfString(text):
    """A property set string: a flat four-byte length and then the bytes."""
    data = text.encode("ascii")
    return u32(len(data)) + data


def shortString(text):
    """A geometry container string: one length byte. Short names only."""
    data = text.encode("ascii")
    assert len(data) < 256
    return u8(len(data)) + data


def typeInformation(name, identifier, version):
    return sgString(name) + u32(identifier) + u32(version)


def objectReference(external, index):
    """THREE fields, not two: present, then kind, then the index.

    The first byte says whether there is a reference at all — a nought there and
    the record stops, one byte long. Only then comes the kind: nought means a
    block in this same collection, anything else means one of the file links.
    Writing it as a kind byte and an index, which is what it looks like from the
    reader's second half, makes every reference one byte short and moves
    everything after it. A CRES then reads its first block correctly and takes
    the middle of the second for a block type."""
    return u8(1) + u8(1 if external else 0) + u32(index)


def noObjectReference():
    return u8(0)


def collectionHeader(links, blockTypes):
    """The mark, the file links, and the list of block types.

    Links are written group, instance, high instance, TYPE — which is not the
    order a package index entry uses, and writing them in index order produces
    a key that finds nothing."""
    out = u32(COLLECTION_MARK) + u32(len(links))
    for (typeIdentifier, group, instance, instanceHigh) in links:
        out += u32(group) + u32(instance) + u32(instanceHigh) + u32(typeIdentifier)
    out += u32(len(blockTypes))
    for blockType in blockTypes:
        out += u32(blockType)
    return out


def objectGraphNode(tag):
    """cObjectGraphNode at version 4, which is the version that carries a tag."""
    return typeInformation("cObjectGraphNode", 0x0C0B7347, 4) + u32(0) + sgString(tag)


def compositionTree(tag, childBlocks):
    """cCompositionTreeNode: a graph node, then the blocks hanging below it."""
    out = typeInformation("cCompositionTreeNode", 0x7F888F27, 11)
    out += objectGraphNode(tag)
    out += u32(len(childBlocks))
    for child in childBlocks:
        out += objectReference(False, child)
    return out


def transformBody(tag, childBlocks, translation, rotation, boneIdentifier):
    out = compositionTree(tag, childBlocks)
    for axis in range(3):
        out += f32(translation[axis])
    for axis in range(4):
        out += f32(rotation[axis])
    out += u32(boneIdentifier)
    return out


def boundedNode(tag, childBlocks, translation, rotation, boneIdentifier):
    out = typeInformation("cBoundedNode", 0xE9075BC5, 5)
    out += typeInformation("cTransformNode", BLOCK_TRANSFORM_NODE, 7)
    out += transformBody(tag, childBlocks, translation, rotation, boneIdentifier)
    out += u8(0)
    return out


def renderableNode(tag, childBlocks, translation, rotation, boneIdentifier):
    out = typeInformation("cRenderableNode", 0xE519C933, 5)
    out += boundedNode(tag, childBlocks, translation, rotation, boneIdentifier)
    out += u8(1)          # part of all render groups
    out += u32(1) + sgString("Practical")
    out += u32(0)         # render group
    out += u8(1)          # add to display list
    return out


# --------------------------------------------------------------------------
# CRES — a transform tree.
# --------------------------------------------------------------------------


def buildSkeleton(resourceName, bones):
    """A skeleton: a resource node, then one transform node per bone.

    `bones` is a list of (name, boneIdentifier, parentBoneIndex, translation).
    Block 0 is the resource node; bone i lives in block i + 1."""
    childrenOfBlock = {index: [] for index in range(len(bones) + 1)}
    for index, (_, _, parent, _) in enumerate(bones):
        if parent < 0:
            childrenOfBlock[0].append(index + 1)
        else:
            childrenOfBlock[parent + 1].append(index + 1)

    blockTypes = [BLOCK_RESOURCE_NODE] + [BLOCK_TRANSFORM_NODE] * len(bones)
    out = collectionHeader([], blockTypes)

    out += typeInformation("cResourceNode", BLOCK_RESOURCE_NODE, 7)
    out += u8(1)                                            # carries a tree
    out += typeInformation("cSGResource", 0xACE46235, 2)
    out += sgString(resourceName)
    out += compositionTree(resourceName, childrenOfBlock[0])
    out += noObjectReference()
    out += u32(0)                                           # skin type

    for index, (name, boneIdentifier, _, translation) in enumerate(bones):
        out += typeInformation("cTransformNode", BLOCK_TRANSFORM_NODE, 7)
        out += transformBody(name, childrenOfBlock[index + 1], translation,
                             (0.0, 0.0, 0.0, 1.0), boneIdentifier)
    return out


def buildPartTree(resourceName, shapeName):
    """A part's CRES: a resource node over one shape reference node.

    The shape is named through a FILE LINK, not by string, which is the whole
    reason a scenegraph reference is disc-global: the link carries the instance
    words a name hashes to, and the engine resolves those against the index of
    every package rather than against this one."""
    links = [(TYPE_SHPE, GROUP, instanceOf(shapeName), instanceHighOf(shapeName))]
    out = collectionHeader(links, [BLOCK_RESOURCE_NODE, BLOCK_SHAPE_REFERENCE_NODE])

    out += typeInformation("cResourceNode", BLOCK_RESOURCE_NODE, 7)
    out += u8(1)
    out += typeInformation("cSGResource", 0xACE46235, 2)
    out += sgString(resourceName)
    out += compositionTree(resourceName, [1])
    out += noObjectReference()
    out += u32(0)

    # Version 20: past 19 so it carries a shape colour, below 21 so the morph
    # names are not there. Both are read by version and getting either wrong
    # moves everything after it.
    out += typeInformation("cShapeRefNode", BLOCK_SHAPE_REFERENCE_NODE, 20)
    out += renderableNode(resourceName, [], (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0), 0x7FFFFFFF)
    out += u32(1) + objectReference(True, 0)                # the shape, by file link
    out += u32(0)                                           # display list flags
    out += u32(0)                                           # no morph references
    out += u32(0)                                           # no trailing bytes
    out += u32(0)                                           # shape colour
    return out


# --------------------------------------------------------------------------
# SHPE — names geometry NODES and binds materials to PRIMITIVES by name.
# --------------------------------------------------------------------------


def buildShape(resourceName, meshNames, materialBindings):
    out = collectionHeader([], [TYPE_SHPE])
    # Version 8: past 6 so the skipped word list is there, and at 8 the mesh
    # names may be strings rather than file links.
    out += typeInformation("cShapeFileNode", TYPE_SHPE, 8)
    out += typeInformation("cSGResource", 0xACE46235, 2)
    out += sgString(resourceName)
    out += typeInformation("cReferentNode", 0x0C0B7347, 3)
    out += objectGraphNode(resourceName)
    out += u32(0)                                           # the skipped word list

    out += u32(len(meshNames))
    for meshName in meshNames:
        out += u32(0)                                       # level of detail
        # Non-zero means "not named by reference", so the name follows as a
        # string. It reads backwards and it is what the format does.
        out += u8(1)
        out += sgString(meshName)

    out += u32(len(materialBindings))
    for (primitiveName, materialName) in materialBindings:
        out += sgString(primitiveName)
        out += sgString(materialName)
        out += u8(0)
        out += u32(0)                                       # no extra groups
        out += u32(0)
    return out


# --------------------------------------------------------------------------
# GMND — names one container, by file link.
# --------------------------------------------------------------------------


def buildGeometryNode(resourceName, containerName):
    links = [(TYPE_GMDC, GROUP, instanceOf(containerName), instanceHighOf(containerName))]
    out = collectionHeader(links, [TYPE_GMND])
    # Version 12: past 6 so it carries the ignored byte, not 11 and not 6 so
    # neither of those skips applies, and at least 10 so the twelve-byte gap
    # older nodes have is absent.
    out += typeInformation("cGeometryNode", TYPE_GMND, 12)
    out += objectGraphNode(resourceName)
    out += typeInformation("cSGResource", 0xACE46235, 2)
    out += sgString(resourceName)
    out += u8(0)
    out += objectReference(True, 0)
    return out


# --------------------------------------------------------------------------
# GMDC — the vertices, and everything hung off them.
# --------------------------------------------------------------------------

ELEMENT_POSITION = 0x5B830781
ELEMENT_NORMAL = 0x3B83078B
ELEMENT_TEXTURE = 0xBB8307AB
ELEMENT_BONE_ASSIGNMENT = 0xFBD70111
ELEMENT_BONE_WEIGHT = 0x3BD70105
ELEMENT_MORPH_MAP = 0xDCF2CFDC
ELEMENT_MORPH_DELTA = 0x5CF2CFE1

# Indices are a word wide below block version 3 and a half word from 3 up.
BLOCK_VERSION = 4


def indexArray(values):
    out = u32(len(values))
    for value in values:
        out += u16(value)
    return out


def floatElement(identifier, values, valuesPerVertex):
    formatCode = 1 if valuesPerVertex == 2 else 2
    out = u32(0) + u32(identifier) + u32(0) + u32(formatCode) + u32(0)
    out += u32(len(values) * 4)
    for value in values:
        out += f32(value)
    out += indexArray([])
    return out


def wordElement(identifier, words):
    """An element whose payload is one packed word per vertex.

    The bone assignment word packs slot nought in its LEAST significant byte.
    The morph map a few elements away packs slot nought in its MOST significant
    one. Two packed words in one container reading in opposite directions, and
    nothing in the file says so."""
    out = u32(0) + u32(identifier) + u32(0) + u32(4) + u32(0)
    out += u32(len(words) * 4)
    for word in words:
        out += u32(word)
    out += indexArray([])
    return out


def packBoneAssignment(slots):
    """Slot nought in the least significant byte. 0xFF is 'no bone'."""
    word = 0
    for index in range(4):
        bone = slots[index] if index < len(slots) else 0xFF
        word |= (bone & 0xFF) << (index * 8)
    return word


def packMorphMap(slots):
    """Slot nought in the MOST significant byte, which is the other way round."""
    word = 0
    for index in range(4):
        channel = slots[index] if index < len(slots) else 0
        word |= (channel & 0xFF) << ((3 - index) * 8)
    return word


class Component:
    """One block of vertices with its own elements, as the file lays them out."""

    def __init__(self, positions, normals, textures, boneSlots, boneWeights,
                 morphMap=None, morphDeltas=None):
        self.positions = positions
        self.normals = normals
        self.textures = textures
        self.boneSlots = boneSlots
        self.boneWeights = boneWeights
        self.morphMap = morphMap
        self.morphDeltas = morphDeltas or []

    @property
    def vertexCount(self):
        return len(self.positions) // 3


def buildContainer(resourceName, components, primitives, bindPoses, morphChannels):
    """`primitives` is a list of (componentIndex, name, faces).

    Faces are numbered from nought WITHIN their own component, which is what the
    file does and what makes a component index mean something only inside one
    container."""
    out = u32(COLLECTION_MARK) + u32(0) + u32(1) + u32(TYPE_GMDC)
    out += typeInformation("cGeometryDataContainer", TYPE_GMDC, BLOCK_VERSION)
    out += typeInformation("cSGResource", 0xACE46235, 2)
    out += shortString(resourceName)

    elements = []
    elementIndicesFor = []
    for component in components:
        indices = []
        indices.append(len(elements))
        elements.append(floatElement(ELEMENT_POSITION, component.positions, 3))
        indices.append(len(elements))
        elements.append(floatElement(ELEMENT_NORMAL, component.normals, 3))
        indices.append(len(elements))
        elements.append(floatElement(ELEMENT_TEXTURE, component.textures, 2))
        indices.append(len(elements))
        elements.append(wordElement(ELEMENT_BONE_ASSIGNMENT,
                                    [packBoneAssignment(slots) for slots in component.boneSlots]))
        indices.append(len(elements))
        # Format two: three stored weights, the fourth worked back from them.
        elements.append(floatElement(ELEMENT_BONE_WEIGHT, component.boneWeights, 3))
        if component.morphMap is not None:
            indices.append(len(elements))
            elements.append(wordElement(ELEMENT_MORPH_MAP,
                                        [packMorphMap(slots) for slots in component.morphMap]))
            for deltas in component.morphDeltas:
                indices.append(len(elements))
                elements.append(floatElement(ELEMENT_MORPH_DELTA, deltas, 3))
        elementIndicesFor.append(indices)

    out += u32(len(elements))
    for element in elements:
        out += element

    out += u32(len(components))
    for component, indices in zip(components, elementIndicesFor):
        out += indexArray(indices)
        out += u32(component.vertexCount)
        out += u32(0)
        out += indexArray([])
        out += indexArray([])
        out += indexArray([])

    out += u32(len(primitives))
    for (componentIndex, name, faces, bones) in primitives:
        out += u32(0)
        out += u32(componentIndex)
        out += shortString(name)
        out += indexArray(faces)
        out += u32(0)                                       # draw order
        out += indexArray(bones)                            # the bone list

    # The bind pose, which begins where the last primitive record ended. One
    # quaternion and one translation per bone, and it is the INVERSE bind — the
    # engine multiplies by it without inverting anything.
    out += u32(len(bindPoses))
    for (rotation, translation) in bindPoses:
        for axis in range(4):
            out += f32(rotation[axis])
        for axis in range(3):
            out += f32(translation[axis])

    # The deformation channels, which begin the moment the bind pose ends.
    # A blank first entry, always: a slot's byte of nought means "this slot
    # moves nothing", so channel nought can never be referred to and closing
    # the gap would rename every real channel.
    out += u32(len(morphChannels))
    for (groupName, channelName) in morphChannels:
        out += shortString(groupName)
        out += shortString(channelName)
    return out


# --------------------------------------------------------------------------
# TXMT and TXTR.
# --------------------------------------------------------------------------


def buildMaterial(resourceName, materialName, textureName, definitionType="StandardMaterial"):
    out = collectionHeader([], [TYPE_TXMT])
    out += typeInformation("cMaterialDefinition", TYPE_TXMT, 11)
    out += typeInformation("cSGResource", 0xACE46235, 2)
    out += sgString(resourceName)
    out += sgString(materialName)
    out += sgString(definitionType)
    out += u32(1)
    out += sgString("stdMatBaseTextureName") + sgString(textureName)
    out += u32(1)
    out += sgString(textureName)
    return out


def buildTexture(resourceName, width, height, colour):
    """One mip level of flat colour, in RGBA32 so nothing has to decode a block.

    The colour is the point: a check can assert which texture landed on which
    range by the pixel that came out, which is what the green face was."""
    out = collectionHeader([], [TYPE_TXTR])
    # Version 9: at 9 each level says whether it is a reference, which is the
    # layout a retail texture uses.
    out += typeInformation("cImageData", TYPE_TXTR, 9)
    out += typeInformation("cSGResource", 0xACE46235, 2)
    out += sgString(resourceName)
    out += u32(width) + u32(height)
    out += u32(1)                                           # RGBA32
    out += u32(1)                                           # one mip level
    out += f32(0.0)
    out += u32(1)                                           # one sub image
    out += u32(0)                                           # which is selected
    out += sgString(resourceName)                           # version > 6
    out += u32(1)                                           # levels in this sub image
    out += u8(0)                                            # not a reference
    payload = bytes(colour) * (width * height)
    out += u32(len(payload)) + payload
    out += u32(0)                                           # a colour for the whole image
    out += f32(1.0)                                         # bump scale
    return out


# --------------------------------------------------------------------------
# The catalogue: a property set naming a shape, and the key list it indexes.
# --------------------------------------------------------------------------

CPF_MAGIC = 0xCBE750E0
CPF_TYPE_INTEGER = 0xEB61E4F7
CPF_TYPE_STRING = 0x0B8BEA18


def buildSkinEntry(properties):
    """A binary property set. Integers and strings only, which is all a skin
    entry needs — and note the flat four-byte string length, which is not the
    scenegraph's rule."""
    out = u32(CPF_MAGIC) + u16(2) + u32(len(properties))
    for (name, value) in properties:
        if isinstance(value, str):
            out += u32(CPF_TYPE_STRING) + cpfString(name) + cpfString(value)
        else:
            out += u32(CPF_TYPE_INTEGER) + cpfString(name) + u32(value)
    return out


def buildKeyList(keys):
    """Version 2, which carries the instance's high half as a fourth word.

    A version 1 list read as version 2 takes the next key's type as an instance
    half and is wrong about every key after the first while still producing
    plausible numbers. The sentinel at the front is what says which."""
    out = u32(0xDEADBEEF) + u32(2) + u32(len(keys))
    for (typeIdentifier, group, instance, instanceHigh) in keys:
        out += u32(typeIdentifier) + u32(group) + u32(instance) + u32(instanceHigh)
    return out


# --------------------------------------------------------------------------
# DBPF.
# --------------------------------------------------------------------------


def buildPackage(resources):
    """resources: list of (type, group, instance, instanceHigh, bytes)."""
    header = bytearray(96)
    header[0:4] = b"DBPF"
    header[4:8] = u32(1)                                    # major version
    header[8:12] = u32(1)                                   # minor version

    body = b""
    placed = []
    offset = 96
    for (typeIdentifier, group, instance, instanceHigh, payload) in resources:
        placed.append((typeIdentifier, group, instance, instanceHigh, offset, len(payload)))
        body += payload
        offset += len(payload)

    index = b""
    for (typeIdentifier, group, instance, instanceHigh, at, size) in placed:
        index += u32(typeIdentifier) + u32(group) + u32(instance) + u32(instanceHigh)
        index += u32(at) + u32(size)

    header[0x24:0x28] = u32(len(placed))
    header[0x28:0x2C] = u32(offset)
    header[0x2C:0x30] = u32(len(index))
    return bytes(header) + body + index


# --------------------------------------------------------------------------
# The Sim itself.
# --------------------------------------------------------------------------

# Small enough to read in a log, deep enough to have a hierarchy. Bone
# identifiers are the numbers nodes carry, not positions in the node list —
# they are deliberately not 0, 1, 2 so that a reader confusing the two is
# caught rather than accidentally right.
BONES = [
    ("root",  10, -1, (0.0, 0.0, 0.0)),
    ("spine", 11,  0, (0.0, 0.5, 0.0)),
    ("head",  12,  1, (0.0, 1.0, 0.0)),
]

# The inverse bind, one per bone, in the order the primitives' bone lists index.
# Identity rotation and the negated translation, which is what the inverse of a
# pure translation is — so at rest every pair multiplies out to the identity and
# a correct skin moves nothing.
BIND_POSES = [
    ((0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0)),
    ((0.0, 0.0, 0.0, 1.0), (0.0, -0.5, 0.0)),
    ((0.0, 0.0, 0.0, 1.0), (0.0, -1.5, 0.0)),
]


def box(originX, originY, originZ, width, height, depth):
    """Eight corners and twelve triangles. Normals point out along y, which is
    enough for a fixture: what is being tested is the plumbing, not the
    shading."""
    positions = []
    normals = []
    textures = []
    for cornerY in (0.0, height):
        for cornerZ in (0.0, depth):
            for cornerX in (0.0, width):
                positions += [originX + cornerX, originY + cornerY, originZ + cornerZ]
                normals += [0.0, 1.0, 0.0]
                textures += [cornerX / max(width, 0.001), cornerZ / max(depth, 0.001)]
    faces = [
        0, 1, 2, 1, 3, 2,      # bottom
        4, 6, 5, 5, 6, 7,      # top
        0, 4, 1, 1, 4, 5,
        2, 3, 6, 3, 7, 6,
        0, 2, 4, 2, 6, 4,
        1, 5, 3, 3, 5, 7,
    ]
    return positions, normals, textures, faces


def weightedTo(vertexCount, boneIndex):
    """Every vertex on one bone, at full weight. The remaining three slots say
    'no bone' rather than bone zero, which would drag the part along with
    whatever joint happened to be first."""
    slots = [[boneIndex, 0xFF, 0xFF, 0xFF] for _ in range(vertexCount)]
    weights = []
    for _ in range(vertexCount):
        weights += [1.0, 0.0, 0.0]
    return slots, weights


def simpleComponent(geometry, boneIndex, morphChannelSlots=None, morphDelta=None):
    positions, normals, textures, faces = geometry
    vertexCount = len(positions) // 3
    slots, weights = weightedTo(vertexCount, boneIndex)
    morphMap = None
    morphDeltas = None
    if morphChannelSlots is not None:
        morphMap = [list(morphChannelSlots) for _ in range(vertexCount)]
        morphDeltas = [morphDelta * vertexCount]
    return Component(positions, normals, textures, slots, weights, morphMap, morphDeltas), faces


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output = os.path.join(root, "testAssets", "scenegraph", "sim_fixture.package")

    resources = []

    def add(typeIdentifier, name, payload):
        resources.append((typeIdentifier, GROUP, instanceOf(name), instanceHighOf(name), payload))

    # ---- the skeleton every part hangs on -------------------------------
    add(TYPE_CRES, "auskel_cres", buildSkeleton("auskel_cres", BONES))

    # ---- a part: CRES, SHPE, GMND, GMDC, and what paints it -------------
    def addPart(partName, shapeName, nodeName, containerName, components, primitives,
                bindings, morphChannels):
        add(TYPE_CRES, partName, buildPartTree(partName, shapeName))
        add(TYPE_SHPE, shapeName, buildShape(shapeName, [nodeName], bindings))
        add(TYPE_GMND, nodeName, buildGeometryNode(nodeName, containerName))
        add(TYPE_GMDC, containerName,
            buildContainer(containerName, components, primitives, BIND_POSES, morphChannels))

    def addPaint(materialName, textureName, colour, size=4):
        add(TYPE_TXMT, materialName + "_txmt",
            buildMaterial(materialName + "_txmt", materialName, textureName))
        add(TYPE_TXTR, textureName + "_txtr", buildTexture(textureName + "_txtr", size, size, colour))

    # The body: TWO components and TWO primitives with TWO materials.
    #
    # This is the shape that mattered. While every part had exactly one
    # primitive, a part index and a primitive index were the same number and
    # nothing could tell them apart — the first garment with two primitives
    # made every texture after it land one range early, and a Sim came out with
    # a green face. A fixture whose parts all had one primitive would have
    # agreed with the bug.
    torso, torsoFaces = simpleComponent(box(-0.3, 0.5, -0.15, 0.6, 0.7, 0.3), 1,
                                        morphChannelSlots=[1, 0, 0, 0],
                                        morphDelta=[0.05, 0.0, 0.0])
    legs, legFaces = simpleComponent(box(-0.25, 0.0, -0.12, 0.5, 0.5, 0.24), 0)
    addPart("amBodyNaked_cres", "ambodynaked_shpe", "amBodyNaked_tslocator_gmnd",
            "ambodynaked_gmdc",
            [torso, legs],
            [(0, "body", torsoFaces, [1]), (1, "legs", legFaces, [0])],
            [("body", "ambodynaked_torso"), ("legs", "ambodynaked_legs")],
            [("", ""), ("botmorphs", "fatbot")])
    # The tone is read off whatever the body ended up wearing — "s1" out of
    # "ambodynaked-nude-s1" — so the texture has to be named the way the disc
    # names one or the face's tone rule has nothing to match against.
    addPaint("ambodynaked_torso", "ambodynaked-nude-s1", (200, 160, 130, 255))
    addPaint("ambodynaked_legs", "ambodynaked-legs-s1", (120, 110, 160, 255))

    head, headFaces = simpleComponent(box(-0.2, 1.5, -0.2, 0.4, 0.4, 0.4), 2)
    addPart("amFace_cres", "amface_shpe", "amFace_tslocator_gmnd", "amface_gmdc",
            [head], [(0, "face", headFaces, [2])],
            [("face", "uuface_browbushy_brown")], [])
    addPaint("uuface_browbushy_brown", "uuface-browbushy-brown", (90, 60, 40, 255))
    addPaint("amface-s1", "amface-s1", (210, 170, 140, 255))

    scalp, scalpFaces = simpleComponent(box(-0.21, 1.88, -0.21, 0.42, 0.06, 0.42), 2)
    addPart("amHairBald_cres", "amhairbald_shpe", "amHairBald_tslocator_gmnd", "amhairbald_gmdc",
            [scalp], [(0, "hair", scalpFaces, [2])],
            [("hair", "amhairbald_skin_s1")], [])
    addPaint("amhairbald_skin_s1", "umhairbald-skin-s1", (200, 160, 130, 255))

    # ---- the catalogue: what this Sim can wear instead ------------------
    #
    # An entry names a shape by an INDEX into a key list beside it, and the key
    # list is matched on group and instance both. Nothing about the entry says
    # which shape; the index and the sidecar together do.
    def addGarment(entryName, outfitSlot, geometry, boneIndex, subsetName,
                   materialName, textureName, colour, morphChannels):
        shapeName = entryName + "_shpe"
        nodeName = entryName + "_gmnd"
        containerName = entryName + "_gmdc"
        component, faces = simpleComponent(geometry, boneIndex)
        add(TYPE_SHPE, shapeName,
            buildShape(shapeName, [nodeName], [(subsetName, materialName + "_asbound")]))
        add(TYPE_GMND, nodeName, buildGeometryNode(nodeName, containerName))
        add(TYPE_GMDC, containerName,
            buildContainer(containerName, [component], [(0, subsetName, faces, [boneIndex])],
                           BIND_POSES, morphChannels))
        # What the SHAPE binds is one arbitrary colourway, and it is
        # deliberately the wrong one here: the entry's own override names the
        # right one, and a check can tell which arrived by the pixel.
        addPaint(materialName + "_asbound", textureName + "-asbound", (255, 0, 255, 255))
        addPaint(materialName, textureName, colour)

        # The entry and its sidecar. Key 0 is the shape and key 1 the material,
        # so shapekeyidx and override0resourcekeyidx point at different things
        # and swapping them cannot pass.
        keys = [
            (TYPE_SHPE, GROUP, instanceOf(shapeName), instanceHighOf(shapeName)),
            (TYPE_TXMT, GROUP, instanceOf(materialName + "_txmt"),
             instanceHighOf(materialName + "_txmt")),
        ]
        add(TYPE_SKIN_ENTRY, entryName, buildSkinEntry([
            ("type", "skin"),
            ("name", entryName),
            ("outfit", outfitSlot),
            ("category", 0x07),
            ("age", 0x10),
            ("gender", 0x02),
            ("species", 0x01),
            ("shapekeyidx", 0),
            ("resourcekeyidx", 0),
            ("numoverrides", 1),
            ("override0subset", subsetName),
            ("override0resourcekeyidx", 1),
            ("override0shape", 0),
        ]))
        # Matched on group AND instance, so the list must share both with the
        # entry that indexes it.
        resources.append((TYPE_KEY_LIST, GROUP, instanceOf(entryName),
                          instanceHighOf(entryName), buildKeyList(keys)))

    addGarment("ambodyoveralls_blue", 0x08,
               box(-0.32, 0.0, -0.17, 0.64, 1.2, 0.34), 1, "body",
               "ambodyoveralls_blue", "ambodyoveralls-blue", (40, 70, 200, 255),
               [("", ""), ("botmorphs", "fatbot")])
    addGarment("amtopjersey_green", 0x04,
               box(-0.33, 0.5, -0.18, 0.66, 0.72, 0.36), 1, "top",
               "amtopjersey_green", "amtopjersey-green", (40, 180, 70, 255),
               [("", ""), ("topmorphs", "fattop")])
    addGarment("ambottomshorts_red", 0x10,
               box(-0.27, 0.0, -0.14, 0.54, 0.52, 0.28), 0, "bottom",
               "ambottomshorts_red", "ambottomshorts-red", (200, 50, 50, 255),
               [("", ""), ("botmorphs", "fatbot")])
    # Two faces: the wrong tone first, so the walk meets it first and the right
    # one has to displace it. A fixture that only offered the right one would
    # pass whether or not the tone rule existed.
    addGarment("CASIE_amface_s9", 0x02,
               box(-0.2, 1.5, -0.2, 0.4, 0.4, 0.4), 2, "face",
               "amface_s9", "amface-s9", (10, 200, 10, 255),
               [])
    addGarment("CASIE_amface_s1", 0x02,
               box(-0.2, 1.5, -0.2, 0.4, 0.4, 0.4), 2, "face",
               "amface_s1_worn", "amface-s1-worn", (210, 170, 140, 255),
               [])
    addGarment("amhairshort_brown", 0x01,
               box(-0.22, 1.86, -0.22, 0.44, 0.1, 0.44), 2, "hair",
               "amhairshort_brown", "amhairshort-brown", (80, 50, 30, 255),
               [])
    # A child's body, which must be refused: it is authored for a skeleton this
    # Sim has not got, and taking it would resolve perfectly and come apart.
    addGarment("cmbodyromper_yellow", 0x08,
               box(-0.2, 0.0, -0.1, 0.4, 0.8, 0.2), 1, "body",
               "cmbodyromper_yellow", "cmbodyromper-yellow", (230, 210, 60, 255),
               [("", ""), ("botmorphs", "fatbot")])

    data = buildPackage(resources)
    with open(output, "wb") as handle:
        handle.write(data)
    sys.stderr.write("wrote %s — %d resources, %d bytes\n"
                     % (output, len(resources), len(data)))


if __name__ == "__main__":
    main()
