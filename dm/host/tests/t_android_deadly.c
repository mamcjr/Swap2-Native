// Pipelines that killed the process on this driver (dm/gfx/vk_android/vka_deadly.h; vka_pso.c deadly_note / deadly_load / deadly_skip): the build in flight a crash
// records (the crashing thread's at any length, another's only once long), how a record refuses later builds and parts, the line the crash handler writes and its
// reading back, and the stamp that a driver, library or host update changes
#include <stdio.h>
#include "../../gfx/vk_android/vka_deadly.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const uint64_t A = 0xa039399e6336de1bull, B = 0x1111111111111111ull, C = 0x2222222222222222ull, MIN = 2000000000ull;
    vkdl_rec r;
    // The crashing thread's build: its exact pair, however short (DroidPlay Core 1ae945d2: a compute pipeline that crashed Qualcomm's compiler at once)
    CHECK(vkdl_choose(1, 1000, MIN, A, 0, &r) && r.pre == A && r.fs == 0, "a short compute build of the crashing thread is recorded");
    CHECK(vkdl_choose(1, 0, MIN, B, C, &r) && r.pre == B && r.fs == C, "the crashing thread's graphics build keeps both shaders");
    CHECK(!vkdl_choose(1, MIN * 5, MIN, 0, 0, &r), "a build of no shader (a vertex input or output part) records nothing");
    // Another thread's build: only once it had run DM_VK_DEADLY_MS, as its first shader alone (the rule before)
    CHECK(!vkdl_choose(0, MIN - 1, MIN, B, C, &r), "a short build of another thread is not to blame");
    CHECK(vkdl_choose(0, MIN, MIN, B, C, &r) && r.pre == B && r.fs == 0, "a long build of another thread: its pre-rasterization shader alone");
    CHECK(vkdl_choose(0, MIN, MIN, 0, C, &r) && r.pre == 0 && r.fs == C, "a long fragment part: its fragment shader alone");

    // Matching: a zero matches any shader there
    vkdl_rec L[3] = {{A, 0}, {B, C}, {0, 0x3333333333333333ull}};
    CHECK(vkdl_match(L, 3, A, 0), "the compute pipeline itself");
    CHECK(vkdl_match(L, 3, A, C), "any pipeline whose pre-rasterization shader is A");
    CHECK(vkdl_match(L, 3, B, C), "the exact pair");
    CHECK(!vkdl_match(L, 3, B, 0), "B's pre-rasterization part alone is not the pair that crashed");
    CHECK(!vkdl_match(L, 3, 0, C), "C's fragment part alone is not the pair that crashed");
    CHECK(vkdl_match(L, 3, B, 0x3333333333333333ull) && vkdl_match(L, 3, 0, 0x3333333333333333ull), "a fragment record refuses its part and every pipeline of it");
    CHECK(!vkdl_match(L, 3, 0, 0), "a build of no shader is never refused");
    vkdl_rec Z[1] = {{0, 0}};
    CHECK(!vkdl_match(Z, 1, A, C), "an empty record refuses nothing");
    CHECK(!vkdl_match(L, 0, A, 0), "an empty list refuses nothing");

    // The line: written by the crash handler, read back at the next start
    char b[35]; vkdl_rec w = {A, C}; vkdl_line(&w, b); b[34] = 0;
    CHECK(!strcmp(b, "a039399e6336de1b 2222222222222222\n"), "line %s", b);
    CHECK(vkdl_parse(b, &r) && r.pre == A && r.fs == C, "the line read back");
    CHECK(vkdl_parse("a039399e6336de1b 0000000000000000", &r) && r.pre == A && !r.fs, "a line without a newline (the file's last)");
    CHECK(vkdl_parse("A039399E6336DE1B 2222222222222222\r\n", &r) && r.pre == A, "upper case, CRLF");
    CHECK(!vkdl_parse("stamp 0123456789abcdef\n", &r), "the stamp is not a record");
    CHECK(!vkdl_parse("# shader library 0123456789abcdef\n", &r), "a comment is not a record");
    CHECK(!vkdl_parse("a039399e6336de1b\n", &r), "the old format (one key a line) is not a record");
    CHECK(!vkdl_parse("0000000000000000 0000000000000000\n", &r), "an empty record");
    CHECK(!vkdl_parse("a039399e6336de1b 2222222222222222 1\n", &r), "trailing text");
    CHECK(!vkdl_parse("a039399e6336de1 2222222222222222\n", &r), "a short key");

    // The stamp: any of the library, the driver (vendor, device, version, cache UUID) and the host build changes it
    uint8_t u[16] = {1, 2, 3}, u2[16] = {1, 2, 4};
    uint64_t s0 = vkdl_stamp(7, 0x5143, 0x43050a01, 0x802fa01f, u, "57f366b");
    CHECK(s0 == vkdl_stamp(7, 0x5143, 0x43050a01, 0x802fa01f, u, "57f366b"), "the same inputs, the same stamp");
    CHECK(s0 != vkdl_stamp(8, 0x5143, 0x43050a01, 0x802fa01f, u, "57f366b"), "another shader library");
    CHECK(s0 != vkdl_stamp(7, 0x5143, 0x43050a01, 0x802fa020, u, "57f366b"), "another driver version");
    CHECK(s0 != vkdl_stamp(7, 0x5143, 0x43050a01, 0x802fa01f, u2, "57f366b"), "another pipeline cache UUID");
    CHECK(s0 != vkdl_stamp(7, 0x5143, 0x43050a02, 0x802fa01f, u, "57f366b"), "another device");
    CHECK(s0 != vkdl_stamp(7, 0x5143, 0x43050a01, 0x802fa01f, u, "57f366c"), "another host build");
    CHECK(vkdl_stamp(7, 0x5143, 0x43050a01, 0x802fa01f, u, NULL) == vkdl_stamp(7, 0x5143, 0x43050a01, 0x802fa01f, u, ""), "no host build known is one stamp");
    CHECK(vkdl_stamp(0, 0, 0, 0, NULL, NULL) != 0, "a stamp is never 0");

    if (fails) { printf("t_android_deadly: %d failed\n", fails); return 1; }
    printf("t_android_deadly: ok\n");
    return 0;
}
