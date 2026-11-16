// A game killed from outside while its guest memory was placed Vulkan memory: after two such runs in a row on a driver, the next ones run on an alias of that
// memory instead (vka_dev.c). Pure logic, no file or Vulkan call: the caller reads and writes the two small records and passes their text (unit test:
// dm/host/tests/t_android_placedkill.c).
//
// Why: Ocarina of Time on a Redmagic 11 Air (REDMAGICOS 11.0.15, kernel 6.6.92, 2026-11-14) is killed by signal 9, in the foreground, in a scene's loading bursts
// on every driver with VK_EXT_map_memory_placed (Qualcomm 891.11 and 891.14, Turnip V39), with nothing drawn too (DM_VK_DRAW=0), and runs on the phone's system
// driver, which has no placed maps and takes the exported alias (vka_fd_alias.h). The same drivers place the same memory on a POCO F7 Ultra (kernel 6.6.118)
// without it. What in that phone's kernel does the killing is not known (no system log from it), so this is a workaround with its cause open: the game's note,
// and docs/reference/memory.md "A game killed from outside on placed guest memory". A kill the system makes for memory reads as ApplicationExitInfo's reason 3
// on most phones and is not counted; reason 2 with status 9 in the foreground or visible is.
//
// The records, both text, in the game's temp folder:
//   vk_backing-last.txt          the run before: "key=<driver> mode=<placed|alias|other> start=<wall ms>"
//   vk_backing-<driver>.txt      this driver: "kills=<n> mode=<placed|alias>" (alias: switched, and it stays)
// "other": a run whose backing was asked for (DM_VK_ALIAS, DM_VK_PLACED_KILLS=0, the automatic diagnosis): it neither counts nor clears.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VKPK_PLACED = 0, VKPK_ALIAS = 1, VKPK_OTHER = 2 };
static const char *const vkpk_mode_name[] = {"placed", "alias", "other"};

static inline const char *vkpk_field(const char *text, const char *name, char *out, size_t size) {   // "name=value" up to a space or the end; "" without it
    out[0] = 0; if (!text) return out;
    size_t n = strlen(name);
    for (const char *p = text; (p = strstr(p, name)) != NULL; p += n) {
        if ((p != text && p[-1] != ' ' && p[-1] != '\n') || p[n] != '=') continue;
        size_t k = 0; for (const char *v = p + n + 1; *v && *v != ' ' && *v != '\n' && *v != '\r' && k + 1 < size; v++) out[k++] = *v;
        out[k] = 0; break;
    }
    return out;
}

typedef struct { int use_alias, kills, counted /* 1: the run before was a kill on placed maps, -1: it ended otherwise and the count was cleared, 0: neither */; } vkpk_result;

// last / mine: the two records' text (NULL or "": none). prev_exit: the launcher's NX_PREV_EXIT, "reason,status,time ms[,importance]" (NULL: not known).
// key: this run's driver. threshold: kills in a row that switch (0: never, and this run is "other"). asked: this run's backing was asked for ("other").
// Writes the two records for this run into last_out / mine_out and says whether this run takes the alias
static inline vkpk_result vkpk_decide(const char *last, const char *mine, const char *prev_exit, const char *key, long long now_ms, int threshold, int asked,
                                      char *last_out, size_t last_size, char *mine_out, size_t mine_size) {
    char b[160]; vkpk_result r = {0, 0, 0};
    int kills = atoi(vkpk_field(mine, "kills", b, sizeof b)); if (kills < 0) kills = 0;
    int switched = !strcmp(vkpk_field(mine, "mode", b, sizeof b), "alias");
    int prev_same = !strcmp(vkpk_field(last, "key", b, sizeof b), key ? key : ""), prev_placed = !strcmp(vkpk_field(last, "mode", b, sizeof b), "placed");
    long long prev_start = atoll(vkpk_field(last, "start", b, sizeof b));
    int reason = -1, status = -1, importance = -1; long long ts = 0;
    if (prev_exit) sscanf(prev_exit, "%d,%d,%lld,%d", &reason, &status, &ts, &importance);
    // The exit the launcher reports is the newest game process's: it is the run the "last" record names only when it ended after that run started
    if (prev_same && prev_placed && prev_start > 0 && ts >= prev_start && reason >= 0) {
        int foreground = importance <= 200;   // (100 foreground, 200 visible; a launcher that does not say, -1, counts)
        if (reason == 2 && status == 9 && foreground) { kills++; r.counted = 1; }
        else { kills = 0; r.counted = -1; }
    }
    if (!asked && threshold > 0 && kills >= threshold) switched = 1;
    r.kills = kills; r.use_alias = !asked && threshold > 0 && switched;
    snprintf(last_out, last_size, "key=%s mode=%s start=%lld\n", key ? key : "", vkpk_mode_name[asked || threshold <= 0 ? VKPK_OTHER : r.use_alias ? VKPK_ALIAS : VKPK_PLACED], now_ms);
    snprintf(mine_out, mine_size, "kills=%d mode=%s\n", kills, switched ? "alias" : "placed");
    return r;
}
