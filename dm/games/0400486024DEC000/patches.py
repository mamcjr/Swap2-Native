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
    screen_shape(ctx)   # dormant here: no ctx.others (below)

# Screen shape (DM_SCREEN_ASPECT), ported from upstream nxnative 47578ded (PR #3037): a screen taller than 16:9 (iPad 4:3, MacBook 16:10) gets a picture of its own shape.
# Dormant in this fork: upstream installs these hooks from its MATCH_FRAME_STEP rule, which hands the stub the screen_shape_functions it located (ctx.others) and takes the
# capability that offers Picture shape (ctx.capabilities). Our patch_nso.py runs FRAME_STEP by build ID without either, so screen_shape returns before its first site and the
# rewritten main is the same as without it. To use it here the rewriter needs MATCH_FRAME_STEP's others and capabilities, and the host loader the state page's screen-shape
# fields (+0x48 output height, +0x4c / +0x50 layout height, +0x5c (16/9) / ratio, +0x60 the projection tolerance, written from DM_SCREEN_ASPECT). Every hook runs the game's
# own code while +0x48 is 0, so a phone (wider than 16:9) keeps the 16:9 picture even then
# shape_anchor.c built with the command in its header comment (__text words, little-endian hex)
_shape_anchor = (
    '484840b908530034405c40bd01103e1e0028211ee890a8520101271e0108211e2820201e0d520054203840bd0890b8520201271e0208221e0390201e4020231e'
    '03d0231e42a4231ee9090054090080520a0c40f98a5000b44b0940f94b5000b4680140b9081153511f552571c14f00546c0d40f9ac0100b40d0080d2a8b89352'
    '8823b0726e3280520e20a0728f696d38ef00003408010f4a087d0e1bad050091bfc100f141ffff54080080524d8140f94a7942794c3d0012ae0500d14a3d4092'
    '0f1680524a7daf9b707491d2d045b7f2105dd4f270d1e5f20d000dcbbf010aebaa7dd09b4afd45d34db50f9bc23140faa03940fa4a018c1a8ddb86528d9bbb72'
    '1f010d6b8d060054addb86528d9bbb721f010d6b000900542d398c522d2abc721f010d6be0090054edfc8e52ede6af721f010d6b0116005402e4006f5f290071'
    '2d0f00545f3100712d1100545f350071e01f00545f390071a02200545f510071a11400540e0080526d2d9e52ad46b272300080520f89a852e301271e2f008052'
    'a300001403102c1e4328231e6800381e63b8a15e63d8215e4020231ee913881a02e4006f001c221e28791e121f150071a04500543f190071c1f4ff54223440bd'
    '03103c1e2108031f213400bd087cb75222020014edfe8b520df0b2721f010d6ba00500546d2e9d526d6cb6721f010d6b2110005402e4006f5f4501718c070054'
    '5f4100712d0d00545f450071a01f00545f010171a02000545f450171c10e00540e0080520f0080528d4881526d4fa1726f0000145f150071e10d00540e008052'
    '0daf81524d8aa272ef008052b000805202e4006f119fa8522302271e6c00001402e4006f5f0d00712c0200545f090071601400545f0d0071e10b00540e008052'
    'add98e52ad32af722f008052500080525e00001402e4006f5f150071c00100545f7d0071c0000054530000145f110071200100545f1d0071e10900540e008052'
    '0f00805250008052cdfe88528da4bc724e0000140e0080520f008052300080528d9c93520d56aa72480000145f710171ed0600545f750171001900545f790171'
    '001a00545f7d0171610700540e008052adf29452adb6b8722f00805210018052cd0000145f1100712c0100545f090071400f00545f0d0071e10500540e008052'
    '0d8d83526d63bb72770000145f150071e00e00545f190071e10400540e008052adfc9d52cd73bd72740000145f2d0071601000545f310071e10300540e008052'
    '6dc981522d53ae722f008052d0008052110090525192a8722302271e1199a8522202271e1a0000145f2d0071601100545f410071010200540e0080520f008052'
    'cd2a86522da6ae72090000145f490171c01000545f690171e10000540e0080520f008052ad488e52ed93a972f000805206000014100080520f0080520d008052'
    '2e00805202e4006f03e4006f5f010c6b10329f1ad01e0034713940b93f020c6bcc059f1a0c3200370ca840b98c6516539f010f6b813100546c1140f94c3100b4'
    '6b1540f90b3100b48e596af8ae0100b4110080d2acb893528c23b072623280520220a072c3697138e30000348c01034a8c7d021b310600913fc200f141ffff54'
    '0c0080529f010d6be12e00546b596af8ab2e00b46c3940b98c2907539f010f6b212e00546820201e80000054643140bd8020231e812d00544820201e80000054'
    '642d40bd8020221ee12c00541f1200718c0c00540a0e00515f090071231200541f060071e01700541f0a007101180054223440bd4238211ebc0000140e008052'
    '6daa83520d91b2723000805203e4006f2f008052beffff170e0080526dbf83526d83bb72af008052050000140e0080528d659d52cd13bd722f00805270008052'
    '519fa8522302271eb1ffff170e008052cdfb81522d73ae722f008052d0008052110090525192a8722302271e11009852319ba8722202271ea5ffff170e008052'
    '0d9781522d33ae722f008052d0008052110090525192a8722302271e11008852519aa8722202271e99ffff170e0080522d2e82522d93ae722f00805250008052'
    '110090525192a8722302271ed172a8522202271e8effff170e0080526de682528d22be720a0000140e0080520f008052cd8195520dc0b5723001805283ffff17'
    '0e008052cd4096524d03a9722f00805270008052100000140e0080524daf9052cdb8a5720a0000140e0080520f0080520d3f9b52cd7db1723001805273ffff17'
    '0e0080520d1e8e522d02b3722f00805290008052f1a0a8522302271e6cffff171f1e00712c0400541f160071e00900541f1a0071c10b00545f3100710b009052'
    '2b91b8726301271e8b57a8526401271e830c231e5f2d00710a009c52ca90a8724401271e830c231e0aaea8524401271e8218221e2440211e2518462d270040fd'
    '6514071f6410a75fc328241e250c062de390820f230000fd230840bd4208231e220800bd420000141f220071a00100541f260071c1070054223040bd4a8fa852'
    '4301271e4020231e2d070054223440bd4138211e213400bd0874b752cf000014220040bd4440211e4820201e844c221e221440bd4540211e4820201ea54c221e'
    'a518241eea4d82526a50a7724601271e8020261e04e4006fa4cc241e8020261e6d0400540510201e260c051feaa0a8524701271e251c051f1f120071c50c251e'
    'a318231e6318241e241040bd6408241e4208231e2408022d221840bd6208221e221800bd12000014045c40bd2228231e8020231e61000054025c00bd441ca24e'
    '8020221e01150054223440bd03103c1e2108031f213400bd0878b7529f000014223440bd2228221e223400bd3f0d0071201300543f11007180120054a97e9452'
    '292dbe721f01096b8c020054492e9d52696cb6721f01096b6c040054c9fe8b5209f0b2721f01096bad07005469dc9c52297eb3721f01096bac0c0054e9fe8b52'
    '09f0b2721f01096b6010005409d689528914b3727b00001429648952e9e4a6721f01096bec030054e9b688520961a5721f01096bad060054a9d79c52c955a672'
    '1f01096b2c0b005409b788520961a5721f01096ba00d0054c9609e5249fba57268000014a9a38752c9a0ba721f01096b2d06005409398c52292abc721f01096b'
    '2c0a0054c9a38752c9a0ba721f01096b200c0054a9f59652e9efba7250000014695a9e5209f9ab721f01096bad05005449c1845249fcad721f01096b6c090054'
    '895a9e5209f9ab721f01096be0090054897b9752e9c6ad724a000014c9d28552a919b0721f01096b6009005429369c52695bb0721f01096b80080054c9179b52'
    'a9e0b2723f000014c97e9452292dbe721f01096ba0070054693a9a520941bf721f01096b2007005429299d52c964a27234000014692e9d52696cb6721f01096b'
    '4006005469f49852e9d1b9721f01096b20060054c98e805229ecb9722900001449648952e9e4a6721f01096be004005489168352895ca8721f01096b60040054'
    '29e186526969aa721e00001489dc9c52297eb3721f01096be0030054e9af9452a95fb67217000014c9d79c52c955a6721f01096ba0020054a9f28f52a999a672'
    '0700001429398c52292abc721f01096b20020054c9118a52e975bc721f01096ba001005410000014e9fc8e52e9e6af721f01096b0001005469c1845249fcad72'
    '1f01096b01010054081e00511f050071c80000540868b7520101271e0028211e203800bdc0035fd6223440bd080090524884a8720301271e4020231e2a010054'
    '0310221e080090524884b8720401271e4020241ec80000544138211e020000142128221e213400bd0390221e0850b7520101271e6000011feaffff17'
)

