# vice-gsync: changes to VICE in this fork

This fork (`grue74/vice-gsync`) is upstream VICE (`VICE-Team/svn-mirror`,
`main`) plus the changes below. Each section says **what** was changed and
**why**. `main` is kept as upstream `main` with these branches merged in:

| Branch              | Contents                                                        |
|---------------------|-----------------------------------------------------------------|
| `gtk3-smooth-video` | DirectX smooth video, CPU turbo, reSIDfp fix, CMake/MSVC build  |
| `ultimate-uci`      | Ultimate Command Interface (UCI) emulation, netsid warp capture |

To update from upstream: `git fetch upstream && git merge upstream/main`.

---

## 1. GTK3 / DirectX: smooth video for VRR (G-Sync) and CRT beam simulation

Files: `src/arch/gtk3/directx_renderer_impl.cc/.h`, `directx_renderer.c`,
`render_queue.h`, `src/video.h`, `src/video/video-resources.c`,
`src/video/video-cmdline-options.c`,
`src/arch/gtk3/widgets/canvasrendervsyncwidget.c`

**Why:** on Windows with a G-Sync / FreeSync display, scrolling in the GTK3
build was jerky, both windowed and fullscreen. The emulator is paced by the
audio device, so frames reach the renderer in bursts (with DirectSound the
write cursor moves in ~10 ms steps, so frames arrive 10/20/30 ms apart).
Presenting each frame the moment it arrives gives an uneven picture,
especially on a variable refresh rate display, which shows every frame as
soon as it is presented.

**What:**

- **Flip model swap chain** (3 buffers, tearing allowed when VSync is off).
  The old bitblt model can't do variable refresh rate in a window or in
  borderless fullscreen.
- **Frame pacing** (VSync on): frames are presented on a steady clock at the
  emulated frame rate instead of when they arrive. The clock is only ever
  slewed, never stepped. The latency it keeps covers the worst frame arrival
  plus draw time over the last 30 s, and latency that isn't needed is
  trimmed every 5 s. GPU work is flushed before each wait.
