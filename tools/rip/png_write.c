// SPDX-License-Identifier: GPL-3.0-or-later
#include "png_write.h"
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc(const uint8_t *b, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void be32w(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static int chunk(FILE *f, const char *tag, const uint8_t *data, size_t n)
{
    uint8_t hdr[8];
    be32w(hdr, (uint32_t)n);
    memcpy(hdr + 4, tag, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (n && fwrite(data, 1, n, f) != n) return -1;

    uint8_t *tmp = malloc(n + 4);
    if (!tmp) return -1;
    memcpy(tmp, tag, 4);
    if (n) memcpy(tmp + 4, data, n);
    uint8_t c[4];
    be32w(c, crc(tmp, n + 4));
    free(tmp);
    return fwrite(c, 1, 4, f) == 4 ? 0 : -1;
}

int png_write_gray(const char *path, const uint8_t *pix, uint32_t w, uint32_t h)
{
    crc_init();
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    static const uint8_t sig[8] = { 137,'P','N','G','\r','\n',26,'\n' };
    if (fwrite(sig, 1, 8, f) != 8) { fclose(f); return -1; }

    uint8_t ihdr[13];
    be32w(ihdr, w); be32w(ihdr + 4, h);
    ihdr[8] = 8;                        // bit depth
    ihdr[9] = 0;                        // colour type 0 = grayscale
    ihdr[10] = ihdr[11] = ihdr[12] = 0; // deflate, no filter, no interlace
    if (chunk(f, "IHDR", ihdr, sizeof ihdr)) { fclose(f); return -1; }

    // Raw stream: one filter byte (0) per row, then the row.
    size_t raw_len = (size_t)h * (w + 1);
    uint8_t *raw = malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    for (uint32_t y = 0; y < h; y++) {
        raw[(size_t)y * (w + 1)] = 0;
        memcpy(raw + (size_t)y * (w + 1) + 1, pix + (size_t)y * w, w);
    }

    // zlib wrapper around stored deflate blocks.
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t z_len   = 2 + nblocks * 5 + raw_len + 4;
    uint8_t *z = malloc(z_len);
    if (!z) { free(raw); fclose(f); return -1; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;
    size_t off = 0;
    while (off < raw_len) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        z[zi++] = (off + n >= raw_len) ? 1 : 0;
        z[zi++] = (uint8_t)(n & 0xFF);        z[zi++] = (uint8_t)(n >> 8);
        z[zi++] = (uint8_t)(~n & 0xFF);       z[zi++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n);
        zi += n; off += n;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32w(z + zi, (b << 16) | a);
    zi += 4;

    int rc = chunk(f, "IDAT", z, zi);
    free(z); free(raw);
    if (rc == 0) rc = chunk(f, "IEND", NULL, 0);
    fclose(f);
    return rc;
}
