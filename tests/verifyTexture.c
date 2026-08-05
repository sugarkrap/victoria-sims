/* Reads the texture fixtures and decodes them.

   Two things about a TXTR are the opposite of what you would guess, and both
   are checked here rather than assumed.

   The mip levels are stored smallest first, so the last entry is the full size
   image. A reader that numbers them the way a graphics API does gets the
   smallest level and an image four pixels across.

   The largest level is often not in the resource. It is a named reference to a
   LIFO holding just that level, which is why one fixture here is a 256 by 256
   texture whose TXTR is eleven kilobytes. That is not truncation, and reporting
   it as such would send someone looking for a bug in the package reader.

   Both fixture textures were decoded by hand before this reader existed; the
   sizes below come from the file, not from running this and writing down what
   it said. */

#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"
#include "victoria/material.h"
#include "victoria/textureDecode.h"
#include "victoria/textureReader.h"

#define FILE_BUFFER_CAPACITY (1UL * 1024UL * 1024UL)
#define ARENA_CAPACITY (1UL * 1024UL * 1024UL)
#define PIXEL_CAPACITY (256UL * 256UL * 4UL)

static Unsigned8 fileBuffer[FILE_BUFFER_CAPACITY];
static Unsigned8 arenaStorage[ARENA_CAPACITY];
static Unsigned8 pixels[PIXEL_CAPACITY];

static Integer32 failureCount = 0;

