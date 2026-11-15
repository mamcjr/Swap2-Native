// Task/mesh fallback integration, included by vka_draw.c. Guest bindings retain their
// NVN stage: only execution moves to compute. Output, payload and offset buffers are
// GPU-only during replay and reused in queue order with barriers, not copied to the CPU.
#include "vka_mesh_control_spv.h"
#include "vka_mesh_compact_spv.h"
// The fallback's info block (scratch of the draw): the controller's records and the host's parameters (mesh_emulate)
enum { MESH_INFO = 80 };
void *vka_static_block(VkDeviceSize n, vka_span *out);   // vka_shader.c: a host-visible block of its own
void vka_static_block_free(void *h);
static vka_span g_mesh_output, g_mesh_payload, g_mesh_offsets;
static VkDeviceSize g_mesh_output_size, g_mesh_payload_size;
static void *g_mesh_payload_h;          // the payload's own block (vka_static_block); NULL: from vka_static (DM_VK_MESH_PAYLOAD_FIT=0), kept for the process's life
static uint64_t g_mesh_payload_used;    // the last frame a draw needed more than a quarter of the payload block
static vka_layout g_mesh_control_layout;
static VkPipeline g_mesh_control_pipeline;
#define MESH_PAYLOAD_MIN ((VkDeviceSize)1 << 20)

// The fallback's own buffers between its phases. DM_VK_MESH_BARRIERS (live, default 1): only the stages that touch them (the controller and task/mesh compute, the
// indirect dispatch and draw records, the pulling vertex stage, the report copy); 0 = ALL_COMMANDS on both sides, as before. Guest memory is not covered here: what the
// task and mesh compute writes of it gets a full barrier of its own (mesh_compute), and a pass the fallback breaks ends with vka_rp_end's barrier ("Pass-end barriers")
// The compaction's draw records (DM_VK_MESH_COMPACT): a draw count word (and 12 bytes of padding), then up to MESH_CMDS VkDrawIndirectCommand. A buffer of its own:
// the static blocks have no indirect usage. Reused batch after batch in queue order, as the fallback's other buffers, under mesh_barrier
enum { MESH_CMDS = 65536 };
static VkBuffer g_mesh_cmds; static VkDeviceMemory g_mesh_cmds_mem;
static int g_mesh_compact_on;   // the pulling draw being recorded takes its records from g_mesh_cmds (vka_draw.c)
static uint32_t g_mesh_compact_max;
static void mesh_barrier(void) {
    const vka_span cmds = {.buf = g_mesh_cmds};
    const vka_span *s[5]={&g_mesh_output,&g_mesh_payload,&g_mesh_offsets,&g_mesh_info,&cmds};
    VkDeviceSize n[5]={g_mesh_output_size,g_mesh_payload_size,65536u*8,MESH_INFO,16+(VkDeviceSize)MESH_CMDS*16};
    unsigned nb = g_mesh_cmds ? 5 : 4;
    VkBufferMemoryBarrier bb[5];
    for(unsigned i=0;i<nb;i++) bb[i]=(VkBufferMemoryBarrier){VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=s[i]->buf,.offset=s[i]->off,.size=n[i]};
    VkPipelineStageFlags st = VKA_ENV("DM_VK_MESH_BARRIERS", 1) ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT
                                                               : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    vk.vkCmdPipelineBarrier(g_cb,st,st,0,0,NULL,nb,bb,0,NULL);
    if (vka_gp_active) vka_gp_mark(g_cb, "vka_mesh.inc(compute)", __LINE__);   // DM_VK_GPUPROF: the fallback's compute is a step of its own, not part of the pass after it
}

static int mesh_aux_writes(void) {
    const vka_span *s[4] = {&g_mesh_output, &g_mesh_payload, &g_mesh_offsets, &g_mesh_info};
    VkDeviceSize bytes[4] = {g_mesh_output_size, g_mesh_payload_size, 65536u * 8, MESH_INFO};
    if (g_w.n > 512 - 4) return 0;
    for (unsigned j = 0; j < 4; j++) {
        uint32_t k = g_w.n++;
        g_w.bi[k] = (VkDescriptorBufferInfo){s[j]->buf, s[j]->off, bytes[j]};
        g_w.w[k] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = DMSH_VK_MESH_OUT + j,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &g_w.bi[k]};
    }
    return 1;
}

