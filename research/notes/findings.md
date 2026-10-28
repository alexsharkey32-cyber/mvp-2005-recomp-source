# Findings (session 1, 2026-09-28)

Tool: `tools/eedis.py` (dependency-free EE disassembler, xref, callers, function finder — 29,100 function starts detected).

## Widescreen — the game already has it
EA left a working 16:9 path in the engine, but retail code hard-wires it off.

- `gDisplaySettings` (pointer @ `0x7DDEEC`) → struct; **`+0x224` = widescreen flag**, `+0x228` = related flag (TBD).
- 17 functions read `+0x224`. Examples:
  - `0x43B208` returns camera aspect **16/9 if set, else 4/3**.
  - `0x43B1C0` returns HUD X-scale 4/3 if set, else 1.0.
  - `0x47F050` (a camera setup) swaps its 1.333 aspect for 1.777 when set.
- The flag is zeroed in two places: `DisplaySettings_ctor` @ `0x4757D0` and `Display_Init` @ `0x476A24`.
- The Flash (Apt) UI asks for `gWidescreenMode`; `AptGetGlobalVar` (`0x29D3E8`) always answers the rodata string `"0"` (`0x86F5B0`). `"1"` exists at `0x870588`.
- → `patches/0544E001.pnach` flips all three. **Confirmed working in PCSX2.** **Confirmed working in PCSX2.**

### Menu fix (session 2)
- User report: widescreen works, menus "a little off".
- Cause: `Frontend_LoadAssetArchives` (`0x1DA438`) and three path builders (`0x1D91E8`, `0x1D94A0`, `0x1D9618`) switch to `frontend\wbkgnds.big` / `frontend\wstad.big` when widescreen is on (table @ `0x7D7FE0`). **Neither file exists on the retail disc** — EA cut the 16:9 menu art. Patch forces the 4:3 archives.
- Also: `FrontendScreen_BaseInit` (`0x205510`) sets flag `0x100` on the `BOTBAR` element in widescreen (likely hides the bottom bar). Left as-is for now.
- The Flash side only defines `WIDESCREEN` from `gWidescreenMode` in `main.big`; no movie reads it, so that part of the patch is harmless but inert.

## Resolution
- `Display_Init` (`0x476910`) calls `EAGLDevice_SetDisplaySize(512.0, 448.0)`, 24-bit color, 24-bit Z.
- For PCSX2, upscaling (Internal Resolution) already gives custom resolutions. For a future native port, this is the one place to change.

## Frame rate
- Debug menu (`0x4A3900`) exposes `Force VSync Off` → `gForceVSyncOff` (`0x7DDEE8`), `Dynamic Vsync` (`0x7DE860`), `Dynamic Vsync cutoff` (`0x7DE864`, compared against frame time ×100000).
- `Display_UpdateVSync` (`0x476E88`) computes `vsyncOff = gForceVSyncOff || (dynamic && frameTime > cutoff)` and calls `EAGLDevice_SetWaitVSync(dev, !vsyncOff)` (`0x54CE60` → device `+0x824`), which `EAGLDevice_Present` (`0x54A188`) honours.
- **Gameplay code uses a hard-coded 1/60 s step in 656 places** (`lui 0x3C88 / ori 0x8889`), plus 51 uses of 1/30. So simulation is tied to fixed ticks; uncapping vsync alone will change game speed, not add smooth frames. Real high-FPS needs the main loop decompiled (next target: callers of `0x476E88`, and whoever reads `0x7DDEDC/0x7DDEE0`, the frame-time accumulators).

### Main loop / timing map (session 2)
- `Game_StepAndTime` (`0x26A580`): one sim update per call; measures EE cycles (COP0 Count) → `gSimCycles` (`0x7DDEDC`). Render cycles → `gRenderCycles` (`0x7DDEE0`, set in `0x481F68`).
- Dynamic vsync compares `gSimCycles + gRenderCycles` against `cutoff × 100000` cycles (EE ≈ 294.912 MHz, so the unit is ~0.34 ms).
- `Clock_SetStepFrames` (`0x2EF7E8`): clock step = n × 1/60 s — time is measured in 60 Hz "frames". Callers found so far are replay controls (0 = pause).
- FPS: 6-sample ring buffer `gFpsSamples` (`0x8C8270`), average shown from `gFpsDisplay` (`0x8C8288`) when `gDebugDraw` (`0x7D8D14`) is on.
- **Open question:** does gameplay render at 30 fps (with 2 × 1/60 ticks) or at 60? Use the "Debug FPS counter" patch in-game to answer this. The high-FPS plan depends on it:
  - If 30 fps with 2 ticks/frame → find the tick count and make it 1 tick/frame at 60 fps (feasible patch).
  - If 60 fps already but dropping → it's a performance issue; PCSX2 EE overclock is the fix.

### Test result (session 2)
- In-game ~40 fps in PCSX2 and gameplay runs in **slow motion** → confirmed: the sim advances a fixed 1/60 s per rendered frame (no delta-time). Speed = fps/60.
- Fix path A (no code): PCSX2 EE Cycle Rate overclock so the game holds 60. **Confirmed: +1/+2 gives a near-locked 60 fps at normal speed.**
- Fix path B (code): make `Game_StepAndTime` run extra ticks when a frame overruns (catch-up), or convert the 656 hardcoded 1/60 steps to a real delta — needs the main loop decompiled first.

## Other notes
- Settings/debug strings: `PS2 Progressive Scan (BE CAREFUL!)` @ `0x8994A8`, `Show FPS` @ `0x8990C8` — both used in `0x5E5230` (frontend overlay debug menu).
- `$gp`-relative code is nearly absent (57 refs); compile flags probably `-G0` for game code.
- ELF CRC (PCSX2 style, XOR of words): `0x0544E001`.

## Recomp stage 1 (session 3)
- Recompiler: 29,834 functions, 6,834 jump-table labels, builds natively in ~1 min (-O1).
- Boot path reached: crt0 `0x100008` → SetupThread/SetupHeap → `_InitSys` (`0x5DFE00`, overridden: patches real kernel)
  → `main` `0x26F3A8` → static ctors (`.ctors` list at `0x7FE2C0`) → memory manager (`0x5527B8`) → SIF init.
- Stops at IOP: AddDmacHandler(SIF0) then SIF DMA of commands to IOP address 0.
- SIF command ids by function: `0x5DC444` (0x80000002/8/9/a/c → sceSifInitRpc), `0x5DCA80` (bind),
  `0x5DCC50` (call), `0x5DBCF8` (init cmd), `0x591E70`/`0x591FD0` (loadfile/iopheap: 0x80000002/4/5/6),
  `0x5BBA28..0x5BC028` (0x80000014–0x1d, probably cdvd/libsd), `0x5E0B98..` (0x80000010), `0x5E1068` (0x80000016).

## Recomp stage 2 (session 4) — IOP
- EE SIF library kept as recompiled; IOP emulated at the packet level. EE SIF0 receive buffer comes from
  INIT_CMD (opt 0) payload (`0x947B40`). Client RPC_END handler `0x5DC740`, SIF0 DMAC handler `0x5DC248`.
- Packet layouts: BIND sid@+32; CALL fno@+32 ssize@+36 recv@+40 rsize@+44 rmode@+48 serve@+52;
  RPC_END cid@+32 server@+36 buff@+40 cbuff@+44 (client/pkt fields +16..+31 echoed).
- Replies need ~60 µs latency, otherwise a prio-99 polling thread (LgKbM keyboard reader) starves main.
- Servers seen: 0x80000003 iopheap, 0x80000006 loadfile, 0x80000001 fileio, 0x80000592 cd init,
  0x80000593 cd S-cmd (0x22 mmode, 0x27, 0x04 error), 0x80000595 cd N-cmd (fno 1 read, 0x0e),
  0x8000059c, 0x80000100/101 padman, 0x80000400 mcserv, 0x61766973 LgKbM USB keyboard.
- The game has its own ISO9660 walker (reads PVD at LBN 16 then directory sectors).
- Bugs fixed on the way: MTSAB/QFSRV (unaligned memcpy), backward-branch poll wrapping, jump tables split
  across function pieces (now `_at` entry points), libkernel alarm syscalls 0xfc/0xfd.
- Overrides: `_InitSys` 0x5DFE00 (kernel patches), tty write 0x5DAFC8 -> host stderr.
- Render loop: VIF1 kick `0x548D20(tadr, flags)` from `0x5487A8`; list swap/flip `0x547390`;
  vblank handler `0x54D360` flips when VIF1 CHCR tag id == 7 (end).

## Recomp stage 3 (session 5) — graphics
- Bugs fixed: interrupts raised inside a handler must be deferred (DMAC GIF IRQ inside the vblank handler
  clobbered its stack); DMA CHCR[31:16] must hold the last tag (render loop polls tag id 7 = END);
  the IOP heap needs to reset on IOP reboot; code pointers in data must all be callable (entry labels).
- EAGL frame sync: VIF1 VIFcodes with the I bit raise INTC 5 and stall VIF1; GIF texture chains stop on IRQ
  tags (TIE). Handlers `0x547770` (VIF1) and `0x547898` (GIF DMAC) count both and restart GIF
  (D2_CHCR=0x184) + unstall VIF1 (VIF1_FBRST=8) when both arrive. `0x547998` = VIF1 DMAC handler.
- EA SND bank upload: user SIF cmd 0 (48-byte packets) -> reply user cmd 1 type 2 with opt = queue<<24;
  EE queues at 0x8F1510. The IOP buffer is 0x88000 (iopheap alloc 0x1000).
- Render targets: main 3D/UI target FBP 0x1180 (512 wide, CT32); final copy to display buffer
  FBP 0 (640x448, CT24) with a sprite; DISPFB1 = 0x9400.
- First displayed screen: "No memory card (PS2) inserted in MEMORY CARD slot 1..." over the title art.

## Session 6 — input, memory cards, getting to the menus
- libpad (EE 0x5C07D8..): 256-byte buffer per port = two 128-byte frames; IOP writes the older one.
  Game's pad state machine 0x103670 requires modeConfig>=2, mode table {4,7}, 2 actuators, model 3,
  button mask 0x3FFFF, and in the end data[1]==0x79 (DualShock 2 pressure mode). RPC results: cmd 1/0x12/0x13 at
  recv+12, SetMainMode (6) at +20, SetButtonInfo (0xA) at +16.
- libmc (EE 0x5BA768..): init needs versions >= 522/526; GetInfo info buffer = send+28 (type,+4 free,+144 format).
  Open mode 0x40 = mkdir, 0x200 create. Save dir pattern `/BASLUS-21135P???????` (profiles), `O` (options).
- Movies: sceMpegIsEnd 0x5B3678 / sceMpegGetPicture 0x5B3620 overridden (blank ~5 s); movie clip completion also
  waits for its SND audio stream (0x501CD8, overridden -> finished).
- SND driver tick: IOP must send user SIF cmd 1 type 0 regularly; EE SND thread 0x512718 waits for it.
- BUG fixed: COP0 Count used a double->u32 cast that overflowed after 14.5 s; the game's input sampler
  (0x43C508, samples when Count advanced > 1,000,000) stopped. Must wrap modulo 2^32.
- Input ring: producer 0x4435E8/0x43CAB0 (guarded by 0x7DCC30), consumer 0x43CCE0.

## Session 7 — a playable match, performance
- Play Now works end to end: main menu -> "create/load a profile?" prompt (answer No) -> Select Team ->
  uniform -> difficulty -> Fenway intro -> lineups -> pitching/batting/fielding. Pitches, hits, fielding,
  batter changes and the score bug all run.
- CRASH fixed: the SND driver tick's header opt is the IOP address where the EE SIF-DMAs its next 0xAF0-byte
  sound command block (EE 0x8EE880, built by 0x5123B0). We used to send vsync*256, so the EE wrote further
  into IOP RAM each tick until it ran past our 2 MB array and trashed host memory (seen as a segfault in
  pad_vsync on entering the match). Now two fixed blocks at 0x1F0000/0x1F1000 alternate (iop_alloc stops
  below 0x1F0000); all IOP-RAM copies are bounds-checked (iop_clamp) with a 64 KB guard area.
- SND command block layout (partly): header {0, tick counter, 0xAF0, 0}, then typed sections
  {3: 0x2A8 bytes, 2, 1: per-channel buffers sized (rate<<9)/48000 samples, 5, 6}. Section 3 looks like
  per-voice words (pairs 0x000D0800/0x0D000800 = idle voices; active ones carry pitch 0x555 = 16 kHz).
  Not yet interpreted: no audio output. MVP_SNDDUMP=dir writes every 60th block.
- VU1: EAGL keeps a microcode cache and re-uploads position-independent programs at varying addresses
  (~130 MPG uploads/s, 24 distinct programs; most are not stored verbatim in the ELF), so an offline VU
  recompiler would need a runtime capture step. Instead the interpreter pre-decodes each instruction pair
  into specialised handlers (template FMAC ops, common lower ops), re-decoding only pairs an MPG changed,
  and skips MAC/status flag work unless loaded code reads them. FMAC flags (MAC/status/CLIP) now reach the
  flag registers 4 cycles late as on hardware (MVP_VU1_NODELAY=1 for the old behaviour).
- GS: primitives are rasterized on worker threads (MVP_GS_THREADS, default cores-1, max 8); each owns
  every n-th band of 8 rows. Drains happen before uploads that touch pages queued work uses, before palette
  decode into a busy slot, before display read-out, and before sampling a texture drawn since the last
  drain. EAGL really does render-to-texture every frame: 256x256 CT32 targets at 0x2D80/0x3080 are read
  back as 512x512 T8 textures, and the 3D target 0x1180 is copied to the display buffer 0 as a texture.
  T8 palettes are decoded once into a cache instead of per pixel.
- Speed in this 4-core ARM VM, everything rendered: menus ~90-140 fps, match ~25 fps (main thread is VU1-
  bound: ~35% VU1, ~30% waiting for rasterizers). A fast desktop CPU should hold 60 in menus; the match
  needs the GPU renderer or VU1 JIT to be safe at 60.
- KNOWN RENDERING BUG: near-camera ground and a wedge over the stands behind the pitcher show black/green
  triangles in match views. It predates this session's changes (same with the old VU interpreter and with
  MVP_GS_THREADS=0). Candidate: the T8H texture at TBP 0x2800 (top byte of the Z-buffer area, alpha test
  GREATER 0x3F) passes where it shouldn't. Debug aids: MVP_BIGTRI=frame and MVP_PROBE=x,y log primitives.

## Session 8 — settings GUI, internal resolution, renderer work
- In-game settings menu (frontend.cpp, SDL renderer + embedded 8x16 font from DejaVu Sans Mono via
  tools/make_font.py). The menu pauses by staying inside the vblank handler until closed. Settings in
  mvp2005.ini (settings.cpp).
- Frame pacing: vblanks are now on an absolute schedule (resync if >4 frames behind); fps_limit sets the
  vblank rate. Frameskip decides per whole game frame (at the VIF1 frame kick), never mid-frame.
