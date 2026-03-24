#!/usr/bin/env bash
set -euo pipefail

# Build pipeline for MIDI PLAYER 64.
# 1) Pick a host-side .mid file.
# 2) Compile player.prg with Oscar64.
# 3) Package player + MIDI into a ready-to-boot D64.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
ARTIFACT_DIR="$ROOT_DIR/artifacts"
ARTIFACT_PRG="$ARTIFACT_DIR/player.prg"
PLAYER_PRG="$BUILD_DIR/player.prg"
MIDI_DISK_FILE="$BUILD_DIR/MIDI.PRG"
DISK_IMAGE="$BUILD_DIR/midi-player-64.d64"
SELECT_ARG="${1:-}"

mkdir -p "$BUILD_DIR"
mkdir -p "$ARTIFACT_DIR"

# Keep build outputs deterministic: remove stale artifacts from both
# current and legacy naming schemes so users always get a fresh disk.
rm -f \
	"$BUILD_DIR"/midi-player-64* \
	"$BUILD_DIR"/midi_sid_player* \
	"$BUILD_DIR"/midi_sid_scan* \
	"$BUILD_DIR"/player.prg \
	"$BUILD_DIR"/player.asm \
	"$BUILD_DIR"/player.int \
	"$BUILD_DIR"/player.lbl \
	"$BUILD_DIR"/player.map \
	"$BUILD_DIR"/PLAYER.PRG \
	"$BUILD_DIR"/scan.prg \
	"$BUILD_DIR"/scan.asm \
	"$BUILD_DIR"/scan.int \
	"$BUILD_DIR"/scan.lbl \
	"$BUILD_DIR"/scan.map \
	"$BUILD_DIR"/SCAN.PRG \
	"$BUILD_DIR"/MIDI_SCAN.PRG \
	"$BUILD_DIR"/MIDISCAN.PRG \
	"$BUILD_DIR"/MIDI.PRG

# Discover selectable MIDI inputs in project root. Case-insensitive
# globbing helps when files come from different host platforms.
shopt -s nullglob nocaseglob
MID_FILES=( "$ROOT_DIR"/*.mid "$ROOT_DIR"/*.midi )
shopt -u nocaseglob

if [[ "$SELECT_ARG" == "--help" || "$SELECT_ARG" == "-h" ]]; then
	echo "Usage:"
	echo "  ./c64build                # show MIDI selection menu and build d64"
	echo "  ./c64build --list         # list MIDI files and exit"
	echo "  ./c64build <index>        # pick MIDI by 1-based index"
	echo "  ./c64build <filename.mid> # pick MIDI by filename"
	exit 0
fi

if [[ "$SELECT_ARG" == "--list" ]]; then
  if (( ${#MID_FILES[@]} == 0 )); then
    echo "No .mid/.midi found in $ROOT_DIR"
  else
    for i in "${!MID_FILES[@]}"; do
      printf "%2d) %s\n" "$((i + 1))" "${MID_FILES[$i]}"
    done
  fi
  exit 0
fi

SELECTED_MID_FILES=()
if (( ${#MID_FILES[@]} == 0 )); then
  echo "No .mid/.midi found in $ROOT_DIR"
  exit 1
fi

# Selection can be interactive (menu) or scripted (index/name).
# This keeps manual workflows and CI-style automation both simple.
if [[ -n "$SELECT_ARG" ]]; then
  if [[ "$SELECT_ARG" =~ ^[0-9]+$ ]] && (( SELECT_ARG >= 1 && SELECT_ARG <= ${#MID_FILES[@]} )); then
    SELECTED_MID_FILES=( "${MID_FILES[$((SELECT_ARG - 1))]}" )
  else
    found=0
    for f in "${MID_FILES[@]}"; do
      if [[ "$SELECT_ARG" == "$(basename "$f")" || "$SELECT_ARG" == "$f" ]]; then
        SELECTED_MID_FILES=( "$f" )
        found=1
        break
      fi
    done
    if (( found == 0 )); then
      echo "Invalid MIDI selection: $SELECT_ARG"
      exit 1
    fi
  fi
else
  echo "MIDI files found in project folder:"
  for i in "${!MID_FILES[@]}"; do
    printf " %2d) %s\n" "$((i + 1))" "${MID_FILES[$i]}"
  done

  if [[ ! -t 0 ]]; then
    echo "No interactive input available. Use: ./c64build <index|filename.mid>"
    exit 1
  fi

  while true; do
    read -r -p "Choose MIDI to include (1-${#MID_FILES[@]}) [1]: " choice
    choice="${choice:-1}"
    if [[ "$choice" =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#MID_FILES[@]} )); then
      SELECTED_MID_FILES=( "${MID_FILES[$((choice - 1))]}" )
      break
    fi
		echo "Invalid choice, retry."
	done
fi

# The chosen MIDI is copied with a fixed C64-side name for robust loading.
cp "${SELECTED_MID_FILES[0]}" "$MIDI_DISK_FILE"

# Compile + package in one Oscar64 invocation, then refresh the tracked
# standalone artifact so c64disk can work without a local rebuild.
oscar64 -tm=c64 -O2 -dDISK_SCAN_MODE=1 -o="$PLAYER_PRG" -d64="$DISK_IMAGE" -f="$MIDI_DISK_FILE" "$ROOT_DIR/src/main.c"
cp "$PLAYER_PRG" "$ARTIFACT_PRG"
echo "Disk OK:  $DISK_IMAGE"
FRESH_DISK_IMAGE="$BUILD_DIR/midi-player-64_fresh_$(date +%Y%m%d_%H%M%S).d64"
cp "$DISK_IMAGE" "$FRESH_DISK_IMAGE"
echo "Disk fresh: $FRESH_DISK_IMAGE"
echo "MIDI included in disk:"
echo " - ${SELECTED_MID_FILES[0]}"
echo " - as C64 file name: MIDI"
echo "Build OK: $PLAYER_PRG"
echo "Artifact: $ARTIFACT_PRG"
echo "Launch on C64: LOAD\"*\",8,1 then RUN"
