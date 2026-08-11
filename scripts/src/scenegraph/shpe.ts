import { u8, u32, sgString } from "../binary/littleEndian.ts";
import { TYPE_SHPE } from "./types.ts";
import { typeInformation, collectionHeader, objectGraphNode } from "./primitives.ts";

export type MaterialBinding = readonly [primitiveName: string, materialName: string];

// SHPE — names geometry NODES and binds materials to PRIMITIVES by name.
export function buildShape(resourceName: string, meshNames: readonly string[], materialBindings: readonly MaterialBinding[]): Buffer {
    const parts: Buffer[] = [collectionHeader([], [TYPE_SHPE])];
    // Version 8: past 6 so the skipped word list is there, and at 8 the mesh
    // names may be strings rather than file links.
    parts.push(typeInformation("cShapeFileNode", TYPE_SHPE, 8));
    parts.push(typeInformation("cSGResource", 0xace46235, 2));
    parts.push(sgString(resourceName));
    parts.push(typeInformation("cReferentNode", 0x0c0b7347, 3));
    parts.push(objectGraphNode(resourceName));
    parts.push(u32(0)); // the skipped word list

    parts.push(u32(meshNames.length));
    for (const meshName of meshNames) {
        parts.push(u32(0)); // level of detail
        // Non-zero means "not named by reference", so the name follows as a
        // string. It reads backwards and it is what the format does.
        parts.push(u8(1));
        parts.push(sgString(meshName));
    }

    parts.push(u32(materialBindings.length));
    for (const [primitiveName, materialName] of materialBindings) {
        parts.push(sgString(primitiveName));
        parts.push(sgString(materialName));
        parts.push(u8(0));
        parts.push(u32(0)); // no extra groups
        parts.push(u32(0));
    }
    return Buffer.concat(parts);
}
