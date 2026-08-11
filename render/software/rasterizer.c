#include "render/software/rasterizer.h"

#define COLOR_FRACTIONAL_BITS 16
#define COLOR_ONE (1 << COLOR_FRACTIONAL_BITS)

void rasterizerClear(const SoftwareSurface *surface, Unsigned32 packedColor)
{
    Unsigned32 rowIndex;
    Unsigned32 columnIndex;

    for (rowIndex = 0U; rowIndex < surface->heightInPixels; rowIndex += 1U)
    {
        Unsigned32 *row = surface->pixels + ((MemorySize)rowIndex * surface->pitchInPixels);
        for (columnIndex = 0U; columnIndex < surface->widthInPixels; columnIndex += 1U)
        {
            row[columnIndex] = packedColor;
        }
    }
}

#if !defined(VICTORIA_HAS_NEON) || VICTORIA_HAS_NEON == 0

static Integer32 clampChannel(Integer32 value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 255)
    {
        return 255;
    }
    return value;
}

void rasterizerFillSpan(Unsigned32 *rowPixels, Unsigned32 startX, Unsigned32 endX,
                        Integer32 redAtStart, Integer32 greenAtStart, Integer32 blueAtStart,
                        Integer32 redStep, Integer32 greenStep, Integer32 blueStep)
{
    Integer32 red = redAtStart;
    Integer32 green = greenAtStart;
    Integer32 blue = blueAtStart;
    Unsigned32 pixelX;

    for (pixelX = startX; pixelX < endX; pixelX += 1U)
    {
        rowPixels[pixelX] = ((Unsigned32)clampChannel(red >> COLOR_FRACTIONAL_BITS) << 16) |
                            ((Unsigned32)clampChannel(green >> COLOR_FRACTIONAL_BITS) << 8) |
                            (Unsigned32)clampChannel(blue >> COLOR_FRACTIONAL_BITS);
        red += redStep;
        green += greenStep;
        blue += blueStep;
    }
}

const char *rasterizerGetSpanImplementationName(void)
{
    return "portable";
}

#endif

static Integer32 toFixed(Real32 value)
{
    return (Integer32)(value * (Real32)COLOR_ONE);
}

