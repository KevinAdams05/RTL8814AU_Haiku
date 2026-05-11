>[!NOTE]
>An LLM was used to aid in development of this code.

# Building and deploying

The rtl8814au driver is a Haiku kernel addon.  It is built on a Haiku
machine against a Haiku source tree, then packaged as a standalone
`.hpkg` that can be dropped into `~/config/packages/` and activated
on reboot.

If you just want to install the driver, see [Installation](../README.md#installation)
in the top-level README — you can grab a prebuilt `.hpkg` from the
[Releases page](https://github.com/KevinAdams05/rtl8814au_unofficial/releases)
without ever building locally.

This document is for people who want to compile from source.

## Prerequisites

You need:

- A Haiku x86_64 system with a few hundred MB of free disk space.
- A Haiku source tree at some path `$HAIKU_TOP` (e.g. `~/haiku-build/haiku`).
- A configured generated directory inside that source tree (e.g.
  `~/haiku-build/haiku/generated.x86_64/`), produced via the standard
  `configure --build-cross-tools x86_64` dance from the
  [official Haiku build docs](https://www.haiku-os.org/development/build-haiku-from-source/).
- A checkout of this repo somewhere convenient (e.g.
  `~/projects/rtl8814au_unofficial/`).

The build does not need any external dependencies — the Haiku tree's
own `jam` and cross-tools handle everything.

## Source layout

```
rtl8814au_unofficial/
├── src/                    # driver source code
│   ├── Jamfile             # KernelAddon target
│   ├── Driver.cpp / Device.cpp / RegisterIO.cpp / Firmware.cpp
│   ├── EfuseReader.cpp / PhyConfig.cpp / TxPath.cpp / RxPath.cpp
│   ├── WiFiManagement.cpp / WPA2Crypto.cpp
│   ├── *.h
│   └── PhyRegTables.h
├── firmware/
│   └── rtl8814aufw.bin     # Lexra 3081 MCU firmware blob
├── package/
│   ├── PackageInfo         # .hpkg manifest
│   └── build-hpkg.sh       # build + package script
├── docs/                   # documentation (you are here)
├── LICENSE                 # MIT
└── README.md
```

## Building the .hpkg

From the project root, with `$HAIKU_TOP` set to your Haiku source tree:

```sh
HAIKU_BUILD=$HAIKU_TOP bash package/build-hpkg.sh
```

What it does:

1. Copies `src/*.cpp`, `src/*.h`, and `src/Jamfile` into
   `$HAIKU_TOP/src/add-ons/kernel/drivers/network/wlan/rtl8814au/`,
   wiping any stale files there.
2. Copies `firmware/rtl8814aufw.bin` into
   `$HAIKU_TOP/data/system/data/firmware/rtl8814au/`.
3. Runs `jam -q -j4 rtl8814au` from `$HAIKU_TOP/generated.x86_64/`.
4. Stages the resulting kernel addon binary, firmware blob, and
   `LICENSE` into a `package_root/` tree with the standard Haiku
   layout (`add-ons/kernel/drivers/bin/...`, `data/firmware/...`,
   etc).
5. Invokes the `package` tool from the Haiku build's `objects/.../tools/`
   directory to produce
   `build/rtl8814au-<version>-<arch>.hpkg`.

If your Haiku tree lives at the default
(`$HOME/haiku-build/haiku`), `HAIKU_BUILD` doesn't need to be set:

```sh
bash package/build-hpkg.sh
```

The build script's first few lines list every env var it honors;
`HAIKU_BUILD` and `HAIKU_ARCH` are the only two you might care about.

## Installing the .hpkg you just built

Copy it into a `packages/` directory packagefs watches:

```sh
cp build/rtl8814au-0.1.0-1-x86_64.hpkg ~/config/packages/
```

User-level (`~/config/packages/`) is preferred over system-level
(`/system/packages/`) — no root needed, and uninstall is just deleting
the file.

Reboot once.  On the way back up, packagefs activates the package and
the kernel scans `/system/add-ons/kernel/drivers/` (which now includes
our binary via the package's overlay).  `ls /dev/net/rtl8814au/`
should show a slot (e.g. `0`) for each connected adapter.

To uninstall, delete the `.hpkg` from the `packages/` directory and
reboot.

## Iterating during development

The .hpkg flow is the right thing for installs but slow for tight
edit-build-test cycles.  See [developer_notes.md](../developer_notes.md)
(not shipped in the repo) for the fast-path workflow that drops the
freshly-built kernel addon directly into
`/boot/home/config/non-packaged/add-ons/kernel/drivers/bin/rtl8814au`,
bypassing the package step.
