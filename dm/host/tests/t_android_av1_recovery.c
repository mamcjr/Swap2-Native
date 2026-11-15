// Real MediaCodec -> dav1d integration through the production nn::codec adapter.
// Feed a synthetic 24-frame IVF clip (640x360 testsrc2, AV1, key interval 12).
// Run separate processes with "software" and "recover"; compare decoded luma hashes.
#include "../hle_movie_av1.c"
#include "../plat/movie_avc_android.c"
#include <assert.h>
#include <dlfcn.h>

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int pictures, width, height;
static uint64_t sum;
static void take(mavc *a, int eos) {
    uint8_t *p = malloc((size_t)width * height * 3 / 2); assert(p); int64_t pts;
    while (mavc_take(a, eos, p, width, height, &pts)) {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < (size_t)width * height; i++) h = (h ^ p[i]) * 1099511628211ull;
        printf("picture %d pts %lld luma %016llx\n", pictures++, (long long)pts, (unsigned long long)h);
        sum ^= h;
    }
    free(p);
}
int main(int argc, char **argv) {
    assert(argc == 3);
    // These platform test APIs are not in the NDK headers. An APK already has Binder callbacks.
    void *binder = dlopen("libbinder_ndk.so", RTLD_NOW); assert(binder);
    int (*max_threads)(uint32_t) = dlsym(binder, "ABinderProcess_setThreadPoolMaxThreadCount");
    void (*start_threads)(void) = dlsym(binder, "ABinderProcess_startThreadPool");
    assert(max_threads && start_threads); max_threads(4); start_threads();
    int recover = !strcmp(argv[2], "recover");
    setenv("DM_AV1_DAV1D", recover ? "0" : "1", 1);
    if (recover) setenv("DM_AV1_MC_FAIL", "8", 1);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    uint8_t head[32]; assert(fread(head, 1, 32, f) == 32 && !memcmp(head, "DKIF", 4));
    width = head[12] | (int)head[13] << 8; height = head[14] | (int)head[15] << 8;
    assert(width > 0 && height > 0 && width <= 1920 && height <= 1080);
    mavc *a = mavc_new_av1(width, height);
    int frames = 0;
    for (;;) {
        uint8_t h[12]; if (fread(h, 1, 12, f) != 12) break;
        uint32_t n = le32(h); assert(n && n < 1 << 20);
        uint8_t *data = malloc(n); assert(data && fread(data, 1, n, f) == n);
        assert(!mavc_decode(a, data, (int)n, (int64_t)frames * 33333));
        free(data); frames++; take(a, 0);
    }
    take(a, 1); fclose(f);
    assert(frames == 24 && pictures == frames);
    if (recover) assert(a->av1dec->soft && atomic_load(&g_mc_fails) == 1);
    mavc_drop(a); assert(!mavc_queued(a)); mavc_free(a);
    printf("AV1 %s: %d frames, hash %016llx, EOS/reset/free passed\n", argv[2], pictures, (unsigned long long)sum);
    return 0;
}
