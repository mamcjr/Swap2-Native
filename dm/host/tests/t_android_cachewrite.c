#include "../../gfx/vk_android/vka_cachewrite.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
int main(void) {
    char dir[] = "/data/local/tmp/vka-cache-XXXXXX"; assert(mkdtemp(dir));
    char path[256], bad[256], temp[260]; snprintf(path, sizeof path, "%s/cache", dir); snprintf(bad, sizeof bad, "%s/blocked", dir);
    assert(vkc_write_file(path, "old", 3) && vkc_write_file(path, "new", 3));
    char bytes[4] = {0}; FILE *f = fopen(path, "rb"); assert(f && fread(bytes, 1, 3, f) == 3 && !fclose(f) && !strcmp(bytes, "new"));
    assert(!mkdir(bad, 0700));
    for (int i = 0; i < 128; i++) assert(!vkc_write_file(bad, "bytes", 5));
    snprintf(temp, sizeof temp, "%s.tmp", bad); assert(access(temp, F_OK));
    struct stat st; assert(!stat(bad, &st) && S_ISDIR(st.st_mode));
    assert(!unlink(path) && !rmdir(bad) && !rmdir(dir));
    puts("cache write: complete replacement, failed rename cleanup, no duplicate close and original destination preservation passed");
    return 0;
}
