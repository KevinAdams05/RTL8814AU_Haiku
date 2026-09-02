# rtl8814au Coding Style Guide

rtl8814au is a **from-scratch, unofficial** Haiku kernel driver for USB WiFi
adapters built on Realtek's RTL8814AU (4x4 802.11ac), distributed as a
standalone `.hpkg`. The codebase is C++ in Haiku's kernel dialect —
`UpperCamelCase` classes owning hardware subsystems, no exceptions, no STL
containers, `status_t` everywhere — plus one small userland C tool in `tools/`.

Our style is the **Haiku Project Coding Guidelines**, with the
project-specific notes called out in §1.

**Authoritative base:** <https://www.haiku-os.org/development/coding-guidelines>

When this document and the Haiku guidelines disagree, **this document wins**
for rtl8814au code. When this document is silent, defer to Haiku.

Two things to know before reading further.

First, this guide is **descriptive before it is prescriptive**. Almost every
rule here was read out of `src/` rather than imposed on it — the tab width, the
80-column habit, the `/*! ... */` doxygen form with space-indented
continuations, the `dprintf(RTL8814AU_DRIVER_NAME ": ...")` idiom, the
`new(std::nothrow)` discipline, the `MutexLocker`-only locking. Where the
existing code is already consistent, the rule just writes that consistency
down. Where it is not, §21 and the baseline say so explicitly instead of
pretending otherwise.

Second, **do not reformat working code to satisfy this guide.** The driver is
~9,800 lines of C++ that took a long time to get onto the air, and a
whitespace-only diff across `Device.cpp` would destroy the ability to bisect a
regression. Adoption is via `scripts/style-baseline.txt` (§22): 175
pre-existing findings are recorded there, the gate requires zero *new*
findings, and the baseline shrinks whenever a file is being edited anyway.

---

## 1. Project-specific notes

These are the **only** intentional differences from upstream Haiku style.
Everything else in this document restates the Haiku rules for convenience.

### 1.1 Where the code lives, and where it thinks it lives

This is a standalone repository, but the `Jamfile` believes it is inside a
Haiku source tree:

```
SubDir HAIKU_TOP src add-ons kernel drivers network wlan rtl8814au ;
```

`package/build-hpkg.sh` stages `src/` into a real Haiku checkout and lets `jam`
build the kernel add-on, because re-implementing `KernelAddon` standalone would
be a second build system to maintain. Two consequences:

- A new source file must be added to `src/Jamfile` *and* will be picked up by
  the staging copy automatically — but if you add a new *directory*, check the
  copy step in `package/build-hpkg.sh`.
- The include paths available to you are the kernel's private ones
  (`UsePrivateHeaders kernel net ; UsePrivateKernelHeaders ;`), not userland's.
  `<SupportDefs.h>`, `<KernelExport.h>`, `<USB3.h>`, `<util/AutoLock.h>` are
  in scope; `<Application.h>` and friends are not, and never will be.

Everything in this project stays inside these directories:

| Path | What it is |
|---|---|
| `src/` | The kernel add-on. One flat directory, ten translation units. |
| `tools/` | Userland C helpers shipped in the same package (§1.5). |
| `firmware/` | The Lexra 3081 firmware blob. Binary, never edited. |
| `package/` | `PackageInfo` and the `.hpkg` build script. |
| `docs/` | Everything in this table, explained. |
| `scripts/` | The style checker and its baseline. |
| `tests/style/` | Fixtures for the style checker (§22). |

### 1.2 Line length — 80 columns, hard-failed at 100

- **Target: 80 columns**, computed at tab width 4. This is plain upstream
  Haiku, and it is not aspirational here: the 99th percentile line in
  `src/*.cpp` is **78 columns wide**, and only 52 of 9,803 lines exceed 80.
- **The linter hard-fails at 100 columns.** Past 100 a line is not "slightly
  wide", it is an unwrapped `dprintf` — all eight of the existing >100 lines
  are exactly that, one of them 192 columns.
- Between 80 and 100 the linter reports `line-over-target`. Treat it as a
  review comment, not a formatting emergency: 31 such lines exist, most of them
  a trailing `//` comment on a constant that lines up with its neighbours.

Measured distribution, for the record:

| Scope | Lines | p50 | p90 | p99 | max | >80 |
|---|---|---|---|---|---|---|
| `src/*.cpp` | 9,803 | 35 | 70 | 78 | 192 | 52 |
| `src/*.h` (excl. register tables) | 1,923 | 46 | 71 | 78 | 90 | 8 |
| `src/RTL8814AU.h` | 1,064 | 53 | 78 | 85 | 91 | 30 |
| `src/PhyRegTables.h` | 4,975 | 27 | 27 | 65 | 78 | 0 |
| `tools/wifi-join.c` | 198 | 26 | 68 | 79 | 79 | 0 |

The register-table headers and constant-table rows are exempt from both
limits (§21). Everything else is held to the numbers above.

When you must wrap:

```cpp
	status = _TransferSection(fData + fDmemOffset, fDmemSize,
		kOcpBaseDMem, true);

	dprintf(RTL8814AU_DRIVER_NAME ": transferring DMEM "
		"(%" B_PRIu32 " bytes)\n", fDmemSize);
```

Adjacent string literals concatenate, so a long log message wraps by splitting
the literal, never by running the line long.

### 1.3 Reference drivers are a reference, never a copy source

You may read Realtek's vendor trees (`morrownr/8814au`, `ulli-kroll/rtl8814au`,
`zebulon2/rtl8814au`), FreeBSD's `rtwn`, and USB packet captures of the
Windows driver in order to understand register semantics, power-on ordering,
the IDDMA firmware-load procedure, and descriptor layouts.

You still may **not** copy code from them, and that rule has not changed just
because this driver is now GPL v2 (§16). Keep writing original Haiku-style
code: a driver that reads like its donor is harder to review, harder to fix,
and harder to defend as our own work, whatever the licence says.

What the licence change does buy is that the question "was that consulted too
closely?" no longer has consequences. It was always the wrong question to have
hanging over a driver whose hardest facts — register values, init ordering,
descriptor bits — can only come from the vendor's tree or a datasheet nobody
has.

FreeBSD's `rtwn` is BSD-licensed and could in principle be ported, but we have
not done so, and mixing licences in one file is still not worth the audit
burden.

When a register sequence was derived from a reference driver, name the function
it came from in a comment. `src/RTL8814AU.h` and `src/Firmware.cpp` already do
this well:

```cpp
// Reference: rtl8814a_hal_init.c — FirmwareDownload8814A(),
//            _FWDownloadEnable_8814A(), IDDMADownLoadFW_3081(),
//            _3081Disable8814A(), _3081Enable8814A(), _FWFreeToGo8814A()
//            in morrownr/8814au.
```

That comment is doing real work: it is the only way a future reader can
re-derive a magic constant when the chip does something unexpected. Write it.

### 1.4 In-driver WPA2 is a deliberate architectural exception

This driver can run the WPA2 four-way handshake **inside the kernel**, which is
not how a Haiku wireless driver is normally built. Software CCMP
encrypt/decrypt runs in the data path too, because the chip's hardware crypto
engine will not engage.

It is a deliberate exception rather than a workaround for a platform defect.
The original justification -- that Haiku does not deliver EAPOL to `AF_LINK`
packet sockets -- **was wrong**, and `docs/wpa-supplicant-and-deskbar.md`
records why. The `wpa_supplicant` path works, and is the one to prefer. The
in-driver path is kept because it works, needs no `net_server` round trip, and
is the better tool for scripting and for isolating the driver during
diagnosis.

