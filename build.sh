#!/usr/bin/env bash
#
#  Shockolate SE -- single-script build driver.
#
#  One command does everything: fetch dependencies from GitHub, build them once, populate
#  build_ext/, configure CMake out-of-source, compile, and drop the runtime DLLs next to the
#  executable.
#
#      ./build.sh                  # native toolchain: deps + configure + build
#      ./build.sh --run            # ... and then launch the game
#      ./build.sh --verify         # check cached archives against the manifest sha256
#      ./build.sh --offline        # never touch the network (fails only if an archive is missing)
#
#  Downloads are skipped whenever the archive already exists in build_ext/.  That check is
#  deliberately independent of whether the archive has been extracted: an archive that is
#  present is never re-fetched, it is only unpacked.
#
#  Every URL lives in cmake/deps.manifest and every one of them is on github.com.
#
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

UA="shockolate-build/1.0 (+https://github.com/geekfinite/shockolate-se)"

# ---------------------------------------------------------------- output helpers ------------
if [ -t 1 ]; then
	C_R=$'\033[31m'; C_G=$'\033[32m'; C_Y=$'\033[33m'; C_B=$'\033[36m'; C_N=$'\033[0m'
else
	C_R=''; C_G=''; C_Y=''; C_B=''; C_N=''
fi
log()  { printf '%s==>%s %s\n' "$C_B" "$C_N" "$*"; }
ok()   { printf '  %sok%s   %s\n' "$C_G" "$C_N" "$*"; }
warn() { printf '%sWARN%s  %s\n' "$C_Y" "$C_N" "$*" >&2; }
die()  { printf '\n%sERROR%s %s\n' "$C_R" "$C_N" "$*" >&2; exit 1; }
step() { printf '\n%s[%s]%s %s\n' "$C_B" "$1" "$C_N" "$2"; }

usage_text() {
	cat <<'EOF'
Options:
  --arch 32|64          Target architecture. Default: whatever the active compiler targets.
  --from-source         Build SDL2/SDL2_mixer from GitHub source instead of using the prebuilt
                        MinGW packages. Forced automatically on UCRT64/CLANG64 (CRT mismatch),
                        and the normal path on Linux/macOS.
  --prebuilt            Force the prebuilt MinGW packages even on UCRT64/CLANG64 (unsafe).
  --verify              Verify the sha256 of every cached archive before using it.
  --offline             Do not use the network; fail only if a required archive is missing.
  --refresh             Re-extract and re-install dependencies (cached archives are reused).
  --manifest FILE       Use an alternative dependency manifest.
  --proxy URL           Use this proxy for downloads (e.g. http://127.0.0.1:10809).
  --no-proxy            Never use a proxy, not even a detected system proxy.
  --no-soundfont        Skip the SoundFont download (music falls back to ADLMIDI/OPL3).
  --ninja               Use the Ninja generator if ninja is on PATH.
  --jobs N              Parallel build jobs (default: number of CPUs).
  --clean               Remove the build directory, keep dependencies.
  --distclean           Remove the build directory and extracted dependencies, KEEP archives.
  --release             After building, package a release with make_release_win32.sh.
  --run                 After building, run the game.
  -h, --help            This text.
EOF
}

# ---------------------------------------------------------------- arguments -----------------
ARCH=""; FROM_SOURCE=0; FORCE_PREBUILT=0; VERIFY=0; OFFLINE=0; REFRESH=0
DO_CLEAN=0; DO_DISTCLEAN=0; DO_RELEASE=0; DO_RUN=0; NO_SOUNDFONT=0; USE_NINJA=0
MANIFEST="$SCRIPT_DIR/cmake/deps.manifest"
PROXY_MODE=auto      # auto | off | <url>
JOBS=""
CMAKE_BIN=""
FLUIDSYNTH_ENABLE=1
SRC_SDL_DIR=""; SRC_MIXER_DIR=""
BUILD_EXT="$SCRIPT_DIR/build_ext"

while [ $# -gt 0 ]; do
	case "$1" in
		--arch)         ARCH="${2:-}";    [ -n "$ARCH" ]    || die "--arch needs 32 or 64"; shift 2 ;;
		--arch=*)       ARCH="${1#*=}"; shift ;;
		--from-source)  FROM_SOURCE=1; shift ;;
		--prebuilt)     FORCE_PREBUILT=1; shift ;;
		--verify)       VERIFY=1; shift ;;
		--offline)      OFFLINE=1; shift ;;
		--refresh)      REFRESH=1; shift ;;
		--manifest)     MANIFEST="${2:-}"; [ -n "$MANIFEST" ] || die "--manifest needs a path"; shift 2 ;;
		--manifest=*)   MANIFEST="${1#*=}"; shift ;;
		--proxy)        PROXY_MODE="${2:-}"; [ -n "$PROXY_MODE" ] || die "--proxy needs a URL"; shift 2 ;;
		--proxy=*)      PROXY_MODE="${1#*=}"; shift ;;
		--no-proxy)     PROXY_MODE=off; shift ;;
		--no-soundfont) NO_SOUNDFONT=1; shift ;;
		--ninja)        USE_NINJA=1; shift ;;
		--jobs)         JOBS="${2:-}"; shift 2 ;;
		--jobs=*)       JOBS="${1#*=}"; shift ;;
		--clean)        DO_CLEAN=1; shift ;;
		--distclean)    DO_DISTCLEAN=1; shift ;;
		--release)      DO_RELEASE=1; shift ;;
		--run)          DO_RUN=1; shift ;;
		-h|--help)      usage_text; exit 0 ;;
		*)              die "unknown option: $1   (try --help)" ;;
	esac
done

[ -f "$MANIFEST" ] || die "dependency manifest not found: $MANIFEST"

# ---------------------------------------------------------------- platform ------------------
HOST="unknown"
case "$(uname -s 2>/dev/null || echo unknown)" in
	MINGW*|MSYS*|CYGWIN*) HOST=windows ;;
	Linux*)               HOST=linux ;;
	Darwin*)              HOST=macos ;;
	*)                    HOST=unknown ;;
esac

MSYSTEM_V="${MSYSTEM:-}"
CRT="unknown"
case "$MSYSTEM_V" in
	MINGW32|MINGW64)           CRT=msvcrt ;;
	UCRT64|CLANG64|CLANGARM64) CRT=ucrt ;;
	*)                         if [ "$HOST" = windows ]; then CRT=msvcrt; fi ;;
