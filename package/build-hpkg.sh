#!/usr/bin/env bash
#
# build-hpkg.sh — cross-build rtl8814au kernel driver and produce a .hpkg.
#
# Runs on the Haiku Linux cross-build server (kevin@192.168.74.122).
# Expects the project tree (src/, firmware/, package/) at the parent dir.
#
# How it works: the driver's Jamfile assumes it lives inside a Haiku
# source tree (uses HAIKU_TOP relative paths, KernelAddon target).
# We could re-implement that logic standalone, but the simplest and
# most maintainable approach is to "stage" our source files into a
# real Haiku checkout and let jam handle the kernel-addon link.
#
# Output: build/rtl8814au-<version>-<arch>.hpkg
#
# Override paths via env vars if needed:
#   HAIKU_BUILD   — top of the haiku source checkout (default: ~/haiku-build/haiku)
#   HAIKU_ARCH    — target arch (default: x86_64)

set -euo pipefail

HAIKU_BUILD="${HAIKU_BUILD:-$HOME/haiku-build/haiku}"
HAIKU_ARCH="${HAIKU_ARCH:-x86_64}"

GENERATED="$HAIKU_BUILD/generated.${HAIKU_ARCH}"
HOST_TOOLS="$GENERATED/objects/linux/${HAIKU_ARCH}/release/tools"
PACKAGE="$HOST_TOOLS/package/package"

# Project paths
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$PROJ/src"
FIRMWARE_SRC="$PROJ/firmware"
BUILD="$PROJ/build"
PKG_ROOT="$BUILD/package_root"
PKG_INFO_TEMPLATE="$PROJ/package/PackageInfo"

# Staging path inside the Haiku tree.  jam needs the source here to
# satisfy the SubDir / KernelAddon machinery.
HAIKU_DRIVER_DIR="$HAIKU_BUILD/src/add-ons/kernel/drivers/network/wlan/rtl8814au"
HAIKU_FIRMWARE_DIR="$HAIKU_BUILD/data/system/data/firmware/rtl8814au"
BUILT_BINARY="$GENERATED/objects/haiku/${HAIKU_ARCH}/release/add-ons/kernel/drivers/network/wlan/rtl8814au/rtl8814au"

# ------------------------------------------------------------------
# Sanity checks
# ------------------------------------------------------------------
if [ ! -x "$PACKAGE" ]; then
	echo "ERROR: package tool not found at $PACKAGE" >&2
	echo "       Build the Haiku tree first: cd $GENERATED && jam -q -j4 \\<build\\>haiku.hpkg" >&2
	exit 1
fi

if [ ! -d "$GENERATED" ]; then
	echo "ERROR: $GENERATED does not exist; this needs a configured Haiku build tree." >&2
	exit 1
fi

# ------------------------------------------------------------------
# Stage source + firmware into the Haiku tree
# ------------------------------------------------------------------
echo "==> Staging source into $HAIKU_DRIVER_DIR"
mkdir -p "$HAIKU_DRIVER_DIR"
# Wipe stale files so a renamed/removed source in our repo doesn't
# linger and silently get linked from the previous build.
find "$HAIKU_DRIVER_DIR" -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.h' -o -name 'Jamfile' \) -delete
cp "$SRC"/*.cpp "$SRC"/*.h "$SRC"/Jamfile "$HAIKU_DRIVER_DIR"/

echo "==> Staging firmware into $HAIKU_FIRMWARE_DIR"
mkdir -p "$HAIKU_FIRMWARE_DIR"
cp "$FIRMWARE_SRC"/rtl8814aufw.bin "$HAIKU_FIRMWARE_DIR"/

# ------------------------------------------------------------------
# Build via jam (kernel addon)
# ------------------------------------------------------------------
echo "==> Building kernel addon via jam..."
( cd "$GENERATED" && jam -q -j4 rtl8814au )

if [ ! -f "$BUILT_BINARY" ]; then
	echo "ERROR: jam reported success but built binary not at $BUILT_BINARY" >&2
	exit 1
fi

# ------------------------------------------------------------------
# Stage the package tree
# ------------------------------------------------------------------
echo "==> Staging package tree..."
rm -rf "$BUILD"
mkdir -p "$BUILD"
mkdir -p "$PKG_ROOT/add-ons/kernel/drivers/bin"
mkdir -p "$PKG_ROOT/add-ons/kernel/drivers/dev/net"
mkdir -p "$PKG_ROOT/data/firmware/rtl8814au"
mkdir -p "$PKG_ROOT/data/documentation/packages/rtl8814au"

cp "$BUILT_BINARY" "$PKG_ROOT/add-ons/kernel/drivers/bin/rtl8814au"
chmod +x "$PKG_ROOT/add-ons/kernel/drivers/bin/rtl8814au"

# Haiku's driver lookup walks dev/net/<name> and expects each entry to
# be a symlink to the actual binary under bin/.  Relative path so the
# package is location-independent.
ln -sf ../../bin/rtl8814au \
	"$PKG_ROOT/add-ons/kernel/drivers/dev/net/rtl8814au"

# Firmware blob — driver reads from /system/data/firmware/rtl8814au/
# which on a packaged install is the union mount of every package's
# data/firmware/rtl8814au/ subtree.
cp "$FIRMWARE_SRC"/rtl8814aufw.bin \
	"$PKG_ROOT/data/firmware/rtl8814au/rtl8814aufw.bin"

# Ship the MIT license under documentation/.
cp "$PROJ/LICENSE" \
	"$PKG_ROOT/data/documentation/packages/rtl8814au/LICENSE"

# .PackageInfo lives at the package root.
cp "$PKG_INFO_TEMPLATE" "$PKG_ROOT/.PackageInfo"

# ------------------------------------------------------------------
# Build the .hpkg
# ------------------------------------------------------------------
VERSION=$(awk '/^version/ { gsub(/[ \t]+/, " "); print $2 }' "$PKG_INFO_TEMPLATE")
HPKG="$BUILD/rtl8814au-${VERSION}-${HAIKU_ARCH}.hpkg"

echo "==> Creating $HPKG..."
rm -f "$HPKG"
( cd "$PKG_ROOT" && "$PACKAGE" create -q "$HPKG" )

echo
echo "==> Done"
echo "    $HPKG"
ls -lh "$HPKG"
