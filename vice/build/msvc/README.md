# Building VICE with Visual Studio (MSVC)

This directory holds the support files for building VICE with the Microsoft
toolchain via CMake. The autotools build (`autogen.sh` / `configure`) stays
the canonical build; this one exists so VICE can be built, debugged and
edited in Visual Studio.

Emulators built by default: **x64, x64sc, x128, xscpu64**.
UI: **SDL2** (the classic SDL UI, menu on **F12**), or headless.

## Requirements

- Visual Studio 2026 with the *Desktop development with C++* workload
  (includes CMake, Ninja and vcpkg). Visual Studio 2022 should work with the
  Ninja presets, but has not been tested.
- Git for Windows (its `bash`, `sed`, `awk` and `sort` run two small header
  generators at configure time).
- Internet access on first configure: vcpkg fetches SDL2, SDL2_image, zlib,
  libpng and pthreads. CMake's FetchContent fetches WinFlexBison
  (bison/flex for the monitor's parser). Every download is pinned by hash.

## Building

From a *Developer PowerShell for VS* (so that `VCPKG_ROOT` is set) in the
`vice` directory:

```powershell
cmake --preset vs2026                      # generates out/build/vs2026/vice.slnx
cmake --build --preset vs2026-release      # or open the .slnx in Visual Studio
```

Or open the `vice` folder in Visual Studio (*File > Open > Folder*). It picks
up `CMakePresets.json`; choose `ninja-debug`, `ninja-release` or `vs2026`.

Executables end up in `out/build/<preset>/[<config>/]`, next to the DLLs
vcpkg copies there.

### Options

| CMake variable    | Default                     | Meaning                                    |
|-------------------|-----------------------------|--------------------------------------------|
| `VICE_UI`         | `sdl2`                      | `sdl2` (classic SDL UI) or `headless` |
| `VICE_EMULATORS`  | `x64;x64sc;x128;xscpu64`    | Which emulators to build (names as in `src/Makefile.am`) |

For example: `cmake --preset vs2026 -DVICE_UI=headless`.

## Running

The emulators need the ROMs and keymaps from `data/`:

```powershell
out\build\vs2026\RelWithDebInfo\x64sc.exe -directory E:\path\to\vice\data
```

With the `vs2026` preset (the generated solution), debugging an emulator
passes `-directory <repo>/data` automatically. With *Open Folder* and the Ninja
presets, add that argument yourself (*Debug > Debug and Launch Settings*).

## How it works

- `ViceAutomake.cmake` reads the library and emulator source lists straight
  from the `Makefile.am` files (including `if`/`else`/`endif` conditionals),
  so new source files added there are picked up automatically.
- `config.h.in` is the hand-written MSVC replacement for the
  configure-generated `config.h`. It is written to the build directory,
  never into `src/`.
- `include/msvc_compat.h` (included from `config.h`) provides the few
  POSIX/GCC shims MSVC lacks: `strcasecmp`, `ssize_t`, `S_IS*`, an empty
  `__attribute__`, and early inclusion of `<windows.h>`. `include/unistd.h`
  is a minimal stand-in.
- Generated at configure/build time into `out/build/<preset>/generated`:
  `version.h`, `debug.h`, reSID's `siddefs.h` and waveform tables,
  `infocontrib.h`, `keysymtable.h`, and the monitor's `mon_parse.c/h` and
  `mon_lex.c`.

Known limitation: the overlay is not DPI-scaled yet, so it is small on 4K
displays in fullscreen.

## Not (yet) supported in the MSVC build

reSIDfp, ethernet (pcap), MIDI, hardware SID devices, OpenCBM, libcurl,
MP3/FLAC/Vorbis output, DirectSound output (sound goes through SDL),
and the GTK3 UI.