static void mesh_block_free(void *h) { vka_static_block_free(h); }
static void mesh_crumb_done(void *h) { VLOG("task/mesh fallback draw %d: its submission completed on the GPU", (int)(intptr_t)h); }   // (mesh_emulate's trail of the first draws)
// tasks: the most task workgroups this draw can have (its count when direct, 65535 when the GPU reads it). DM_VK_MESH_PAYLOAD_FIT (live, default 1): the payload is a block
// of its own sized for the draw (at least 1 MiB), grown when a draw needs more and given back down to the draw's size once no draw needed more than a quarter of it for
// DM_VK_MESH_PAYLOAD_IDLE frames (live, default 1200; 0 = never); a superseded block is freed after the last submission that may use it. Ocarina of Time's direct task draws
// (64-word payloads, 1 to 13 tasks) then take 1 MiB, not 16; its indirect 1024-word ones still take 256 MiB while they are drawn. 0 = the old sizing: 65535 tasks always,
// from vka_static, every superseded block kept for the process's life (about 336 MiB once both widths ran)
static int mesh_buffers(uint32_t payload_words, uint32_t tasks) {
    if (!g_mesh_output.buf) {
        VkDeviceSize n = 64u << 20;
        if (n > V.props.limits.maxStorageBufferRange) n = V.props.limits.maxStorageBufferRange;
        if (!n || !vka_static(n, V.sb_align, &g_mesh_output)) return 0;
        g_mesh_output_size = n;
    }
    int fit = VKA_ENV("DM_VK_MESH_PAYLOAD_FIT", 1);
    if (!fit || tasks > 65535) tasks = 65535;   // (a direct count over 65535 is refused by the controller: it then reads no payload)
    if (!tasks) tasks = 1;
    // The largest possible task grid. It is one shared GPU buffer, not one allocation
    // per draw/frame. Earlier smaller buffers remain valid for commands already recorded.
    VkDeviceSize need = (VkDeviceSize)(payload_words ? payload_words : 1) * tasks * 4;
    if (need > V.props.limits.maxStorageBufferRange) { VRED_ONCE("task payload exceeds fallback storage-buffer range"); return 0; }
    // A block superseded here is still bound by commands recorded in this submission. Every fence is reserved either before its submission is replayed (vka_worker.c) or at its
    // commit on this thread, so vka_fence_last() + 1 is this submission's fence or a later one: freeing there is never early
    if (!fit) {
        if (need > g_mesh_payload_size) {
            vka_span s; if (!vka_static(need, V.sb_align, &s)) return 0;
            if (g_mesh_payload_h) vka_after(vka_fence_last() + 1, mesh_block_free, g_mesh_payload_h);
            g_mesh_payload = s; g_mesh_payload_size = need; g_mesh_payload_h = NULL;
        }
    } else {
        uint64_t f = vka_frame(); int idle = VKA_ENV("DM_VK_MESH_PAYLOAD_IDLE", 1200);
        if (need > g_mesh_payload_size / 4) g_mesh_payload_used = f;
        int shrink = g_mesh_payload_h && idle > 0 && g_mesh_payload_size > MESH_PAYLOAD_MIN && f - g_mesh_payload_used > (uint64_t)idle;
        if (need > g_mesh_payload_size || shrink) {
            VkDeviceSize n = need < MESH_PAYLOAD_MIN ? MESH_PAYLOAD_MIN : need;
            vka_span s; void *h = vka_static_block(n, &s); if (!h) return 0;
            if (VKA_ENV("DM_VK_MESH_LOG", 0)) VLOG("task/mesh payload block: %llu KiB -> %llu KiB (%u words x %u tasks)", (unsigned long long)(g_mesh_payload_size >> 10), (unsigned long long)(n >> 10), payload_words, tasks);
            if (g_mesh_payload_h) vka_after(vka_fence_last() + 1, mesh_block_free, g_mesh_payload_h);
            g_mesh_payload = s; g_mesh_payload_size = n; g_mesh_payload_h = h; g_mesh_payload_used = f;
        }
    }
    if (!g_mesh_offsets.buf && !vka_static(65536u * 8, V.sb_align, &g_mesh_offsets)) return 0;
    return 1;
}

static int mesh_control_init(void) {
    if (g_mesh_control_pipeline) return 1;
    VkDescriptorSetLayoutBinding bindings[4];
    for (unsigned i = 0; i < 4; i++) bindings[i] = (VkDescriptorSetLayoutBinding){DMSH_VK_MESH_PAYLOAD + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    vka_layout *l = &g_mesh_control_layout;
    l->push = V.push_desc && V.max_push >= 4; l->ndesc = 4;
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0, .bindingCount = 4, .pBindings = bindings};
    if (vk.vkCreateDescriptorSetLayout(V.dev, &di, NULL, &l->set0)) return 0;
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &l->set0,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    if (vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &l->layout)) return 0;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_mesh_control_spv, .pCode = vka_mesh_control_spv};
    if (vk.vkCreateShaderModule(V.dev, &mi, NULL, &mod)) return 0;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = l->layout,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"}};
    VkResult r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_mesh_control_pipeline);
    vk.vkDestroyShaderModule(V.dev, mod, NULL);
    if (r) { VRED("task/mesh fallback controller pipeline: %d", r); return 0; }
    VLOG("task/mesh fallback: compute -> bounded output batches -> pulling vertex stage; %llu MiB output buffer", (unsigned long long)(g_mesh_output_size >> 20));
    return 1;
}

