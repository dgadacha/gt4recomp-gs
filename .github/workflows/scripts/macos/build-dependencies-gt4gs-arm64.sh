#!/bin/bash
# GT4Recomp fork: the dependencies of gt4-gs for macOS arm64 that Homebrew does not provide (PlutoVG, PlutoSVG,
# RapidYAML), at the versions and SHA-256 of build-dependencies-universal.sh. The others come from Homebrew:
#   brew install cmake ninja sdl3 zstd lz4 libpng jpeg-turbo webp freetype harfbuzz
# Usage: build-dependencies-gt4gs-arm64.sh <output directory>

set -e

if [ "$#" -ne 1 ]; then
	echo "Syntax: $0 <output directory>"
	exit 1
fi

export MACOSX_DEPLOYMENT_TARGET=14.4

NPROCS="$(getconf _NPROCESSORS_ONLN)"
INSTALLDIR="$1"
if [ "${INSTALLDIR:0:1}" != "/" ]; then
	INSTALLDIR="$PWD/$INSTALLDIR"
fi

PLUTOVG=1.3.2
PLUTOSVG=0.0.7
RAPIDYAML=0.12.1

mkdir -p "$INSTALLDIR" "$INSTALLDIR-build"
cd "$INSTALLDIR-build"

BREW="$(brew --prefix)"
CMAKE_COMMON=(
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_PREFIX_PATH="$INSTALLDIR;$BREW"
	-DCMAKE_INSTALL_PREFIX="$INSTALLDIR"
	-DCMAKE_OSX_ARCHITECTURES=arm64
	-DCMAKE_INSTALL_NAME_DIR="$INSTALLDIR/lib"
	-G Ninja
)

cat > SHASUMS <<SUMS
7bd4e79ce18b1d47517e7e91fbb7cf19d4f01942804a519bc7c0bf32b6325dd5  plutovg-$PLUTOVG.tar.gz
78561b571ac224030cdc450ca2986b4de915c2ba7616004a6d71a379bffd15f3  plutosvg-$PLUTOSVG.tar.gz
e9efcdd17f86287748793cf21d106e461fcad8d103a3e5a23632afe93828660d  rapidyaml-$RAPIDYAML-src.tgz
SUMS

curl -fL -o "plutovg-$PLUTOVG.tar.gz" "https://github.com/sammycage/plutovg/archive/v$PLUTOVG/plutovg-$PLUTOVG.tar.gz"
curl -fL -o "plutosvg-$PLUTOSVG.tar.gz" "https://github.com/sammycage/plutosvg/archive/v$PLUTOSVG/plutosvg-$PLUTOSVG.tar.gz"
curl -fL -o "rapidyaml-$RAPIDYAML-src.tgz" "https://github.com/biojppm/rapidyaml/releases/download/v$RAPIDYAML/rapidyaml-$RAPIDYAML-src.tgz"
shasum -a 256 --check SHASUMS

echo "Building PlutoVG..."
rm -fr "plutovg-$PLUTOVG"
tar xf "plutovg-$PLUTOVG.tar.gz"
cmake -S "plutovg-$PLUTOVG" -B "plutovg-$PLUTOVG/build" "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DPLUTOVG_BUILD_EXAMPLES=OFF
cmake --build "plutovg-$PLUTOVG/build" --parallel "$NPROCS"
cmake --install "plutovg-$PLUTOVG/build"

echo "Building PlutoSVG..."
rm -fr "plutosvg-$PLUTOSVG"
tar xf "plutosvg-$PLUTOSVG.tar.gz"
cmake -S "plutosvg-$PLUTOSVG" -B "plutosvg-$PLUTOSVG/build" "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF
cmake --build "plutosvg-$PLUTOSVG/build" --parallel "$NPROCS"
cmake --install "plutosvg-$PLUTOSVG/build"

echo "Building RapidYAML..."
rm -fr "rapidyaml-$RAPIDYAML-src"
tar xf "rapidyaml-$RAPIDYAML-src.tgz"
cmake -S "rapidyaml-$RAPIDYAML-src" -B "rapidyaml-$RAPIDYAML-src/build" "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON
cmake --build "rapidyaml-$RAPIDYAML-src/build" --parallel "$NPROCS"
cmake --install "rapidyaml-$RAPIDYAML-src/build"

echo "Done: $INSTALLDIR"
