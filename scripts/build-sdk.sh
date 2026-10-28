#!/usr/bin/env bash
# Builds and installs the PlotJuggler plugin SDK and the libraries it needs
# into a prefix, so the plugin can be built with
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<prefix>
#
# Usage: scripts/build-sdk.sh <prefix> [sdk-version]
#
# The SDK version must match the PlotJuggler release the plugin is meant
# for: PlotJuggler 4.0.0 is built against plotjuggler_sdk 0.34.1.
set -euo pipefail

PREFIX=${1:?usage: build-sdk.sh <prefix> [sdk-version]}
SDK_VERSION=${2:-0.34.1}
PREFIX=$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

GEN=()
if command -v ninja >/dev/null; then GEN=(-G Ninja); fi

build() {  # name repo tag [cmake args...]
  local name=$1 repo=$2 tag=$3
  shift 3
  echo "== $name $tag"
  git -c advice.detachedHead=false clone -q --depth 1 -b "$tag" "$repo" "$WORK/$name"
  cmake -S "$WORK/$name" -B "$WORK/$name/build" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" >/dev/null
  cmake --build "$WORK/$name/build" --config Release --parallel >/dev/null
  cmake --install "$WORK/$name/build" --config Release >/dev/null
}

build nlohmann_json https://github.com/nlohmann/json v3.12.0 -DJSON_BuildTests=OFF
build fast_float https://github.com/fastfloat/fast_float v8.1.0
build fmt https://github.com/fmtlib/fmt 12.1.0 -DFMT_TEST=OFF -DFMT_DOC=OFF
build plotjuggler_sdk https://github.com/PlotJuggler/plotjuggler_sdk "v$SDK_VERSION" \
  -DPJ_INSTALL_SDK=ON -DPJ_BUILD_TESTS=OFF -DPJ_BUILD_PORTED_PLUGINS=OFF -DPJ_WARNINGS_AS_ERRORS=OFF

echo "SDK $SDK_VERSION installed in $PREFIX"