// lead: the barrier before the controller; 0 where the command just before it is the fallback's own barrier (DM_VK_MESH_BARRIERS=0: always). A pass ended here with a
// full barrier needs no other. One ended with the attachment-scoped barrier ("Pass-end barriers") does: that barrier's source has no transfer stage, and the report copy
// (mesh_rep_copy) may have read g_mesh_info by transfer before the pass, which the controller now rewrites (a write after a read: mesh_barrier has the transfer stage)
// The fallback's work per frame (vka_mesh_stats, every 300 frames with the replay's statistics): emulated draws by kind (a task shader or not, the count direct or read by the
// GPU), their output batches, and the controller and task/mesh compute dispatches they record
static uint64_t g_mesh_n[4], g_mesh_batches, g_mesh_ctl, g_mesh_cs, g_mesh_fused, g_mesh_compacted;
void vka_mesh_stats(char *b, size_t n, double frames) {
    static uint64_t n0[4], b0, c0, s0, f0, k0;
    uint64_t d = g_mesh_n[0] + g_mesh_n[1] + g_mesh_n[2] + g_mesh_n[3] - n0[0] - n0[1] - n0[2] - n0[3];
    if (n) b[0] = 0;
    if (frames > 0 && d)
        snprintf(b, n, "task/mesh fallback per frame: %.1f draws (%.1f direct and %.1f indirect without a task shader, %.1f direct and %.1f indirect with one), %.1f output batches, "
                 "%.1f controller and %.1f task/mesh compute dispatches; %.1f draws with the controller's phases in one dispatch (DM_VK_MESH_FUSE), %.1f batches compacted (DM_VK_MESH_COMPACT)",
                 d / frames, (g_mesh_n[0] - n0[0]) / frames, (g_mesh_n[1] - n0[1]) / frames, (g_mesh_n[2] - n0[2]) / frames, (g_mesh_n[3] - n0[3]) / frames,
                 (g_mesh_batches - b0) / frames, (g_mesh_ctl - c0) / frames, (g_mesh_cs - s0) / frames, (g_mesh_fused - f0) / frames, (g_mesh_compacted - k0) / frames);
    memcpy(n0, g_mesh_n, sizeof n0); b0 = g_mesh_batches; c0 = g_mesh_ctl; s0 = g_mesh_cs; f0 = g_mesh_fused; k0 = g_mesh_compacted;
}
static int mesh_control(uint32_t phase, uint32_t base, const VkDescriptorBufferInfo *source, int lead) {
    int was_on = R.on; vka_rp_end(); g_mesh_ctl++;
    if (!VKA_ENV("DM_VK_MESH_BARRIERS", 1) || (lead && !(was_on && rp_end_was_full()))) mesh_barrier();
    const vka_span *s[3] = {&g_mesh_payload, &g_mesh_offsets, &g_mesh_info};
    VkDescriptorBufferInfo bi[4]; VkWriteDescriptorSet wr[4];
    VkDeviceSize sizes[3] = {g_mesh_payload_size, 65536u * 8, MESH_INFO};
    for (unsigned i = 0; i < 4; i++) {
        bi[i] = i == 3 ? *source : (VkDescriptorBufferInfo){s[i]->buf, s[i]->off, sizes[i]};
        wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = DMSH_VK_MESH_PAYLOAD + i,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]};
    }
    const vka_layout *l = &g_mesh_control_layout;
    if (l->push) vk.vkCmdPushDescriptorSetKHR(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, l->layout, 0, 4, wr);
    else {
        VkDescriptorSet set = set_alloc(l->set0); if (!set) return 0;
        for (unsigned i = 0; i < 4; i++) wr[i].dstSet = set;
        vk.vkUpdateDescriptorSets(V.dev, 4, wr, 0, NULL);
        vk.vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, l->layout, 0, 1, &set, 0, NULL);
    }
    g_pd[1].n = 0;   // (set 0 replaced by the control pass's own: the draws' last compute push is gone, vka_draw.c push_same)
    uint32_t push[2] = {phase, base};
    vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mesh_control_pipeline);
    vk.vkCmdPushConstants(g_cb, l->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vk.vkCmdDispatch(g_cb, 1, 1, 1); mesh_barrier();
    return 1;
}

