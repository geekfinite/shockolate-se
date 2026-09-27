#!/bin/bash
set -e

# Fix paths with spaces - create symlink to Git installation BEFORE any other operations
if [ -d "/c/Program Files/Git" ]; then
	if [ ! -d "/c/git" ]; then
		# Try ln -s first (works on Git Bash with Developer Mode or admin)
		ln -s "/c/Program Files/Git" /c/git 2>/dev/null || {
			# Fallback to cmd mklink
			cmd //c "mklink /D c:\git \"C:\Program Files\Git\"" 2>/dev/null || {
				echo "Warning: Could not create symlink for Git path with spaces"
				echo "Please manually run in PowerShell as Admin: cmd /c 'mklink /D c:\git \"C:\Program Files\Git\"'"
			}
		}
	fi
	# Add the symlinked Git to PATH
	export PATH="/c/git/usr/bin:${PATH}"
fi

SDL_version=2.0.10
SDL2_mixer_version=2.0.4
GLEW_version=2.1.0
CMAKE_target=Unix\ Makefiles

# Removing the mwindows linker option lets us get console output
function remove_mwindows {
	sed -i -e "s/ \-mwindows//g" Makefile
}

function download_sdl {
	if [ -f "SDL2-devel-${SDL_version}-mingw.tar.gz" ] && [ -d "SDL2-${SDL_version}" ]; then
		echo "SDL2 ${SDL_version} already downloaded, skipping..."
		return
	fi
	echo "Downloading SDL2 ${SDL_version} from GitHub..."
	curl -L --fail --silent -o SDL2-devel-${SDL_version}-mingw.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-${SDL_version}/SDL2-devel-${SDL_version}-mingw.tar.gz
	if [ $? -ne 0 ]; then
		echo "  GitHub failed, trying libsdl.org..."
		curl -L -o SDL2-devel-${SDL_version}-mingw.tar.gz https://www.libsdl.org/release/SDL2-devel-${SDL_version}-mingw.tar.gz
	fi
	echo "  Extracting SDL2..."
	tar xvf SDL2-devel-${SDL_version}-mingw.tar.gz
}

function download_sdl_mixer {
	if [ -f "SDL2_mixer-devel-${SDL2_mixer_version}-mingw.tar.gz" ] && [ -d "SDL2_mixer-${SDL2_mixer_version}" ]; then
		echo "SDL2_mixer ${SDL2_mixer_version} already downloaded, skipping..."
		return
	fi
	echo "Downloading SDL2_mixer ${SDL2_mixer_version}..."
	# SDL_mixer releases are only on libsdl.org
	curl -L -o SDL2_mixer-devel-${SDL2_mixer_version}-mingw.tar.gz https://www.libsdl.org/projects/SDL_mixer/release/SDL2_mixer-devel-${SDL2_mixer_version}-mingw.tar.gz
	echo "  Extracting SDL2_mixer..."
	tar xf SDL2_mixer-devel-${SDL2_mixer_version}-mingw.tar.gz --exclude=Xcode
}

