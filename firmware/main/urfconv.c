// Streaming URF -> 1-bit converter. Image pipeline:
// resize to the head width (area average), gray, levels clamp (<=7 -> 0, >=248 -> 255), Floyd-Steinberg.
#include "urfconv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

#define MAX_PAGES 16
#define PS(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define PSZ(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

enum { S_FILEHDR, S_PAGEHDR, S_REPEAT, S_CTRL, S_REPPIX, S_LIT, S_DONE, S_ERROR };

struct urfconv {
    int W;                       // output width (dots)
    int state;
    uint8_t hdr[32];
    int hdr_n;
    uint32_t pages_total;
    // current page
    int bpc, w, h, dpi, Ho;
    uint8_t *row;                // assembled source row (w * bpc)
    int row_fill, repeat, rep_left, lit_left, pix_n;
    uint8_t pix[4];
    int y;                       // source rows emitted
    // horizontal area-average map: for each output x, up to 4 (index, weight/65536)
    uint16_t *hx_idx;
    uint16_t *hx_w;              // 4 per x
    uint8_t *gray;               // source row as gray (w)
    uint8_t *hs;                 // horizontally scaled row (W)
    // vertical accumulation
    double sy;
    int cur;                     // output row index held in acc_cur
    uint32_t *acc_cur, *acc_next;
    uint32_t wsum_cur, wsum_next;
    // dithering
    int16_t *err_cur, *err_next;
    a80_page_t *page;
    a80_page_t *pages[MAX_PAGES];
    int npages;
    char err[64];
};

static uint8_t LUT[256];

static void build_lut(void)
{
    if (LUT[255]) return;
    for (int i = 0; i < 256; i++) {
        int v = i <= 7 ? 0 : i >= 248 ? 255 : (int)lround((i - 7) * 255.0 / (248 - 7));
        LUT[i] = v;
    }
}

static int fail(urfconv_t *c, const char *msg)
{
    snprintf(c->err, sizeof c->err, "%s", msg);
    c->state = S_ERROR;
    return -1;
}

urfconv_t *urfconv_new(int out_width)
{
    build_lut();
    urfconv_t *c = PSZ(sizeof *c);
    if (!c) return NULL;
    c->W = out_width;
    c->state = S_FILEHDR;
    return c;
}

static void free_page_bufs(urfconv_t *c)
{
    free(c->row); free(c->hx_idx); free(c->hx_w); free(c->gray); free(c->hs);
    free(c->acc_cur); free(c->acc_next); free(c->err_cur); free(c->err_next);
    c->row = c->gray = c->hs = NULL;
    c->hx_idx = c->hx_w = NULL;
    c->acc_cur = c->acc_next = NULL;
    c->err_cur = c->err_next = NULL;
}

// ------------------------------------------------------------ output side: dither + pack one row
static void emit_out_row(urfconv_t *c, uint32_t *acc, uint32_t wsum, int oy)
{
    if (oy >= c->Ho || !wsum) return;
    int W = c->W;
    uint8_t *dst = c->page->bits + (size_t)oy * c->page->bytes_per_row;
    int16_t *e = c->err_cur, *en = c->err_next;
    memset(en, 0, (W + 2) * sizeof *en);
    for (int x = 0; x < W; x++) {
        int v = LUT[(acc[x] + wsum / 2) / wsum > 255 ? 255 : (acc[x] + wsum / 2) / wsum] + e[x + 1];
        int out = v < 128 ? 0 : 255;
        if (!out) dst[x >> 3] |= 0x80 >> (x & 7);
        int err = v - out;
        e[x + 2] += err * 7 / 16;
        en[x] += err * 3 / 16;
        en[x + 1] += err * 5 / 16;
        en[x + 2] += err / 16;
    }
    c->err_cur = en;   // swap error rows
    c->err_next = e;
}

