# Changelog

## 0.0.3 "dev" (2026-11-28)

First hardware report (RTL8821CE, Dave McKenna): the RTL8821CE driver loaded silently - no boot message even with /V -
and `\DEV\WRND32$` did not exist; the RTL8821AE driver correctly refused the card and named the right driver.

* Both drivers now print what INIT found without /V: the PCI id, bus / device / function, IRQ and the register BAR,
  and a closing "ready" or "NDIS MAC not registered" line, so a silent boot can be told apart from a driver that did
  not load. (The cause of the silent boot is not known yet; see the README section "If nothing happens at boot".)
* Install examples are split per driver (`STARTUP.AE.CMD.example` / `STARTUP.CE.CMD.example`, `PROTOCOL.INI.AE.example`
  / `.CE.example`); the combined STARTUP example had the RTL8821AE firmware line active, which is wrong for a CE card.

## 0.0.2 "dev" (2026-11-27)

A second driver from the same tree: **RTL8821CE.OS2** for Realtek RTL8821CE cards (PCI `10EC:C821`, `10EC:B821`), a
different chip from the RTL8821AE (Linux drives it with rtw88).

* New back end `src/rtw88/` behind the same `wlan/hw.h`, ported from Linux rtw88: power sequences, firmware download
  (reserved page through the beacon queue, then the DDMA engine into DMEM / IMEM / EMEM), FIFO pages, auto LLT, H2C
  queue, `rtw8821c_mac_init`, eFuse, register tables (two board variants, `tools/gen_tbl88.py`), 20 MHz channel switch,
  eFuse transmit power, firmware IQK, Bluetooth coexistence in "WiFi only" mode, buffer-descriptor rings (BE,
  management, H2C, beacon, RX), 8-word CAM keys with default-key search, port 0 registers, media status H2C.
* `RTL8821CE.FW` = linux-firmware `rtw88/rtw8821c_fw.bin`; `install/RTL8821CE.NIF`; `RTWMAC.CMD` finds the section of
  whichever driver is loaded.
* `os2/drv.h` names the driver by build (`-dR88`); `src/Makefile` and `build.cmd` build both drivers.
* Both drivers build without warnings; **neither has been tried on hardware yet**.

## 0.0.1 "dev" (2026-11-27)

First version: a driver for Realtek RTL8821AE PCI Express cards (`10EC:8821`).

* The OS/2 side (PCI configuration, BAR mapping, interrupt and context hook, DMA memory, NDIS MAC, GenMAC OIDs,
  status and debug channel) and the chip-independent station (scan table, join, WPA2 keys, EAPOL, Ethernet <-> 802.11,
  Block Ack reorder buffer) come from the iwlwifi OS/2 driver (psmedley/iwlwifi). `pci.c` maps the BAR the back end
  names (the RTL8821AE has its registers in BAR2) and `lib.asm` gained 8- and 16-bit MMIO.
* The chip layer is the RTL8821AU driver's (OS2World/DRV-NET-RTLUSBWifi: power-on, LLT, firmware download, MAC / BB /
  AGC / RF tables, band and channel, 5 GHz transmit power, eFuse, firmware IQ calibration, receiver gain, CAM keys, the
  TX descriptor, the RX status, the rate ladder), with the PCIe parts from Linux rtlwifi `rtl8821ae`: the power sequence
  for the PCIe interface, the DMA descriptor rings and their registers, the interrupt registers, the MAC set-up of
  `_rtl8821ae_init_mac` / `_rtl8821ae_hw_configure`, the eFuse MAC address at 0xD0.
* Firmware: `RTL8821AE.FW` = linux-firmware `rtlwifi/rtl8821aefw_29.bin` (redistributable, licence alongside);
  `RTL8821AU.FW` is accepted too.
* 20 MHz, one stream, HT / WMM client, receive A-MPDU; no transmit aggregation or power save.
* Builds (Open Watcom 2.0 + NASM, on OS/2 or cross on Linux) without warnings; **not yet tried on hardware**.
