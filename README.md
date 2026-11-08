# usf

usf rips and plays Nintendo 64 music in USF, the PSF-family format for the Nintendo 64. A USF rip contains the parts
of a game that its music uses: the code and data that the game reads, and a save state from a moment just before a
track starts. A player runs that code on an emulated console, so the music plays as it does in the game.

usf has an emulator core that boots a game from its cartridge image and runs it, and these programs:

- `usfrip` rips the music of a game that it has a profile for into a set of USF files.
- `usfplay` renders USF files to WAV or FLAC files.
- `foo_input_usfplay` plays USF files in foobar2000, on Windows and macOS.
- `xmp-usfplay` plays USF files in XMPlay, on Windows.

The USF players that came before usf are built on older emulators. This README calls them the legacy players. The
sets that `usfrip` makes play in them too, and usf plays the sets that older rippers made.

## Supported games

`usfrip` has a profile for each of these games. It recognizes an image by its SHA-1. Other releases of these games
don't rip.

- Chameleon Twist (USA, revision 1)
- Chameleon Twist 2 (USA)
- Cruis'n World (USA)
- GoldenEye 007 (USA)
- Hey You, Pikachu! (USA)
- The Legend of Zelda: Majora's Mask (USA)
- The Legend of Zelda: Ocarina of Time (USA, revision 2)
- Mario Kart 64 (USA)
- Mario Tennis (USA)
- Paper Mario (USA)
- Pokémon Puzzle League (USA)
- Pokémon Snap (USA)
- Pokémon Stadium (USA, revision 2)
- Pokémon Stadium 2 (USA)
- Super Mario 64 (USA), for its music, and for its sound effects one at a time
- Wave Race 64 (USA, revision 1)
- Yoshi's Story (USA)

For the rest of the list, `usfrip` rips the music. The two Zelda games boot with the CIC-NUS-6105 chip, and ripping them
requires a dump of the console's PIF ROM (see "Ripping a game").

`usfrip` also rips the music of these games of Seta's Aleck64, an arcade board built from the Nintendo 64's chips:

- Eleven Beat
- Hanabi de Doon! - Don-chan Puzzle
- Hi Pai Paradise
- Hi Pai Paradise 2
- Kurukuru Fever
- Magical Tetris Challenge
- Mayjinsen 3
- Star Soldier: Vanishing Earth
- Super Real Mahjong VS
- Tower & Shaft
- Vivid Dolls

An Aleck64 game comes as the ROM set of its board, not as a cartridge image.

## Ripping a game

```
usfrip game.z64
```

`usfrip` writes the set to a folder next to the image, named after it with `_usf` added. The set is one `.usflib` with
what the tracks share, and one `.miniusf` for each track. `-o DIR` writes it to DIR instead. The image may be in any
byte order (`.z64`, `.v64` or `.n64`), and it may be in a ZIP archive, as the only file of the archive with one of
those extensions.

For an Aleck64 game, give its ROM set in place of the image: a ZIP archive of the set, or a folder of its chips.
`usfrip` finds each chip of the game by its SHA-1 and size, whatever the file is called, and puts the image together
from them. Other files in the set are ignored, and the board's PIF data isn't needed.

```
usfrip set.zip
```

The players have no Aleck64 board. Each Aleck64 profile stops the threads of the game that read the board's inputs or
drive its video chip, and the music runs without them. `usfrip` warns if a track still reads or writes the board's
addresses after the snapshot.

Some images have more than one set, such as Super Mario 64's music and its sound effects. `usfrip` then writes each
set to a separate folder inside that folder: `music` and `sound effects`. `--bgm` rips only the music.

Super Real Mahjong VS plays its music in mono or in stereo. A setting in the game's service menu picks which, and the
game plays mono until someone changes it. `usfrip` rips both, as the sets `mono` and `stereo`.

A profile (`src/rip/profile.h`) says where in the game code to save the state, which register or word of RDRAM selects
a track, which threads to stop, what to patch, and the list of tracks. Some games have no song-select call to stop at,
such as a set of sound effects. For those, `usfrip` writes a small driver into RDRAM (`src/rip/driver.h`). The driver
calls the game's functions with the number of each track.