esac

if [ -z "$JOBS" ]; then
	if   command -v nproc >/dev/null 2>&1;        then JOBS=$(nproc)
	elif [ -n "${NUMBER_OF_PROCESSORS:-}" ];      then JOBS="$NUMBER_OF_PROCESSORS"
	else                                               JOBS=4
	fi
fi

# Find a compiler and ask IT what it targets.  Never trust `uname -m`: inside MSYS2's MINGW32
# shell uname -m reports the host CPU (x86_64) while gcc targets i686-w64-mingw32.  Getting this
# wrong is how you end up configuring a 64-bit build with a 32-bit compiler.
if ! command -v gcc >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1; then
	die "no C compiler on PATH.
       On Windows run this from the MSYS2 MINGW32 or MINGW64 shell
       (L:\\msys64\\mingw32.exe), not from PowerShell or cmd."
fi
CC_BIN=$(command -v gcc 2>/dev/null || command -v cc)
CXX_BIN=$(command -v g++ 2>/dev/null || command -v c++ 2>/dev/null || true)
ACTIVE_TUPLE=$("$CC_BIN" -dumpmachine 2>/dev/null || true)
[ -n "$ACTIVE_TUPLE" ] || die "could not determine the compiler target from $CC_BIN -dumpmachine"

# Key on the CPU prefix, never on the vendor suffix: "x86_64-w64-mingw32" also ends in "mingw32"
# and a naive *mingw32 glob classifies it as 32-bit.
case "${ACTIVE_TUPLE%%-*}" in
	i386|i486|i586|i686|x86)      NATIVE_BITS=32 ;;
	x86_64|amd64|arm64|aarch64)   NATIVE_BITS=64 ;;
	*)                            NATIVE_BITS=64 ;;
esac

BITS="$NATIVE_BITS"
if [ -n "$ARCH" ]; then
	case "$ARCH" in
		32|64) BITS="$ARCH" ;;
		*)     die "--arch must be 32 or 64 (got '$ARCH')" ;;
	esac
	if [ "$BITS" != "$NATIVE_BITS" ]; then
		if [ "$BITS" = 64 ]; then
			die "the active toolchain targets $ACTIVE_TUPLE (${NATIVE_BITS}-bit), not 64-bit.
       Install and enter the 64-bit environment instead:
         pacman -S mingw-w64-x86_64-toolchain
         L:\\msys64\\mingw64.exe   then   ./build.sh --arch 64"
		else
			die "the active toolchain targets $ACTIVE_TUPLE (${NATIVE_BITS}-bit), not 32-bit.
       Enter the 32-bit environment instead:
         L:\\msys64\\mingw32.exe   then   ./build.sh --arch 32"
		fi
	fi
fi
TUPLE="$ACTIVE_TUPLE"

# The prebuilt SDL packages are built against the MSVCRT runtime.  Linking them into a UCRT
# executable mixes two incompatible C runtimes (separate heaps; freeing an SDL-allocated
# surface across the boundary is undefined), so switch to source builds automatically.
if [ "$CRT" = ucrt ] && [ "$FORCE_PREBUILT" != 1 ] && [ "$FROM_SOURCE" != 1 ]; then
	FROM_SOURCE=1
	warn "MSYSTEM=$MSYSTEM_V uses the UCRT runtime, which is incompatible with the"
	warn "prebuilt MSVCRT SDL packages. Switching to --from-source automatically."
fi
if [ "$CRT" = ucrt ] && [ "$FORCE_PREBUILT" = 1 ]; then
	warn "MSYSTEM=$MSYSTEM_V is a UCRT toolchain but --prebuilt was forced; continuing as asked."
fi

# Everything architecture-specific is stamped with this.  Without it a 32-bit dependency tree
# gets silently reused by a 64-bit build (or an MSVCRT tree by a UCRT build), which only shows
# up much later as a wall of "file format not recognized" link errors.
ARCH_STAMP="$TUPLE/$CRT"

# Locate CMake.  This is the single biggest source of failure in the old scripts: the correct
# compiler lives in the MINGW32 tree while cmake lives in mingw64/ucrt64/Program Files, so
# `cmake` is simply absent from the MINGW32 shell's PATH.
find_cmake() {
	local c
	for c in "${CMAKE:-}" \
	         "${MINGW_PREFIX:-}/bin/cmake.exe" \
	         /mingw32/bin/cmake.exe /mingw64/bin/cmake.exe \
	         /ucrt64/bin/cmake.exe /clang64/bin/cmake.exe /usr/bin/cmake.exe \
	         "/c/Program Files/CMake/bin/cmake.exe" \
	         "/c/Program Files (x86)/CMake/bin/cmake.exe"; do
		if [ -n "$c" ] && [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
	done
	if command -v cmake >/dev/null 2>&1; then command -v cmake; return 0; fi
	return 1
}

CMAKE_BIN=$(find_cmake || true)
[ -n "$CMAKE_BIN" ] || die "CMake was not found. Install one of:
         MSYS2:   pacman -S mingw-w64-i686-cmake     (or mingw-w64-x86_64-cmake)
         or the official installer from https://cmake.org/download/
       Searched: \$CMAKE, \$MINGW_PREFIX/bin, /mingw32|mingw64|ucrt64|clang64/bin, PATH,
                 C:\\Program Files\\CMake\\bin"

CMAKE_VER=$("$CMAKE_BIN" --version 2>/dev/null | head -1 | awk '{print $3}')
[ -n "$CMAKE_VER" ] || die "could not run: $CMAKE_BIN --version"
CMAKE_MAJOR=${CMAKE_VER%%.*}
if [ "$CMAKE_MAJOR" -lt 3 ] 2>/dev/null; then
	die "CMake $CMAKE_VER is too old; version 3.20 or newer is required"
fi

# Generator: prefer the compiler's own make so CMake never has to search for one.  That search
# is exactly what produced the old `CMAKE_MAKE_PROGRAM-NOTFOUND` dead end.
GEN=""; MAKEPROG=""
if [ "$USE_NINJA" = 1 ] && command -v ninja >/dev/null 2>&1; then
	GEN="Ninja"
elif [ "$HOST" = windows ]; then
	if   command -v mingw32-make >/dev/null 2>&1; then GEN="MinGW Makefiles"; MAKEPROG=$(command -v mingw32-make)
	elif command -v make >/dev/null 2>&1;         then GEN="MSYS Makefiles";  MAKEPROG=$(command -v make)
	fi
else
	if   command -v ninja >/dev/null 2>&1; then GEN="Ninja"
	elif command -v make >/dev/null 2>&1;  then GEN="Unix Makefiles"; MAKEPROG=$(command -v make)
	fi
fi
[ -n "$GEN" ] || die "no make or ninja on PATH (one is needed to drive the build)"

# CMake is a native Windows binary, so it must not be handed MSYS-style paths such as
# /mingw32/bin/gcc: it reads those as L:/mingw32/bin/gcc and then fails with "is not a full path
# to an existing compiler tool".  Translate every path we hand to CMake into native form.
native_path() {
	local p="$1"
	[ -n "$p" ] || { printf ''; return 0; }
	if command -v cygpath >/dev/null 2>&1; then
		cygpath -m "$p" 2>/dev/null || printf '%s' "$p"
	else
		printf '%s' "$p"
	fi
}
CC_ARG=$(native_path "$CC_BIN")
CXX_ARG=$(native_path "$CXX_BIN")
MAKE_ARG=$(native_path "$MAKEPROG")

case "$TUPLE" in
	i686*)   PRESET=32 ;;
	x86_64*) PRESET=64 ;;
	*)       PRESET="$BITS" ;;
