#!/usr/bin/env bash
# ═════════════════════════════════════════════════════════════════════════════
#  rebuild.sh — Pinpunch Terminal incremental rebuild (macOS, Ninja, Release)
#
#  Usage:
#    ./rebuild.sh              # incremental build (fastest, ~10-30s on a no-op)
#    ./rebuild.sh --clean      # wipe build/macos-release first, full reconfigure
#    ./rebuild.sh --run        # build then launch the .app
#    ./rebuild.sh --clean --run
#    ./rebuild.sh --jobs 4     # override parallelism (default: all cores)
#
#  Exit codes:
#    0   build succeeded (and app launched if --run)
#    1   configure/build failed
#    2   --run requested but .app not found after build
#
#  Assumes:
#    - Qt 6.8.3 at /Users/venkatsairamkadari/Qt/6.8.3/macos
#    - ninja + cmake in PATH (brew install cmake ninja)
#    - macOS host (uses sysctl for core count, open for --run)
# ═════════════════════════════════════════════════════════════════════════════

set -euo pipefail

# ── Resolve paths from this script's location so it works no matter where
# it's invoked from (relative cwd, GUI launcher, etc.)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_DIR/build/macos-release"
QT_DIR="${QT_DIR:-/Users/venkatsairamkadari/Qt/6.8.3/macos}"
APP_PATH="$BUILD_DIR/PinpunchTerminal.app"

# ── Args
CLEAN=0
RUN=0
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --run)   RUN=1;   shift ;;
        --jobs)  JOBS="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

# ── Color helpers (no-op if not a TTY)
if [[ -t 1 ]]; then
    BOLD=$'\033[1m'; DIM=$'\033[2m'; GREEN=$'\033[32m'; RED=$'\033[31m'; CYAN=$'\033[36m'; RESET=$'\033[0m'
else
    BOLD=""; DIM=""; GREEN=""; RED=""; CYAN=""; RESET=""
fi

say()  { echo "${CYAN}▶${RESET} ${BOLD}$*${RESET}"; }
ok()   { echo "${GREEN}✓${RESET} $*"; }
fail() { echo "${RED}✗${RESET} $*" >&2; }

# ── Sanity check tooling
for tool in cmake ninja; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        fail "$tool not found in PATH. Install: brew install cmake ninja"
        exit 1
    fi
done

if [[ ! -d "$QT_DIR" ]]; then
    fail "Qt not found at $QT_DIR"
    echo "  Set QT_DIR env var to override, e.g.: QT_DIR=/Users/me/Qt/6.8.3/macos ./rebuild.sh"
    exit 1
fi

# ── Clean if requested
if [[ "$CLEAN" -eq 1 ]]; then
    say "Wiping $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# ── Configure (only if needed — Ninja's build.ninja stamps reconfigure deps)
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    say "Configuring (cmake → Ninja, Release)"
    cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$QT_DIR" \
        -DQt6_DIR="$QT_DIR/lib/cmake/Qt6"
else
    echo "${DIM}(reusing existing build.ninja — ninja will reconfigure if needed)${RESET}"
fi

# ── Build
say "Building with $JOBS parallel jobs"
START_TS=$(date +%s)
if ! cmake --build "$BUILD_DIR" --parallel "$JOBS"; then
    fail "Build failed"
    exit 1
fi
END_TS=$(date +%s)
ok "Build finished in $((END_TS - START_TS))s"

# ── Report bundle + run
if [[ -d "$APP_PATH" ]]; then
    ok "App bundle: $APP_PATH"
    if [[ "$RUN" -eq 1 ]]; then
        say "Launching PinpunchTerminal.app"
        # Kill any running instance first so we know we're seeing the new build
        pkill -f "PinpunchTerminal.app/Contents/MacOS/PinpunchTerminal" 2>/dev/null || true
        sleep 0.3
        open "$APP_PATH"
    fi
else
    if [[ "$RUN" -eq 1 ]]; then
        fail ".app bundle not found at $APP_PATH after build — can't --run"
        exit 2
    fi
fi
