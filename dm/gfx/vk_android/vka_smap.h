// The texture cache's serial -> image map and the set of serials the CPU wrote early (vka_tex.c "Images by serial"): open addressing with tombstones.
// Ported from upstream Banana Native v0.3.13 (624c267e). Before (until 2026-11-13 here) the count of entries only went up: a put into a tombstone counted too and a delete
// never took one off, so the table doubled with every serial ever registered (about 32 MB after a million) and map_del_im's full scan of it, run for each image that goes,
// grew with the session. Its put also stopped at the first tombstone of a probe chain without looking further, which would store a live key twice; in this tree map_put
// only runs after a map_get miss under g_tmu (vka_tex_get), so that path was not reached, but the set (cpuw) and any later caller get the right rule.
// Now: a probe runs past tombstones to the key or an empty slot; a new key goes into the first tombstone seen only after the chain proved it absent; live entries and tombstones
// are counted apart; the table is made again at the same size when tombstones filled it, and grows only when live entries need it.
// The includer defines VKA_SMAP_IM as the image type. Not thread-safe: the includer's lock (g_tmu). Tested by dm/host/tests/t_android_smap.c
#ifndef VKA_SMAP_H
#define VKA_SMAP_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define VKSM_TOMB (~0ull)   // never a valid serial
typedef struct { uint64_t serial; VKA_SMAP_IM *im; } smap_ent;   // serial 0 = empty, im NULL with a serial = a texture that has no image (format refused)
typedef struct { smap_ent *e; size_t cap, live, tomb; } vksm;   // cap: a power of two or 0; live + tomb < cap always, so a probe meets an empty slot

static size_t vksm_hslot(uint64_t s, size_t cap) { return (size_t)((s * 0x9E3779B97F4A7C15ull) >> 20) & (cap - 1); }
static smap_ent *vksm_get(const vksm *m, uint64_t s) {
    if (!m->cap) return NULL;
    for (size_t i = vksm_hslot(s, m->cap);; i = (i + 1) & (m->cap - 1)) { if (m->e[i].serial == s) return &m->e[i]; if (!m->e[i].serial) return NULL; }
}
static int vksm_live_ent(const smap_ent *e) { return e->serial && e->serial != VKSM_TOMB; }
// The live entries again in a table of cap slots (no tombstones). 0: no memory (the table stays as it was)
static int vksm_rebuild(vksm *m, size_t cap) {
    smap_ent *n = calloc(cap, sizeof *n);
    if (!n) return 0;
    for (size_t i = 0; i < m->cap; i++) if (vksm_live_ent(&m->e[i])) {
        size_t j = vksm_hslot(m->e[i].serial, cap); while (n[j].serial) j = (j + 1) & (cap - 1);
        n[j] = m->e[i];
    }
    free(m->e); m->e = n; m->cap = cap; m->tomb = 0; return 1;
}
// s -> im. 0: the table could not take a new key (no memory): s is not in it
static int vksm_put(vksm *m, uint64_t s, VKA_SMAP_IM *im) {
    smap_ent *e = vksm_get(m, s);
    if (e) { e->im = im; return 1; }
    if ((m->live + m->tomb + 1) * 2 > m->cap) {   // full of live entries and tombstones: grown when live entries are the cause (room for a quarter of the table before the next), else only cleaned
        size_t c = m->cap ? m->cap : 1024; while ((m->live + 1) * 4 > c) c *= 2;
        if (!vksm_rebuild(m, c) && m->live + m->tomb + 1 >= m->cap) return 0;   // (without memory the old table serves while it keeps an empty slot)
    }
    size_t i = vksm_hslot(s, m->cap);
    while (vksm_live_ent(&m->e[i])) i = (i + 1) & (m->cap - 1);   // (s is absent: the first free slot, a tombstone or the empty one)
    if (m->e[i].serial == VKSM_TOMB) m->tomb--;
    m->e[i] = (smap_ent){s, im}; m->live++; return 1;
}
static void vksm_del_ent(vksm *m, smap_ent *e) { e->serial = VKSM_TOMB; e->im = NULL; m->live--; m->tomb++; }
static int vksm_del(vksm *m, uint64_t s) { smap_ent *e = vksm_get(m, s); if (e) vksm_del_ent(m, e); return e != NULL; }
// How many live entries name im (the image's serial owners)
static int vksm_count_im(const vksm *m, const VKA_SMAP_IM *im) {
    int n = 0;
    for (size_t i = 0; i < m->cap; i++) n += vksm_live_ent(&m->e[i]) && m->e[i].im == im;
    return n;
}
// Every entry of im out; how many
static int vksm_del_im(vksm *m, const VKA_SMAP_IM *im) {
    int n = 0;
    for (size_t i = 0; i < m->cap; i++) if (vksm_live_ent(&m->e[i]) && m->e[i].im == im) { vksm_del_ent(m, &m->e[i]); n++; }
    return n;
}
#endif
