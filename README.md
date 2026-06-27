# Photo Mode

SKSE plugin that adds photomode for Skyrim Special Edition

## Requirements
* [CMake](https://cmake.org/)
	* Add this to your `PATH`
* [PowerShell](https://github.com/PowerShell/PowerShell/releases/latest)
* [Vcpkg](https://github.com/microsoft/vcpkg)
	* Add the environment variable `VCPKG_ROOT` with the value as the path to the folder containing vcpkg
* [Visual Studio Community 2022](https://visualstudio.microsoft.com/)
	* Desktop development with C++
* [CommonLibVR (CommonLibSSE-NG)](https://github.com/alandtse/CommonLibVR)
	* All targets (SSE/AE/VR) build against the NG library bundled at `extern/CommonLibVR`
	* SSE and AE are single-runtime builds; VR is a full cross-runtime build

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

## License
[MIT](LICENSE)
