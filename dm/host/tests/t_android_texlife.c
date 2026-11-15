// Build with the Android NDK and run on ARM64 Android. Exercises production ownership/cache code without a GPU; unused sections are discarded by the linker.
#define __ANDROID__ 1
#include "../../gfx/vk_android/vka_tex.c"
#include <assert.h>
vka_dev V;
vka_fns vk;
__thread _Atomic int dm_host_work;
_Atomic uint32_t vka_env_gen = 1;
static uint64_t test_frame = 10, test_done = 8, test_generation = 2;
static void (*retire_fn)(void *); static void *retire_arg; static uint64_t retire_fence; static int freed;
static unsigned char upload_bytes[4]; static vka_img *copy_image; static int copies, destroyed;
uint64_t nvn_dirty_now(void) { return 1; }
void nvn_dirty_protect(const void *p, size_t n) { (void)p; (void)n; }
int vka_scratch(VkDeviceSize n, VkDeviceSize a, vka_span *out) {
    (void)a; assert(n <= sizeof upload_bytes); *out = (vka_span){(VkBuffer)(uintptr_t)1, upload_bytes, 0}; return 1;
}
static void VKAPI_CALL copy_to_image(VkCommandBuffer cb, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    (void)cb; (void)b; (void)l;
    assert(copy_image && im == copy_image->img && n == 1 && r->imageExtent.width == 1);
    assert(copy_image->last_frame == test_frame && copy_image->last_fence == 10); // marked before the copy reaches the encoder
    copies++;
}
static void VKAPI_CALL destroy_image(VkDevice d, VkImage im, const VkAllocationCallbacks *a) {
    (void)d; (void)im; (void)a; destroyed++;
}
uint64_t vka_frame(void) { return test_frame; }
uint64_t vka_done(void) { return test_done; }
uint64_t vka_fence_last(void) { return 9; }
void vka_after(uint64_t f, void (*fn)(void *), void *p) {
    if (f <= test_done) { fn(p); return; }
    assert(!retire_fn); retire_fence = f; retire_fn = fn; retire_arg = p;
}
void vka_imem_free(vka_imem *m) { (void)m; freed++; }
void vks_free(vks_img *p) { (void)p; assert(0); }
void vks_va_give(uint64_t n) { (void)n; assert(0); }
uint64_t vka_imem_zbytes(void) { return 0; }
uint64_t nvn_pool_fin(void) { return test_generation; }
uint64_t nvn_pool_live(const void *p, unsigned *f, int *s) { (void)p; (void)f; (void)s; return test_generation; }
double vka_env_slow(vka_envc *c, const char *k, double d) { (void)c; (void)k; return d; }
int vka_static(VkDeviceSize n, VkDeviceSize a, vka_span *o) { (void)n; (void)a; (void)o; return 0; }   // (texture pool reads' bitmap: no device here, so they are off)
static vka_img *image(int extra) {
    vka_img *im = calloc(1, sizeof *im); im->refs = 2 + extra; im->bytes = 4096;
    im->pool = (void *)1; im->pgen = 1; im->pfin = 1;
    if (!g_live) { g_livecap = 8; g_live = calloc(g_livecap, sizeof *g_live); }
    g_live[g_nlive++] = im; g_img_bytes += im->bytes;
    map_put(101, im); map_put(102, im); return im;
}
static void complete(void) {
    assert(retire_fn && retire_fence == 10 && !g_img_bytes && atomic_load(&g_retired_bytes) == 4096);
    // CPU ownership ended, but destruction waits for the recording/submitted GPU work.
    assert(test_done < retire_fence);
    test_done = retire_fence; retire_fn(retire_arg); retire_fn = NULL;
    assert(!atomic_load(&g_retired_bytes)); test_done = 8;
}
uint64_t dm_guest_read_mapped(void *dst, uintptr_t a, size_t n) { memcpy(dst, (const void *)a, n); return 0; }
int main(void) {
    vka_img *im = image(1); im->holds = 1;
    assert(img_detach_locked(im));
    assert(!map_get(101) && !map_get(102) && im->refs == 1 && im->holds == 1 && !retire_fn);
    assert(!img_detach_locked(im)); // idempotent even while a presenter retains the image
    vka_tex_drop(im); complete(); assert(freed == 1);
    im = image(1); im->wb_pending = 1;
    assert(img_detach_locked(im) && im->refs == 1 && !retire_fn);
    im->wb_pending = 0; unref_locked(im); complete(); assert(freed == 2);
    im = image(0); vka_tex_drop_pool((void *)1);
    assert(g_nlive == 1 && map_get(101)); // callback only queues, never destroys a borrowed recording pointer
    g_prune_frame = test_frame; vka_tex_prune_locked(); complete(); assert(freed == 3 && !g_nlive);
    im = image(0); im->pgen = test_generation; // pool object reinitialized at the same address
    vka_tex_drop_pool((void *)1); vka_tex_prune_locked();
    assert(g_nlive == 1 && map_get(101) && !retire_fn);
    assert(img_detach_locked(im)); complete(); assert(freed == 4);
    im = image(0); im->pgen = test_generation; im->last_frame = 1; im->last_fence = 8;
    test_frame = 20; g_lim_frame = 20; g_lim_ns = img_time_ns(); g_img_lim = 2048;
    im->gpu_owned = 1; g_prune_frame = UINT64_MAX; vka_tex_prune_locked();
    assert(map_get(101) && !retire_fn); // preserve the only copy of live GPU output, even over budget
    im->gpu_owned = 0; im->holds = 1; im->refs++;
    g_prune_frame = UINT64_MAX; vka_tex_prune_locked(); assert(map_get(101) && !retire_fn);
    im->holds = 0; im->refs--; im->last_fence = 11;
    g_prune_frame = UINT64_MAX; vka_tex_prune_locked(); assert(map_get(101) && !retire_fn);
    im->last_fence = 8; im->last_frame = 20;
    g_prune_frame = UINT64_MAX; vka_tex_prune_locked(); assert(map_get(101) && !retire_fn);
    im->last_frame = 1; g_prune_frame = UINT64_MAX; vka_tex_prune_locked();
    // No new submission is needed to reclaim an image whose last use already completed.
    assert(!map_get(101) && !map_get(102) && !retire_fn && !atomic_load(&g_retired_bytes));
    assert(freed == 5 && !g_img_bytes && !g_nlive);
    // An allocation burst after the first scan in a frame must trigger another pressure check.
    test_frame = 30; g_lim_frame = 30; g_lim_ns = img_time_ns(); g_img_lim = 2048;
    vka_tex_prune_pressure_locked();
    im = image(0); im->pgen = test_generation; im->last_frame = 1; im->last_fence = test_done;
    g_img_bytes -= im->bytes; im->bytes = 20 * VKTL_MIB; g_img_bytes += im->bytes;
    vka_tex_prune_pressure_locked();
    assert(!g_nlive && !g_img_bytes && !retire_fn && !atomic_load(&g_retired_bytes) && freed == 6);
    im = image(0); im->pgen = test_generation; im->last_frame = 1; im->last_fence = test_done;
    g_img_bytes -= im->bytes; im->bytes = 20 * VKTL_MIB; g_img_bytes += im->bytes;
    vka_tex_prune_pressure_locked();
    assert(!g_nlive && !g_img_bytes && !retire_fn && !atomic_load(&g_retired_bytes) && freed == 7);
    // Transfer-only use must protect an otherwise-old, unheld image from a later allocation scan.
    im = image(0); im->pgen = test_generation; im->last_frame = 1; im->last_fence = test_done;
    img_touch(im);
    assert(im->last_frame == test_frame && im->last_fence == 10);
    test_frame = 40; g_lim_frame = 40; g_lim_ns = img_time_ns();
    vka_tex_prune_pressure_locked();
    assert(g_nlive == 1 && map_get(101) && !retire_fn && freed == 7); // old enough again, but the queued copy has not completed
    test_done = 10; test_frame++;
    vka_tex_prune_pressure_locked();
    assert(!g_nlive && !g_img_bytes && !retire_fn && !atomic_load(&g_retired_bytes) && freed == 8);
    // Exercise the real partial-upload entry point too: the mark must happen before vkCmdCopyBufferToImage.
    test_done = 8; im = image(0); copy_image = im; im->img = (VkImage)(uintptr_t)1234;
    im->w = im->h = im->depth = im->levels = im->layers = 1;
    im->f.bw = im->f.bh = 1; im->f.bpb = 4;
    unsigned char source[4] = {1, 2, 3, 4}; tex_t t = {0}; t.mem = source; t.L.size = sizeof source; t.d.flags = NVN_TEXTURE_FLAGS_LINEAR;
    vk.vkCmdCopyBufferToImage = copy_to_image; vk.vkDestroyImage = destroy_image;
    assert(upload_rows_tracked(im, &t, (VkCommandBuffer)(uintptr_t)1, 0, sizeof source));
    assert(copies == 1 && !memcmp(source, upload_bytes, sizeof source) && !destroyed);
    assert(img_detach_locked(im) && !destroyed); complete();
    assert(destroyed == 1 && freed == 9);
    // A pool-only descriptor remains valid under pressure, even without CPU-side draw binds.
    test_frame = 60; test_done = 10; g_lim_frame = test_frame; g_lim_ns = img_time_ns(); g_img_lim = 2048;
    im = image(0); im->pgen = test_generation; im->last_frame = 1; im->last_fence = test_done;
    g_ntreg = 2; g_treg = calloc(2, sizeof *g_treg);
    g_treg[1].t.serial = 101; g_treg[1].t.pool = (pool_t *)im->pool; g_treg[1].pgen = test_generation;
    g_prune_frame = UINT64_MAX; vka_tex_prune_pressure_locked();
    assert(map_get(101) && g_nlive == 1 && freed == 9);
    // Registration overwritten/unregistered: no permanent residency leak.
    g_treg[1].t.serial = 0; test_frame++; g_lim_frame = test_frame; g_lim_ns = img_time_ns();
    g_prune_frame = UINT64_MAX; vka_tex_prune_pressure_locked();
    assert(!g_nlive && freed == 10);
    im = image(0); im->pgen = test_generation; im->last_frame = 1; im->last_fence = test_done;
    g_treg[1].t.serial = 101; g_treg[1].pgen = test_generation - 1;
    g_prune_frame = UINT64_MAX; vka_tex_prune_pressure_locked();
    assert(!g_nlive && freed == 11); // a dead-generation slot cannot keep a replacement alive
    // Texture pool reads reported (DM_VK_POOL_FB, no eager slots): a pool read the GPU reported (pfb_hit) stamps the slot's image used, kept like a drawn one
    atomic_store(&g_pfb_eager, 0); g_ntex_slots = 2; g_pfb_st = calloc(1u << 20, 1); test_done = 8;
    im = image(0); im->pgen = test_generation; im->last_frame = 1; im->last_fence = 8;
    g_treg[1].t.serial = 101; g_treg[1].pgen = test_generation;
    test_frame++; g_lim_frame = test_frame; g_lim_ns = img_time_ns();
    pfb_hit(1, test_frame, 8);
    assert(im->last_frame == test_frame && !(g_pfb_st[1] & PFB_WANT));   // (its slot names it: nothing to write)
    g_prune_frame = UINT64_MAX; vka_tex_prune_pressure_locked();
    assert(map_get(101) && g_nlive == 1 && !retire_fn);
    // Unread for VKTL_KEEP_FRAMES frames: evicted although a slot names it, and destroyed only once every submission recorded so far is done (vka_fence_last() + 1)
    test_frame += 3; g_lim_frame = test_frame; g_lim_ns = img_time_ns();
    g_prune_frame = UINT64_MAX; vka_tex_prune_pressure_locked();
    assert(!map_get(101) && !g_nlive && retire_fn && retire_fence == 10);
    // ...and its slot is written again at the next prepare (redirty_ids: dirty), where write_tex finds no image and gives the null view, a write vka_tex_prepare sends with
    // the pool reads reported (slot_write_keep): the slot stops naming the view before the image goes. (write_tex itself reaches image creation, which this test cannot link)
    assert(g_tdflag && g_tdflag[1] && g_ntd >= 1 && g_tdirty[g_ntd - 1] == 1);
    { VkDescriptorImageInfo si = {VK_NULL_HANDLE, g_null_view, VK_IMAGE_LAYOUT_GENERAL};
      atomic_store(&g_pfb, 1); assert(slot_write_keep(&si));
      atomic_store(&g_pfb, 0); assert(!slot_write_keep(&si)); }   // (the eager path, DM_VK_POOL_FB=0: a null write is dropped, as before)
    g_tdflag[1] = 0; g_ntd = 0;   // (as vka_tex_prepare leaves them)
    complete(); assert(freed == 12);
    // A read of the slot once its image is gone asks for it: the slot written again (its image made) at the next prepare
    g_pfb_st[1] |= PFB_NULL; pfb_hit(1, test_frame, 8);
    assert((g_pfb_st[1] & PFB_WANT) && g_tdflag && g_tdflag[1]);
    // A bitmap the GPU never writes (DM_VK_POOL_FB_DEAD readbacks, not one bit): every slot eager again; one bit seen keeps the feedback
    g_pfb_bits = 1; g_pfb_nrb = 600; pfb_dead_check_locked(); assert(!atomic_load(&g_pfb_blind_eager) && !tex_eager());
    g_pfb_bits = 0; g_pfb_nrb = 599; pfb_dead_check_locked(); assert(!atomic_load(&g_pfb_blind_eager));
    g_pfb_nrb = 600; pfb_dead_check_locked(); assert(atomic_load(&g_pfb_blind_eager) && atomic_load(&g_pfb_blind_new) && tex_eager());
    atomic_store(&g_pfb_blind_eager, 0); atomic_store(&g_pfb_blind_new, 0); g_pfb_nrb = 0;
    free(g_pfb_st); g_pfb_st = NULL; g_ntex_slots = 0; atomic_store(&g_pfb_eager, 1); test_done = 10;
    free(g_treg); g_treg = NULL; g_ntreg = 0;
    // Exercise production init/release, not just the owner-map helper.
    tex_t original = {0}; original.magic = MAGIC('T', 'E', 'X', 0); original.serial = 101;
    mtl_tex_init(&original); tex_t copied = original;
    im = image(0); im->last_fence = test_done;
    mtl_tex_release(&copied);
    assert(map_get(101) && map_get(102) && im->refs == 2);
    mtl_tex_release(&original);
    assert(!map_get(101) && map_get(102) && im->refs == 1);
    assert(img_detach_locked_at(im, test_done));
    assert(!map_get(102) && !g_nlive && freed == 13);
    puts("t_android_texlife: aliases, holds, writeback, GPU retirement, pool reuse, cache eviction, live GPU-content protection and texture pool reads passed");
    return 0;
}