This is unusual, and the style consequence is: `src/WPA2Crypto.{h,cpp}` and the
handshake code in `Device.cpp` are held to the same style as everything else,
but the *design* is not up for review on style grounds. See
`docs/wpa2-in-driver.md` before proposing to move any of it.

### 1.5 `tools/` is userland C, not kernel C++

`tools/wifi-join.c` is C compiled against the public headers. It follows
Haiku's *formatting* rules — tabs, 80 columns, brace placement, return type on
its own line, copyright header — and is exempt from the rules that only make sense
in kernel C++:

| Exempt | Because |
|---|---|
| `raw-printf` | `printf`/`fprintf` are its output channel. It is a command-line tool. |
| `dprintf-prefix` | There is no `dprintf` in userland. |
| `bare-new`, `manual-mutex`, `raw-usb-request` | No `new`, no kernel mutexes, no USB bus manager. |
| `const-k-prefix`, `global-g-prefix`, `member-f-prefix` | It is C. Functions are `snake_case` (`parse_bssid`, `usage`), there are no classes. |
| `abbreviated-name` | Its structs mirror the net80211 ABI field-for-field (§3.4). |
| `include-order` | Its includes are grouped by relatedness, POSIX-style. |
| `nullptr` | Not a C keyword in the dialect it is built with. |

Everything else applies. The one existing violation is
`if (b[i] > 0xFF) return -1;` — a real §4 finding, baselined, not exempted.

### 1.6 Adoption is baseline-based

`scripts/style-baseline.txt` records every finding that existed when the
checker was written, so `python3 scripts/style-check.py` exits 0 today and
exits 1 the moment a new violation is introduced. This is the same mechanism
RadeonHD uses and for the same reason: a large existing codebase gets a working
gate immediately instead of a cleanup project it will never finish.

Rules for the baseline:

- **It shrinks.** Fix findings in files you are already touching, then run
  `--update-baseline`.
- **Growing it needs a sentence of justification** in the commit message.
  Adding a line to the baseline to silence a finding you could have fixed is
  the one thing that makes this whole mechanism worthless.
- `--all` shows baselined findings, which is how you find something to fix.

---

## 2. Indentation and whitespace

- **Tabs** for indenting blocks. Editor tab width is **4** for the purposes of
  computing line length and alignment.
- Wrapped lines get **at least one extra tab**, plus one more per expression
  nesting level.
- **Tabs also do column alignment** inside class bodies and constant tables —
  see §15.3 and §3.3. This is why the tab width matters: at width 8 every
  aligned declaration in `Device.h` falls off the right edge.
- Namespace contents are **not indented** — they sit flush at column 0.
  `namespace wpa2_crypto` in `WPA2Crypto.h` is the only namespace in the tree
  and does this correctly.
- **Spaces** on both sides of binary operators (`a + b`, `x == y`).
- **No space** between a C-style cast operator and its operand: `(uint32)value`.
- **Always a space** after a comma.
- Every file ends with a newline.
- No trailing whitespace on any line.
- Line endings are LF. `.gitattributes` pins `*.cpp`, `*.h`, `*.c`, `*.sh`,
  `Jamfile` and `PackageInfo` to `eol=lf`; `*.md` is deliberately left native,
  which is why `README.md` is CRLF in the working tree and why the checker's
  CRLF rule does not look at markdown.

The **one** place spaces are correct for indentation is the body of a
`/*! ... */` doxygen block, which this tree indents four spaces under the
opening `/*!` (§13.1). The checker knows about this; 499 lines depend on it.

## 3. Naming

| Kind | Convention | Example from the tree |
|---|---|---|
| Classes | `RTL8814AU` + `UpperCamelCase` | `RTL8814AUDevice`, `RTL8814AURegisterIO`, `RTL8814AUWiFiManager` |
| POD hardware/data structs | `UpperCamelCase`, no prefix | `RxFrameInfo`, `BssInfo`, `PowerSeqCommand`, `RTL8814AUDeviceID` |
| Methods | `UpperCamelCase` | `Read32`, `SubmitFrame`, `StartScan` |
| Private methods | `_` + `UpperCamelCase` | `_InitHardware`, `_PowerOnSequence`, `_ProgramCamEntry` |
| Local variables, parameters | `lowerCamelCase` | `frameLength`, `slotIndex`, `pixelClock` |
| Class data members | `f` + `UpperCamelCase` | `fRegisterIO`, `fJoinState`, `fMacAddress` |
| POD struct fields | bare `lowerCamelCase`, **no** `f` | `packetLength`, `dataRate`, `hasPhyStatus` |
| Constants | `k` + `UpperCamelCase` | `kMaxDeviceCount`, `kRegMcuFwDl`, `kJoinIdle` |
| Enumerators | `k` + `UpperCamelCase` | `kPwrCmdWrite`, `kEapolWaitM3` |
| Globals | `g` + `UpperCamelCase` | `gUSBModule`, `gDeviceList`, `gDeviceCount` |
| File-scope statics | `s` + `UpperCamelCase` | `sDeviceNames`, `sDataProtected` |
| Preprocessor macros | `ALL_CAPS` | `RTL8814AU_DRIVER_NAME`, `SIOCS80211` |

The `f`/`k`/`g`/`s` prefixes are not decoration. In a kernel driver where a USB
callback, an ioctl handler and two worker threads all touch the same object,
being able to see at a glance that an identifier is per-device state rather
than a local is worth the one character.

Note the **class vs. struct** split in the table, because it trips people:
`class` bodies use `f`-prefixed members, `struct`s that describe hardware or
data layouts do not. `RxFrameInfo::packetLength` is right; `RxFrameInfo::
fPacketLength` would be wrong. The checker only applies `member-f-prefix`
inside `class` bodies for exactly this reason.

Other rules:

- No underscores in class or method names, other than the `_` prefix on private
  methods and the lowercase driver entry points (§3.2).
- No articles: `message`, not `aMessage` or `theMessage`.
- All identifiers, comments and strings in **US English**.
- One documented legacy exception: `kRTL8814AU_ChipID` in `RTL8814AU.h` carries
  an internal underscore. Do not add more.

### 3.1 Descriptive names beat short ones

**Spell names out.** The few extra characters pay for themselves the first time
someone unfamiliar reads the code. `frameLength`, not `frameLen`; `buffer`, not
`buf`; `context`, not `ctx`; `message`, not `msg`; `index`, not `idx`.

The checker enforces this as `abbreviated-name` against a deliberately short
list — one entry per abbreviation a reviewer actually has to remember:

| Flagged | Write instead |
|---|---|
| `msg` | `message` |
| `idx` | `index` |
| `ptr` | `pointer` |
| `cnt` | `count` |
| `buf`, `buff` | `buffer` |
| `len` | `length` |
| `val` | `value` |
| `hdr` | `header` |
| `resp` | `response` |
| `err` | `error` |
| `sz` | `size` |
| `ctx` | `context` |
| `rslt` | `result` |
| `nbr` | `number` |

It matches on **word segments**, so `ssidLen`, `payloadLen` and `keyLength`
are treated the same way `len` is — and it reports once per identifier per
file, at first sight, rather than once per use. Fixing `ssidLen` is one rename,
so it should be one baseline line.

**Not flagged, because in this domain they are already the spelled-out form:**

