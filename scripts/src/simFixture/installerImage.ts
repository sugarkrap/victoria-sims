// Builds a fake TSData.exe: a real program at the front, and an archive
// appended past the end of it — the shape the disc's own TSData.exe has,
// three sections ending at 0xCE00 then 2.7 gibibytes beginning "Rar!". The
// front holds nothing that identifies an archive, which is why the engine
// reads the section table to find out where the front stops.
//
// Two entries, one stored and one packed, because the difference between
// them decides everything downstream. A stored entry is a range of the file
// and can go straight to the package reader; a packed one cannot.
const HEADER_AT = 0x80;
const SECTION_TABLE_AT = HEADER_AT + 4 + 20 + 224;
const PROGRAM_ENDS_AT = 0x200;

function fileHeader(chunks: Buffer[], name: string, data: Buffer, unpackedSize: number, method: number): void {
    const nameBytes = Buffer.from(name, "utf8");
    const headerSize = 32 + nameBytes.length;
    const header = Buffer.alloc(headerSize);
    header[2] = 0x74;
    header.writeUInt16LE(0x8000, 3);
    header.writeUInt16LE(headerSize, 5);
    header.writeUInt32LE(data.length, 7);
    header.writeUInt32LE(unpackedSize, 11);
    header[24] = 20; // version needed
    header[25] = method; // 0x30 stored
    header.writeUInt16LE(nameBytes.length, 26);
    nameBytes.copy(header, 32);
    chunks.push(header, data);
}

export function buildFakeInstaller(storedPackageBytes: Buffer): Buffer {
    // Delphi's stub, which is what separates an installer from every other
    // program on a disc: Microsoft's linker writes MZ, Borland's writes MZP.
    const image = Buffer.alloc(PROGRAM_ENDS_AT);
    image.set(Buffer.from("MZP\0", "latin1"), 0);
    image.writeUInt32LE(HEADER_AT, 0x3c);
    image.set(Buffer.from("PE\0\0", "latin1"), HEADER_AT);
    // Two bytes of machine, then the section count; twelve bytes of symbol
    // table fields, then the optional header's size.
    image.writeUInt16LE(1, HEADER_AT + 4 + 2);
    image.writeUInt16LE(224, HEADER_AT + 4 + 16);
    // That section covers the program and nothing after it.
    image.writeUInt32LE(PROGRAM_ENDS_AT, SECTION_TABLE_AT + 16);
    image.writeUInt32LE(0, SECTION_TABLE_AT + 20);

    const chunks: Buffer[] = [image];

    // Rar!, fourth generation. The fifth writes an eighth byte and is a
    // different format; the reader refuses that one by name rather than
    // misreading it.
    chunks.push(Buffer.from([0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00]));

    // The archive header block: a length and nothing this reader needs.
    const archiveHeader = Buffer.alloc(13);
    archiveHeader[2] = 0x73;
    archiveHeader.writeUInt16LE(13, 5);
    chunks.push(archiveHeader);

    // Stored, and its data really is a package. Mounting it means adding the
    // containing file's own offset to the entry's, and if that addition is
    // wrong the mounted file starts somewhere that is not a package — which
    // is exactly what the engine checks once it has mounted one.
    fileHeader(chunks, "TSData/Res/Materials/mounted.package", storedPackageBytes, storedPackageBytes.length, 0x30);

    // Packed, whose sizes differ — which is the other thing that says so.
    fileHeader(chunks, "TSData/Res/Sims3D/Sims02.package", Buffer.alloc(8), 32, 0x35);

    // The end block a real archive closes with. Without it the walk runs off
    // the last entry into whatever follows and reports it as damage.
    const endBlock = Buffer.alloc(7);
    endBlock[2] = 0x7b;
    endBlock.writeUInt16LE(7, 5);
    chunks.push(endBlock);

    return Buffer.concat(chunks);
}
