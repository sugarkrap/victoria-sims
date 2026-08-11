import { u16, u32, f32, shortString } from "../binary/littleEndian.ts";
import { TYPE_GMDC, COLLECTION_MARK, type BindPose } from "./types.ts";
import { typeInformation } from "./primitives.ts";

const ELEMENT_POSITION = 0x5b830781;
const ELEMENT_NORMAL = 0x3b83078b;
const ELEMENT_TEXTURE = 0xbb8307ab;
const ELEMENT_BONE_ASSIGNMENT = 0xfbd70111;
const ELEMENT_BONE_WEIGHT = 0x3bd70105;
const ELEMENT_MORPH_MAP = 0xdcf2cfdc;
const ELEMENT_MORPH_DELTA = 0x5cf2cfe1;

// Indices are a word wide below block version 3 and a half word from 3 up.
const BLOCK_VERSION = 4;

export function indexArray(values: readonly number[]): Buffer {
    return Buffer.concat([u32(values.length), ...values.map((v) => u16(v))]);
}

function floatElement(identifier: number, values: readonly number[], valuesPerVertex: number): Buffer {
    const formatCode = valuesPerVertex === 2 ? 1 : 2;
    const parts: Buffer[] = [u32(0), u32(identifier), u32(0), u32(formatCode), u32(0)];
    parts.push(u32(values.length * 4));
    for (const value of values) {
        parts.push(f32(value));
    }
    parts.push(indexArray([]));
    return Buffer.concat(parts);
}

// An element whose payload is one packed word per vertex.
//
// The bone assignment word packs slot nought in its LEAST significant byte.
// The morph map a few elements away packs slot nought in its MOST significant
// one. Two packed words in one container reading in opposite directions, and
// nothing in the file says so.
function wordElement(identifier: number, words: readonly number[]): Buffer {
    const parts: Buffer[] = [u32(0), u32(identifier), u32(0), u32(4), u32(0)];
    parts.push(u32(words.length * 4));
    for (const word of words) {
        parts.push(u32(word));
    }
    parts.push(indexArray([]));
    return Buffer.concat(parts);
}

// Slot nought in the least significant byte. 0xFF is 'no bone'.
export function packBoneAssignment(slots: readonly number[]): number {
    let word = 0;
    for (let index = 0; index < 4; index += 1) {
        const bone = index < slots.length ? slots[index]! : 0xff;
        word |= (bone & 0xff) << (index * 8);
    }
    return word >>> 0;
}

// Slot nought in the MOST significant byte, which is the other way round.
export function packMorphMap(slots: readonly number[]): number {
    let word = 0;
    for (let index = 0; index < 4; index += 1) {
        const channel = index < slots.length ? slots[index]! : 0;
        word |= (channel & 0xff) << ((3 - index) * 8);
    }
    return word >>> 0;
}

// One block of vertices with its own elements, as the file lays them out.
export class Component {
    positions: readonly number[];
    normals: readonly number[];
    textures: readonly number[];
    boneSlots: ReadonlyArray<readonly number[]>;
    boneWeights: readonly number[];
    morphMap: ReadonlyArray<readonly number[]> | null;
    morphDeltas: ReadonlyArray<readonly number[]>;

    constructor(
        positions: readonly number[],
        normals: readonly number[],
        textures: readonly number[],
        boneSlots: ReadonlyArray<readonly number[]>,
        boneWeights: readonly number[],
        morphMap: ReadonlyArray<readonly number[]> | null = null,
        morphDeltas: ReadonlyArray<readonly number[]> | null = null,
    ) {
        this.positions = positions;
        this.normals = normals;
        this.textures = textures;
        this.boneSlots = boneSlots;
        this.boneWeights = boneWeights;
        this.morphMap = morphMap;
        this.morphDeltas = morphDeltas ?? [];
    }

    get vertexCount(): number {
        return Math.floor(this.positions.length / 3);
    }
}