// ---------- Batch compaction (DM_VK_MESH_COMPACT, live, default 0) ----------
// The pulling draw of a batch ran every corner of every primitive every group could have emitted: groups x max primitives x corners (8,300,502 vertex invocations for
// one of Ocarina of Time's batches), each loading its group's count to find most of them dead. With the compaction a small compute (selftest/mesh_compact.comp) reads
// each group's count and appends a draw of only its primitives' corners, and the batch is drawn with vkCmdDrawIndirectCount. A mesh shader whose count offset is not
// known (vka_shader.c mesh_count_offset), or a device without indirect count, keeps the old draw.
// Off by default: measured slower where it was meant to help. Ocarina of Time by Kokiri Forest's scarecrow on the POCO F7 Ultra (Adreno 830, 891.14, 2026-11-15): 15.2 fps
// against 17.1, GPU 59.5 ms against 52, the passes 35 ms against 27: one indirect draw a live group costs the GPU more than the dead corners it skips, which only load a
// count and branch. The picture was right. Kept for a GPU whose draws are cheaper (Mali, Xclipse: not measured). 1 = compact
static vka_layout g_mesh_compact_layout;
static VkPipeline g_mesh_compact_pipeline;
static int mesh_compact_init(void) {
    static int failed;
    if (g_mesh_compact_pipeline) return 1;
    if (failed || !V.indirect_count || !vk.vkCmdDrawIndirectCount) return 0;
    failed = 1;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16 + (VkDeviceSize)MESH_CMDS * 16,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vk.vkCreateBuffer(V.dev, &bi, NULL, &g_mesh_cmds)) return 0;
    VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, g_mesh_cmds, &mr);
    uint32_t mt = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); if (mt == UINT32_MAX) mt = vka_mem_type(mr.memoryTypeBits, 0, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    if (mt == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &g_mesh_cmds_mem) || vk.vkBindBufferMemory(V.dev, g_mesh_cmds, g_mesh_cmds_mem, 0)) {
        vk.vkDestroyBuffer(V.dev, g_mesh_cmds, NULL); g_mesh_cmds = VK_NULL_HANDLE; return 0; }
    VkDescriptorSetLayoutBinding bindings[3];
    for (unsigned i = 0; i < 3; i++) bindings[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    vka_layout *l = &g_mesh_compact_layout;
    l->push = V.push_desc && V.max_push >= 3; l->ndesc = 3;
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0, .bindingCount = 3, .pBindings = bindings};
    if (vk.vkCreateDescriptorSetLayout(V.dev, &di, NULL, &l->set0)) return 0;
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &l->set0, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    if (vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &l->layout)) return 0;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_mesh_compact_spv, .pCode = vka_mesh_compact_spv};
    if (vk.vkCreateShaderModule(V.dev, &mi, NULL, &mod)) return 0;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = l->layout,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"}};
    VkResult r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_mesh_compact_pipeline);
    vk.vkDestroyShaderModule(V.dev, mod, NULL);
    if (r) { VRED("task/mesh fallback compaction pipeline: %d", r); return 0; }
    failed = 0;
    VLOG("task/mesh fallback: batches compacted to the groups' own primitives (vkCmdDrawIndirectCount, up to %u draws a batch; DM_VK_MESH_COMPACT)", (unsigned)MESH_CMDS);
    return 1;
}
// After the batch's mesh compute and its barrier: the count word zeroed, the groups' draws appended, and a barrier before the pulling draw reads them
static int mesh_compact(const vka_shader *ms, uint32_t capacity, uint32_t prims, uint32_t topology) {
    uint32_t stride = ms->e->vin_mask;
    if (!VKA_ENV("DM_VK_MESH_COMPACT", 0) || ms->mesh_count_off <= 0 || (stride & 3) || ((uint32_t)ms->mesh_count_off & 3) || capacity > MESH_CMDS || !mesh_compact_init()) return 0;
    vk.vkCmdFillBuffer(g_cb, g_mesh_cmds, 0, 16, 0);
    // (the fill after the batch's mesh compute and its mesh_barrier, which ordered the last batch's draw, its indirect read, before it; the barrier below orders the fill
    // before the atomics)
    mesh_barrier();
    VkDescriptorBufferInfo bi[3] = {{g_mesh_output.buf, g_mesh_output.off, g_mesh_output_size}, {g_mesh_info.buf, g_mesh_info.off, MESH_INFO}, {g_mesh_cmds, 0, 16 + (VkDeviceSize)MESH_CMDS * 16}};
    VkWriteDescriptorSet wr[3];
    for (unsigned i = 0; i < 3; i++) wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]};
    const vka_layout *l = &g_mesh_compact_layout;
    if (l->push) vk.vkCmdPushDescriptorSetKHR(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, l->layout, 0, 3, wr);
    else {
        VkDescriptorSet set = set_alloc(l->set0); if (!set) return 0;
        for (unsigned i = 0; i < 3; i++) wr[i].dstSet = set;
        vk.vkUpdateDescriptorSets(V.dev, 3, wr, 0, NULL);
        vk.vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, l->layout, 0, 1, &set, 0, NULL);
    }
    g_pd[1].n = 0;   // (set 0 replaced: the draws' last compute push is gone, as after mesh_control)
    uint32_t pc[4] = {stride / 4, (uint32_t)ms->mesh_count_off / 4, prims, topology};
    vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mesh_compact_pipeline);
    vk.vkCmdPushConstants(g_cb, l->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vk.vkCmdDispatch(g_cb, (capacity + 63) / 64, 1, 1); g_mesh_cs++;
    mesh_barrier();
    g_mesh_compacted++; g_mesh_compact_max = capacity;
    return 1;
}

static void mesh_report(void *arg) {
    const uint32_t *p = arg;
    if (p[17]) VRED("task/mesh fallback refused unsupported count (flags %#x, %u tasks, %u requested mesh workgroups)", p[17], p[10], p[18]);
    if (VKA_ENV("DM_VK_MESH_LOG", 0)) VLOG("task/mesh completed: %u tasks, %u generated mesh groups, flags %#x, diagnostic loop counter %u", p[10], p[18], p[17], p[3]);
    if (VKA_ENV("DM_VK_MESH_TRACE_GLOBAL", 0) && p[8]) VLOG("task/mesh diagnostic raw misses %u, first guest address %#llx", p[8], (unsigned long long)p[6] | (unsigned long long)p[7]<<32);

}
static void mesh_probe_print(const uint32_t *i, uint64_t key) {
    if (i[12]) VLOG("mesh raw diagnostic %016llx: %u unresolved reads, first guest address %#llx", (unsigned long long)key, i[12], (unsigned long long)i[10] | (unsigned long long)i[18]<<32);
}
typedef struct { const uint32_t *info; uint64_t key; } mesh_probe;
static void mesh_probe_report(void *arg) { mesh_probe *p = arg; mesh_probe_print(p->info, p->key); free(p); }

