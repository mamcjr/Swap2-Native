// Game-independent coverage through the production recording, shader, compute and draw paths.
#include "vka_mesh_test_spv.h"
// With DM_VK_MESH_PACK set the selftest's task and mesh computes run packed two workgroups a subgroup (vka_mesh.inc), made before their first dispatch: the same five
// pictures are then the packed modules' check on the device (an odd count, 3, has its last workgroup's second half repeat the first)
extern int vka_mesh_pack_sync; uint64_t vka_mesh_packed_count(void);
static int mesh_selftest(tex_t *t, uint8_t *memory) {
    if (V.mesh) { VRED("mesh selftest needs DM_VK_MESH=0 to exercise emulation"); return 1; }
    vka_mesh_pack_sync = 1; uint64_t packed0 = vka_mesh_packed_count();
    const uint32_t *code[4] = {SPV_MESH_TASK_COMPUTE, SPV_MESH_MESH_COMPUTE, SPV_MESH_MESH_VERTEX, SPV_MESH_FRAGMENT};
    const size_t len[4] = {sizeof SPV_MESH_TASK_COMPUTE, sizeof SPV_MESH_MESH_COMPUTE, sizeof SPV_MESH_MESH_VERTEX, sizeof SPV_MESH_FRAGMENT};
    size_t blob = 0, at[4]; for (int i = 0; i < 4; i++) { at[i] = blob; blob = (blob + len[i] + 15) & ~(size_t)15; }
    size_t bo = sizeof(dmsh_vk_header) + 3 * sizeof(dmsh_vk_entry) + 4 * sizeof(dmsh_vk_module);
    uint8_t *bytes = calloc(1, bo + blob); if (!bytes) return 1;
    dmsh_vk_header *h = (void *)bytes;
    h->magic = DMSH_VK_LIB_MAGIC; h->version = DMSH_VK_LIB_VERSION;
    memcpy(h->chain, DMSH_CHAIN, strlen(DMSH_CHAIN)); memcpy(h->vk_chain, DMSH_VK_CHAIN, strlen(DMSH_VK_CHAIN));
    h->entry_off = sizeof *h; h->entry_count = 3; h->bind_off = h->mod_off = sizeof *h + 3 * sizeof(dmsh_vk_entry);
    h->mod_count = 4; h->blob_off = bo; h->blob_size = blob;
    dmsh_vk_entry *e = (void *)(bytes + h->entry_off);
    uint64_t key = 0x5e1f7e5780000000ull;
    e[0] = (dmsh_vk_entry){.key=key+1,.stage=DMSH_VK_STAGE_TS,.mod_count=1,.mod=0,.flags=DMSH_VK_F_MESH_FALLBACK,.local={32,1,1},.reserved=64};
    e[1] = (dmsh_vk_entry){.key=key+2,.stage=DMSH_VK_STAGE_MS,.mod_count=2,.mod=1,.flags=DMSH_VK_F_MESH_FALLBACK,.local={32,1,1},.reserved=64 | 256u<<16,.vin_mask=MESH_TEST_STRIDE};
    e[2] = (dmsh_vk_entry){.key=key+3,.stage=DMSH_VK_STAGE_FS,.mod_count=1,.mod=3,.flags=DMSH_VK_F_MESH_PRIM,.fout_mask=1};
    dmsh_vk_module *mods = (void *)(bytes + h->mod_off);
    uint8_t variant[4] = {DMSH_VK_MOD_MESH_COMPUTE,DMSH_VK_MOD_MESH_COMPUTE,DMSH_VK_MOD_MESH_VERTEX,DMSH_VK_MOD_BASE};
    for (int i = 0; i < 4; i++) { mods[i] = (dmsh_vk_module){.off=(uint32_t)at[i],.size=(uint32_t)len[i],.stored=(uint32_t)len[i],.variant=variant[i]}; memcpy(bytes+bo+at[i],code[i],len[i]); }
    // Shader objects retain the library and its bytes for the process lifetime.
    vka_shlib *L = calloc(1, sizeof *L); char err[256];
    if (!L || !vka_shlib_check(L, bytes, bo+blob, DMSH_CHAIN, DMSH_VK_CHAIN, NULL, NULL, err, sizeof err)) { VRED("mesh selftest library: %s", err); return 1; }
    vka_progsh ps = {0}; ps.sh[6]=vka_shader_make(L,key+1); ps.sh[7]=vka_shader_make(L,key+2); ps.sh[1]=vka_shader_make(L,key+3);
    if (!ps.sh[6] || !ps.sh[7] || !ps.sh[1]) return 1;
    prog_t p = {.magic=MAGIC('P','R','O','G'),.nstage=3,.serial=nvn_new_serial()}; vka_prog_put(p.serial,&ps);
    uint32_t *source=(uint32_t *)memory, *indirect=source+16; uint8_t *rb=memory+256; int bad=0;
    unsigned repeats=(unsigned)VKA_ENV("DM_VK_MESH_TEST_REPEATS",1); if(repeats<1 || repeats>32) repeats=1;
    for(unsigned run=0;run<repeats;run++) for (unsigned test=0; test<5; test++) {
        uint8_t cmd[2048], *c=cmd;
        p_set_rt rt={0}; rt.n=1; rt.color[0]=t;
        p_clear_color clear={.index=0,.mask=15}; clear.rgba[3]=1;
        p_scissor sc={0,0,1280,720}; p_viewports vp={.n=1}; vp.v[0][2]=1280;vp.v[0][3]=720;vp.v[0][5]=1;
        p_bind_program bp={&p,0xc2};
        p_draw draw={.prim=NVN_PRIM_MESH,.count=test==1?0:test==2?1:test==4?6000:3,.first=test==2?1:0,.instances=1};
        c=rec(c,OP_SET_RT,&rt,sizeof rt);c=rec(c,OP_SCISSOR,&sc,sizeof sc);c=rec(c,OP_CLEAR_COLOR,&clear,sizeof clear);
        c=rec(c,OP_VIEWPORTS,&vp,sizeof vp);c=rec(c,OP_BIND_PROGRAM,&bp,sizeof bp);
        if(test==3) {
            source[0]=3;source[1]=0;indirect[0]=0;indirect[1]=0;
            uint64_t cp[3]={(uintptr_t)source,(uintptr_t)indirect,8};c=rec(c,OP_COPY_B2B,cp,sizeof cp);draw.ind=(uintptr_t)indirect;
        }
        c=rec(c,OP_DRAW,&draw,sizeof draw);
        for(int x=0;x<3;x++) {
            p_copy_b2t read={.src=(uintptr_t)rb+16*x,.tex=t};
            read.r=(NVNcopyRegion){256+384*x,360,0,1,1,1};c=rec(c,OP_COPY_T2B,&read,sizeof read);
        }
        c=rec(c,OP_END,NULL,0); mtl_set_frame(50000+run*5+test);mtl_sub_begin();mtl_exec(cmd);uint64_t fence=mtl_sub_commit();
        int failed=!mtl_wait(fence,10000000000ull);
        for(int x=0;x<3;x++) {
            uint32_t got;memcpy(&got,rb+16*x,4);
            uint32_t want=x==0 || test==1 || (test==2 && x==2) ? 0xff000000u : x==1?0xff00ff00u:0xffff0000u;
            if(got!=want) { failed=1;VRED("mesh selftest case %u pixel %d: %08x wanted %08x",test,x,got,want); }
        }
        VLOG("mesh selftest %s: %s",test==0?"task payload/empty task/primitive interpolation":test==1?"zero tasks":test==2?"nonzero first":test==3?"GPU-written indirect arguments":"multiple output batches",failed?"FAILED":"passed");
        bad+=failed;
    }
    vka_mesh_pack_sync = 0;
    if (VKA_ENV("DM_VK_MESH_PACK", 0)) VLOG("mesh selftest: %llu task/mesh dispatches packed two workgroups a subgroup (DM_VK_MESH_PACK)", (unsigned long long)(vka_mesh_packed_count() - packed0));
    mtl_prog_release(&p);return bad;
}