screen_shape_functions = {
    'graphics init': {'size': 0x570, 'fingerprints': ['f2c5558922a471ab585d374cc9293013d48da1562d43169367efdc8ea90a3fe8']},   # main+0x1202ce0
    'scene app init': {'size': 0x273c, 'fingerprints': ['2227b93a9fa9dd0fd9aeb64359649df6e2ade519a8e47fa560f840efcdb5d7e7']},   # main+0x31f3890
    'base scene defaults': {'size': 0x3cc, 'fingerprints': ['4d34ae0b1d0fa05603a24e64aae393040502bdfda2fc053a5c3c02ffda42053e']},   # main+0x108b300
    'display mgr defaults': {'size': 0x4e0, 'fingerprints': ['871e4f9e24b2007d3a9ddbb061b6d1a3709c05f623b356daba35fe1dfeb2004c']},   # main+0x36b2bc0
    'base res read': {'size': 0x1a0, 'fingerprints': ['c49c68fad34b6fce5c0dc0f519f5ddda261ad56778917dc2a711553a10cf4db1']},   # main+0xe82f30
    'display target a': {'size': 0x58, 'fingerprints': ['125411e6b2df56ae4e8c0f9c16f134b4ed9247aca4e426c72b406b1f838bfbb9']},   # main+0xd565a0
    'display target b': {'size': 0x1a8, 'fingerprints': ['212c9a344038bdcf99e4a462ecbe1bb58a2b6374d364debd0d0a0664e1dc1175']},   # main+0xebf370
    'display target c': {'size': 0x188, 'fingerprints': ['0b669aaafc4c17cef5c96ac9e4054895152937e9c780f0c72893973256e9dca7']},   # main+0xf96310
    'layout ortho': {'size': 0x7b0, 'fingerprints': ['fed0ce1f2ad6c8f6118a974ec7d5d5a346194a4c10a0fc01f894cfec3d0a1b68']},   # main+0x1520c80
    'layout view ortho': {'size': 0x62c, 'fingerprints': ['103de33fb929cda858c2fbd57b726e6e1bca705bad66de872701819fbf5c6089']},   # main+0x773b90
    'perspective proj': {'size': 0xb8, 'fingerprints': ['e36f61aa83e94dfacc860ab4dd60dd1362cdab40fa490aee9e921a7ae0f3156d']},   # main+0x3485cc0
    'camera update': {'size': 0x1648, 'fingerprints': ['66a50abf2237dee694c40d6a2371071d62e91379d40b643869d680f597bfab02']},   # main+0x33a8c0
    'device proj': {'size': 0x3b0, 'fingerprints': ['ab5c2809cdbc7f9e30b966872350ac69301aa246a9169610d038ea9480bde910']},   # main+0x23b910
    'layout calc matrix': {'size': 0x208, 'fingerprints': ['0e057804c3c8c7d14ff833983335d834e0bd7505e05263e9c065472b9d206c74']},   # main+0x14e9980
    'pane global matrix': {'size': 0x46c, 'fingerprints': ['4eba5ac4bdf9d227e40612fa426d39fbe1abaf00fc069d5d6ba382809b5c5a5c']},   # main+0x152caa0
    # shape_anchor.c reads the array bounds and class fields written by these two functions.
    'layout pane array': {'size': 0x1d0, 'fingerprints': ['cbf1ceccfe3fca0e81ff2f0c4013e728f16c16de2ee4fc928ec2e9f848d2be8f']},   # main+0x1525d60
    'pane constructor': {'size': 0xfbc, 'fingerprints': ['48ccdf26b4c9a8212e6f4b169b89bb834b3c32f8804bb07a172f37b52f8535ef']},   # main+0x1529620
    'movie draw': {'size': 0x158, 'fingerprints': ['cc1ca04feb1ca4e553a6c7a4f36f99b13f56977185eea8ef5cd8dd3dbbdcdbcd']},  # main+0x3b5c710
}