void rasterizerDrawTriangle(const SoftwareSurface *surface, const RasterizerVertex *vertices,
                            Real32 colorScale)
{
    Real32 screenX[3];
    Real32 screenY[3];
    Unsigned32 order[3] = { 0U, 1U, 2U };
    Real32 halfWidth = (Real32)surface->widthInPixels * 0.5f;
    Real32 halfHeight = (Real32)surface->heightInPixels * 0.5f;
    Real32 area;
    Real32 inverseArea;
    Integer32 minimumX;
    Integer32 maximumX;
    Integer32 minimumY;
    Integer32 maximumY;
    Integer32 pixelY;
    Unsigned32 index;

    for (index = 0U; index < 3U; index += 1U)
    {
        screenX[index] = (vertices[index].positionX + 1.0f) * halfWidth;
        screenY[index] = (1.0f - vertices[index].positionY) * halfHeight;
    }

    area = ((screenX[1] - screenX[0]) * (screenY[2] - screenY[0])) -
           ((screenY[1] - screenY[0]) * (screenX[2] - screenX[0]));

    if (area == 0.0f)
    {
        return;
    }
    if (area < 0.0f)
    {
        Real32 swappedValue;
        Unsigned32 swappedIndex;

        swappedValue = screenX[1]; screenX[1] = screenX[2]; screenX[2] = swappedValue;
        swappedValue = screenY[1]; screenY[1] = screenY[2]; screenY[2] = swappedValue;
        swappedIndex = order[1]; order[1] = order[2]; order[2] = swappedIndex;
        area = -area;
    }
    inverseArea = 1.0f / area;

    minimumX = (Integer32)(screenX[0] < screenX[1] ? (screenX[0] < screenX[2] ? screenX[0] : screenX[2])
                                                   : (screenX[1] < screenX[2] ? screenX[1] : screenX[2]));
    maximumX = (Integer32)(screenX[0] > screenX[1] ? (screenX[0] > screenX[2] ? screenX[0] : screenX[2])
                                                   : (screenX[1] > screenX[2] ? screenX[1] : screenX[2])) + 1;
    minimumY = (Integer32)(screenY[0] < screenY[1] ? (screenY[0] < screenY[2] ? screenY[0] : screenY[2])
                                                   : (screenY[1] < screenY[2] ? screenY[1] : screenY[2]));
    maximumY = (Integer32)(screenY[0] > screenY[1] ? (screenY[0] > screenY[2] ? screenY[0] : screenY[2])
                                                   : (screenY[1] > screenY[2] ? screenY[1] : screenY[2])) + 1;

    if (minimumX < 0)
    {
        minimumX = 0;
    }
    if (minimumY < 0)
    {
        minimumY = 0;
    }
    if (maximumX > (Integer32)surface->widthInPixels)
    {
        maximumX = (Integer32)surface->widthInPixels;
    }
    if (maximumY > (Integer32)surface->heightInPixels)
    {
        maximumY = (Integer32)surface->heightInPixels;
    }

    for (pixelY = minimumY; pixelY < maximumY; pixelY += 1)
    {
        Unsigned32 *row = surface->pixels + ((MemorySize)pixelY * surface->pitchInPixels);
        Real32 sampleY = (Real32)pixelY + 0.5f;
        Integer32 spanStart = -1;
        Integer32 spanEnd = -1;
        Integer32 pixelX;

        for (pixelX = minimumX; pixelX < maximumX; pixelX += 1)
        {
            Real32 sampleX = (Real32)pixelX + 0.5f;
            Real32 weight1 = (((sampleX - screenX[0]) * (screenY[2] - screenY[0])) -
                              ((sampleY - screenY[0]) * (screenX[2] - screenX[0]))) * inverseArea;
            Real32 weight2 = (((screenX[1] - screenX[0]) * (sampleY - screenY[0])) -
                              ((screenY[1] - screenY[0]) * (sampleX - screenX[0]))) * inverseArea;
            Real32 weight0 = 1.0f - weight1 - weight2;
            Boolean isInside = (weight0 >= 0.0f && weight1 >= 0.0f && weight2 >= 0.0f) ? BOOLEAN_TRUE
                                                                                      : BOOLEAN_FALSE;

            if (isInside == BOOLEAN_TRUE && spanStart < 0)
            {
                spanStart = pixelX;
            }
            if (isInside == BOOLEAN_FALSE && spanStart >= 0)
            {
                break;
            }
        }

        if (spanStart < 0)
        {
            continue;
        }
        spanEnd = pixelX;

        {
            Real32 startSampleX = (Real32)spanStart + 0.5f;
            Real32 endSampleX = (Real32)(spanEnd - 1) + 0.5f;
            Real32 startColor[3];
            Real32 endColor[3];
            Integer32 spanLength = spanEnd - spanStart;
            Unsigned32 channel;
            Integer32 startFixed[3];
            Integer32 stepFixed[3];

            for (channel = 0U; channel < 2U; channel += 1U)
            {
                Real32 sampleX = (channel == 0U) ? startSampleX : endSampleX;
                Real32 weight1 = (((sampleX - screenX[0]) * (screenY[2] - screenY[0])) -
                                  ((sampleY - screenY[0]) * (screenX[2] - screenX[0]))) * inverseArea;
                Real32 weight2 = (((screenX[1] - screenX[0]) * (sampleY - screenY[0])) -
                                  ((screenY[1] - screenY[0]) * (sampleX - screenX[0]))) * inverseArea;
                Real32 weight0 = 1.0f - weight1 - weight2;
                Real32 *target = (channel == 0U) ? startColor : endColor;

                target[0] = ((weight0 * vertices[order[0]].red) + (weight1 * vertices[order[1]].red) +
                             (weight2 * vertices[order[2]].red)) * colorScale * 255.0f;
                target[1] = ((weight0 * vertices[order[0]].green) + (weight1 * vertices[order[1]].green) +
                             (weight2 * vertices[order[2]].green)) * colorScale * 255.0f;
                target[2] = ((weight0 * vertices[order[0]].blue) + (weight1 * vertices[order[1]].blue) +
                             (weight2 * vertices[order[2]].blue)) * colorScale * 255.0f;
            }

            for (channel = 0U; channel < 3U; channel += 1U)
            {
                startFixed[channel] = toFixed(startColor[channel]);
                stepFixed[channel] = spanLength > 1
                    ? (toFixed(endColor[channel]) - startFixed[channel]) / (spanLength - 1)
                    : 0;
            }

            rasterizerFillSpan(row, (Unsigned32)spanStart, (Unsigned32)spanEnd,
                               startFixed[0], startFixed[1], startFixed[2],
                               stepFixed[0], stepFixed[1], stepFixed[2]);
        }
    }
}
