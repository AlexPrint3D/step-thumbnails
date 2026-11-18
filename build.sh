#!/usr/bin/env bash
# Builds A3D STEP Thumbnails for Windows x64 on Linux (Ubuntu 24.04) with MinGW-w64.
# Used both locally and by GitHub Actions.
#
#   ./build.sh            -> build-win/stepthumb.dll, build-win/stepthumb-render.exe,
#                            installer/A3D-STEP-Thumbnails-<ver>-setup.exe
#
# Needs: mingw-w64 cmake ninja-build nsis git
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$PWD"
VERSION="$(cat VERSION)"
OCCT_TAG="V7_7_2"
DEPS="${DEPS_DIR:-$ROOT/deps}"
OCCT_PREFIX="$DEPS/occt-win"
JOBS="${JOBS:-$(nproc)}"
CXX=x86_64-w64-mingw32-g++-posix

# ---------------------------------------------------------------- OpenCASCADE
if [ ! -f "$OCCT_PREFIX/.done-$OCCT_TAG" ]; then
  echo "== Building OpenCASCADE $OCCT_TAG (static, minimal) =="
  rm -rf "$DEPS/occt-src" "$DEPS/occt-build"
  mkdir -p "$DEPS"
  git clone -q --depth 1 --branch "$OCCT_TAG" https://github.com/Open-Cascade-SAS/OCCT.git "$DEPS/occt-src"
  cmake -S "$DEPS/occt-src" -B "$DEPS/occt-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_LIBRARY_TYPE=Static -DINSTALL_DIR="$OCCT_PREFIX" \
    -DBUILD_MODULE_FoundationClasses=OFF -DBUILD_MODULE_ModelingData=OFF \
    -DBUILD_MODULE_ModelingAlgorithms=OFF -DBUILD_MODULE_Visualization=OFF \
    -DBUILD_MODULE_ApplicationFramework=OFF -DBUILD_MODULE_DataExchange=OFF -DBUILD_MODULE_Draw=OFF \
    -DBUILD_MODULE_DETools=OFF \
    -DBUILD_ADDITIONAL_TOOLKITS="TKSTEP;TKMesh" \
    -DUSE_FREETYPE=OFF -DUSE_TK=OFF -DUSE_OPENGL=OFF -DUSE_GLES2=OFF -DUSE_D3D=OFF -DUSE_TBB=OFF \
    -DUSE_VTK=OFF -DUSE_RAPIDJSON=OFF -DUSE_DRACO=OFF -DUSE_FREEIMAGE=OFF -DUSE_FFMPEG=OFF \
    -DUSE_OPENVR=OFF -DBUILD_DOC_Overview=OFF \
    -DCMAKE_CXX_FLAGS="-Wa,-mbig-obj"
  ninja -C "$DEPS/occt-build" -j "$JOBS" install
  touch "$OCCT_PREFIX/.done-$OCCT_TAG"
fi

OCCT_INC="$(dirname "$(find "$OCCT_PREFIX" -name Standard.hxx | head -1)")"
OCCT_LIB="$(dirname "$(find "$OCCT_PREFIX" -name 'libTKernel.a' | head -1)")"

mkdir -p build-win
# ---------------------------------------------------------------- renderer
echo "== stepthumb-render.exe =="
x86_64-w64-mingw32-windres src/renderer/stepthumb-render.rc -O coff -o build-win/render-res.o
$CXX -O2 -std=gnu++17 -w -D_USE_MATH_DEFINES -DOCCT_STATIC_BUILD -I"$OCCT_INC" \
  src/renderer/step_render.cpp build-win/render-res.o -o build-win/stepthumb-render.exe \
  -static -static-libgcc -static-libstdc++ -mconsole \
  -L"$OCCT_LIB" -Wl,--start-group \
  -lTKSTEP -lTKSTEPAttr -lTKSTEP209 -lTKSTEPBase -lTKXSBase -lTKMesh -lTKShHealing \
  -lTKTopAlgo -lTKGeomAlgo -lTKBRep -lTKGeomBase -lTKG3d -lTKG2d -lTKMath -lTKernel \
  -Wl,--end-group -lws2_32 -ladvapi32 -luser32 -lgdi32 -lpsapi -lwsock32
x86_64-w64-mingw32-strip build-win/stepthumb-render.exe

# ---------------------------------------------------------------- shell handler
echo "== stepthumb.dll =="
$CXX -O2 -Wall -shared -static -static-libgcc -static-libstdc++ \
  -o build-win/stepthumb.dll src/handler/thumb_handler.cpp src/handler/thumb_handler.def \
  -lole32 -luuid -lshlwapi -lgdi32 -lshell32
x86_64-w64-mingw32-strip build-win/stepthumb.dll

# test harness (not shipped)
$CXX -O2 -municode -static -o build-win/test_handler.exe tools/test_handler.cpp \
  -lole32 -luuid -lshlwapi -lgdi32 -lshell32

# ---------------------------------------------------------------- installer
echo "== installer =="
(cd installer && makensis -V2 -DVERSION="$VERSION" -DBUILD_DIR="../build-win" installer.nsi)
ls -la build-win installer/*.exe
