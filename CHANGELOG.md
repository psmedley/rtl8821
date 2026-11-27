# Changelog

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