- **Frame doubling:** each frame is presented twice when the display is at
  least 2.2x the emulated rate. This keeps a VRR display away from its
  minimum refresh rate (50 Hz PAL is close to many panels' 48 Hz floor).
- **Render queue of 4 backbuffers** on Windows, so a burst of frames isn't
  dropped.
- **CRT beam simulation (rolling scan):** each emulated frame is shown as
  several subframes, and a D3D11 pixel shader lights the rows as a simulated
  beam passes them. On a fast display this gives CRT-like motion clarity
  without the flashing of plain black frame insertion. Two settings:
  persistence (how long a row stays lit) and phosphor decay (afterglow).
- **Swap chain retry:** when no swap chain can be created (for example with
  no interactive desktop), the D3D device is kept and the swap chain is
  retried every 500 ms. Before this change everything was recreated every
  frame, which used a whole CPU core and slowed the emulator to ~0.3x.

**Settings:** Settings > Host > Display (Windows only; the other renderers
don't support them). Per-chip resources and command line options:
`<CHIP>CRTBeamSim`, `<CHIP>CRTBeamPersistence` (0-100 %, default 50),
`<CHIP>CRTBeamDecay` (0-100 %, default 25). Pacing, doubling and CRT
simulation work only with **VSync on**.

Notes: for VRR in a window, the VICE window has to be focused (Windows
composes unfocused windows at the fixed refresh rate). The `wmm` sound
driver delivers frames more evenly than `dx`, so it needs less latency.

---

## 2. x64sc: Ultimate 64 style CPU turbo

Files: `src/mainc64cpu.c/.h`, `src/c64/c64cpusc.c`,
`src/arch/gtk3/uisettings.c`, `src/arch/gtk3/widgets/settings_cputurbo.c/.h`,
`src/arch/gtk3/widgets/Makefile.am`

**Why:** to run software with only the CPU faster, as the Ultimate 64 does,
while staying as compatible as possible. Warp mode speeds up the whole
machine, so video, timers and sound speed up with it. This turbo speeds up
only the 6510.

**What:**

- CPU speeds of 1-48 MHz, using the U64 speed table
  (1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 20, 24, 32, 40, 48 MHz).
- Implemented as an integer divisor in the CPU's cycle counter
  (`CLK_INC` in `c64cpusc.c`): `maincpu_clk` still counts 1 MHz system
  cycles, so VIC-II, CIA, SID and drives keep their timing. IRQ/NMI delays
  are counted in CPU cycles.
- **I/O stays at 1 MHz:** an access to `$D000-$D7FF` or `$DC00-$DFFF` (when
  mapped in) waits for the next system cycle with BA high and takes a whole
  system cycle, as on the U64. Colour RAM and cartridge ROM run at full speed.
- **Registers** (handled in the CPU's I/O path rather than in `viciisc`,
  because `viciisc` is shared with xscpu64):
  - `$D031` speed index (bits 0-3), bit 7 = badlines off
  - `$D030` bit 0 = turbo on (enable-bit mode; reads back `$FE | on`)
  - `$D07A` / `$D07B` = normal speed / turbo (SuperCPU compatible)
- **Badlines off** option: the CPU keeps running while BA is low (about 7 %
  faster).
- **IEC slowdown** (a VICE-only heuristic, on by default): any `$DD00/$DD01`
  access drops the CPU to 1 MHz for 1000 cycles. Without it, KERNAL and
  JiffyDOS loads hang at high speeds, because their timing loops are
  CPU-timed.
- Snapshot module `C64TURBO` (optional when reading, so older snapshots
  still load).

**Settings:** Settings > Machine > CPU turbo (x64sc, GTK3 only; no SDL menu).
Resources and options:

| Resource           | Option                                   | Meaning                                            |
|--------------------|------------------------------------------|----------------------------------------------------|
| `TurboMode`        | `-turbomode <0-3>`                       | 0 off, 1 manual, 2 U64 registers (`$D031`), 3 enable bit (`$D030`) |
| `TurboSpeed`       | `-turbospeed <MHz>`                      | speed in manual mode (default 4)                   |
| `TurboBadlines`    | `-turbobadlines` / `+turbobadlines`      | keep badline timing (default on)                   |
| `TurboIECSlowdown` | `-turboiecslowdown` / `+turboiecslowdown`| 1 MHz after IEC access (default on)                |

Measured (BASIC loop, iterations per time unit): 1 MHz 110, 4 MHz 447,
20 MHz ~2300, 48 MHz 5526 (about 50x; IRQ overhead shrinks at higher speeds).
KERNAL and JiffyDOS loads work at 48 MHz with the IEC slowdown on.
Not tested yet: other fastloaders and tape turbos. The read-back values of
the upper bits of `$D030` and of bits 4-6 of `$D031` are guesses.

---

## 3. C64: Ultimate Command Interface (UCI) emulation, for netsid

Files: `src/c64/cart/ultimate_uci.c/.h`, `src/c64/cart/c64carthooks.c`,
`src/c64/cart/Makefile.am`, `src/cartridge.h`, `src/sound.c`

**Why:** to run netsid, and other software that uses the 1541 Ultimate-II /
Ultimate 64 command interface, in VICE.

**What:**

- An I/O-only device at `$DF1C-$DF1F` (like the RTC or Digimax cartridges),
  modelled on Ultimate firmware v3.15a:
  - DOS target with an in-memory file system
  - network target with real TCP sockets on the host
  - control target (`GET_HWINFO`, `LOAD_CONFIG`)
- Resources: `UltimateUCI`, `UltimateUCILoadConfig`, `UltimateUCITempFS`,
  `UltimateUCIMAC` (default `02:55:43:49:00:01`), `UltimateUCIProduct`
  (default `Ultimate 64 Elite`). Options: `-uci` / `+uci`, `-ucimac`,
  `-uciproduct`, ...
- `sound.c`: in warp mode, samples keep going to a file sound device (wav)
  and to the recording device, so a netsid capture made in warp mode is
  complete. Normally VICE drops sound in warp.

---

## 4. reSIDfp: no `std::call_once`

Files: `src/lib/libresidfp/src/FilterModelConfig6581.cpp`,
`FilterModelConfig8580.cpp`

**Why:** on MinGW, `std::call_once` calls through a null pointer in
profile-instrumented (PGO) builds, so the PGO+LTO build crashed on startup.

**What:** `getInstance()` uses a function-local static instead. C++11
guarantees that it is initialised once and thread safely, so behaviour is
unchanged.

With LTO+PGO, warp speed was 13-24 % higher than a plain `-O3` build
(Karateka +13 %, a SID album +18 %, the 48 MHz turbo +24 %).

---

## 5. CMake / Visual Studio (MSVC + vcpkg) build

Files: `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `build/msvc/`,
`.gitignore`, `src/arch/headless/videoarch.h`

**Why:** so VICE can be built, debugged and edited in Visual Studio. The
autotools build stays the main build.

**What:** a CMake build for x64, x64sc, x128 and xscpu64 with the SDL2 or
headless UI. vcpkg provides the dependencies, and every download is pinned
by hash. `src/arch/headless/videoarch.h` no longer has an empty struct
(that's a GCC extension that MSVC rejects). See `vice/build/msvc/README.md`
for requirements, presets and options.
