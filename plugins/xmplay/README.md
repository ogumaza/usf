# xmp-usfplay

XMPlay input plugin for USF rips (`.miniusf` and `.usf`). It uses the same player as `usfplay`. The player runs the
game's code on an emulated Nintendo 64, and the game's audio microcode on the emulated RSP.

XMPlay is a 32-bit program for Windows. The plugin is therefore a 32-bit DLL, `xmp-usfplay.dll`, built with clang-cl. It
requires XMPlay 3.8.2 or later. Put it in XMPlay's folder, or in a folder inside it, and restart XMPlay.

## Features

- Tags become XMPlay's tags: `game` as album and `year` as date. The Message info window lists every tag except `_lib`,
  the flags, `length`, `fade`, `volume` and `utf8`.
- Length and fade come from the `length` and `fade` tags. Files without a length tag play for the default length and
  fade.
- The plugin's options page, among the decoders in XMPlay's options: default length and fade, endless playback, and
  rendering ahead. XMPlay stores the settings.
- During playback, the plugin renders ahead on a separate thread as fast as the CPU allows. It buffers up to 10 minutes
  and 10 seconds of audio. Seeking within the buffer is instant. Seeking further ahead waits for rendering to reach the
  requested position. Seeking back before the buffered audio renders again from the start of the track.
- Output is 16-bit stereo at the rate that the game gives its audio interface, without resampling.

## Building

**On GitHub (no Windows machine needed):** the `XMPlay plugin` workflow (`.github/workflows/xmplay.yml`) builds the DLL
and uploads it with the licences. Start it from the repository's Actions tab (Run workflow). The workflow downloads the
XMPlay plugin SDK from un4seen.com (`externals/xsflib/ci/xmplay-sdk`).

**On Windows:** you need Visual Studio 2022 or later with the C++ workload and the "C++ Clang tools for Windows"
component, plus CMake 3.23 or later. Download the XMPlay plugin SDK (<https://www.un4seen.com/files/xmp-sdk.zip>) and
unpack it. Use the folder that holds `xmpin.h` as the SDK path below.

```
cmake -S . -B build-xmplay -A Win32 -T ClangCL -DXMPLAY_SDK_DIR=C:/path/to/xmp-sdk
cmake --build build-xmplay --config Release --target xmp-usfplay
```

This produces `build-xmplay/plugins/xmplay/Release/xmp-usfplay.dll`. Everything is built with the static C runtime
(`/MT`). The plugin then requires no runtime DLLs.

## Performance

Playback runs on one CPU core. Most of the time goes to emulating the RSP and the Nintendo 64's CPU. A 64-bit build is
faster than a 32-bit one. The 64-bit foobar2000 component therefore takes less CPU time than the plugin for the same
rips.

## Licence

Like the rest of usf, the plugin is MIT-licensed. It contains xsflib (MIT) and code adapted from Iconoclast's RSP (CC0
1.0), and uses zlib. Ship `LICENSE` and `THIRD-PARTY-NOTICES.md` (or their text) with a build. The XMPlay plugin SDK
states no licence. The plugin uses only the SDK's declarations of XMPlay's interface. No code from the SDK goes into it.
