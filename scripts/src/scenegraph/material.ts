import { u8, u32, f32, sgString } from "../binary/littleEndian.ts";
import { TYPE_TXMT, TYPE_TXTR, type Colour } from "./types.ts";
import { typeInformation, collectionHeader } from "./primitives.ts";

export function buildMaterial(
    resourceName: string,
    materialName: string,
    textureName: string,
    definitionType = "StandardMaterial",
): Buffer {
    const parts: Buffer[] = [collectionHeader([], [TYPE_TXMT])];
    parts.push(typeInformation("cMaterialDefinition", TYPE_TXMT, 11));
    parts.push(typeInformation("cSGResource", 0xace46235, 2));
    parts.push(sgString(resourceName));
    parts.push(sgString(materialName));
    parts.push(sgString(definitionType));
    parts.push(u32(1));
    parts.push(sgString("stdMatBaseTextureName"), sgString(textureName));
    parts.push(u32(1));
    parts.push(sgString(textureName));
    return Buffer.concat(parts);
}

// One mip level of flat colour, in RGBA32 so nothing has to decode a block.
//
// The colour is the point: a check can assert which texture landed on which
// range by the pixel that came out, which is what the green face was.
export function buildTexture(resourceName: string, width: number, height: number, colour: Colour): Buffer {
    const parts: Buffer[] = [collectionHeader([], [TYPE_TXTR])];
    // Version 9: at 9 each level says whether it is a reference, which is the
    // layout a retail texture uses.
    parts.push(typeInformation("cImageData", TYPE_TXTR, 9));
    parts.push(typeInformation("cSGResource", 0xace46235, 2));
    parts.push(sgString(resourceName));
    parts.push(u32(width), u32(height));
    parts.push(u32(1)); // RGBA32
    parts.push(u32(1)); // one mip level
    parts.push(f32(0.0));
    parts.push(u32(1)); // one sub image
    parts.push(u32(0)); // which is selected
    parts.push(sgString(resourceName)); // version > 6
    parts.push(u32(1)); // levels in this sub image
    parts.push(u8(0)); // not a reference
    const payload = Buffer.concat(Array(width * height).fill(Buffer.from(colour)) as Buffer[]);
    parts.push(u32(payload.length), payload);
    parts.push(u32(0)); // a colour for the whole image
    parts.push(f32(1.0)); // bump scale
    return Buffer.concat(parts);
}
