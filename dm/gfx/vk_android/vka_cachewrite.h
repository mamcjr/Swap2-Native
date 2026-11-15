// Complete-file replacement: one close, no partial cache published on a failed write or rename.
#pragma once
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static inline int vkc_write_file(const char *path, const void *data, size_t bytes) {
    char temp[1100]; int n = snprintf(temp, sizeof temp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof temp) return 0;
    FILE *file = fopen(temp, "wb"); if (!file) return 0;
    int ok = fwrite(data, 1, bytes, file) == bytes;
    if (fclose(file)) ok = 0;
    if (ok && !rename(temp, path)) return 1;
    unlink(temp); return 0;
}