// ---------- The draw's own record for its report ----------
// g_mesh_info is scratch of the submission being recorded, and vka_fence_last() + 1 can name a later submission than this one (the worker reserves fences before replay):
// a report read there could see the span already reused, a spurious or a missed "refused" line. DM_VK_MESH_REPORT_COPY (live, default 1): the GPU copies the 80 bytes into a
// ring slot of the fallback's own, which stays the draw's until its report has read it (a late report reads the right bytes). 0 = the old read of the scratch span
enum { MESH_NREP = 1024 };
static vka_span g_mesh_rep;
static _Atomic uint8_t g_mesh_rep_busy[MESH_NREP];
static unsigned g_mesh_rep_next;
static int mesh_rep_take(void) {
    if (!g_mesh_rep.buf && !vka_static((VkDeviceSize)MESH_NREP * 80, V.sb_align, &g_mesh_rep)) { VRED_ONCE("task/mesh fallback: no report ring; the draws are not reported (and keep the full batch range)"); return -1; }
    for (unsigned k = 0; k < MESH_NREP; k++) {
        unsigned i = (g_mesh_rep_next + k) % MESH_NREP;
        if (!atomic_exchange(&g_mesh_rep_busy[i], 1)) { g_mesh_rep_next = i + 1; return (int)i; }
    }
    VRED_ONCE("task/mesh fallback: more than %u reports in flight; the next ones are not read (and their draws keep the full batch range)", MESH_NREP);
    return -1;
}
static void mesh_rep_give(int slot) { if (slot >= 0) atomic_store_explicit(&g_mesh_rep_busy[slot], 0, memory_order_release); }
static void mesh_rep_copy(int slot) {   // outside a pass, after the controller's barrier
    VkBufferCopy cp = {g_mesh_info.off, g_mesh_rep.off + (VkDeviceSize)slot * 80, 80};
    vk.vkCmdCopyBuffer(g_cb, g_mesh_info.buf, g_mesh_rep.buf, 1, &cp);
    VkBufferMemoryBarrier hb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = g_mesh_rep.buf, .offset = cp.dstOffset, .size = 80};
    vk.vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
}

// ---------- How many output batches a draw records ----------
// A draw whose mesh-group total only the GPU knows (a task shader's, or an indirect count) recorded batches over all 65535 groups: with Ocarina of Time's capacities (5,475 to
// 22,075 groups) 3 to 12 pass breaks a draw, where its logged totals are at most 1,124 groups. DM_VK_MESH_LEARN (live, default 1; needs the report copy): a task/mesh shader
// pair first drawn over the full range then records batches for 4 times the largest total its reports showed (at least one batch). A total over that is still drawn up to
// the batches recorded, logged in red once, and the pair goes back to the full range for good. 0 = the full range always
typedef struct { uint64_t key; uint32_t max; uint8_t set, full; } mesh_seen;
enum { MESH_NSEEN = 4096 };
static mesh_seen g_mesh_seen[MESH_NSEEN];
static unsigned g_mesh_nseen;
static pthread_mutex_t g_mesh_seen_mu = PTHREAD_MUTEX_INITIALIZER;
static mesh_seen *mesh_seen_at(uint64_t key, int add) {   // under g_mesh_seen_mu; NULL: not there (or the table is full)
    if (!key) key = 1;
    for (unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 52) & (MESH_NSEEN - 1), k = 0; k < MESH_NSEEN; k++, i = (i + 1) & (MESH_NSEEN - 1)) {
        if (g_mesh_seen[i].key == key) return &g_mesh_seen[i];
        if (!g_mesh_seen[i].key) {
            if (!add || 2 * (g_mesh_nseen + 1) > MESH_NSEEN) return NULL;
            g_mesh_nseen++; g_mesh_seen[i] = (mesh_seen){key}; return &g_mesh_seen[i];
        }
    }
    return NULL;
}
static uint32_t mesh_plan(uint64_t key) {   // the groups a GPU-counted draw is recorded for
    uint32_t cover = 65535;
    pthread_mutex_lock(&g_mesh_seen_mu);
    mesh_seen *e = mesh_seen_at(key, 1);
    if (e && e->set && !e->full) { uint64_t w = 4ull * e->max; cover = w < 1 ? 1 : w < 65535 ? (uint32_t)w : 65535; }
    pthread_mutex_unlock(&g_mesh_seen_mu);
    return cover;
}
typedef struct { int slot, trace, learn; uint32_t cover; uint64_t key, tkey, mkey; } mesh_rec;
static void mesh_report_rec(void *arg) {   // the completion thread, once the draw's submission is done
    mesh_rec *r = arg; uint32_t p[20];
    memcpy(p, g_mesh_rep.ptr + (size_t)r->slot * 80, 80); mesh_rep_give(r->slot);
    mesh_report(p);
    if (r->trace) mesh_probe_print(p, r->mkey);
    if (r->learn) {
        int over = 0; uint32_t total = p[18];
        pthread_mutex_lock(&g_mesh_seen_mu);
        mesh_seen *e = mesh_seen_at(r->key, 0);
        if (e) {
            if ((total > r->cover || (p[17] & 2)) && r->cover < 65535 && !e->full) { e->full = 1; over = 1; }
            if (!e->set || total > e->max) e->max = total;
            e->set = 1;
        }
        pthread_mutex_unlock(&g_mesh_seen_mu);
        if (over) VRED("task/mesh fallback: %u mesh groups, over the %u its batches were recorded for (task %016llx mesh %016llx): that draw lost the rest; the pair is drawn over the full range from now on",
                       total, r->cover, (unsigned long long)r->tkey, (unsigned long long)r->mkey);
    }
    free(r);
}