`addr`, `desc` (a TX/RX *descriptor* is what the datasheet calls it), `mac`,
`ie` (802.11 information element), `seq`, `sec`, `cfg`, `pkt`, `chan`, `phy`,
`rf`, `bb`, `efuse`, `dma`, `usb`, `mcu`, `ssid`, `bssid`, `gtk`, `ptk`, `pmk`,
`tk`, `kck`, `kek`, `ccmp`, `aes`, `eapol`, `rssi`, `tid`, `aad`, `pn`, `llt`,
`cam`, `h2c`, `c2h`, `qsel`, `iddma`, `id`, `min`, `max`, `tmp`, and `i`/`j`/`k`
as tight loop indices.

When in doubt, spell it out. There are 69 baselined `abbreviated-name` findings
and every one of them is a rename somebody will be glad of later.

### 3.2 Haiku driver entry-point naming exception

The Haiku kernel add-on ABI specifies lowercase-with-underscores names for the
driver exports, and the USB bus manager specifies the shape of the notify
hooks. Use the exact names the OS expects:

```cpp
extern "C" {

status_t		init_hardware();
status_t		init_driver();
void			uninit_driver();
const char**	publish_devices();
device_hooks*	find_device(const char* name);

}
```

`api_version` is likewise a mandated file-scope symbol with no `g` prefix. Do
not "Haiku-ify" any of these to `InitDriver` style; the loader looks them up by
name and the driver will silently fail to attach.

### 3.3 Register and constant naming

Registers and bitfields are `static const` with a `k` prefix, grouped under a
banner comment, and **tab-aligned into columns** with a trailing `//` comment
per line:

```cpp
static const uint16 kRegMcuFwDl				= 0x0080;

// kRegMcuFwDl / REG_8051FW_CTRL_8814A bit definitions (32-bit at 0x0080)
// Byte 0 (0x0080):
static const uint32 kMcuFwDlEn				= (1 << 0);	// FW download enable
static const uint32 kMcuFwDlRdy				= (1 << 1);	// Set by host after write
static const uint32 kMcuMacIniRdy			= (1 << 3);	// MAC init ready
```

Rules:

- `static const uint8/uint16/uint32`, **not** `#define`. They are typed, they
  are scoped, and they show up in a debugger. The only `#define`s in the tree
  are `RTL8814AU_DRIVER_NAME`, `RTL8814AU_DEVICE_PATH_BASE`, and the net80211
  ioctl numbers, all of which need to be preprocessor tokens.
- Register **addresses** are `kReg<Name>`. Bit definitions take the register's
  name as a prefix so a grep for the register finds its bits:
  `kMcuFwDlEn` under `kRegMcuFwDl`.
- Keep the column alignment. Adding a name long enough to break it means
  re-tabbing the block, not letting one line stick out.
- Say what the bit does in the trailing comment. `// Lexra 3081 ready — poll
  this` is worth ten minutes to the next reader.
- When the Realtek name differs from ours, put the Realtek name in the comment.
  That is how the tree stays diffable against the vendor headers.

### 3.4 ABI-mirror struct fields keep their upstream names

`src/WiFiIoctl.h` reproduces FreeBSD's net80211 ioctl ABI byte for byte, and
`tools/wifi-join.c` reproduces a subset of it again on the userland side. Their
field names — `i_type`, `i_len`, `ik_keyix`, `im_ssid_len`, `isr_ie_off`,
`jp_passphrase_len` — **are** the interface. Renaming them to something
descriptive would be renaming somebody else's API, and the next person
comparing against `ieee80211_ioctl.h` would have no way to line the two up.

The checker skips any identifier matching a short lowercase prefix plus an
underscore (`i_`, `ik_`, `im_`, `isr_`, `jp_`, ...) wherever it appears, not
just inside `WiFiIoctl.h`, because those names travel into `Device.cpp` as
locals.

Everything you write that is *not* mirroring an external ABI follows §3.1.

## 4. Braces and blocks

- **Class / struct** opening brace: same line as the declaration.
- **Function** opening brace: on its **own line**, at column 0. The tree is
  100% consistent on this; keep it that way.
- **`if` / `else` / `for` / `while` / `switch`** opening brace: same line as
  the keyword and condition.
- `else` and `else if` go on a new line, after the closing brace.
- **Single-statement** `if`/`else`/`for`/`while`: omit the braces, and put the
  statement **on its own indented line**. Never on the same line as the
  condition — `if (channel <= 2)	return 0;` is a finding
  (`same-line-body`, 26 baselined).
- **Multi-statement** blocks: always braces.
- After an early `return`, `break` or `continue` inside an `if`, do **not**
  write an `else`.

```cpp
status_t
RTL8814AURxPath::Start()
{
	if (fFrameCallback == NULL) {
		dprintf(RTL8814AU_DRIVER_NAME ": RX start failed - no callback\n");
		return B_BAD_VALUE;
	}

	MutexLocker locker(fLock);
	if (fRunning)
		return B_OK;

	for (uint32 i = 0; i < kRxTransferCount; i++) {
		status_t status = _SubmitTransfer(i);
		if (status != B_OK)
			return status;
	}

	fRunning = true;
	return B_OK;
}
```

## 5. Functions

- Return type on its own line, **above** the function name, in a
  *definition*.
- In a *declaration* the return type stays on the same line — that is what
  every header in the tree does, and it is what makes the tab-aligned class
  bodies work.
- Opening brace on its own line, flush left.
- **Two blank lines** between function definitions. The tree has zero
  violations of this; the checker enforces it as `function-spacing`.
- Long argument lists: wrap and indent the continuation by **one tab**.

```cpp
status_t
RTL8814AUDevice::_ProgramCamEntry(uint8 entry, uint8 algo, uint8 keyId,
	bool isGroupKey, const uint8 mac[6], const uint8* key, uint32 keyLength)
{
	...
}
```

### 5.1 Free functions

`src/WPA2Crypto.{h,cpp}` is the one place in the tree with free functions
rather than methods, and it names them `lower_snake_case` inside
`namespace wpa2_crypto`: `sha1`, `hmac_sha1`, `prf_384`, `aes128_encrypt`,
`pbkdf2_hmac_sha1`, `aes_key_unwrap`.

This is deliberate and correct. Those names are the names the RFCs and IEEE
802.11i use, and a reader checking the implementation against RFC 3174 §5 or
802.11i §8.5.1.1 should not have to translate. Keep it for anything else in
that file; do not spread the convention to new subsystems, which get a
`RTL8814AU<Name>` class.

## 6. Constructor initializer lists

- Colon on its **own line**, indented one tab.
- Each initializer on its own line, indented one tab, in declaration order.
- Prefer the initializer list over assignment in the body.

```cpp
RTL8814AURxPath::RTL8814AURxPath(RTL8814AURegisterIO* registerIO,
	usb_module_info* usbModule, usb_device usbDevice, usb_pipe bulkIn)
	:
	fRegisterIO(registerIO),
	fUSBModule(usbModule),
	fUSBDevice(usbDevice),
	fBulkIn(bulkIn),
	fFrameCallback(NULL),
	fRunning(false),
	fInitStatus(B_NO_INIT)
{
	mutex_init(&fLock, "rtl8814au:rx");
	...
}
```

`RTL8814AUDevice`'s constructor breaks the "prefer the list" half of this rule
on purpose: it has ~40 members, many of them arrays, and it clears them with
`memset` and explicit assignments in the body. The comment in `Device.h`
explains why it matters that **every** member is initialized —

> All zero-initialized in the constructor — net_server probes the ioctl
> interface before userland sets state, and we must never run `_DoJoin`
> against uninitialized memory.

— which is a correctness argument, not a style one. If you add a member to
`RTL8814AUDevice`, initialize it in the constructor. There is no default you
can rely on.

## 7. Blank lines

