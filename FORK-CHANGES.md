# vice-gsync: changes to VICE in this fork

This fork (`grue74/vice-gsync`) is upstream VICE (`VICE-Team/svn-mirror`,
`main`) plus the changes below. Each section says **what** was changed and
**why**. `main` is kept as upstream `main` with these branches merged in (later
work, such as the x64 CPU turbo, is committed on `main` directly):

| Branch              | Contents                                                        |
|---------------------|-----------------------------------------------------------------|
| `gtk3-smooth-video` | DirectX smooth video, CPU turbo (x64sc), reSIDfp fix, CMake/MSVC build |
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

## 2. x64 and x64sc: Ultimate 64 style CPU turbo

Files: `src/c64/c64turbo.c/.h` (shared: settings, registers, IEC slowdown,
snapshot), `src/c64/c64cpusc.c` + `src/mainc64cpu.c` (x64sc CPU core),
`src/c64/c64cpu.c` (x64 CPU core), `src/dma.c/.h`, `src/c64/c64.c`,
`src/c64/c64-snapshot.c`, `src/c64/Makefile.am`,
`src/arch/gtk3/uisettings.c`, `src/arch/gtk3/widgets/settings_cputurbo.c/.h`,
`src/arch/gtk3/widgets/Makefile.am`

**Why:** to run software with only the CPU faster, as the Ultimate 64 does,
while staying as compatible as possible. Warp mode speeds up the whole
machine, so video, timers and sound speed up with it. This turbo speeds up
only the 6510. It is in both C64 emulators: x64sc for accuracy, x64 for
testing that doesn't need cycle-exact timing (x64 is about 30 % faster at
1 MHz; at high turbo speeds the two are about as fast, because most of the
time then goes into the CPU).

**What:**

- CPU speeds of 1-48 MHz, using the U64 speed table
  (1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 20, 24, 32, 40, 48 MHz).
- `maincpu_clk` still counts 1 MHz system cycles, so VIC-II, CIA, SID and
  drives keep their timing. Only the CPU's cycles are divided:
  - **x64sc:** an integer divisor in the per-cycle `CLK_INC`
    (`c64cpusc.c`). IRQ/NMI delays are counted in CPU cycles. Cycle exact.
  - **x64:** the x64 core adds an instruction's cycles in steps (`CLK_ADD`),
    so `c64cpu.c` accumulates them as CPU cycles and advances the system
    clock once per `TurboSpeed` of them (the same idea as the C128's 2 MHz
    mode). `REWIND_FETCH_OPCODE` (ROM traps, JAM) goes through the same
    accumulator. Not cycle exact: interrupts can be taken a couple of system
    cycles late.
- **I/O stays at 1 MHz:** an access to `$D000-$D7FF` or `$DC00-$DFFF` (when
  mapped in) waits for the next system cycle (x64sc: with BA high) and takes
  a whole system cycle, as on the U64. Colour RAM and cartridge ROM run at
  full speed.
- **Registers** (handled in the CPU's I/O path rather than in the VIC-II
  code, which is shared with xscpu64 and x128):
  - `$D031` speed index (bits 0-3), bit 7 = badlines off
  - `$D030` bit 0 = turbo on (enable-bit mode; reads back `$FE | on`)
  - `$D07A` / `$D07B` = normal speed / turbo (SuperCPU compatible)
- **Badlines off** option: the CPU keeps running while the VIC-II fetches
  (about 7 % faster). x64sc ignores BA; x64 skips the VIC-II's cycle
  stealing (`dma_maincpu_steal_disabled` in `dma.c`).
- **IEC slowdown** (a VICE-only heuristic, on by default): any `$DD00/$DD01`
  access drops the CPU to 1 MHz for 1000 cycles. Without it, KERNAL and
  JiffyDOS loads hang at high speeds, because their timing loops are
  CPU-timed.
- Snapshot module `C64TURBO` (optional when reading, so older snapshots
  still load).

