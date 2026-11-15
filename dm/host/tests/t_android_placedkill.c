// vka_placedkill.h: when a game killed from outside on placed guest memory takes the alias (Android, vka_dev.c). The records pass from run to run as on the phone.
#include "../../gfx/vk_android/vka_placedkill.h"
#include <assert.h>

static char g_last[200], g_mine[200];   // the two records, as the files hold them between runs
static vkpk_result run(const char *prev_exit, const char *key, long long now_ms, int threshold, int asked) {
    char lo[200], mo[200];
    vkpk_result r = vkpk_decide(g_last, g_mine, prev_exit, key, now_ms, threshold, asked, lo, sizeof lo, mo, sizeof mo);
    snprintf(g_last, sizeof g_last, "%s", lo); snprintf(g_mine, sizeof g_mine, "%s", mo);
    return r;
}
static void fresh(void) { g_last[0] = g_mine[0] = 0; }

int main(void) {
    vkpk_result r; char b[64];
    // the field reader
    assert(!strcmp(vkpk_field("key=891.14 mode=placed start=1000\n", "mode", b, sizeof b), "placed"));
    assert(!strcmp(vkpk_field("key=891.14 mode=placed start=1000\n", "start", b, sizeof b), "1000"));
    assert(!strcmp(vkpk_field("xkey=1 key=a", "key", b, sizeof b), "a"));
    assert(!strcmp(vkpk_field(NULL, "key", b, sizeof b), "") && !strcmp(vkpk_field("", "key", b, sizeof b), "") && !strcmp(vkpk_field("key", "key", b, sizeof b), ""));

    // The phone this was written for: killed in the foreground on placed maps, twice in a row, then the alias, and it stays
    fresh();
    r = run(NULL, "891.14", 1000, 2, 0);                 assert(!r.use_alias && r.kills == 0 && r.counted == 0);   // the first run ever
    r = run("2,9,5000,100", "891.14", 6000, 2, 0);       assert(!r.use_alias && r.kills == 1 && r.counted == 1);
    r = run("2,9,9000,100", "891.14", 10000, 2, 0);      assert(r.use_alias && r.kills == 2 && r.counted == 1);
    assert(strstr(g_last, "mode=alias") && strstr(g_mine, "mode=alias"));
    r = run("10,0,20000,100", "891.14", 21000, 2, 0);    assert(r.use_alias && r.counted == 0);                    // stopped by the player on the alias: still the alias
    r = run("2,9,30000,100", "891.14", 31000, 2, 0);     assert(r.use_alias);                                      // a kill on the alias changes nothing

    // A run that ends otherwise clears the count: two kills must be in a row
    fresh();
    run(NULL, "d", 1000, 2, 0);
    r = run("2,9,2000,100", "d", 3000, 2, 0);            assert(r.kills == 1);
    r = run("10,0,4000,100", "d", 5000, 2, 0);           assert(r.kills == 0 && r.counted == -1 && !r.use_alias);
    r = run("2,9,6000,100", "d", 7000, 2, 0);            assert(r.kills == 1 && !r.use_alias);

    // Not counted: low memory (reason 3), a crash signal, a kill in the background (importance 400), an exit older than the run before
    fresh(); run(NULL, "d", 1000, 2, 0);
    r = run("3,0,2000,100", "d", 3000, 2, 0);            assert(r.kills == 0 && r.counted == -1);
    r = run("2,11,4000,100", "d", 5000, 2, 0);           assert(r.kills == 0);
    r = run("2,9,6000,400", "d", 7000, 2, 0);            assert(r.kills == 0);
    r = run("2,9,6500,100", "d", 9000, 2, 0);            assert(r.kills == 0 && r.counted == 0);                   // 6500 < 7000: another run's end
    r = run("2,9,9500", "d", 10000, 2, 0);               assert(r.kills == 1 && r.counted == 1);                   // a launcher that gives no importance counts

    // Another driver's run in between: its death is not this driver's
    fresh();
    run(NULL, "a", 1000, 2, 0);
    r = run("2,9,2000,100", "a", 3000, 2, 0);            assert(r.kills == 1);
    snprintf(g_mine, sizeof g_mine, "%s", "");           // (driver b's own record: none yet)
    r = run("2,9,4000,100", "b", 5000, 2, 0);            assert(r.kills == 0 && r.counted == 0);                   // the run before was a's
    snprintf(g_mine, sizeof g_mine, "%s", "kills=1 mode=placed\n");   // (back on a, with a's record)
    r = run("2,9,6000,100", "a", 7000, 2, 0);            assert(r.kills == 1 && r.counted == 0 && !r.use_alias);   // the run before was b's: neither counted nor cleared

    // A run whose backing was asked for (the diagnosis, DM_VK_ALIAS) neither counts nor clears, and never switches
    fresh();
    run(NULL, "d", 1000, 2, 0);
    r = run("2,9,2000,100", "d", 3000, 2, 0);            assert(r.kills == 1);
    r = run("2,9,4000,100", "d", 5000, 2, 1);            assert(!r.use_alias && r.kills == 2 && strstr(g_last, "mode=other"));   // counted (the run before was placed), not acted on
    r = run("2,9,6000,100", "d", 7000, 2, 1);            assert(!r.use_alias && r.kills == 2 && r.counted == 0);   // the run before was "other"
    r = run("10,0,8000,100", "d", 9000, 2, 0);           assert(r.use_alias && r.kills == 2);                      // the next plain run acts on the two
    // threshold 0: never, and such a run is "other"
    fresh(); run(NULL, "d", 1000, 0, 0);                 assert(strstr(g_last, "mode=other"));
    r = run("2,9,2000,100", "d", 3000, 0, 0);            assert(!r.use_alias && r.kills == 0);
    // threshold 1: the first kill switches
    fresh(); run(NULL, "d", 1000, 1, 0);
    r = run("2,9,2000,100", "d", 3000, 1, 0);            assert(r.use_alias && r.kills == 1);
    // damaged records and exits do not crash and do not switch
    snprintf(g_last, sizeof g_last, "%s", "garbage\n\n=== key== mode start=x"); snprintf(g_mine, sizeof g_mine, "%s", "kills=-7 mode=");
    r = run("x,y,z", NULL, 1, 2, 0);                     assert(!r.use_alias && r.kills == 0);
    r = run("", "", 2, 2, 0);                            assert(!r.use_alias);
    printf("t_android_placedkill: ok\n");
    return 0;
}
