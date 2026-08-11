import { u8, u32, sgString } from "../binary/littleEndian.ts";
import { instanceOf, instanceHighOf } from "../../lib/hash.ts";
import { BLOCK_RESOURCE_NODE, BLOCK_TRANSFORM_NODE, BLOCK_SHAPE_REFERENCE_NODE, TYPE_SHPE, GROUP, type Bone } from "./types.ts";
import {
    typeInformation,
    noObjectReference,
    collectionHeader,
    compositionTree,
    transformBody,
    renderableNode,
    objectReference,
} from "./primitives.ts";

// A skeleton: a resource node, then one transform node per bone.
//
// Block 0 is the resource node; bone i lives in block i + 1.
export function buildSkeleton(resourceName: string, bones: readonly Bone[]): Buffer {
    const childrenOfBlock = new Map<number, number[]>();
    for (let index = 0; index <= bones.length; index += 1) {
        childrenOfBlock.set(index, []);
    }
    bones.forEach(([, , parent], index) => {
        const owner = parent < 0 ? 0 : parent + 1;
        childrenOfBlock.get(owner)!.push(index + 1);
    });

    const blockTypes = [BLOCK_RESOURCE_NODE, ...bones.map(() => BLOCK_TRANSFORM_NODE)];
    const parts: Buffer[] = [collectionHeader([], blockTypes)];

    parts.push(typeInformation("cResourceNode", BLOCK_RESOURCE_NODE, 7));
    parts.push(u8(1)); // carries a tree
    parts.push(typeInformation("cSGResource", 0xace46235, 2));
    parts.push(sgString(resourceName));
    parts.push(compositionTree(resourceName, childrenOfBlock.get(0)!));
    parts.push(noObjectReference());
    parts.push(u32(0)); // skin type

    bones.forEach(([name, boneIdentifier, , translation], index) => {
        parts.push(typeInformation("cTransformNode", BLOCK_TRANSFORM_NODE, 7));
        parts.push(
            transformBody(name, childrenOfBlock.get(index + 1)!, translation, [0.0, 0.0, 0.0, 1.0], boneIdentifier),
        );
    });
    return Buffer.concat(parts);
}

// A part's CRES: a resource node over one shape reference node.
//
// The shape is named through a FILE LINK, not by string, which is the whole
// reason a scenegraph reference is disc-global: the link carries the instance
// words a name hashes to, and the engine resolves those against the index of
// every package rather than against this one.
export function buildPartTree(resourceName: string, shapeName: string): Buffer {
    const links = [[TYPE_SHPE, GROUP, instanceOf(shapeName), instanceHighOf(shapeName)] as const];
    const parts: Buffer[] = [collectionHeader(links, [BLOCK_RESOURCE_NODE, BLOCK_SHAPE_REFERENCE_NODE])];

    parts.push(typeInformation("cResourceNode", BLOCK_RESOURCE_NODE, 7));
    parts.push(u8(1));
    parts.push(typeInformation("cSGResource", 0xace46235, 2));
    parts.push(sgString(resourceName));
    parts.push(compositionTree(resourceName, [1]));
    parts.push(noObjectReference());
    parts.push(u32(0));

    // Version 20: past 19 so it carries a shape colour, below 21 so the morph
    // names are not there. Both are read by version and getting either wrong
    // moves everything after it.
    parts.push(typeInformation("cShapeRefNode", BLOCK_SHAPE_REFERENCE_NODE, 20));
    parts.push(renderableNode(resourceName, [], [0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0], 0x7fffffff));
    parts.push(u32(1), objectReference(true, 0)); // the shape, by file link
    parts.push(u32(0)); // display list flags
    parts.push(u32(0)); // no morph references
    parts.push(u32(0)); // no trailing bytes
    parts.push(u32(0)); // shape colour
    return Buffer.concat(parts);
}