**Settings:** Settings > Machine > CPU turbo (x64 and x64sc, GTK3 only; no
SDL menu). Resources and options:

| Resource           | Option                                   | Meaning                                            |
|--------------------|------------------------------------------|----------------------------------------------------|
| `TurboMode`        | `-turbomode <0-3>`                       | 0 off, 1 manual, 2 U64 registers (`$D031`), 3 enable bit (`$D030`) |
| `TurboSpeed`       | `-turbospeed <MHz>`                      | speed in manual mode (default 4)                   |
| `TurboBadlines`    | `-turbobadlines` / `+turbobadlines`      | keep badline timing (default on)                   |
| `TurboIECSlowdown` | `-turboiecslowdown` / `+turboiecslowdown`| 1 MHz after IEC access (default on)                |

Note: x64 isn't built by default; configure with `--enable-x64`.

Measured with a BASIC loop that counts iterations for 60 jiffies (the
results vary by about 1 % from run to run):

| Speed               | x64sc  | x64   |
|---------------------|--------|-------|
| off                 | 110-111 | 110-111 |
| 4 MHz               | 443    | 448   |
| 20 MHz (registers)  | 2304-2341 | 2319 |
| 48 MHz              | 5508-5594 | 5577 |
| 48 MHz, badlines off| 5886   | 5893  |

KERNAL (true drive), JiffyDOS and virtual-device (trap) loads work at
48 MHz with the IEC slowdown on in both emulators. Without it they fail
(a hang or ?DEVICE NOT PRESENT).
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

---

## 6. reSIDfp: mismatched filter capacitors C1/C2

Files: `src/lib/libresidfp/src/Integrator.h`, `Integrator6581.cpp`,
`Integrator8580.cpp`, `Filter.h`, `Filter6581.h`, `Filter8580.h`, `SID.cpp/.h`,
`src/sid/residfp.cc/.h`, `src/sid/sid-resources.c`,
`src/sid/sid-cmdline-options.c`, `src/sid/sid.h`,
`src/arch/gtk3/widgets/sidsoundwidget.c`, `src/arch/gtk3/widgets/mixerwidget.c`,
`src/arch/gtk3/widgets/vsidmixerwidget.c`

**Why:** the SID filter integrates on two external capacitor pairs: C1
(CAP1A/B, lowpass integrator) and C2 (CAP2A/B, bandpass integrator). The
datasheet assumes a matched pair, but Commodore fitted 10 % ceramics (470 pF on
6581 boards, 22 nF on 8580 boards), so real boards differ. Both reSID and
reSIDfp used one value for both capacitors.

**What:**

- Each reSIDfp integrator has its own capacitor value: the charge it adds per
  cycle is scaled by 1/C (`Integrator::setCapacitor`, `charge()`).
  `SID::setFilterCapacitors(c1, c2)` sets both, for the 6581 and the 8580.
- In the linear model the cutoff goes with 1/sqrt(C1*C2) and Q with
  sqrt(C2/C1). With 10 % parts: up to about ±10 % cutoff (both caps off the
  same way) and ±10 % Q (opposite ways).
- **Live:** a change only bumps a counter; each chip picks the new values up
  before its next sound buffer. No SID re-init, so it can be changed while a
  tune plays.
- Resources `SidResidFilterC1`, `SidResidFilterC2`: deviation from nominal in
  0.1 % (-200..200 = ±20 %), for all reSIDfp chips. Options `-residfilterc1`,
  `-residfilterc2`.
- GTK3: Settings > Audio > SID, block "ReSIDfp filter capacitors C1/C2": a
  slider and an exact value per cap, a reset button, and presets **Random
  board** (two 10 % parts, normal distribution with the tolerance at 2 sigma),
  **Most resonance** (C1 -10 %, C2 +10 %), **Least resonance** and **Matched
  pair**.
- The mixer under the emulated screen (status bar) has C1 and C2 sliders too,
  shown when reSIDfp is the engine; its Reset button sets a matched pair.
