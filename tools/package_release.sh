#!/usr/bin/env bash
# Package the already-built firmware of every board into the extra assets a
# GitHub Release publishes for browser flashing (see .releaserc.json), all
# written to dist/:
#
#   manifest-<board>.json
#       ESP Web Tools manifest for one board. ESP Web Tools picks a build by
#       chip family only, and both boards are ESP32-S3, so each board needs
#       its own manifest rather than two builds in one.
#   bootloader-<board>.bin, partitions-<board>.bin
#       per-board parts (the bootloader header carries the flash size, which
#       differs: 8MB on the 1.54, 16MB on the 3.97)
#   boot_app0.bin
#       shared by both boards
#
# The application images themselves are the release's existing firmware.bin
# (1.54) and firmware-epaper397.bin assets, uploaded straight from .pio/build;
# the manifests reference them by those asset names. Every manifest part path
# is a flat asset name, so a site mirroring the release assets into one folder
# (annota-ws's tools/sync_firmware.sh) can serve the manifests unmodified.
#
# Usage: pio run -e esp32-s3-epaper154 && pio run -e esp32-s3-epaper397 \
#          && tools/package_release.sh
# In CI it runs from semantic-release's prepareCmd right after the builds, once
# include/version.h already holds the new version.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

OUT_DIR="dist"

# Offsets for the ESP32-S3 (second-stage bootloader at 0x0, unlike the
# original ESP32's 0x1000) and this project's partition table (huge_app.csv,
# see platformio.ini: otadata at 0xe000, app0 at 0x10000). If the board family
# or partition table ever change, these need to change with them.
OFF_BOOTLOADER=0
OFF_PARTITIONS=32768   # 0x8000
OFF_BOOT_APP0=57344    # 0xe000
OFF_APP=65536          # 0x10000

# boot_app0.bin isn't a build output: it's a small static "boot slot 0" otadata
# image shipped with the arduino-esp32 framework, needed because huge_app.csv
# has an otadata partition.
CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
BOOT_APP0="$CORE_DIR/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
if [[ ! -f "$BOOT_APP0" ]]; then
  echo "error: boot_app0.bin not found at $BOOT_APP0 (framework package missing?)" >&2
  exit 1
fi

VERSION="$(sed -n 's/^#define FW_VERSION "\([^"]*\)".*/\1/p' include/version.h)"
if [[ -z "$VERSION" ]]; then
  echo "error: couldn't parse FW_VERSION out of include/version.h" >&2
  exit 1
fi

# check_part <part> <factory image> <offset>: fails unless <part> sits
# byte-for-byte at <offset> in pioarduino's merged firmware.factory.bin, i.e.
# the offsets above still match what the build itself laid out.
check_part() {
  local size
  size=$(wc -c < "$1")
  # Process substitution, not a pipe into cmp: head exiting early SIGPIPEs
  # tail, which pipefail would report as a mismatch.
  if ! cmp -s <(tail -c +$(( $3 + 1 )) "$2" | head -c "$size") "$1"; then
    echo "error: $1 doesn't match $2 at offset $3 (offsets out of date?)" >&2
    exit 1
  fi
}

# package <env> <board key> <app asset name> <display name>
package() {
  local env="$1" board="$2" app="$3" name="$4"
  local build=".pio/build/$env"

  for f in bootloader.bin partitions.bin firmware.bin firmware.factory.bin; do
    if [[ ! -f "$build/$f" ]]; then
      echo "error: $build/$f not found (run 'pio run -e $env' first)" >&2
      exit 1
    fi
  done
  check_part "$build/bootloader.bin" "$build/firmware.factory.bin" "$OFF_BOOTLOADER"
  check_part "$build/partitions.bin" "$build/firmware.factory.bin" "$OFF_PARTITIONS"
  check_part "$BOOT_APP0" "$build/firmware.factory.bin" "$OFF_BOOT_APP0"
  check_part "$build/firmware.bin" "$build/firmware.factory.bin" "$OFF_APP"

  cp "$build/bootloader.bin" "$OUT_DIR/bootloader-$board.bin"
  cp "$build/partitions.bin" "$OUT_DIR/partitions-$board.bin"

  # new_install_improv_wait_time 0: the firmware doesn't speak Improv Serial
  # (WiFi is set up through WiFiManager's AP), so don't make the user wait
  # for it after flashing.
  cat > "$OUT_DIR/manifest-$board.json" <<EOF
{
  "name": "$name",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bootloader-$board.bin", "offset": $OFF_BOOTLOADER },
        { "path": "partitions-$board.bin", "offset": $OFF_PARTITIONS },
        { "path": "boot_app0.bin", "offset": $OFF_BOOT_APP0 },
        { "path": "$app", "offset": $OFF_APP }
      ]
    }
  ]
}
EOF
}

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
cp "$BOOT_APP0" "$OUT_DIR/boot_app0.bin"

package esp32-s3-epaper154 epaper154 firmware.bin "Annota (ESP32-S3-ePaper-1.54)"
package esp32-s3-epaper397 epaper397 firmware-epaper397.bin "Annota (ESP32-S3-ePaper-3.97)"

echo "Packaged Annota $VERSION into $OUT_DIR/:"
ls -l "$OUT_DIR"