`usfrip` boots the game and runs it to the snapshot point of the profile. It checks that nothing is in flight there
that a save state can't record. Each track then runs from that state, with the timing of the legacy players:

- The length comes from xsflib's length analysis of the output: two passes of the loop and a fade of 10 s, or the
  end of the sound.
- Coverage records every ROM word that the run reads. The run lasts for the length and fade of the track, half the
  length more for a track that loops, and a margin of 10 s.
- The set keeps only those ROM words. The library contains the ROM words that every track reads, and the whole save
  state: the registers, all of RDRAM and the RSP memory. Each mini contains the rest of the ROM words for its track,
  and the track number. RDRAM isn't trimmed. The legacy players time the hardware differently from the core, and a
  game running in them can read RDRAM that the core never read.

At the end, `usfrip` loads each mini with its library, as a player would. It checks that the mini renders the same
samples as the untrimmed state over its length and fade.

The files have the tags `_enablecompare=1` and `_enableFIFOfull=1`, the usual metadata and `usfby`. Two more tags are
for information only, and players ignore them: `usfrip` with the version of the ripper, and `usfdriver` with the name
of the sound driver of the game.

Options:

- `-o DIR`, `--output DIR`: write the set to DIR
- `--list`: list the tracks of each set
- `--only REGEX`: rip only the tracks whose names match
- `--bgm`: rip only the music, without the sets of sound effects
- `--no-length`: skip the length analysis
- `--no-trim`: keep the whole ROM and the whole state in the library
- `--no-check`: skip the check at the end
- `--margin S`: the seconds of play after the fade that the coverage still covers (default 10)
- `--pif FILE`: a dump of the PIF ROM
- `--jobs N`: the number of tracks that run at once (default: the number of CPU threads)
- `--game`, `--artist`, `--year`, `--copyright`, `--by`: the tags of those names

Games with the CIC-NUS-6105 boot chip read code that the boot ROM of the PIF leaves in the RSP memory. usf doesn't
include that code. Give a dump of the PIF ROM with `--pif FILE`.

## Playing USF files

A USF set is a `.usflib` with what the tracks share, and a `.miniusf` for each track. Keep them in one folder. A track
names its library in its tags, and the player loads the chain of libraries by itself. Open only the `.miniusf` files.

The player runs the game code on the emulated console, and the audio microcode of the game on the emulated RSP. It
uses the timing of the legacy players and follows the `_enablecompare` and `_enableFIFOfull` flags of the set. Output
is 16-bit stereo at the rate that the game sets on its audio interface. The rate differs from game to game, and the
player doesn't resample it.

`usfplay` renders each track to a WAV file next to it, for the length and fade in the tags of the track:

```
usfplay "0001 Title.miniusf"
```