- VSID's main window has a full SID panel in place of its three-slider mixer:
  engine and model, filter on/off, every filter slider of the current engine
  and model (reSID passband/gain/bias; reSIDfp curve, range, combined
  waveforms, old 2200 pF caps), C1 and C2 with the four presets, and "Reset
  sliders to defaults". It rebuilds itself when the engine or model changes.
- **6581 DAC kink:** the 6581 cutoff DAC is an R-2R ladder with a 2R/R ratio
  of 2.20 (not 2.00) and no termination, so the cutoff drops at the FC bit
  carries (255/256, 511/512, 1023/1024). Resource `SidResid6581DacRatio`
  (x1000, 1900..2600, default 2200; option `-resid6581dacratio`), applied
  live, with a "DAC kink (2R/R)" slider on the VSID panel, on the
  Settings > Audio > SID page (reSIDfp 6581 block) and in the status-bar
  mixer (reSIDfp, 6581 only; Reset sets 2.200). Measured at FC
  1016 -> 1024 (filter curve 0.25): 2.00 smooth (+1.7 %), 2.20 -12 %,
  2.40 -24 %. Library: `Dac::kinkedDac(..., ratio2R)`,
  `FilterModelConfig6581::setDacRatio`, `SID::setFilter6581DacRatio`; the
  SID filter parameters are now initialised (`new Params()`).
- **6581 snake resistor:** the long "snake" transistor beside each integrator's
  cutoff transistor (W/L 1/115, about 0.9 MOhm) carries the current when the
  cutoff transistor is nearly off, so it sets the floor of the cutoff curve.
  Resource `SidResid6581Snake` (W/L scale x1000, 250..4000, default 1000;
  option `-resid6581snake`), per chip, applied live; "Snake resistor" slider
  on the VSID panel. Measured (default curve): x0.5 / x1 / x2 / x4 give
  181 / 322 / 600 / 1113 Hz at FC 0 and 16.1 / 16.2 / 16.4 / 16.8 kHz at
  FC 1920. Library: `Integrator6581::setSnakeScale`,
  `SID::setFilter6581SnakeScale`.
- **Resonance strength** (both models): the resonance ladder gain (1/Q) is
  divided by it. Resource `SidResidResonanceStrength` (x1000, 250..4000,
  default 1000; option `-residresonancestrength`). Measured: 8580 Q scales
  exactly (x0.5 / x2: 2.58 -> 1.28 / 5.05 at resonance 15); 6581 Q x1.33 at
  resonance 10 for x2, no effect at resonance 15 (the 6581 ladder gain is
  zero there).
- **6581 op-amp gain**: the measured op-amp curve is made steeper or flatter
  around its working point (4.54 V; end points kept). The 6581's low op-amp
  gain makes the integrators lossy and limits Q (about 1.75 at resonance 15).
  Resource `SidResid6581OpampGain` (x1000, 500..2500, default 1000; option
  `-resid6581opampgain`). Measured at resonance 15: x1.5 / x2 give Q 2.24 /
  2.49 and a peak about 4 dB higher, but the cutoff also roughly doubles at
  x2; above about x2.5 the peak drops again.
- Both rebuild tables shared by all chips of a model (about 50 ms), so VICE
  applies them on the sound side once the value has been still for 0.2 s.
  Library: `FilterModelConfig6581/8580::setResonanceStrength`,
  `FilterModelConfig6581::setOpampGain` (with `buildOpampRev` split out of
  the base constructor), `SID::setFilterResonanceStrength`,
  `SID::setFilter6581OpampGain`. Sliders for the snake, resonance strength
  and op-amp gain on the VSID panel, on Settings > Audio > SID (6581 block:
  snake, op-amp gain; both models: resonance strength) and in the status-bar
  mixer (reSIDfp only; the 6581 ones only with a 6581).
