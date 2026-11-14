# ultralib

ultralib is a C++20 library for the Nintendo 64: an emulator core that boots a game from its cartridge image and runs
it, the readers and writers of the console's sound files, models of the games' sound engines and of the audio
microcodes, and the conversion of the games' music to MIDI, SoundFont 2 and SFZ files.

The emulator core builds on xsflib, a library for players and rippers of the PSF family, and needs nothing else. The
sound modules build on ntdlib, a library for the formats and hardware of Nintendo's consoles, and through it on xsflib
and seqlib. ultralib's modules are static libraries. A project links the modules it uses.

| Module | What it holds |
|---|---|
| `ultralib::machine` | The emulator core: the VR4300 CPU with COP0, the TLB and the FPU, the RSP with its vector unit and DMA, the interfaces of the RCP, the Aleck64 board, the PIF RAM and its joybus devices, boot, coverage, and Project64 1.4 save states. `ultralib::machine::System` (`ultralib/machine/system.h`) is the API |
| `ultralib::rom_set` | The ROM sets of the Aleck64 games: the chips of each game, and the reading of a set, as a ZIP archive or a folder of its chips, into the image of a cartridge. A set that contains the chips of more than one game gives the image of the first and names the others. It links `xsflib::rom_set` alone, and builds only where xsflib builds that, with zlib |
| `ultralib::n64` | The Nintendo 64's sound files: the readers and writers of libultra's sequence banks (S1) and bank files (B1), of standard and compressed MIDI, of VADPCM samples, and of the banks of Nintendo EAD's sound engines, with the reader of those engines' sequences. A file comes back byte for byte. Compressed MIDI files can be built from events, with new blocks. VADPCM frames come from samples by the rule of the SDK's encoder. Frame-by-frame models of the console's sequence players and audio microcodes run them, with snapshots, mutes, meters and an enhanced mixing mode. `ultralib/n64/song_names.h` contains the names of some games' songs. The header requires only the standard library. A project may include it without the module |
| `ultralib::sequence_export_n64` | Conversion of the Nintendo 64's sound data to MIDI, SoundFont 2 and SFZ files, from a cartridge image or an Aleck64 game's ROM set. It links `ntdlib::sequence_export`, `ultralib::n64` and `ultralib::rom_set`. It builds only where xsflib builds `xsflib::rom_set`, with zlib |

`ultralib::testing` holds the builders of small MIPS programs for the core's unit tests. It links `xsflib::testing`.

## The emulator core

`ultralib::machine` emulates what the sound of a game uses:

- the VR4300 CPU: MIPS III with 64-bit registers, COP0 with the TLB and exceptions, and the FPU with its four rounding
  modes, its exception flags and the FR bit
- the RSP: the scalar unit, the vector unit with its accumulators, flags and divider, and its DMA engine
- the interfaces of the RCP: MI, SP, DP, VI, AI, PI, SI, RI and the registers of the RDRAM chips. The RDP doesn't
  draw. The DP walks its command lists and raises its interrupt at each full sync. The VI is only a clock.
- the Aleck64 board: 8 MiB of RDRAM, the CIC-NUS-5101, the DIP switches and controls, the coin, service, test and
  start inputs, and a mahjong panel. The video chip of the E90 main board takes writes and reads 0.
- the PIF RAM and its joybus commands. By default no device answers, as in older emulators of the console. A program may
  connect a standard controller to each port, with scripted input, and an EEPROM of 4 or 16 Kbit to the cartridge.
- boot: the state that the boot code of the PIF leaves for the IPL3 of the cartridge. The IPL3 then runs as on the
  hardware.
- coverage: every ROM word that the CPU or the PI reads, and every RDRAM word read before it's written
- Project64 1.4 save states, read and written, and a check that reports when a moment can't be saved without losing
  something in flight

Time runs at the hardware rates by default. An option switches to the Count model of older emulators.

A program loads a ROM with `LoadRom`, boots it with `Boot`, runs it with `Run` or `RunSeconds`, stops it at
breakpoints, reads and writes memory and registers, patches the ROM, turns coverage on with `SetCoverage`, and saves a
snapshot with `SaveState` once `CheckQuiet` reports a quiet moment. `GetPif` connects controllers and an EEPROM, and
accepts a script of controller input by video field or by time. `SetPifRom` sets the dump of the PIF ROM.
`SetAudioCallback` receives each buffer that the audio interface starts. `SetRspTaskHook` is called around each audio
task that runs at once in the legacy timing.

## Building

ultralib requires CMake 3.20 or later, a C++20 compiler and ntdlib. ntdlib is a Git submodule at `externals/ntdlib`.
Clone with `git clone --recurse-submodules`, or run `git submodule update --init --recursive` in an existing clone.
The ntdlib submodule holds xsflib as a submodule, and xsflib holds seqlib. A build of the core alone uses only xsflib.

ultralib supports macOS on Apple Silicon and Windows x64 built with clang-cl. Other platforms and compilers aren't
supported.

- **Windows x64**: Visual Studio 2022 with the C++ workload and its "C++ Clang tools for Windows" component. Configure
  with `cmake -S . -B build -A x64 -T ClangCL` in place of the first command below.
