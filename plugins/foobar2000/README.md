# foo_input_usfplay

foobar2000 input component for USF rips (`.miniusf` and `.usf`). It uses the same player as `usfplay`. The player runs
the game's code on an emulated Nintendo 64, and the game's audio microcode on the emulated RSP.

A third-party component already holds the name `foo_input_usf`. This one is a separate component under another name. If
both are installed, foobar2000 uses one of them for these files. Remove the other to be sure which one plays.

## Features

- Like other PSF-family components, the component shows tags as metadata: `game` as album, `year` as date, and
  `replaygain_*` as ReplayGain. Tags can be edited. Editing rewrites the tag area and keeps `_lib`, the flags, `length`,
  `fade` and `volume`.
- Length and fade come from the `length` and `fade` tags. Files without a length tag play for the default length and
  fade.
- Advanced preferences › Decoding › USF decoder (usfplay): default length and fade, endless playback, and rendering
  ahead. Endless playback and rendering ahead apply to playback only, never to conversion or scanning.
- During playback, the component renders ahead on a separate thread as fast as the CPU allows. It buffers up to 10
  minutes and 10 seconds of audio. Seeking within the buffer is instant. Seeking further ahead waits for rendering to
  reach the requested position. Seeking back before the buffered audio renders again from the start of the track.
- Output is 16-bit stereo at the rate that the game gives its audio interface, without resampling. The rate differs
  from game to game. foobar2000 gets it when a track starts. The file's properties show it only during playback.

## Building

You need the foobar2000 SDK, from <https://www.foobar2000.org/SDK>. Unpack it and use the folder containing `pfc` and
`foobar2000` as the SDK path below. A newer SDK than the one the component was last built with may require changes to
`xsflib_add_foobar2000_component` in `externals/xsflib/cmake/XsflibApps.cmake`.

**On macOS:** you need the Xcode command line tools (`xcode-select --install`), CMake 3.23 or later, and the SDK
unpacked somewhere (macOS's `tar -xf SDK-<date>.7z` unpacks it).

```
cmake -S . -B build-fb2k -DCMAKE_BUILD_TYPE=Release -DFOOBAR2000_SDK_DIR=/path/to/SDK \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-fb2k --target foo_input_usfplay
```

This produces `build-fb2k/plugins/foobar2000/foo_input_usfplay.component`, a bundle for Apple Silicon Macs. To install
it by hand, put it in `~/Library/foobar2000-v2/user-components/foo_input_usfplay/`. For a `.fb2k-component`, put the
bundle in a `mac` folder inside a zip. The bundle is signed ad hoc. It has no Developer ID signature and isn't
notarized.

**On Windows:** you need Visual Studio 2022 or later with the C++ workload and the "C++ Clang tools for Windows"
component, plus CMake 3.23 or later.

```
cmake -S . -B build-fb2k -A x64 -T ClangCL -DFOOBAR2000_SDK_DIR=C:/path/to/SDK
cmake --build build-fb2k --config Release --target foo_input_usfplay
```

This produces `build-fb2k/plugins/foobar2000/Release/foo_input_usfplay.dll`. For a `.fb2k-component`, zip the DLL. For
64-bit foobar2000 v2, put the DLL in an `x64` folder inside the zip. For 32-bit foobar2000, configure with
`-A Win32 -T ClangCL` and put that DLL at the top level of the zip. With the 64-bit DLL in `pkg\x64`, Windows' `tar`
makes the zip: `tar -a -c -f foo_input_usfplay.zip -C pkg x64`. Then rename the zip to
`foo_input_usfplay.fb2k-component`. Windows PowerShell's `Compress-Archive` writes the folder's name with a backslash.
The zip format doesn't allow that, and foobar2000 doesn't recognize such a package.

On Windows everything is built with the static C runtime (`/MT`). The component then requires no runtime DLLs. On macOS
it uses only the system's libraries.

## Performance

Playback runs on one CPU core. Most of the time goes to emulating the RSP and the Nintendo 64's CPU. A 64-bit build is
faster than a 32-bit one. Use 64-bit foobar2000 and the 64-bit component if your other components support them.

## Licence

Like the rest of usf, the component is MIT-licensed. It contains xsflib (MIT) and code adapted from Iconoclast's RSP
(CC0 1.0), and uses zlib. Ship `LICENSE` and `THIRD-PARTY-NOTICES.md` (or their text) with a build. Nothing in the
component is GPL. It can therefore be distributed as a foobar2000 component.
