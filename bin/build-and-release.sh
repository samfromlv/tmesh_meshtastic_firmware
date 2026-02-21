#!/bin/bash
# build-and-release.sh: Build all PlatformIO targets and collect firmware files into a flat .pio/release directory for S3 upload
set -e

RELEASE_DIR=".pio/release"

# Clean previous release dir
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

# Build all PlatformIO environments
echo "Building all PlatformIO targets..."
platformio run

echo "Collecting firmware files..."

# Find all relevant firmware and manifest files in .pio/build/*
find .pio/build -type f \
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
  # Flatten: copy as envname_filename.ext
  base=$(basename "$f")
  echo "Copying $f -> $RELEASE_DIR/$base"
  cp "$f" "$RELEASE_DIR/$base"
done

echo "All firmware files are in $RELEASE_DIR, ready for S3 upload."