def screen_shape(ctx):
    # Render sizes, projections, panes and movies must agree on the output shape.
    others = getattr(ctx, 'others', None) or {}
    if any(others.get(name) is None for name in screen_shape_functions):
        return
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    def patch_height(site, orig_hex, state_off, kind, why):
        fs_site(site, orig_hex)
        start = here()
        emit(int.from_bytes(bytes.fromhex(orig_hex), 'little'))
        emit(ADRP(16, here(), FS_STATE_OFF))
        emit(0xb9400210 | ((state_off // 4) << 10))   # ldr w16, [x16, #state_off]
        if kind == 'hi32_4_3':
            body = (0x340000b0,   # cbz w16, +20
                    0x531e7610,   # lsl w16, w16, #2
                    0x52800071,   # mov w17, #3
                    0x1ad10a10,   # udiv w16, w16, w17
                    0xb3607e08)   # bfi x8, x16, #32, #32
        elif kind == 'hi32':
            body = (0x34000050,   # cbz w16, +8
                    0xb3607e08)   # bfi x8, x16, #32, #32
        elif kind == 'w10':
            body = (0x34000050,   # cbz w16, +8
                    0x2a1003ea)   # mov w10, w16
        elif kind == 'f32_w8':
            body = (0x34000070,   # cbz w16, +12
                    0x1e230200,   # ucvtf s0, w16 (s0 overwritten at site+4)
                    0x1e260008)   # fmov w8, s0
        for w in body: emit(w)
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], why)

    # All 1920x1080, 2560x1440, 1280x720, and 1080.0f height sites across InitializeGraphicsSystem, scene defaults, and display manager.
    sites = [
        ('graphics init',        0x204, '08b4c0f2', 0x48, 'hi32_4_3', 'swapchain height = host oh * 4 / 3'),
        ('graphics init',        0x3c0, '8886e8f2', 0x50, 'hi32',     'virtual screen height = host layout height'),
        ('scene app init',       0x168, '08b4c0f2', 0x48, 'hi32_4_3', 'scene app RT height (2560x1440 -> 2560 x oh*4/3)'),
        ('scene app init',       0x174, '0887c0f2', 0x48, 'hi32',     'scene app RT height (1920x1080 -> 1920 x oh)'),
        ('base scene defaults',  0x38c, '0887c0f2', 0x48, 'hi32',     'base scene full height (1920x1080 -> 1920 x oh)'),
        ('base scene defaults',  0x39c, '085ac0f2', 0x4c, 'hi32',     'base scene layout height (1280x720 -> 1280 x lh)'),
        ('display mgr defaults', 0x00c, '0a878052', 0x48, 'w10',      'display mgr table height (1080 -> oh)'),
        ('base res read',        0x114, 'e890a852', 0x48, 'f32_w8',   'base res reference height (1080.0f -> (float)oh)'),
        ('display target a',     0x044, '0887c0f2', 0x48, 'hi32',     'display target height a (1920x1080 -> 1920 x oh)'),
        ('display target b',     0x110, '0887c0f2', 0x48, 'hi32',     'display target height b (1920x1080 -> 1920 x oh)'),
        ('display target c',     0x130, '0887c0f2', 0x48, 'hi32',     'display target height c (1920x1080 -> 1920 x oh)'),
    ]
    for key, off, orig_hex, state_off, kind, why in sites:
        patch_height(others[key] + off, orig_hex, state_off, kind, why)

    # Layout2 ortho projection builder: scales 16:9 canvas height (>= 1000 wide, e.g. 1920x1080 or 1280x720) by state +0x5c (k = (16/9) / aspect) so UI widgets keep 1:1 pixel aspect.
    # At +0x208 (orig ldr q6, [sp, #0xa0]), x16/x17, v0..v3 and nzcv are free; stores updated {W, H'} back to [sp, #0xa0] for both ortho blocks (x19+0xbf0 and x19+0xca0).
    ortho_fn = others['layout ortho']
    ortho_site = ortho_fn + 0x208
    fs_site(ortho_site, 'e62bc03d')   # ldr q6, [sp, #0xa0]
    start = here()
    emit(0x3dc02be6)
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11, 0x34000291, 0x52a88f51, 0x1e270222, 0x5e0404c0, 0x1e222000, 0x540001eb,
              0x5e0c04c1, 0x1e245002, 0x1e261003, 0x1e220800, 0x1e230822, 0x1e223800, 0x1e20c000,
              0x1e221002, 0x1e222000, 0x540000aa, 0xbd405e02, 0x1e220821, 0x6e0c0426, 0x3d802be6):
        emit(w)
    emit(B(here(), ortho_site + 4))
    fs_put(ortho_site, [B(ortho_site, start)], 'Layout2 ortho canvas height scaled by (16/9) / aspect when screen aspect is active')

    # Layout view draw (main+0x773b90): builds the screen-space OrthoProjection (-W/2..W/2, -H/2..H/2) at +0x398 (orig ldp s0, s1, [x0]) into the 0x90-byte UBO copied to Layout2.
    # Scales 16:9 viewport height s1 (W >= 1000) by state +0x5c (k = (16/9) / aspect) so 2D HUD widgets render 1:1 without vertical stretch. x16/x17, s2..s4 and nzcv are free there.
    vo_fn = others['layout view ortho']
    vo_site = vo_fn + 0x398
    fs_site(vo_site, '0004402d')   # ldp s0, s1, [x0]
    start = here()
    emit(0x2d400400)
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11, 0x34000211, 0x52a88f51, 0x1e270222, 0x1e222000, 0x5400018b,
              0x1e245002, 0x1e261003, 0x1e220804, 0x1e230822, 0x1e223884, 0x1e20c084,
              0x1e221002, 0x1e222080, 0x5400006a, 0xbd405e02, 0x1e220821):
        emit(w)
    emit(B(here(), vo_site + 4))
    fs_put(vo_site, [B(vo_site, start)], 'Layout view ortho height scaled by (16/9) / aspect when screen aspect is active')

    # sead::PerspectiveProjection::doUpdateMatrixImpl_ (main+0x3485cc0): for ~16:9 projections (|aspect - 16/9| <= 0.01), divides aspect by k (to host r = 1920/oh) and widens fovyTan by k (when <= state +0x60 tol) for true Vert+ 3D.
    proj_fn = others['perspective proj']
    proj_site = proj_fn
    fs_site(proj_site, '0084552d')   # ldp s0, s1, [x0, #0xac]
    start = here()
    emit(0x2d558400)
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11, 0x34000231, 0xbd405e03, 0xbd406204, 0x5291c731, 0x72a7fc71, 0x1e270225, 0x1e253825,
              0x1e20c0a5, 0x529ae151, 0x72a78471, 0x1e270226, 0x1e2620a0, 0x540000ac, 0x1e231821, 0x1e2420a0,
              0x5400004c, 0x1e230800):
        emit(w)
    emit(B(here(), proj_site + 4))
    fs_put(proj_site, [B(proj_site, start)], 'PerspectiveProjection aspect and Vert+ fovyTan scaled by (16/9) / aspect when screen aspect is active')
    # Camera projection update (main+0x33a8c0): inlined sead::PerspectiveProjection update at +0xf6c (main+0x33b82c, orig ldp s0, s1, [x2, #0xac]).
    # Scales aspect and fovyTan for ~16:9 projections (|aspect - 16/9| <= 0.01) so gameplay 3D view maintains 1:1 circular aspect on any screen ratio.
    cam_fn = others['camera update']
    cam_site = cam_fn + 0xf6c
    fs_site(cam_site, '4084552d')   # ldp s0, s1, [x2, #0xac]
    start = here()
    emit(0x2d558440)
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11, 0x34000231, 0xbd405e03, 0xbd406204, 0x5291c731, 0x72a7fc71, 0x1e270225, 0x1e253825,
              0x1e20c0a5, 0x529ae151, 0x72a78471, 0x1e270226, 0x1e2620a0, 0x540000ac, 0x1e231821, 0x1e2420a0,
              0x5400004c, 0x1e230800):
        emit(w)
    emit(B(here(), cam_site + 4))
    fs_put(cam_site, [B(cam_site, start)], 'Camera update projection aspect and Vert+ fovyTan scaled by (16/9) / aspect when screen aspect is active')

    # Device projection matrix (main+0x23b910): inlined sead::PerspectiveProjection update at +0x98 (main+0x23b9a8, orig ldp s0, s3, [x0, #0xac]).
    # Scales aspect (s3) and fovyTan (s0) for ~16:9 projections.
    dev_fn = others['device proj']
    dev_site = dev_fn + 0x98
    fs_site(dev_site, '008c552d')   # ldp s0, s3, [x0, #0xac]
    start = here()
    emit(0x2d558c00)
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11, 0x34000231, 0xbd405e07, 0xbd406206, 0x5291c731, 0x72a7fc71, 0x1e270222, 0x1e223862,
              0x1e20c042, 0x529ae151, 0x72a78471, 0x1e270221, 0x1e212040, 0x540000ac, 0x1e271863, 0x1e262040,
              0x5400004c, 0x1e270800):
        emit(w)
    emit(B(here(), dev_site + 4))
    fs_put(dev_site, [B(dev_site, start)], 'Device projection aspect and Vert+ fovyTan scaled by (16/9) / aspect when screen aspect is active')

    # The movie's draw (main+0x3b5c710, the title screen's background and the opening): its sead::Viewport at x29 - 0x28 (min x, y at +0x8, +0xc, max x, y at +0x10, +0x14) is
    # the whole frame buffer, so the 16:9 movie (1920x1080, 2560x1440 after the upscale) filled a 2560x1920 frame at 4:3, 1.33x taller (an oval moon). At +0x64, after the viewport
    # is built and before main+0x2846e0 applies it (viewport and scissor), a rect taller than 16:9 becomes 16:9 about its centre; the frame is already cleared to black above and
    # below. x16 / x17 and s0-s7 are free there (x8 / x9 are loaded by the original instruction, the next call's arguments after it)
    movie_fn = others['movie draw']
    movie_site = movie_fn + 0x64
    fs_site(movie_site, '69a241a9')   # ldp x9, x8, [x19, #0x18]
    start = here()
    emit(0xa941a269)
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11,   # ldr w17, [x16, #0x48]   oh (0 = 16:9)
              0x34000271,   # cbz w17, out
              0xd100a3b1,   # sub x17, x29, #0x28     the viewport
              0x2d410620,   # ldp s0, s1, [x17, #0x8]
              0x2d420e22,   # ldp s2, s3, [x17, #0x10]
              0x1e203844,   # fsub s4, s2, s0         width
              0x1e213865,   # fsub s5, s3, s1         height
              0x52a7e210,   # mov w16, #0x3f100000    9 / 16
              0x1e270206,   # fmov s6, w16
              0x1e260886,   # fmul s6, s4, s6         the 16:9 height
              0x1e2620a0,   # fcmp s5, s6
              0x5400012d,   # b.le out                not taller
              0x1e2638a7,   # fsub s7, s5, s6
              0x52a7e010,   # mov w16, #0x3f000000    0.5
              0x1e270204,   # fmov s4, w16
              0x1e2408e7,   # fmul s7, s7, s4         half the extra height
              0x1e272821,   # fadd s1, s1, s7
              0x1e273863,   # fsub s3, s3, s7
              0xbd000e21,   # str s1, [x17, #0xc]
              0xbd001623):  # str s3, [x17, #0x14]
        emit(w)
    emit(B(here(), movie_site + 4))   # out
    fs_put(movie_site, [B(movie_site, start)], 'Movie draw viewport kept 16:9 about its centre on a taller screen when screen aspect is active')


    # Layout2 pane matrix (main+0x14e9980): for fullscreen PicturePanes (width >= 1000, height >= 700, ~16:9), scales Y matrix row by state +0x5c (k = (16/9) / aspect) so full-screen faders cover the expanded canvas without letterboxing.
    calc_fn = others['layout calc matrix']
    calc_site = calc_fn + 0x19c
    fs_site(calc_site, '6042883c')   # stur q0, [x19, #0x84]
    start = here()
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9404a11, 0x34000351, 0xb940aab1, 0x53166631, 0x7100063f, 0x540002c1, 0x2d4b0ea2,
              0x52a88f51, 0x1e270224, 0x1e242040, 0x54000224, 0x52a885f1, 0x1e270224, 0x1e242060,
              0x540001a4, 0x1e245004, 0x1e261005, 0x1e240844, 0x1e250865, 0x1e253884, 0x1e20c084,
              0x52a86911, 0x1e270225, 0x1e252080, 0x5400006a, 0xbd405e02, 0x4f829000, 0x3c884260):
        emit(w)
    emit(B(here(), calc_site + 4))
    fs_put(calc_site, [B(calc_site, start)], 'Layout2 fullscreen picture panes scaled by (16/9) / aspect when screen aspect is active')

    # The HUD and the menu bars at the taller screen's edges (shape_anchor.c, which says what moves). The pane global matrix function (main+0x152caa0) has the new
    # matrix in v0-v3 at +0x3b0 on both paths. At 16:9 skip the stack and call. Otherwise save v0-v3, call shape_calc, reload v0-v3 and run the original instruction.
    # x0-x17 are written before they are read after this point, and x30 is reloaded by the epilogue. The helper leaves x18 alone.
    pane_fn = others['pane global matrix']
    site = pane_fn + 0x3b0
    fs_site(site, '68c20091')   # add x8,x19,#0x30
    start = here()
    original = start + 13 * 4
    emit(ADRP(16, here(), FS_STATE_OFF))
    emit(0xb9404a11)   # ldr w17, [x16, #0x48]
    emit(0x34000011 | ((original - here()) // 4 << 5))   # cbz w17, original
    for w in (0xd10103ff,    # sub sp,sp,#0x40
              0xad0007e0,    # stp q0,q1,[sp]
              0xad010fe2,    # stp q2,q3,[sp,#0x20]
              0xaa1303e0,    # mov x0,x19            the pane
              0x910003e1,    # mov x1,sp             its matrix (q0-q3)
              0xaa1003e2):  # mov x2,x16            the state page
        emit(w)
    emit(B(here(), original + 8) | 0x80000000)   # bl   # shape_calc follows the original instruction and its return branch
    for w in (0xad4007e0,    # ldp q0,q1,[sp]
              0xad410fe2,    # ldp q2,q3,[sp,#0x20]
              0x910103ff):  # add sp,sp,#0x40
        emit(w)
    assert here() == original
    emit(0x9100c268)                     # add x8,x19,#0x30      (the original)
    emit(B(here(), site + 4))
    for k in range(0, len(_shape_anchor), 8): emit(int.from_bytes(bytes.fromhex(_shape_anchor[k:k + 8]), 'little'))
    fs_put(site, [B(site, start)], 'Layout2 pane global matrix: HUD panes past the 16:9 edge region and the menu bars moved to the taller screen\'s edges with their subtrees (shape_anchor.c)')
    ctx.capabilities.add('SCREEN_ASPECT')   # the launcher's Picture shape (dm/games/settings.tsv fixups:SCREEN_ASPECT)


FRAME_STEP = {'e0f3ccfaf50065ae4ea01eb5aeb13b1a': frame_step}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'frame ratio from the host frame step', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame ratio, base / the fps its controller picked",
     'blocked': 'frame-step stubs have no function-matched form yet',
     'checked': 'v131072 main: 2818201e at +0x6e6cc4, the stub words and the liveness of s0/x16/x17 after the site, by disassembly'},
]