- **6581 resonance Q**: scales Q at every resonance value, 15 and 0
  included, without moving the cutoff. The Vhp->Vbp integrator runs 1/q as
  fast and the Vbp->Vlp one q times as fast; the cutoff follows the product
  of the two rates, Q their ratio (in the linear model Q ~ sqrt(C2/C1)/k).
  This is what a C2 > C1 cap mismatch does, but as a property of the chip
  model, so fits to a real chip don't have to push the board caps to
  unrealistic values. Resonance strength can't do it on the 6581: its ladder
  gain is zero at resonance 15, and it also raises the bandpass peak.
  Resource `SidResid6581ResonanceQ` (x1000, 500..2000, default 1000; option
  `-resid6581resonanceq`), per chip and live like the caps. Library:
  `Filter6581::setResonanceQ`, `SID::setFilter6581ResonanceQ` (the
  integrator capacitor clamp is widened to x0.25..x4 so caps and Q combine).
  Sliders on the VSID panel, Settings > Audio > SID and the status-bar mixer.
  Checked against three recordings of a real 6581R4 (one tune at resonance
  15, one at 0): matched caps with Q x1.106 score the same as the fitted
  C1 -10 % / C2 +10 % (3.627 vs 3.628, every tune within 0.001), and a refit
  with the caps held at 0 % found nothing better.
