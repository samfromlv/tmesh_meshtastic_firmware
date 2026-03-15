#!/bin/bash
# build-and-release.sh: Build all PlatformIO targets and collect firmware files into a flat .pio/release directory for S3 upload
set -e

RELEASE_DIR=".pio/release"

# Clean previous release dir
rm -rf "$RELEASE_DIR"
#startFromTarget='tlora-c6'
mkdir -p "$RELEASE_DIR"
skipBuild=false
# Build all PlatformIO environments
# Get list of targets from release_json_template.json

TARGETS=$(jq -r '.targets[].board' bin/release_json_template.json)

# Check that all targets exist in PlatformIO environments
MISSING_ENVS=""
for ENV in $TARGETS; do
  if ! platformio run --list-targets -e "$ENV" >/dev/null 2>&1; then
    echo "Error: PlatformIO environment '$ENV' does not exist!"
    MISSING_ENVS="$MISSING_ENVS $ENV"
  else
    echo "Found PlatformIO environment '$ENV', ready to build."
  fi
done

if [ -n "$MISSING_ENVS" ]; then
  echo "\nThe following environments are missing in PlatformIO configuration:$MISSING_ENVS"
  echo "Aborting build. Please check your release_json_template.json and platformio.ini."
  exit 1
fi

echo "Building targets one by one and cleaning up after each..."


# Read extra build flags from build-flags-extra.txt and set PLATFORMIO_BUILD_FLAGS
EXTRA_FLAGS_FILE="$(dirname "$0")/build-flags-extra.txt"
if [ -f "$EXTRA_FLAGS_FILE" ]; then
  export PLATFORMIO_BUILD_FLAGS="$(cat "$EXTRA_FLAGS_FILE")"
else
  unset PLATFORMIO_BUILD_FLAGS
fi

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

  if [ "$skipBuild" = true ]; then
    echo "Skipping build for $ENV, just collecting files..."
  else
    echo "Running build for $ENV..."
    platformio run -e "$ENV"
  fi

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
      echo "Copying $f -> $RELEASE_DIR/$base"
      cp "$f" "$RELEASE_DIR/$base"
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

echo "All firmware files are in $RELEASE_DIR, ready for S3 upload using bin/upload-release.sh."