esac
case "$HOST" in
	windows) BUILD_SUBDIR="build/win${PRESET}" ;;
	linux)   BUILD_SUBDIR="build/linux${PRESET}" ;;
	macos)   BUILD_SUBDIR="build/macos${PRESET}" ;;
	*)       BUILD_SUBDIR="build/host${PRESET}" ;;
esac
BUILD_DIR="$SCRIPT_DIR/$BUILD_SUBDIR"

# Never allow the build directory to be the source directory. An in-source configure writes
# CMakeCache.txt, Makefile and the generated shockolate_version.h straight into the tree, which
# is exactly the mess this script exists to remove. Fail loudly instead.
case "$BUILD_DIR" in
	"$SCRIPT_DIR"/build/*) : ;;
	*) die "internal error: refusing to use '$BUILD_DIR' as the build directory" ;;
esac

step "1/5" "Toolchain"
printf '  %-16s %s\n' "host"      "$HOST${MSYSTEM_V:+  ($MSYSTEM_V)}"
printf '  %-16s %s\n' "compiler"  "$CC_BIN  [$ACTIVE_TUPLE]"
printf '  %-16s %s\n' "runtime"   "$CRT"
printf '  %-16s %s\n' "target"    "$BITS-bit  ($TUPLE)"
printf '  %-16s %s\n' "cmake"     "$CMAKE_BIN  ($CMAKE_VER)"
printf '  %-16s %s\n' "generator" "$GEN${MAKEPROG:+  [$MAKEPROG]}"
printf '  %-16s %s\n' "deps"      "$([ "$FROM_SOURCE" = 1 ] && echo 'build from source' || echo 'prebuilt MinGW packages')"
printf '  %-16s %s\n' "build dir" "$BUILD_SUBDIR"
printf '  %-16s %s\n' "jobs"      "$JOBS"

# ---------------------------------------------------------------- proxy ---------------------
# curl does not read the Windows registry proxy, only http_proxy/https_proxy.  If a system proxy
# is configured (very common with V2Ray/Xray/Clash-style clients) and those variables are unset,
# every curl in the old scripts could reach nothing while the browser worked fine.  So: try the
# direct route first and fall back to the system proxy -- never the other way round, because
# forcing a dead proxy would break downloads that would otherwise have succeeded.
detect_system_proxy() {
	local key out en srv
	if [ -n "${https_proxy:-}" ]; then printf '%s' "$https_proxy"; return 0; fi
	if [ -n "${HTTPS_PROXY:-}" ]; then printf '%s' "$HTTPS_PROXY"; return 0; fi
	if [ -n "${http_proxy:-}" ];  then printf '%s' "$http_proxy";  return 0; fi
	if [ -n "${HTTP_PROXY:-}" ];  then printf '%s' "$HTTP_PROXY";  return 0; fi
	command -v reg.exe >/dev/null 2>&1 || return 0
	# Query the key as a whole rather than using /v: MSYS rewrites a bare /v into a path, which
	# makes reg.exe fail with "Invalid syntax".
	key='HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings'
	out=$(reg.exe query "$key" 2>/dev/null | tr -d '\r')
	[ -n "$out" ] || return 0
	en=$(printf '%s\n' "$out" | sed -n 's/.*ProxyEnable[[:space:]]*REG_DWORD[[:space:]]*//p' | head -1)
	[ "$en" = "0x1" ] || return 0
	srv=$(printf '%s\n' "$out" | sed -n 's/.*ProxyServer[[:space:]]*REG_SZ[[:space:]]*//p' | head -1)
	[ -n "$srv" ] || return 0
	case "$srv" in
		*http=*) srv=$(printf '%s' "$srv" | tr ';' '\n' | sed -n 's/^http=//p' | head -1) ;;
	esac
	[ -n "$srv" ] || return 0
	case "$srv" in
		*://*) printf '%s' "$srv" ;;
		*)     printf 'http://%s' "$srv" ;;
	esac
}

SYSTEM_PROXY=""
if [ "$PROXY_MODE" = auto ]; then SYSTEM_PROXY=$(detect_system_proxy || true); fi
case "$PROXY_MODE" in
	off)  printf '  %-16s %s\n' "proxy" "disabled (--no-proxy)" ;;
	*)    if [ -n "$SYSTEM_PROXY" ] && [ "$PROXY_MODE" = auto ]; then
	          printf '  %-16s %s\n' "proxy" "$SYSTEM_PROXY  (system; only used if direct fails)"
	      elif [ "$PROXY_MODE" = auto ]; then
	          printf '  %-16s %s\n' "proxy" "none configured"
	      else
	          printf '  %-16s %s\n' "proxy" "$PROXY_MODE"
	      fi ;;
esac

