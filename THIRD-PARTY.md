# Third-party code

nwsnes is distributed under the MIT license (see `LICENSE`). Parts of it derive
from other projects, whose notices are reproduced here.

## LakeSnes

`core/cpu.c`, `core/dma.c`, `core/ppu_regs.c`, `core/ppu_ref.c`, the register
semantics in `core/snes.c`, the priority tables and pixel-exact paths in
`core/ppu.c`, and the header scoring in `pc/romhdr.c` derive from LakeSnes
(https://github.com/angelo-wf/LakeSnes). `tools/import_cpu.py` and
`tools/import_ppu.py` document the transformations applied.

```
MIT License

Copyright (c) 2021-2023 angelo_wf and contributors

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

## snesrev

The line renderer in `core/ppu.c` was written for nwsnes, but its central idea
(one priority value per pixel, layers drawn in any order) follows the renderer
of the snesrev projects (https://github.com/snesrev), MIT license,
Copyright (c) 2023 snesrev, Copyright (c) 2021 elzo_d. No code was copied.

## NumWorks sample app

The build flags of the calculator app in `Makefile` and the two declarations
at the top of `numworks/main.c` (`eadk_app_name`, `eadk_api_level`) follow
NumWorks' sample C app (https://github.com/numworks/epsilon-sample-app-c),
Copyright (c) 2022 NumWorks, BSD 3-clause license. The EADK header and the
`nwlink` tool are not part of this repository; they are fetched from npm when
building.

## Test ROMs

The test ROM in `tests/rom/` is original work and is covered by this
repository's license. The validation figures in the README were obtained with
the test ROMs of https://github.com/PeterLemon/SNES, which are not
redistributed here.
