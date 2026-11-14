#pragma once
#include <stddef.h>
#include <stdint.h>

// Streaming URF (UNIRAST) -> A80 1-bit page converter.
// Feed the document in arbitrary pieces; each page is decoded row by row, area-scaled to out_width,
// level-clamped and Floyd-Steinberg dithered into a packed 1-bit buffer (MSB = left, 1 = black).

typedef struct {
    uint8_t *bits;   // rows * bytes_per_row, in PSRAM
    int width, rows, bytes_per_row;
    int src_w, src_h, src_dpi, src_bpp;
} a80_page_t;

typedef struct urfconv urfconv_t;

urfconv_t *urfconv_new(int out_width);
int urfconv_feed(urfconv_t *c, const uint8_t *d, size_t n);   // 0 ok, -1 malformed / out of memory
int urfconv_finish(urfconv_t *c);                             // 0 if the document ended cleanly
int urfconv_page_count(const urfconv_t *c);
a80_page_t *urfconv_page(urfconv_t *c, int i);
const char *urfconv_error(const urfconv_t *c);
a80_page_t *urfconv_take_page(urfconv_t *c, int i);   // caller now owns the page
void urfconv_free(urfconv_t *c);   // frees pages too unless taken
