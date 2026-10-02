@echo off
rem EchoFramePCVR build: LibOVRRT64_1.dll (ReviveXR + our patch, static OpenXR loader, static CRT)
rem and EchoFrame.exe (the launcher), into build\out\. Fetches the pinned sources into deps\ once.
setlocal
cd /d "%~dp0"

rem (no parenthesised block here: the ")" in %ProgramFiles(x86)% would end it)
if defined VSCMD_ARG_TGT_ARCH goto :have_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath >"%TEMP%\efp-vsdir.txt"
set /p VSDIR=<"%TEMP%\efp-vsdir.txt"
if not defined VSDIR echo Visual Studio with the C++ tools is required. & exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
:have_vs

rem ---- pinned sources -------------------------------------------------------------------
set REVIVE_COMMIT=ab73167e2380135aee9fe9f68874ec7dd2912cb0
set OVRSDK_COMMIT=4b1d6b7e86077062e5de33dda7d77b5790202c4c
set OPENXR_COMMIT=f2448a8797c85814aa892efc1ab8707900fbcc78
if not exist deps mkdir deps
if not exist deps\Revive (
  git clone -q https://github.com/LibreVR/Revive deps\Revive || exit /b 1
  git -C deps\Revive checkout -q %REVIVE_COMMIT% || exit /b 1
  git -C deps\Revive submodule update -q --init Externals/microprofile Externals/Vulkan || exit /b 1
  git -C deps\Revive apply ..\..\patches\revive-efp.patch || exit /b 1
)
if not exist deps\ovr_sdk_pc (
  git clone -q https://github.com/sparsebase/ovr_sdk_win deps\ovr_sdk_pc || exit /b 1
  git -C deps\ovr_sdk_pc checkout -q %OVRSDK_COMMIT% || exit /b 1
)
if not exist deps\OpenXR-SDK (
  git clone -q https://github.com/KhronosGroup/OpenXR-SDK deps\OpenXR-SDK || exit /b 1
  git -C deps\OpenXR-SDK checkout -q %OPENXR_COMMIT% || exit /b 1
  git -C deps\OpenXR-SDK apply ..\..\patches\openxr-loader-static-crt.patch || exit /b 1
)

rem ---- OpenXR loader, static ------------------------------------------------------------
if exist build\openxr\src\loader\openxr_loader.lib goto :have_loader
cmake -S deps\OpenXR-SDK -B build\openxr -G Ninja -DCMAKE_BUILD_TYPE=Release -DDYNAMIC_LOADER=OFF -DBUILD_API_LAYERS=OFF -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF || exit /b 1
cmake --build build\openxr --target openxr_loader || exit /b 1
:have_loader

rem ---- LibOVRRT64_1.dll -----------------------------------------------------------------
set REV=deps\Revive\ReviveXR
set EXT=deps\Revive\Externals
set OVR=deps\ovr_sdk_pc\LibOVR
if not exist build\obj mkdir build\obj
if not exist build\out mkdir build\out

set INC=/Iruntime\src /Ibuild\obj /I%EXT%\microprofile /I%EXT%\glad\include /I%EXT%\Vulkan\include /Ideps\OpenXR-SDK\include /I%OVR%\Include /Ideps\Revive\ReviveOverlay
set DEF=/DXR_USE_PLATFORM_WIN32 /DVK_NO_PROTOTYPES /DVK_USE_PLATFORM_WIN32_KHR /DNOMINMAX /DMICROPROFILE_ENABLED=0 /DMICROPROFILE_GPU_TIMERS=0 /DOVR_DLL_BUILD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE
set SRC=%REV%\Common.cpp %REV%\HapticsBuffer.cpp %REV%\InputManager.cpp %REV%\REV_CAPI.cpp %REV%\REV_CAPI_Audio.cpp
set SRC=%SRC% %REV%\REV_CAPI_D3D.cpp %REV%\REV_CAPI_GL.cpp %REV%\REV_CAPI_Vk.cpp %REV%\Session.cpp %REV%\Runtime.cpp
set SRC=%SRC% %REV%\Swapchain.cpp %REV%\SwapchainD3D11.cpp %REV%\SwapchainD3D12.cpp %REV%\SwapchainGL.cpp %REV%\SwapchainVk.cpp
set SRC=%SRC% %REV%\microprofile.cpp %OVR%\Shim\OVR_CAPI_Util.cpp %OVR%\Shim\OVR_StereoProjection.cpp runtime\src\efp_main.cpp runtime\src\efp_fp64.cpp runtime\src\efp_dxil.cpp runtime\src\efp_timing.cpp runtime\src\efp_foveation.cpp runtime\src\efp_input.cpp
set LIBS=build\openxr\src\loader\openxr_loader.lib Ws2_32.lib opengl32.lib d3d11.lib d3d12.lib dxgi.lib dxguid.lib dsound.lib
set LIBS=%LIBS% Winmm.lib Shlwapi.lib Pathcch.lib user32.lib advapi32.lib ole32.lib shell32.lib cfgmgr32.lib

rem stub shaders for GPUs without double precision (runtime\src\efp_fp64.cpp)
fxc /nologo /T ps_5_0 /E main /Vn g_StubPS /Fh build\obj\stub_ps.h runtime\shaders\stub_ps.hlsl >nul || exit /b 1
fxc /nologo /T vs_5_0 /E main /Vn g_StubVS /Fh build\obj\stub_vs.h runtime\shaders\stub_vs.hlsl >nul || exit /b 1
fxc /nologo /T cs_5_0 /E main /Vn g_StubCS /Fh build\obj\stub_cs.h runtime\shaders\stub_cs.hlsl >nul || exit /b 1
cl /nologo /c /MT /O2 /W1 %INC% %DEF% /Fobuild\obj\glad.obj %EXT%\glad\src\glad.c || exit /b 1
cl /nologo /LD /MT /O2 /EHsc /std:c++17 /W1 /MP /FIchrono %INC% %DEF% /Fobuild\obj\ /Febuild\out\LibOVRRT64_1.dll %SRC% build\obj\glad.obj /link %LIBS% || exit /b 1

rem ---- LibOVRPlatform64_1.dll (Platform SDK stand-in) --------------------------------------
cl /nologo /LD /MT /O2 /W3 /DUNICODE /D_UNICODE /Fobuild\obj\ /Febuild\out\LibOVRPlatform64_1.dll platform\efp_platform.c ole32.lib || exit /b 1

rem ---- EchoFrame.exe --------------------------------------------------------------------
cl /nologo /MT /O2 /EHsc /W3 /DUNICODE /D_UNICODE /Fobuild\obj\ /Febuild\out\EchoFrame.exe launcher\efp_launcher.cpp advapi32.lib user32.lib || exit /b 1

del /q build\out\*.exp build\out\*.lib 2>nul
copy /y runtime\echoframe.ini build\out\ >nul
echo Built build\out\: LibOVRRT64_1.dll, LibOVRPlatform64_1.dll, EchoFrame.exe
