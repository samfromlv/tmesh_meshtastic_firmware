#!/bin/bash
# Ask version
read -p "Enter version: " version

sourceDir=".pio/download/$version"
RELEASE_DIR=".pio/release"
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

#copy all files from sourceDir to tmp
TMP_DIR=".pio/tmp"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"
cp -r "$sourceDir"/* "$TMP_DIR/"

TARGETS=$(jq -r '.targets[].board' bin/release_json_template.json | awk '{ print length, $0 }' | sort -rn | cut -d' ' -f2-)

for ENV in $TARGETS; do
  if [ -d "$sourceDir" ]; then
    echo "Collecting firmware files for $ENV..."
    find "$TMP_DIR" -type f \
      \( -name "${ENV}_firmware-*.bin" \
      -o -name "${ENV}_firmware-*.uf2" \
      -o -name "${ENV}_firmware-*.hex" \
      -o -name "${ENV}_firmware-*.zip" \
      -o -name "${ENV}_device-*.sh" \
      -o -name "${ENV}_device-*.bat" \
      -o -name "${ENV}_littlefs-*.bin" \
      -o -name "${ENV}_bleota*bin" \
      -o -name "${ENV}_Meshtastic_nRF52_factory_erase*.uf2" \
      -o -name "${ENV}_*.elf" \
      -o -name "${ENV}_*.mt.json" \
      \) | while read f; do
      base=$(basename "$f")
      echo "Copying $f -> $RELEASE_DIR/$base"
        # Remove env from name and copy to release dir
        # Special handling for firmware files: remove leading board name
        if [[ "$base" =~ ^${ENV}_(.*) ]]; then
          newname="${BASH_REMATCH[1]}"
          cp "$f" "$RELEASE_DIR/$newname"
        else
          cp "$f" "$RELEASE_DIR/$base"
        fi

        #remove file from tmp
        rm "$f"
    done

  fi
done