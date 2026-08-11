import { u8, sgString } from "../binary/littleEndian.ts";
import { instanceOf, instanceHighOf } from "../../lib/hash.ts";
import { TYPE_GMND, TYPE_GMDC, GROUP } from "./types.ts";
import { typeInformation, collectionHeader, objectGraphNode, objectReference } from "./primitives.ts";

// GMND — names one container, by file link.
export function buildGeometryNode(resourceName: string, containerName: string): Buffer {
    const links = [[TYPE_GMDC, GROUP, instanceOf(containerName), instanceHighOf(containerName)] as const];
    const parts: Buffer[] = [collectionHeader(links, [TYPE_GMND])];
    // Version 12: past 6 so it carries the ignored byte, not 11 and not 6 so
    // neither of those skips applies, and at least 10 so the twelve-byte gap
    // older nodes have is absent.
    parts.push(typeInformation("cGeometryNode", TYPE_GMND, 12));
    parts.push(objectGraphNode(resourceName));
    parts.push(typeInformation("cSGResource", 0xace46235, 2));
    parts.push(sgString(resourceName));
    parts.push(u8(0));
    parts.push(objectReference(true, 0));
    return Buffer.concat(parts);
}
