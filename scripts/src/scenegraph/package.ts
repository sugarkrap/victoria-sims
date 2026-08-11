// DBPF — writer side. (scripts/lib/dbpf.ts is the reader; the two never
// share code, since a writer and a reader disagreeing is exactly the bug a
// fixture like this exists to catch.)

export type PackagedResource = readonly [
    typeIdentifier: number,
    group: number,
    instance: number,
    instanceHigh: number,
    payload: Buffer,
];

export function buildPackage(resources: readonly PackagedResource[]): Buffer {
    const header = Buffer.alloc(96);
    header.write("DBPF", 0, "ascii");
    header.writeUInt32LE(1, 4); // major version
    header.writeUInt32LE(1, 8); // minor version

    const bodyParts: Buffer[] = [];
    const placed: Array<[number, number, number, number, number, number]> = [];
    let offset = 96;
    for (const [typeIdentifier, group, instance, instanceHigh, payload] of resources) {
        placed.push([typeIdentifier, group, instance, instanceHigh, offset, payload.length]);
        bodyParts.push(payload);
        offset += payload.length;
    }

    const indexParts: Buffer[] = [];
    for (const [typeIdentifier, group, instance, instanceHigh, at, size] of placed) {
        const entry = Buffer.alloc(24);
        entry.writeUInt32LE(typeIdentifier, 0);
        entry.writeUInt32LE(group, 4);
        entry.writeUInt32LE(instance, 8);
        entry.writeUInt32LE(instanceHigh, 12);
        entry.writeUInt32LE(at, 16);
        entry.writeUInt32LE(size, 20);
        indexParts.push(entry);
    }
    const index = Buffer.concat(indexParts);

    header.writeUInt32LE(placed.length, 0x24);
    header.writeUInt32LE(offset, 0x28);
    header.writeUInt32LE(index.length, 0x2c);
    return Buffer.concat([header, ...bodyParts, index]);
}
