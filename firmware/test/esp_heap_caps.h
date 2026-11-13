#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(n, c) malloc(n)
#define heap_caps_calloc(a, n, c) calloc(a, n)
