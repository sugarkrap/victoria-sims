/* Exercises the software rasterizer with no window system involved, so a
   rendering fault can be told apart from a presentation fault.

   It also pins down the NEON span filler: both builds render the same scene
   and print the same checksum, and the checksums are compared against each
   other rather than against a number written down here. Run it as:

     verifyRasterizer [outputFile.ppm]

   Exit status is non-zero if any check fails. */

#include <stdio.h>

#include "utils/assert.h"
#include "render/software/rasterizer.h"

#define SURFACE_WIDTH 320U
#define SURFACE_HEIGHT 180U
#define CLEAR_COLOR 0x0F1219U

static Unsigned32 surfacePixels[SURFACE_WIDTH * SURFACE_HEIGHT];

static const RasterizerVertex triangleVertices[3] = {
    { 0.0f, 0.6f, 1.0f, 0.35f, 0.55f },
    { -0.6f, -0.5f, 0.35f, 0.75f, 1.0f },
    { 0.6f, -0.5f, 1.0f, 0.9f, 0.4f }
};

static Integer32 failureCount = 0;

int main(int argumentCount, char **argumentValues)
{
    SoftwareSurface surface;
    Unsigned32 coveredPixels = 0U;
    Unsigned32 checksum = 2166136261U;
    Unsigned32 index;

    surface.pixels = surfacePixels;
    surface.widthInPixels = SURFACE_WIDTH;
    surface.heightInPixels = SURFACE_HEIGHT;
    surface.pitchInPixels = SURFACE_WIDTH;

    printf("span implementation: %s\n", rasterizerGetSpanImplementationName());

    rasterizerClear(&surface, CLEAR_COLOR);
    checkThat(&failureCount, "clear reaches the last pixel",
          surfacePixels[(SURFACE_WIDTH * SURFACE_HEIGHT) - 1U] == CLEAR_COLOR);

    rasterizerDrawTriangle(&surface, triangleVertices, 1.0f);

    for (index = 0U; index < SURFACE_WIDTH * SURFACE_HEIGHT; index += 1U)
    {
        if (surfacePixels[index] != CLEAR_COLOR)
        {
            coveredPixels += 1U;
        }
        /* Fowler-Noll-Vo over the whole surface: any single wrong pixel in
           either implementation changes it. */
        checksum = (checksum ^ surfacePixels[index]) * 16777619U;
    }

    printf("covered pixels: %u of %u\n", coveredPixels, SURFACE_WIDTH * SURFACE_HEIGHT);
    printf("checksum: %08X\n", checksum);

    /* The triangle spans 60%% of the width and 55%% of the height, so a little
       over a sixth of the surface. Loose bounds: this is here to catch "drew
       nothing" and "filled everything", not to pin down exact coverage. */
    checkThat(&failureCount, "triangle covered a plausible area",
          coveredPixels > (SURFACE_WIDTH * SURFACE_HEIGHT) / 8U &&
          coveredPixels < (SURFACE_WIDTH * SURFACE_HEIGHT) / 2U);

    /* Corners are outside the triangle, the centre is inside it. */
    checkThat(&failureCount, "top-left corner untouched", surfacePixels[0] == CLEAR_COLOR);
    checkThat(&failureCount, "bottom-right corner untouched",
          surfacePixels[(SURFACE_WIDTH * SURFACE_HEIGHT) - 1U] == CLEAR_COLOR);
    checkThat(&failureCount, "centre is inside the triangle",
          surfacePixels[((SURFACE_HEIGHT / 2U) * SURFACE_WIDTH) + (SURFACE_WIDTH / 2U)] != CLEAR_COLOR);

    /* Vertex colours are distinct, so a correctly interpolated triangle has
       many shades. A flat fill would collapse to one or two. */
    {
        Unsigned32 distinctSample = 0U;
        Unsigned32 previous = 0xFFFFFFFFU;
        Unsigned32 row = SURFACE_HEIGHT / 2U;
        for (index = 0U; index < SURFACE_WIDTH; index += 1U)
        {
            Unsigned32 pixel = surfacePixels[(row * SURFACE_WIDTH) + index];
            if (pixel != CLEAR_COLOR && pixel != previous)
            {
                distinctSample += 1U;
                previous = pixel;
            }
        }
        printf("distinct shades across one scanline: %u\n", distinctSample);
        checkThat(&failureCount, "colour is interpolated, not flat", distinctSample > 8U);
    }

    if (argumentCount > 1)
    {
        FILE *outputFile = fopen(argumentValues[1], "wb");
        if (outputFile != NULL)
        {
            fprintf(outputFile, "P6\n%u %u\n255\n", SURFACE_WIDTH, SURFACE_HEIGHT);
            for (index = 0U; index < SURFACE_WIDTH * SURFACE_HEIGHT; index += 1U)
            {
                unsigned char rgb[3];
                rgb[0] = (unsigned char)((surfacePixels[index] >> 16) & 0xFFU);
                rgb[1] = (unsigned char)((surfacePixels[index] >> 8) & 0xFFU);
                rgb[2] = (unsigned char)(surfacePixels[index] & 0xFFU);
                fwrite(rgb, 1U, 3U, outputFile);
            }
            fclose(outputFile);
            printf("wrote %s\n", argumentValues[1]);
        }
    }

    return checkSummarize(failureCount, "rasterizer");
}
