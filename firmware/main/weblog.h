#pragma once
#include <stddef.h>
void weblog_init(void);
void weblog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
size_t weblog_dump(char *out, size_t cap);
