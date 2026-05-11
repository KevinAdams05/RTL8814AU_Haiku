>[!NOTE]
>An LLM was used to aid in development of this code.

# Hardware initialization

The bring-up sequence the driver runs the first time the device is
opened.  About a second of work, mostly USB control transfers.

## Overview

| Phase | What | Where |
|---|---|---|
| 1. USB enumeration | Endpoint discovery, configuration set | `Device.cpp::_SetupEndpoints` |
| 2. Power-on | Clock setup, regulator wake, MAC reset | `Device.cpp::_PowerOnSequence` |
| 3. EFUSE read | MAC, antenna config, RFE type, thermal cal | `EfuseReader.cpp` |
| 4. MAC init | Page allocation, queue priorities, TRX DMA | `Device.cpp::_InitMAC` |
| 5. Firmware load | DMEM + IRAM via IDDMA | `Firmware.cpp`, see [firmware.md](firmware.md) |
| 6. PHY init | BB / AGC / RF table replay, channel set, IQ cal | `PhyConfig.cpp` |
| 7. RX aggregation | RXDMA aggregation thresholds, FLTMAP | `Device.cpp::_InitRxAggregation`, `_OpenRxFltmap` |
| 8. MAC enable | Set REG_CR TX/RX bits, configure TRX path | `Device.cpp::_EnableMacTxRx`, `_ConfigTrxPath` |
| 9. RX path | Submit initial bulk-IN URBs | `RxPath.cpp::Start` |
| 10. WiFi management | Submit interrupt-IN URB for C2H events | `WiFiManagement.cpp::Start` |

## Power-on sequence

The cold-start power-on writes about 30 registers.  Mirrored from
morrownr's Linux driver but written from scratch.  Highlights:

- 0x10C2 workaround: write byte `0x02` (USB voltage stabilization).
  Without this, some boards refuse to enumerate cleanly.
- SYS_CLKR clock-ready poll: bit 1 of REG_SYS_CLKR.  Times out after
  ~50 ms if the chip is wedged.
- APS FSM transition: tells the chip's power-state finite-state
  machine to leave standby.  Times out after ~200 ms if the chip
  doesn't respond.

When power-on returns, `REG_CR` reads `0x603f` (TX/RX path enabled
in MAC, scheduler running).

## EFUSE map

The EFUSE is the chip's electronic fuses — a tiny one-time-programmable
memory that holds factory configuration.  Layout (offsets into the
decoded map):

| Offset | Field | Use |
|---|---|---|
| 0x00E | antenna config | bitmask of which RF paths are wired (this hardware = `12` = paths C+D) |
| 0x010 | RFE type | board variant index, drives RFE pinmux choice |
| 0x100 | thermal calibration | currently unused |
| 0x0D8..0x0DD | MAC address | per-device, e.g. `2c:4d:54:cb:6c:fd` |

## MAC initialization

`_InitMAC` programs:

- **Page allocation** — divides the chip's TX FIFO into queue
  partitions: HPQ=20, LPQ=20, NPQ=20, EPQ=20, PUB=1960 pages, with
  a beacon queue boundary at page 0x07F5.
- **Queue priorities** — `REG_TRXDMA_CTRL = 0xf5b4` mapping AC0..3
  + mgmt + high-priority.
- **LLT (Link List Table)** — per-page next-page pointers for the
  TX FIFO chain.  Triggered by writing `LLT_INIT` to REG_AUTO_LLT
  and polling.

## PHY initialization

The big function.  Three phases:

1.  **BB unlock** — set `REG_SYS_CFG3+2` BIT 1 (`FEN_BB_GLB_RSTn`).
    Without this, every BB-region register write to 0x800–0x1FFF is
    silently dropped by the chip even though the bus reports success.
    Not documented anywhere — found by replaying a working USB
    packet capture and watching for the difference between "register
    writes ignored" and "register writes land".
2.  **Cold-start replay** — write the 3914 BB/AGC/RF table entries
    morrownr's Linux driver writes during cold-start.  The flat
    replay is more reliable than a hand-derived register table
    because we don't have the Realtek datasheet and the table
    relationships are not all obvious from the reference driver.
3.  **IQ calibration** — runs the chip's auto-cal sequence on all 4
    RF paths.  Verifies by reading back `REG_IQK_RPT*`.

## RX subtype filter

After PHY init the chip needs to be told which 802.11 subtypes to
deliver.  We open all of them (`FLTMAP0/1/2 = 0xFFFF`) and rely on
software (`_RxFrameReceived`) to filter mgmt/data/control as needed.
This is broader than what most drivers do, but it gives us
visibility into beacons + probes for scan results without a separate
"scan mode" flag.

## What's *not* configured

- **Hardware CCMP** — `kRegSECCFG` is left at `0` (no chip-side
  encryption).  The chip's HW crypto engine refused to engage for
  our setup despite a chip-CAM-correct programming, so AES-CCMP
  runs in software on the host instead.  See
  [wpa2-in-driver.md](wpa2-in-driver.md) for the full SW CCMP story.
- **A-MPDU aggregation** — currently disabled (`kTxDescAGGEn = 0`
  on data frames).  The morrownr driver has fairly aggressive AMPDU
  but tuning it requires more chip docs than we have today.
- **TX power control** — single value from EFUSE applied; per-rate
  power tables aren't programmed.  Probably fine in normal-range
  conditions, marginal for far-edge clients.
