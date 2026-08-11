import { u32, u16, cpfString } from "../binary/littleEndian.ts";
import type { ResourceLink } from "./types.ts";

const CPF_MAGIC = 0xcbe750e0;
const CPF_TYPE_INTEGER = 0xeb61e4f7;
const CPF_TYPE_STRING = 0x0b8bea18;

export type SkinEntryProperty = readonly [name: string, value: string | number];

// A binary property set. Integers and strings only, which is all a skin
// entry needs — and note the flat four-byte string length, which is not the
// scenegraph's rule.
export function buildSkinEntry(properties: readonly SkinEntryProperty[]): Buffer {
    const parts: Buffer[] = [u32(CPF_MAGIC), u16(2), u32(properties.length)];
    for (const [name, value] of properties) {
        if (typeof value === "string") {
            parts.push(u32(CPF_TYPE_STRING), cpfString(name), cpfString(value));
        } else {
            parts.push(u32(CPF_TYPE_INTEGER), cpfString(name), u32(value));
        }
    }
    return Buffer.concat(parts);
}

// Version 2, which carries the instance's high half as a fourth word.
//
// A version 1 list read as version 2 takes the next key's type as an
// instance half and is wrong about every key after the first while still
// producing plausible numbers. The sentinel at the front is what says which.
export function buildKeyList(keys: readonly ResourceLink[]): Buffer {
    const parts: Buffer[] = [u32(0xdeadbeef), u32(2), u32(keys.length)];
    for (const [typeIdentifier, group, instance, instanceHigh] of keys) {
        parts.push(u32(typeIdentifier), u32(group), u32(instance), u32(instanceHigh));
    }
    return Buffer.concat(parts);
}