int main(void)
{
    MemoryArena arena;
    Package package;
    MemorySize sizeInBytes;
    FILE *inputFile;
    Unsigned32 index;
    Unsigned32 texturesRead = 0U;
    Unsigned32 referencedElsewhere = 0U;

    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);

    inputFile = fopen("testAssets/scenegraph/textures.package", "rb");
    if (inputFile == NULL)
    {
        printf("FAIL  cannot open the texture fixture\n");
        return 1;
    }
    sizeInBytes = (MemorySize)fread(fileBuffer, 1, FILE_BUFFER_CAPACITY, inputFile);
    fclose(inputFile);

    if (packageReaderOpen(&package, fileBuffer, sizeInBytes, &arena) != PACKAGE_READ_OK)
    {
        printf("FAIL  the texture fixture would not open\n");
        return 1;
    }

    printf("-- reading every texture in the fixture --\n");
    for (index = 0U; index < package.resourceCount; index++)
    {
        const PackageResource *resource = &package.resources[index];
        TextureDescription texture;
        TextureReadResult result;

        if (resource->key.typeIdentifier != (Unsigned32)PACKAGE_TYPE_TXTR)
        {
            continue;
        }
        result = textureReaderOpen(&texture, packageReaderGetResourceBytes(&package, resource),
                                   (MemorySize)resource->sizeInBytes);
        if (result != TEXTURE_READ_OK)
        {
            printf("FAIL  a texture would not read: %s\n", textureReadResultGetName(result));
            failureCount += 1;
            continue;
        }
        texturesRead++;
        printf("  %-28s %4dx%-4d %-6s %u mips, level %dx%d in %lu bytes%s\n", texture.resourceName,
               (int)texture.width, (int)texture.height, textureFormatGetName(texture.format),
               (unsigned)texture.mipCount, (int)texture.levelWidth, (int)texture.levelHeight,
               (unsigned long)texture.byteCount,
               texture.largestIsElsewhere ? ", top level elsewhere" : "");

        /* The level's byte count has to match its dimensions exactly. Getting
           the level order backwards gives a count that is wrong by a factor of
           four or more, which this catches and eyeballing would not. */
        checkThat(&failureCount, "the level's size matches its dimensions",
                  texture.byteCount == textureFormatGetLevelBytes(texture.format, texture.levelWidth,
                                                                  texture.levelHeight));
        if (texture.largestIsElsewhere)
        {
            referencedElsewhere++;
            checkThat(&failureCount, "a referenced level names the resource holding it",
                      texture.lifoName[0] != '\0');
        }

        {
            TextureDecodeResult decoded =
                textureDecodeLevel(pixels, sizeof(pixels), texture.bytes, texture.byteCount,
                                   texture.format, texture.levelWidth, texture.levelHeight);

            checkThat(&failureCount, "and it decodes", decoded == TEXTURE_DECODE_OK);
            if (decoded != TEXTURE_DECODE_OK)
            {
                printf("  decode: %s\n", textureDecodeResultGetName(decoded));
            }
        }
    }

    checkThat(&failureCount, "the fixture holds four textures", texturesRead == 4U);
    /* One of them keeps its top level in a LIFO and one does not, which is the
       pair that makes the distinction testable at all. */
    checkThat(&failureCount, "one of which keeps its largest level elsewhere",
              referencedElsewhere == 1U);

    printf("\n-- following a reference to the level that is elsewhere --\n");
    {
        /* The whole point of the reference. A face read without following it is
           128 by 128 when the disc holds 512 by 512 — not wrong, just a quarter
           of the resolution, and silently. */
        char referencedName[RESOURCE_NAME_LIMIT];
        Integer32 referencedFrom = 0;
        TextureFormat referencedFormat = TEXTURE_FORMAT_UNKNOWN;
        Boolean foundReference = BOOLEAN_FALSE;

        referencedName[0] = '\0';
        for (index = 0U; index < package.resourceCount && !foundReference; index++)
        {
            const PackageResource *resource = &package.resources[index];
            TextureDescription texture;

            if (resource->key.typeIdentifier != (Unsigned32)PACKAGE_TYPE_TXTR ||
                textureReaderOpen(&texture, packageReaderGetResourceBytes(&package, resource),
                                  (MemorySize)resource->sizeInBytes) != TEXTURE_READ_OK ||
                !texture.largestIsElsewhere)
            {
                continue;
            }
            stringAppend(referencedName, sizeof(referencedName), texture.lifoName);
            referencedFrom = texture.levelWidth;
            referencedFormat = texture.format;
            foundReference = BOOLEAN_TRUE;
        }
        checkThat(&failureCount, "the texture that refers out is found", foundReference);

        if (foundReference)
        {
            Boolean opened = BOOLEAN_FALSE;

            printf("  %s refers to %s, holding %d wide itself\n", "the texture", referencedName,
                   (int)referencedFrom);
            for (index = 0U; index < package.resourceCount && !opened; index++)
            {
                const PackageResource *resource = &package.resources[index];
                TextureLevel level;
                TextureDescription asTexture;

                if (resource->key.typeIdentifier != (Unsigned32)PACKAGE_TYPE_LIFO)
                {
                    continue;
                }
                /* Not the same block as a texture, which is the thing worth
                   knowing: a TXTR holds a cImageData and a LIFO holds a
                   cLevelInfo, and the first attempt at this ran the texture
                   reader over one and got nothing. */
                checkThat(&failureCount, "the texture reader does not open a level",
                          textureReaderOpen(&asTexture,
                                            packageReaderGetResourceBytes(&package, resource),
                                            (MemorySize)resource->sizeInBytes) !=
                              TEXTURE_READ_OK);
                checkThat(&failureCount, "but the level reader does",
                          textureReaderOpenLevel(&level,
                                                 packageReaderGetResourceBytes(&package, resource),
                                                 (MemorySize)resource->sizeInBytes) ==
                              TEXTURE_READ_OK);
                printf("  it holds %dx%d in %lu bytes\n", (int)level.width, (int)level.height,
                       (unsigned long)level.byteCount);
                checkThat(&failureCount, "and holds a larger level than the texture did",
                          level.width > referencedFrom);
                /* A level carries no format of its own — the texture that named
                   it owns that — so the size is checked against the format the
                   texture reported, which is exactly what a caller has to do. */
                checkThat(&failureCount, "whose byte count matches its dimensions in that format",
                          level.byteCount == textureFormatGetLevelBytes(referencedFormat,
                                                                       level.width, level.height));
                opened = BOOLEAN_TRUE;
            }
            checkThat(&failureCount, "and the package really holds it", opened);
        }
    }

    printf("\n-- the level order, on the texture that proves it --\n");
    {
        /* brick_dxt1_no_lifo is 128 by 128 with eight levels and all of them
           present. Its last level is the full size image: 128/4 by 128/4 blocks
           of eight bytes is 8192. If the levels were read the other way round
           this would be 8, and an image four pixels across would still decode
           without complaint. */
        for (index = 0U; index < package.resourceCount; index++)
        {
            const PackageResource *resource = &package.resources[index];
            TextureDescription texture;

            if (resource->key.typeIdentifier != (Unsigned32)PACKAGE_TYPE_TXTR ||
                textureReaderOpen(&texture, packageReaderGetResourceBytes(&package, resource),
                                  (MemorySize)resource->sizeInBytes) != TEXTURE_READ_OK)
            {
                continue;
            }
            if (!stringEquals(texture.resourceName, "brick_dxt1_no_lifo_txtr"))
            {
                continue;
            }
            checkThat(&failureCount, "the level kept is the full size one",
                      texture.levelWidth == 128 && texture.levelHeight == 128);
            checkThat(&failureCount, "which is 8192 bytes of DXT1", texture.byteCount == 8192UL);
            checkThat(&failureCount, "and not the smallest, which would be 8",
                      texture.byteCount != 8UL);
        }
    }

    printf("\n-- decoding blocks whose answer is known --\n");
    {
        /* Two endpoints and four pixels chosen so every selector is used. Red
           and blue at full strength, so the two interpolated colours are a
           third and two thirds of the way between them. */
        static const Unsigned8 block[8] = {
            0x00U, 0xF8U, /* 0xF800, pure red */
            0x1FU, 0x00U, /* 0x001F, pure blue */
            0xE4U, 0xE4U, 0xE4U, 0xE4U /* selectors 0,1,2,3 across every row */
        };
        TextureDecodeResult decoded =
            textureDecodeLevel(pixels, sizeof(pixels), block, sizeof(block), TEXTURE_FORMAT_DXT1, 4, 4);

        checkThat(&failureCount, "a DXT1 block decodes", decoded == TEXTURE_DECODE_OK);
        /* Five bits of red widened by repeating the top bits, not by shifting:
           31 becomes 255, not 248. A texture that never reaches white is a
           permanent, subtle wrongness. */
        checkThat(&failureCount, "the first endpoint is full red",
                  pixels[0] == 255U && pixels[1] == 0U && pixels[2] == 0U && pixels[3] == 255U);
        checkThat(&failureCount, "the second is full blue",
                  pixels[4] == 0U && pixels[5] == 0U && pixels[6] == 255U);
        checkThat(&failureCount, "the third is two thirds of the way to the second",
                  pixels[8] == 170U && pixels[10] == 85U);
        checkThat(&failureCount, "and the fourth is one third",
                  pixels[12] == 85U && pixels[14] == 170U);
        checkThat(&failureCount, "all of them opaque", pixels[7] == 255U && pixels[15] == 255U);
    }

    printf("\n-- and the transparent mode, which is not the same block --\n");
    {
        /* The same block with the endpoints swapped. Now the first is not
           greater than the second, so DXT1 spends a slot on transparency:
           three colours and a hole, rather than four colours. */
        static const Unsigned8 block[8] = {
            0x1FU, 0x00U, 0x00U, 0xF8U, 0xE4U, 0xE4U, 0xE4U, 0xE4U
        };

        checkThat(&failureCount, "it decodes",
                  textureDecodeLevel(pixels, sizeof(pixels), block, sizeof(block),
                                     TEXTURE_FORMAT_DXT1, 4, 4) == TEXTURE_DECODE_OK);
        checkThat(&failureCount, "the midpoint is halfway, not a third",
                  pixels[8] == 127U && pixels[10] == 127U);
        checkThat(&failureCount, "and the fourth selector is a hole", pixels[15] == 0U);
    }

    printf("\n-- an image that does not fill its blocks --\n");
    {
        /* Two pixels wide still costs a whole four by four block, and the
           fourteen pixels that are not part of the image must not be written
           anywhere. */
        static const Unsigned8 block[8] = {
            0x00U, 0xF8U, 0x1FU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
        };

        pixels[8] = 0xAAU;
        checkThat(&failureCount, "a two by two image decodes from one block",
                  textureDecodeLevel(pixels, sizeof(pixels), block, sizeof(block),
                                     TEXTURE_FORMAT_DXT1, 2, 2) == TEXTURE_DECODE_OK);
        checkThat(&failureCount, "writing only the four pixels that exist", pixels[8] == 255U);
    }

    printf("\n-- the material, and the name it gives its texture --\n");
    {
        /* A shape says a part wears "ufocrash_cabin". Nothing numbers these:
           the whole hop is spelling. The material lives in the resource named
           for it with _txmt on the end, and names its texture in a property,
           which in turn lives in a resource with _txtr on the end. */
        Package materialPackage;
        MaterialDescription material;
        const PackageResource *resource;
        MaterialReadResult result;
        MemorySize materialSize;
        char expected[RESOURCE_NAME_LIMIT];

        inputFile = fopen("testAssets/scenegraph/material_definition.package", "rb");
        if (inputFile == NULL)
        {
            printf("FAIL  cannot open the material fixture\n");
            return checkSummarize(failureCount + 1, "texture");
        }
        materialSize = (MemorySize)fread(fileBuffer, 1, FILE_BUFFER_CAPACITY, inputFile);
        fclose(inputFile);

        if (packageReaderOpen(&materialPackage, fileBuffer, materialSize, &arena) != PACKAGE_READ_OK)
        {
            printf("FAIL  the material fixture would not open\n");
            return checkSummarize(failureCount + 1, "texture");
        }
        resource = packageReaderFindFirstOfType(&materialPackage, (Unsigned32)PACKAGE_TYPE_TXMT);
        checkThat(&failureCount, "the fixture holds a material", resource != NULL_POINTER);
        if (resource == NULL_POINTER)
        {
            return checkSummarize(failureCount, "texture");
        }

        result = materialRead(&material, packageReaderGetResourceBytes(&materialPackage, resource),
                              (MemorySize)resource->sizeInBytes);
        checkThat(&failureCount, "the material reader accepts it", result == MATERIAL_READ_OK);
        if (result != MATERIAL_READ_OK)
        {
            printf("  result: %s\n", materialReadResultGetName(result));
            return checkSummarize(failureCount, "texture");
        }

        checkThat(&failureCount, "at block version 11", material.blockVersion == 11U);
        checkThat(&failureCount, "named ufocrash_cabin_txmt",
                  stringEquals(material.resourceName, "ufocrash_cabin_txmt"));
        /* This is the name a shape's material binding uses, and it is the
           resource name without its suffix rather than a separate identifier. */
        checkThat(&failureCount, "which a shape refers to as ufocrash_cabin",
                  stringEquals(material.materialName, "ufocrash_cabin"));
        checkThat(&failureCount, "a standard material",
                  stringEquals(material.definitionType, "StandardMaterial"));

        /* Forty properties in this material and one of them matters. Reading
           the wrong one, or losing count part way through the list, lands on a
           culling mode or an animation waveform rather than a texture. */
        checkThat(&failureCount, "naming its base texture ufocrash-cabin",
                  stringEquals(material.baseTextureName, "ufocrash-cabin"));
        checkThat(&failureCount, "and listing exactly one texture", material.textureCount == 1U);
        checkThat(&failureCount, "which is the same one",
                  stringEquals(material.textureNames[0], "ufocrash-cabin"));

        materialBuildResourceName(expected, sizeof(expected), material.baseTextureName, "_txtr");
        checkThat(&failureCount, "so the texture it wants is ufocrash-cabin_txtr",
                  stringEquals(expected, "ufocrash-cabin_txtr"));
        materialBuildResourceName(expected, sizeof(expected), "ufocrash_cabin", "_txmt");
        checkThat(&failureCount, "and the material a shape asks for is ufocrash_cabin_txmt",
                  stringEquals(expected, "ufocrash_cabin_txmt"));
    }

    printf("\n-- refusing what it should --\n");
    {
        TextureDescription texture;
        static const Unsigned8 notAResource[16] = { 0 };
        static const Unsigned8 block[8] = { 0 };

        MaterialDescription material;

        checkThat(&failureCount, "rejects bytes that are not a collection",
                  textureReaderOpen(&texture, notAResource, sizeof(notAResource)) ==
                      TEXTURE_READ_NOT_A_RESOURCE);
        checkThat(&failureCount, "a material reader refuses them too",
                  materialRead(&material, notAResource, sizeof(notAResource)) ==
                      MATERIAL_READ_NOT_A_RESOURCE);
        checkThat(&failureCount, "refuses to decode into somewhere too small",
                  textureDecodeLevel(pixels, 16UL, block, sizeof(block), TEXTURE_FORMAT_DXT1, 4, 4) ==
                      TEXTURE_DECODE_DESTINATION_TOO_SMALL);
        checkThat(&failureCount, "refuses a source with fewer blocks than the image needs",
                  textureDecodeLevel(pixels, sizeof(pixels), block, sizeof(block),
                                     TEXTURE_FORMAT_DXT1, 8, 8) == TEXTURE_DECODE_TRUNCATED);
    }

    return checkSummarize(failureCount, "texture");
}
