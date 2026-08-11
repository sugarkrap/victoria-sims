
#include "render/software/rasterizer.h"

#if defined(VICTORIA_HAS_NEON) && VICTORIA_HAS_NEON == 1

#include <arm_neon.h>

#define COLOR_FRACTIONAL_BITS 16

void rasterizerFillSpan(Unsigned32 *rowPixels, Unsigned32 startX, Unsigned32 endX,
                        Integer32 redAtStart, Integer32 greenAtStart, Integer32 blueAtStart,
                        Integer32 redStep, Integer32 greenStep, Integer32 blueStep)
{
    Unsigned32 pixelX = startX;
    int32x4_t red;
    int32x4_t green;
    int32x4_t blue;
    int32x4_t redStepByFour;
    int32x4_t greenStepByFour;
    int32x4_t blueStepByFour;
    const int32x4_t zero = vdupq_n_s32(0);
    const int32x4_t maximumChannel = vdupq_n_s32(255);
    const int32x4_t laneOffsets = { 0, 1, 2, 3 };

    if (endX <= startX)
    {
        return;
    }

    red = vaddq_s32(vdupq_n_s32(redAtStart), vmulq_n_s32(laneOffsets, redStep));
    green = vaddq_s32(vdupq_n_s32(greenAtStart), vmulq_n_s32(laneOffsets, greenStep));
    blue = vaddq_s32(vdupq_n_s32(blueAtStart), vmulq_n_s32(laneOffsets, blueStep));

    redStepByFour = vdupq_n_s32(redStep * 4);
    greenStepByFour = vdupq_n_s32(greenStep * 4);
    blueStepByFour = vdupq_n_s32(blueStep * 4);

    while (pixelX + 4U <= endX)
    {
        int32x4_t redChannel = vminq_s32(vmaxq_s32(vshrq_n_s32(red, COLOR_FRACTIONAL_BITS), zero),
                                         maximumChannel);
        int32x4_t greenChannel = vminq_s32(vmaxq_s32(vshrq_n_s32(green, COLOR_FRACTIONAL_BITS), zero),
                                           maximumChannel);
        int32x4_t blueChannel = vminq_s32(vmaxq_s32(vshrq_n_s32(blue, COLOR_FRACTIONAL_BITS), zero),
                                          maximumChannel);

        uint32x4_t packed = vorrq_u32(
            vorrq_u32(vshlq_n_u32(vreinterpretq_u32_s32(redChannel), 16),
                      vshlq_n_u32(vreinterpretq_u32_s32(greenChannel), 8)),
            vreinterpretq_u32_s32(blueChannel));

        vst1q_u32(rowPixels + pixelX, packed);

        red = vaddq_s32(red, redStepByFour);
        green = vaddq_s32(green, greenStepByFour);
        blue = vaddq_s32(blue, blueStepByFour);
        pixelX += 4U;
    }

    {
        Unsigned32 consumed = pixelX - startX;
        Integer32 tailRed = redAtStart + ((Integer32)consumed * redStep);
        Integer32 tailGreen = greenAtStart + ((Integer32)consumed * greenStep);
        Integer32 tailBlue = blueAtStart + ((Integer32)consumed * blueStep);

        while (pixelX < endX)
        {
            Integer32 redChannel = tailRed >> COLOR_FRACTIONAL_BITS;
            Integer32 greenChannel = tailGreen >> COLOR_FRACTIONAL_BITS;
            Integer32 blueChannel = tailBlue >> COLOR_FRACTIONAL_BITS;

            redChannel = redChannel < 0 ? 0 : (redChannel > 255 ? 255 : redChannel);
            greenChannel = greenChannel < 0 ? 0 : (greenChannel > 255 ? 255 : greenChannel);
            blueChannel = blueChannel < 0 ? 0 : (blueChannel > 255 ? 255 : blueChannel);

            rowPixels[pixelX] = ((Unsigned32)redChannel << 16) | ((Unsigned32)greenChannel << 8) |
                                (Unsigned32)blueChannel;

            tailRed += redStep;
            tailGreen += greenStep;
            tailBlue += blueStep;
            pixelX += 1U;
        }
    }
}

const char *rasterizerGetSpanImplementationName(void)
{
    return "NEON";
}

#endif
