# Third-party notices

ultralib is under the MIT licence (`LICENSE`). Its dependencies and the code it adapts have the following licences.

| Component | Licence | Use in ultralib |
|---|---|---|
| xsflib, Copyright (c) 2026 the xsflib authors | MIT (`externals/ntdlib/externals/xsflib/LICENSE`) | Compiled into every module: byte order, files, the PSF container, the reader of ROM sets and their ZIP archives, and the checks of the tests. |
| ntdlib, Copyright (c) 2026 the ntdlib authors | MIT (`externals/ntdlib/LICENSE`) | In `externals/ntdlib`, a submodule. The sound modules link it: its decoders of compressed data, its wave helpers and its conversion layer. ntdlib's `THIRD-PARTY-NOTICES.md` lists the third-party code in the parts that ultralib links. |
| seqlib, Copyright (c) 2026 the seqlib authors | MIT (`externals/ntdlib/externals/xsflib/externals/seqlib/LICENSE`) | The sound modules write MIDI files with it, and the conversion writes MIDI, SoundFont 2 and SFZ files with it. |
| [Iconoclast's RSP](https://github.com/mupen64plus/mupen64plus-rsp-cxd4) (mupen64plus-rsp-cxd4), by Iconoclast | CC0 1.0 Universal | `src/machine/rsp_vector.cpp` and `include/ultralib/machine/rsp_vector.h` adapt its vector unit (the files `vu/vu.c`, `vu/add.c`, `vu/multiply.c`, `vu/select.c`, `vu/logical.c` and `vu/divide.c`) at commit 00906a92641c540a64e4e7505012157315314810: the arithmetic, the clamps, the clip compares and the divider's use of its table. ultralib computes the table's values from their formulas. The changes to these files are dedicated to the public domain under CC0 1.0 as well. |
| [n64-systemtest](https://github.com/lemmy-64/n64-systemtest) | MIT | Only for the tests. Not included here. The tests of the core run its ROM when it's supplied (`tests/test_roms.cmake`). |
| [PeterLemon's N64 tests](https://github.com/PeterLemon/N64) | Unlicense | Only for the tests. Not included here. The tests of the core run their ROMs when they're supplied, and compare some frames with hashes of the screenshots in that repository. |

Binary distributions must include the MIT notices of ultralib and of the libraries that they link: xsflib always, and
ntdlib and seqlib with the sound modules. CC0 asks for no notice. The credit above is given all the same.

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