function download_glew {
	if [ -d "built_glew" ] && [ -f "glew-${GLEW_version}.tgz" ]; then
		echo "GLEW ${GLEW_version} already downloaded, skipping..."
		return
	fi
	echo "Downloading GLEW ${GLEW_version}..."
	curl -L -o glew-${GLEW_version}.tgz https://github.com/nigels-com/glew/releases/download/glew-${GLEW_version}/glew-${GLEW_version}.tgz
	if [ $? -ne 0 ]; then
		echo "  GitHub failed, trying SourceForge..."
		curl -L -o glew-${GLEW_version}.tgz https://sourceforge.net/projects/glew/files/glew/${GLEW_version}/glew-${GLEW_version}.tgz/download
	fi
	echo "  Extracting GLEW..."
	tar xvf glew-${GLEW_version}.tgz
	mv glew-${GLEW_version}/ built_glew/
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

# Skip if all dependencies are already built
if [ -d ./build_ext/built_sdl ] && [ -d ./build_ext/built_sdl_mixer ] && [ -d ./build_ext/built_glew ] && [ -d ./build_ext/fluidsynth-lite ]; then
	echo "All dependencies already built in build_ext/, skipping..."
	# Set up build.bat
	if [[ -z "${APPVEYOR}" ]]; then
		echo "Normal build"
		echo "@echo off
		cmake -G \\"${CMAKE_target}\\" .
		mingw32-make systemshock" >build.bat
	else
		echo "Appveyor"
		echo "cmake -G \\"${CMAKE_target}\\" . 
		make systemshock" >build.bat
	fi
	echo "Dependencies are ready. Run BUILD.BAT in a Windows shell to build the actual source."
	exit
fi

if ! [ -x "$(command -v cmake)" ]; then
	echo CMake is needed to install Shockolate. 
	echo Please download CMake from https://cmake.org/download/,
	echo install it and try again in a new Git Bash window.
	exit
fi

rm -rf CMakeFiles/
rm -rf CMakeCache.txt

cp windows/make.exe /usr/bin/

if [ ! -d ./res/ ]; then
mkdir ./res/
fi

mkdir -p ./build_ext/
cd ./build_ext/
install_dir=`pwd -W`

# STEP 1: Download ALL dependencies first
echo "=== STEP 1: Downloading dependencies ==="
download_sdl
download_sdl_mixer
download_glew
download_fluidsynth

# STEP 2: Build ALL dependencies
echo ""
echo "=== STEP 2: Building dependencies ==="

function build_sdl {
	echo "Building SDL2..."
	cp -r SDL2-${SDL_version}/x86_64-w64-mingw32/ built_sdl/
}

function build_sdl_mixer {
	echo "Building SDL2_mixer..."
	cp -r SDL2_mixer-${SDL2_mixer_version}/x86_64-w64-mingw32/ built_sdl_mixer/
}

function build_glew {
	echo "Building GLEW..."
	pushd built_glew
	mingw32-make glew.lib
	popd
}

function build_fluidsynth {
	echo "Building fluidsynth..."
	pushd fluidsynth-lite
	sed -i 's/DLL"\ off/DLL"\ on/' CMakeLists.txt
	set +e
	cmake -G "${CMAKE_target}" -DCMAKE_C_FLAGS="-Wno-error=unterminated-string-initialization -Wno-error=unused-but-set-variable" .
	cmake --build .
	set -e
	# download a soundfont that's close to the Windows default everyone knows
	curl -o music.sf2 http://rancid.kapsi.fi/windows.sf2
	popd
}

build_sdl
build_sdl_mixer
build_glew
build_fluidsynth

# Back to the root directory, copy required DLL files for the executable
cd ..
cp build_ext/built_sdl/bin/SDL*.dll .
cp build_ext/built_sdl_mixer/bin/SDL*.dll .
cp build_ext/built_glew/lib/*.dll .
cp build_ext/fluidsynth-lite/src/*.dll .

# Copy MinGW runtime DLLs (required by pre-built SDL packages)
cp /c/mingw-w64/i686-*/mingw32/bin/libgcc_s_dw2-1.dll . 2>/dev/null || true
cp /c/mingw-w64/i686-*/mingw32/bin/libstdc++-6.dll . 2>/dev/null || true
cp /c/mingw-w64/i686-*/mingw32/bin/libwinpthread-1.dll . 2>/dev/null || true

# move the soundfont to the correct place if we successfully built fluidsynth
mv build_ext/fluidsynth-lite/*.sf2 ./res

# Set up build.bat
if [[ -z "${APPVEYOR}" ]]; then
	echo "Normal build"
	echo "@echo off
	cmake -G \\"${CMAKE_target}\\" .
	mingw32-make systemshock" >build.bat
else
	echo "Appveyor"
	echo "cmake -G \\"${CMAKE_target}\\" . 
	make systemshock" >build.bat
fi

echo ""
echo "Our work here is done. Run BUILD.BAT in a Windows shell to build the actual source."
