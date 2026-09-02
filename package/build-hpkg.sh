#!/usr/bin/env bash
#
# build-hpkg.sh — cross-build rtl8814au kernel driver and produce a .hpkg.
#
# Runs on a Linux machine with a configured Haiku cross-build tree.
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

# Cross-compile bits for the userland helper (wifi-join).  Same recipe
# as the in-tree Haiku app build — see reference_haiku_app_crossbuild.md
# in the dev notes for the lib-path / startfile-search-path gymnastics.
CROSS_BIN="$GENERATED/cross-tools-${HAIKU_ARCH}/bin"
CROSS_CC="$CROSS_BIN/${HAIKU_ARCH}-unknown-haiku-gcc"
HAIKU_DEVEL_LIB="$GENERATED/objects/haiku/${HAIKU_ARCH}/packaging/packages_build/regular/hpkg_-haiku_devel.hpkg/contents/develop/lib"
HAIKU_RUNTIME_LIB="$GENERATED/objects/haiku/${HAIKU_ARCH}/packaging/packages_build/regular/hpkg_-haiku.hpkg/contents/lib"

# Project paths
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$PROJ/src"
TOOLS_SRC="$PROJ/tools"
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

# Ensure the parent wlan Jamfile descends into our staged subdir.  Our
# standalone repo dropped rtl8814au from the upstream image defs so the
# SubInclude line was removed too; the build still needs jam to walk
# in here, so add the line locally on the build server if missing.
WLAN_JAMFILE="$HAIKU_BUILD/src/add-ons/kernel/drivers/network/wlan/Jamfile"
if [ -f "$WLAN_JAMFILE" ] && ! grep -q 'SubInclude HAIKU_TOP .* rtl8814au' "$WLAN_JAMFILE"; then
	echo "==> Adding SubInclude for rtl8814au to $WLAN_JAMFILE"
	echo "SubInclude HAIKU_TOP src add-ons kernel drivers network wlan rtl8814au ;" \
		>> "$WLAN_JAMFILE"
fi

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
# Build userland helper (wifi-join)
# ------------------------------------------------------------------
rm -rf "$BUILD"
mkdir -p "$BUILD"
if [ -f "$TOOLS_SRC/wifi-join.c" ]; then
	echo "==> Building wifi-join userland helper..."
	if [ ! -x "$CROSS_CC" ]; then
		echo "ERROR: cross compiler not found at $CROSS_CC" >&2
		exit 1
	fi
	mkdir -p "$BUILD/tools"
	"$CROSS_CC" \
		-I"$HAIKU_BUILD/headers" \
		-I"$HAIKU_BUILD/headers/posix" \
		-I"$HAIKU_BUILD/headers/os" \
		-I"$HAIKU_BUILD/headers/os/support" \
		-I"$HAIKU_BUILD/headers/os/kernel" \
		-I"$HAIKU_BUILD/headers/config" \
		-B"$HAIKU_DEVEL_LIB" \
		-L"$HAIKU_DEVEL_LIB" -L"$HAIKU_RUNTIME_LIB" \
		-Wl,-rpath-link,"$HAIKU_RUNTIME_LIB" \
		-O2 -Wall \
		"$TOOLS_SRC/wifi-join.c" \
		-lroot -lnetwork \
		-o "$BUILD/tools/wifi-join"
fi

# ------------------------------------------------------------------
# Stage the package tree
# ------------------------------------------------------------------
echo "==> Staging package tree..."
mkdir -p "$PKG_ROOT/add-ons/kernel/drivers/bin"
mkdir -p "$PKG_ROOT/add-ons/kernel/drivers/dev/net"
mkdir -p "$PKG_ROOT/data/firmware/rtl8814au"
mkdir -p "$PKG_ROOT/data/documentation/packages/rtl8814au"
mkdir -p "$PKG_ROOT/data/licenses"
mkdir -p "$PKG_ROOT/bin"

cp "$BUILT_BINARY" "$PKG_ROOT/add-ons/kernel/drivers/bin/rtl8814au"
chmod +x "$PKG_ROOT/add-ons/kernel/drivers/bin/rtl8814au"

if [ -f "$BUILD/tools/wifi-join" ]; then
	cp "$BUILD/tools/wifi-join" "$PKG_ROOT/bin/wifi-join"
	chmod +x "$PKG_ROOT/bin/wifi-join"
fi

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

# Ship the license under documentation/.
cp "$PROJ/LICENSE" \
	"$PKG_ROOT/data/documentation/packages/rtl8814au/LICENSE"

# Realtek's firmware licence must travel with the blob: it permits binary
# redistribution only if the copyright notice and disclaimer are reproduced
# with the distribution.  Shipping the driver's LICENSE alone is not enough,
# because the firmware is not covered by it.
#
# It goes in two places, and data/licenses/ is the one that is mandatory.
# `package create` validates every entry in PackageInfo's `licenses` list
# against the package's OWN contents, not against the host system: a name it
# does not recognise as one of its built-in standard licences must exist at
# data/licenses/<exact name> or the build is refused outright with
# "License '...' isn't contained in package!".  "GNU GPL v2" is recognised;
# "Realtek WiFi Firmware" is not, even though an installed Haiku carries that
# licence at /system/data/licenses/Realtek WiFi Firmware.
#
# The filename must therefore match the declared name character for character,
# spaces included.  The documentation copy is the one a user is likely to open,
# and having it there means the notice travels with the .hpkg rather than
# depending on what the host system happens to have installed -- which is what
# Realtek's terms actually require.
cp "$PROJ/firmware/LICENCE.rtlwifi_firmware.txt" \
	"$PKG_ROOT/data/licenses/Realtek WiFi Firmware"
cp "$PROJ/firmware/LICENCE.rtlwifi_firmware.txt" \
	"$PKG_ROOT/data/documentation/packages/rtl8814au/LICENCE.rtlwifi_firmware.txt"

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