- Internal resolution: the main target is found from the final full-screen copy sprite (CT32 texture ->
  display buffer; in MVP it's TBP 0x1180, 512 wide). Every primitive drawn into it is also rasterized into a
  linear S x-scaled colour + Z buffer; the window shows that buffer while the copy happens each frame.
  Native VRAM rendering is kept, so render-to-texture, format reinterpretation and readbacks are unaffected.
- Bilinear filtering implemented per TEX1.MMAG (was always nearest).
- Rasterizer: templated per (texture, blend, depth, high-res) with incremental span interpolation and a
  single address computation per FB/Z access: 13.9 -> 10.2 ns/pixel single-threaded.
- GS record/replay (MVP_GSREC / --gs-replay) makes renderer changes testable frame-exactly; threaded and
  single-threaded output are bit-identical.
- Match-view wedge investigation: the green wedge behind the pitcher is a blended T8 polygon (tex 0x3B80,
  program at VU1 0x0010) whose three vertices all have w < 0 (behind the camera), so it is projected
  mirrored. The microcode (tools/vudis.py on /tmp/vu1snap_0010.bin) culls with CLIP + FCAND/FCOR, which only
  catches it if a CLIP flag is set; either the object's matrix (computed on the EE/VU0 side) differs from
  hardware or the flags do. Open.
- VIF1 FIFO was a std::deque<u32> with per-word push/pop and a payload copy per VIFcode; now a vector with
  a read index and in-place payloads (interrupts from I-bit codes are raised after the loop so a handler's
  FBRST can't re-enter mid-command). Headless match speed 28 -> 40 frames/s in the test VM.
- Experimental graphics thread (MVP_GS_THREAD=1): VIF1/VU1/GIF/primitive setup on a separate thread fed by
  a command ring, events (DMA done, INTC 5, SIGNAL/FINISH, channel registers) applied by the main thread.
  Needs: interrupt events held back while a handler runs (the game counts VIF1/GIF interrupts; latched
  duplicates merge), a full sync before each vblank and before EE reads of VIF1/GIF/GS status (EAGL's
  render watchdog otherwise fires: "DMA CHAIN ... ended at final tag"). With those syncs it was slower
  than inline on the 4-core VM, so it's off by default.

## Session 9 — UI, real-time timing, HD textures, post-processing
- Simulation step: `lui 0x3C88 / ori 0x8889` (1/60 s) and `lui 0x3D08 / ori 0x8889` (1/30 s) are the game's
  fixed step. The recompiler emits ORI_DT for every `ori rt, rs, 0x8889`; at run time it substitutes
  g_sim_dt_bits when the resulting value is one of the two constants (771 sites). Real-time mode sets the
  step from the smoothed frame time (clamped 1/240..1/30, snapped to exactly 1/60 at ~60 fps). Observed:
  the title screen's idle timeout counts frames (runs out sooner at 120 fps); gameplay uses the step.
- Texture cache: decoded RGBA per (TEX0, palette content hash, TEXA) validated by per-page write generations;
  pages written by drawing are never cached (render targets). Uploads only bump generations when their
  bytes differ from the previous upload to the same place and nothing else wrote the pages in between.
  Replays are bit-identical with and without the cache. It's only switched on for HD textures / dumping
  (it isn't faster for plain rendering: EAGL streams many small textures every frame).
- HD textures: content-hash naming WxH_hash.png; replacements (any size) from textures/replace; built-in
  enhancer = separable Catmull-Rom 2x/4x with alpha-weighted colour + unsharp mask, on background threads.
  Only applied where the game samples bilinearly: point-sampled textures include look-up data (a 16x32 CT32
  texture used as per-vertex colour ramp on player skin turned into noise when upscaled).
- Presentation moved to OpenGL 3.3 core (functions loaded through SDL, no GL headers): FXAA at source
  resolution -> resample (nearest / bilinear / bicubic / area for supersampling) -> CAS + colour + CRT ->
  UI (SDF rounded rects, soft shadows, anti-aliased text from DejaVu Sans). Tested with Mesa llvmpipe
  under Xvfb; SDL_Renderer fallback (MVP_NO_GL=1) also tested.

## Session 10 — glitches, timing, sound
- Match glitches (green wedge, black ground triangles) were a VU1 bug: WAITQ/WAITP in the lower slot must
  stall the pair before the upper instruction reads Q. The near-plane clipper read a stale Q. Fixed in vu1.cpp.
- Timing: adaptive real-time mode steps the vblank rate between 60..240 Hz so game speed stays 100%
  (measured as sim ticks x step / wall time). Gameplay is correct at any reachable rate; some menu timers
  count frames.
- Sound architecture (from SNDDRV.IRX on the disc, EA SND library v9, disassembled with tools/eedis.py):
  - The driver ticks every 512 output samples at 48 kHz (93.75 Hz) and asks the EE for a 0xAF0-byte block.
  - Walker at 0x7990, jump table at rodata 0xD410:
    - type 0 and type 5: skip 12 bytes
    - type 1: EE-mixed PCM, planar s16, n = rate*512/48000 per channel (0x7510)
    - type 2: handled at 0x7220
    - type 3: 48-voice SPU2 mirror (0x86F0)
    - type 4: handled at 0x78DC
    - type 6: end
  - The type 3 mirror:
    - +0 VMIXEL/VMIXER
    - +8 VMIXL/VMIXR (48-bit masks)
    - +32 per voice: {u32 SSA, u16 pitch, s8 volL, s8 volR}
    - A new SSA keys the voice on, with LSA = 0x5000 (a silent loop), ADSR 0x000F/0x0005, and volume v*129.
    - If ENVX is still non-zero, the driver keys the voice off first and keys it on at the next tick.
    - The driver reads NAX; below 0x5040 means the voice has finished.
  - Music (DATA/AUDIO/MUSIC/*.ASF: 32 kHz stereo EA ADPCM) and crowd/commentary streams are decoded
    and mixed on the EE and arrive as the PCM section. Short effects from the ABK banks play on the
    SPU voices from the SPU RAM uploads.
- snd_mix.cpp now:
  - paces ticks by the SDL audio queue (about 43 ms target);
  - waits for each block with a marker word;
  - mixes the PCM section and a 48-voice PS-ADPCM voice engine (cubic interpolation, the driver's fixed
    ADSR);
  - outputs 48 kHz stereo.
- Checks and debug switches:
  - PCM continuity at block edges was verified (edge step = inner step).
  - The output spectrum shows music structure.
  - MVP_WAVOUT=file.wav records the output. MVP_SPUDUMP=file dumps SPU RAM at exit.
- Not done yet: section types 2 and 4 (not seen in menus or the match so far), reverb (VMIXE masks are 0 in
  the captured blocks), and movie playback (FRONTEND/MOVIES/*.MPC).

## Session 11 — GPU renderer (2026-09-29)
- `runtime/src/gs_gpu.cpp`: OpenGL 3.3 core (macOS 4.1 compatible) renderer for the main target (the texture of
  the final full-screen copy; 0x1180, 512 wide). Primitives that draw into it are batched by state and drawn
  at the internal resolution; one fragment shader reproduces the software rasterizer's per-pixel maths:
  texture wrap modes incl. region clamp/repeat, integer bilinear (texelFetch x4), TFX, fog, alpha test with
  the partial-write fail modes (drawn as a pass + fail pass with colour/depth masks).
  - Blending ((A-B)*C>>7)+D uses dual-source blending (o0 = Cs*|kS|, o1 = |kD|, equation from the signs).
    C = destination alpha and colclamp=0 fall back to an exact shader blend against a copy of the target,
    one primitive at a time (never seen in MVP so far).
  - Depth: D32F_S8 target, gl_FragDepth = z / 2^32 (exact for Z24 values; Z32 loses the same bits the
    software path's float vertices do).
  - Destination alpha test (DATE) through the stencil: stencil = (dst alpha bit 7 == DATM) from a copy of the
    target, then the batch draws with stencil EQUAL 1 and clears the stencil where it writes an alpha whose
    bit 7 fails DATM. MVP's shadows rely on this "draw each pixel once" behaviour (test=0x5c10c, alpha 0x46).
- Ownership (gs.cpp): the software rasterizer no longer draws the main target. Pages the GPU drew are "owned";
  any VRAM read of them (texture, palette, transfer source, display read-out) first downloads the GPU picture
  at native resolution, software writes to them make the GPU copy stale (re-uploaded before the next GPU
  primitive). Page tests are exact (px_page/px_range) because EAGL keeps data in awkward places:
  - The Z buffer (0x1F80, Z24) holds T8H textures in its top bytes (512x256 at 0x1F80 and 256x256 at 0x1F84).
    Top-byte formats (T8H/T4HL/T4HH) only check colour-owned pages, so this never triggers a download.
  - The display buffer (FBP 0, CT24) holds T8H textures in its top bytes too (tbp 0x00004 / 0x00200).
  - The final copy into the display buffer is kept as a lazy copy (only executed if someone reads those
    pages); the window shows the GPU target directly.
  - Palettes at the last VRAM block (0x3FFC) made page ranges wrap to page 0: ranges are now exact.
- Result: in the attract mode and in a match, zero read-backs and zero re-uploads per frame. Replays: 99.7-
  99.98% of pixels identical to the software renderer at 1x, same at 3x.
- Presenting: the frontend blits the target's crop into the presenter texture (no CPU read-back). Frame dumps
  and screenshots read the GPU target back.

## Session 12 — FMV: IPU emulation
- MVP uses Sony's libmpeg (0x5B3xxx-0x5BAxxx) on the EE; the IPU does bitstream work: `runtime/src/ipu.cpp`
  implements BCLR, VDEC (MB address increment / MB type by IPU_CTRL.PCT / motion code / dmvector, with the
  code length in DATA[31:16] as the IPU reports it), FDEC, SETIQ, SETVQ, BDEC (DC/AC VLC incl. B-15, inverse
  quantisation with MPEG-2 mismatch control, float IDCT, frame/field DCT, CBP -> IPU_CTRL, start-code check
  -> SCD/ECD), CSC (IPU coefficients, SETTH alpha thresholds, RGB16 dither), PACK.
- Input: toIPU DMA (ch 4, normal + source chain) feeds a quadword queue; commands restart from their
  beginning if the bitstream runs dry (they only commit on success) and are retried when data arrives.
  Output goes to fromIPU DMA (ch 3). MADR bit 31 = scratchpad.
- libmpeg's motion compensation fetches reference blocks with **toSPR chain DMA** (CHCR 0x105): scratchpad
  DMA now supports source chain, destination chain (fromSPR) and interleave modes. Without it P/B pictures
  came out green.
- MPC files: chunks `MPCh` (MPEG-2 video ES, 576x448 = 36x28 macroblocks) and EA audio `SCHl/SCCl/SCDl/SCEl`
  (44.1 kHz stereo); the audio is decoded and mixed by the game on the EE like the music, so the movie audio
  plays through the existing PCM path. The override that reported "movie audio finished" (0x501CD8) is gone:
  the game's own check works, the intro plays in full and START skips it.
- Speed: ~10 s of host time per minute of video in the test VM (BDEC ~8 us).

## Sound follow-up
- Reverb: section 3 +16..+18 of the SND block are the reverb mode, delay and feedback the driver passes to
  sceSdSetEffectAttr (SNDDRV 0x8480). The mode is 0 (off) in the intro, menus, attract mode and a match, and
  the VMIXE masks are 0, so the game doesn't use reverb. A log line appears if it ever does.
- Section types 2 (a second PCM buffer, copied only when a driver flag is 1) and 4 (2 KB, pointer stored at
  0xC80) never appear in those parts of the game either. Also logged if seen.

## Session 13 — performance (main thread)
Profiled a CPU-vs-CPU match in the 2-core VM (headless, no drawing, `MVP_PROF`).
- GS image transfers (`xfer_data`) were 18.6% of the main thread: now whole GIF IMAGE runs per call and
  per-row address math for CT32/T8/T4 -> ~10%; frame rate in the heavy match scenes went from ~19 to ~24-28.
- VU1 was ~50%. Precompiled blocks: 274 blocks (5138 pairs) cover all VU1 time in a match (0 uncompiled
  entries); every pair is a `step()` call on a constexpr decoded pair, so handlers inline with register
  numbers folded in. The first dump had 3579 overlapping blocks (17.7 MB of C++) because the dumper recorded
  every entry pc; dumping while the interpreter runs whole blocks gives the same set the dispatcher will
  look up. Specialised handlers were added for the ops the hot vertex loops use (CLIP, FTOI/ITOF, ABS,
  FCAND/FCOR/FCEQ/FCSET/FCGET, ILW/ISW, MTIR/MFIR, IAND/IOR, MR32, IBxxZ). Self-check: 0 mismatches.
  Without blocks the same scenes run at ~13-16 fps; with them ~22-40.
- The blocks are split into 8 translation units sharing `vu1_core.h`, so they build in parallel
  (~3.5 min with 2 cores here, instead of one 6-minute file).
- Remaining main-thread profile: VU1 ~39%, EE code ~11%, xfer_data ~10%, vif1_run ~6%, vertex_kick ~5%,
  VU0 macro ~5%, libc/libm ~6%.

## Session 14 — modern rosters, more speed
**Database format.** `DATA/DATABASE/DATABASE.BIG` is a BIGF archive (big-endian header, file size little-endian)
of 18 tables. Each table is EA RefPack-compressed (0x10FB) text: a header `0 first_name,1 last_name,...,;`
and CRLF lines `<9-hex key>,0 value,1 value,...,;`. The same format as the PC version's database folder.
- Keys: teams use djb2(short name) (e.g. djb2("KC") = 0x597433); player keys are arbitrary unique 32-bit
  values, referenced by roster.dat, the stat tables and org.dat (MLB/AAA/AA/A team keys as decimal, manager
  keys as signed decimal).
- attrib: positions 0 SP, 1 C, 2 1B ... 8 RF, 9 DH, 10 RP (secondary 11 OF, 12 IF, 13 utility);
  bats 0 R 1 L 2 S; height = inches - 48; weight = lb - 100; birthday = days since 1947-12-31;
  face < 900 = a real head in GHEAD.BIG, 901-915 generic; photo 1 = portrait, 2 = none; salary in $100k.
- lh/rhattrib = batting vs left/right-handed pitchers: contact, power (0-100), 3x3 hot zones (0-2),
  spray and batted-ball percentages, chase/take/miss tendencies (0-15).
- pitcher: stamina, pickoff, fastball control/velocity (mph), 4 more pitches (type, movement 0-15,
  description, control, velocity). Types: 1 changeup, 2 curve, 3 knuckleball, 4 screwball, 5 sinker,
  6 slider, 7 splitter, 8 two-seamer, 9 cutter, 10 circle change, 11 forkball, 12 knuckle curve,
  13 12-6 curve, 14 slurve.
- roster.dat: per team and player the position + batting order in four lineups (vs RHP/LHP x AL/NL):
  C 1B ... DH, B (bench), SP1-SP5, LR, MR, SU, CP; the NL lineups bat SP1 ninth.
- Team names shown in the menus come from `DATA/FEENG.LOC` / `IGENG.LOC` (UTF-16 string tables: LOCH
  header, LOCI id->index table, LOCL offsets; strings end in 4 zero bytes, section padded to 4 bytes).
- The game parses the ISO directory itself (it reads sectors 16, 261..297) instead of asking sceCdSearchFile,
  so replacement files need their directory records patched; iso.cpp does that on the fly and serves the
  new data from sectors past the end of the image.

**Rosters.** tools/fetch_mlb_data.py pulls the 40-man and active rosters of all 30 clubs and their
AAA/AA/High-A affiliates, season/split/career stats and Statcast leaderboards (sprint speed, OAA, arm
strength, pitch arsenals, batted-ball profile, chase/whiff). tools/build_rosters.py picks 25/25/22/22
players per organisation level, translates minor-league lines to MLB level, regresses small samples,
and quantile-maps every derived score onto the 2005 database's distribution of that rating (defence per
position, stamina per role), so the rating spread matches what the game was tuned for. The 100 Legends /
Heroes / EA-team players and "Default" are kept.

**Speed** (same headless match benchmark, 2-core VM; frame reached after ~195 s: 6300 before -> 9000 after):
- VU1: delayed MAC/CLIP flag results are applied lazily, only before an instruction that reads or sets
  flags (and at the end of the program), instead of checking the queue every pair. The queue is FIFO so
  the flag values seen are identical.
- GS uploads: CT32 and T8 image data is written a whole block row group (8 / 16 pixels) at a time.
- VU0 macro instructions (COP2 in the EE code) are specialised per call site at compile time
  (`vu0_inline.h`): the recompiled code calls `vu0i::op<word>`, which folds the decode away.
- Tried and reverted: polling interrupts/timers less often made the game spin longer in its own wait
  loops (slower overall).

## Session 15 — HD texture pack, player looks, more speed
- The GPU renderer now uses HD textures too (packs and the built-in upscaler): the decoded texture's pixel hash
  selects a replacement, which is uploaded as a larger GL texture; the shader samples it on a grid scaled by
  u_hs (UV coordinates scaled, STQ coordinates use the HD size directly, region clamps scaled). Render
  targets and REGION_REPEAT addressing keep the native texture.
- Pack building: 30 scripted Play Now sessions (one per home park) with texture dumping and a texture-use log
  (MVP_TEXUSE: frames used, first sprite rectangle). ~31,800 distinct dumps, but ~85% are FMV frames (menu
  backgrounds are video, drawn as 16x512 strips; each frame unique) and ~1,500 are VRAM data decoded as
  textures (palettes drawn as 16x16/8x16 CT32 sprites, invisible data with alpha <= 6, random-looking blocks).
  Upscaling those breaks effects (a 16x16 palette texture upscaled turned the Red Sox logo into noise), so
  make_pack.py skips them: 1,187 real textures remain (fonts, UI, logos, scoreboards, stadium, crowd, field).
- Real-ESRGAN runs in NumPy (tools/hd/esrgan.py reads the .pth files without PyTorch). The anime model is
  ~6x slower than realesr-general-x4v3, so it is used for flat artwork up to 128x128; bigger textures use the
  general model. 4x pack: ~700 MB.
- Player looks: official headshots (img.mlbstatic.com) -> face lightness (cheeks/nose, exposure-corrected
  against the grey studio background) ranked onto the 2005 skin-tone codes, and a logistic model (96
  hand-labelled headshots, leave-one-out: 82% facial hair yes/no, 92% full beard) for facial hair.
  Learning EA's codes directly from 2005 players' headshots did not work (476 matches, photos taken years
  apart from the 2005 ratings): no better than the majority class.
- VIF1 UNPACK: specialised loops per format for unmasked MODE 0 (checked against the general path on 37M
  unpacks: identical).

## Session 16: launcher, anisotropic filtering, commentary names
- Launcher: the window opens before the ELF is loaded; the settings menu runs with a "Game files" page.
  Import checks ISO9660 + SYSTEM.CNF (SLUS_211.35) + DATABASE.BIG and copies SLUS_211.35 to game/.
  File dialog: osascript (macOS), PowerShell OpenFileDialog (Windows), zenity/kdialog (Linux); SDL drop-file too.
- Anisotropic filtering: triangles with bilinear textures and REPEAT/CLAMP wrap are sampled by the GPU sampler
  (mipmaps + GL_TEXTURE_MAX_ANISOTROPY) instead of the manual PS2 bilinear; sprites and region-clamp keep the
  exact path. GS replay A/B (MVP_REPLAY_ANISO): no artifacts.
- Commentary: DATA/DATABASE/ROOKIE.BIG audioid.csv is EA's surname -> audioid list for generated rookies
  (also names.csv with a "use fullname flag"). PBP PNAMEHDR/PNAMEDAT hold surnames (ids 3-5032, a-d
  inflections), PA PNAMEHDR/PNAMEDAT full names (ids 3-3228). 954 of 2820 new players get an id (10 full name).
  PA clips are EA MicroTalk (SCHl codec2 4): zeroing data isn't safe, so the PA index entry is renamed
  NNNN.hdr -> NNNN.hdx for surname-only ids; in a match the game then skips the PA name (checked with
  cdvd-read logs: Walker PBP clips 0922/0923 read, PA 0922 not read, no stall).
- MVP_LOG_ALL=key1,key2 lifts the per-key log limit.

## Session 17: scoreboard menus with disc art, type-14 textures, CPU tuning, looks, speed
**EA shape files (.ssh, "SHPS").** Header: count at +8, then 8-byte entries (4-char name, offset). A shape:
type byte, 24-bit size of this section, width, height; pixels follow at +16. Type 2 = 8-bit + palette (the next
section, type 0x21, 256 x RGBA, CSM1 order: swap index bits 3 and 4), type 5 = RGBA, PS2 alpha (0x80 = opaque).
- **Type 14 = 3 bits per pixel, unpacked by the GS.** Two layers: A is W/8 x H/4 32-bit texels (4 base palette
  indices per 8x4 block), B is W/4 x H/4 32-bit texels that the GS reads as an 8-bit W/2 x H/2 texture (2-bit
  offsets). The game uploads both, draws A as a DECAL sprite into a CT32 target of W/2 x H/2, adds CLUT[B]
  (ALPHA Cd+Cs, FBMSK fc000000) and then uses the target's memory as a PSMT8 W x H texture. So every byte of
  the target is a palette index = base + offset. Found by tracing reads of the file data (MVP_FINDMEM,
  MVP_RWATCH), then logging uploads, TEX0 and draw state (MVP_XFERLOG=all, MVP_TEXLOG, MVP_GSLOG=fff).
  runtime/src/game_art.cpp decodes it offline by simulating the CT32 <-> T8 memory layouts.
- The game does this *every frame* for about 20 textures in a match: texture slots in VRAM are shared, a slot
  holds [target][A][B], and several textures go through the same slot within one frame (90% of the unpacks
  change the target's pixels). All of it went through the general sprite rasterizer.
- Art on the disc: FEONLY.BIG/mainart.ssh (badge `tp03`), LOGOS.BIG/lNNN.ssh (NNN = team in team.dat order;
  l = wordmark 128x64, g/i = 64x64 logos, d/e = colour bars), STADIUMS.BIG/<code>[n].ssh 1024x256 panoramas,
  BKGNDS.BIG menu photos, GHEAD.BIG generic head textures (below).
- Menu music: DATA/AUDIO/MUSIC/MENU1-9.ASF, EA SCHl/SCCl/SCDl/SCEl blocks, EA-XA ADPCM (15-byte frames per
  channel, 28 samples), 32 kHz stereo.

**Tuning file.** DATA/DATAFILE/DATAFILE.BIG/datafile.txt (4.4 MB RefPack text, read at boot). A schema line
`0xHASH N K;idx Name;...` names the fields of the next K object lines `0xHASH N<object.name>;idx value;...`.
21,425 objects, mostly animation tables and cameras; gameplay: `cpubatter` (swing decisions by zone, count and
discipline), `pitcher` (delays, fatigue), `stealtune`, `hitnrun`, `runner`, infield/outfield alignments
(`if_*`, `of_*`: X toward third, Z toward first, feet from home; the mound is 42.8, 42.8), difficulty levels
`rookie/pro/allstar/mvp`. tools/datafile.py reads and writes it; tools/build_ai.py makes mods/ai-2026.
The manager AI itself (bullpen, bench, substitutions, offensive strategy: 0x2a5538-0x2f4450) is code.

**Generic heads.** GHEAD.BIG holds 8,400 type-14 face textures g<h><c><s><f>.ssh (128x256): h = head 901-915
(a-o), c = hair colour (7), s = hair style (10), f = facial hair (8); the name is built at 0x424118 from the
player record (face - 804, haircolour, hairstyle, facialhair + 'a'). Each head has its own skin colour and the
2005 database pairs it with exactly one body skin_tone (901:0 902-903:1 904-905:2 906:3 907:4 908-909:5 910:6
911:7 912:8 913-914:9 915:10). EA used 901-905 for white, 906 for East Asian, 907-910 for Latino and 911-915
for Black players. Hair colour 0 black 1 blond 2 light brown 3 red 4 brown 5 dark brown 6 grey; style 0 shaved,
1-2 long, 3-6 short, 7-8 cropped, 9 braids; facial hair 0 none, 1 full beard, 2-4 moustaches, 5 goatee,
6 chin beard, 7 soul patch. The roster builder had assumed 6 = full beard (it is a chin beard) and picked head
and body tone separately; both fixed. The card portrait is the game's live render of the head.

**Speed.**
- EAGL's frame sync is a spin loop (0x54a0f4 in f_0054a030, 4% of the profile at full speed). Backward branches
  listed in tools/recomp/waitloops.txt call ee_wait_poll, which delivers pending events or sleeps until shortly
  before the next vblank/alarm (or a graphics event), then spins the last 0.4 ms. Sleeping right up to the
  vblank lost 8% of the frames on a machine that only just keeps up: a late wake-up makes a frame miss its
  vblank. Headless match, 200 s, frame 7800 reached either way: 192 s -> 149 s of CPU.
- Texture unpacking fast path (gs.cpp, dec_*): the DECAL sprite is held until the additive one arrives, then
  both are done in one pass on VRAM. Checked against the general rasterizer on 120,000 unpacks: identical.
  The target's pages then count as uploaded, not drawn, so the texture goes through the texture cache (decode
  once per unpack) instead of per-pixel VRAM reads. Software drawing of a match, frames 3600-4200: 155 s -> 128 s.
- The dynamic texture area (0x3280-0x3f00 in a match) is a ring that the game refills every frame: about 1,000
  uploads and 20 unpacks per frame, the same slot reused several times within a frame. Nothing there survives
  a frame, so the old frame skip (drop every draw) was not the cause of the garbage below. Frame skip now only
  drops draws into the picture (display buffers and the main target); draws into texture memory still run
  (safer for anything rendered once and kept; a headless run that skips every frame is about 10% slower).
  An experiment that re-runs a skipped frame when it copies the picture into texture memory (MVP_UNSKIP=1:
  VRAM + register snapshot and the frame's GS input are kept) works but turned out to be unnecessary.
- VIF1 UNPACK: general path (masks, MODE 1/2, wl > cl) rewritten per format; MVP_UNPACKCHECK on 44M unpacks: 0 bad.
- MVP_GS_THREAD=1 (graphics on its own thread): no gain in headless runs (nothing is drawn there), +47 s of
  system time in 200 s from waiting; still off by default.

**Depth of field (the "noise in close-ups" bug).** Batter walk-up shots (cutscene scripts batterintro_NN, camera
objects with a `.DOF-1` cDepthOfFieldParameters object in datafile.txt; `ps2lighting.DOF_Enabled=1`) blur the
background. What reaches the GS in such a frame, in order: the scene with FBMSK ff000000 (alpha protected);
48 untextured sprites with FBMSK 00ffffff, ZTST GREATER at several depths (they write a blur amount per pixel
into the main target's alpha: about 0x10-0x25 on the background); 16 column sprites (plus a few) over the whole
screen with a 32-bit 256x256 texture at 0x3280, the start of the dynamic texture ring, blended
(Cs - Cd) * Ad + Cd (ALPHA 0x54), texture coordinates covering 256x224 from texel 1.5; then the player and the
HUD, whose textures are uploaded to 0x3280 again. Nothing fills that texture in the frame. The only draws into
it (FBP 0x3280, FBW 4) are a grab of the main target at the end of some frames (two textured triangles,
upside down relative to the layer), on a 90-frame grid, for something else. So the layer showed the previous
ring contents: noise. On a PS2 the copy must exist; why it is missing here is not known (the DMA tag walker and
the GIF FIFO were checked; the grab that does arrive comes through VIF1 like everything else).
Fix (gs.cpp, dof_grab): when the first layer sprite of a frame arrives, draw the main target at half size,
bilinear, into the layer's texture, positioned from the layer's own texture coordinates, then let the layer
draw. Checked on a recorded frame (MVP_GSREC_DOF, replay): noise gone, background soft, aligned, the batter
sharp. The cut grabs were first mistaken for the backdrop (and frame skipping for the cause).
- Tools added while looking: MVP_GSTRACE, MVP_GSLOG_TBP/TBW, MVP_PICSRC, MVP_VWATCH, MVP_GSREC_DOF,
  MVP_REPLAY_PRE, MVP_REPLAY_BUF, MVP_FRAMES_FROM, MVP_TEXAT_FROM/MIN.
- Open: the batter card draws a black square left of the name (an 87x76 quad with an 8x8 solid texture,
  tbp 3de0); nothing is drawn on top of it. Not yet known whether the 2005 game puts a logo or photo there
  (the card was never captured with the original rosters).

## Session 18: team logos with modern rosters, custom music, stereo, uniforms, ballparks
**The black square on the batter card** was the team logo. The game numbers the teams in the order they first
appear in roster.dat (table of keys at 0x7d7360, numbers at 0x7d7550) and loads its in-game logos by that number
(func_001d86f0: frontend/logos.big, `l%03d.ssh` for both teams). The 2005 file lists all 30 MLB teams first; the
roster builder wrote each organisation's four teams together, so St. Louis became team 97 and the game loaded
l096.ssh. The image widget then looked up "j024" (func_0040edc8), missed, and got the texture manager's default
8x8 black texture. Fix: build_rosters.py keeps the 2005 team order. Found by comparing a run with the original
rosters (probe on the quad: 128x64 texture vs 8x8), then logging texture lookups by name (MVP_LOGCALL with
strings) and the shape-file loads. The "white boxes" in the pitch display are the game's own label design
(identical with the original rosters).
- Uniform previews / portraits: players without a photo get a portrait texture named after their generic head
  code (e.g. "jadb"); real 2005 players use the photo id ("1193").

**Music.** DATA/AUDIO/MUSIC/MENU1-9.ASF (table at 0x84ebb0, with AUDIO/FELOOP/LOADn.BNK): SCHl (44 bytes, "PT"
tags: 0x85 sample count, 0x82 channels, 0x84 rate 32000, 0xa0 = 10, 0x8c = 4), SCCl (block count), SCDl blocks
{samples, channel offsets, per channel: two 16-bit history samples then 15-byte EA-XA frames (1 header: filter
<< 4 | shift, 28 nibbles)}, SCEl. Blocks end on frame boundaries just past k/15 s (76 or 77 frames).
Decoder: s = ((nibble << (12 - shift)) * 256 + c1*h1 + c2*h2 + 128) >> 8 with (c1, c2) = (0,0) (240,0)
(460,-208) (392,-220). music_import.cpp writes the same layout; a decoded disc song re-encoded: same file size,
49 dB SNR. The game plays a replacement of any length (5 s test tones cycle fine). Song names: the 27 strings
after "EA SPORTS(tm) TRAX" in FEENG.LOC (artist, title, album x 9; three more triplets follow that the PS2
version has no files for).

**Stereo was swapped.** EE-mixed PCM arrives in section 1 of the sound block as two planar buffers. SNDDRV.IRX
(0x61e8, with small tables at 0xd3f8: channel 1,0 and 0xd408: source offsets 512,0,768,256) copies the second
buffer into the first half of each SPU2 AutoDMA block (left) and the first into the second half (right). So the
first buffer is the right channel. Checked with a test song (1 kHz left, 3 kHz right) through the game.

**Overlays.** An overlay is now a list of segments (memory, host file read on demand, bytes of the image). A
folder named like a BIG archive replaces members: the directory at the start of the archive is rewritten
(offset, size; total size at +4 little-endian) and the new members are appended after the original data; the
game reads the header and then only the members it wants (MVP_OVLLOG). Members stay RefPack-compressed.

**Shape files** (tools/ssh.py): chunks are relocatable (section sizes are relative), so shapes can be spliced
between files; each chunk ends with a type-0x70 section holding the shape's name again, and the game looks
textures up by *that* name (a spliced jersey preview stayed black until it was renamed there too). Type-14
encoder: every 8 pixels share a base index and pick base + 0..3, bases are chosen by exhaustive search over the
palette; re-encoding an original gives identical colours.

**Uniforms.** MODELS.BIG u<team 000-125><slot a-p>.ssh (32 shapes: jersey fronts jerf/jers/jert and long-sleeve
jrfl/jrsl/jrtl, back jerk, sleeves, legs lega-d, hat, helmets hlm1/hhlm, catcher's gear, llod = 128x128 atlas
for distant players: front 64x64, back, legs 32x64, hat 32x32, ...), f<team><slot>.ssh name/number lettering,
UNIFORMS.BIG <team>.ssh with one 128x128 thumbnail per slot (shape "<team><slot>"). Slots: a home, b road,
c/d alternates, g-n throwbacks, o batting practice (letter table at 0x857988, names built at 0x198690-0x1987d8).
Texture colours are much less saturated than the real team colours (cloth look), so recolouring keeps each
texture's own saturation level and only moves hue, relative saturation and relative brightness.

**Stadiums.** DATA/STADIUM/<code>DAY|NITE|DOME.BIG: models as split ELF objects (x.ord = ELF header + .data,
x.orl = string/symbol/relocation tables and section headers; offsets as if concatenated). Geometry is VIF
batches: UNPACK V4-32 header, V3-32 positions (masked, w from STROW), V2-32 UVs, V4-8 colours, MSCAL. Positions
are world coordinates in inches, home plate at the origin, foul lines along +X (left) and +Z (right). Bounding
volumes sit in the model descriptor as min(x,y,z,1) max(x,y,z,1) centre radius, plus a 6-float box at __BBOX.
Text files: wall.dat (wall triangles; base edges = the fence line), coll.dat, shadow.dat, crowd.dat (sections:
count + box, then 3x4 matrix + position per figure), stadium.ifo (camera men, lights), flags.csv, render.ifo.
tools/build_stadiums.py moves every point from 62% of the fence distance outwards by the displacement of the
fence at its angle; the fence corners move radially and the fence between two corners stays straight, so the
drawn wall and the collision wall move alike. A test with +/-30 ft at Fenway plays normally (ball off the wall,
fielders at the track). DATAFILE.BIG <park>day.txt holds each park's intro cameras and lighting (not changed).

## Session 19: every reshaped park played, a freeze on busy machines, the commentators' team and park names

**All 14 reshaped parks load and play** (Atlanta, Washington, Mets, San Francisco, San Diego, Miami, Kansas City,
Athletics, Texas, Detroit, Minnesota, St. Louis, Seattle, plus Houston from session 18): fence, warning track,
stands and foul poles in one piece, balls off the wall and over it behave. Team-select order from Boston, one
step per press: Cubs, White Sox, Reds, Cleveland, Colorado, Detroit, Miami, Houston, Kansas City, Dodgers,
Milwaukee, Minnesota, Mets, Yankees, Athletics, Phillies, Pirates, Padres, Giants, Mariners, Cardinals, Rays,
Rangers, Blue Jays, Nationals, AL, NL, Angels, D-backs, Braves, Orioles. Scripted input: `2840:DOWN/6` presses
for 6 frames. **The scripted tests are sensitive to load:** loading takes real time, input is counted in frames,
so with three processes on two cores the same script lands in a different menu. Two runs at a time at most.

**A freeze when the computer is busy (fixed).** One test stopped for good in a menu: every guest thread waiting,
the main thread in SleepThread at 0x5ae2e4. That is the game's delay function (0x5ae270): SetAlarm(ticks,
0x5ae250, own thread id), then SleepThread; the handler calls libkernel's iWakeupThread wrapper (0x5da9c8). The
wrapper asks GetThreadId first: if the thread to wake is "the running thread", it does not wake it but queues the
request for libkernel's helper thread - which this game never starts (InitThread at 0x5da8f0 is not called;
[0x7fcad8] stays 0), so the wrapper returns -1 and the wake-up is lost. On a PS2 the alarm cannot go off between
the two calls, and while everybody sleeps the idle thread is the running one. Here both could happen: (1) if the
host stalls between SetAlarm and SleepThread the alarm was due at the next polling point, with the main thread
still running; (2) with every thread waiting, GetThreadId in a handler still returned the thread that went to
sleep last. Fixes in ee_kernel.cpp: an alarm is not delivered to the thread that set it until that thread has
blocked or passed 8 polling points; GetThreadId returns 0 (the idle thread) inside a handler while nothing runs;
the alarm list is walked by index and reuses free slots (a handler that set a new alarm could invalidate the
loop, and the list grew for ever). `MVP_TEST_ALARM_STALL=1` forces the stall: with `MVP_TEST_NO_ALARM_GUARD=1`
(old behaviour) the game freezes before the first menu, without it it plays. `kill -USR1 <pid>` now prints every
guest thread (state, what it waits for, return address; call stack in trace builds) and the SIF reply queue.
A fatal error now leaves with _Exit: exit() could wait for ever in static destructors (graphics/audio threads),
so a crash looked like a freeze.

**Open:** one St. Louis-at-St. Louis test (three processes on two cores, before the fix above) died with a
virtual call through a null pointer: func_00179610 (a generic "call method n of the object at +4" helper, only
caller 0x179928) with object pointer 0x118 - a member of a null object. Two more runs of the same match (one
with the trace build) did not repeat it. Related, and also only when the machine cannot keep up (frames at a
third of the vblank rate): the name lookup at 0x55db88 (open-addressing hash table, strcmp at 0x5cd09c) meets
slots that hold wild pointers (0x2e96d328, 0x0338e2f6, different every run) - with and without the mods. The
runtime answers reads outside RAM with 0, so the lookup just moves on; the first twelve such reads are now
logged with registers (and the call stack in trace builds). The read of 0x04e47758 once per match is separate
and happens in every run.

**The commentary's speech format** (tools/utk.py). SPCH_PBP and SPCH_PA keep names in pairs of BIG archives:
xHDR.BIG (index per team / park / player) and xDAT.BIG (the recordings). An index is: 2 bytes, id, 0, entry
layout (0: offset; 1: offset + 1-byte code; 2: offset + 2-byte code), count, 0, 0, total size (u32 LE, in
256-byte units), then the entries (offsets u16 BE in 256-byte units, codes BE). Each recording is its own EA
stream (SCHl/SCCl/SCDl/SCEl, 24 kHz mono, codec 4 = MicroTalk 10:1, "UTK" revision 3): blocks of 4 frames of 432
samples; see the comment in tools/utk.py for the bit layout. The decoder's tables sit in the executable at
0x7e5070 (64 reflection coefficients), 0x7e5170 (2 x 256 Huffman lookups) and 0x7e5370 (29 commands); the game's
frame decoder is func_0051b6a0. My Python decoder ends every block within 3 bytes of the block's end and its
output matches a recording of the game itself (normalised correlation 0.82-0.90 for the clips that were played,
below 0.2 for all others) - which is also how the changes below were checked: `MVP_WAVOUT=x.wav`, then
`python3 tools/utk.py game/SLUS_211.35 --heard x.wav <iso>` lists the team and park recordings that were said
and when. `MVP_OVLLOG=1` shows the reads (a replaced member is read from its new place).

**What the 16 team recordings are** (TNAMEHDR/TNAMEDAT, members numbered like team_artid, 124 of 126 teams):
lining recordings up against each other with dynamic time warping on MFCCs (same speaker, so the same word
matches at ~35-45, different words at ~80-100) gives: 0000 the place; 0001 and 0101 place + nickname; 0003,
0103, 0203, 020f the nickname; 0107, 0108 a short lead-in + nickname; 010d another nickname (it matches neither,
and the Dodgers have none); 0202, 0204, 0205, 0209 a 0.7-1.0 s lead-in that is the same words for every team +
nickname; 020a, 020b nickname + 0.65 s of words that are the same for every team. The pre-game introduction says
the visitors with 0101 and the home team right after it. The Nationals are already in this game (team 28,
recorded late: three takes of 010d, no 0202-0209); teams 125/126 (the Burnaby fantasy teams) have no recordings,
and the stadium announcer (SPCH_PA, 5 long recordings per team, 91 teams) has none for the Nationals either - so
"no index for this team" is a state the game already handles.

**Renamed teams** (tools/build_team_speech.py, part of mods/commentary-2026): a new .hdr/.dat pair per team in
which a code keeps its place but plays another recording of the same team (each entry gets its own copy, offsets
ascending as in the original). Nickname changed (Cleveland, Tampa Bay): everything but 0000 plays 0000. Place
changed (Angels, Athletics, Marlins): 0000 and 0001 play 0003, 0101 plays 0103. Checked in game: the Cleveland
introduction now says "St. Louis Cardinals" (0101) and then "Cleveland" where it said "Cleveland Indians"; Miami
gets "Marlins". The stadium announcer's index (SPCH_PA/TNAMEHDR.BIG) has the five teams renamed NN.hdr ->
NN.hdx, as for the player names.

**Park names** (STDNMHDR/STDNMDAT): members <id>a (3-4 takes of the name, layout 0) and <id>b (6 longer takes,
code 6). Ids follow team_artid for the 30 home parks (28 is Olympic Stadium), RFK Stadium is 44a. The
introduction plays one of them ("06b" take 5 for Cleveland, "44a" take 5 for Washington). With the members hidden
(.hdx) the game goes straight on to the team names. tools/build_stadiums.py hides the 15 renamed parks (in
mods/stadiums-2026, so the switch for ballparks covers it).

**Cleveland's park.** JACODAY/JACONITE.BIG cram.ssh (135 textures): the 2005 logo is in inda (64x64, 8-bit),
indd (128x64) and ads2 (256x64, the wall panel next to the Jacobs Field emblem), and four times in the atlas add4
(sign, two banners, a small one). tools/build_uniforms.py paints the block C there (type-14 textures re-encoded
with their own palette, so everything that was not painted keeps its pixels) and puts the two cram.ssh in
mods/uniforms-2026. vram, field and crowd textures have none.

## Session 19: online lobby, dynasty year
- `.apt_codeoverlay` (0x75e630-0x7d32c0) is flagged writable+executable and keeps its read-only data between its
  functions: ~40 switch tables and vtables (0x7a8610.., 0x7ce150.., 0x7d1fe0..) and `{offset, function}` member
  pointer pairs (0x7b0454..). None are in a data section, so discovery now also scans code sections: words that
  point into their own section and cannot be an instruction (SPECIAL with a non-zero shift field or an unused
  function code), their neighbours, and runs of >= 4 pointers within 64 KB.
- MVP Online: `EASO_ISP_Initialize` (0x4f1560) -> 0x5bba28 starts the multi-thread SIF RPC layer (MSIFRPC.IRX):
  sends SET_SREG(1, 1), binds 0x80000401.., then spins on `sceSifGetSreg(1)` (0x5dbcb8) until the IOP sets it.
  The HLE answers SET_SREG reg 1 with the same command. Server 0x61766973 (netcnf, fno 1/3) is not emulated.
- HIST.DAT: u32 count, 25-byte records {u32 player key, u8 1, u32 0, u16 year, u8 club 0-29 (255 = none),
  u8 0x81 batting / 0x82 pitching, 3 x u32}. Batting: AB:10 G:8 SB:7 | H:9 RBI:8 R:8 HR:7 | 0.
  Pitching: ER:8 BB:8 SO:9 G:7 | IP:9 thirds:2 W:6 L:6 | SV. Loaded at boot by 0x196398 into the table at
  0x10b4d20 (24-byte nodes of a red-black tree keyed by player, 3750 nodes, allocator 0x194768 with the cursor at
  +0x15f98). The allocator does not notice a full table: it hands out a live node, and the insert fix-up
  (0x194bb0) then loops for ever. 2005 file: 1304 players; a database with other keys adds its own on first use.
- Triangles: w0*z + w1*z + w2*z is not z in float; menu layers drawn at one depth with a >= test lost whole rows
  (the online screen's panels). Attributes are now interpolated relative to the last vertex.

## Session 20: HD pack from the archives, stable texture names, letters

- GIF PACKED: ST latches Q, RGBAQ applies it. Primitives that send (ST, XYZF2) with Q = 1 and no RGBAQ at all
  (one spectator model per walk-up shot, the helmet shine) ran with the Q of whatever was drawn before: static
  on the texture. Q = 1 from ST is now applied when the vertex has no RGBAQ.
- Texture names (WIDTHxHEIGHT_hash): menu art and fonts are uploaded smaller than the power-of-two texture that
  reads them; the rest was whatever lay in VRAM. The runtime records the uploaded rectangle per block address
  (upext_*) and zeroes the texels outside before hashing, dumping and sampling. Not part of .gsr recordings.
- Textures the game composes at run time, predicted offline (tools/hd/extract_textures.py):
  jersey numbers 64x64 = two 32x64 digit cells of f<team><slot>.ssh, the frame overwritten with the digits'
  commonest corner colour, a single digit at x = 16 on the palette-0 field; names 256x32, 16 px pitch, centred
  in the first 224 px, non-letters blank; left-handers get mirrored copies (numbers whole, names within the first
  224 px, cap badge x 70-126 / y 16-63, helmet hlm1 x 2-62). Checked against dumps of a match: 112 of 125
  numbers and 12 of 14 names predicted exactly.
- Upscaling (tools/hd/hdpack.py): residual orig - downscale(result): its low-pass part is colour drift (always
  restored), its high-pass part is grain (re-added away from edges, with synthesized finer octaves).
  Alpha: cut-outs (93 % of what is visible is solid) and clean on/off line art go through the model; letters with
  shadows (15-45 % in-between alpha) are enlarged bicubically and steepened between the local minimum and maximum
  (the model draws blotches into 1-2 texel shadows); more in-between than that is a glow and stays smooth.
- Font pages 256x256 (menu text, hashes 0b601f71..., b967f9c1...): palette entry 0 = texel (0,0) = the filler
  between the glyph cells, (242,246,242,a=56/255) on the face page and (0,0,0,36/255) on the shadow page; the
  cell backgrounds are alpha 0. Text is drawn scaled (10.5 texels on 10 pixels), so the filler tints the outer
  pixel of every glyph. At 2x-4x that is a thin box around each letter; an upscaler smears it further. The
  filler is found as: first row all that colour, 8-75 % of the page, >= 75 % of it in runs of 8+ (rows or
  columns), >= 25 % of the rest fully transparent, upright lines (column runs of 8+ outside row runs of 4+) in
  at least 6 columns. Pages >= 128x128 only: jersey numbers of some clubs (f090a...) have a similar frame that
  is drawn. Runtime: tex_gutter() when the internal resolution is above 1x; pack: hdpack.gutter().
- Sprites at Nx: the rasterizer covers sub-pixels before the first and after the last native sample point, so
  texture coordinates run up to one texel past what the console read (right/bottom, and left/top when the
  corner is at a fractional position). sprite_tex_range() clamps to the native first/last sample, widened by
  0.5 - 0.5/hs texels for an hs-times replacement texture; sprites wider than 64 texels are left alone (backdrop
  tiles that continue each other). Both renderers; the GPU one carries the range as a flat vertex attribute.
- The grey bevel at the top and left of the in-game overlay font (player name under the batter) is in the
  texture (256x128_2f6df33d...), not an artifact. The bands and lines seen for a second while the main menu
  fades in are the transition itself: 32 translucent rectangles (8x8 texture at 0x2da0, black at alpha 128 and
  white at alpha 61) sliding over the cross-fading backdrops; the 1x software picture has them too.
- Colour: Real-ESRGAN (both x4plus and general-x4v3) overshoots in chroma. EA's textures carry sharpening halos
  (texels like 227,244,248 next to red script on a grey jersey); the models turn them into (107,237,252). Not the
  restore step: the raw model output has it. hdpack.chroma_transfer(): Y from the model, Cb/Cr as a weighted
  mean of the 3x3 original texels (spatial sigma 0.85 texel, brightness sigma 18), so no colour can appear that
  the original did not have at that spot and colour edges follow brightness edges. Idempotent enough to apply
  to finished textures (build_pack.py REV 3, --repair-only); done on the 17,729 textures of the first pack.
- Software rasterizer: inside test used 1 - w0 - w1 for the third edge; on a row exactly on a horizontal edge
  that is -1e-8 for some triangles and the row is dropped (field mesh rows meeting at y = 112.0: a black line
  across the infield at 2x, where y*2 is a whole number more often). Third weight now from its own edge function.
- The index the first pack was built from lacked the mirrored cap/helmet badges of the disc's own uniforms (619
  textures, e.g. u000o hlm1 flipped at x 2..62 for a left-handed batter); a fresh extract has them. It also held
  9,443 stale name/number predictions from before the frame-colour rule; none occurs in any dump.

## Session 21: ages, jersey numbers, wall seams

- Ages: the game counts a player's age from playerattrib_birthday (days since 1947-12-31) to its own calendar,
  which always starts in 2005. Real birthdays made a 33-year-old of 2026 twelve (player card, and everything a
  dynasty derives from age). build_rosters.py now moves birthdays back by SEASON - 2005 years; the median birth
  year is 1980 again, as on the disc. Checked on the card: W. Contreras 33, A. Seigler 26.
- Jersey numbers (64x64) are composed in 32-bit colour on a canvas cleared to (19,19,19,alpha 0), and the frame of
  each 32x64 digit cell is overwritten with that value. Most fonts happen to have it as the digits' corner colour
  (the earlier rule); f025a does not, and a single digit leaves 16 columns of the cleared canvas on each side
  (not palette entry 0 of the font). 31,886 predicted names changed; dumps of a match: 114 of 125 numbers now.
- Wrapped padding for upscaling was applied to every ballpark texture in both directions. A wall texture with a
  white line along its foot got it along its top too (visible as a bright seam on the outfield wall). hdpack.tiles()
  compares the step across the wrap with the 75th percentile of the steps between neighbouring rows (x1.6, at
  least 4): of the non-transparent ballpark textures about a quarter repeat both ways, a quarter across only,
  an eighth down only, a third not at all.
- Free agents with modern rosters are still the 2005 pool (team "Eaa", Burnaby Argonauts: Delgado, Ordonez, Olerud,
  Alomar, Larkin...). The stats feed only has players under contract; the best of those who did not make an in-game
  roster are low-minors depth, which would make free agency pointless, so the pool was left alone.
- Two lists of 40 sluggers' names (Jon Dowd ... Todd Helton) sit in the executable next to the rewards and owner
  mode screens; nothing visible depends on them with the modern rosters (Home Run Showdown picks from the rosters).
- Checked this session with no errors: My MVP (profile, options, rewards, checklist, trophies, stats), Manage
  Rosters (statistics, career stats, free agents, trades, create player), Home Run Showdown into play, an
  exhibition match with the GPU renderer at 2x and the pack live (offscreen GL), dynasty and exhibition sweeps.
- Palettes with fewer than 256 colours: section 0x21 with n x 1 in its header and n entries (n = highest index
  used + 1). The loader puts them at the start of an otherwise zero 16x16 CLUT, read in CSM1 order: colour(i) =
  entry csm1(i) if that is below n, else 0. ssh.py only knew 256-entry palettes, so 3,248 shapes were skipped by
  the texture extractor: the fields' grass, dirt and detail noise (field*.ssh detl/fl##, cram.ssh pen#), wall
  pieces, in-game overlay parts, help buttons. 397 of them matched dumps at once. They had reached the pack
  only as dumps, processed like menu art (no grain): the infield came out smooth. Type 1 shapes (431) have
  2-colour palettes and odd sizes; the scoreboard font (128x128 4-bit) is not among the shape files.
- Texture alpha above 0x80: fonts 256x128_2f6df33d, 256x128_9626f88b, 256x256_0b601f71, 256x256_1b9846e3,
  256x256_b967f9c1, 256x256_ef25717e and the 4-bit scoreboard font 128x128_e3e6f840 have alpha up to 0xff and are
  drawn with vertex alpha 0x40 (A = At * Av >> 7). Dumps doubled and clipped alpha, replacements were loaded as
  (a + 1) >> 1: HD text at half opacity (grey name plates and scoreboard). tex_wide_alpha(): max alpha > 0xa0
  -> PNG alpha is the GS value, both ways. Jersey lettering only overshoots to 0x84-0x86 and stays "normal".
- "Flat artwork" test (neighbouring texels within 6, or under 64 colours) was also true for smooth photographs:
  every head, 62 % of the uniform textures, 70 % of the ballparks. Those were upscaled with grain restoration off
  and looked like plastic. Grain is now off only for menu art. Finished textures are repaired without the model:
  restore(original, finished picture) measures the same residual, because the finished picture is the model's
  output plus a low-pass correction (mean difference to a fresh build 0.3 levels, against 1.2 before the repair).
- hdpack.luma_clamp: brightness limited to the 3x3 original range (+6 +12 %), bounds upsampled smoothly. The
  scoreboard panel's bevel lines had become solid white strokes.

## Session 22: the calendar year, IOP heap

- Dates: {u8 month, u8 day, u16 year}; day numbers count from the static date at 0x8cf718 (1/1/1948, set by the
  static initializer in func_004ab9f8) and are 16 bits wide in the schedule records (0x70bdd0 clamps to 0xffff):
  that is why a dynasty ends in 2125. 0x4934b8 date -> days (leaves month - 1 in the caller's struct; adds 1900 to
  years below 100; whole 4-year blocks of 1461 days, 1460 when the block's first year is not a leap year, so the
  epoch year must be one), 0x493610 days -> date (0 or less: the epoch), 0x4ad948 days in month, 0x4ad9b0 leap
  year, 0x4ad7a8 weekday of a date, 0x4ada00 first day on or after a day number with a given weekday. Only the two
  conversions read the epoch.
- 2005 in the code: 26 static initializers each build the same five dates (3/1, 3/29, 5/15, 7/31, 11/1/2005; a
  header's constants, one copy per translation unit); 0x180e38 new calendar; 0x192410 year without a calendar;
  0x4b1e58 age = (days(3/26/2005) - birthday) / 365.25 outside a dynasty; first-season tests (0x5ebab8, 0x736a4c,
  second season 0x73b974); year - 2005 as an index: all-star host table 0x857218 (32 entries, 2005 Detroit,
  2006 Pittsburgh, ...; year - 1974 before 2005), schedule file (mlb162_1..3 by (year - 2005) mod 6, aaa mod 4,
  aa/a mod 2; 0x70bdd0 loads it and moves every game by (opening day of the year - first date in the file), the
  files' dates all say 2005), owner mode's per-season arrays (0x73ba60.., 0x756828, 0x758f04); contracts
  2004 + n (0x73ec00, 0x73eddc); Create Player's birth years 1961.0-1987.0 plus seasons played (0x66e890);
  end of a dynasty 2125, of owner mode 2035 (0x1efc08, 0x608230). Not years: 0x4216xx-0x4225xx and 0x42da20
  (uniform eras), 0x1fa65x (string ids), 0x589e00 (credit card expiry of the online sign-up).
- g_year_shift: tools/recomp/years.txt lists the 63 instructions (the recompiler adds the shift to the
  immediate; two lui/ori pairs build floats), wraps.txt the two conversions (recompiled as f_..._orig, the
  wrapper moves the year on the way in and out). Day numbers inside stay those of the original game, so the
  16-bit limit, saves and everything that counts days are untouched; weekdays come from the shown date and are
  the real ones. build_rosters.py: FIRST = SEASON + 1 once the season is over (15 September), birthdays stored
  FIRST - 2005 years early, HIST.DAT with real years for seasons before FIRST, year.txt for the runtime.
  Checked: Arenado 35, career lines 2025/2026, Create Player 1983-2009, dynasty opens Sun. April 4th, 2027,
  all-star game Tue. July 13th, trade deadline stop Sat. July 31st; original rosters: G. Anderson 32, lines to 2004.
- The Makefile only ran the recompiler when generated/functable.cpp was missing; it now runs when anything in
  tools/recomp/ is newer than generated/.stamp (unchanged files keep their date).
- IOP heap (sifrpc 0x80000003): fno 2 (free) was a no-op on a bump allocator. Every match allocates its sound
  bank (0x24440-0x310c0 + 0xdf70 bytes) and frees it afterwards: "IOP RAM exhausted" at the seventh match of a
  session. iop_alloc now has a first-fit free list with merging.
- Free agents are two hidden teams of league 10: Eaa (Burnaby Argonauts, 25) and Eab (Burnaby Chimaera, 25):
  39 unsigned players of early 2005 and 11 old-timers with birthdays in 1948 (unlockables). build_rosters.py puts
  SEASON's big-leaguers without a place there (same roster rows, new keys), each into a place of his own
  position where possible. The Free Agents screen shows them together with players the game makes up at run time.
- Roster selection ranked by regressed rates only: 109 of 750 big-league places went to call-ups ahead of regulars
  (Volpe, Salvador Perez, Chapman, Josh Naylor, J.P. Crawford, Holliday ...). pick() now adds up to 0.14 (hitters,
  at 380 PA) / 0.22 (pitchers, at 260 BF) for time in the majors, 60 % of that for AAA.
- Checked: a full season simulated from the calendar (all-star game, 31 July stop, playoffs day by day), awards,
  retirements, draft, contracts, free agency (Crochet 27 -> 28), "Sun. March 5th, 2028"; original rosters:
  "Sun. April 3rd, 2005". run/watch.py keeps an auto-pressed dynasty out of matches (pause, Return to Sim Options).

## Session 23: lineups, universal DH, main-thread speed

- Lineups (build_rosters.py): the old fill went position by position with a weak penalty for playing out of
  position (Soto at second, Turner as DH, Ohtani at first). Now an assignment problem (Hungarian method, positions
  x hitters): worth = bat + DEF_WEIGHT[pos] * glove * fit - (1 - fit) * 150, fit from games played there (1.0 his
  own position ... 0 cannot); DH = bat (+10 for DH types, who lose 25 in the field). Order: leadoff = best of the
  top six by on-base minus 0.35 power, two best bats, most power fourth, rest descending, pitcher ninth without
  a DH. 'DH' / 'TWP' are no longer primary positions (most-played field position with 20+ games, else 1B). Five
  starters per staff before the best remaining arms.
- DH: func_0018a9c0(db, league, left-handed starter, DH rule) returns lineup 0/2 (DH, vs RHP/LHP) or 1/3; the
  setting is *(*(*(0x7d66c8)+4)+16)+44 (0 league rule, 2 always, 3 never). One nop at 0x18a9c8 (option "dh", on
  with CPU play 2026) makes the league rule "always". Checked in Cincinnati: Reds with a DH, order as built.
- Texture unpack (dec_run): address tables were keyed by absolute base pointers (34,110 tables for 120k unpacks,
  5% in std::sort). VRAM addressing is linear in the base pointer, so one table per shape (14 in a match), offset
  at use. Results are kept by content: the 64-word blocks of both layers + 256 CLUT words + flags are gathered
  and hashed (4 chains), confirmed with memcmp, and the kept output is written back. 99.9% hits; 64 MB budget.
  dec_run 15.3% -> 2.4% of the main thread; MVP_DECCHECK=1: 0 mismatches.
- VU1: MVP_VU1REC records program runs (registers, micro memory, data memory, TOP/ITOP), --vu1-bench replays
  them (ns per pair + checksum 9e372ae1cecf8c86 for run/vu1_a.rec, 4000 runs of a match). Compiled blocks:
  Run kept in a local (registers) and written back once, pairs counted once per block, branch targets from
  the pc instead of the decoded pair (pairs stay constexpr), GNU vector types for the add/sub/mul family,
  -fstrict-aliasing for the block files only: 9.05 -> ~6 ns/pair. 8 more blocks found by recording (282);
  one of them, 42 pairs, had been interpreted 385M times in a 15-minute match.
- VU1 uploads: a match changes microcode 1,500 times a second (a different program for each material, ~190
  pairs each). Decoding those pairs again was up to 5.7% of the main thread; a memo of decoded pairs did not
  help (10,800 distinct (pair, place), and copying is no cheaper than decoding). Pairs inside compiled blocks are
  never decoded, so decoding is now lazy (the count of flag-reading pairs is kept at upload). Block look-ups
  were thrown away for all of micro memory at every change; now only those whose pairs changed (each start
  remembers how far it reaches). One table per content of the whole micro memory was tried first: 8,000
  contents in five minutes, no use. "VU1 other" 11.2% -> 4.5%. MVP_VU1CHECK=1: 135M block runs, 0 mismatches.
- Clock: host_time_ns() around every VU1 program, GIF packet and inline primitive was 2.4% (vdso). Now only
  with MVP_TIMING=1. MVP_PROF also writes file.maps; run/proflibs.py names samples in shared libraries,
  run/proflines.py splits one function by source line (needs that object built with -g).
- vertex_kick: 9.3% -> 4.2%. Per triangle it built a DrawState from the registers (2%), searched 64 palette
  entries of 1 KB each (2.2%) and looked the texture up (1.4%). For triangles all three are now kept until a
  state register is written (PRIM only counts when its value changes), s_prep_epoch moves (page generations or
  "drawn" marks the texture cache reads, palettes, the cache itself; drawing again into a page already marked
  drawn does not count) or the rasterizer queue's epoch changes (palette slots are protected per epoch).
  24 triangles per prepared state in a match. 76 recorded frames replay to identical pictures with and without.
- Uploads (xfer_data, 8-9%): 32-45 MB/s in a match, 2.3M uploads in 200 s averaging 4 KB, nearly all different
  from the last upload to the same place (texture streaming into shared slots), so nothing to skip. 32-bit rows
  are pairs of neighbouring words: four 8-byte copies per 8 pixels. The 8-bit loop (16 byte stores through the
  column table) is bound by fetching the block, which a rasterizer thread had last: a version with four 64-bit
  read-modify-writes was not faster (stores alone do not wait for the line). The upload hash runs in four chains.

### Simulated games (session 23)

- Engine object *(0x7d6d60); +76 -> state; state+8 -> game; game+20 -> team array, 3684 bytes a team: +360 lineup
  -> roster index, +380 pitcher's index, +996 players, +1000 + 52 * i player id, +2340 + 4 * split (+296 * table)
  stat objects, +3188 pitch attribute table, +3228 player attribute table, +3268 / +3272 lrattrib (vs L / vs R).
  game+7368 inning, +7369 outs, +7376 half. func_00194030(game, base) -> runner slot (byte index, 0xff empty).
- Accessors: 0x19f480(table, id, player) player attributes, bit fields of 60-byte records; id = position in the
  name table at 0x7d69e0 (speed 13, throwstrength 14, throwaccuracy 15, fielding 16, durability 18,
  platediscipline 19, stealing_aggressive 20, baserunning 21). 0x19c2b8 the same for pitch attributes (names at
  0x7d68e0: stamina 0, pickoff 1, fastball movement 2, control 4, velocity 5, then type / movement / description /
  control / velocity for pitches 2-5 from id 6). 0x182f10(table, id, player) = value / maximum (maxima: jump
  tables in 0x19c258 and 0x19f420; velocity 101, control 100, movement 15; minima are 0). 0x19e7b8 one split's
  batting line (11 bit fields in 12 bytes). Bat stat names at 0x7d6688 (g, sb, cs, r, then the lr ones).
- func_00716748 = one plate appearance. 0x717980 the managers (0x7199d0 pitching change, 0x7196c0, 0x719380),
  0x713bb0 intentional walk, 0x717fa8 altercations (suspensions), 0x717560 / 0x717710 ground balls (infield hit:
  speed * bunting * 0.2), 0x714f10 steal decision, 0x715088 steal outcome (speed, baserunning, the catcher's
  arm and glove, the pitcher's pickoff), 0x71b040 match-up set-up.
- The sim runs on ratings, not on last season's lines. Pitcher: three numbers from func_0019d2b0(type, movement,
  -, control, velocity) = quality of a pitch: fastball control / 2 + velocity * 1.5 - 0.93, others
  (control + movement) / 2 + 0.04, clamped to 0..1. P1 = (fastball quality + the other pitches' control) / n
  (0x752340), P2 = mean quality (0x752418), P3 = best quality (0x7524e0). Hitter: lrattrib contact / power,
  ids 20-22 (left / centre / right) and 24-26 (ground ball / fly / line drive), each normalised to shares.
- Steals: func_00714f10(engine, base): runner there, has not tried (slot + 8), outs < 2, lead < 5, next base
  open -> attempt with probability 0.3728 * (stealing_aggressive / 15)^2. One try per time on base.
- Baseline, original rosters to 12 July 2005 (88 games): 4.42 runs per team and game; AVG .388 .377 .367; HR 26;
  SB 35 35 26 24; ERA of the best starters 2.31 2.34 2.37. First modern database (119 games): 3.62; ERA 1.32-1.68
  for five starters; SB leader 19; AVG leaders three Rockies farmhands; a club with 2.5 runs a game.
  Big-league pitchers, 2005 vs new (stamina 60+ / under): P1 .725 / .725 vs .783 / .715, P2 .674 / .663 vs
  .671 / .701, P3 .825 / .817 vs .868 / .841, P2's 90th percentile .756 vs .811.
  After the fastball constant alone (0.9773): 3.88. After level-by-level mapping, secondaries built like 2005's
  (control 6.5 under the fastball's, movement around the pitcher's average) and the translation changes: P1
  .749 / .703, P2 .660 / .673, P3 .822 / .802; 4.63 runs (DH in both leagues), ERA 2.19 2.35 2.48, SB 37 34 33
  29, AVG .373 .366 .357, HR 26. Final database (free agents refilled), 31 July, 104 games: 4.57 runs; ERA 1.96
  2.27 2.50 2.52; HR 36 31 30; SB 47 34 32; AVG .358 .349 .344.
- quality() (who makes a roster) went negative for pitchers with minor-league time under the new translation,
  and the free-agent candidates were filtered by quality > 0: 11 of 39 places filled, and Jeromy Burnitz, a free
  agent of 2005, was signed by St. Louis in July 2027 at 58 and hit .316. Filter is now "has a line".
- Arm angles: Savant's pitcher-arm-angles leaderboard (csv=true), 832 pitchers for 2026, 596 for 2025, year to
  year 2.3 degrees apart on average. Under 8 degrees -> delivery 9 (13 pitchers with data), under 20 -> 8, else
  one of 0 1 2 3 5 6 7 drawn from the 2005 pitchers of the same hand. Deliveries 10-58 are signature motions.
- run/dyn.sh NAME "INI" makes a dynasty and simulates from the calendar (stops at the all-star break);
  run/leaders.sh NAME saves the standings (RS / RA) and the leaders. yr/procmem.py reads the EE RAM of a running
  game from /proc/pid/mem, yr/xref.py finds strings, address and call references, immediate arguments.
- Workloads: full 2027 season (final database, 2005 habits): IP 245 234 231, CG 12 8 7 (2005-like); playoffs,
  World Series and the draft ran without errors. func_007199d0 (pitching change): stamina left = current / full
  (0x752270); out below 0.3, kept down to 0.1 in some late situations. Raising 0.3 to 0.4 alone changed nothing
  visible. func_0071af18 takes 1.562 + random(0..1) per batter off the current stamina (attribute 27, written
  with 0x19c6c8): 1.85 under the 2026 rules (with 0.4): through 151 games IP 210 201 200, CG 4 3 3, 4.51 runs a
  game, HR 40, SB 57, best starter's ERA 2.56.
- Frame dumps were named frame_%05d: past frame 99999 they sorted before the older ones (scripts took stale
  pictures for the newest). Now six digits.
- VIF1: DMA blocks are parsed in place (vif1_parse over the game's memory); only an incomplete tail or a stall
  goes into the FIFO. memcpy / memset 7.6% -> 3.9% of the main thread, xfer_data 8.7% -> 6.4%.

## Session 24: portraits, league packs, uniforms, eras

- Portraits: DATA/FRONTEND/PORTRAIT.BIG holds 1487 photos named after the audio id ("%04d.ssh") and generic heads
  named by a 4-letter code (head a-o, hair colour a-g, 'd', facial hair a-h). Every builder (func_001dad10 in a
  game, 0x1dae30 / 0x1db050 in the menus) ends in func_001db260(a0, a1, id) "%04d" or func_001db1f0 (generic code).
  The player is the last one whose attributes were read: the database getter func_0018c308(db, attr, key) in the
  menus, func_0039f158(slot, attr) in a game (team = *(*(*(*a0)+1020)+1780)+20) + *(*a0+1740) * 3684, index = byte
  *(*a0+1744), key at team + 1000 + 52 * index). Both are wrapped and remember the key; the two loaders take the
  mapped picture number (mods/portraits-2026/portraits.txt). Checked on Edit Player and on in-game cards.
- Portraits are type 14 (3 bits a pixel: an 8-pixel group shares a base index, each pixel adds 0-3). A photo
  palette: 63 ramps of 4 colours, the k-means centres of the groups' own DXT-like ramps (ssh.image14_photo):
  33 dB on a re-encoded EA photo. The HD version is named by the runtime's hash of the decoded 128 x 128 texture
  (mix64(w, h), then FNV-style over RGBA words), checked against a texture dump.
- BIG overlays can add members: the directory grows into the data after it; members stored there move to the end.
  The game finds members by name (directories are not sorted).
- Capture: Edit Player > Appearance > Face, the screen's background texture replaced by plain magenta (cut-out),
  4x shots (.once commands MARK / RES n / SHOT path; NAME/frames holds a button). The camera angle is fixed (3/4
  view): the right stick does nothing there, the left stick changes the selected value.
- Number and letter fonts (f<team><slot>.ssh): the game draws the glyphs with their own indices and palettes;
  re-quantising them gave blocky numbers. Recolour palette entries only.
- Uniform textures (u<team><slot>.ssh): jerf/jers/jert fronts per body type, jr*l = their mirror images (long
  sleeves), jerk/jrkl backs, slvl/slvr, lega-d pants, hat (128x64: front panel with the logo at the right, brim
  top bottom-left, underside bottom-right, button top), hlm1 batting helmet, hhlm catcher's helmet (fixed layout),
  merf/merk/mslv jacket, llod low-detail atlas. tools/uniform_maker.py takes them apart (cloth chromaticity mode,
  thin lines = piping, solid shapes = lettering, collar = undershirt) and paints new colours with the old folds.
- League packs (tools/leaguepack.py): export and build give the same database byte for byte. team.dat's short
  code orders the Edit Player club list (by the code, not the shown name).
- Rookies: DATA/DATABASE/ROOKIE.BIG holds the generator's tables (rating spreads by star level and position,
  names, heights, retirement odds by age ...). func_006efc18 loads them; func_00676a58 (called from 0x6c9830) writes
  rookies with func_0018c488(db, attr, key, value) and func_0018c500(db, attr, key, string). MVP_TRACE_ROOKIE=1.
- The container restarts between turns: background runs die (the capture takes a club-skip argument).

### Simulated plate appearances, league rates (session 25)

- func_007169d0 is one pitch-level step of a simulated plate appearance (about 4.3 calls per plate appearance;
  the count bytes game+7370/7371 stay 0 there). func_00713558 fills twelve floats at the state (from the batter's
  split contact / power, plate discipline, speed and the pitcher's three qualities), func_007140e8 shifts them by
  the count, func_007142e8 is an empty stub, func_00716ef8 draws: +40 IBB, +36 HBP, +32 BB, +4 1B, +8 2B, +12 3B,
  +16 HR, +44 (0), +20 K, +24 / +28 outs in play; +0 = sum of hits. Means (1977 pack, and the same in 2027):
  K .203, BB .066, IBB .003, HBP .010, 1B .179, 2B .042, 3B .005, HR .018; seasons show K .194, BB .098, HR
  .027, the hits as drawn: walks and home runs about 1.5 times their buckets (other paths).
- Every era played the same league: 1977 pack .281 / K 19.4% / BABIP .332 (real .264 / 13.4% / .287), 2027
  .285 / 18.6% / .331 (real 2025 .245 / 22.2% / .291).
- hle_pa_probs (wraps 0x7140e8): bucket * target / running mean * correction, outs in play take the rest; the
  correction follows the current season's batting lines (slots 416 / 420, every 5000 calls, windows of 15000 PA,
  step (target / observed)^0.8 within 0.75..1.33). 1977 after four windows: .259 / .322 / .393, K .134, BB .083,
  HR .022, BABIP .281 (real .262 / .326 / .398, .135, .085, .023, .285).
- Database slots: current season 412 bstats, 416 / 420 split batting, 472 pstats, 476 / 480 split pitching; last
  season 400 / 404 / 408, 460 / 464 / 468; 444 fielding-like, 456 pitch attributes; at the end of the regular season the current lines move to the last-season slots and
  the current ones count the postseason. Pitching attribute order: g ip pip w l r er sv blsv gs cg sho pk wp;
  split pitching bb ibb so hbp 1b 2b 3b hr sf sh tpa (totals equal the batting ones outcome by outcome).
- Draft classes: a new dynasty makes two before its calendar exists (the dates seen are 2005 whatever the shift):
  now the first class is the first season's, the second the next one's (1977 pack: classes 1977 and 1978).
- Spread: with the plain scaling a simulated 1977's regulars struck out 11-16% (p10-p90; real 7-18%), led with 34
  home runs (real 52). Outcomes now scale as (bucket / mean)^spread (k 3.2, bb 1.25, 2b 1.15, 3b 1.3, hr 1.5),
  normalised by the running mean of that power. At k 2.6: K 10-17%, BB 4.6-12.8% (real 4.8-13.1%), half-season
  home-run leaders 29-31, league line unchanged (.260 / .323 / .397).
- Draft: the pack's class replaces the game's rookies of the same kind in order; the rest of the game's pool
  (it makes more than a class file holds) now get filler ratings (contact 22-48, power 15-42; fastball control
  35-55, 84-90 mph) and lose top-prospect status, so the real players head the draft board. Seen in a 1977
  dynasty's draft: Candiotti, Beene, Thurmond, Botelho, Escarrega, Snell, Cliburn, Mohorcic, Farr, Redus among
  made-up names.

### Base running, double plays, draft board (session 25, later)

- Outcome handlers (func_00716ef8): 0x751cd0 IBB, 0x717fa8 HBP (altercations), 0x751c98 BB, 0x7186f8 1B,
  0x7189c8 2B, 0x718d18 3B, 0x7190c8 HR, 0x751d08 K, 0x717eb8 +24 (ground out), 0x717b88 +28 (fly out).
  Runner bytes at game + 7380 + 12 * base (0 first), outs + 7369, half + 7376, inning + 7368.
- Runners on a single: func_00714b00 (on second: scores if speed > 0.6, speed read returns to 0x714bd0),
  func_00714cd0 (on first, s1 = 0 single / 1 double: speed read returns to 0x714dc0; to third above 0.7 with the
  ball's direction flags, home on a double above 0.8). Measured (MVP_TRACE_RUNS, 1977): 31% / 18% / 10%; now a
  draw a + b * speed (hle_rating_norm by return address): 62% / 29% / 43%.
- Extra outs (PA - outs vs times on base): 2.5 a nine innings, mostly double plays ending innings (ground outs with
  a runner on first and < 2 outs: about 60% became two). Fly outs scored a runner from third 29-31%, ground outs
  61-71%. Buckets now shift with the bases: runner on first, < 2 outs: 60% of +24 to +28; runner on third, first
  open, < 2 outs: 85% of +28 to +24. 1977 runs a nine innings: 3.66 -> 3.94 (hits) -> 4.11 (outs), real 4.47; the
  rest is reach-on-error, wild pitches and the like that the simulation does not make; per baserunner it is now
  right (0.33 vs 0.34).
- Draft class (func_006edc28): remakes its undrafted members (a1 = key) and then adds new ones with a1 = 0 (key
  returned in v0) until the database holds 3250 players: about 550 rookies a class. The pack's players went only
  into the remade ones before; now into both (and func_0067bd18's free-agent fill, also a1 = 0, outside a class,
  is left alone). 1977: 47 real players, 507 fillers in the April class; the board's top was made-up rookies who
  develop by their star level (starpower), so fillers now get 0 and the real ones 1-4 by career rank.
- Classes are made at a new dynasty (two) and again when a loaded dynasty starts its season (April); that April
  class is the one drafted.

### Faces, portraits, head models (session 26)

- Head templates: the Edit Player close-up (Face row: cap on; Hair Colour / Hair Style rows take the cap off) recorded
  with ".once PRIMS dir" for every head (Face 1-15) and hair class (styles 0, 1, 2, 9 and the short ones 3-8), 75
  recordings; which head/style a recording shows is read from its 128 x 256 face texture's hash (GHEAD.BIG names
  hashed: faces/ghead_hash.txt), not from counting presses (presses get lost; the first try mislabelled them all).
- render_league.py: per player the template of his head; face texture swapped for his made face at 4x, the
  template club's uniform pieces (found by matching recorded textures against u010a.ssh's shapes) for his club's,
  upscaled with realesr-general-x4v3 (the anime model is 5x slower), the 128 x 64 arm / neck skin tinted to his
  cheeks (no skin texture per tone exists on the disc; basebody.ssh has gear only). C rasterizer
  (tools/portraits/csrc/raster.c, ctypes): identical output, 9 s -> 1.3 s a frame. Runtime key = DB id's low 32 bits.
- build_faces.py: --part k/n workers append to faces_part_k.txt (resumable), --merge writes the database; MLB + AAA
  only by default (1509 players with generic heads in the modern rosters). Type-14 encoder fast path for ramps at
  multiples of 4 (vectorised); 2x HD faces from a 2x copy of the pieces (faces_2x.npz). Same seed -> same face at
  every size (size-dependent random draws moved after the others).
- Head meshes (g001-g045.ord): VIF unpacks V4-32 headers, V4-16 x 47 per batch (x, z span the int16 range, y only
  -19000..-16400: not plain positions; perhaps packed normals), V2-32 UVs, V4-8. Positions not found yet; new or
  reshaped head models wait on that.
- Eras: packs for 1910 ... 2019 (13), Eras page in the launcher (frontend.cpp), blurbs (tools/eras/era_blurb.py:
  majors only, stars of the next ten draft classes).

### Era logos, 2026 overlays under era packs (session 27)

- Overlays: a later iso_add_overlay wins (place_overlay replaces), so mods/uniforms-2026 (62 MODELS.BIG members, 5
  logos) and mods/stadiums-2026 (members of the 2005 park files) were put over an era pack's uniforms and old parks
  (the Guardians' "C" on the 1961 Indians; 2026 Busch members patched into the Sportsman's Park file). Now left
  out when the pack's pack.json says "era": true; g_opt_dh off before 2022 for era packs.
- tools/eras/era_logos.py: LOGOS.BIG l<n>.ssh shapes a (primary 128x64), l (wordmark: the city), g (cap), i (64x64),
  j (32x16), d / e (colour bars). Seen in the Home Run Showdown club picker (Main > Game Modes > Home Run Showdown:
  UP/DOWN cycles clubs, L1/R1 levels). Colours from the era home uniform's hat / jersey (hue families, the lit 40%).
- lahman_convert: "Tampa Bay Rays" was split "Tampa" / "Bay Rays" (TWO_WORD had 'Bay Rays'); fixed, and the 2012 /
  2019 packs' team.dat patched.
- tools/eras/era_names.py: slots of franchises not founded yet (art id -> first season) renamed for minor-league
  clubs of the day (team.dat location / long name / short name only; standins.txt lists them). era_logos.py gives
  those plain logos and, with uniform_maker on the Red Sox home / road cut (u010a / u010b: no piping), plain
  uniforms + the UNIFORMS.BIG thumbnail -> PACK/art/. Portraits of their players re-rendered
  (/home/claude/faces/standin_fix.sh).
- DH by era: func_0018a9c0(db, league, lhp, rule) -> lineup; option nodh (patches.txt: v0 = 0, v1 = 1 at entry: every
  park counts as the NL's) on for era packs before 1973; dh (universal) off for era packs before 2022.
- Roster rules (FEENG.LOC 4388-4391, 981, 1091-1093): each club 5 starting pitchers (primary position 0) and a closer
  (10), 10 fielders, its full roster (disc: 25 MLB / 25 AAA / 22 AA / 22 A), 6-15 pitchers in the majors, 100 per
  organisation. The era packs had 200+ clubs short of starters (the 1961 Senators had none), 30 short of players, 25
  without a catcher; tools/eras/fix_rosters.py fixes them inside each organisation (moves of bench players, swaps,
  reliever -> starter, made-up players when an organisation has fewer than 94), then each lineup's SP1-5 / CP / C
  roles go to a starter / reliever / catcher. roster.dat rows: key 000000000, [team, player, role, order] x 4 lineups
  (rh AL, rh NL, lh AL, lh NL). rosters-2026 had one (Montgomery, 4 starters). tools/eras/check_pack.py checks it all.
- The commentary overlay's DATABASE.BIG (a whole copy, loaded after the league's) hid every later change to the
  league's database (faces' hair / facial hair, era salaries, era names) with commentary on (the default): mvpdb's
  write_database now writes the copy again (refresh_commentary: only the audio ids kept from it).

### QoL (session 27)
- tools/custom_league.py: teams/*.zip (Team Maker) -> today's rosters with those clubs -> mods/pack-mine ("My League"),
  with rosters-2026's faces, portraits-2026 and commentary-2026 copied in (same keys). Launcher / F1 Eras page:
  "Today's rosters", "My League", "Build My League"; status lamps say "the era's" for uniforms / ballparks under an era.
- Era packs: 2026-rules hook only from 2015, more steals 1975-1992 and from 2023 (g_opt_hook / g_opt_steal).
- Mac sync: device_commit_files adds C2PA metadata to a file that starts like a PNG: raw tar chunks that begin with a
  PNG header arrive changed. Chunks are gzip now (/home/claude/bin/mkchunks; Mac side _old/ext.sh).

### Head models decoded (session 27)
- g001-g045.ord (MODELS.BIG): per VIF batch (<= 56 vertices) V4-32 header -> 0, V4-8 x3 -> 1-3, STMOD 1 (offset
  STROW = 1.0f) + V4-16 unsigned -> 4: the POSITIONS (x, y, z 16-bit fixed, centre 32768, +-16384; w a per-vertex
  value, 65535 on most), STMOD 0 + V4-16 signed -> 64: texture coordinates (s, t x 4096) + two small indices,
  STMASK + V3-32 -> 124, V4-8 x3 -> 184-186, MSCNT. (The earlier note that the V4-16 were "not positions" was wrong.)
  Checked: an affine map of (x, y, z) gives the recorded Edit Player close-up's screen x, y within 0.2-0.5 pixel.
  x is across the face, z up, y the depth (stored in a narrow band: its scale comes from elsewhere, header or
  matrix). The close-up uses g016-g030 (one per head, f01 -> g016 ... f15 -> g030; g001-015 and g031-045 are near
  copies with a few vertices more or less). Strips: w is not the ADC flag; the triangles can be taken from a
  recorded draw (UV + predicted position -> vertex index). tools/heads/ordmesh.py reads and writes the positions in
  place (same size), so reshaping a head is possible; new vertices are not (VU batch layout, relocations).
- Era commentary (tools/eras/era_speech.py): build_rosters gave every era pack the 2026 team-name remaps (Cleveland
  said as "Cleveland", the Angels as "the Angels" ...), and relocated clubs used the 2005 club's recordings (the 1961
  Kansas City A's as "Oakland Athletics"). Now per pack: same nickname elsewhere -> nickname only, same place other
  nickname -> place only, a different club (Expos, the stand-ins) -> its PBP index member renamed .hdx (the whole
  TNAMEHDR.BIG written, mvpdb.write_big) and the stadium announcer's too: the name is skipped. speech.txt per pack.
- lahman_convert split "Los Angeles Angels of Anaheim" as "Los Angeles Angels of" / "Anaheim": fixed (2012, 2019).

## Season's end: the one-game-playoff loop

- A dynasty could stop for good at a level's last day with "%1 one game playoff has been scheduled for %2"
  (FEENG.LOC index 676; "Continue Sim" brought it back at once, the date never moved). Seen in a 1977 league on
  5/6 September (the minor leagues' last day) and in 2027 on 3 October (the majors' last day).
- Path: func_001b3bc0 (one sim step) -> func_001b6638: today = func_00191c38(calendar *(0x7d66f8));
  func_0070d1f0(schedules, today) == 1 -> sim state *(0x7d6d60) +116 = 2, +128 = 7 (event 7) and return before the
  day advances. The front end (func_002013e0, case 7 -> vtable +832 -> func_00202a08) shows the message with the
  date of the last record of the level whose schedule grew (A, AA, AAA compared with the last dates it saw; the
  majors otherwise).
- func_0070d1f0: for each level 0-3 (schedules at +40 + 4 L, 28-byte records, u16 day at +0, word >> 27 != 0 once
  played): when the last record is played and today - its day < 4, the level's check (MLB 0x70f060: divisions
  0-5 and the wild cards; AAA 0x70f238: 6-12; 0x7507b0, 0x750888) returns 0 if every race is decided
  (0x70c238 / 0x70c450: each division's +32 == 4 and the wild-card lead safe), else adds a game for every exact tie
  at the top (0x710e08, 0x70de10) and returns 1; 0x70e2e0 then dates the new records (day 0) the day after the
  last one. A race still open without an exact tie adds nothing and returns 1 every time: the loop.
- Root cause: the races never closed because the divisions were wrong. The standings (func_00721288, rebuilt
  on every func_001b6220) take each division's clubs from func_006e0140, which reads a club's division with
  func_0018d588(db, 1, key) -> func_00182e88 (team.dat key -> row via the table's index at +16, 12-byte entries
  {key, row, row}). The index numbers the clubs in the order of their first rows in roster.dat, not team.dat's.
  The disc's roster.dat is grouped in team.dat order; build_rosters / the era tools appended rows out of order
  (Pawtucket after Boston, Tucson after Arizona ...), so every later club read the division of a club a few rows
  on: minor-league clubs in big-league divisions, leaders that could never clinch, the 2027 Nationals in the AL
  West. mvpdb.write_database now writes roster.dat grouped in team.dat order (sort_roster); every league's
  database was written again; check_pack.py reports a roster.dat out of order. A dynasty started before keeps
  its saved database: start it again.
- Safety net kept: hle_tiebreak_check (wraps.txt) keeps the original's 1 only when the four schedules changed
  (count and the last 32 days); otherwise 0, logged once a day ("[season]").
- Debug helpers added: MVP_LOGCALL_IF ("a3=684" or "a2:0=170"), MVP_LOGCALL_HIST=n (calls before), [logcall] and
  [watch] lines now printed in full with the shadow stack (trace builds), MVP_DATELOG=month (who makes dates of that
  month, with stack; dumps the four schedules' last records at the playoff message), MVP_TIEDEBUG=1 (the
  standings' first groups when the safety net acts), MVP_RAMDUMP=file (main memory at exit).

### Head templates again, with the varied heads (session 29)

- The 75 templates were recorded again with mods/heads on (tools/faces/record_templates.py, a studio run), so the
  portraits show the same jaw / cheek / chin as the head in the game; every league's portraits rendered again.
- With the modern rosters the face textures of heads 2-5 are the roster's own (GHEAD.BIG overlay), so their hashes
  are unknown and the recorder skips them: those seven (f02-f05 with styles 0 and 3-8) were recorded by hand, face
  number read off the screen (record_templates.py --as f02_s4 records whatever is showing).

### Wild pitches and passed balls in simulated games (session 29)

- The simulation never makes one (pstats wp 0 all season). hle_sim_pa (wraps.txt, func_00716ef8: a simulated
  plate appearance's outcome; a0 = the simulated game, +204/+208/+212 the runner records of first/second/third,
  +228 the scorer) first lets a pitch get away with "Simulated games: real league": with runners on, 2.7 % of plate
  appearances (3.6 % before 1920; sim.txt wild_pitch = rate), every runner moves up (func_00192e18 copy,
  func_00192e00 clear) and a runner on third scores through the game's own run event (func_007126f0: earned or not
  as the game decides). 1977 at the end of July: 4.10 -> 4.25 runs a nine innings (real 4.47); the rest is runs
  after errors (unearned 5 % of runs simulated, about 11 % real).
- Reached on error (hle_sim_out24): the runner record's +4 word is "reached on his own" (the walk and single set 1;
  func_007126f0 calls a run unearned when it is 0). A share of simulated ground outs (0.08 x the season's real
  errors a team game, Lahman: 7 % in 1977, 5 % today, 14 % in 1910; sim.txt error_rate) becomes the walk's base
  advance (func_007142f0, play type 13) without the walk's statistics event (event 58, func_00712938), batter's +4
  set to 0. 1977 by early August, with the wild pitches: 4.54 runs a nine innings with 9 % unearned at 0.091
  (real 4.47, 11 %); 0.08 kept. BB / AVG unchanged.

### 154-game seasons (session 29)

- SCHEDULE.BIG holds CSV text (DATE,TIME,HOME,ROAD,HALF; refpack): mlb162_1..3 (2431 rows: 2430 games and the
  all-star game AL-NL), aaa144_1..2, aa140_1..2, a140_1, mlbspr_1. tools/eras/era_schedule.py removes 120 major
  league games (8 a club, 4 home and 4 away, a seeded random matching) from all three -> PACK/DATA/DATABASE/
  SCHEDULE.BIG/ (an overlay of members). build_era.py does it when the season's median games a club are 150-159
  (1910-1960; strike seasons keep 162). The last four days keep every game: the game ends a club's season after
  its last game, and the league's after the last day.
- Games a season are also a constant: func_00196a48(league) = 162 for leagues 0-1 (AAA 144, the rest 140; jump
  table 0x854310). The standings count games left from it, so with 154 played no race ever closed and "Advance to
  Playoffs" kept preparing for ever. hle_season_games (wraps.txt) answers the pack's sim.txt season_games for the
  majors; era_schedule.py writes it. A 1927 dynasty: 154 games a club, into the playoffs.

### Indirect calls (session 29)

- A simulated season, profiled (MVP_PROF, software renderer): ee_find_func 6 % of the main thread (a binary search
  over 30 000 functions on every indirect call). Now a flat table over the code range (16 MB, built at the first
  call): +6 % frames in the same 300 s of season simulation. What is left on top: the software rasterizer
  (tex_sample, plot_t; not on the GPU path) and the game's own database lookups (func_004b59e8 binary search,
  func_004b8468, func_001a2d80: ~10 %), some 600 000 a second, nearly all from the daily player loop
  (func_006f3080 -> func_00700120 -> func_001897b0 -> func_00191480 -> func_0075c1b8 -> func_004b8568; sampled with
  the new MVP_LOGCALL_EVERY=n). hle_db_lookup (wraps.txt) keeps where each key was found per index and checks the
  entry still holds it (and is the first of its run) before answering; otherwise the game's search runs and the
  answer is cached. Together: 9600 -> 10500 frames in the first 290 s of a 1977 season; league rates unchanged.
  The reverse lookup func_001a2d80 (key of the first {key, a, b} entry whose a or b is a given value, a linear scan
  calling func_004b8468 per entry; ~200k calls from func_00700120 / func_006e13a8 via func_00183f50) is now
  hle_rev_lookup: the same scan done natively. Those two functions were ~6 % of samples; now they no longer show up.

## Uniforms by season inside one dynasty
- The dynasty's season: the big-league schedule (calendar object at *(0x7d66f8), schedule at +12: {?, count, records};
  u16 day number at +0 of the first 28-byte record). At boot the game builds a placeholder schedule (day 31140 with a
  1977 pack); that first value is ignored (no dynasty: the pack's own season).
- iso_big_switchable / iso_big_switch (iso.cpp): a BIG member gets a slot as large as its largest candidate and the
  directory points at it once; changing the look only changes which disc member fills the slot (zeros after it: the
  game's shape loader does not mind). era_uniforms_poll (every 64 vblanks) refills u/f<club>a/b when the season
  changes. Checked: exhibition in a 1977 pack with MVP_UNIFORM_YEAR=1940 puts the Cardinals and Red Sox in their
  1940 sets.
- Checked in a dynasty: a 1977 season simmed through the playoffs, draft, contracts and free agency into spring
  training 1978 -> "[era] season 1978" (Cleveland's 1978 set), and the spring game BOS at CLE shows it.
- Test harness note: season.sh's rules (X on every dumped frame) can leave the draft's "Are you sure" dialog and the
  Contract screen deaf for minutes (the game keeps drawing and reading the pad). With single presses a few seconds
  apart (the way a person plays) the draft, contracts (Down to MAKE OFFER, X) and both Auto Finishes go straight
  through. Steer the off-season by hand: echo BUTTON > NAME.rules.once with the rules switched off.

## Club names by season inside one dynasty
- The live database (*0x7d66f8): team.dat is the table at +84 (func_0018d588 & co.), not among the slots 300..700
  the dump walks. Its vtable (table + 32): +40/+44 number getter (a0 table + adj, a1 attribute, a2 row; number 1 =
  division, 2 = art id), +48/+52 string getter (a3 = buffer: 0 unique id, 1 nickname, 2 abbreviation, 3 city),
  +64/+68 number setter, +72/+76 string setter (a3 = string, t0 = 0). Rows from the index at table + 16.
- era_teams_apply (hle.cpp) runs as a native job (kernel_queue_native; guest stack 0x7fff0, strings at 0x7e000) when
  the schedule's season changes, and sets city / nickname / abbreviation of every slot whose team_years.txt entry
  differs between the old season and the new one. The menus show the new names at once (calendar: KCA after
  MVP_TEAMS_TO=1958 on a 1977 save) and the dynasty's save keeps them.
- Uniforms and logos per season: every club's u/f a/b and l<club>.ssh are switchable (iso_big_switch now also takes
  "@file" candidates). The pack's own season and minor-league stand-ins ("|S" in team_years.txt) keep what the pack
  built; otherwise the season's wardrobe slot (or the 2005 a/b) and, for a name not the 2005 club's, the logo from
  mods/era-logos (by city|nickname), or the disc's.
- Ballparks per season: iso_file_switchable / iso_file_switch (iso.cpp) do for whole files what the BIG members do:
  each DATA/STADIUM/<2005 park>*.BIG of a club in park_years.txt (tools/eras/era_parks.py) is as large as the
  largest park it can become and is filled with the season's old park (its first file), the 2005 park ("=") or the
  pack's own ("", its season). Checked: San Diego at home in a 1977 pack forced to 1960 plays in PETCO (padded with
  zeros, loads fine). The park names follow too: loc_track (loc_patch.cpp) gives every string that is exactly a
  tracked park's 2005 or pack name room at boot; loc_ram_set rewrites them in the tables the game loaded. The game
  loads FEENG.LOC in the menus and IGENG.LOC in a match and drops them again, so loc_ram_refresh (every 64 vblanks)
  keeps the tables it found, checks them, looks again (at most every 3 s) when one is missing, and puts every name
  set so far back in (log "[loc] string tables at N places in memory").
- LocFile::parse keeps the room (NULs up to the next string) an earlier patch left, so patches stack.
- Club names in the menus: the screens build a club's name from the database city and the string tables' nickname
  (FEENG.LOC 384.. nicknames, 416.. full names; IGENG.LOC 137.., in the game's division order), so an era pack
  showed "Denver Rockies" for its Denver Bears. At boot (era packs) those 60 strings get the pack season's names,
  with room to grow (NULs inside the string); when a dynasty renames a club, loc_ram_set_club finds the loaded
  tables in RAM by their "LOCL" block and string count and rewrites that club's two strings in place (both tables).
- Fixed (u31t): hle_rev_lookup (u31m) compared the 32-bit entries with only the low half of a1 and gave the key
  back zero-extended; the game's loop compares sign-extended words with the whole 64-bit register and returns an LW.
  The difference changed which pitchers the simulation thought rested: a simulated 1977 had starters with 30
  relief appearances and 430 innings, relievers in 120 games (top IP 245 / 1.5 relief games a starter without it,
  240 / 0.5 with the fix). hle_db_lookup now also caches only keys that occur once in their index (a binary search
  over duplicates need not land on the first).
- Starter workloads by era: patches.txt now has float patches from runtime variables ("=g_name": the lui / ori pair
  that builds a float constant takes the variable while it is above 0), used for func_0071af18's tiring per batter
  (g_sim_drain) and func_007199d0's pitching-change threshold (g_sim_pull). Under the 2026 rules 1.85 / 0.4 as
  before; era packs before 2000 follow the dynasty's season (hle.cpp sim_workload_for, set for the real share of
  complete games); sim.txt "drain" / "pull" override. 1977 at 1.10 / 0.15: complete games 20.8 % of starts (real
  21.6 %), top 267 innings, ERA 3.98 (real 3.91); before: 3.7 %, 240.

- Rotations (u31z): a club's rotation sits in its 480-byte record (+0 pitcher keys, +20 slot bytes, +24 slots = 5,
  +32 whose turn); func_001b5d98 moves both clubs on one slot after every game (func_00749510), whatever the rest;
  func_006e3028 only picks the slot to open a schedule with (first of three back to 90 % stamina). So simulated
  starters never passed 31-33 starts. hle_rotation_next (wrap of 0x749510): before 1975 the five's lowest-stamina
  man is skipped twice in three turns when the next one will be rested (90 % after a day: + 15 % of full + 10,
  func_0074e690): the four make 37-39 starts. MVP_ROTATION=4/5 overrides (debug). Era test rules: the autopress
  rules file was read into 1 KB (rules past it silently dropped: a dynasty "stuck" on Auto-Fix); now 4 KB.
- How simulated starters leave (MVP_TRACE_SUBS, 1951): func_00717980 asks, before every batter, func_007199d0
  (fielding club: change pitchers?) and func_007196c0 (batting club: hit for the pitcher? rule sets func_00751dc0 /
  00751f38 / 00751fc8 by inning, score, outs, stamina left and a random number; func_0071a050 makes the change).
  A club's first change takes its starter out: 87 % of those were pinch hitters, in the 6th-8th inning, with 10-40 %
  of his stamina left (game + 80 + club * 24: now +24, full +28; func_00752270). Leaving fresh (50 %+) pitchers in
  changed nothing (they are rarely fresh by then), and a no re-rolled at the next batter becomes a yes. Now
  (hle_sim_ph_a/b/c): a pitcher with 25 %+ left bats for the rest of the game with chance g_sim_ph_keep (one roll
  per club and game). 1951 full season: keep 0.7 -> complete games 40.2 % (30.0 % without; real 38 %), top 306
  innings, ERA 4.05; drain 0.5 with keep 0.5 -> 40.8 %. Era table now 1910-2000 with a keep column.
- Then (u32a): in 1910, with every fresh starter batting, the fielding club's own late changes (func_007199d0, 7th-9th
  inning, 20-70 % stamina left) held complete games at 52 % (drain 0.35 or a 12 % threshold: no change). "hold": per
  club and game, while the starter is in with 25 %+ left, func_007199d0 is not asked (except when func_001a95e8 says
  he is hurt / thrown out: skipping that once crashed a 1910 season, "write outside RAM" at 0x5ae9f4). hold 0.5 -> 74 %,
  0.85 -> 91 %, 0.22 -> 61.9 % (real 62.1). Era table (drain, pull, keep, hold): 1910 0.50 0.04 1.00 0.22, 1927 0.62
  0.06 0.90 0.02, 1941 0.70 0.07 0.75 0, 1950 0.75 0.08 0.60 0, 1965 1.10 0.13 0.15 0, 1977+ as before. Results in hle.cpp.
  Debug: MVP_DRAIN / MVP_PULL / MVP_PH_KEEP / MVP_HOLD / MVP_PH_RATIO / MVP_ROTATION override with the era rules on.
- Home runs of an era's sluggers (u32b): ratings are quantile-mapped onto the 2005 game's, so every era's top is
  rated 96-100 and the simulation gives the best power hitters about 2x the league's home runs a PA (simulated 1927:
  Ruth 15, Gehrig 22; real 60, 47). The sim reads only contact (0) and power (1) of the split batting tables
  (MVP_TRACE_ATTR; lrattrib_hr_pct is never read). Power -> home runs over six eras' simulated seasons is one curve
  (65 -> league rate, 90 -> 2.05x, 100 -> 2.45x). tools/eras/hr_factor.py writes mods/<pack>/hr_factor.txt: real
  HR/PA (last two seasons, 4 home runs of prior at the rating's rate) over the curve, 0.25-5 (Ruth 2.61, Foxx 1.65,
  Kiner 1.55, Kingman 1.77; slap hitters ~0.5); build_era.py runs it. hle_pa_probs multiplies the batter's home-run
  bucket by it (the draw takes the bucket as is: func_00716ef8, +16 -> func_007190c8; func_00717278 can make the
  outcome itself, a catch-up for trailing clubs behind a flag never set in these games).
  Who bats: func_007169d0 (one plate appearance): fill func_00713558(state = game + 20), probabilities
  func_007140e8, draw, tiring. The fill reads power for the batter, the next batter, the batter again, from the
  game's per-club rating copies: row = slot in the club record (game + 8 -> +20, clubs of 3684 bytes, key at
  club + 1000 + 52 * slot) of the club number game + 80 (the other club gave a random hitter's factor: with Ruth at
  20x, the right club made him hit 178, the wrong one 16). Simulated 1927 to 19 September: Ruth 46, Fournier 32,
  Ken Williams 29, Cy Williams 28 (league .278 / K 7.3 % / HR 1.0 %, real .277 / 7.2 / 1.0); 1961: Williams 61,
  Robinson 57, Killebrew 56, Aaron 52, Mantle 46 (league unchanged). A bucket times 2.6 without the right club moved
  nothing: league totals are held by the correction, so a factor on the wrong man is noise.
  Caution: a dump after the season summary shows other numbers in the last-season slots (1927: K 14.6 %); dump at
  the draft or before.
- Hits in play (u32c): the simulation gave contact 90 only 5-7 % more hits in play than the league (flat by speed
  too), so batting races were luck (simulated 1910: Bush .340, Cobb .280; 1941 Appling .358, Williams .300). Third
  column of hr_factor.txt: real (H - HR) / (AB - SO - HR + SF), 300 balls in play of prior at the contact curve's
  rate, over the curve, 0.7-1.4 (Cobb 1.22, Boggs 1.19, Heilmann 1.13); hle_pa_probs multiplies singles, doubles,
  triples. 1941 at the World Series: Foxx .348 / 39, Williams .345, Mize .343 / 34, Greenberg 31, DiMaggio 29
  (league .261 / .326 / .372, as before); 1910: Speaker .332, Wagner .330, Cobb .326, Crawford 12 HR, Cobb 11.
- Pitchers (u32d): strikeout leaders were compressed too (simulated 1910 top 161, real Johnson 313, Walsh 258;
  1941 Feller 166 / 260; 1969 top 209 / McDowell 279); regulars' simulated K rates 0.73-1.23x the league (10th-90th
  percentile), driven by fastball control most (r 0.65-0.74), then velocity (0.05-0.45 by era), little by movement.
  tools/eras/pitch_factor.py PACK LAHMAN DUMP: least squares of the dump's K and BB ratios on velocity, control,
  best movement, other pitches' control (clamped 0.5-1.8; a pitcher of the dump: his own simulated rates with 300
  BF of the fit), real K/BF and BB/BF (last two seasons, 400 BF of prior) over that, 0.4-2.5. Needs a simulated
  season of the pack without pitch_factor.txt (so not part of build_era.py). Runtime: the pitcher is the current one
  of the club other than game + 80, slot byte at club + 380 (checked: Cicotte / Hughes alternating by half inning).
  Dumps used: 1910/1941/1951/1961/1969/1977/1999/2004/2019 last-season slots at the playoffs or draft, 1927/1986/
  1993/2012 current slots in late September (a modern pack's last-season slots also hold its real prior season:
  2012 showed 8946 starts). Simulated 1969 with all factors: K leaders Lolich 249, McDowell 243, Tiant 238, Singer
  236, Jenkins 224 (real 271, 279, ..., 273; before 209 at the top), ERA Gibson 1.60, Singer, Jenkins, Marichal;
  complete games 25 %; league .245 / .313 / .363, K 15.4 %; HR Howard 65, Yastrzemski 57, McCovey 51 (real tops
  48-49: the top home-run seasons run a little high).
- Steals by era (u32f): func_00714f10's attempt constant (0.3728; 0.48 under the 2026 rules) is now g_sim_steal
  (patches.txt var), by season in era packs (hle.cpp sim_steal_for). The simulation stole 1.0-1.5 % of PA in every
  era (real 0.9 % in the 1940s-60s to 2.1 % in 1986, 3.5 % in 1910). Attempts scale with the constant but go to the
  fastest (0.3728 * (stealing_aggressive / 15)^2): at the 1910 total Collins stole 167-171 (real 81), and at 0.3728
  1951 gave Jethroe 73 (real 35). Table = geometric mean of what total and top three needed (1910 by its leaders:
  its totals count the extra bases scorers called steals). Results: 1986 total 2.05 % (2.06), leaders 107 98 93
  (107 87 80); 1910 leaders 77 74 70 (81 70 65); 1951 35 31 25 (35 31 29); 1941 Case 45 (33).
- The 1910 crash: not asking func_007199d0 (the "hold" skip) left a club's game record (e.g. 010d0fd4) with garbage
  byte lists (capacity 1927843690, pointer 4b42f891) that func_001aeae8 (the club's game set-up, from 0x1a8908)
  cleared past RAM (memset via 0x5ae9e0; "write outside RAM" 0x2000000). Reproduced twice in two runs with the trace
  build (build_tr, make BUILD=build_tr OPT="-O1 -DEE_TRACE": the call stack in the FATAL report), never without the
  skip. Now the hold lets func_007199d0 run with the starter at full stamina and its late rule off (patches.txt
  holdlate: 0x719d2c slti v1, 3 -> 0, from the 7th inning a club not ahead changes; g_opt_holdlate set for that
  call). hle_club_setup (wrap of 0x1aeae8) logs and empties such lists anyway. 1910 hold 0.25: complete games
  61.2 % (real 62.1), 1927 0.02: 48.9 %, 1941: 45.1 %.
- Modern rosters (u32g): a simulated 2026 season (rosters=1) plays like 2025 (.242 / .311 / .395, K 22.6 %, HR 3.0 %;
  Judge 55, Crow-Armstrong 49, Ohtani 48). Starters went long (top 219-230 innings, complete games 1.4 %; real 2025
  192-207, 0.6 %): the 2026-rules tiring is now 2.0 (2.05 gave 202-218 and 0.3 %). The era-pack factors were not made
  for them: the modern builder rates power from Statcast data, so the era packs' power curve does not hold (it would
  have made Ke'Bryan Hayes 1.79 and Corbin Carroll 0.65), and their ratings come from the same seasons anyway.
  (2025-26 lines as Lahman-style CSVs from game/rosters/mvp_statsapi_2026.txt: /tmp/claude-0/lah26.)
- Hitters' strikeouts and walks (u32h): the simulation squeezed hitters' K rates together (1977 regulars 9.2-14.4 %,
  real 6.7-17.7; Dave Cash 11 % for a real 1.8, Kingman 16 for 26.5; 2026 Arraez 16 % for 3.7, Jacob Wilson .227
  with 107 K; sim vs real K by hitter r 0.47 (1977), 0.74 (2026)). tools/eras/bat_factor.py PACK LAHMAN DUMP, as
  pitch_factor.py (fit on contact, power, plate discipline; regulars' real rates over the real regulars', 200+ PA,
  so pitchers batting do not set the baseline), writes bat_factor.txt "key K BB"; the runtime multiplies the batter's
  with the pitcher's. Simulated 1977: r 0.91, Cash 3.9 %, Kingman 25.8 %, regulars 7.3-17.3 %; 2026: r 0.92, Arraez
  4.1 %, regulars 14.7-29.3 % (real 13.6-28.7); league lines unchanged. All 13 era packs and rosters-2026 have one.
  rosters-2026 also has hr_factor.txt from hr_factor.py --hits-only (home runs 1; a simulated 2026 gave contact 50
  1.06 and 90 0.99 times the league's hits in play): hit factors only (Judge, Witt, Ohtani 1.08; Raleigh 0.88).
- Two seasons in one dynasty (1977 pack): the off-season ran with no errors (draft with the pack's 1977 class:
  Smithson, Dravecky, Candiotti, Redus; Auto Finish in Draft, Contract Signing and Sign Free Agents; spring training
  March 1978). 1978 to late July: complete games 22.8 %, ERA 3.94, .256 / .326 / .382, K 12.5 % (real .255 / .320
  / .375, 12.6 %). Walks ran 10.0 % in April-May (target 8.5) and 8.5 % after: the correction re-learns after the
  year changes. Test harness: the season rules do not get through the off-season (X on the draftee contract screen
  goes nowhere); Start on the draft (then Down, X), Auto Finish (4 x Down, X, Down, X) in the two signing menus.
- Today's bullpens (u32j). How a simulated club changes pitchers: func_007196c0 (the club whose pitcher is in; its
  rule sets 0x751dc0 / f38 / fc8 say when) calls func_0071a050, which first looks by the inning and score (closer
  group 4, setup 3, the others 1 / 2) for a reliever rested (func_007520a8: 85 %+ stamina left, unused this game),
  and a starter gets no more than that look unless spent (0x752180 under 0.01) or hurt; for a reliever, or a spent
  starter, then groups 1 2 3 0 4 (1 2 3 4 0 with under three innings left) rested, then tired ones too. Group 0 is
  the rotation (back to 90 % after a day: func_0074e690 + 15 % of full + 10). func_007199d0 (fielding club) almost
  never changes anyone (5 of 17,000 starters in 2026; MVP_TRACE_SUBS). In a simulated 2026, relievers went 1.56
  innings an appearance (real 2025 0.98), 2.09 relief appearances a start (3.29).
  Now (relief factor 3.2 under the 2026 rules / era packs from 2015, 1.8 2005-14, 1.7 1995-2004, less back to 1970,
  hle.cpp sim_steal_for):
  - hle_sim_tire (0x71af18): a reliever (not the game's starter slot) tires 3.2 times as fast, the closer (group 4)
    1.6, the database's stamina too (attribute 27, carries to the next days);
  - hle_sim_pick (0x71a558) from 1995: the rotation is asked only for a pitcher hurt or thrown out, the closer
    before it; a reliever under 50 % left (g_sim_floor) is not used; hle_sim_avail (0x7520a8): nor one who pitched
    on 2 of the 3 days before (the calendar's day, g_sim_today; games begun count all levels: about 120 a day);
  - patches.txt g_sim_spent 0.25: a starter under a quarter of his stamina gets any reliever, tired or not;
  - 2026 rules: a starter who has faced 27 batters is seen as spent (hle_sim_hit_for_p; MVP_BFCAP), and a reliever
    into a new inning with chance 0.6 (MVP_ONEINN; 0.7: 3.24 relief appearances a start, 1.09 IP each, complete
    games 1.5 %, top saves 31; 0.5: 3.13, 1.12, 1.1 %, 31).
  Steps on the way (each a simulated 2026): factor 1.7 alone, 2.46 relief appearances a start, 1.40 IP each, top
  reliever 94 games; 2.5, starters relieved 5.9 times each (the rotation came first: relievers rarely back to 85 %);
  rotation last, 3.0: complete games 3.1 % (starters stayed when no reliever was rested; "rule yes" 70,303 for
  26,929 changes); any tired reliever for a spent starter: relievers in 157 games; rest by "games begun / 30" did
  nothing (the minor leagues' games count too); one factor for all at 3.2: top saves 27.
  Now (one season; seasons differ by a few): complete games 0.5 % (real 2025 0.6), 2.99 relief appearances a start
  (3.29), relievers 1.20 IP each (0.98), top games 81 80 80 (81 80 79), top saves 36 35 30 (42 40 38), top innings
  217 215 212 (207 205 202), league line unchanged (ERA 3.88). Real figures: Lahman.
- Closers (u32k): with any reliever group asked in any inning, closers pitched 75 games and 95 innings with 25-31
  saves (top 30 savers 51 % of all saves; real 2025 62 %, top 42). hle_sim_pick: group 4 (the closer) only from the
  9th inning on or in func_0071a050's first look (its save spot: last inning, ahead by 1-3). Simulated 2026: top
  saves 36 36 36, closers 63-78 games and 68-78 innings, 27 pitchers with 20+ saves (real 21), all saves 1204 (real
  1201), top 30 67 %; relief appearances a start 3.20, 1.10 IP each.
- Complete games under the 2026 rules (u32k): 1.4 % of starts with the one-inning relievers (real 2025 0.6 %): a
  spent starter stayed when no reliever was past the rest days and the 50 % floor. hle_sim_change (0x71a050): for a
  starter under g_sim_spent (or past 27 batters), a second try without them (from 2015). For relievers too: relievers
  in 134 games. Starters only: complete games 0.2 %, top innings 220 213 213, top saves 38 35 34, relief appearances
  a start 3.14, 1.10 IP each, top games 84, ERA 3.97.
- Loading (u33a). Boot to the title took about 35 s of movies and waits. (1) func_0026e3a0 waits until 240 vblanks
  after a noted time before showing anything (4 s of black; the main thread just polls the counter): patches.txt
  fastboot makes it 0. (2) The disc module (func_005a6628 and neighbours, 0x5a6628-0x5a7c00) waits 480 hsyncs (30
  ms) with func_005a65c0 (sema + alarm) after every read, often twice; our reads finish when the call returns:
  hle_cd_delay makes those waits 4 hsyncs (40 directory reads at boot: 2.5 s). Match loading does not go through
  it (the streaming reader thread, 5a6c48, reads asynchronously) and is CPU-bound. (3) The boot movies: func_001ce958
  (player, movie number in the table at 0x7d78e0: 9 easports, 11 mvpintro, 19-56 cameos) called from func_0066b100
  (ra 0x66b3f4) for the EA logo, a random cameo and the intro: hle_movie_start asks for movie -1 (stop; the player's
  finished callback runs) unless settings intro_movies. Title screen now at game frame ~400 (was ~1800).
- Launcher / menu (u33a): rebuilt in a broadcast style (runtime/src/frontend_menu.inc): full-bleed park art with a
  slow drift and crossfades, frosted glass (a 1/6-size blurred copy of each picture drawn behind panels in the same
  mapping), a hub of tiles, a leagues carousel, segmented controls and sliding switches. UiBatch gained gradients
  (UI_GRADIENT: alpha ramp in the texture coordinates) and an underlay (quads drawn before the game picture), which
  lets the game picture sit in a live-preview card on the Display / Graphics / Textures / Image pages (in game it
  glides from full screen into the card). Fonts: Poppins (OFL) for text, TeX Gyre Heros Condensed Bold Italic for
  headings, rasterized at 56 px into a 2048-wide atlas that is mipmapped.
- Renderers (u34): gs_gpu.cpp keeps the batching and hands GpuBatch runs to a GpuBackend (OpenGL in gs_gpu.cpp,
  Metal in metal.mm, Vulkan in vulkan.cpp); each API has its presenter, chosen by Settings > Graphics API (Auto:
  Metal on Apple GPUs). Built and checked on an M5 Mac: Metal and Vulkan (MoltenVK) screenshots of the same menus
  agree on 99.6 % of pixels within 8/255; both draw the 3D demo correctly. What mattered for speed: the game makes
  2,000-6,000 new textures a second in the 3D demo (~20,000 per 5 s). Metal: a pool of freed textures reused once
  the command buffers that used them finished (completion serials), and uploads copied from a shared staging
  ring by the GPU instead of replaceRegion (the CPU swizzle took 1.1 s of every 5): 3D demo from ~55 % to 93-95 %
  game speed (30 frames/s, the game's own rate). Vulkan: VkImage pool (new image + allocation per upload was the
  slowest path under MoltenVK), lazy mip chains, and mip chains averaged on the CPU and uploaded in one copy (GPU
  blits make a render pass per level under MoltenVK: 14 s of GPU per 300 frames with 16x anisotropic filtering).
  MVP_GPU_PROF=1 prints where the GPU-side time goes every 5 s; MVP_VK_PROF=1 the Vulkan fence/acquire waits.
  Mac build/test: tools/mac/*.command (double-click in Finder; logs next to them), tools/mac/gfx.mk adds the Metal
  and Vulkan rules to an older Makefile.
- Texture re-sends (u34b): the game sends its textures to VRAM again every frame, which bumped the page
  generations and made the GPU path decode and upload them again. gpu_texture now hashes the VRAM pages first
  (when they are not much bigger than the texture) and then the decoded texels, and keeps the GPU texture when
  either matches: in the 3D demo on the Mac, 20,000-50,000 uploads per 5 s became ~1,000, and Metal and Vulkan
  both run it at 96-101 % game speed (was 55-77 %). Metal: state set only on change, vertices in the staging
  ring, per-draw values in one buffer (base_instance carries first vertex / vertices per primitive). Vulkan:
  texture copies queued and recorded together per flush; GS push constants packed into 28 bytes (the 128-byte
  minimum every driver allows; SwiftShader / some mobile drivers have no more); GPUs without dual-source
  blending blend through the target copy; anisotropic filtering off under MoltenVK (Metal is the Mac path).