try_download() {  # url dest [proxy]
	local url="$1" dest="$2" proxy="${3:-}"
	local args=(-L --fail --retry 3 --retry-delay 2 --connect-timeout 25 --max-time 1800 -A "$UA")
	if [ -n "$proxy" ]; then args+=(--proxy "$proxy"); fi
	rm -f "$dest.part"
	if curl -sS "${args[@]}" -o "$dest.part" "$url"; then
		mv -f "$dest.part" "$dest"     # atomic: an interrupted download never looks "present"
		return 0
	fi
	rm -f "$dest.part"
	return 1
}

download() {  # url dest
	local url="$1" dest="$2"
	if [ "$PROXY_MODE" != "off" ] && [ "$PROXY_MODE" != "auto" ]; then
		try_download "$url" "$dest" "$PROXY_MODE" && return 0
		return 1
	fi
	try_download "$url" "$dest" "" && return 0
	if [ "$PROXY_MODE" = auto ] && [ -n "$SYSTEM_PROXY" ]; then
		log "  direct download failed; retrying via system proxy $SYSTEM_PROXY"
		try_download "$url" "$dest" "$SYSTEM_PROXY" && return 0
	fi
	return 1
}

sha256_of() {
	if   command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
	elif command -v certutil  >/dev/null 2>&1; then certutil -hashfile "$1" SHA256 | sed -n '2p' | tr -d ' \r' | tr 'A-Z' 'a-z'
	elif command -v shasum    >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
	else printf ''
	fi
}

# ---------------------------------------------------------------- deps -----------------------
mkdir -p "$BUILD_EXT"

# --- architecture stamps ---------------------------------------------------------------------
# Each directory holding compiled/arch-specific artefacts records which target produced it.
_stamp_path() { printf '%s/.shockolate-arch' "$BUILD_EXT/$1"; }
_stamp_of()   { cat "$(_stamp_path "$1")" 2>/dev/null || printf 'an unknown target'; }
stamp_matches() {  # dir-name
	[ -f "$(_stamp_path "$1")" ] && [ "$(_stamp_of "$1")" = "$ARCH_STAMP" ]
}
write_stamp() { printf '%s\n' "$ARCH_STAMP" > "$(_stamp_path "$1")"; }

# Drop a directory whose stamp does not match this target.  Returns 0 if it is still usable.
deps_arch_guard() {  # label dir-name
	local d="$BUILD_EXT/$2"
	[ -d "$d" ] || return 1
	if stamp_matches "$2"; then return 0; fi
	warn "$1: build_ext/$2 was built for $(_stamp_of "$2"), this build is $ARCH_STAMP"
	warn "       rebuilding it for $ARCH_STAMP"
	rm -rf "$d"
	return 1
}

# Soft variant: returns 1 on any problem and never exits, so best-effort dependencies can fail
# without taking the whole build down.  ensure_archive is the hard wrapper.
try_ensure_archive() {  # name filename url sha
	local name="$1" file="$BUILD_EXT/$2" url="$3" sha="$4" got

	if [ -s "$file" ]; then
		if [ "$VERIFY" = 1 ] && [ -n "$sha" ]; then
			got=$(sha256_of "$file")
			if [ -n "$got" ] && [ "$got" != "$sha" ]; then
				warn "$2 exists but its sha256 does not match the manifest -- re-downloading"
				rm -f "$file"
			elif [ -n "$got" ]; then
				ok "$name: cached archive sha256 verified"
			else
				warn "$name: no sha256 tool available, could not verify the cached archive"
			fi
		fi
		if [ -s "$file" ]; then
			ok "$name: archive cached, skipping download"
			return 0
		fi
	fi

	if [ "$OFFLINE" = 1 ]; then
		warn "$name: archive is missing and --offline was given"
		return 1
	fi

	log "  $name: downloading $2"
	if ! download "$url" "$file"; then
		warn "$name: download failed: $url"
		return 1
	fi

	if [ -n "$sha" ]; then
		got=$(sha256_of "$file")
		if [ -z "$got" ]; then
			warn "$name: no sha256 tool available, skipping the integrity check"
		elif [ "$got" != "$sha" ]; then
			rm -f "$file"
			warn "$name: sha256 mismatch after download
       expected $sha
       got      $got
       The archive was removed; re-run to fetch it again."
			return 1
		else
			ok "$name: sha256 verified"
		fi
	else
		warn "$name: upstream publishes no sha256 for this asset; presence check only"
	fi
	return 0
}

ensure_archive() {  # name filename url sha
	try_ensure_archive "$@" && return 0
	die "$1: could not obtain $2
       If github.com is unreachable, start your proxy/VPN and retry, or pass --proxy URL.
       Once the archives are cached you can rebuild with --offline."
}

try_ensure_extracted() {  # name extract_dir filename
	local name="$1" dir="$BUILD_EXT/$2" file="$BUILD_EXT/$3"

	if [ "$REFRESH" != 1 ] && [ -d "$dir" ] && [ -n "$(ls -A "$dir" 2>/dev/null || true)" ]; then
		ok "$name: already extracted, skipping"
		return 0
	fi
	[ -f "$file" ] || { warn "$name: archive missing, cannot extract"; return 1; }

	log "  $name: extracting $3"
	rm -rf "$dir.tmp"
	mkdir -p "$dir.tmp"
	case "$3" in
		*.tar.gz|*.tgz)
			tar xzf "$file" -C "$dir.tmp" --strip-components=1 ;;
		*.tar.xz)
			tar xJf "$file" -C "$dir.tmp" --strip-components=1 ;;
		*.zip)
			command -v unzip >/dev/null 2>&1 || { rm -rf "$dir.tmp"; warn "$name: unzip is required for $3"; return 1; }
			unzip -q "$file" -d "$dir.tmp" ;;
		*)
			rm -rf "$dir.tmp"; warn "$name: unsupported archive type: $3"; return 1 ;;
	esac
	if [ -z "$(ls -A "$dir.tmp" 2>/dev/null || true)" ]; then
		rm -rf "$dir.tmp"
		warn "$name: extraction produced nothing from $3"
		return 1
	fi
	rm -rf "$dir"
	mv "$dir.tmp" "$dir"
	ok "$name: extracted"
	return 0
}

ensure_extracted() {  # name extract_dir filename
	try_ensure_extracted "$@" && return 0
	die "$1: could not extract $3"
}

