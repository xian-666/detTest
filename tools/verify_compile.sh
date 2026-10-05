#!/bin/bash
# Build verification for detTest, without MSBuild (blocked in this sandbox).
#
# Two things must hold for the refactor to be considered clean:
#   1. every translation unit compiles (catches dangling #include refs to the
#      deleted HeroStrategy / GamePadDev / WeaponObserver game layer)
#   2. the image LINKS (catches any surviving call into those deleted symbols)
#
# Command line mirrors Release|x64, recovered from
# detTest/x64/Release/*.obj.dt.module.json.command
# NOTE: Windows SDK lives on D: here, hence the explicit -I/-libpath entries.

# MSYS/Git-Bash rewrites arguments starting with '/' into drive paths, turning
# every compiler switch into garbage. Opt out for this process.
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1

ROOT="D:/Project/GameDetect/detTest"
VS="C:/Program Files/Microsoft Visual Studio/18/Community"
VER="14.44.35207"
CL="$VS/VC/Tools/MSVC/$VER/bin/Hostx64/x64/cl.exe"
LIB="$VS/VC/Tools/MSVC/$VER/bin/Hostx64/x64/lib.exe"
SDKINC="D:\\Windows Kits\\10\\Include\\10.0.28000.0"
SDKLIB="D:\\Windows Kits\\10\\Lib\\10.0.28000.0"
MSVCINC="$VS\\VC\\Tools\\MSVC\\$VER\\include"
ATLINC="$VS\\VC\\Tools\\MSVC\\$VER\\atlmfc\\include"

cd "$ROOT/detTest" || exit 1
OUT="$(mktemp -d)"
NOISE="warning C(4828|4819|4530|4244|4267|4996|4100|4101|4189|4458|4505|4127|26812|6387|6011|33004|4091|4278)"

COMMON=(
  /nologo /c /EHsc /MD /std:c++20 /permissive- /Zc:wchar_t /Zc:forScope
  /fp:precise /W3 /WX- /utf-8 /Fo:"$OUT/"
  /D NDEBUG /D _CONSOLE /D _WIN32_WINNT=0x0A00 /D _CRT_SECURE_NO_WARNINGS
  /D _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING /D NOMINMAX
  /D _AMD64_ /D MODEL_ENCRYPT /D _UNICODE /D UNICODE
  "/I../" "/I." "/I../DirectML/include" "/I../opencv4.8.0/opencv/build/include"
  "/I$SDKINC/um" "/I$SDKINC/shared" "/I$SDKINC/winrt" "/I$SDKINC/cppwinrt" "/I$SDKINC/ucrt"
  "/I$MSVCINC" "/I$ATLINC"
)

FILES=(
  CaptureMetrics.cpp
  common/AppConfig.cpp
  detTest.cpp
  GameInferenceSystem.cpp
  ONNXInference.cpp
  ROIAdaptationConfig.cpp
  Screen/DXGIScreenCapture.cpp
  Screen/GpuEnvironmentProbe.cpp
  Screen/ScreenShot.cpp
  Screen/UnifiedScreenCapture.cpp
  Screen/WGCScreenCapture.cpp
  ../Component/AES.cpp
  ../Component/BaseUtility.cpp
  ../Component/DumpHelper.cpp
  ../Component/Log/debugLog.cpp
  ../Component/Log/ServerLog.cpp
  ../Component/RegistryReader.cpp
)

fail=0
for f in "${FILES[@]}"; do
  log=$("$CL" "${COMMON[@]}" "$f" 2>&1)
  st=$?
  # Two files share a base name (debugLog.cpp / ServerLog.cpp are unique, but
  # keep reporting explicit anyway).
  printf '%s\n' "$log" | grep -avE "$NOISE" | sed 's/^/    /'
  if [ $st -eq 0 ]; then echo "  compile OK   $f"; else echo "  compile FAIL $f  ($st)"; fail=1; fi
done

if [ $fail -ne 0 ]; then
  echo ""
  echo "=== SKIPPING LINK (compile failures above) ==="
  exit 1
fi

echo ""
echo "=== LINK ==="
link_log=$("$CL" /nologo /EHsc /MD /Fe:"$OUT/detTest.exe" "${OUT}/"*.obj \
  /link "/LIBPATH:../opencv4.8.0/opencv/build/x64/vc16/lib" \
        "/LIBPATH:../DirectML/lib" \
        "/LIBPATH:$SDKLIB/um/x64" "/LIBPATH:$SDKLIB/ucrt/x64" \
        "/LIBPATH:$VS\\VC\\Tools\\MSVC\\$VER\\lib\\x64" \
        "/LIBPATH:$VS\\VC\\Tools\\MSVC\\$VER\\atlmfc\\lib\\x64" \
        opencv_world480.lib onnxruntime.lib \
        kernel32.lib user32.lib gdi32.lib advapi32.lib winmm.lib ole32.lib \
        oleaut32.lib shell32.lib shlwapi.lib comdlg32.lib propsys.lib \
        dwmapi.lib d3d11.lib dxgi.lib uuid.lib comctl32.lib 2>&1)
link_st=$?
printf '%s\n' "$link_log" | grep -avE "warning LNK(4098|4049|4217|4099)|^$" | sed 's/^/    /'

if [ $link_st -eq 0 ]; then
  echo "  LINK OK"
  ls -la "$OUT/detTest.exe"
else
  echo "  LINK FAILED ($link_st)"
  fail=1
fi

echo ""
echo "artifacts: $OUT"
exit $fail
