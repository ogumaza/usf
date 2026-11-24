# Third-party notices

usf is under the MIT licence (`LICENSE`). The libraries that usf builds with, and the code that they adapt, have these
licences.

| Component | Licence | Use in usf |
|---|---|---|
| xsflib, Copyright (c) 2026 the xsflib authors | MIT (`externals/xsflib/LICENSE`) | In `externals/xsflib`, a submodule, compiled into every usf program: byte order, files and WAV output, the PSF container, the length analysis, the reader of ROM sets and their ZIP archives, the command-line frames of `usfrip` and `usfplay`, and the playback framework and the plugin templates of the player programs. |
| ultralib, Copyright (c) 2026 the ultralib authors | MIT (`externals/ultralib/LICENSE`) | In `externals/ultralib`, a submodule. The emulator core (`ultralib::machine`), compiled into every usf program, and the Aleck64 ROM sets (`ultralib::rom_set`), compiled into `usfrip`. usf builds none of the sound modules of ultralib. |
| seqlib, Copyright (c) 2026 the seqlib authors | MIT (`externals/xsflib/externals/seqlib/LICENSE`) | In `externals/xsflib/externals/seqlib`, a submodule of xsflib. Only the tests use it: their checks come from it, through `xsflib::testing`. The tests that play sets also read WAV files with it. |
| [Iconoclast's RSP](https://github.com/mupen64plus/mupen64plus-rsp-cxd4) (mupen64plus-rsp-cxd4), by Iconoclast | CC0 1.0 Universal | ultralib's `src/machine/rsp_vector.cpp` and `include/ultralib/machine/rsp_vector.h` adapt its vector unit (the files `vu/vu.c`, `vu/add.c`, `vu/multiply.c`, `vu/select.c`, `vu/logical.c` and `vu/divide.c`) at commit 00906a92641c540a64e4e7505012157315314810: the arithmetic, the clamps, the clip compares and the use of the divider table. ultralib computes the values of the table from their formulas. The changes to these files are dedicated to the public domain under CC0 1.0 as well. |
| [zlib](https://zlib.net), Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler | zlib | Parts of xsflib: the PSF writer and the reader of the ZIP archives of ROM sets, both in `usfrip`, and the playback framework (`usfplay`, the foobar2000 component and the XMPlay plugin). The installed copy is used if there is one. Otherwise v1.4 is downloaded when configuring. |
| [libFLAC](https://xiph.org/flac/), Copyright the Xiph.Org Foundation | BSD-3-Clause | FLAC output in `usfplay`. The installed copy is used if there is one. Otherwise 1.5.0 is downloaded when configuring. |
| [foobar2000 SDK](https://www.foobar2000.org/SDK), Copyright Peter Pawlowski | BSD-style (the SDK's `sdk-license.txt`, and `pfc-license.txt` for its pfc library) | Only for `plugins/foobar2000`. Not included here. The SDK is supplied when building the component. |
| [XMPlay plugin SDK](https://www.un4seen.com/xmplay.html), from Un4seen Developments | None stated | Only for `plugins/xmplay`. Not included here. The SDK is supplied when building the plugin. CI downloads it from un4seen.com. The plugin uses only the SDK's declarations of XMPlay's interface. |

Binary distributions must include the MIT notices of usf, xsflib and ultralib. Every program contains xsflib and
ultralib. The libFLAC notice is required when libFLAC is linked in: `usfplay` links it, and `usfrip`, `n64run` and the
plugins don't. Shipping `LICENSE` and this file meets these requirements. The licence texts are at the end of this file.
The licences of zlib, the foobar2000 SDK and its pfc library require no notice in binaries. The checks from seqlib go
only into tests. No code from the XMPlay plugin SDK goes into the XMPlay plugin. CC0 requires no notice, but this file
credits Iconoclast's RSP all the same.

## Licence texts

### xsflib

```
MIT License

Copyright (c) 2026 the xsflib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### ultralib

```
MIT License

Copyright (c) 2026 the ultralib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### libFLAC

```
Copyright (C) 2000-2009  Josh Coalson
Copyright (C) 2011-2025  Xiph.Org Foundation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.Org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### zlib

```
Copyright notice:

 (C) 1995-2026 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```