install_mingw_pkg() {  # name extract_dir tuple dest
	local name="$1" src="$BUILD_EXT/$2/$3" dst="$BUILD_EXT/$4"

	[ -d "$src" ] || die "$name: the package contains no $3/ subdirectory (looked in $src)"
	if [ "$REFRESH" != 1 ] && [ -d "$dst/include/SDL2" ] && stamp_matches "$4"; then
		ok "$name: build_ext/$4 ready"
		return 0
	fi
	if [ -d "$dst" ] && [ "$REFRESH" != 1 ] && ! stamp_matches "$4"; then
		warn "$name: build_ext/$4 was built for $(_stamp_of "$4"), this build is $ARCH_STAMP"
		warn "       reinstalling it for $ARCH_STAMP"
	fi
	log "  $name: installing $3 -> build_ext/$4"
	rm -rf "$dst"
	mkdir -p "$dst"
	cp -r "$src/." "$dst/"
	[ -d "$dst/include/SDL2" ] || die "$name: install into build_ext/$4 produced no include/SDL2"
	write_stamp "$4"
	ok "$name: build_ext/$4 ready"
}

build_glew() {
	local d="$BUILD_EXT/built_glew" dll
	if [ "$REFRESH" != 1 ] && [ -f "$d/lib/libglew32.dll.a" ] && stamp_matches "built_glew"; then
		ok "GLEW: already built"
		return 0
	fi
	log "  GLEW: building import library and DLL (make glew.lib)"
	if ! ( cd "$d" && "$MAKEPROG" -j"$JOBS" glew.lib ) >"$d/.build.log" 2>&1; then
		tail -n 30 "$d/.build.log" >&2
		die "GLEW build failed (full log: build_ext/built_glew/.build.log)"
	fi
	[ -f "$d/lib/libglew32.dll.a" ] || die "GLEW build produced no lib/libglew32.dll.a"
	dll=$(find "$d" -name 'glew32.dll' -print -quit 2>/dev/null || true)
	[ -n "$dll" ] || die "GLEW build produced no glew32.dll"
	write_stamp "built_glew"
	ok "GLEW: built"
}

build_fluidsynth() {  # best-effort: returns 1 instead of exiting
	local d="$BUILD_EXT/fluidsynth-lite" lib dll
	local -a args

	if [ "$REFRESH" != 1 ] && { [ -f "$d/src/libfluidsynth.dll.a" ] || [ -f "$d/src/libfluidsynth.a" ]; } \
	   && stamp_matches "fluidsynth-lite"; then
		ok "fluidsynth-lite: already built"
		return 0
	fi
	[ -d "$d" ] || { warn "fluidsynth-lite: source tree missing"; return 1; }

	# Upstream defaults to a static library; the game wants the shared one.
	if grep -q 'DLL"\ off' "$d/CMakeLists.txt" 2>/dev/null; then
		sed -i 's/DLL"\ off/DLL"\ on/' "$d/CMakeLists.txt"
	fi

	log "  fluidsynth-lite: configuring and building (best-effort)"
	args=(-S "$d" -B "$d" -G "$GEN"
	      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DDLL=ON
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	      -DCMAKE_C_FLAGS="-Wno-error=unterminated-string-initialization -Wno-error=unused-but-set-variable"
	      -DCMAKE_CXX_FLAGS="-Wno-error=unterminated-string-initialization -Wno-error=unused-but-set-variable")
	if [ -n "$MAKE_ARG" ]; then args+=(-DCMAKE_MAKE_PROGRAM="$MAKE_ARG"); fi

	if ! "$CMAKE_BIN" "${args[@]}" >"$d/.configure.log" 2>&1; then
		warn "fluidsynth-lite: configure failed (see build_ext/fluidsynth-lite/.configure.log)"
		return 1
	fi
	if ! "$CMAKE_BIN" --build "$d" --parallel "$JOBS" >"$d/.build.log" 2>&1; then
		warn "fluidsynth-lite: build failed (see build_ext/fluidsynth-lite/.build.log)"
		return 1
	fi

	# Collect the artefacts wherever the project decided to put them.
	lib=$(find "$d" -name 'libfluidsynth.dll.a' 2>/dev/null | head -1 || true)
	if [ -z "$lib" ]; then lib=$(find "$d" -name 'libfluidsynth.a' 2>/dev/null | head -1 || true); fi
	dll=$(find "$d" -name 'libfluidsynth.dll' 2>/dev/null | head -1 || true)
	if [ -z "$lib" ]; then
		warn "fluidsynth-lite: build finished but produced no import library"
		return 1
	fi
	mkdir -p "$d/src"
	# These may already sit in src/ when the project builds in-source -- never copy onto itself.
	if [ ! "$lib" -ef "$d/src/$(basename "$lib")" ]; then cp -f "$lib" "$d/src/"; fi
	if [ -n "$dll" ] && [ ! "$dll" -ef "$d/src/$(basename "$dll")" ]; then cp -f "$dll" "$d/src/"; fi
	write_stamp "fluidsynth-lite"
	ok "fluidsynth-lite: built"
	return 0
}