static int mesh_compute(vka_dstate *d, vka_shader *s, int stage) {
    vka_shader *sh[VKA_NSTAGE] = {0}; sh[stage] = s;
    vka_layout lay;
    // Keep stage 6/7 for c0/c2 and uniform/storage tables; the layout exposes them to compute.
    if (!vka_layout_of(sh, 0, &lay)) return 0;
    walk_all(d, sh);
    if (make_images(d, sh, 1)) { vka_rp_end(); make_images(d, sh, 0); }
    VkPipeline p = vka_cs_pipeline(s, &lay); if (!p) return 0;
    if (!bind_sets(d, sh, &lay, VK_PIPELINE_BIND_POINT_COMPUTE, NULL)) return 0;
    vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vk.vkCmdDispatchIndirect(g_cb, g_mesh_info.buf, g_mesh_info.off); g_mesh_cs++;
    if (vka_gp_active) vka_gp_compute(s->key, 0, 0, 0, 1);   // DM_VK_GPUPROF: the step the barrier below ends names the task or mesh program it ran
    // The task and mesh shaders run here as compute with the game's own bindings, so they may store to its storage buffers and images and to raw global memory. mesh_barrier
    // covers only the fallback's four buffers: those guest writes were made visible only by the full barrier a later pass ended with, and not at all to the pulling
    // fragment stage of the same draw. A batch that may write guest memory (draw_writes, over the walk above: any storage image counts, the translator's write flag on
    // images is not followed through arrays) gets a full barrier in place of mesh_barrier, which it covers (every buffer, every stage): the batch's barrier is widened,
    // not doubled. It makes those writes visible to everything after, and keeps the invariant the attachment-scoped pass end relies on ("Pass-end barriers").
    // DM_VK_MESH_GUEST_BAR=0 (live, default 1; bisection only: with DM_VK_RP_SCOPE on it leaves those writes unpublished) records mesh_barrier alone, as before
    if (draw_writes(sh, d) && VKA_ENV("DM_VK_MESH_GUEST_BAR", 1)) { g_mesh_guest_bar++; vka_barrier(g_cb); }
    else mesh_barrier();
    return 1;
}

