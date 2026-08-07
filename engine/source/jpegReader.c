#include "victoria/jpegReader.h"

/* Baseline JPEG decoder (SOF0 only).
 *
 * No dynamic allocation.  All state lives on the stack or in caller-owned
 * buffers.  Supports YCbCr with any sampling factors up to 2x2 per
 * component, which covers 4:2:0, 4:2:2, and 4:4:4. */

/* ---- fixed-point IDCT ---- */

/* T[k][n] = C(k) * cos(pi*k*(2n+1)/16) * 4096
 * where C(0) = 1/sqrt(2), C(k>0) = 1.
 * Two passes of this (column then row) with >>13 rounding gives the 2-D IDCT. */
static const short IDCT_T[8][8] = {
    { 2896, 2896, 2896, 2896, 2896, 2896, 2896, 2896 },
    { 4017, 3406, 2276,  799, -799,-2276,-3406,-4017 },
    { 3784, 1567,-1567,-3784,-3784,-1567, 1567, 3784 },
    { 3406, -799,-4017,-2276, 2276, 4017,  799,-3406 },
    { 2896,-2896,-2896, 2896, 2896,-2896,-2896, 2896 },
    { 2276,-4017,  799, 3406,-3406, -799, 4017,-2276 },
    { 1567,-3784, 3784,-1567,-1567, 3784,-3784, 1567 },
    {  799,-2276, 3406,-4017, 4017,-3406, 2276, -799 }
};

static void idct1d(int *v, int stride)
{
    int n, k;
    long long tmp[8];
    for (n = 0; n < 8; n++) {
        long long s = 0;
        for (k = 0; k < 8; k++) s += (long long)v[k * stride] * IDCT_T[k][n];
        tmp[n] = (s + 4096LL) >> 13;
    }
    for (n = 0; n < 8; n++) v[n * stride] = (int)tmp[n];
}

static void idct2d(int block[64])
{
    int i;
    for (i = 0; i < 8; i++) idct1d(block + i,     8);
    for (i = 0; i < 8; i++) idct1d(block + i * 8, 1);
}

/* ---- Huffman ---- */

typedef struct {
    int       minCode[17];
    int       maxCode[17];
    int       firstSym[17];
    Unsigned8 values[256];
    int       count;
} HuffTable;

static void buildHuffTable(HuffTable *ht,
                            const Unsigned8 counts[16],
                            const Unsigned8 *syms)
{
    int L, code = 0, symIdx = 0, srcIdx = 0;
    for (L = 1; L <= 16; L++) {
        int n = counts[L - 1];
        ht->minCode[L]  = code;
        ht->maxCode[L]  = (n > 0) ? code + n - 1 : -1;
        ht->firstSym[L] = symIdx;
        for (; srcIdx < symIdx + n; srcIdx++) ht->values[symIdx++] = syms[srcIdx];
        code = (code + n) << 1;
    }
    ht->count = symIdx;
}

/* ---- bit reader ---- */

typedef struct {
    const Unsigned8 *data;
    MemorySize        pos;
    MemorySize        end;
    Unsigned32        buf;
    int               nbits;
} BitReader;

static void brInit(BitReader *br, const Unsigned8 *data, MemorySize start, MemorySize end)
{
    br->data  = data;
    br->pos   = start;
    br->end   = end;
    br->buf   = 0;
    br->nbits = 0;
}

static void brRefill(BitReader *br)
{
    Unsigned8 b;
    if (br->pos >= br->end) { br->buf <<= 8; br->nbits += 8; return; }
    b = br->data[br->pos++];
    if (b == 0xFF && br->pos < br->end && br->data[br->pos] == 0x00) br->pos++;
    br->buf    = (br->buf << 8) | (Unsigned32)b;
    br->nbits += 8;
}

static int brRead(BitReader *br, int n)
{
    int result;
    while (br->nbits < n) brRefill(br);
    result    = (int)((br->buf >> (br->nbits - n)) & ((1U << n) - 1U));
    br->nbits -= n;
    return result;
}

static int brReadBit(BitReader *br)
{
    if (br->nbits == 0) brRefill(br);
    br->nbits--;
    return (int)((br->buf >> br->nbits) & 1U);
}

static int huffDecode(BitReader *br, const HuffTable *ht)
{
    int L, code = 0;
    for (L = 1; L <= 16; L++) {
        code = (code << 1) | brReadBit(br);
        if (ht->maxCode[L] >= 0 && code <= ht->maxCode[L])
            return ht->values[ht->firstSym[L] + (code - ht->minCode[L])];
    }
    return -1;
}

static int extendMag(int val, int cat)
{
    if (cat == 0) return 0;
    return (val < (1 << (cat - 1))) ? val - ((1 << cat) - 1) : val;
}

/* ---- zigzag ---- */

static const Unsigned8 ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