build_from_source_sdl() {  # name extract_dir dest
	local name="$1" src="$BUILD_EXT/$2" dst="$BUILD_EXT/$3" bdir="$BUILD_EXT/$2/.cmake-build"
	local -a common

	[ -d "$src" ] || { warn "$name: source tree missing at $src"; return 1; }
	if [ "$REFRESH" != 1 ] && [ -d "$dst/include/SDL2" ] && stamp_matches "$3"; then
		ok "$name: build_ext/$3 ready"
		return 0
	fi
	if [ -d "$dst" ] && [ "$REFRESH" != 1 ] && ! stamp_matches "$3"; then
		warn "$name: build_ext/$3 was built for $(_stamp_of "$3"), this build is $ARCH_STAMP"
		rm -rf "$dst"
	fi

	log "  $name: configuring ${BITS}-bit from source"
	common=(-G "$GEN" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$dst")
	if [ -n "$MAKE_ARG" ]; then common+=(-DCMAKE_MAKE_PROGRAM="$MAKE_ARG"); fi
	if [ "$HOST" = windows ]; then
		common+=(-DCMAKE_C_COMPILER="$CC_ARG")
		if [ -n "$CXX_ARG" ]; then common+=(-DCMAKE_CXX_COMPILER="$CXX_ARG"); fi
	fi

	rm -rf "$bdir"
	mkdir -p "$bdir"
	if [ "$name" = "SDL2-src" ]; then
		"$CMAKE_BIN" -S "$src" -B "$bdir" "${common[@]}" \
			-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF >"$src/.configure.log" 2>&1 \
			|| { warn "$name: configure failed (see $src/.configure.log)"; return 1; }
	else
		# GitHub's SDL2_mixer source tarball does NOT contain the git submodules under
		# external/ (ogg, opus, opusfile, tremor, libxmp, wavpack, mpg123, libflac, libgme),
		# so SDL2MIXER_VENDORED=ON fails with
		#     "No cmake project for ogg found in external/ogg"
		# as soon as any codec wants one.  SDL2MIXER_OPUS and SDL2MIXER_MOD are ON by default
		# and both need a submodule, so they must be switched off explicitly.
		# Everything this game actually uses is in-tree: WAV, MP3 (minimp3), FLAC (drflac),
		# OGG (stb_vorbis).  The game's only call is Mix_LoadWAV_RW(); Mix_Init(MIX_INIT_MP3)
		# merely logs on failure, and music goes through its own FluidSynth/ADLMIDI path.
		"$CMAKE_BIN" -S "$src" -B "$bdir" "${common[@]}" \
			-DSDL2MIXER_VENDORED=ON -DSDL2MIXER_SAMPLES=OFF -DSDL2MIXER_CMD=OFF \
			-DSDL2MIXER_OPUS=OFF -DSDL2MIXER_MOD=OFF -DSDL2MIXER_WAVPACK=OFF -DSDL2MIXER_GME=OFF \
			-DSDL2_DIR="$BUILD_EXT/built_sdl/lib/cmake/SDL2" >"$src/.configure.log" 2>&1 \
			|| { warn "$name: configure failed (see $src/.configure.log)"; return 1; }
	fi
	"$CMAKE_BIN" --build "$bdir" --parallel "$JOBS" >"$src/.build.log" 2>&1 \
		|| { warn "$name: build failed (see $src/.build.log)"; return 1; }
	"$CMAKE_BIN" --install "$bdir" >"$src/.install.log" 2>&1 \
		|| { warn "$name: install failed (see $src/.install.log)"; return 1; }
	[ -d "$dst/include/SDL2" ] || { warn "$name: install produced no include/SDL2"; return 1; }
	write_stamp "$3"
	ok "$name: build_ext/$3 ready"
	return 0
}

ensure_soundfont() {  # url dest
	local url="$1" dest="$SCRIPT_DIR/$2"

	if [ "$NO_SOUNDFONT" = 1 ]; then ok "soundfont: skipped (--no-soundfont)"; return 0; fi
	if ls "$SCRIPT_DIR"/res/*.sf2 >/dev/null 2>&1; then
		ok "soundfont: res/*.sf2 already present, skipping"
		return 0
	fi
	if [ "$OFFLINE" = 1 ]; then
		warn "soundfont: offline with no res/*.sf2 -- music will use ADLMIDI/OPL3"
		return 0
	fi
	log "  soundfont: downloading GeneralUser-GS.sf2 (about 32 MB, one time)"
	mkdir -p "$(dirname "$dest")"
	if download "$url" "$dest"; then
		ok "soundfont: res/music.sf2"
	else
		# Deliberately non-fatal: the in-tree ADLMIDI/OPL3 emulation provides music without it.
		warn "soundfont: download failed -- music will use ADLMIDI/OPL3 instead"
	fi
	return 0
}

dep_line() {  # one manifest record
	local name="$1" kind="$2" ver="$3" file="$4" url="$5" sha="$6" extract="$7" dest="$8"

	case "$kind" in
		mingw-pkg)
			if [ "$FROM_SOURCE" = 1 ]; then ok "$name: skipped (--from-source)"; return 0; fi
			ensure_archive   "$name" "$file" "$url" "$sha"
			ensure_extracted "$name" "$extract" "$file"
			install_mingw_pkg "$name" "$extract" "$TUPLE" "$dest"
			;;
		source-src)
			if [ "$FROM_SOURCE" != 1 ]; then ok "$name: skipped (prebuilt mode)"; return 0; fi
			ensure_archive   "$name" "$file" "$url" "$sha"
			ensure_extracted "$name" "$extract" "$file"
			case "$name" in
				SDL2-src)       SRC_SDL_DIR="$extract" ;;
				SDL2_mixer-src) SRC_MIXER_DIR="$extract" ;;
			esac
			;;
		source)
			# Compiled artefacts live inside the extracted tree, so a change of target means
			# this dependency has to be rebuilt from scratch.
			deps_arch_guard "$name" "$extract" || true
			if [ "$name" = "fluidsynth-lite" ]; then
				# Best-effort: MIDI synthesis must not be able to break the build.
				if ! try_ensure_archive "$name" "$file" "$url" "$sha" \
				   || ! try_ensure_extracted "$name" "$extract" "$file"; then
					warn "fluidsynth-lite: unavailable -- FluidSynth MIDI will be disabled"
					FLUIDSYNTH_ENABLE=0
					return 0
				fi
				if ! build_fluidsynth; then
					warn "fluidsynth-lite: build failed -- FluidSynth MIDI will be disabled"
					FLUIDSYNTH_ENABLE=0
				fi
				return 0
			fi
			ensure_archive   "$name" "$file" "$url" "$sha"
			ensure_extracted "$name" "$extract" "$file"
			case "$name" in
				GLEW) build_glew ;;
			esac
			;;
		data)
			if [ "$name" = "soundfont" ]; then ensure_soundfont "$url" "$dest"; fi
			;;
		*)
			die "manifest: unknown kind '$kind' for $name" ;;
	esac
	return 0
}

step "2/5" "Dependencies  (archives in build_ext/ are reused, never re-downloaded)"

while IFS='|' read -r m_name m_kind m_ver m_file m_url m_sha m_extract m_dest; do
	case "$m_name" in ''|\#*) continue ;; esac
	m_name=$(printf '%s' "$m_name" | tr -d ' \t')
	m_kind=$(printf '%s' "$m_kind" | tr -d ' \t')
	m_ver=$(printf '%s' "$m_ver" | tr -d ' \t')
	m_file=$(printf '%s' "$m_file" | tr -d ' \t')
	m_url=$(printf '%s' "$m_url" | tr -d ' \t')
	m_sha=$(printf '%s' "$m_sha" | tr -d ' \t')
	m_extract=$(printf '%s' "$m_extract" | tr -d ' \t')
	m_dest=$(printf '%s' "$m_dest" | tr -d ' \t')
	if [ "$m_dest" = "-" ];    then m_dest=""; fi
	if [ "$m_extract" = "-" ]; then m_extract=""; fi
	dep_line "$m_name" "$m_kind" "$m_ver" "$m_file" "$m_url" "$m_sha" "$m_extract" "$m_dest"
done < <(tr -d '\r' < "$MANIFEST")

if [ "$FROM_SOURCE" = 1 ]; then
	[ -n "$SRC_SDL_DIR" ]   || die "manifest has no SDL2-src entry for --from-source"
	[ -n "$SRC_MIXER_DIR" ] || die "manifest has no SDL2_mixer-src entry for --from-source"
	build_from_source_sdl "SDL2-src"       "$SRC_SDL_DIR"   "built_sdl"       || die "SDL2 source build failed"
	build_from_source_sdl "SDL2_mixer-src" "$SRC_MIXER_DIR" "built_sdl_mixer" || die "SDL2_mixer source build failed"
fi

# Verify the pieces the existing CMakeLists.txt expects, so a dependency problem is reported
# here rather than as a confusing link error later.
[ -d "$BUILD_EXT/built_sdl/include/SDL2" ]       || die "build_ext/built_sdl is incomplete"
[ -d "$BUILD_EXT/built_sdl_mixer/include/SDL2" ] || die "build_ext/built_sdl_mixer is incomplete"
if [ "$HOST" = windows ]; then
	[ -f "$BUILD_EXT/built_glew/lib/libglew32.dll.a" ] || die "build_ext/built_glew is incomplete"
fi

# ---------------------------------------------------------------- clean ----------------------
if [ "$DO_DISTCLEAN" = 1 ]; then
	log "distclean: removing $BUILD_SUBDIR and the extracted dependencies (archives are kept)"
	rm -rf "$BUILD_DIR"
	for d in built_sdl built_sdl_mixer built_glew fluidsynth-lite \
	         SDL2-2.32.10 SDL2_mixer-2.8.2 SDL2-2.32.10-src SDL2_mixer-2.8.2-src; do
		rm -rf "$BUILD_EXT/$d"
	done
	log "distclean: done -- the cached archives still live in build_ext/"
	exit 0
fi
if [ "$DO_CLEAN" = 1 ]; then
	log "clean: removing $BUILD_SUBDIR"
	rm -rf "$BUILD_DIR"
	exit 0
fi

# ---------------------------------------------------------------- configure / build ----------
# ---------------------------------------------------------------- stale build trees ----------
# Switching target invalidates any other configured build tree: its CMake cache still points at
# the dependency directories we just repopulated for a different architecture, so it would fail
# later with "file format not recognized" instead of anything helpful.  Build trees are derived
# artefacts, so drop the ones that no longer match -- but verify the path really is ours first.
mkdir -p "$BUILD_DIR"
for d in "$SCRIPT_DIR"/build/*/; do
	[ -d "$d" ] || continue
	[ -f "$d/CMakeCache.txt" ] || continue
	p=$(cd "$d" && pwd)
	[ "$p" = "$BUILD_DIR" ] && continue
	other=""
	if [ -f "$d/.shockolate-arch" ]; then
		other=$(cat "$d/.shockolate-arch" 2>/dev/null || true)
	else
		# Configured before stamps existed: infer the target from the compiler it recorded.
		cc=$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$d/CMakeCache.txt" 2>/dev/null | head -1 | tr 'A-Z' 'a-z')
		case "$cc" in
			*ucrt64*|*clang64*) other="x86_64-w64-mingw32/ucrt" ;;
			*mingw32*)          other="i686-w64-mingw32/msvcrt" ;;
			*mingw64*)          other="x86_64-w64-mingw32/msvcrt" ;;
			*)                  other="an unknown target" ;;
		esac
	fi
	[ "$other" = "$ARCH_STAMP" ] && continue
	case "$p" in
		"$SCRIPT_DIR"/build/*)
			warn "removing stale build tree $(basename "$p") (configured for $other; this build is $ARCH_STAMP)"
			rm -rf "$p" ;;
		*)
			warn "refusing to remove an unexpected path: $p" ;;
	esac
done
case "$BUILD_DIR" in
	"$SCRIPT_DIR"/build/*) printf '%s\n' "$ARCH_STAMP" > "$BUILD_DIR/.shockolate-arch" ;;
	*) warn "not writing a build stamp outside build/: $BUILD_DIR" ;;
esac

step "3/5" "Configure  ($BUILD_SUBDIR)"

cmake_args=(-S "$SCRIPT_DIR" -B "$BUILD_DIR" -G "$GEN" -DCMAKE_BUILD_TYPE=Release)
if [ -n "$MAKE_ARG" ]; then cmake_args+=(-DCMAKE_MAKE_PROGRAM="$MAKE_ARG"); fi
if [ "$HOST" = windows ]; then
	cmake_args+=(-DCMAKE_C_COMPILER="$CC_ARG")
	if [ -n "$CXX_ARG" ]; then cmake_args+=(-DCMAKE_CXX_COMPILER="$CXX_ARG"); fi
fi
cmake_args+=(-DENABLE_SDL2=BUNDLED -DENABLE_SOUND=BUNDLED)
if [ "$FLUIDSYNTH_ENABLE" = 1 ]; then
	cmake_args+=(-DENABLE_FLUIDSYNTH=BUNDLED)
else
	cmake_args+=(-DENABLE_FLUIDSYNTH=OFF)
fi

if ! "$CMAKE_BIN" "${cmake_args[@]}"; then
	# A half-written cache would poison every later run; clear it so a retry starts clean.
	rm -f "$BUILD_DIR/CMakeCache.txt"
	rm -rf "$BUILD_DIR/CMakeFiles"
	warn "the CMake cache was cleared so the next run starts from scratch"
	die "CMake configure failed"
fi

step "4/5" "Compile"
"$CMAKE_BIN" --build "$BUILD_DIR" --target systemshock --parallel "$JOBS" || die "compile failed"

EXE="$BUILD_DIR/systemshock.exe"
if [ ! -f "$EXE" ]; then EXE="$BUILD_DIR/systemshock"; fi
[ -f "$EXE" ] || die "the build finished but produced no executable in $BUILD_DIR"

step "5/5" "Runtime"
copied=0
copy_dll() {  # space-separated candidate paths
	local src
	for src in $1; do
		if [ -f "$src" ]; then
			cp -f "$src" "$BUILD_DIR/"
			printf '  %sok%s   %s\n' "$C_G" "$C_N" "$(basename "$src")"
			copied=$((copied + 1))
		fi
	done
}

# objdump: the compiler's own comes first (it defines the expected format), plus a second one so a
# wrong-architecture DLL can be described rather than merely rejected.
OBJDUMP_BIN=""
for c in "$(dirname "$CC_BIN")/objdump.exe" /ucrt64/bin/objdump.exe /clang64/bin/objdump.exe \
         /mingw64/bin/objdump.exe /mingw32/bin/objdump.exe; do
	if [ -x "$c" ]; then OBJDUMP_BIN="$c"; break; fi
done
if [ -z "$OBJDUMP_BIN" ] && command -v objdump >/dev/null 2>&1; then OBJDUMP_BIN=$(command -v objdump); fi
ALT_OBJDUMP=""
for c in /ucrt64/bin/objdump.exe /clang64/bin/objdump.exe /mingw64/bin/objdump.exe /mingw32/bin/objdump.exe; do
	if [ -x "$c" ] && [ "$c" != "$OBJDUMP_BIN" ]; then ALT_OBJDUMP="$c"; break; fi
done
pe_format() { if [ -n "$1" ] && [ -x "$1" ]; then "$1" -f "$2" 2>/dev/null | sed -n 's/.*file format //p' | head -1; fi; }

if [ "$HOST" = windows ]; then
	# This step owns the DLL set next to the executable, so clear it first.  A foreign DLL left
	# behind by an earlier build (the exact failure that produced 0xc000007b) must not survive.
	rm -f "$BUILD_DIR"/*.dll

	copy_dll "$BUILD_EXT/built_sdl/bin/SDL2.dll"
	copy_dll "$BUILD_EXT/built_sdl_mixer/bin/SDL2_mixer.dll"
	copy_dll "$BUILD_EXT/built_glew/bin/glew32.dll $BUILD_EXT/built_glew/lib/glew32.dll"
	copy_dll "$BUILD_EXT/fluidsynth-lite/src/libfluidsynth.dll"

	# The MinGW runtime DLLs must come from the directory of the compiler that actually built the
	# executable.  Do NOT derive them from $MINGW_PREFIX: that variable can disagree with the
	# compiler on PATH (an inherited MINGW_PREFIX=/ucrt64 inside a MINGW32 shell, for instance),
	# which copies 64-bit runtime DLLs next to a 32-bit exe.  Windows reports that combination as
	# 0xc000007b (STATUS_INVALID_IMAGE_FORMAT) at launch, and nothing in the build notices.
	CC_DIR=$(dirname "$CC_BIN")
	log "  runtime DLLs from $CC_DIR"
	for f in libwinpthread-1.dll libstdc++-6.dll; do
		if [ -f "$CC_DIR/$f" ]; then copy_dll "$CC_DIR/$f"; fi
	done
	# Exactly one of libgcc_s_dw2-1.dll (32-bit) or libgcc_s_seh-1.dll (64-bit) exists per
	# toolchain -- copy whichever this one actually ships instead of listing both.
	for f in "$CC_DIR"/libgcc_s_*.dll; do
		if [ -f "$f" ]; then copy_dll "$f"; fi
	done
	if [ "$copied" = 0 ]; then warn "no runtime DLLs were copied"; fi

	# Refuse to emit a binary that cannot start.
	if [ -n "$OBJDUMP_BIN" ]; then
		exe_fmt=$(pe_format "$OBJDUMP_BIN" "$EXE")
		if [ -z "$exe_fmt" ]; then
			warn "could not determine the architecture of $(basename "$EXE"); skipping the DLL check"
		else
			bad=0
			for f in "$BUILD_DIR"/*.dll; do
				[ -f "$f" ] || continue
				fmt=$(pe_format "$OBJDUMP_BIN" "$f")
				if [ -z "$fmt" ]; then
					foreign=$(pe_format "$ALT_OBJDUMP" "$f")
					warn "$(basename "$f") is not a $exe_fmt file${foreign:+ -- it is $foreign}"
					bad=1
				elif [ "$fmt" != "$exe_fmt" ]; then
					warn "$(basename "$f") is $fmt but $(basename "$EXE") is $exe_fmt"
					bad=1
				fi
			done
			if [ "$bad" = 1 ]; then
				die "architecture mismatch among the DLLs beside $(basename "$EXE").
       Launching it would fail with 0xc000007b (STATUS_INVALID_IMAGE_FORMAT).
       Expected format: $exe_fmt.  Runtime DLLs were taken from $CC_DIR."
			fi
			ok "all DLLs beside the executable match ($exe_fmt)"
		fi
	fi
fi

VER_STR=$(sed -n 's/.*SHOCKOLATE_VERSION "\(.*\)".*/\1/p' \
	"$BUILD_DIR/src/GameSrc/Headers/shockolate_version.h" 2>/dev/null | head -1 || true)
printf '\n%sBuilt:%s %s\n' "$C_G" "$C_N" "$EXE"
if [ -n "$VER_STR" ]; then printf '       %s\n' "$VER_STR"; fi

if [ "$DO_RELEASE" = 1 ]; then
	step ">" "Release packaging"
	[ -f "$SCRIPT_DIR/make_release_win32.sh" ] || die "make_release_win32.sh not found"
	EXE="$EXE" ARCH_TAG="win${PRESET}" MINGW_BIN="$CC_DIR" \
		bash "$SCRIPT_DIR/make_release_win32.sh" || die "release packaging failed"
fi

if [ "$DO_RUN" = 1 ]; then
	step ">" "Running"
	# Run from the repository root so res/ and shaders/ resolve, with the build directory on
	# PATH so the Windows loader finds the freshly copied DLLs.
	( cd "$SCRIPT_DIR" && PATH="$BUILD_DIR:$PATH" "$EXE" )
fi
