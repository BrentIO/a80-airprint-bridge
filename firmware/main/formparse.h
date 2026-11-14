#pragma once
// application/x-www-form-urlencoded parsing, for the Wi-Fi setup form. No ESP-IDF dependencies (host-tested).
#include <stddef.h>
#include <string.h>

static inline int form_hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Find `key` in the NUL-terminated `body` ("a=1&b=x%20y"), decode its value into `out` (at most cap-1 bytes).
// Returns the full decoded length (greater than cap-1 means it was cut off), or -1 if the key isn't there.
static inline int form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;
    if (cap) out[0] = 0;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > klen && !strncmp(p, key, klen) && p[klen] == '=') {
            size_t n = 0;
            for (const char *v = p + klen + 1; v < end; v++) {
                int c = *v;
                if (c == '+') c = ' ';
                else if (c == '%' && v + 2 < end && form_hex(v[1]) >= 0 && form_hex(v[2]) >= 0) {
                    c = form_hex(v[1]) << 4 | form_hex(v[2]);
                    v += 2;
                }
                if (cap && n + 1 < cap) out[n] = (char)c;
                n++;
            }
            if (cap) out[n + 1 < cap ? n : cap - 1] = 0;
            return (int)n;
        }
        p = *end ? end + 1 : end;
    }
    return -1;
}
