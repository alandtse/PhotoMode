# Credits & Third-Party Notices

PhotoMode is MIT-licensed (see `LICENSE`). It incorporates or adapts the following
third-party work; their notices are reproduced below as required by their licenses.

## PlayerMannequin — appearance/clone technique (MIT)

The VR photo clone (`src/PhotoMode/PlayerClone.{h,cpp}`) adapts the player-appearance-copy
technique from **PlayerMannequin** by **Adrien Melia (Sylennus)** — specifically borrowing
the player's facegen through `TESNPC::faceNPC` and the `SetActorBaseDataFlag` engine call.

- Source: https://github.com/Sylennus/PlayerMannequin

```
MIT License

Copyright (c) 2022 Adrien Melia

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

## ImGuiVRHelper — VR ImGui rendering client SDK (LGPL-3.0-or-later)

The VR build links the header-only client SDK from **ImGuiVRHelper**
(`api/` surface) to render the photo mode UI in-headset.

- Source: https://github.com/alandtse/imgui-vr-helper
- License: LGPL-3.0-or-later (`api/COPYING.LESSER`)

> Note: the `api/` client SDK is LGPL. Because PhotoMode compiles it in statically, LGPL
> §4 obligations apply to VR distributions (allow the user to relink against a modified
> ImGuiVRHelper — e.g. ship object files or use a shared library). Confirm the chosen
> distribution form meets this before release.
