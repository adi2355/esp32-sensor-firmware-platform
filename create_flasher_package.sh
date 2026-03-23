#!/bin/bash

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"



if [ -n "$1" ]; then
    VERSION="$1"
else
    MAJOR=$(grep -oP 'FIRMWARE_VERSION_MAJOR\s+\K\d+' src/Config.h | head -1)
    MINOR=$(grep -oP 'FIRMWARE_VERSION_MINOR\s+\K\d+' src/Config.h | head -1)
    PATCH=$(grep -oP 'FIRMWARE_VERSION_PATCH\s+\K\d+' src/Config.h | head -1)
    VERSION="${MAJOR}.${MINOR}.${PATCH}"
fi

echo -e "${CYAN}Creating flasher package for version: ${VERSION}${NC}"
echo ""

BUILD_DIR=".pio/build/xiao_esp32s3"

if [ ! -f "$BUILD_DIR/firmware.bin" ]; then
    echo -e "${YELLOW}Step 1: Building firmware first...${NC}"
    pio run -e xiao_esp32s3
else
    echo -e "${GREEN}Step 1: Using existing build${NC}"
    echo "  (Run 'pio run' first if you want fresh binaries)"
fi

if [ ! -f "$BUILD_DIR/firmware.bin" ]; then
    echo -e "${RED}ERROR: firmware.bin not found!${NC}"
    exit 1
fi

echo -e "${YELLOW}Step 2: Copying binaries to flasher-package...${NC}"

mkdir -p flasher-package/binaries
mkdir -p flasher-package/src

cp "$BUILD_DIR/bootloader.bin" flasher-package/binaries/
cp "$BUILD_DIR/partitions.bin" flasher-package/binaries/
cp "$BUILD_DIR/firmware.bin" flasher-package/binaries/

echo "  ✓ bootloader.bin ($(du -h flasher-package/binaries/bootloader.bin | cut -f1))"
echo "  ✓ partitions.bin ($(du -h flasher-package/binaries/partitions.bin | cut -f1))"
echo "  ✓ firmware.bin ($(du -h flasher-package/binaries/firmware.bin | cut -f1))"

echo -e "${YELLOW}Step 3: Verifying package files...${NC}"

if [ ! -f "flasher-package/platformio.ini" ]; then
    echo -e "${RED}ERROR: flasher-package/platformio.ini missing!${NC}"
    exit 1
fi

if [ ! -f "flasher-package/flash_prebuilt.py" ]; then
    echo -e "${RED}ERROR: flasher-package/flash_prebuilt.py missing!${NC}"
    exit 1
fi

if [ ! -f "flasher-package/src/dummy.cpp" ]; then
    echo "// Dummy file for PlatformIO" > flasher-package/src/dummy.cpp
    echo "void setup() {}" >> flasher-package/src/dummy.cpp
    echo "void loop() {}" >> flasher-package/src/dummy.cpp
fi

echo "  ✓ platformio.ini"
echo "  ✓ flash_prebuilt.py"
echo "  ✓ src/dummy.cpp"
echo "  ✓ README.md"

echo -e "${YELLOW}Step 4: Creating ZIP package...${NC}"

ZIP_NAME="firmware-pio-flasher-v${VERSION}.zip"

rm -f "$ZIP_NAME"

cd flasher-package
zip -r "../$ZIP_NAME" \
    platformio.ini \
    flash_prebuilt.py \
    README.md \
    binaries/ \
    src/
cd ..


echo -e "${CYAN}Package:${NC} $ZIP_NAME"
ls -lh "$ZIP_NAME" | awk '{print "Size: " $5}'
echo ""
echo -e "${CYAN}Contents:${NC}"
unzip -l "$ZIP_NAME" | tail -n +4 | head -n -2
echo ""
echo -e "${YELLOW}Usage:${NC}"
echo "  1. Extract the ZIP"
echo "  2. cd into the extracted folder"
echo "  3. Run: pio run -t upload"
echo "  4. Or:  pio run -t erase && pio run -t upload && pio device monitor"
echo ""
echo -e "${GREEN}Ready for distribution!${NC}"