- **Two blank lines** between function definitions.
- **Two blank lines** between the include block and the first declaration, and
  around banner comments.
- **One blank line** between cases in a `switch`.
- **Two blank lines** after the `#define` of a header guard, and two before the
  closing `#endif`.
- **No** blank line between the copyright block and the header guard.
  `Device.h`, `Driver.h` and `RTL8814AU.h` are the canonical form;
  `WPA2Crypto.h` has a stray blank line there and is wrong.
- Never three or more consecutive blank lines outside a register table.

Banner comments separate the major regions of a `.cpp`:

```cpp
// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------


/*! Register the callback that will receive parsed frames. ...
```

Two blank lines after the banner, then the first function. Use them; a
2,600-line `Device.cpp` is navigable because of them.

## 8. Control flow specifics

### 8.1 If / else

- Always use explicit tests.
  - Pointers: `if (pointer != NULL)`, not `if (pointer)`.
  - Integers: `if (count != 0)`, not `if (count)`.
  - `bool` is the exception: `if (fRunning)` is correct and idiomatic.
- Bitmasks always get an explicit comparison:
  `if ((keyInfo & 0x0008) != 0)`. The tree does this well in
  `_HandleEapolFrame` and badly in `Firmware.cpp`; there are 7 baselined
  `implicit-bitmask` findings.
- No assignment inside an `if` or `while` condition. Split it:
  ```cpp
  status_t status = _ValidateHeader();
  if (status != B_OK)
      return status;
  ```
- Variable on the **left**: `if (status == B_OK)`, never `if (B_OK == status)`.
  The tree has zero Yoda conditions. Keep the streak.
- No redundant outer parentheses, and do not parenthesise each clause:
  `if (pairwise && keyAck)`, not `if ((pairwise) && (keyAck))`.

### 8.2 Long conditions

Put the **logical operator at the start** of the continuation line:

```cpp
	if (frameLength < kMinimumFrameLength
		|| frameLength > kUsbRxBufferSize
		|| (descriptor.crcError && !fAcceptErrors)) {
		fFramesDropped++;
		return;
	}
```

### 8.3 Switch

- `case` labels indented one tab inside the `switch`, bodies one tab further.
- One blank line between cases.
- Wrap a case body in `{ }` whenever it declares a variable.
- Always have a `default:`, even if it only `break;`s. In a driver dispatching
  on a value that came off the wire or out of a register, the `default:` is
  where the "chip did something undocumented" log line lives.

```cpp
	switch (subtype) {
		case kSubtypeBeacon:
		case kSubtypeProbeResponse:
			_ParseBeaconOrProbe(frameData, frameLength, rxInfo);
			break;

		case kSubtypeAuth:
		{
			uint16 statusCode = ...;
			_HandleAuthResponse(frameData, frameLength);
			break;
		}

		default:
			dprintf(RTL8814AU_DRIVER_NAME ": unhandled mgmt subtype %u\n",
				subtype);
			break;
	}
```

### 8.4 Loops

- Prefer `for` over `while`-with-assignment.
- Index loops use `uint32 i` (or `j`, `k`) — the one place a single letter is
  correct.
- Range-based `for` is available but there is nothing in the tree to iterate
  with it; fixed-size arrays and `kRxTransferCount`-style bounds are the norm.

### 8.5 No `goto`

Use early return, or a small helper function, or restore state in one place at
the end of a linear sequence.

There are **7 `goto`s in the tree** — six in `Firmware.cpp::Load()` and one in
`PhyConfig.cpp`. They are baselined, not blessed. `Firmware.cpp` is the hardest
case in the codebase to fix: it is a twelve-step hardware sequence with two
different unwind paths (`cleanup` and `cleanup_resume`, the second of which
also has to restore two saved registers and resume the MCU). The right fix is a
small RAII holder for "download mode is enabled / the MCU is halted / CR+1 is
modified" so each unwind happens in a destructor, at which point the `goto`s
become `return`s. Do that when you next need to touch firmware loading; do not
do it as a standalone refactor of code that currently works.

**New code does not add a `goto`.** The checker will fail the gate.

## 9. Types

### 9.1 Prefer Haiku types over raw C types

Kernel code uses the `<SupportDefs.h>` types throughout:

- `int32` / `uint32` instead of `int` / `unsigned`.
- `uint8` / `uint16` for register values and on-air bytes — and use the width
  the register actually has. `Read8`/`Read16`/`Read32` exist because the chip
  cares.
- `int8` for RSSI (it is signed dBm).
- `uint64` for the 48-bit CCMP packet number and the EAPOL replay counter.
- `size_t` / `ssize_t` for sizes and byte counts.
- `off_t` for the `Read`/`Write` device-hook position argument.
- `bigtime_t` for `system_time()` values (`fLastSeen`, scan timeouts).
- `status_t` for anything that can fail, `B_OK` on success.
- `sem_id`, `thread_id`, `mutex`, `usb_device`, `usb_pipe` for kernel objects.

Do not use `long`. It is 32-bit on x86 and 64-bit on x86_64, and the only
appearances in the tree are `(unsigned long)` casts feeding `%lu` in a
`dprintf`, which is the one legitimate use.

### 9.2 Format macros in log messages

Haiku's fixed-width types need the `B_PR*` macros or the format string is wrong
on one architecture:

```cpp
	dprintf(RTL8814AU_DRIVER_NAME ": RX path initialized "
		"(%" B_PRIu32 " buffers x %" B_PRIu32 " bytes)\n",
		kRxTransferCount, kUsbRxBufferSize);
```

`B_PRId32` / `B_PRIu32` / `B_PRIx32` for `int32`/`uint32`, `B_PRIuSIZE` for
`size_t`, `B_PRIu64` for `uint64`. `%u` and `%x` are fine for `uint8`/`uint16`,
which promote to `int`.

### 9.3 Strings

There is no `BString` in kernel space. Use `char[N]` with `strlcpy` and
`snprintf`:

```cpp
	strlcpy(fDeviceName, deviceName, sizeof(fDeviceName));
	snprintf(path, sizeof(path), "%s/%" B_PRIu32, RTL8814AU_DEVICE_PATH_BASE,
		slotIndex);
```

`strlcpy`/`strlcat`, never `strcpy`/`strcat`. `snprintf` with `sizeof(buffer)`,
never `sprintf`. `snprintf` is not a §19 violation — it formats into a caller's
buffer; `printf` writes to a stdout the kernel does not have.

### 9.4 Casts

- C-style casts are the convention here and are correct for primitive numeric
  conversions: `(uint32)length`, `(unsigned long long)pn`,
  `((uint16)body[1] << 8) | body[2]`.
- **No whitespace after the cast operator.** `(int32)count`, not
  `(int32) count`. The tree has zero violations.
- C++ casts (`static_cast`, `reinterpret_cast`, `const_cast`) are used where
  they carry information a C cast would hide — reinterpreting a byte buffer as
  a descriptor, or casting away `const` deliberately. Roughly 20 sites do this;
  follow the local pattern rather than converting either way.
- `dynamic_cast` does not work in the kernel (no RTTI). Do not reach for it.

### 9.5 Byte order

The chip is little-endian and so is every host Haiku runs on today, but
on-air 802.11 and EAPOL fields are big-endian and must be assembled explicitly:

```cpp
	uint16 keyInfo = ((uint16)body[1] << 8) | body[2];
```

Use `<ByteOrder.h>` (`B_HOST_TO_LENDIAN_INT32`, `B_BENDIAN_TO_HOST_INT16`) for
anything that goes into or comes out of a descriptor or a frame header. Never
`memcpy` a multi-byte field out of a packet and use it as-is.