// ------------------------------------------------------------ source side: one decoded row
static void push_src_row(urfconv_t *c)
{
    int W = c->W;
    // to gray
    if (c->bpc == 1) memcpy(c->gray, c->row, c->w);
    else
        for (int i = 0; i < c->w; i++) {
            const uint8_t *p = c->row + i * c->bpc;
            c->gray[i] = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
        }
    // horizontal area average
    for (int x = 0; x < W; x++) {
        uint32_t s = 0;
        const uint16_t *wt = c->hx_w + 4 * x;
        int i0 = c->hx_idx[x];
        for (int k = 0; k < 4 && wt[k]; k++) s += c->gray[i0 + k] * (uint32_t)wt[k];
        c->hs[x] = (s + 32768) >> 16;
    }
    // vertical: this source row covers output rows [a, b)
    double a = c->y * c->sy, b = (c->y + 1) * c->sy;
    int o = (int)a;
    while (c->cur < o) {   // rows before o are complete
        emit_out_row(c, c->acc_cur, c->wsum_cur, c->cur);
        uint32_t *t = c->acc_cur; c->acc_cur = c->acc_next; c->acc_next = t;
        memset(c->acc_next, 0, W * sizeof(uint32_t));
        c->wsum_cur = c->wsum_next; c->wsum_next = 0;
        c->cur++;
    }
    double split = c->cur + 1;
    uint32_t w0 = (uint32_t)lround(((b < split ? b : split) - a) * 65536);
    uint32_t w1 = b > split ? (uint32_t)lround((b - split) * 65536) : 0;
    for (int x = 0; x < W; x++) {
        c->acc_cur[x] += c->hs[x] * w0;
        if (w1) c->acc_next[x] += c->hs[x] * w1;
    }
    c->wsum_cur += w0;
    c->wsum_next += w1;
    c->y++;
}

static int page_start(urfconv_t *c)
{
    const uint8_t *h = c->hdr;
    int bpp = h[0];
    c->w = h[12] << 24 | h[13] << 16 | h[14] << 8 | h[15];
    c->h = h[16] << 24 | h[17] << 16 | h[18] << 8 | h[19];
    c->dpi = h[20] << 24 | h[21] << 16 | h[22] << 8 | h[23];
    if (bpp != 8 && bpp != 24) return fail(c, "unsupported bits per pixel");
    if (c->w < 16 || c->w > 10000 || c->h < 16 || c->h > 20000) return fail(c, "bad page size");
    if (c->npages >= MAX_PAGES) return fail(c, "too many pages");
    c->bpc = bpp / 8;
    int W = c->W;
    c->Ho = (int)lround((double)c->h * W / c->w);
    c->sy = (double)c->Ho / c->h;
    if (c->Ho > 0xFFFF) return fail(c, "page too long");

    c->row = PS(c->w * c->bpc);
    c->gray = PS(c->w);
    c->hs = PS(W);
    c->hx_idx = PS(W * sizeof(uint16_t));
    c->hx_w = PSZ(W * 4 * sizeof(uint16_t));
    c->acc_cur = PSZ(W * sizeof(uint32_t));
    c->acc_next = PSZ(W * sizeof(uint32_t));
    c->err_cur = PSZ((W + 2) * sizeof(int16_t));
    c->err_next = PSZ((W + 2) * sizeof(int16_t));
    a80_page_t *p = PSZ(sizeof *p);
    if (p) {
        p->width = W;
        p->bytes_per_row = (W + 7) / 8;
        p->rows = c->Ho;
        p->bits = PSZ((size_t)p->bytes_per_row * p->rows);
        p->src_w = c->w; p->src_h = c->h; p->src_dpi = c->dpi; p->src_bpp = bpp;
    }
    if (!c->row || !c->gray || !c->hs || !c->hx_idx || !c->hx_w || !c->acc_cur || !c->acc_next ||
        !c->err_cur || !c->err_next || !p || !p->bits) {
        if (p) { free(p->bits); free(p); }
        return fail(c, "out of memory for page");
    }
    c->page = p;
    // horizontal map: output x covers source [x*r, (x+1)*r)
    double r = (double)c->w / W;
    for (int x = 0; x < W; x++) {
        double s0 = x * r, s1 = (x + 1) * r;
        int i0 = (int)s0;
        c->hx_idx[x] = i0;
        uint32_t tot = 0;
        for (int k = 0; k < 4 && i0 + k < c->w && i0 + k < s1; k++) {
            double lo = i0 + k > s0 ? i0 + k : s0, hi = i0 + k + 1 < s1 ? i0 + k + 1 : s1;
            uint32_t wt = (uint32_t)lround((hi - lo) / r * 65536);
            if (wt > 65535) wt = 65535;
            c->hx_w[4 * x + k] = wt ? wt : 1;
            tot += wt;
        }
    }
    c->y = 0;
    c->cur = 0;
    c->wsum_cur = c->wsum_next = 0;
    c->row_fill = 0;
    return 0;
}