- **macOS on Apple Silicon**: Xcode's command-line tools (`xcode-select --install`) and CMake (from cmake.org or
  Homebrew).

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Output is meant to be the same on both platforms. ultralib builds with xsflib's flags. They turn off the contraction of
floating-point operations into fused multiply-adds. The core's FPU (`src/machine/cpu_fpu.cpp`) builds with strict
floating point, so that each operation stays between the calls that set the rounding mode and read the exception
flags. A project that includes ultralib's headers should build its sources the same way.

## Options

| Option | Default | Effect |
|---|---|---|
| `ULTRALIB_SOUND` | on when built alone | The `ultralib::n64` module, the sound files and the models. Requires ntdlib and seqlib. |
| `ULTRALIB_SEQUENCE_EXPORT` | on when built alone | The `ultralib::sequence_export_n64` module, the conversion to MIDI, SoundFont 2 and SFZ. Requires `ULTRALIB_SOUND`, and xsflib built with zlib. |
| `ULTRALIB_TESTS` | on when built alone | The tests, registered with CTest. |
| `ULTRALIB_TEST_ROMS` | empty | A folder of test ROMs for the core's tests (see "Tests"). |
| `ULTRALIB_INSTALL` | off | Installs the headers, the libraries and the CMake package. A build of ultralib alone installs its copies of xsflib and, with `ULTRALIB_SOUND`, ntdlib too. Requires an installed seqlib. |

## Tests

`ctest` runs the unit tests of the core, the sound files, the models and the conversion. They use no game data.

The tests of the CPU, the FPU, the RSP and the devices also run two suites of test ROMs, which aren't in the
repository: [n64-systemtest](https://github.com/lemmy-64/n64-systemtest) (MIT) and
[PeterLemon's N64 tests](https://github.com/PeterLemon/N64) (Unlicense). Put them in a folder: the ROM of
n64-systemtest as `n64-systemtest.z64`, and the `CPUTest` and `RSPTest` folders of the PeterLemon repository under
`PeterLemon/`, with their ROMs and fonts. Then configure with `-DULTRALIB_TEST_ROMS=<folder>`, and CTest runs each ROM.

n64-systemtest passes when none of its tests fails outside three areas that the core doesn't cover. The RDP doesn't
draw. The core has no caches. It doesn't reverse the byte order in user mode when the RE bit of the Status register
is set. Games run in kernel mode, so none of them depends on that.

A PeterLemon test passes when its frame shows PASS and no FAIL. Some of the tests draw values but no verdict. Their
frames are compared with the screenshots in the repository of the tests. `tests/test_roms.cmake` lists the tests that
are left out, and why.

With `ULTRALIB_FRAME=<file>` set, the runner writes the frame of a PeterLemon test to that file as a PPM image. With
`ULTRALIB_TRACE` set, it prints the first 64 exceptions other than interrupts that the CPU takes, and where it found
each verdict.

## Using ultralib

Each module is a static library with an alias, such as `ultralib::machine`. A project links the modules it uses.

A project that uses ultralib adds xsflib first, and ntdlib too for the sound modules. ultralib then uses those copies.
The copies in ultralib's submodule go unused.

As Git submodules, for the core alone:

```cmake
add_subdirectory(externals/xsflib EXCLUDE_FROM_ALL)
set(ULTRALIB_SOUND OFF)
add_subdirectory(externals/ultralib EXCLUDE_FROM_ALL)
target_link_libraries(my_program PRIVATE ultralib::machine)
```

For the conversion:

```cmake
set(NTDLIB_SEQUENCE_EXPORT ON)
set(ULTRALIB_SEQUENCE_EXPORT ON)
add_subdirectory(externals/xsflib EXCLUDE_FROM_ALL)
add_subdirectory(externals/ntdlib EXCLUDE_FROM_ALL)
add_subdirectory(externals/ultralib EXCLUDE_FROM_ALL)
target_link_libraries(my_program PRIVATE ultralib::sequence_export_n64)
```

From installed copies (`cmake --install build` after configuring each library with its `_INSTALL` option), with
xsflib and ntdlib installed first:

```cmake
find_package(ultralib 1.0 REQUIRED)
target_link_libraries(my_program PRIVATE ultralib::machine)
```

ultralib's package finds xsflib, and ntdlib and seqlib with `ULTRALIB_SOUND`.
`tests/package` is a project that uses ultralib in each of these ways, to check the package.

## Licence

ultralib is under the MIT licence (`LICENSE`). The vector unit of the RSP (`src/machine/rsp_vector.cpp` and
`include/ultralib/machine/rsp_vector.h`) is adapted from
[Iconoclast's RSP](https://github.com/mupen64plus/mupen64plus-rsp-cxd4) (mupen64plus-rsp-cxd4), which Iconoclast
dedicated to the public domain under CC0 1.0. `THIRD-PARTY-NOTICES.md` lists the libraries and code that ultralib uses.
ultralib links xsflib, and ntdlib and seqlib for the sound modules. They're under the MIT licence too. ntdlib's
`THIRD-PARTY-NOTICES.md` lists the third-party code in ntdlib's modules that ultralib links.