## 10. Pointers and null

- `NULL`, not `0` or `nullptr`. (Haiku tradition; zero `nullptr` in the tree.)
- Initialize with assignment: `RTL8814AUDevice* device = NULL;`
- **Pointer asterisk binds to the type**: `uint8* buffer`, not `uint8 *buffer`.
  The tree is 100% consistent here.
- Do **not** check for `NULL` before `delete` / `delete[]` / `free` — all three
  accept `NULL`:
  ```cpp
  delete[] fData;
  fData = NULL;
  ```
- Do check for `NULL` after every allocation (§17).

## 11. Boolean conventions

- `true` / `false`, never `TRUE` / `FALSE`. The only `TRUE`/`FALSE` in the tree
  are inside comments quoting the reference driver's argument names, which is
  fine.
- Functions that report success/failure return `status_t`, not `bool`. `bool`
  means a genuine yes/no flag (`fRunning`, `hasPhyStatus`, `crcError`).

## 12. Returns and parentheses

- Do not parenthesise the return expression: `return status;`, not
  `return (status);`. Zero violations in the tree.
- Prefer early returns; keep the happy path at one indent level.
- A `void` function that has nothing left to do just falls off the end. A bare
  `return;` at the end of a `void` function is noise.

## 13. Comments

- Prefer `//` for ordinary comments. `/* */` is for the file header and the
  doxygen blocks (§13.1). `tools/wifi-join.c` uses `/* */` throughout because
  it is C; that is correct there.
- Explain **why**, not what. `i++; // increment i` is noise.
- For hardware code, "why" usually means one of three things, and all three are
  worth writing down:
  1. **What the chip demands.** `// The MCU is halted during transfer via
     REG_SYS_FUNC_EN bit 12, NOT the 8051 reset bit in REG_MCUFWDL.`
  2. **What went wrong last time.** `// That path permanently locks the
     0x1200+ DDMA register space.`
  3. **Which reference function this came from** (§1.3).
- Point at the spec by section when there is one: `// per RFC 2104 §2`,
  `// IEEE 802.11i §8.5.1.1`.
- No author initials. Git knows.
- Plain `// TODO:` is fine. `// TODO(kevin):` is not — git knows that too.
- No `#if 0` dead code. Delete it.

### 13.1 The doxygen block form

Public methods get a `/*! ... */` block immediately above the definition, with
the body indented **four spaces** and the closing `*/` on its own line:

```cpp
/*! Create a new device instance for a USB device we claimed.
    Sets up the register I/O module and discovers USB endpoints.
    Does NOT initialize the hardware yet — that happens on first open().

    \param device       USB device handle from the bus manager
    \param slotIndex    Index in gDeviceList[] for this device
    \param deviceName   Human-readable name (e.g., "ASUS USB-AC68")
*/
RTL8814AUDevice::RTL8814AUDevice(usb_device device, uint32 slotIndex,
	const char* deviceName)
```

Single-line form for something short:

```cpp
/*! Read a 32-bit register, clear the bits in mask, set new bits from value. */
```

Two things about this form:

- The four-space continuation indent is the **one** legitimate use of spaces
  for leading whitespace in this codebase, and it applies to 499 lines. The
  style checker knows the difference between a doxygen body and
  space-indented code; if you write a rule of your own, make sure it does too.
- `\param` descriptions are column-aligned with spaces, not tabs, so they stay
  aligned regardless of tab width.

Header files put the same information in a plain `//` comment above the
declaration rather than a second doxygen block — see `RegisterIO.h`. Do not
duplicate a doxygen block in both places.

## 14. Includes

### 14.1 Ordering

In a `.cpp`, in this order, with **one blank line** between groups:

1. The file's own header (`#include "RxPath.h"` from `RxPath.cpp`).
2. POSIX / standard C headers, plus `<new>`: `<new>`, `<stdio.h>`,
   `<stdlib.h>`, `<string.h>`.
3. Haiku kernel and API headers: `<ByteOrder.h>`, `<KernelExport.h>`,
   `<OS.h>`, `<USB3.h>`, `<lock.h>`, `<util/AutoLock.h>`.
4. Private network-stack headers: `<ether_driver.h>`,
   `<NetworkNotifications.h>`, `<net/if_media.h>`.
5. Local project headers: `"RegisterIO.h"`, `"WiFiIoctl.h"`, `"WPA2Crypto.h"`.

**Alphabetize within each group.** The checker accepts either ASCII or
case-insensitive order, because the tree contains groups that are only sorted
under one of the two, and being strict would flag correct code.

`RxPath.cpp` is the model:

```cpp
#include "RxPath.h"

#include <new>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <OS.h>

#include "RegisterIO.h"
```

One documented exception: `Device.h` includes its subsystem headers in
**dependency order** rather than alphabetically, because that order is the
architecture — `RTL8814AU.h`, then `RegisterIO.h`, then the modules that own a
`RegisterIO*`. It is baselined rather than fixed.

### 14.2 Style

- `<angle>` for system, kernel and private-stack headers.
- `"quoted"` for our own headers.
- **C-style header names**: `<string.h>`, not `<cstring>`. `<new>` is the one
  C++ header used, and only for `std::nothrow`.
- No path components the build system makes unnecessary.
- Forward-declare instead of including where you only need a pointer —
  `Driver.h` does `class RTL8814AUDevice;` rather than pulling in `Device.h`.

## 15. Header files

### 15.1 Layout

```cpp
/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the GNU General Public License version 2.
 *
 * RegisterIO.h — Hardware register access for the RTL8814AU.
 *
 * All RTL8814AU registers are accessed via USB vendor-specific control
 * transfers. This class wraps the raw USB send_request() calls into
 * typed read/write operations with error handling and retry logic.
 */
#ifndef RTL8814AU_REGISTER_IO_H
#define RTL8814AU_REGISTER_IO_H


#include <USB3.h>
#include <lock.h>

#include "RTL8814AU.h"


class RTL8814AURegisterIO {
public:
								RTL8814AURegisterIO(usb_device device,
									usb_module_info* usbModule);
								~RTL8814AURegisterIO();

	uint8						Read8(uint16 address);

private:
	status_t					_SendRequest(...);

	usb_device					fDevice;
	mutex						fLock;
};


#endif	// RTL8814AU_REGISTER_IO_H
```

The **file-purpose comment** in the header block is not optional and it is the
best thing about this codebase. Every header in `src/` explains what its class
is for, and several explain the hardware protocol they wrap — `RegisterIO.h`
documents the full USB control-transfer format, `Device.h` lists the seven
lifecycle stages in order, `RxPath.cpp` diagrams the de-aggregation format.
Write that comment. It is where the driver's design documentation actually
lives, `docs/` notwithstanding.

### 15.2 Header-guard rules

- Form: `RTL8814AU_<NAME>_H`, where `<NAME>` is the filename in
  `SCREAMING_SNAKE_CASE` — `Device.h` → `RTL8814AU_DEVICE_H`,
  `WiFiManagement.h` → `RTL8814AU_WIFI_MANAGEMENT_H`, `RegisterIO.h` →
  `RTL8814AU_REGISTER_IO_H`. An acronym run stays one word (`IO` → `IO`, not
  `I_O`), and the chip name is one word (`RTL8814AU.h` → `RTL8814AU_H`, with no
  duplicated prefix).
- No leading underscore. `_RTL8814AU_WPA2_CRYPTO_H` in `WPA2Crypto.h` is the
  one violation, is baselined, and is a one-line fix whenever that file is next
  touched. Names beginning with an underscore followed by a capital are
  reserved to the implementation in C++ anyway.