- **6581 wave offset**: the zero level of the 6581 waveform DAC, as a raw
  12 bit DAC input (reSIDfp default 0x380 = 896). It sets the DC level of
  the voices going into the filter and mixer, so it moves the filter
  working point (its distortion) and the level of volume register writes
  (digis, clicks). reSIDfp had `setOffset6581(0..1)` (0x380..0x580, "TODO
  determine a reasonable range") but VICE never called it. Resource
  `SidResid6581WaveOffset` (384..1792, default 896; option
  `-resid6581waveoffset`), per chip and live; `SID::setOffset6581Value`
  rebuilds the waveform DAC table only when the value changes. Sliders on
  the VSID panel, Settings > Audio > SID and the status-bar mixer; saved
  in own filter settings.
- **6581 cutoff curve shape, with a graph**: nine knots that move the
  cutoff DAC voltage at FC 0, 256, ..., 1792, 2047 (linear in between, in
  mV; positive raises the cutoff there), on top of the model's curve, so
  a chip whose curve has a different shape (not just a shifted or
  stretched one) can be matched. Resources `SidResid6581CurveK0..K8`
  (-1000..1000, default 0; options `-resid6581curvek0..k8`), per chip and
  live, saved in own filter settings. Library: `Filter6581::setCurveShape`
  adds the offsets inside `FilterModelConfig6581::getDAC` (clamped below
  Vddt), so DAC ratio and curve changes keep them. The graph (VSID panel and
  Settings > Audio > SID, 6581 only) plots cutoff against FC for the
  playing chip, with and without the shape, and the knots can be dragged
  (double click = 0, "Flat" = all 0). The cutoff comes from
  `Filter6581::estimateCutoffs`: copies of the integrators get a full-level
  step at resonance 15 and the first half period of the bandpass ringing
  is timed. That matches a full-level noise measurement of the cutoff within
  about 15 %; low in the curve the 6581 cutoff also depends on the signal
  level (the ringing slows down as it decays). It is computed on the sound
  thread, 8 points per buffer, so playback is never held up for long.
  The graph also shows the reSIDfp default curve as a fixed grey line,
  and samples both sides of every cutoff DAC carry (FC 255/256, 511/512,
  ...) so the DAC kink steps show.
  The mixer under the screen doesn't get the graph (no room).
- **Slider guide**: [reSIDfp-6581-filter-guide.html](reSIDfp-6581-filter-guide.html)
  shows, with measured curves, which 6581 slider moves which part of the
  cutoff curve (snake = floor, range = everything, curve = where it opens,
  op-amp = the middle, DAC kink = the steps, knots = one spot) and why,
  which sliders leave the curve alone, and a tuning order.
- **Own filter settings** ("My settings" on the VSID panel, "My filter
  settings" on Settings > Audio > SID): save every filter value (reSID and
  reSIDfp sliders, C1/C2, DAC ratio, snake, resonance strength, op-amp gain,
  resonance Q, wave offset, curve shape, combined waveforms, old caps, filter on/off; not the engine or
  model) under a name, load it by choosing it from the list, delete it. A
  value missing from a saved setting (saved before that slider existed) is
  set to its default on loading, so a setting always sounds the same. Both places share
  one list, stored in `sidfilter-presets.txt` in the VICE configuration
  directory, one line per preset (`widgets/sidfilterpresets.c`).

**Measured** (voice 3 white noise through the lowpass at resonance 15,
recorded to wav, 2-pole fit of the spectrum; repeat runs agree within 0.2 %):

| Setting                | 8580 f0 | 8580 Q | 6581 f0 loud | 6581 Q loud | 6581 f0 quiet | 6581 Q quiet |
|------------------------|---------|--------|--------------|-------------|---------------|--------------|
| matched (nominal)      | 3817 Hz | 2.36   | 2026 Hz      | 1.99        | 1268 Hz       | 2.17         |
| both +10 %             | x0.905  | x1.02  | x0.924       | x1.05       | x0.919        | x1.09        |
| C1 -10 %, C2 +10 %     | x1.012  | x1.11  | x0.984       | x1.05       | x1.008        | x1.02        |
| C1 +10 %, C2 -10 %     | x0.997  | x0.90  | x1.025       | x0.98       | x1.004        | x0.96        |

(6581: FC 450; "loud" is sustain 15, "quiet" sustain 3.)

The 8580 follows the linear model (cutoff x0.909, Q x1.105 / x0.905). On the
6581 the cutoff follows C1*C2 too, but the resonance doesn't separate cleanly:
the 6581's response shape depends on the cutoff frequency and the signal level
(the 2-pole fit is only an approximation for it), so even matched caps change
the fitted Q, and the mismatch's own effect on Q is smaller, about ±2-4 %
instead of ±10 %. A change while playing gave the same result as a fresh start,
with no gap or click in the output.

The settings apply to every reSIDfp chip (multi-SID setups share them); reSID
is unchanged.

---

## 7. VSID: export tunes to MP3, FLAC, Ogg Vorbis or WAV

Files: `src/arch/gtk3/vsidexport.c/.h` (new), `src/arch/gtk3/Makefile.am`,
`src/arch/gtk3/widgets/vsidmixerwidget.c/.h`,
`src/arch/gtk3/widgets/vsidmainwidget.c`, `src/sound.c/.h`

**Why:** to save tunes as audio files from VSID, with the fork's SID and
filter settings. VSID's sound recording menu entries were disabled
(`#if 0` in `uivsidmenu.c`), and the build had no MP3 encoder.

**What:**

- "Export tune..." button on the VSID SID panel opens a dialog: file name
  (the format follows the extension), format (MP3 128 kbit/s, FLAC, Ogg
  Vorbis, WAV), current subtune or all of them (`name-01.mp3`, ...), length
  from the HVSC song length database or a fixed time, fade-out, and fast
  export (warp: silent, much faster than real time). A progress bar and a
  Cancel button; closing the dialog lets the export finish.
- `sound.c`: a recording limit, `sound_record_limit(seconds, fade)`: the
  recording stops after exactly that many samples, and the last part is
  faded out (on a copy, so only the file fades). The export restarts each
  subtune, records it through the normal recording device
  (`SoundRecordDeviceName/Arg`), and moves on when the limit is reached.
- The panel gets the export function through
  `vsid_mixer_widget_set_export_handler()`, set by `vsidmainwidget.c`,
  because the panel's code is also linked into the other emulators, which
  have no PSID player.
- **Build:** MP3 needs LAME. On MSYS2: `pacman -S mingw-w64-x86_64-lame`,
  then configure with `--with-lame --with-static-lame --with-flac
  --with-vorbis` (static LAME: no extra DLL in the release).
