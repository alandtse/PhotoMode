# Photo Mode

SKSE plugin that adds photomode for Skyrim
[SSE/AE](https://www.nexusmods.com/skyrimspecialedition/mods/91701)
[VR](https://www.nexusmods.com/skyrimspecialedition/mods/184058)

This VR fork produces a single universal DLL that runtime-detects SE, AE, and VR; the
original SE/AE listing above remains powerof3's own separate build.

## Requirements

- [CMake](https://cmake.org/)
  - Add this to your `PATH`
- [PowerShell](https://github.com/PowerShell/PowerShell/releases/latest)
- [Vcpkg](https://github.com/microsoft/vcpkg)
  - Add the environment variable `VCPKG_ROOT` with the value as the path to the folder containing vcpkg
- [Visual Studio Community 2022](https://visualstudio.microsoft.com/)
  - Desktop development with C++
- [CommonLibVR (CommonLibSSE-NG)](https://github.com/alandtse/CommonLibVR)
  - Bundled as the `extern/CommonLibVR` submodule and used for **all** targets (a single
    cross-runtime build covers SE, AE, and VR); run `git submodule update --init`.

## User Requirements

- [Address Library for SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
  - Needed for SE/AE
- [VR Address Library for SKSEVR](https://www.nexusmods.com/skyrimspecialedition/mods/58101)
  - Needed for VR
- [ImGui VR Helper](https://www.nexusmods.com/skyrimspecialedition/mods/183466)
  - Needed for VR (renders the photo mode UI in-headset)

## Register Visual Studio as a Generator
* Open `x64 Native Tools Command Prompt`
* Run `cmake`
* Close the cmd window

## Building

A single cross-runtime build produces one DLL that runs on Skyrim SE, AE and VR.

```
git clone https://github.com/alandtse/PhotoMode.git
cd PhotoMode
# openvr is a submodule of CommonLibVR
git submodule update --init --recursive
cmake --preset vs2022-windows-vcpkg
cmake --build build --config Release
```

## Credits
Third-party code (PlayerMannequin for the VR photo clone, ImGuiVRHelper for in-headset
rendering) is credited in [CREDITS.md](CREDITS.md).

Please report VR-specific bugs to [GitHub Issues](https://github.com/alandtse/PhotoMode/issues).

## Licensing

[GPL-3.0-or-later](COPYING) WITH a [Modding Exception and a GPL-3.0 Linking
Exception (with Corresponding Source)](EXCEPTIONS.md), where:

- **Modded Code** — Skyrim and its variants
- **Modding Libraries** — [SKSE](https://skse.silverlock.org/), CommonLib and variants

This is a VR fork of [powerof3's PhotoMode](https://github.com/powerof3/PhotoMode); the
original work is © powerofthree under the MIT License.
