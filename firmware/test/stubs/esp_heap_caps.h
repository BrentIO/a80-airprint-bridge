#pragma once
// Host-test stand-in for ESP-IDF's heap allocator (urfconv.c asks for PSRAM; the host has only malloc)
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(n, caps) malloc(n)
#define heap_caps_calloc(a, b, caps) calloc((a), (b))