- The guard immediately follows the copyright block — **no blank line between
  them**.
- **Two blank lines** after the `#define`, and two before the closing `#endif`.
- The closing `#endif` carries a `// <GUARD>` comment:
  `#endif	// RTL8814AU_REGISTER_IO_H`.
- `#pragma once` is not used and will not be.

### 15.3 Member and method declaration alignment

Class bodies align in columns using tabs, and the name always lands on
**column 32** at tab width 4. Three forms, all in `Device.h`:

- **Methods**: one tab, the return type, then tabs out to column 32.
  ```
  	status_t					InitCheck() const { return fInitStatus; }
  ```
- **Constructors / destructors**: no return type, so eight tabs straight to
  column 32.
  ```
  								RTL8814AUDevice(usb_device device,
  									uint32 slotIndex,
  									const char* deviceName);
  ```
- **Data members**: one tab, the type, then tabs out to column 32.
  ```
  	usb_device					fUSBDevice;
  	uint32						fSlotIndex;
  ```

Wrapped parameter lists get one extra tab beyond the name column. When you add
a declaration, match the block you are adding to; when a new type name breaks
the alignment, re-tab the block rather than letting one line stick out.

An empty inline body is allowed on one line inside the class, and only there:
`bool IsOpen() const { return fOpenCount > 0; }`.

## 16. Copyright headers

rtl8814au is **GPL v2-licensed**, matching the Realtek vendor driver used as a
reference (§1.3). It was MIT until 2026-08-27; the change is precautionary
rather than the result of any code being copied. Every source file starts with
the Haiku two-line form:

```cpp
/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the GNU General Public License version 2.
 *
 * <Filename> — one-line purpose.
 *
 * <Longer explanation of what this file is for and any protocol or
 * hardware detail a reader needs before the first function.>
 */
```

- The authorship line is exactly
  `Kevin Adams <kevinadams05@gmail.com>`.
- Update the year on a substantive change. A typo fix does not bump it.
- If a file ever gains a second author, add a `Authors:` block below the
  license line in the Haiku style; no file needs one today.
- **Still do not copy code in.** The licence no longer forbids it, but the
  reasons to keep this driver original outlive the licence: code that reads
  like its donor is harder to review and harder to fix, and we cannot debug
  what we did not reason through. `package/PackageInfo` declares
  `licenses { "GNU GPL v2" }` and the `.hpkg` ships `LICENSE`; keep both
  accurate.

## 17. Resource management

Kernel code, no exceptions, so allocation and locking both have hard rules.

- **Allocate with `new(std::nothrow)` and check the result.** The kernel is
  built without exceptions; a bare `new` cannot throw and cannot report
  failure, so it hands you a null pointer to dereference later. The tree is
  100% `new(std::nothrow)` across 13 sites; keep it that way.
  ```cpp
  fRegisterIO = new(std::nothrow) RTL8814AURegisterIO(device, gUSBModule);
  if (fRegisterIO == NULL) {
      fInitStatus = B_NO_MEMORY;
      return;
  }
  ```
- **Lock with `MutexLocker`, never `mutex_lock`/`mutex_unlock`.** There are 37
  `MutexLocker` sites and zero manual pairs. With the early-return style in §12
  a manual pair is a deadlock waiting for the next `return` to be added above
  the unlock.
  ```cpp
  MutexLocker locker(fLock);
  ```
  `<util/AutoLock.h>` provides it.
- **Two-phase construction.** Constructors cannot fail, so every class stores
  a `status_t fInitStatus` (initialized to `B_NO_INIT`) and exposes
  `InitCheck()`. The caller checks it before using the object. Follow the
  pattern; do not invent a `bool valid` flag.
- **Semaphores and threads are `-1` when absent**, and the destructor deletes
  or kills only what it created. `RTL8814AUDevice` initializes `fEapolReady`,
  `fPostAssocThread`, `fScanNotifierThread` and friends to `-1` for exactly
  this reason.
- **Hardware init is deferred to first `open()`**, not done at USB attach time,
  so the ~1 second firmware download does not block the USB bus manager and so
  the error reaches the caller who actually wanted the device. Do not move work
  into `DeviceAdded()`.
- **Hot-unplug is refcounted.** `fRemoved` plus `fOpenCount` decide whether
  `DeviceRemoved()` or the last `Free()` destroys the object. Any new resource
  must be released on both paths.
- No `goto cleanup:` (§8.5).

### 17.1 Never block in a USB callback

The RX bulk-transfer callback runs on the USB stack's thread. A **synchronous
USB control transfer issued from there deadlocks** — this is written into
`Device.h` next to `_PostAssocLoop`, and it is why the post-associate H2C
sequence runs on its own worker thread woken by a semaphore rather than inline.

So: a callback parses, queues and signals. It does not do register I/O, does
not `snooze()`, and does not wait on anything. If new work needs to happen in
response to a received frame, add it to a worker thread and release a
semaphore.

## 18. Dead code and debug code

- No `#if 0` blocks. Delete the code; git keeps history.
- No commented-out code left "in case".
- Diagnostic `dprintf`s that fire per-frame must be removed or gated before a
  release. There is a real reason beyond tidiness, recorded in `Device.cpp`:

  > Single-line classifier — verbose hex dumps removed to keep syslog volume
  > low (BFS `acquire_vnode` panics correlate with log floods).

  A driver that logs every received frame can take the machine down. Treat a
  per-frame log line as a debugging tool you take back out.
- Counters guarded to log every Nth event (`fSubmitsLogged`,
  `fTransfersCompleted`) are the right shape for something you want to keep.

## 19. Logging

There is no `TRACE()`/`ERROR()` macro pair in this project. The idiom is a
direct `dprintf` with the driver name pasted on by the preprocessor:

```cpp
	dprintf(RTL8814AU_DRIVER_NAME ": firmware version %u, "
		"DMEM %" B_PRIu32 " bytes, IRAM %" B_PRIu32 " bytes\n",
		fVersion, fDmemSize, fIramSize);
```

`RTL8814AU_DRIVER_NAME` is `"rtl8814au"` (`RTL8814AU.h`), and because it is a
string literal it concatenates at compile time — there is no runtime cost and
no format-string risk.

Rules:

- **Every `dprintf` leads with `RTL8814AU_DRIVER_NAME ": "`.** The whole point
  is that `grep rtl8814au /var/log/syslog` finds everything the driver said and
  nothing else. Two baselined violations exist, both continuation lines of a
  hex dump in `WiFiManagement.cpp`.
- **No `printf` / `fprintf` / `puts` in `src/`.** The kernel has no stdout.
  `snprintf` into a buffer is fine and is not this rule.
- **ASCII only** inside a log string (§20.4).
- Every message ends with `\n`. The syslog does not add one.
- Include the register address or offset when reporting a hardware failure —
  `PollFor8(0x%04x) timed out` is actionable, `poll failed` is not.
- Say which device when there could be more than one. `fSlotIndex` is the
  index in `gDeviceList[]` and is what the device node is named after.

## 20. Driver-specific rules

These are the sections worth re-reading. Everything above is formatting; this
is about the driver working.

### 20.1 All register access goes through `RegisterIO`

`RTL8814AURegisterIO` is the only class that calls the USB bus manager's
`send_request()`. Everything else reads and writes registers through
`Read8`/`Read16`/`Read32`, `Write8`/`Write16`/`Write32`, `WriteN`,
`MaskedWrite*` and `PollFor*`.

