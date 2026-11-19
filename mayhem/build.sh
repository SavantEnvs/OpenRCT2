#!/usr/bin/env bash
#
# mayhem/build.sh — build OpenRCT2's fuzz harnesses and its own test suite.
#
# Runs inside the commit image (mayhem/Dockerfile) as `mayhem` in /mayhem; re-runnable and offline.
#   1. libopenrct2 (+ openrct2-cli) with $SANITIZER_FLAGS + SanCov + DWARF 3, headless, as a shared
#      library in bin/fuzz/ so the 16 harness binaries share one instrumented copy of the project.
#   2. Each harness in mayhem/harnesses/fuzz_openrct2.cpp (one -DFUZZ_MODE_* per target), linked twice:
#      bin/fuzz_<t> (libFuzzer) and bin/fuzz_<t>-standalone (run-once reproducer).
#   3. The upstream gtest suite (OpenRCT2Tests) + openrct2-cli with the project's NORMAL flags in bin/test/.
#   4. Runtime data the targets read: data/object -> the pinned OpenRCT2 object set baked into the image
#      (/opt/toolchains/deps/openrct2-objects-1.8.1, what upstream's install step downloads), and a
#      pre-built object index (bin/fuzz-userdata/objects.idx) so each fuzz process skips the rescan.
# Everything written here is under gitignored paths (bin/, data/object), so `git clean -ffdX` resets it.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

OBJECTS_DIR=/opt/toolchains/deps/openrct2-objects-1.8.1/src
REPLAYS_DIR=/opt/toolchains/deps/openrct2-replays-0.0.98/src

# Headless build: no GUI/audio/HTTP/TTF (the fuzz build also drops multiplayer; the test suite needs it), and never download anything (objects come from the image).
CMAKE_COMMON=(
  -G Ninja
  -DDISABLE_GUI=ON -DDISABLE_HTTP=ON -DDISABLE_TTF=ON -DDISABLE_DISCORD_RPC=ON
  -DDISABLE_VERSION_CHECKER=ON -DDISABLE_FLAC=ON -DDISABLE_VORBIS=ON -DDISABLE_OPENGL=ON
  -DDISABLE_GOOGLE_BENCHMARK=ON -DOPENRCT2_USE_CCACHE=OFF
  -DDOWNLOAD_TITLE_SEQUENCES=OFF -DDOWNLOAD_OBJECTS=OFF -DDOWNLOAD_OPENSFX=OFF
  -DDOWNLOAD_OPENMUSIC=OFF -DDOWNLOAD_REPLAYS=OFF
)

# --- 0) runtime data (both paths are gitignored upstream; upstream's install step downloads them) ------
#   data/object                  -> the pinned object set (every park/object/replay target needs it)
#   test/tests/testdata/replays  -> the pinned replay set (upstream's ReplayTests)
for pair in "data/object:$OBJECTS_DIR" "test/tests/testdata/replays:$REPLAYS_DIR"; do
  link="${pair%%:*}"; target="${pair#*:}"
  if [ -L "$link" ] || [ ! -e "$link" ]; then
    ln -sfn "$target" "$link"
  fi
done

# --- 1) sanitized, SanCov-instrumented libopenrct2 ------------------------------------------------------
# UBSan checks relaxed (ASan and the rest of UBSan stay on and halting): they fire on benign, by-design
# patterns on nearly every input and would hide everything else —
#   alignment          unaligned little-endian field loads in the RCT1/RCT2 readers (x86 handles them);
#   nonnull-attribute  MemoryStream::Write(nullptr, 0) -> memcpy(dst, NULL, 0) on empty chunks;
#   pointer-overflow   SpriteFile/G1 code stores offsets in pointer fields and rebases them onto the
#                      buffer (null + offset for an empty buffer).
UBSAN_RELAX="-fno-sanitize=alignment,nonnull-attribute,pointer-overflow"
FUZZ_FLAGS="$SANITIZER_FLAGS $UBSAN_RELAX -fsanitize=fuzzer-no-link -O1 $DEBUG_FLAGS"
cmake -S . -B bin/fuzz "${CMAKE_COMMON[@]}" -DDISABLE_NETWORK=ON -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_C_FLAGS="$FUZZ_FLAGS" -DCMAKE_CXX_FLAGS="$FUZZ_FLAGS"
cmake --build bin/fuzz -j"$MAYHEM_JOBS" --target libopenrct2

# --- 2) harnesses ------------------------------------------------------------------------------------
# Must match libopenrct2's own compile definitions (they change class layouts) — see bin/fuzz/build.ninja.
HARNESS_CXXFLAGS=(
  -std=gnu++20 -fno-char8_t -DDEBUG=0 -DDISABLE_HTTP -DDISABLE_NETWORK -DDISABLE_TTF
  -DDISABLE_VERSION_CHECKER -DENABLE_SCRIPTING -DJSON_HAS_FILESYSTEM=0 -D_FILE_OFFSET_BITS=64
  -I"$SRC/src" -isystem "$SRC/src/thirdparty"
)
OBJ_DIR=bin/fuzz-harness
mkdir -p "$OBJ_DIR" bin
# shellcheck disable=SC2086
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$OBJ_DIR/lsan_off.o"
# shellcheck disable=SC2086
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OBJ_DIR/standalone_main.o"
LINK=(bin/fuzz/libopenrct2.so "-Wl,-rpath,\$ORIGIN/fuzz" -lpthread)

TARGETS=(park_file s6 s4 td6 td4 object_dat object_json object_parkobj parkinfo simulate sea sprite
         language config_ini replay title_sequence)
for t in "${TARGETS[@]}"; do
  mode="FUZZ_MODE_$(echo "$t" | tr '[:lower:]' '[:upper:]')"
  # shellcheck disable=SC2086
  $CXX $FUZZ_FLAGS "${HARNESS_CXXFLAGS[@]}" -D"$mode" \
    -c "$SRC/mayhem/harnesses/fuzz_openrct2.cpp" -o "$OBJ_DIR/fuzz_$t.o"
  # shellcheck disable=SC2086
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OBJ_DIR/fuzz_$t.o" "$OBJ_DIR/lsan_off.o" \
    "${LINK[@]}" -o "bin/fuzz_$t"
  # shellcheck disable=SC2086
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS "$OBJ_DIR/fuzz_$t.o" "$OBJ_DIR/standalone_main.o" "$OBJ_DIR/lsan_off.o" \
    "${LINK[@]}" -o "bin/fuzz_$t-standalone"
done

# --- 3) upstream test suite, normal flags ------------------------------------------------------------
cmake -S . -B bin/test "${CMAKE_COMMON[@]}" -DDISABLE_NETWORK=OFF -DWITH_TESTS=ON \
  -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS"
cmake --build bin/test -j"$MAYHEM_JOBS" --target OpenRCT2Tests openrct2-cli
# The tests find their data at ./data (upstream symlinks it at install time).
ln -sfn ../../data bin/test/data

# --- 4) pre-built object index for the fuzz targets' user-data dir -----------------------------------
mkdir -p bin/fuzz-userdata
bin/test/openrct2-cli scan-objects --openrct2-data-path="$SRC/data" --user-data-path="$SRC/bin/fuzz-userdata"
test -s bin/fuzz-userdata/objects.idx

echo "built: $(for t in "${TARGETS[@]}"; do printf 'bin/fuzz_%s ' "$t"; done)"
