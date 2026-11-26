# VICE GitHub Mirror

## About this fork (vice-gsync)

This fork is upstream VICE with three kinds of changes:

- **Video stability:** smooth, evenly paced output, especially on VRR
  (G-Sync / FreeSync) displays.
- **Audio tweaks:** a more tunable reSIDfp SID filter, closer to real chips
  and boards.
- **Missing features:** things real hardware or other tools have and VICE
  lacked.

Details, reasons and settings for each change are in
[FORK-CHANGES.md](FORK-CHANGES.md). Windows builds are on the
[releases page](https://github.com/grue74/vice-gsync/releases).

### Build on macOS (GTK3)

Install the Xcode command line tools (`xcode-select --install`) and
[Homebrew](https://brew.sh/), then run from the clone's root:

```sh
sh vice/build/macOS/install-deps.sh
export PATH="$(brew --prefix)/bin:$PATH"
cd vice
./autogen.sh
./configure --enable-gtk3ui
make -j "$(sysctl -n hw.ncpu)"
```

The emulators are in `vice/src/` (`x64sc`, `x128`, `vsid`, etc.).
`vice/src/x64sc --console --version` prints the version without initializing GTK.
Add `--enable-x64` to configure to build the older `x64` emulator too.
See [macOS build notes](vice/doc/building/macOS-Howto.txt) for installation,
SDL2 and optional dependencies.

### Work so far

Video:
- Frame pacing: frames shown on a steady clock, not in audio-driven bursts.
- Each frame shown twice on fast displays, so VRR stays away from its minimum refresh rate.
- DirectX flip model swap chain, VRR in a window and in borderless fullscreen.
- CRT beam simulation (rolling scan), with persistence and phosphor afterglow (Windows).
- Swap chain retry instead of rebuilding the device every frame.
- Same pacing in the Linux/X11 OpenGL renderer (new, so far tested only under WSL).

Audio (reSIDfp):
- Separate filter capacitors C1/C2, like a real board with 10 % parts; presets.
- 6581 DAC kink (2R/R ratio) for the cutoff steps at bit carries.
- 6581 snake resistor, resonance strength, op-amp gain, resonance Q.
- 6581 waveform DAC zero level (wave offset).
- 6581 cutoff curve shape with nine knots and a live curve graph.
- Every filter setting can be changed live while a tune plays.
- Named filter settings: save every filter value under a name, load it later.
- Slider guide: [reSIDfp-6581-filter-guide.html](reSIDfp-6581-filter-guide.html).
- Fix: no `std::call_once`, which crashed MinGW PGO builds.

Features:
- Ultimate 64 style CPU turbo, 1-48 MHz, in x64sc and x64 (U64 registers).
- Ultimate Command Interface (UCI) emulation: DOS, TCP/UDP network, control targets (netsid runs).
- Sound is kept in warp mode when recording, so netsid captures are complete.
- VSID: a full SID panel with every filter slider.
- VSID: export tunes to MP3, FLAC, Ogg Vorbis or WAV, with song lengths and fade-out.
- Status bar mixer: C1/C2 and the other filter sliders.
- Larger settings dialog; pages that don't fit scroll.
- CMake / Visual Studio (MSVC + vcpkg) build.

---

This is the official git mirror of the [VICE subversion repo](https://sourceforge.net/p/vice-emu/code/HEAD/tree/).

For news, documentation, developer information, [visit the VICE website](https://vice-emu.sourceforge.io/).

## Download VICE
* [Official Releases](https://vice-emu.sourceforge.io/#download)
* [Snapshot builds of the latest code](https://github.com/VICE-Team/svn-mirror/releases)
