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

echo -e "${BLUE}============================================${NC}"
echo -e "${BLUE}  Firmware Release Builder${NC}"
echo -e "${BLUE}============================================${NC}"
echo ""

if [ -n "$1" ]; then
    VERSION="$1"
else
    MAJOR=$(grep -oP 'FIRMWARE_VERSION_MAJOR\s+\K\d+' src/Config.h | head -1)
    MINOR=$(grep -oP 'FIRMWARE_VERSION_MINOR\s+\K\d+' src/Config.h | head -1)
    PATCH=$(grep -oP 'FIRMWARE_VERSION_PATCH\s+\K\d+' src/Config.h | head -1)
    VERSION="${MAJOR}.${MINOR}.${PATCH}"
fi

echo -e "${CYAN}Building firmware version: ${VERSION}${NC}"
echo ""

echo -e "${YELLOW}Step 1: Cleaning previous build...${NC}"
pio run --target clean -e xiao_esp32s3 2>/dev/null || true

echo -e "${YELLOW}Step 2: Building release firmware...${NC}"
pio run -e xiao_esp32s3

BUILD_DIR=".pio/build/xiao_esp32s3"
if [ ! -f "$BUILD_DIR/firmware.bin" ]; then
    echo -e "${RED}ERROR: Build failed! firmware.bin not found.${NC}"
    exit 1
fi

echo -e "${GREEN}Build successful!${NC}"
echo ""

echo -e "${YELLOW}Step 3: Copying binaries to dist/...${NC}"

mkdir -p dist

cp "$BUILD_DIR/bootloader.bin" dist/
cp "$BUILD_DIR/partitions.bin" dist/
cp "$BUILD_DIR/firmware.bin" dist/

echo ""
echo -e "${CYAN}Binary files created:${NC}"
ls -lh dist/*.bin | awk '{print "  " $9 ": " $5}'
echo ""

echo -e "${YELLOW}Step 4: Updating version strings...${NC}"

sed -i "s/Flasher v[0-9]\+\.[0-9]\+\.[0-9]\+/Flasher v${VERSION}/g" dist/flash.sh 2>/dev/null || true

sed -i "s/Flasher v[0-9]\+\.[0-9]\+\.[0-9]\+/Flasher v${VERSION}/g" dist/flash.bat 2>/dev/null || true

sed -i "s/Firmware v[0-9]\+\.[0-9]\+\.[0-9]\+/Firmware v${VERSION}/g" dist/README.md 2>/dev/null || true
sed -i "s/Firmware Version\*\*: [0-9]\+\.[0-9]\+\.[0-9]\+/Firmware Version**: ${VERSION}/g" dist/README.md 2>/dev/null || true

echo -e "${YELLOW}Step 5: Creating distribution ZIP...${NC}"

ZIP_NAME="firmware-release-v${VERSION}.zip"

rm -f "$ZIP_NAME"

cd dist
zip -r "../$ZIP_NAME" \
    bootloader.bin \
    partitions.bin \
    firmware.bin \
    flash.sh \
    flash.bat \
    README.md
cd ..

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Release Build Complete!${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""
echo -e "${CYAN}Distribution package:${NC}"
echo "  $ZIP_NAME"
ls -lh "$ZIP_NAME" | awk '{print "  Size: " $5}'
echo ""
echo -e "${CYAN}Contents (NO SOURCE CODE):${NC}"
unzip -l "$ZIP_NAME" | tail -n +4 | head -n -2
echo ""
echo -e "${YELLOW}IMPORTANT:${NC}"
echo "  - The ZIP contains ONLY compiled binaries"
echo "  - Source code (.cpp, .h) is NOT included"
echo "  - Users can flash but cannot extract your code"
echo ""
echo -e "${GREEN}Ready for distribution!${NC}"

