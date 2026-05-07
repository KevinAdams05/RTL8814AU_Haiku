>[!NOTE]
>An LLM was used to aid in development of this code.

# Firmware

The RTL8814AU has an on-chip Lexra 3081 MIPS MCU that runs firmware
loaded by the host driver at startup.  The firmware blob ships as
`/boot/system/data/firmware/rtl8814au/rtl8814aufw.bin` (~67 KB).

## Blob layout

```
+---------------------------------+   offset 0
| 12-byte header                  |
|   sig:    uint16   = 0x8814     |
|   ver:    uint16                |
|   rsvd:   uint16                |
|   dmemSz: uint16   (5112)       |
|   iramSz: uint32   (61712)      |
+---------------------------------+   offset 12
| DMEM bytes (5112 bytes)         |
+---------------------------------+
| DMEM 8-byte XOR trailer         |
+---------------------------------+   offset 12 + 5120
| IRAM bytes (61712 bytes)        |
+---------------------------------+
| IRAM 8-byte XOR trailer         |
+---------------------------------+   end-of-file
```

> **The 8-byte trailers per section are real bytes in the file but
> NOT counted in the section sizes the header reports.**  The driver
> must extend `dmem_size` and `iram_size` by 8 each before it tells
> the chip how many bytes to expect via IDDMA.  Without this, the
> chip's checksum poll fails and `CPU_DL_READY` never asserts.

This was the primary bring-up gotcha — the morrownr Linux reference
driver hides this in a helper that adds the 8 bytes implicitly.  We
hit it as `firmware load failed: timed out` for hours of debugging.

## IDDMA load procedure

Programming sequence (mirrored from morrownr/8814au, written from
scratch in `Firmware.cpp`):

1.  Enable download mode: write `REG_MCUFWDL = 0x2001`.
2.  Halt the MCU: clear `SYS_FUNC_EN` BIT 10.
3.  Reset the IDDMA engine: `REG_CPU_DMEM_CON = 0x00010000`,
    `DDMA_CH0_CTRL = 0`.
4.  Enable IDDMA: set `REG_CR + 1` BIT 0 (`0x60 → 0x61`).
5.  Prepare the beacon queue (the IDDMA load uses the same TX
    descriptor path).
6.  Clear `ENDPOINT_HALT` on bulk-OUT pipe 0.
7.  For each section (DMEM, then IRAM):
    - Cycle CHKSUM_RST without OWN to clear the stale accumulator.
    - Write `REG_CPU_DMEM_CON` to point at the right destination
      offset.
    - Push the section bytes (including the 8-byte trailer!) via
      bulk-OUT with the right TX descriptor type.
    - Poll `REG_CPU_DMEM_CON` for the ready flag (DMEM_DL_RDY for
      DMEM, IMEM_DL_RDY for IRAM).
8.  Set the firmware-ready flag in `REG_MCUFWDL`.
9.  Resume the MCU: set `SYS_FUNC_EN` BIT 10.
10. Poll `REG_MCUFWDL` for `CPU_DL_READY`.

When `CPU_DL_READY` asserts (typically after one poll once the
trailer bytes are included), the firmware is alive and serving
H2C/C2H mailbox traffic.

## H2C and C2H

Once running, the firmware exposes a host-to-card / card-to-host
mailbox protocol:

- **H2C** (host → card) commands go via 8-byte writes to a mailbox
  region.  We use them for `RA_INFO` (rate adaptation) and
  `MEDIA_STATUS_RPT` (connect/disconnect notification) in the
  post-assoc worker.
- **C2H** (card → host) events come back over the USB interrupt-IN
  endpoint as 4+N-byte messages.  We listen for them in
  `WiFiManagement.cpp` but currently don't dispatch on most.

The firmware also handles per-packet TX rate selection, AMPDU
aggregation, and CCMP encryption once the security CAM is programmed
— so we don't software-encrypt data frames; we just plumb plaintext
in and out.

## Reference

The 23,787-packet `morrownr-coldstart.pcap` USB capture from a
working Linux Mint install on `DevHaikuBox` is the source of
truth for any unclear sequence.  Extract writes with:

```
tshark -r /tmp/morrownr-coldstart.pcap \
    -Y 'usb.setup.bRequest == 5 and usb.bmRequestType == 0x40' \
    -T fields -e frame.number -e usb.setup.wValue -e usb.setup.wLength \
        -e usb.data_fragment > /tmp/cold_writes.txt

# look up specific registers (data is little-endian)
grep '0x0808' /tmp/cold_writes.txt    # OFDMCCK_EN over time
grep '0x0a04' /tmp/cold_writes.txt    # CCK_RX_PATH
grep '0x1002' /tmp/cold_writes.txt    # BB reset/clock
```