`-o FILE` writes FILE instead. With a `.flac` extension, the file is FLAC. `--length T` and `--fade T` replace the
values of the tags, in seconds, `m:s` or `h:m:s`. `--default T` sets the length for a file with no `length` tag (2:30
if you don't set it, with a fade of 10 s). `--info` prints the tags and the files that each track loads, without
rendering. `usfplay` prints how fast each render ran.

For foobar2000 and XMPlay, see `plugins/foobar2000/README.md` and `plugins/xmplay/README.md`. The foobar2000 component
is named `foo_input_usfplay` because a third-party component already uses the name `foo_input_usf`.

The emulation is heavy, and a 32-bit build is slower than a 64-bit one. The plugins render ahead to cover short gaps.

### Downloads

The Releases page has `usfrip` and `usfplay` for 64-bit Windows, the foobar2000 component for 64-bit and 32-bit
foobar2000 on Windows, and the XMPlay plugin. On macOS, build usf from source (see "Building").

To install the XMPlay plugin, unzip `xmp-usfplay-X.Y.Z.zip` and put `xmp-usfplay.dll` in the XMPlay folder, or in a
folder inside it. Then restart XMPlay.

### Drag and drop

- **Windows:** drop files on `usfrip.exe` or `usfplay.exe`. A console window shows the results and stays open until
  you press Enter.
- **macOS:** the build also makes `usfplay.app` in the build folder. Drop files on it in the Finder.

As above, the results go next to the files.

## Building

You need Git, CMake 3.20 or later, and C and C++20 compilers. usf builds on xsflib, a library for players and rippers
of the PSF family, which the repository includes as a submodule in `externals/xsflib`. Clone with
`git clone --recursive`, or run `git submodule update --init --recursive` in a clone. zlib and libFLAC are downloaded
and built along with usf when they aren't installed.

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload and its "C++ Clang tools for
  Windows" component. The workload includes CMake. The component adds clang-cl. Configure with
  `cmake -S . -B build -A x64 -T ClangCL` instead of the configure command below. Run the commands in a Developer
  PowerShell or Developer Command Prompt.
- **macOS on Apple Silicon:** the Xcode command line tools (`xcode-select --install`) and CMake (`brew install cmake`).
  `brew install flac` saves building libFLAC.

Linux, Intel Macs, MSVC and 32-bit Windows aren't supported. The exceptions are the 32-bit foobar2000 component and the
XMPlay plugin. Build them with clang-cl, configured with `-A Win32 -T ClangCL`.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The build makes `usfrip`, `usfplay` and `n64run` in `build`, or in `build/Release` on Windows.

`CMakePresets.json` has `release` and `debug` presets for the same steps: `cmake --preset release`,
`cmake --build --preset release` and `ctest --preset release`. The presets require CMake 3.23 or later. They don't
select clang-cl. On Windows, use the commands above.

The foobar2000 component and the XMPlay plugin each require the SDK of their host. Their READMEs say how to build
them: `plugins/foobar2000/README.md` and `plugins/xmplay/README.md`.

## The emulator core

`src/n64` emulates what the sound of a game uses:

- the VR4300 CPU: MIPS III with 64-bit registers, COP0 with the TLB and exceptions, and the FPU with its four rounding
  modes, its exception flags and the FR bit
- the RSP: the scalar unit, the vector unit with its accumulators, flags and divider, and its DMA engine
- the interfaces of the RCP: MI, SP, DP, VI, AI, PI, SI, RI and the registers of the RDRAM chips. The RDP doesn't
  draw. The DP walks its command lists and raises its interrupt at each full sync. The VI is only a clock.
- the Aleck64 board: 8 MiB of RDRAM, the CIC-NUS-5101, the DIP switches and controls, the coin, service, test and
  start inputs, and a mahjong panel. The video chip of the E90 main board takes writes and reads 0. `n64run` runs an
  image of the chips, not the set.
- the PIF RAM and its joybus commands. By default no device answers, as in the legacy players. A ripper may connect a
  standard controller to each port, with scripted input, and an EEPROM of 4 or 16 Kbit to the cartridge.
- boot: the state that the boot code of the PIF leaves for the IPL3 of the cartridge. The IPL3 then runs as on the
  hardware.
- coverage: every ROM word that the CPU or the PI reads, and every RDRAM word read before it's written
- Project64 1.4 save states, read and written, and a check that reports when a moment can't be saved without losing
  something in flight

Time runs at the hardware rates by default. An option switches to the Count model of the legacy players.

`usf::n64::System` (`src/n64/system.h`) is the API. A ripper loads a ROM with `LoadRom`, boots it with `Boot`, runs it
with `Run` or `RunSeconds`, stops it at breakpoints, reads and writes memory and registers, patches the ROM, turns
coverage on with `SetCoverage`, and saves a snapshot with `SaveState` once `CheckQuiet` reports a quiet moment.
`GetPif` connects controllers and an EEPROM, and accepts a script of controller input by video field or by time.
`SetPifRom` sets the dump of the PIF ROM.

`n64run` boots a cartridge image, runs it, and writes what its audio interface plays to a WAV file:

```
build/n64run --seconds 10 --wav out.wav game.z64
```

`--legacy` uses the timing of the legacy players. `--pif FILE` reads the dump of the PIF ROM. `--8mb` sets the RDRAM
to 8 MiB. `--debug-port` prints what a program writes to the debug port at 0x13FF0000. `--controller`
connects a controller to the first port, and `--eeprom 4k` or `--eeprom 16k` an EEPROM to the cartridge.
`--input F:B[:X:Y]` holds the buttons B, in hex as the controller sends them, from video field F on, with the stick at
X and Y. `--board aleck64` or `--board aleck64-e90` runs an image on the Aleck64 board, with 8 MiB of RDRAM.
`--board-input F:S:Y[:R]` sets the board's inputs from video field F on: the words S at 0xC0800000 and Y at
0xC0800004, and the four rows R of a mahjong panel. `n64run` also plays a USF track with its libraries:

```
build/n64run --seconds 60 --wav out.wav "0001 Title.miniusf"
```

## Tests

`ctest` runs the unit tests of the core, the loader, the ripper and the player. They use no game data.
`tests/test_set.cpp` writes USF sets made from small programs. The programs send a known ramp to the audio interface
at a chosen rate, with a chosen load on the CPU and the RSP. `tests/front_end_test.cpp` runs the loader and the
playback framework on those sets. `usf_make_test_set` writes such a set to a folder. Its track `01 Heavy` puts more
load on the emulator than any real track measured. The test `usfplay_test_set` plays that set through `usfplay`.

The tests of the CPU, the FPU, the RSP and the devices also run two suites of test ROMs, which aren't in the
repository: [n64-systemtest](https://github.com/lemmy-64/n64-systemtest) (MIT) and
[PeterLemon's N64 tests](https://github.com/PeterLemon/N64) (Unlicense). Put them in a folder: the ROM of
n64-systemtest as `n64-systemtest.z64`, and the `CPUTest` and `RSPTest` folders of the PeterLemon repository under
`PeterLemon/`, with their ROMs and fonts. Then configure with `-DUSF_TEST_ROMS=<folder>`, and CTest runs each ROM.

n64-systemtest passes when none of its tests fails outside three areas that the core doesn't cover. The RDP doesn't
draw. The core has no caches. It doesn't reverse the byte order in user mode when the RE bit of the Status register
is set. Games run in kernel mode, so none of them depends on that.

A PeterLemon test passes when its frame shows PASS and no FAIL. Some of the tests draw values but no verdict. Their
frames are compared with the screenshots in the repository of the tests. `tests/test_roms.cmake` lists the tests that
are left out, and why.

With `USF_FRAME=<file>` set, the runner writes the frame of a PeterLemon test to that file as a PPM image. With
`USF_TRACE` set, it prints each exception that the CPU takes and where it found each verdict.

## Known gaps

- The core has no instruction or data caches, and the RDP doesn't draw.
- Reverse-endian user mode isn't emulated.
- By default no controller is connected, as in the legacy players. Some games play no sound without one. A ripper
  may connect one before its snapshot. A controller has no Controller Pak or Rumble Pak.
- A write to the EEPROM takes effect at once. The EEPROM never reports itself busy.
- The length analysis follows only the output of the game. A track whose profile gives no loop point ends after 180 s
  of silence, or at the limit of 600 s.
- A track plays as the legacy players play it, but not always sample for sample. Where a game sizes its audio buffers
  from AI_LEN, the core can make a different choice from them after a few seconds.
- A file keeps one sample rate. If a game changes its rate during a track, the player converts the later frames to
  the first rate by linear interpolation.
- A game that sends no audio plays as silence for the length of the track.
- Seeking back renders again from the start of the track. The render-ahead buffer covers seeks within the last 10
  minutes of audio.
- foobar2000 shows the sample rate of a track only during playback. Finding the rate requires a run of the game code.

## Layout

```
src/n64/             the emulator core
src/usf/             the USF loader and the player
src/rip/             the ripper, and the game profiles in src/rip/games
src/tools/           usfrip, usfplay and n64run
externals/xsflib/    a library for players and rippers of the PSF family
plugins/foobar2000/  the foobar2000 component
plugins/xmplay/      the XMPlay plugin
docs/release-notes/  the notes of each release
tests/               the CTest suites
```

## Licence

usf is under the MIT licence (`LICENSE`). The vector unit of the RSP (`src/n64/rsp_vector.cpp` and
`src/n64/rsp_vector.h`) is adapted from [Iconoclast's RSP](https://github.com/mupen64plus/mupen64plus-rsp-cxd4)
(mupen64plus-rsp-cxd4), which Iconoclast dedicated to the public domain under CC0 1.0. `THIRD-PARTY-NOTICES.md` lists
the libraries and code that usf uses, and what a binary distribution must include. This licence doesn't cover game
data. Rips contain the code and sound data of a game and are for personal use.
