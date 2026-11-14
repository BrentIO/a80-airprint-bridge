#include <stdio.h>
#include "urfconv.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); static unsigned char buf[7919]; size_t n;
    urfconv_t *c = urfconv_new(1664);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) if (urfconv_feed(c, buf, n)) break;   // odd chunk size on purpose
    printf("finish=%d err=%s pages=%d\n", urfconv_finish(c), urfconv_error(c), urfconv_page_count(c));
    a80_page_t *p = urfconv_page(c, 0);
    if (p) { FILE *o = fopen(argv[2], "wb"); fprintf(o, "P4\n%d %d\n", p->width, p->rows);
             fwrite(p->bits, 1, (size_t)p->bytes_per_row * p->rows, o); fclose(o); printf("%dx%d\n", p->width, p->rows); }
    return 0; }
