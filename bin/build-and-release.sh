#!/bin/bash
# build-and-release.sh: Build all PlatformIO targets and collect firmware files into a flat .pio/release directory for S3 upload
set -e

RELEASE_DIR=".pio/release"

# Clean previous release dir
rm -rf "$RELEASE_DIR"
#startFromTarget='tlora-c6'
mkdir -p "$RELEASE_DIR"

# Build all PlatformIO environments
# Get list of targets from release_json_template.json
TARGETS=$(jq -r '.targets[].board' bin/release_json_template.json)

echo "Building targets one by one and cleaning up after each..."

for ENV in $TARGETS; do
  echo "Building $ENV..."

  if [ -n "$startFromTarget" ]; then
    if [ "$ENV" != "$startFromTarget" ]; then
      echo "Skipping $ENV until we reach $startFromTarget..."
      continue
    else
      echo "Starting from target $startFromTarget..."
      startFromTarget=""
    fi
  fi

  platformio run -e "$ENV"

  BUILD_DIR=".pio/build/$ENV"
  if [ -d "$BUILD_DIR" ]; then
    echo "Collecting firmware files for $ENV..."
    find "$BUILD_DIR" -type f \
      \( -name 'firmware-*.bin' \
      -o -name 'firmware-*.uf2' \
      -o -name 'firmware-*.hex' \
      -o -name 'firmware-*.zip' \
      -o -name 'device-*.sh' \
      -o -name 'device-*.bat' \
      -o -name 'littlefs-*.bin' \
      -o -name 'bleota*bin' \
      -o -name 'Meshtastic_nRF52_factory_erase*.uf2' \
      -o -name '*.elf' \
      -o -name '*.mt.json' \
      \) | while read f; do
      base=$(basename "$f")
      echo "Copying $f -> $RELEASE_DIR/${ENV}_$base"
      cp "$f" "$RELEASE_DIR/${ENV}_$base"
    done

    echo "Cleaning up $BUILD_DIR, keeping only firmware files..."
    find "$BUILD_DIR" -type f \
      ! \( -name 'firmware-*.bin' \
      -o -name 'firmware-*.uf2' \
      -o -name 'firmware-*.hex' \
      -o -name 'firmware-*.zip' \
      -o -name 'device-*.sh' \
      -o -name 'device-*.bat' \
      -o -name 'littlefs-*.bin' \
      -o -name 'bleota*bin' \
      -o -name 'Meshtastic_nRF52_factory_erase*.uf2' \
      -o -name '*.elf' \
      -o -name '*.mt.json' \
      \) -exec rm -f {} +
  fi
done

echo "All firmware files are in $RELEASE_DIR, ready for S3 upload."
