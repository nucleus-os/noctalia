#!/usr/bin/env bash
set -euo pipefail

# Build the C++-ABI-bearing dependencies with the same Clang/libc++ toolchain
# as Nucleus. Sources and build trees are disposable cache outputs; only this
# recipe is versioned.
toolchain_root="${NUCLEUS_TOOLCHAIN_ROOT:-/opt/nucleus-swift/release-6.4.x/usr}"
cache_root="${XDG_CACHE_HOME:-$HOME/.cache}/nucleus/noctalia-cpp-deps"
prefix="${1:-$cache_root/clang-21-libcxx}"
source_root="$cache_root/sources"
build_root="$cache_root/build"
jobs="${JOBS:-$(nproc)}"

cc="$toolchain_root/bin/clang"
cxx="$toolchain_root/bin/clang++"
ar="$toolchain_root/bin/llvm-ar"
ranlib="$toolchain_root/bin/llvm-ranlib"

for tool in "$cc" "$cxx" "$ar" "$ranlib" cmake ninja curl tar; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done

mkdir -p "$source_root" "$build_root" "$prefix"

fetch_extract() {
  local name="$1" url="$2" expected_sha256="$3"
  local archive="$source_root/$name.tar.gz" destination="$source_root/$name"
  if [[ -f "$archive" ]]; then
    printf '%s  %s\n' "$expected_sha256" "$archive" | sha256sum --check --status || {
      echo "checksum mismatch for cached $name source" >&2
      exit 1
    }
  fi
  if [[ ! -d "$destination" ]]; then
    curl --fail --location --retry 3 --output "$archive.tmp" "$url"
    printf '%s  %s\n' "$expected_sha256" "$archive.tmp" | sha256sum --check --status || {
      rm -f "$archive.tmp"
      echo "checksum mismatch for $name" >&2
      exit 1
    }
    mv "$archive.tmp" "$archive"
    mkdir -p "$destination"
    tar -xzf "$archive" --strip-components=1 -C "$destination"
  fi
}

fetch_extract sdbus-cpp-2.2.1 \
  https://github.com/Kistler-Group/sdbus-cpp/archive/refs/tags/v2.2.1.tar.gz \
  da69a0104beb6e51415a59f1571a47beb1eacc65cc6027b250eb1cf13ff4f802
fetch_extract libqalculate-5.9.0 \
  https://github.com/Qalculate/libqalculate/releases/download/v5.9.0/libqalculate-5.9.0.tar.gz \
  94d734b9303b3b68df61e4255f2eddeee346b66ec4b6e134f19e1a3cc3ff4a09
fetch_extract tomlplusplus-3.4.0 \
  https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz \
  8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155

cmake -S "$source_root/sdbus-cpp-2.2.1" -B "$build_root/sdbus-cpp-2.2.1" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_C_COMPILER="$cc" \
  -DCMAKE_CXX_COMPILER="$cxx" \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
  -DCMAKE_SHARED_LINKER_FLAGS="-stdlib=libc++ -Wl,-rpath,$toolchain_root/lib" \
  -DBUILD_SHARED_LIBS=ON \
  -DSDBUSCPP_BUILD_LIBSYSTEMD=OFF \
  -DSDBUSCPP_BUILD_TESTS=OFF \
  -DSDBUSCPP_BUILD_EXAMPLES=OFF \
  -DSDBUSCPP_BUILD_DOCS=OFF
cmake --build "$build_root/sdbus-cpp-2.2.1" --parallel "$jobs"
cmake --install "$build_root/sdbus-cpp-2.2.1"

qalculate_build="$build_root/libqalculate-5.9.0"
mkdir -p "$qalculate_build"
(
  cd "$qalculate_build"
  CC="$cc" CXX="$cxx" AR="$ar" RANLIB="$ranlib" \
    CXXFLAGS="-O2 -stdlib=libc++" \
    LDFLAGS="-stdlib=libc++ -Wl,-rpath,$toolchain_root/lib" \
    "$source_root/libqalculate-5.9.0/configure" \
      --prefix="$prefix" --libdir="$prefix/lib" \
      --disable-static --enable-shared --enable-compiled-definitions
)
make -C "$qalculate_build" -j"$jobs"
make -C "$qalculate_build" install

mkdir -p "$prefix/include"
rm -rf "$prefix/include/toml++"
cp -a "$source_root/tomlplusplus-3.4.0/include/toml++" "$prefix/include/toml++"

# Keep the matching C++ runtime beside the private libraries. Noctalia adds
# this directory to its RUNPATH, so neither the executable nor these libraries
# can accidentally resolve to a distro libstdc++/libc++ installation.
for runtime in libc++.so libc++.so.1 libc++abi.so libc++abi.so.1 libunwind.so libunwind.so.1; do
  if [[ -e "$toolchain_root/lib/$runtime" ]]; then
    cp -aL "$toolchain_root/lib/$runtime" "$prefix/lib/$runtime"
  fi
done

echo "$prefix"
