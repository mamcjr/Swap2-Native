# The Legend of Zelda: Ocarina of Time (Switch 2, 0400486024DEC000): the rewriter applies these patches to this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py
# and applies the ones that match a module's NSO build ID. FRAME_STEP works as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# v131072 main (e0f3ccfaf50065ae4ea01eb5aeb13b1a). ModuleSystem::moduleCalc_ (main+0x6e6be0) runs once per frame through a sead delegate: main+0x3b59ea4 stores it at
# ModuleSystem+0x268 under the name string main+0x3c9e709. At main+0x6e6cc4 it computes the frame ratio s8 = s1 / s0, where:
#   s1 = 60.0, or 30.0 when bit 0 of framework+0x24f is set (fcsel at +0x6e6cc0). That bit is the 30-fps-base mode, set and cleared at main+0x346c8ac / +0x346c8b4.
#   s0 = the int at framework+0x2c8 (scvtf at +0x6e6ca8), the fps the game's own frame-rate controller picked (main+0x4dbda0, str at +0x4dbe08 after setFps main+0x4dc0f0): 30 on the Switch.
# The ratio is added to the frame counter double at +0xd58 and passed to main+0x3b4e300, which stores it, 1/ratio and 60/ratio as the frame's game-time delta.
# With the host presenting every vsync, s0 stayed 30, the ratio stayed 2 at 60 fps, and the game ran at twice real time.
# The stub replaces the game's fps with the rate the host really presents at, 60 / step: ratio = step * s1 / 60, so one frame advances step / 60 s in either base mode. Over time the steps
# add up to the vsyncs shown, so game time = real time. The stub also counts the vsyncs stepped. At target 0, and before the host has decided a step, it runs the game's own fdiv.
# Checked by disassembly of this build (2026-11-12, capstone): the original word 2818201e at main+0x6e6cc4, every stub word, and both cbz branches landing on the fallback.
# Every path from +0x6e6cc8 to the function's end writes s0/d0 (ldr d0 at +0x6e6d04, fmov s0,s8 at +0x6e6dcc) or reaches a call (main+0x50b20 / +0x50ea0, integer arguments only)
# before it reads s0, x16 or x17, so those registers are dead. The stub leaves NZCV alone, and w8/w9 (tested right after the site) too.
# Left as they were: procFrame (main+0x3b4ea90) still writes 60 / [framework+0x2c8] to main+0x4d41fc8 (no direct reader found), and the controller still asks for its own present interval.
# From erickmendes22/Swap2-Native 8021046 (the same template as Paper Mario: The Thousand-Year Door's stub)
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    site = 0x6e6cc4
    fs_site(site, '2818201e')                                        # fdiv s8,s1,s0
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps (float bits; 0 = off)
              0x340001b1,   # cbz w17,orig (+13)
              0xb9402211,   # ldr w17,[x16,#0x20]    step in vsyncs
              0x34000171,   # cbz w17,orig (+11)     not decided yet
              0x1e230220,   # ucvtf s0,w17           the step
              0xf9401a10):  # ldr x16,[x16,#0x30]
        emit(w)
    emit(0x8b110210)                                                 # add x16,x16,x17
    emit(ADRP(17, here(), FS_STATE_OFF)); emit(0xf9001a30)           # str x16,[x17,#0x30]    vsyncs stepped
    for w in (0x1e210808,   # fmul s8,s0,s1          step * base (60, or 30 in the 30-fps-base mode)
              0x52a84e10,   # mov w16,#0x42700000    60.0f
              0x1e270200,   # fmov s0,w16
              0x1e201908):  # fdiv s8,s8,s0          ratio = step * base / 60
        emit(w)
    emit(B(here(), site + 4))
    emit(0x1e201828); emit(B(here(), site + 4))                      # orig: fdiv s8,s1,s0; back
    fs_put(site, [B(site, start)], "frame step: the frame ratio is the host's vsyncs times the base rate / 60 (state page target fps; 0 = the game's own fps)")

FRAME_STEP = {'e0f3ccfaf50065ae4ea01eb5aeb13b1a': frame_step}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'frame ratio from the host frame step', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame ratio, base / the fps its controller picked",
     'blocked': 'frame-step stubs have no function-matched form yet',
     'checked': 'v131072 main: 2818201e at +0x6e6cc4, the stub words and the liveness of s0/x16/x17 after the site, by disassembly'},
]