static int decodeBlock(BitReader *br,
                       const HuffTable *dcHt, const HuffTable *acHt,
                       const int qt[64],
                       int *dcPred,
                       int block[64])
{
    int k, s, r, val;
    int zz[64];

    for (k = 0; k < 64; k++) zz[k] = 0;

    s = huffDecode(br, dcHt);
    if (s < 0) return 0;
    val    = (s > 0) ? brRead(br, s) : 0;
    *dcPred += extendMag(val, s);
    zz[0]  = *dcPred;

    for (k = 1; k < 64; ) {
        s = huffDecode(br, acHt);
        if (s < 0) break;
        r  = (s >> 4) & 0xF;
        s &= 0xF;
        if (s == 0) { if (r == 0) break; k += 16; continue; }
        k += r;
        if (k >= 64) break;
        val   = brRead(br, s);
        zz[k++] = extendMag(val, s);
    }

    for (k = 0; k < 64; k++) block[ZIGZAG[k]] = zz[k] * qt[k];
    return 1;
}

/* ---- colour conversion ---- */

static void ycbcr2rgba(int y, int cb, int cr, Unsigned8 *out)
{
    int r = y + (((cr - 128) * 91750) >> 16);
    int g = y - (((cb - 128) * 22544 + (cr - 128) * 46802) >> 16);
    int b = y + (((cb - 128) * 116130) >> 16);
    out[0] = (r < 0) ? 0 : (r > 255) ? 255 : (Unsigned8)r;
    out[1] = (g < 0) ? 0 : (g > 255) ? 255 : (Unsigned8)g;
    out[2] = (b < 0) ? 0 : (b > 255) ? 255 : (Unsigned8)b;
    out[3] = 255;
}

/* ---- main decoder ---- */