static void page_end(urfconv_t *c)
{
    emit_out_row(c, c->acc_cur, c->wsum_cur, c->cur);
    emit_out_row(c, c->acc_next, c->wsum_next, c->cur + 1);
    c->pages[c->npages++] = c->page;
    c->page = NULL;
    free_page_bufs(c);
}

static void row_done(urfconv_t *c)
{
    // pad short rows with white
    if (c->row_fill < c->w * c->bpc) memset(c->row + c->row_fill, 0xFF, c->w * c->bpc - c->row_fill);
    for (int i = 0; i < c->repeat && c->y < c->h; i++) push_src_row(c);
    c->row_fill = 0;
    if (c->y >= c->h) {
        page_end(c);
        c->hdr_n = 0;
        c->state = (uint32_t)c->npages >= c->pages_total ? S_DONE : S_PAGEHDR;
    } else {
        c->state = S_REPEAT;
    }
}

int urfconv_feed(urfconv_t *c, const uint8_t *d, size_t n)
{
    int rowlen = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = d[i];
        rowlen = c->w * c->bpc;
        switch (c->state) {
        case S_FILEHDR:
            c->hdr[c->hdr_n++] = b;
            if (c->hdr_n == 12) {
                if (memcmp(c->hdr, "UNIRAST", 8)) return fail(c, "not a URF document");
                c->pages_total = c->hdr[8] << 24 | c->hdr[9] << 16 | c->hdr[10] << 8 | c->hdr[11];
                c->hdr_n = 0;
                c->state = c->pages_total ? S_PAGEHDR : S_DONE;
            }
            break;
        case S_PAGEHDR:
            c->hdr[c->hdr_n++] = b;
            if (c->hdr_n == 32) {
                if (page_start(c)) return -1;
                c->state = S_REPEAT;
            }
            break;
        case S_REPEAT:
            c->repeat = b + 1;
            c->state = S_CTRL;
            break;
        case S_CTRL:
            if (b == 0x80) {
                memset(c->row + c->row_fill, 0xFF, rowlen - c->row_fill);
                c->row_fill = rowlen;
                row_done(c);
            } else if (b < 0x80) {
                c->rep_left = b + 1;
                c->pix_n = 0;
                c->state = S_REPPIX;
            } else {
                c->lit_left = (257 - b) * c->bpc;
                c->state = S_LIT;
            }
            break;
        case S_REPPIX:
            c->pix[c->pix_n++] = b;
            if (c->pix_n == c->bpc) {
                while (c->rep_left-- && c->row_fill < rowlen) {
                    memcpy(c->row + c->row_fill, c->pix, c->bpc);
                    c->row_fill += c->bpc;
                }
                if (c->row_fill >= rowlen) row_done(c);
                else c->state = S_CTRL;
            }
            break;
        case S_LIT: {
            // copy as many literal bytes as are available in this piece
            size_t k = n - i;
            if (k > (size_t)c->lit_left) k = c->lit_left;
            size_t room = rowlen - c->row_fill;
            size_t cp = k < room ? k : room;
            memcpy(c->row + c->row_fill, d + i, cp);
            c->row_fill += cp;
            c->lit_left -= k;
            i += k - 1;
            if (c->lit_left == 0) {
                if (c->row_fill >= rowlen) row_done(c);
                else c->state = S_CTRL;
            }
            break;
        }
        case S_DONE:
            return 0;   // ignore trailing bytes
        default:
            return -1;
        }
    }
    return 0;
}

int urfconv_finish(urfconv_t *c)
{
    if (c->state == S_DONE) return 0;
    if (c->state != S_ERROR) fail(c, "document ended early");
    return -1;
}

int urfconv_page_count(const urfconv_t *c) { return c->npages; }
a80_page_t *urfconv_page(urfconv_t *c, int i) { return i < c->npages ? c->pages[i] : NULL; }
a80_page_t *urfconv_take_page(urfconv_t *c, int i)
{
    a80_page_t *p = urfconv_page(c, i);
    if (p) c->pages[i] = NULL;
    return p;
}
const char *urfconv_error(const urfconv_t *c) { return c->err[0] ? c->err : "ok"; }

void urfconv_free(urfconv_t *c)
{
    if (!c) return;
    free_page_bufs(c);
    if (c->page) { free(c->page->bits); free(c->page); }
    for (int i = 0; i < c->npages; i++)
        if (c->pages[i]) { free(c->pages[i]->bits); free(c->pages[i]); }
    free(c);
}
