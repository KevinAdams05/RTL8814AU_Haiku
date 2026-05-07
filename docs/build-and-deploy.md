>[!NOTE]
>An LLM was used to aid in development of this code.

# Building and deploying

The driver currently builds inside a full Haiku source tree as a kernel
add-on, cross-compiled from a Linux host.  A standalone build that
doesn't require the Haiku source is on the roadmap (see
[known-issues.md](known-issues.md)).

## Build server setup

We develop on a Linux host with a Haiku source checkout and the Haiku
cross-tools:

```
~/haiku-build/haiku/                        # Haiku source tree
~/haiku-build/haiku/generated.x86_64/       # build output
~/haiku-build/haiku/generated.x86_64/cross-tools-x86_64/bin/
                                             # cross-compiler
```

Driver source lives at:

```
src/add-ons/kernel/drivers/network/wlan/rtl8814au/
   Driver.cpp / Device.cpp / RegisterIO.cpp / Firmware.cpp
   EfuseReader.cpp / PhyConfig.cpp / TxPath.cpp / RxPath.cpp
   WiFiManagement.cpp / WPA2Crypto.cpp
   WiFiIoctl.h / Device.h / WPA2Crypto.h / RTL8814AU.h
   Jamfile
```

Once the standalone build lands, this layout will move to the project
repo (`C:\Code\Haiku\rtl8814au\src\` or similar) and ship as a
`.hpkg` plus build instructions.

## Building the driver

From the build server:

```
cd ~/haiku-build/haiku/generated.x86_64
jam -q -j8 rtl8814au
```

Output: `objects/haiku/x86_64/release/add-ons/kernel/drivers/network/wlan/rtl8814au/rtl8814au` (~150 KB binary).

## Deploying to the test box

The test box is a separate Haiku machine accessed by SSH key auth.

```
scp <build-server>:.../rtl8814au \
    user@<test-box>:/boot/home/config/non-packaged/add-ons/kernel/drivers/bin/rtl8814au
ssh user@<test-box> '/boot/home/reboot.sh'
```

The kernel's directory watcher does **not** swap a live driver — even
after dropping a new binary, the in-memory module stays old.  Reboot is
the reliable way to load a new driver build.

### Driver location priorities

Haiku looks for kernel drivers at three priority levels:

| Priority | Path |
|---|---|
| 2 (wins) | `/boot/home/config/non-packaged/add-ons/kernel/drivers/bin/rtl8814au` |
| 0 | `/boot/system/non-packaged/add-ons/kernel/drivers/bin/rtl8814au` |
| 0 | `/boot/system/add-ons/kernel/drivers/bin/rtl8814au` (packagefs, read-only) |

The symlink `dev/net/rtl8814au -> ../../bin/rtl8814au` must exist
alongside the binary in the same priority path or the driver won't be
published as a network device.

> **Gotcha:** if both the priority-2 binary and the read-only packagefs
> binary are present, the priority-2 wins on most boots — but not all.
> When the older packagefs driver wins (e.g., it claimed the USB device
> first during enumeration), the new code doesn't run.  Either bump the
> haiku.hpkg version (instructions below) or remove the old non-packaged
> copies before iterating.

## Cross-building userland tools

Tools (`wpa2_join`, `eapol_sniff`) are cross-built using the same Haiku
cross-compiler.  Recipe:

```bash
HAIKU=~/haiku-build/haiku
GEN=$HAIKU/generated.x86_64
CC=$GEN/cross-tools-x86_64/bin/x86_64-unknown-haiku-gcc
DEVEL_LIB=$GEN/objects/haiku/x86_64/packaging/packages_build/regular/hpkg_-haiku_devel.hpkg/contents/develop/lib
RUNTIME_LIB=$GEN/objects/haiku/x86_64/packaging/packages_build/regular/hpkg_-haiku.hpkg/contents/lib

$CC -o wpa2_join wpa2_join.c \
    -I$HAIKU/headers \
    -I$HAIKU/headers/posix \
    -I$HAIKU/headers/os \
    -I$HAIKU/headers/os/support \
    -I$HAIKU/headers/os/kernel \
    -I$HAIKU/headers/config \
    -B$DEVEL_LIB -L$DEVEL_LIB -L$RUNTIME_LIB \
    -Wl,-rpath-link,$RUNTIME_LIB \
    -lroot -lnetwork
```

Notes:

- `-B"$DEVEL_LIB"` is **not** the same as `-L"$DEVEL_LIB"`.  The cross
  compiler's spec file looks for CRT files (`crti.o`, `start_dyn.o`)
  via the *startfile* search path which `-B` extends and `-L` doesn't.
- The DEVEL_LIB / RUNTIME_LIB split is deliberate: `develop/lib/`
  contains symlinks like `libbe.so → ../../lib/libbe.so` which only
  resolve at install time.  We need the actual `.so` files at link
  time, hence linking against `RUNTIME_LIB` directly.
- `-lnetwork` is required for `socket`, `bind`, `recvfrom`, etc.
  `-lroot` for `ioctl`, `strerror`.

## Updating the packagefs haiku.hpkg (when bumping versions)

Sometimes the right move is to rebuild `haiku.hpkg` with the new
driver baked in, replacing the read-only packagefs copy.  The dance:

```
# 1. Commit the driver change (so the Haiku revision is clean).
# 2. Force re-derive the revision string:
rm generated.x86_64/build/haiku-revision \
   generated.x86_64/build/last-built-revision

# 3. Build:
jam -q haiku.hpkg

# 4. Push to test box:
ssh user@<test-box> 'rm /boot/system/packages/haiku-OLD.hpkg'
scp .../haiku.hpkg user@<test-box>:/boot/system/packages/haiku-NEW.hpkg

# 5. Activate the new one in the package list:
ssh user@<test-box> \
    'sed -i s/haiku-OLD/haiku-NEW/ \
     /boot/system/packages/administrative/activated-packages'

# 6. Reboot.
```

> **DANGER:** `/boot` on a typical Haiku test box is small (~1.5 GB).
> Trying to copy a new `haiku.hpkg` (~39 MB) while the old one is still
> there can fail with "No space left on device" and leave a partial
> file that puts packagefs in a degraded state.  Always delete the old
> hpkg first.

## Future: standalone .hpkg

The release plan is to publish the driver as a self-contained `.hpkg`
that doesn't require the Haiku source tree.  Approximate layout:

```
rtl8814au_unofficial-X.Y.Z.hpkg
   add-ons/kernel/drivers/bin/rtl8814au
   add-ons/kernel/drivers/dev/net/rtl8814au -> ../../bin/rtl8814au
   bin/wpa2_join
   data/firmware/rtl8814au/rtl8814aufw.bin
```

See [known-issues.md](known-issues.md) for the open work to get there
(packaging script, firmware redistribution license review, version
discipline, `pkgman`-installable upload).
