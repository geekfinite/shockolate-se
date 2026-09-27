#!/bin/bash
set -e

SDL_version=2.0.9
SDL2_mixer_version=2.0.4

if [ -d ./build_ext/ ]; then
	# Check if all dependencies are already built
	if [ -d "./build_ext/SDL2-${SDL_version}" ] && [ -d "./build_ext/SDL2_mixer-${SDL2_mixer_version}" ] && [ -d "./build_ext/fluidsynth-lite" ]; then
		echo "All dependencies already built in build_ext/, skipping..."
		cd ..
		mv build_ext/fluidsynth-lite/*.sf2 ./res 2>/dev/null || true
		exit
	fi
	echo "A directory named build_ext exists but dependencies may need rebuilding."
	echo "Please remove it if you want to recompile everything."
	exit
fi

if [ ! -d ./res/ ]; then
	mkdir ./res/
fi

mkdir -p ./build_ext/
cd ./build_ext/

install_dir=$(pwd)

function download_sdl {
	if [ -f "SDL2-${SDL_version}.tar.gz" ] && [ -d "SDL2-${SDL_version}" ]; then
		echo "SDL2 ${SDL_version} already downloaded, skipping..."
		return
	fi
	echo "Downloading SDL2 ${SDL_version} from GitHub..."
	curl -L --fail --silent -o SDL2-${SDL_version}.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-${SDL_version}/SDL2-${SDL_version}.tar.gz
	if [ $? -ne 0 ]; then
		echo "  GitHub failed, trying libsdl.org..."
		curl -L -o SDL2-${SDL_version}.tar.gz https://www.libsdl.org/release/SDL2-${SDL_version}.tar.gz
	fi
	echo "  Extracting SDL2..."
	tar xvf SDL2-${SDL_version}.tar.gz
}

function download_sdl_mixer {
	if [ -f "SDL2_mixer-${SDL2_mixer_version}.tar.gz" ] && [ -d "SDL2_mixer-${SDL2_mixer_version}" ]; then
		echo "SDL2_mixer ${SDL2_mixer_version} already downloaded, skipping..."
		return
	fi
	echo "Downloading SDL2_mixer ${SDL2_mixer_version}..."
	# SDL_mixer releases are only on libsdl.org
	curl -L -o SDL2_mixer-${SDL2_mixer_version}.tar.gz https://www.libsdl.org/projects/SDL_mixer/release/SDL2_mixer-${SDL2_mixer_version}.tar.gz
	echo "  Extracting SDL2_mixer..."
	tar xvf SDL2_mixer-${SDL2_mixer_version}.tar.gz
}

function download_fluidsynth {
	if [ -d "fluidsynth-lite" ]; then
		echo "fluidsynth-lite already cloned, skipping..."
		return
	fi
	echo "Cloning fluidsynth-lite..."
	git clone https://github.com/EtherTyper/fluidsynth-lite.git
}

## Actual building starts here

# STEP 1: Download ALL dependencies first
echo "=== STEP 1: Downloading dependencies ==="
download_sdl
download_sdl_mixer
download_fluidsynth

# STEP 2: Build ALL dependencies
echo ""
echo "=== STEP 2: Building dependencies ==="

function build_sdl {
	echo "Building SDL2..."
	pushd SDL2-${SDL_version}
	./configure --prefix=${install_dir}/built_sdl
	make
	make install
	popd
}

function build_sdl_mixer {
	echo "Building SDL2_mixer..."
	pushd SDL2_mixer-${SDL2_mixer_version}
	export SDL2_CONFIG="${install_dir}/built_sdl/bin/sdl2-config"
	./configure --prefix=${install_dir}/built_sdl_mixer
	make
	make install
	popd
}

function build_fluidsynth {
	echo "Building fluidsynth..."
	pushd fluidsynth-lite
	sed -i 's/DLL"\ off/DLL"\ on/' CMakeLists.txt
	set +e
	cmake -DCMAKE_C_FLAGS="-Wno-error=unterminated-string-initialization -Wno-error=unused-but-set-variable" .
	cmake --build .
	set -e
	# download a soundfont that's close to the Windows default everyone knows
	curl -o music.sf2 http://rancid.kapsi.fi/windows.sf2
	popd
}

build_sdl
build_sdl_mixer
build_fluidsynth

cd ..
mv build_ext/fluidsynth-lite/*.sf2 ./res
