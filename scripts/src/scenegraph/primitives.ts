import { u8, u32, f32, sgString } from "../binary/littleEndian.ts";
import { BLOCK_TRANSFORM_NODE, COLLECTION_MARK, type ResourceLink, type Vec3, type Quat } from "./types.ts";

export function typeInformation(name: string, identifier: number, version: number): Buffer {
    return Buffer.concat([sgString(name), u32(identifier), u32(version)]);
}

// THREE fields, not two: present, then kind, then the index.
//
// The first byte says whether there is a reference at all — a nought there
// and the record stops, one byte long. Only then comes the kind: nought means
// a block in this same collection, anything else means one of the file links.
// Writing it as a kind byte and an index, which is what it looks like from
// the reader's second half, makes every reference one byte short and moves
// everything after it. A CRES then reads its first block correctly and takes
// the middle of the second for a block type.
export function objectReference(external: boolean, index: number): Buffer {
    return Buffer.concat([u8(1), u8(external ? 1 : 0), u32(index)]);
}

export function noObjectReference(): Buffer {
    return u8(0);
}

// The mark, the file links, and the list of block types.
//
// Links are written group, instance, high instance, TYPE — which is not the
// order a package index entry uses, and writing them in index order produces
// a key that finds nothing.
export function collectionHeader(links: readonly ResourceLink[], blockTypes: readonly number[]): Buffer {
    const parts: Buffer[] = [u32(COLLECTION_MARK), u32(links.length)];
    for (const [typeIdentifier, group, instance, instanceHigh] of links) {
        parts.push(u32(group), u32(instance), u32(instanceHigh), u32(typeIdentifier));
    }
    parts.push(u32(blockTypes.length));
    for (const blockType of blockTypes) {
        parts.push(u32(blockType));
    }
    return Buffer.concat(parts);
}

// cObjectGraphNode at version 4, which is the version that carries a tag.
export function objectGraphNode(tag: string): Buffer {
    return Buffer.concat([typeInformation("cObjectGraphNode", 0x0c0b7347, 4), u32(0), sgString(tag)]);
}

// cCompositionTreeNode: a graph node, then the blocks hanging below it.
export function compositionTree(tag: string, childBlocks: readonly number[]): Buffer {
    const parts: Buffer[] = [typeInformation("cCompositionTreeNode", 0x7f888f27, 11), objectGraphNode(tag)];
    parts.push(u32(childBlocks.length));
    for (const child of childBlocks) {
        parts.push(objectReference(false, child));
    }
    return Buffer.concat(parts);
}

export function transformBody(
    tag: string,
    childBlocks: readonly number[],
    translation: Vec3,
    rotation: Quat,
    boneIdentifier: number,
): Buffer {
    const parts: Buffer[] = [compositionTree(tag, childBlocks)];
    for (let axis = 0; axis < 3; axis += 1) {
        parts.push(f32(translation[axis]!));
    }
    for (let axis = 0; axis < 4; axis += 1) {
        parts.push(f32(rotation[axis]!));
    }
    parts.push(u32(boneIdentifier));
    return Buffer.concat(parts);
}

export function boundedNode(
    tag: string,
    childBlocks: readonly number[],
    translation: Vec3,
    rotation: Quat,
    boneIdentifier: number,
): Buffer {
    return Buffer.concat([
        typeInformation("cBoundedNode", 0xe9075bc5, 5),
        typeInformation("cTransformNode", BLOCK_TRANSFORM_NODE, 7),
        transformBody(tag, childBlocks, translation, rotation, boneIdentifier),
        u8(0),
    ]);
}

export function renderableNode(
    tag: string,
    childBlocks: readonly number[],
    translation: Vec3,
    rotation: Quat,
    boneIdentifier: number,
): Buffer {
    return Buffer.concat([
        typeInformation("cRenderableNode", 0xe519c933, 5),
        boundedNode(tag, childBlocks, translation, rotation, boneIdentifier),
        u8(1), // part of all render groups
        u32(1),
        sgString("Practical"),
        u32(0), // render group
        u8(1), // add to display list
    ]);
}
