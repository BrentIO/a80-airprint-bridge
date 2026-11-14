// Host test for the URF / PWG raster reader (main/urfconv.c). Run by CI:
//   gcc -Wall -Wextra -Werror -I test/stubs -I main -o /tmp/raster_test test/raster_host_test.c main/urfconv.c -lm
//   /tmp/raster_test test/fixtures/checker.pwg
// The fixture is two 64x48 8-bit gray pages made by test/mkpwg.py and decoded with CUPS's own raster reader to check the
// layout. Page 1 is an 8-pixel checkerboard, page 2 is the same with its rows in reverse order.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "urfconv.h"

#define W 64
#define H 48
#define OUT 48   // output width: smaller than the source, so the scaling path runs too

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t px(int x, int y, bool flip) { y = flip ? H - 1 - y : y; return ((x / 8 + y / 8) % 2 == 0) ? 255 : 0; }

// one row: runs of equal pixels become repeat runs, the rest literals (same coding in URF and PWG raster)
static size_t enc_row(uint8_t *o, const uint8_t *row)
{
    size_t n = 0;
    int i = 0;
    while (i < W) {
        int j = i;
        while (j + 1 < W && row[j + 1] == row[i] && j - i < 127) j++;
        if (j > i) { o[n++] = j - i; o[n++] = row[i]; i = j + 1; continue; }
        int k = i;
        while (k < W && k - i < 128 && !(k + 1 < W && row[k + 1] == row[k])) k++;
        if (k == i) k = i + 1;
        if (k - i == 1) { o[n++] = 0; o[n++] = row[i]; }
        else { o[n++] = 257 - (k - i); memcpy(o + n, row + i, k - i); n += k - i; }
        i = k;
    }
    return n;
}

static size_t enc_page(uint8_t *o, bool flip)
{
    size_t n = 0;
    uint8_t row[W], prev[W];
    int count = 0;
    for (int y = 0; y <= H; y++) {
        if (y < H) for (int x = 0; x < W; x++) row[x] = px(x, y, flip);
        if (y < H && count && count < 256 && !memcmp(row, prev, W)) { count++; continue; }
        if (count) { o[n++] = count - 1; n += enc_row(o + n, prev); }
        if (y < H) { memcpy(prev, row, W); count = 1; }
    }
    return n;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static size_t make_urf(uint8_t *o)
{
    size_t n = 0;
    memcpy(o, "UNIRAST", 8); put32(o + 8, 2); n = 12;
    for (int pg = 0; pg < 2; pg++) {
        uint8_t *h = o + n;
        memset(h, 0, 32); h[0] = 8; put32(h + 12, W); put32(h + 16, H); put32(h + 20, 300);
        n += 32;
        n += enc_page(o + n, pg == 1);
    }
    return n;
}

static urfconv_t *decode(const uint8_t *d, size_t n, size_t chunk, int *rc)
{
    urfconv_t *c = urfconv_new(OUT);
    *rc = 0;
    for (size_t i = 0; i < n && !*rc; i += chunk) *rc = urfconv_feed(c, d + i, n - i < chunk ? n - i : chunk);
    return c;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: raster_test fixture.pwg\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    static uint8_t pwg[1 << 16], urf[1 << 16];
    size_t npwg = fread(pwg, 1, sizeof pwg, f);
    fclose(f);
    size_t nurf = make_urf(urf);
    CHECK(npwg > 3600 && !memcmp(pwg, "RaS2", 4), "fixture is not a PWG raster file (%zu bytes)", npwg);

    // the same picture as URF and as PWG raster must convert to the same printer bits, however the bytes arrive
    int rc;
    urfconv_t *u = decode(urf, nurf, nurf, &rc);
    CHECK(rc == 0 && urfconv_finish(u) == 0 && urfconv_page_count(u) == 2, "URF decode: rc %d, %s", rc, urfconv_error(u));
    static const size_t chunks[] = {1, 7, 100, 4096, 1 << 16};
    for (size_t k = 0; k < sizeof chunks / sizeof *chunks; k++) {
        urfconv_t *p = decode(pwg, npwg, chunks[k], &rc);
        CHECK(rc == 0, "PWG decode (chunks of %zu): %s", chunks[k], urfconv_error(p));
        CHECK(urfconv_finish(p) == 0, "PWG finish (chunks of %zu): %s", chunks[k], urfconv_error(p));
        CHECK(urfconv_page_count(p) == 2, "PWG pages: %d", urfconv_page_count(p));
        for (int i = 0; i < 2 && urfconv_page_count(p) == 2 && urfconv_page_count(u) == 2; i++) {
            a80_page_t *a = urfconv_page(p, i), *b = urfconv_page(u, i);
            CHECK(a->src_w == W && a->src_h == H && a->src_dpi == 300 && a->src_bpp == 8, "PWG page %d header: %dx%d %d dpi %d-bit",
                  i + 1, a->src_w, a->src_h, a->src_dpi, a->src_bpp);
            CHECK(a->rows == b->rows && a->bytes_per_row == b->bytes_per_row &&
                  !memcmp(a->bits, b->bits, (size_t)a->rows * a->bytes_per_row), "PWG page %d differs from the same picture as URF", i + 1);
        }
        urfconv_free(p);
    }
    urfconv_free(u);

    // a PWG stream that stops between pages is complete; one that stops inside a page, or has no page, is not
    urfconv_t *c = decode(pwg, 4 + 1796 + 40, 1 << 16, &rc);
    CHECK(rc == 0 && urfconv_finish(c) == -1, "stream cut inside a page must be an error");
    urfconv_free(c);
    c = decode(pwg, 4, 4, &rc);
    CHECK(rc == 0 && urfconv_finish(c) == -1, "a PWG stream with no page must be an error");
    urfconv_free(c);

    // unsupported types are refused, not misread
    static uint8_t bad[1 << 16];
    memcpy(bad, pwg, npwg);
    put32(bad + 4 + 388, 1); put32(bad + 4 + 400, 3);   // black_1
    c = decode(bad, npwg, 1 << 16, &rc);
    CHECK(rc == -1, "1-bit PWG raster must be refused");
    urfconv_free(c);
    memcpy(bad, pwg, npwg);
    put32(bad + 4 + 396, 1);   // banded
    c = decode(bad, npwg, 1 << 16, &rc);
    CHECK(rc == -1, "banded PWG raster must be refused");
    urfconv_free(c);
    memcpy(bad, "JUNK", 4);
    c = decode(bad, npwg, 1 << 16, &rc);
    CHECK(rc == -1 || urfconv_finish(c) == -1, "a document that is neither URF nor PWG must be refused");
    urfconv_free(c);

    printf(failures ? "%d check(s) failed\n" : "raster reader: all checks passed\n", failures);
    return failures ? 1 : 0;
}