static int mesh_emulate(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw *dw) {
    vka_shader *ts = NULL, *ms = NULL, *fs = NULL; unsigned bad = 0;
    const prog_t *mp = d->bst[7];
    vka_shader *m2 = NULL; unsigned b2 = 0;
    if (d->bst[1] && d->bst[1] != mp && resolve(d->bst[1], 7, &m2, &b2) && (m2 || (b2 & (1u << 7)))) mp = d->bst[1];
    if (!resolve(mp, 6, &ts, &bad) || !resolve(mp, 7, &ms, &bad) || !resolve(d->bst[1], 1, &fs, &bad) || bad || !ms || !ms->mesh_vertex) return 0;
    // Replay bisection only: omitted geometry is explicitly not a visual fix.
    int diag_limit = VKA_ENV("DM_VK_MESH_DIAG_LIMIT", 0);
    if (diag_limit > 0) {
        static uint64_t frame = UINT64_MAX; static unsigned ordinal;
        if (frame != vka_frame()) { frame = vka_frame(); ordinal = 0; }
        if (++ordinal > (unsigned)diag_limit) return 1;
        VLOG("task/mesh diagnostic ordinal %u of limit %d", ordinal, diag_limit);
    }
    // DM_VK_MESH_BATCH_FIT (live, default 1): a direct draw without a task shader has exactly count mesh groups, so its batches end there, and a direct count of 0 draws
    // nothing at all (no controller, no pass break). 0 = batches over all 65535 groups, as before
    int fit = VKA_ENV("DM_VK_MESH_BATCH_FIT", 1);
    if (fit && !dw->ind && dw->count == 0) return 1;
    uint32_t stride = ms->e->vin_mask, prims = ms->e->reserved >> 16 & 511;
    uint32_t topology = ms->e->reserved >> 25 & 1 ? 1 : 3;
    uint32_t pw = ms->e->reserved & 65535, tpw = ts ? ts->e->reserved & 65535 : 0;
    if (tpw > pw) pw = tpw;
    uint32_t tasks = dw->ind || dw->count < 0 ? 65535 : (uint32_t)dw->count;
    if (!stride || !prims || !mesh_buffers(pw, tasks) || !mesh_control_init() || !vka_scratch(MESH_INFO, V.sb_align, &g_mesh_info)) return 0;
    uint32_t capacity = (uint32_t)(g_mesh_output_size / stride);
    if (capacity > V.props.limits.maxComputeWorkGroupCount[0]) capacity = V.props.limits.maxComputeWorkGroupCount[0];
    if (!capacity) return 0;
    if (VKA_ENV("DM_VK_MESH_LOG", 0)) VLOG("task/mesh record: task %016llx mesh %016llx, count %d first %d indirect %#llx, payload %u words, stride %u, batch capacity %u",
        (unsigned long long)(ts ? ts->key : 0), (unsigned long long)ms->key, dw->count, dw->first, (unsigned long long)dw->ind, pw, stride, capacity);
    // The first emulated draws leave a trail in the log, always (four draws, and the submissions around them through DM_VK_SYNCLOG's lines): a phone where the game is
    // killed from outside in the first frame that draws through the fallback (a Redmagic 11 Air, Adreno 830 on 891.14, 2026-11-14: signal 9 right after the fallback's
    // first line, every pipeline made, 5 GiB available) shows by its last line whether the draw was recorded, handed to the GPU, or finished there
    { static _Atomic int crumbs; int cr = atomic_fetch_add(&crumbs, 1);
      if (cr < 4) {
          extern _Atomic int vka_synclog_n;
          if (!cr && atomic_load(&vka_synclog_n) <= 0) atomic_store(&vka_synclog_n, 8);
          VLOG("task/mesh fallback draw %d recorded: task %016llx mesh %016llx, count %d%s, payload %u words, stride %u, batch capacity %u, payload block %llu KiB; its submission's fence is %llu or later",
               cr + 1, (unsigned long long)(ts ? ts->key : 0), (unsigned long long)ms->key, dw->count, dw->ind ? " (indirect)" : "", pw, stride, capacity,
               (unsigned long long)(g_mesh_payload_size >> 10), (unsigned long long)(vka_fence_last() + 1));
          vka_after(vka_fence_last() + 1, mesh_crumb_done, (void *)(intptr_t)(cr + 1));
      } }
    uint32_t *ip = (uint32_t *)g_mesh_info.ptr; memset(ip, 0, MESH_INFO);
    ip[11] = pw; ip[12] = stride / 4; ip[13] = prims; ip[14] = topology; ip[15] = capacity; ip[16] = ts != NULL;
    int trace_mesh = VKA_ENV("DM_VK_MESH_TRACE_MS", 0);
    if (trace_mesh) ip[12] = 0;   // unused by production modules/controller; diagnostic counter only
    VkDescriptorBufferInfo source;
    if (dw->ind) {
        VkBuffer buffer; VkDeviceSize offset;
        if (!vka_addr(dw->ind, 8, &buffer, &offset, NULL) || (offset & 3)) return 0;
        // Storage-buffer offsets must also respect the device alignment. Preserve the
        // guest record with a GPU copy into aligned scratch rather than reading it early.
        // DM_VK_MESH_IND_DIRECT (live, default 1): a record at an offset the storage-buffer alignment allows is bound where it is, so the controller reads it at the same
        // point of the command stream the copy did, without the copy and its second full barrier (37 indirect draws a frame in Ocarina of Time's Kokiri Forest, each with
        // a pass end, two full barriers and a copy: the plan of 2026-11-15, "pass ends by site"). The full barrier before stays: whatever wrote the record (a dispatch, a
        // draw's store) is done before the controller reads it. 0 = always the copy, as before
        static _Atomic uint64_t n_direct, n_copied; int direct = (offset % V.sb_align) == 0 && VKA_ENV("DM_VK_MESH_IND_DIRECT", 1);
        uint64_t nd = direct ? atomic_fetch_add(&n_direct, 1) + 1 : atomic_load(&n_direct), nc = direct ? atomic_load(&n_copied) : atomic_fetch_add(&n_copied, 1) + 1;
        if (nd + nc == 64 || nd + nc == 65536) VLOG("task/mesh fallback indirect records: %llu bound where they are (aligned), %llu copied (DM_VK_MESH_IND_DIRECT)", (unsigned long long)nd, (unsigned long long)nc);
        vka_rp_end(); vka_barrier(g_cb);
        if (direct) source = (VkDescriptorBufferInfo){buffer, offset, 8};
        else {
            vka_span src; if (!vka_scratch(8, V.sb_align, &src)) return 0;
            VkBufferCopy cp = {offset, src.off, 8}; vk.vkCmdCopyBuffer(g_cb, buffer, src.buf, 1, &cp); vka_barrier(g_cb);
            source = (VkDescriptorBufferInfo){src.buf, src.off, 8};
        }
    } else {
        if (dw->count < 0 || dw->first < 0) return 0;
        vka_span src; if (!vka_scratch(8, V.sb_align, &src)) return 0;
        uint32_t p[2] = {(uint32_t)dw->count, (uint32_t)dw->first}; memcpy(src.ptr, p, sizeof p);
        source = (VkDescriptorBufferInfo){src.buf, src.off, 8};
    }
    // The groups the batches cover: exact for a direct draw without a task shader (a count over 65535 is refused by the controller: no batch), learned for the others
    int copy = VKA_ENV("DM_VK_MESH_REPORT_COPY", 1), late = trace_mesh || VKA_ENV("DM_VK_MESH_TRACE_GLOBAL", 0);
    int slot = copy ? mesh_rep_take() : -1, learn = 0, copied = 0, reported = 0;
    uint64_t pair = ms->key ^ (ts ? ts->key * 0x9E3779B97F4A7C15ull : 0) ^ (dw->ind ? 0x5bd1e995ull : 0);
    uint32_t cover = 65535;
    if (fit && !ts && !dw->ind) cover = dw->count > 65535 ? 0 : (uint32_t)dw->count;
    else if (slot >= 0 && VKA_ENV("DM_VK_MESH_LEARN", 1)) { learn = 1; cover = mesh_plan(pair); }
    uint32_t nb = (uint32_t)(((uint64_t)cover + capacity - 1) / capacity);
    if ((uint64_t)nb * capacity < 65535) cover = nb * capacity; else cover = 65535;
    int ok = 0; g_mesh_aux = 1;
    int diag_stage = VKA_ENV("DM_VK_MESH_DIAG_STAGE", 0);
    VkBufferMemoryBarrier host = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=g_mesh_info.buf,.offset=g_mesh_info.off,.size=80};
    // DM_VK_MESH_FUSE (live, default 1): a draw without a task shader runs the controller's three phases for its first batch (the count, the groups, the batch's dispatch and
    // draw records) in one dispatch, phase 3 (selftest/mesh_control.comp), with one barrier after it instead of three: no task compute runs between them, so nothing else
    // reads or writes the fallback's buffers in between. Ocarina of Time's Kokiri Forest: 135 emulated draws a frame by the scarecrow, 538 fallback dispatches, 17.9 of the
    // frame's 53 ms of GPU time, most of it the GPU emptying around one-thread dispatches (the plan of 2026-11-15). 0 = the three phases one by one, as before
    int fuse = !ts && VKA_ENV("DM_VK_MESH_FUSE", 1);
    g_mesh_n[(ts ? 2 : 0) + (dw->ind ? 1 : 0)]++;
    if (fuse) {
        if (!mesh_control(3, 0, &source, 1)) goto out;
        g_mesh_fused++;
        if (diag_stage == -1) { ok = 1; goto out; }
    } else {
        if (!mesh_control(0, 0, &source, 1)) goto out;
        if (diag_stage == -1) { ok = 1; goto out; }
        if (ts && !mesh_compute(d, ts, 6)) goto out;
        if (!mesh_control(1, 0, &source, 0)) goto out;
    }
    reported = 1;
    // Only DM_VK_MESH_REPORT_COPY=0 (explicitly the old behaviour) reads the scratch span late. With the copy on and no slot (the ring full, or its buffer not made),
    // the draw has no report: the scratch block can be reused or freed (sblk_trim) before vka_fence_last() + 1 completes, the field crash in mesh_report
    if (!copy) {
        vk.vkCmdPipelineBarrier(g_cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
        vka_after(vka_fence_last() + 1, mesh_report, g_mesh_info.ptr);
        if (trace_mesh) {
            mesh_probe *probe = malloc(sizeof *probe);
            if (probe) { *probe = (mesh_probe){(const uint32_t *)g_mesh_info.ptr, ms->key}; vka_after(vka_fence_last() + 1, mesh_probe_report, probe); }
        }
    } else if (slot >= 0 && !late) { mesh_rep_copy(slot); copied = 1; }   // (the controller's totals are final here; the diagnostic counters only after the batches)
    // Diagnostic only, deliberately incomplete image: separate payload, compute and
    // pulling-stage failures in an exact-frame replay. Never a rendering fallback.
    if (diag_stage == 1 || diag_stage == 2) VRED_ONCE("task/mesh diagnostic stage %d: incomplete geometry, not a visual acceptance test", diag_stage);
    if (diag_stage == 1) { ok = 1; goto out; }
    for (uint32_t base = 0; base < cover; base += capacity) {
        // (the fused controller wrote the first batch's records: the report's copy only read them, so the batch needs no controller and no barrier before its compute)
        g_mesh_batches++;
        if ((!(fuse && !base) && !mesh_control(2, base, &source, base || copied)) || !mesh_compute(d, ms, 7)) goto out;
        if (diag_stage == 2) continue;
        g_mesh_compact_on = mesh_compact(ms, capacity, prims, topology);
        g_mesh_pull = ms->mesh_vertex; g_mesh_fragment = fs;
        p_draw draw = {topology == 1 ? NVN_DRAW_PRIMITIVE_POINTS : NVN_DRAW_PRIMITIVE_TRIANGLES, 0, (int)(capacity * prims * topology), 1, 0, 0};
        vka_draw(d, rt, has_rt, sc, has_sc, &draw, NULL);
        g_mesh_pull = g_mesh_fragment = NULL; g_mesh_compact_on = 0;
    }
    ok = 1;
out:
    if (trace_mesh) { vka_rp_end(); vk.vkCmdPipelineBarrier(g_cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL); }
    if (slot >= 0) {
        if (reported && !copied) { vka_rp_end(); mesh_barrier(); mesh_rep_copy(slot); copied = 1; }
        mesh_rec *r = copied ? malloc(sizeof *r) : NULL;
        if (r) { *r = (mesh_rec){slot, trace_mesh, learn, cover, pair, ts ? ts->key : 0, ms->key}; vka_after(vka_fence_last() + 1, mesh_report_rec, r); }
        else mesh_rep_give(slot);   // (a copy recorded without its report: the slot is only rewritten by a later copy, after this one in queue order)
    }
    g_mesh_aux = 0; g_mesh_pull = g_mesh_fragment = NULL; g_mesh_compact_on = 0; return ok;
}
