// Submission-time sync snapshots must not observe a later re-fence of the same object.
#define __ANDROID__ 1
#include "../../gfx/vk_android/vka_worker.c"
#include <assert.h>
int main(void) {
    sync_t sync = {0}; sync.fenced = 1; sync.fence = 7;
    struct { op_hdr a; const sync_t *p; op_hdr b; sync_t *q; op_hdr c; const sync_t *r; op_hdr end; } rec = {
        {OP_WAIT_SYNC, 16}, &sync, {OP_FENCE_SYNC, 16}, &sync, {OP_WAIT_SYNC, 16}, &sync, {OP_END, 8}
    };
    job_t j = {0}; int cap = 0;
    fence_syncs((const uint8_t *)&rec, 9, 0, &j, &cap);
    assert(j.nws == 2 && j.ws[0].fence == 7 && j.ws[1].fence == 9);
    sync.fence = 99;
    assert(j.ws[0].fence == 7 && j.ws[1].fence == 9 && j.w0kind == 2 && j.w0fence == 7);
    free(j.ws);
    puts("wait records: submit-time and same-list fence snapshots survive later sync reuse");
    return 0;
}
