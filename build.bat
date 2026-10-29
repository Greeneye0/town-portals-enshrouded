@echo off
rem Builds Town Portal for the Greeneye Loader: bin\loader\town-portal.dll + mod.json
setlocal

if not defined GREENEYE_COMMON set "GREENEYE_COMMON=%~dp0src"
if not exist "%GREENEYE_COMMON%\greeneye.h" (
    echo greeneye.h not found in %GREENEYE_COMMON%
    exit /b 1
)

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo Failed to initialize VS2022 x64 build environment
    exit /b 1
)

cd /d "%~dp0"
set "PORTAL_OUTPUT=bin\loader"
set "PORTAL_DEFINES="
if defined TOWN_PORTAL_DEV set "PORTAL_OUTPUT=bin-dev\loader"
if defined TOWN_PORTAL_DEV set "PORTAL_DEFINES=/DTOWN_PORTAL_DEV"
if not exist "%PORTAL_OUTPUT%" mkdir "%PORTAL_OUTPUT%"
if not exist build mkdir build

echo Compiling town-portal.dll...
cl /nologo /std:c++20 /utf-8 /O2 /Zi /EHsc /MT /LD /W4 %PORTAL_DEFINES% /I "%GREENEYE_COMMON%" ^
    src\main.cpp src\portal_travel.cpp src\portal_render.cpp src\portal_scene_probe.cpp src\portal_cast.cpp src\portal_names.cpp src\portal_markers.cpp src\portal_snapshot_codec.cpp src\portal_snapshot_share.cpp src\portal_snapshot_cache.cpp src\portal_default_images.cpp ^
    /Fo:build\ ^
    /Fe:"%PORTAL_OUTPUT%\town-portal.dll" ^
    /Fd:build\ ^
    /link /INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF /PDB:"%PORTAL_OUTPUT%\..\town-portal.pdb" /MAP:"%PORTAL_OUTPUT%\..\town-portal.map" /IMPLIB:build\town-portal.lib user32.lib gdi32.lib winmm.lib windowscodecs.lib ole32.lib
if errorlevel 1 exit /b 1
copy /y mod.json "%PORTAL_OUTPUT%\mod.json" >nul
if errorlevel 1 exit /b 1

echo Compiling test_patches.exe...
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 ^
    tests\test_patches.cpp ^
    /Fo:build\test_patches.obj ^
    /Fe:bin\test_patches.exe
if errorlevel 1 exit /b 1

echo Running offline binary and protocol verification...
bin\test_patches.exe
if errorlevel 1 exit /b 1

echo Built %PORTAL_OUTPUT%\town-portal.dll for Greeneye Loader API 2
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_render_math.cpp /Fo:build\test_render_math.obj /Fe:bin\test_render_math.exe
if errorlevel 1 exit /b 1
bin\test_render_math.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_clip_math.cpp /Fo:build\test_clip_math.obj /Fe:bin\test_clip_math.exe
if errorlevel 1 exit /b 1
bin\test_clip_math.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_composite_math.cpp /Fo:build\test_composite_math.obj /Fe:bin\test_composite_math.exe
if errorlevel 1 exit /b 1
bin\test_composite_math.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_terrain_policy.cpp /Fo:build\test_terrain_policy.obj /Fe:bin\test_terrain_policy.exe
if errorlevel 1 exit /b 1
bin\test_terrain_policy.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_snapshot_timing.cpp /Fo:build\test_snapshot_timing.obj /Fe:bin\test_snapshot_timing.exe
if errorlevel 1 exit /b 1
bin\test_snapshot_timing.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_snapshot_transport.cpp src\portal_snapshot_codec.cpp /Fo:build\ /Fe:bin\test_snapshot_transport.exe
if errorlevel 1 exit /b 1
bin\test_snapshot_transport.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_snapshot_latency.cpp src\portal_snapshot_codec.cpp /Fo:build\ /Fe:bin\test_snapshot_latency.exe
if errorlevel 1 exit /b 1
bin\test_snapshot_latency.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_home_cache.cpp src\portal_snapshot_codec.cpp /Fo:build\ /Fe:bin\test_home_cache.exe
if errorlevel 1 exit /b 1
bin\test_home_cache.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_default_view.cpp /Fo:build\ /Fe:bin\test_default_view.exe
if errorlevel 1 exit /b 1
bin\test_default_view.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_map_warp.cpp /Fo:build\ /Fe:bin\test_map_warp.exe
if errorlevel 1 exit /b 1
bin\test_map_warp.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_quality.cpp /Fo:build\ /Fe:bin\test_quality.exe
if errorlevel 1 exit /b 1
bin\test_quality.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_prompt_view.cpp /Fo:build\ /Fe:bin\test_prompt_view.exe
if errorlevel 1 exit /b 1
bin\test_prompt_view.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /utf-8 /O2 /EHsc /MT /W4 tests\test_language.cpp /Fo:build\ /Fe:bin\test_language.exe
if errorlevel 1 exit /b 1
bin\test_language.exe
if errorlevel 1 exit /b 1
