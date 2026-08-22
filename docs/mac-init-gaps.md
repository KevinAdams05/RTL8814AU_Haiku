# MAC registers the vendor initialises and we do not

A systematic sweep for missing initialisation, prompted by finding
`REG_TX_HANG_CTRL` (`0x045E`) missing while chasing an intermittent transmit
stall. That register was found by accident. This is the same question asked
deliberately, of every MAC-range register at once.

Regenerate with `scratchpad/mac-init-gaps.py <decoded-capture.txt>`.

## Method, and why the naive version is useless

The comparison is between the registers the vendor driver writes during
initialisation — taken from a decoded `usbmon` capture, not from its source —
and the registers this driver writes anywhere. Two normalisations are
essential:

- **The vendor writes many registers one byte at a time**, each byte a
  separate USB control transfer, while this driver writes 16 or 32 bits at the
  group base. Comparing raw addresses reports about 160 "missing" registers,
  nearly all of them bytes 1-3 of something we do write. So each vendor write
  is expanded across its width, and any register we write is treated as
  covering its whole 4-byte group.
- **Some registers are runtime, not init**: the H2C mailboxes, our own MAC
  address, the BSSID. Left in, they dominate the output.

Contiguous gaps are collapsed into runs, so a byte-wise table reads as one
entry rather than sixteen.

Registers at `0x0800` and above are excluded: those are BB and RF, and the PHY
initialisation replay covers them.

## The gaps

277 vendor MAC-range registers, 24 gap runs. **Unverified**: this is a list of
differences, not of bugs. A register we never write may be one the chip does
not need in our configuration.

| Registers | Values | Notes |
|---|---|---|
| `0x011C-0x011D` | `5BFF` | |
| `0x01CC` | `0F` | |
| `0x0480` | `20` | |
| `0x0483` | `00` | written 32 times — likely a sequence or toggle |
| `0x04A0-0x04A3` | `E0 03 00 40` | ARFR4 region |
| `0x04A8-0x04AB` | `E0 00 00 00` | ARFR5 region |
| `0x04C6` | `04` | |
| `0x055C-0x055D` | `64 FF` | |
| `0x0564-0x0567` | `00000012` | |
| `0x0577` | `03` | |
| `0x05BE` | `64` | |
| `0x0604-0x0605` | `01 30` | |
| `0x0607` | `01` | |
| `0x0624-0x0627` | `FF FF FF FF` | |
| `0x0652` | `C8` | |
| `0x066E` | `05` | |
| `0x06DE` | `84` | |
| `0x06F4-0x06F5` | `0D0B` | |
| `0x0714-0x0715` | `0000` | |
| `0x0718-0x071B` | `40`, `20020240` | |
| `0x0764-0x0767` | `00000600` | |
| `0x07D5` | `BC` | |
| `0x07D8-0x07DA` | `28 00 08` | |

The `0x04A0` and `0x04A8` runs independently confirm a gap already recorded in
`NEXT_SESSION.md`: ARFR4 and ARFR5 (`0x049C` and `0x04A4`) are unwritten. Worth
noting that the ARFR registers on this chip are 64-bit and non-contiguous —
`0x0444`, `0x044C`, `0x048C`, `0x0494`, `0x049C`, `0x04A4` — so the four-byte
stride in `hal_com_reg.h` does not apply here.

## How to use this list

**One at a time, measuring in between.** Writing several of these together is
how a previous session lost a day: a WMM information element and QoS EAPOL
frames were introduced in the same change, association broke, and there was no
way to tell which half did it.

The intermittent transmit stall is the reason this list exists, so the
plausible candidates for it — the `0x04xx` transmit-control registers first —
are the ones to try, each on its own, against a measured failure rate rather
than a single run.