export type Primitive = readonly [componentIndex: number, name: string, faces: readonly number[], bones: readonly number[]];
export type MorphChannel = readonly [groupName: string, channelName: string];

// GMDC — the vertices, and everything hung off them.
//
// Faces are numbered from nought WITHIN their own component, which is what
// the file does and what makes a component index mean something only inside
// one container.
export function buildContainer(
    resourceName: string,
    components: readonly Component[],
    primitives: readonly Primitive[],
    bindPoses: readonly BindPose[],
    morphChannels: readonly MorphChannel[],
): Buffer {
    const parts: Buffer[] = [u32(COLLECTION_MARK), u32(0), u32(1), u32(TYPE_GMDC)];
    parts.push(typeInformation("cGeometryDataContainer", TYPE_GMDC, BLOCK_VERSION));
    parts.push(typeInformation("cSGResource", 0xace46235, 2));
    parts.push(shortString(resourceName));

    const elements: Buffer[] = [];
    const elementIndicesFor: number[][] = [];
    for (const component of components) {
        const indices: number[] = [];
        indices.push(elements.length);
        elements.push(floatElement(ELEMENT_POSITION, component.positions, 3));
        indices.push(elements.length);
        elements.push(floatElement(ELEMENT_NORMAL, component.normals, 3));
        indices.push(elements.length);
        elements.push(floatElement(ELEMENT_TEXTURE, component.textures, 2));
        indices.push(elements.length);
        elements.push(
            wordElement(ELEMENT_BONE_ASSIGNMENT, component.boneSlots.map((slots) => packBoneAssignment(slots))),
        );
        indices.push(elements.length);
        // Format two: three stored weights, the fourth worked back from them.
        elements.push(floatElement(ELEMENT_BONE_WEIGHT, component.boneWeights, 3));
        if (component.morphMap !== null) {
            indices.push(elements.length);
            elements.push(wordElement(ELEMENT_MORPH_MAP, component.morphMap.map((slots) => packMorphMap(slots))));
            for (const deltas of component.morphDeltas) {
                indices.push(elements.length);
                elements.push(floatElement(ELEMENT_MORPH_DELTA, deltas, 3));
            }
        }
        elementIndicesFor.push(indices);
    }

    parts.push(u32(elements.length));
    for (const element of elements) {
        parts.push(element);
    }

    parts.push(u32(components.length));
    components.forEach((component, i) => {
        parts.push(indexArray(elementIndicesFor[i]!));
        parts.push(u32(component.vertexCount));
        parts.push(u32(0));
        parts.push(indexArray([]));
        parts.push(indexArray([]));
        parts.push(indexArray([]));
    });

    parts.push(u32(primitives.length));
    for (const [componentIndex, name, faces, bones] of primitives) {
        parts.push(u32(0));
        parts.push(u32(componentIndex));
        parts.push(shortString(name));
        parts.push(indexArray(faces));
        parts.push(u32(0)); // draw order
        parts.push(indexArray(bones)); // the bone list
    }

    // The bind pose, which begins where the last primitive record ended. One
    // quaternion and one translation per bone, and it is the INVERSE bind —
    // the engine multiplies by it without inverting anything.
    parts.push(u32(bindPoses.length));
    for (const [rotation, translation] of bindPoses) {
        for (let axis = 0; axis < 4; axis += 1) {
            parts.push(f32(rotation[axis]!));
        }
        for (let axis = 0; axis < 3; axis += 1) {
            parts.push(f32(translation[axis]!));
        }
    }

    // The deformation channels, which begin the moment the bind pose ends.
    // A blank first entry, always: a slot's byte of nought means "this slot
    // moves nothing", so channel nought can never be referred to and closing
    // the gap would rename every real channel.
    parts.push(u32(morphChannels.length));
    for (const [groupName, channelName] of morphChannels) {
        parts.push(shortString(groupName));
        parts.push(shortString(channelName));
    }
    return Buffer.concat(parts);
}
