# Third-party notices

EchoFramePCVR's own code is MIT (see `LICENSE`). It builds on the following.

## Revive (ReviveXR), MIT

`build.cmd` fetches [LibreVR/Revive](https://github.com/LibreVR/Revive) at `ab73167` and
applies `patches/revive-efp.patch`; its OpenXR backend is compiled into `LibOVRRT64_1.dll`.

```
MIT License

Copyright (c) 2016 LibreVR

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

## EchoFrame (EchoQuestXR), MIT

`platform/efp_platform.c` is a Windows port of EchoFrame's `runtime/ovrplatform_standin.c`
([heisthecat31/EchoFrame](https://github.com/heisthecat31/EchoFrame)).

```
MIT License

Copyright (c) 2026 heisthecat31

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

## OpenXR SDK, Apache License 2.0

The Khronos OpenXR loader ([KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK)
at `f2448a8`, with `patches/openxr-loader-static-crt.patch`) is linked statically into
`LibOVRRT64_1.dll`. It includes JsonCpp (MIT). Full texts: `deps/OpenXR-SDK/LICENSE` and
`deps/OpenXR-SDK/LICENSES/` after a build.

## Oculus PC SDK

The Oculus PC SDK 1.55 headers and two of its shim sources (`OVR_CAPI_Util.cpp`,
`OVR_StereoProjection.cpp`), fetched from [sparsebase/ovr_sdk_win](https://github.com/sparsebase/ovr_sdk_win)
at `4b1d6b7`, are compiled into `LibOVRRT64_1.dll`, as in Revive. They are under the
Oculus SDK License Agreement (https://developer.oculus.com/licenses/oculussdk/). Nothing
from the SDK is in this repository.

## Prior work this builds on (no code taken)

- [heisthecat31/EchoXR](https://github.com/heisthecat31/EchoXR) showed ReviveXR running
  Echo on SteamVR, the launch-copy approach for Echo's runtime signature check, and the
  SteamVR fixes (OpenXR 1.0, the EPIC FOV struct, waiting for READY, Touch bindings).
- [TesseractCat/libovr-openxr-rs](https://github.com/TesseractCat/libovr-openxr-rs) showed
  Echo on Linux through Proton and the headset calibration cache idea.
- [Villagers654/RiftLift](https://github.com/Villagers654/RiftLift) (GPL-3) runs Rift games,
  Echo included, on Linux.
- [saphid/frame-control](https://github.com/saphid/frame-control) and EchoFrame for the
  Steam Frame's adb, Steam shortcut and SteamVR details.
- ValveSoftware/Proton#10211 for the x86-64 MSVC runtime crash on the Frame.