The checker enforces this as `raw-usb-request`, with `src/RegisterIO.cpp` as
the only exempt file. The payoff is concrete:

1. **One place to instrument.** A register trace of a failing power-on sequence
   is one `dprintf` in `RegisterIO.cpp`, not a hunt through six files.
2. **One place for retry and error handling.** USB control transfers fail
   transiently; the retry loop and the "disconnected device returns all-ones"
   convention live in one function.
3. **One place that knows the transfer format.** `bmRequestType` 0xC0/0x40,
   `bRequest` 0x05, address in `wValue`. Nothing else should know that.

If you need an access pattern `RegisterIO` does not offer, add a method to
`RegisterIO`. Do not reach past it.

### 20.2 Errors propagate as `status_t`, and nobody swallows one

- Every function that can fail returns `status_t`. Every `status_t` return is
  checked.
- Return the Haiku error that describes the failure, not a generic one:
  `B_BAD_VALUE` for a malformed argument, `B_NO_MEMORY` for allocation,
  `B_TIMED_OUT` for a poll that never matched, `B_BUSY` for a state conflict,
  `B_DEV_NOT_READY` for hardware that is not up, `B_NO_INIT` for an object
  whose `InitCheck()` failed, `B_NOT_SUPPORTED` for an ioctl we do not handle.
  `PollFor8` returning `B_TIMED_OUT` rather than `B_ERROR` is what makes a
  power-on failure diagnosable from the syslog alone.
- Log at the point of failure with enough context to identify the register or
  the frame, then return. Do not log the same failure again at every level as
  it unwinds.
- A frame-level failure in the RX path increments a counter and drops the
  frame; it does not tear down the pipe. A transfer-level failure re-submits.
  Know which one you are handling.

### 20.3 Hardware sequences are data, not code

The power-on sequence is a table of `PowerSeqCommand` (write / poll / delay /
end) walked by a small interpreter, not 200 lines of open-coded register
writes. Same for the PHY register tables in `PhyRegTables.h`.

Keep new sequences in that shape. It is how the sequence stays comparable
against the reference driver's `Hal8814PwrSeq.c`, and it is how a chip-revision
difference becomes a `cutMask` change rather than an `#ifdef`.

### 20.4 Log strings are ASCII

`dprintf` output lands in the syslog and, when debugging, on a serial console
at 115200 baud. Neither is reliably UTF-8. Use `-` and `->` and `x`, not `—`
and `→` and `×`.

There are **19 baselined violations**, all em-dashes and one multiplication
sign that read beautifully in the source and arrive as mojibake on a serial
capture. Prose comments may use whatever punctuation reads best — this rule is
about string literals only.

### 20.5 The firmware blob is opaque and its trailers matter

`firmware/rtl8814aufw.bin` is a Realtek binary for the chip's Lexra 3081
MIPS-derived MCU. It is never edited, never regenerated, and never parsed
beyond its 64-byte header.

The one thing to know, because it cost a debugging session: each section
carries an **8-byte XOR trailer**, and the DMEM/IRAM sizes from the header must
be **extended by 8** to include it or the checksum never validates. See
`docs/firmware.md`.

## 21. Relaxed files, in detail

Three files are held to a reduced rule set. This is not a licence to be sloppy
in them; it is an acknowledgement that they are transcriptions of somebody
else's data.

| File | Exempt from | Why |
|---|---|---|
| `src/RTL8814AU.h` | `line-over-target`, `line-too-long`, `abbreviated-name`, `member-f-prefix`, `include-order` | 1,064 lines of register addresses and bitfields, column-aligned with trailing comments. Names mirror the Realtek spec header so the two can be diffed; wrapping a table row at 80 destroys the alignment for the whole block. |
| `src/PhyRegTables.h` | same | 4,975 lines of generated PHY/RF register-value tables. Nothing here is prose. (It already fits in 78 columns, so the exemption is mostly insurance.) |
| `src/WiFiIoctl.h` | `abbreviated-name`, `member-f-prefix` | Byte-for-byte mirror of FreeBSD's net80211 ioctl ABI (§3.4). Its field names are the interface. |
| `tools/*.c` | see §1.5 | Userland C, not kernel C++. |

Plus one content-based exemption, which applies anywhere: a **constant-table
row** — a line consisting only of numeric literals, commas, braces and
whitespace — is exempt from both column limits. That is what lets the AES
S-boxes in `WPA2Crypto.cpp` stay 16 bytes wide at 99 columns and be read as the
16x16 grid they are.

What is **not** relaxed in any of these files: line endings, final newline,
trailing whitespace, tab indentation, the copyright header, `#if 0`, `goto`,
`nullptr`, `TRUE`/`FALSE`, non-ASCII string literals, and the header-guard
form. A register table still has to be a well-formed source file.

## 22. Tooling

```sh
python3 scripts/style-check.py                 # honour the baseline
python3 scripts/style-check.py --all           # show baselined findings too
python3 scripts/style-check.py --changed       # only what changed vs HEAD
python3 scripts/style-check.py --list-rules    # what it enforces
python3 scripts/style-check.py --update-baseline
python3 scripts/style-check.py --self-test     # verify the checker itself
```

Exit status is 0 when there are no **new** findings, 1 otherwise, so it gates a
release directly. Run it before `package/build-hpkg.sh`.

The linter is a **checker, never a reformatter**. It reports; a human fixes.

It cannot check everything in this document. Whether a comment explains *why*,
whether a name is well chosen beyond the abbreviation list, whether a `status_t`
is checked, whether a callback blocks — all beyond it. A clean run means "no
mechanical violations found", not "this is good code".

### 22.1 The baseline

`scripts/style-baseline.txt` holds one line per pre-existing finding, keyed by
`path`, `rule` and `line`. It was generated by running the checker over the
tree at the point this guide was written: **175 findings**.

```
   69  abbreviated-name      (§3.1 — 69 identifiers, at first sight per file)
   31  line-over-target      (§1.2 — 80..100 columns)
   26  same-line-body        (§4 — statement after the condition)
   19  non-ascii-str         (§20.4 — em-dashes in dprintf strings)
    8  line-too-long         (§1.2 — over 100 columns; 4 in build-hpkg.sh)
    7  goto-used             (§8.5 — 6 in Firmware.cpp, 1 in PhyConfig.cpp)
    7  implicit-bitmask      (§8.1)
    3  const-k-prefix        (§3 — sBox, invSBox, rcon in WPA2Crypto.cpp)
    2  dprintf-prefix        (§19 — hex-dump continuation lines)
    1  header-guard          (§15.2 — _RTL8814AU_WPA2_CRYPTO_H)
    1  include-order         (§14.1 — Device.h, dependency-ordered on purpose)
    1  trailing-space        (§2)
```

Because the key includes the line number, editing a file shifts its baselined
findings and they resurface as "new". That is a feature: it means the file you
are already editing is the file whose debt you clear. Fix them, then
`--update-baseline`.

### 22.2 The self-test

`--self-test` runs the checker against `tests/style/`: `bad.cpp` and `bad.h`
must trip a listed set of rules, `good.cpp` and `good.h` must be silent.

**Add a fixture violation whenever you add a rule.** On a codebase this clean,
a rule that matches nothing looks exactly like a rule that works — Elviq
shipped two rules anchored to the wrong column that silently matched nothing,
and that is the failure mode this guards against.

`good.cpp` also pins the tricky positives: it carries the space-indented
doxygen continuation lines from §13.1, so a naive "indentation must be tabs"
rule fails the self-test instead of producing 499 false findings.