JpegReadResult jpegReadToRgba(const Unsigned8 *d, MemorySize sz,
                              Unsigned8 *outRgba, MemorySize outCap,
                              Unsigned32 *outW, Unsigned32 *outH)
{
    HuffTable ht[4];
    int qt[2][64];
    Unsigned32 width = 0, height = 0;
    int nComp = 0;
    int sfH[3]  = {0,0,0}, sfV[3]  = {0,0,0};
    int qtIdx[3] = {0,0,0};
    int dcHt[3] = {0,0,0}, acHt[3] = {0,0,0};
    int maxSfH = 0, maxSfV = 0;
    MemorySize scanStart = 0;
    int i, k;
    Boolean foundSOS = BOOLEAN_FALSE;

    if (!d || sz < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return JPEG_READ_INVALID;

    for (i = 0; i < 4; i++) {
        int L; ht[i].count = 0;
        for (L = 0; L <= 16; L++) { ht[i].minCode[L] = 0; ht[i].maxCode[L] = -1; ht[i].firstSym[L] = 0; }
    }
    for (i = 0; i < 2; i++) for (k = 0; k < 64; k++) qt[i][k] = 1;

    /* Parse markers */
    {
        MemorySize pos = 2;

        while (pos + 1 < sz && !foundSOS) {
            MemorySize segLen;
            Unsigned8 m1;

            if (d[pos] != 0xFF) { pos++; continue; }
            m1 = d[pos + 1];
            pos += 2;

            if (m1 == 0xD9) break;
            if (m1 == 0xD8) continue;
            if (m1 >= 0xD0 && m1 <= 0xD7) continue;
            if (pos + 2 > sz) break;
            segLen = (MemorySize)((d[pos] << 8) | d[pos+1]) - 2U;
            pos += 2;
            if (pos + segLen > sz) break;

            if (m1 == 0xC0) {
                int j;
                if (segLen < 11 || d[pos] != 8) { return JPEG_READ_UNSUPPORTED; }
                height = ((Unsigned32)d[pos+1] << 8) | d[pos+2];
                width  = ((Unsigned32)d[pos+3] << 8) | d[pos+4];
                nComp  = d[pos+5];
                if (nComp != 3 || width > 512 || height > 512 || width == 0 || height == 0)
                    return JPEG_READ_UNSUPPORTED;
                if (outCap < (MemorySize)width * height * 4) return JPEG_READ_INVALID;
                for (j = 0; j < 3; j++) {
                    int ci = d[pos+6+j*3] - 1;
                    if (ci < 0 || ci > 2) return JPEG_READ_INVALID;
                    sfH[ci]   = (d[pos+7+j*3] >> 4) & 0xF;
                    sfV[ci]   =  d[pos+7+j*3] & 0xF;
                    qtIdx[ci] =  d[pos+8+j*3] & 1;
                    if (sfH[ci] > maxSfH) maxSfH = sfH[ci];
                    if (sfV[ci] > maxSfV) maxSfV = sfV[ci];
                }
            }
            else if (m1 == 0xC4) {
                MemorySize p = 0;
                while (p + 17 <= segLen) {
                    Unsigned8 tc = (d[pos+p] >> 4) & 1;
                    Unsigned8 th =  d[pos+p] & 1;
                    int idx = (tc == 0) ? (int)th : 2 + (int)th;
                    int nSym = 0, j;
                    Unsigned8 counts[16];
                    p++;
                    for (j = 0; j < 16; j++) { counts[j] = d[pos+p+j]; nSym += counts[j]; }
                    p += 16;
                    if (p + (MemorySize)nSym > segLen) break;
                    buildHuffTable(&ht[idx], counts, d + pos + p);
                    p += (MemorySize)nSym;
                }
            }
            else if (m1 == 0xDB) {
                MemorySize p = 0;
                while (p + 65 <= segLen) {
                    int qi = d[pos+p] & 1, j;
                    if ((d[pos+p] >> 4) != 0) return JPEG_READ_UNSUPPORTED;
                    p++;
                    for (j = 0; j < 64; j++) qt[qi][j] = d[pos+p+j];
                    p += 64;
                }
            }
            else if (m1 == 0xDA) {
                int nScan = d[pos], j;
                MemorySize p = 1;
                if (nScan != 3) return JPEG_READ_UNSUPPORTED;
                for (j = 0; j < nScan && p+1 < segLen; j++) {
                    int ci = d[pos+p] - 1;
                    Unsigned8 tbl = d[pos+p+1];
                    p += 2;
                    if (ci < 0 || ci > 2) return JPEG_READ_INVALID;
                    dcHt[ci] = ((tbl >> 4) & 0xF) == 0 ? 0 : 2;
                    acHt[ci] = (tbl & 0xF) == 0 ? 1 : 3;
                }
                scanStart  = pos + segLen;
                foundSOS   = BOOLEAN_TRUE;
                break;
            }

            pos += segLen;
        }
    }

    if (!foundSOS || width == 0 || height == 0 || maxSfH == 0 || maxSfV == 0)
        return JPEG_READ_INVALID;

    /* Entropy decode */
    {
        int mcuW    = maxSfH * 8;
        int mcuH    = maxSfV * 8;
        int mcuCols = ((int)width  + mcuW - 1) / mcuW;
        int mcuRows = ((int)height + mcuH - 1) / mcuH;
        int dcPred[3] = {0,0,0};
        int mcuC, mcuR;
        BitReader br;

        brInit(&br, d, scanStart, sz);

        for (mcuR = 0; mcuR < mcuRows; mcuR++) {
            for (mcuC = 0; mcuC < mcuCols; mcuC++) {
                int comp;
                Unsigned8 compBuf[3][256];

                for (comp = 0; comp < 3; comp++) {
                    int bH = sfH[comp], bV = sfV[comp];
                    int bRow, bCol;
                    for (bRow = 0; bRow < bV; bRow++) {
                        for (bCol = 0; bCol < bH; bCol++) {
                            int pi, pj;
                            int block[64];
                            for (k = 0; k < 64; k++) block[k] = 0;
                            decodeBlock(&br,
                                        &ht[dcHt[comp]], &ht[acHt[comp]],
                                        qt[qtIdx[comp]], &dcPred[comp], block);
                            idct2d(block);
                            for (pi = 0; pi < 8; pi++) {
                                for (pj = 0; pj < 8; pj++) {
                                    int v  = block[pi*8+pj] + 128;
                                    int cx = bCol*8+pj, cy = bRow*8+pi;
                                    int bufW = bH * 8;
                                    compBuf[comp][cy * bufW + cx] =
                                        (v < 0) ? 0 : (v > 255) ? 255 : (Unsigned8)v;
                                }
                            }
                        }
                    }
                }

                {
                    int py, px;
                    for (py = 0; py < mcuH; py++) {
                        for (px = 0; px < mcuW; px++) {
                            Unsigned32 imgX = (Unsigned32)(mcuC * mcuW + px);
                            Unsigned32 imgY = (Unsigned32)(mcuR * mcuH + py);
                            Unsigned8 *dst;
                            int y, cb, cr;
                            if (imgX >= width || imgY >= height) continue;
                            dst = outRgba + (imgY * width + imgX) * 4;
                            {
                                int cx = (sfH[0]>0) ? (px * sfH[0]) / maxSfH : 0;
                                int cy = (sfV[0]>0) ? (py * sfV[0]) / maxSfV : 0;
                                y  = compBuf[0][cy * sfH[0] * 8 + cx];
                            }
                            {
                                int cx = (sfH[1]>0) ? (px * sfH[1]) / maxSfH : 0;
                                int cy = (sfV[1]>0) ? (py * sfV[1]) / maxSfV : 0;
                                cb = compBuf[1][cy * sfH[1] * 8 + cx];
                            }
                            {
                                int cx = (sfH[2]>0) ? (px * sfH[2]) / maxSfH : 0;
                                int cy = (sfV[2]>0) ? (py * sfV[2]) / maxSfV : 0;
                                cr = compBuf[2][cy * sfH[2] * 8 + cx];
                            }
                            ycbcr2rgba(y, cb, cr, dst);
                        }
                    }
                }
            }
        }
    }

    *outW = width;
    *outH = height;
    return JPEG_READ_OK;
}