Two rules (`eol-crlf`, `no-final-eol`) are driven from scratch files written at
self-test time rather than committed fixtures, because `.gitattributes` would
repair a CRLF fixture on checkout.

## 23. PR / release checklist

Before opening a PR or cutting a release:

- [ ] `python3 scripts/style-check.py` exits 0 (§22).
- [ ] `python3 scripts/style-check.py --self-test` passes, if you touched the
      checker.
- [ ] The baseline did not grow. If it did, the commit message says why.
- [ ] `package/build-hpkg.sh` builds the kernel add-on and `wifi-join` clean.
- [ ] Any new source file is in `src/Jamfile` (§1.1).
- [ ] No lines over 100 columns; nothing new over 80 without a reason.
- [ ] New public/header API has a purpose comment, and a `/*! ... */` block on
      the definition (§13.1, §15.1).
- [ ] Every `dprintf` leads with `RTL8814AU_DRIVER_NAME ": "` and its string is
      ASCII (§19, §20.4).
- [ ] No `printf`/`fprintf` in `src/`; no per-frame log lines left on (§18).
- [ ] Every allocation is `new(std::nothrow)` and its result is checked (§17).
- [ ] Every lock is a `MutexLocker` (§17).
- [ ] No register access outside `RegisterIO` (§20.1).
- [ ] No blocking work added to a USB callback (§17.1).
- [ ] Every new `status_t` return is checked, with a specific error code
      (§20.2).
- [ ] Any new member of `RTL8814AUDevice` is initialized in the constructor
      (§6).
- [ ] No `goto`, no `#if 0`, no `nullptr`, no `TRUE`/`FALSE`.
- [ ] Copyright header present, GPL v2, `Kevin Adams
      <kevinadams05@gmail.com>` (§16).
- [ ] No code copied from a GPL reference driver — register knowledge only
      (§1.3).
- [ ] Every file ends with a newline, LF endings.
- [ ] Tested on real hardware. This is a kernel driver; a clean build proves
      nothing. Say which adapter and which AP in the PR.

---

## Appendix A — Quick reference card

```
Indent:  TAB (width 4); tabs also align columns in class bodies + reg tables
Line:    target <=80, linter hard-fails at 100
Brace:   class/struct same line; function own line; if/for/while same line
         single-statement body on its OWN line, never after the condition
Naming:  RTL8814AU<Thing> classes; UpperCamel POD structs (no f prefix);
         lowerCamel locals; f members, k consts/enums, g globals, s statics;
         _Private methods; driver entry points stay lower_case_with_underscores
         spell it out: frameLength not frameLen, buffer not buf, context not ctx
Pointer: uint8* buffer = NULL;
Cast:    (uint32)value  - no space; static_cast where it carries meaning
Null:    NULL, never nullptr; no NULL check before delete/free
Bool:    true/false, never TRUE/FALSE; status_t for success, bool for flags
Bitmask: if ((keyInfo & 0x0008) != 0)
Switch:  case indented; { } if it declares vars; default: required
Types:   Haiku types from SupportDefs.h; no long; B_PRIu32/B_PRIuSIZE in
         format strings; int8 for RSSI; uint64 for the 48-bit CCMP PN
Strings: char[N] + strlcpy + snprintf. No BString in the kernel.
Errors:  status_t, B_OK on success, specific error codes, never ignored
Alloc:   new(std::nothrow) + NULL check, always
Locking: MutexLocker, never mutex_lock/mutex_unlock
Guard:   #ifndef RTL8814AU_FOO_H / #define / #endif  // RTL8814AU_FOO_H
Doxygen: /*! ... */ above the definition, body indented FOUR SPACES
License: GPL v2, Kevin Adams <kevinadams05@gmail.com>

rtl8814au rules that are not about formatting:
  every dprintf starts with RTL8814AU_DRIVER_NAME ": ", and is ASCII
  no printf/fprintf in src/ - the kernel has no stdout
  all register access goes through RTL8814AURegisterIO; only it calls
    send_request()
  never block, snooze, or do register I/O in a USB callback - it deadlocks
  hardware init happens on first open(), not at USB attach
  two-phase construction: fInitStatus + InitCheck(), constructors cannot fail
  hardware sequences are tables walked by an interpreter, not open-coded
  no code copied from the GPL reference drivers - register knowledge only
  per-frame logging can panic BFS under log flood; take it back out
```

## Appendix B — Hardware and code quick facts

Facts a contributor needs to keep examples accurate:

- **Chip:** Realtek RTL8814AU, 4x4 802.11ac USB 3.0. `kRTL8814AU_ChipID` =
  `0x8814`.
- **Device node:** `/dev/net/rtl8814au/<index>`, built from
  `RTL8814AU_DEVICE_PATH_BASE` = `"net/rtl8814au"`.
- **Register access:** USB vendor control transfers only. No MMIO.
  `bmRequestType` 0xC0 (read) / 0x40 (write), `bRequest` 0x05, address in
  `wValue`, length 1/2/4.
- **MCU:** Lexra 3081, MIPS-derived. Firmware loads by beacon-queue TX plus
  IDDMA into DMEM and IRAM; halted via `REG_SYS_FUNC_EN` bit 12.
- **Endpoints:** 3 bulk OUT (`kBulkOutEndpointCount`), 1 bulk IN, 1 interrupt
  IN. Beacon queue is pipe index 2, `QSEL` = `kQslBeacon` = 0x10.
- **RX aggregation:** many frames per bulk-IN transfer; each is a 24-byte
  descriptor, an optional 0–64-byte PHY status block, the payload, and padding
  to 128-byte alignment.
- **Supported devices:** `kSupportedDevices[]` of `RTL8814AUDeviceID` in
  `RTL8814AU.h` — nine adapters, matched in `DeviceAdded()`.
- **Classes:** `RTL8814AUDevice` (owns everything), `RTL8814AURegisterIO`,
  `RTL8814AUFirmware`, `RTL8814AUEfuseReader`, `RTL8814AUPhyConfig`,
  `RTL8814AUTxPath`, `RTL8814AURxPath`, `RTL8814AUWiFiManager`, plus the
  `wpa2_crypto` namespace of free functions.
- **Globals:** `gUSBModule`, `gNotificationModule`, `gDeviceListLock`,
  `gDeviceList[kMaxDeviceCount]`, `gDeviceCount` — all declared in `Driver.h`.
- **Threads per open device:** the EAPOL 4-way handshake loop, the
  post-associate H2C worker, and a scan-complete notifier. All woken by
  semaphores; none of them run on the USB callback thread (§17.1).
- **Deeper reading:** `docs/architecture.md`, `docs/hardware-init.md`,
  `docs/firmware.md`, `docs/rx-path.md`, `docs/tx-path.md`,
  `docs/wifi-management.md`, `docs/wpa2-in-driver.md`,
  `docs/ioctl-reference.md`, `docs/build-and-deploy.md`.

## Appendix C — Distribution

rtl8814au ships as a standalone, unofficial `.hpkg` built by
`package/build-hpkg.sh` on the Haiku cross-build server. The package is
`rtl8814au` (GPL v2), and it also provides `cmd:wifi_join`. The repository lives at
`KevinAdams05/RTL8814AU_Haiku`.

It is not part of the Haiku source tree, and since 2026-08-27 it is GPL v2,
which closes the door on contributing it to Haiku's MIT-licensed tree. The
driver-only scope would otherwise have left that open, as would the in-driver
WPA2
architecture (§1.4) is a workaround for a stack-level EAPOL problem and would
have to be unwound first, so that is not a goal today.
