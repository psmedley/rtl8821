# RTL8821AE / RTL8821CE - Realtek PCIe WiFi for OS/2 and ArcaOS

Native drivers for two Realtek 802.11ac one-stream PCI Express adapters (mini PCIe and M.2 cards):

| Adapter | PCI id | Driver | Firmware | Back end | State |
|---|---|---|---|---|---|
| Realtek **RTL8821AE** | `10EC:8821` | `RTL8821AE.OS2` | `RTL8821AE.FW` | `src/rtw/` | builds, not yet tried on hardware |
| Realtek **RTL8821CE** | `10EC:C821`, `10EC:B821` | `RTL8821CE.OS2` | `RTL8821CE.FW` | `src/rtw88/` | builds, not yet tried on hardware |

**Which one is yours?** The PCI device id tells (the Hardware Manager, `pci.exe`, or the `ID=` line of
`copy \DEV\WRND32$ con` with either driver loaded): `8821` is an RTL8821AE, `C821` or `B821` is an RTL8821CE. The two
chips share a name but not a design: Linux drives the 8821AE with rtlwifi and the 8821CE with rtw88. Each driver
refuses the other's card and says so at boot.

Like the other drivers of their family, each is one 16-bit physical device driver that shows the adapter to the system
in two ways:

* a **GenMAC-compatible device** (`\DEV\WRND32$`), so the ArcaOS **Wireless LAN Monitor** and `wpa_supplicant` manage it,
* an **NDIS 2.0.1 MAC** (`RTL8821AE_NIF` / `RTL8821CE_NIF`), so the OS/2 TCP/IP stack binds to it as `lanN`.

They are put together from working drivers:

| Part | Comes from |
|---|---|
| OS/2 plumbing: device header, DevHelp glue, strategy routine, PCI configuration, BAR mapping, interrupt and context hook, DMA memory, NDIS MAC, GenMAC OIDs, status and debug channel (`src/os2/`) | the **iwlwifi** OS/2 driver ([psmedley/iwlwifi](https://github.com/psmedley/iwlwifi)), itself from the QCA6174 and RTL88EU drivers |
| The chip-independent station: scan table, authentication / association, WPA2 keys from the supplicant, EAPOL queue, Ethernet <-> 802.11, Block Ack reorder buffer (`src/wlan/`) | the same iwlwifi driver, unchanged apart from one debug hook (`hw_cmd`) |
| RTL8821AE chip layer (`src/rtw/`): power-on, LLT, firmware download, MAC / BB / AGC / RF tables, band and channel, 5 GHz transmit power, eFuse, firmware IQ calibration, receiver gain, CAM keys, TX descriptor, RX status, rate ladder | the **RTL8821AU** USB driver of OS2World's [DRV-NET-RTLUSBWifi](https://github.com/OS2World/DRV-NET-RTLUSBWifi) ([`rtl8821au/`](https://github.com/OS2World/DRV-NET-RTLUSBWifi/tree/main/rtl8821au)) - the same chip behind USB |
| RTL8821AE PCIe side: power sequence, DMA descriptor rings, interrupt registers, TX doorbell | Linux **rtlwifi** `rtl8821ae` |
| RTL8821CE back end (`src/rtw88/`): power sequences, firmware download through the reserved page and the DDMA engine, FIFO / LLT / H2C queue set-up, MAC init, register tables, channel switch, eFuse, WiFi-only Bluetooth coexistence, buffer-descriptor rings, keys, port registers | Linux **rtw88** (`main.c`, `mac.c`, `pci.c`, `fw.c`, `phy.c`, `efuse.c`, `tx.c`, `sec.c`, `coex.c`, `rtw8821c.c`, `rtw8821c_table.c`); the station steps and the rate ladder as in the 8821AE back end |

> **Status: written, builds, not yet tried on hardware (0.0.2, build "dev").** Every register sequence is taken from
> one of the sources above, but none of the PCIe code has run on a real card yet. Expect the first runs to need
> fixes; the status text (`copy \DEV\WRND32$ con`) and the debug commands below are there to find them. Where to
> look first: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) section 7 (RTL8821AE) and [docs/RTL8821CE.md](docs/RTL8821CE.md)
> section 6 (RTL8821CE).

## What they do (once they run)

* Scan of 2.4 GHz (1-13) and 5 GHz (36-64, 100-140 passive, 149-165), the networks in the Wireless LAN Monitor.
* Join an open or WPA2-PSK (CCMP) network: open authentication, association (HT 20 MHz and WMM when the access point
  has them), the 4-way and group handshakes through `wpa_supplicant`, keys in the chip's CAM (hardware CCMP).
* IP over WiFi through the normal TCP/IP stack: configure `lanN` for DHCP or a static address as for any adapter
  (unlike the USB drivers, there is no DHCP client inside these drivers).
* Receive aggregation (A-MPDU): the chip answers with block acks, `wlan/sta.c` puts frames back in order.
* Transmit rates from the signal strength (the 8821AU ladder: OFDM up to 36 Mbit/s, CCK at the edge of 2.4 GHz),
  handshake / ARP / DHCP frames at 1 Mbit/s (6 Mbit/s on 5 GHz); receiver gain follows the signal.
* Beacon loss (10 s) is reported to the station, which leaves; the supplicant or the monitor rejoins.
* RTL8821CE: the antenna and the radio grant are given to WiFi (coexistence "WiFi only"); the firmware calibrates
  (IQK) before each association.

Not done: 40 / 80 MHz, transmit aggregation, power save, Bluetooth (both chips are WiFi + Bluetooth combos; the
Bluetooth half is not used), WEP / TKIP / WPA3, the RTL8812AE (2x2) and the RTL8822 family; for the RTL8821CE also
the per-rate transmit power table, the regulatory power limits and thermal power tracking (the eFuse base power is
used, as in the RTL8821AU / AE drivers).

## Install (for testing)

`xx` below is `AE` or `CE`, as the table at the top says.

1. Copy `bin\RTL8821xx.OS2`, `bin\RTL8821xx.FW` and `install\RTL8821xx.NIF` to `C:\IBMCOM\MACS`.
2. `CONFIG.SYS`: `DEVICE=C:\IBMCOM\MACS\RTL8821xx.OS2 /V`. Only one `WRND32$` driver can be loaded: remove or `REM`
   GenMAC, the RTL USB drivers, the other driver of this package, the iwlwifi or the QCA6174 driver line.
3. `PROTOCOL.INI`: bind `RTL8821xx_NIF` as in `install\PROTOCOL.INI.AE.example` or `.CE.example` (MPTS can do it too, with the NIF).
4. `STARTUP.CMD`: the `copy /b` line for your driver from `install\STARTUP.AE.CMD.example` or `STARTUP.CE.CMD.example` (the firmware, once per boot).
5. Reboot. `copy \DEV\WRND32$ con` shows the state; `STATE=0064 READY` means the chip is up. Then `RTWMAC.CMD`
   writes the adapter's MAC into `PROTOCOL.INI` (recommended: TCP/IP copies the address when it binds).
6. Use the Wireless LAN Monitor, or for a first test of an open network: `echo join MyNetwork > \DEV\WRND32$`.

Firmware, both Realtek's from linux-firmware (the files Linux loads for these chips), redistributed under
`bin\LICENCE.rtlwifi_firmware.txt`:

* `RTL8821AE.FW` = `rtlwifi/rtl8821aefw_29.bin`. The RTL8821AU driver's `RTL8821AU.FW` (Realtek's version 42 "nic"
  image) is accepted as well; with it the firmware IQ calibration runs after 5 GHz joins.
* `RTL8821CE.FW` = `rtw88/rtw8821c_fw.bin` (version 24.11).

## If nothing happens at boot

Since 0.0.3 each driver prints its findings at boot without `/V` (silence them with `/Q`):

```
RTL8821CE: found 10EC:C821 bus 03 dev 00 fn 0 IRQ 0B registers at F7D00000
RTL8821CE: ready; copy the firmware file to \DEV\WRND32$ (STARTUP.CMD).
```

* **No `RTL8821xx:` line at all**: the driver was not loaded or did not get as far as printing. Check the `DEVICE=`
  line (full path, the right file for your card) and that it comes after `PROTMAN.OS2` and the other NDIS lines.
  `/B` makes the driver beep the number of the step it reached (n short beeps, a pause, again) - the last group
  counted is where it stopped; `/S1` ... `/S9` end the start-up after that step (to find a step that hangs the boot).
* **"no ... adapter found"**: the driver and the card do not match (the message names the right driver), or OS/2 does not see
  the card (then `pci.exe` / the Hardware Manager will not list it either: check the BIOS and the slot).
* **"found ..." but then `path not found` for `\DEV\WRND32$`**: should not happen; please send the lines the driver printed.
* **"NDIS MAC is not registered"**: `[RTL8821xx_NIF]` is missing in `PROTOCOL.INI` or `PROTMAN$` is loaded after the driver.
  The adapter still works as a GenMAC device, but TCP/IP cannot bind it.

## Reading the state

`copy \DEV\WRND32$ con` (or `> file`) prints `KEY=VALUE` lines. The general ones are those of the iwlwifi driver
(`STATE`, `MAC`/`HWMAC`, `LINK`, `DATA`, `NDIS`, `BA`, `NET=` per network, the last GenMAC requests). The back ends add:

RTL8821AE:

| Line | Meaning |
|---|---|
| `RTW step= warm= SYSCFG= cut= pwrfail= lltfail=` | bring-up step (docs/ARCHITECTURE.md section 5), whether the chip was still on, `REG_SYS_CFG`, the failing power-sequence entry, the failing LLT entry |
| `FW ver= sub= tries= chksum= ready= MCUFWDL= H2CBUSY=` | firmware version, download attempts, checksum report, `WINTINI_RDY`, the register, firmware commands that found no free box |
| `EFUSE ok= used= xtal= rfe= pa2g/lna2g/pa5g/lna5g= thermal=` | the eFuse: decoded, bytes used, crystal trim, RFE type, external amplifiers |
| `PHY ch= band= igi= txp5= ofdm= ht= grp= IQK ...` | channel, band, receiver gain, 5 GHz power indexes, IQ calibration counters |
| `CHK`, `REGS`, `TRX`, `RX`, `TX` | read-backs after bring-up, live registers, ring positions and counters |

RTL8821CE (details in docs/RTL8821CE.md):

| Line | Meaning |
|---|---|
| `R88 step= warm= SYSCFG1= cut= pwrfail=` | bring-up step, whether the chip was still on, `REG_SYS_CFG1`, the failing power-sequence entry |
| `FW ver= sub= h2cfmt= FWERR= MCUFW= FWDBG7= H2CBUSY= H2CSEQ=` | firmware header, where a download failed, `REG_MCUFW_CTRL` (`xxxxC078` = running), `REG_FW_DBG7` |
| `EFUSE ok= rfe= pkg1= btg= tables= xtal= board= bt= swing= thermal= ant=` | eFuse fields, the register-table variant in use, whether the antenna switch was set |
| `PHY ch= band= igi= txp cck/ofdm/ht= IQK ...` | channel, receiver gain, transmit power indexes, IQK counters |
| `REGS`, `TRX`, `RX`, `TX` | live registers (ring indexes included), ring positions and counters |

## Debug commands

`echo <command> > \DEV\WRND32$` (note the space before `>`). Values are hexadecimal; results go to COM1.

Both drivers: `up` (retry a failed start), `scan`, `join <ssid>` (open network), `leave`, `r <reg>` (32-bit read),
`r1 <reg>`, `w1|w2|w4 <reg> <value>`, `rf <reg> [value]`, `ch <channel>` (not while joined; `ch 24` = channel 36),
`iqk <mode>`, `dig <igi>` / `dig 0` (fixed / automatic receiver gain), `rate <code>` / `rate ff` (fixed / automatic
data rate: 0-3 CCK, 4-B OFDM 6-54), `ctl <code>`, `down`.
RTL8821AE only: `txp 0|1` (5 GHz eFuse power), `bsw 0|1` (external antenna band switch of single-antenna boards), `lck`.

## Building

Linux (cross): Open Watcom 2.0 `binl64` and NASM on the `PATH`, `WATCOM` set, `INCLUDE` empty, then `make -C src`
(both drivers). OS/2: `build.cmd` in `src`. Flags and rules: [BUILD.md](BUILD.md). The register tables are generated:
`src/rtw/rtw_tbl.c` by `tools/gen_tbl.py ... pcie`, `src/rtw88/r88_tbl.c` by `tools/gen_tbl88.py`. Host tests of the
shared station code: `sh tests/llc/run.sh`, `sh tests/ba_reorder/run.sh`.

## Layout

```
src/os2/    OS/2 plumbing (from the iwlwifi driver; pci.c reads the BAR the back end names, lib.asm has 8/16-bit MMIO;
            drv.h names the driver: RTL8821CE with -dR88)
src/wlan/   the station (from the iwlwifi driver) and hw.h, the contract a chip back end implements
src/rtw/    the RTL8821AE back end
src/rtw88/  the RTL8821CE back end
install/    NIFs, PROTOCOL.INI and STARTUP.CMD examples, RTWMAC.CMD
bin/        RTL8821AE.OS2 + .FW, RTL8821CE.OS2 + .FW, the firmware licence
docs/       ARCHITECTURE.md (both, RTL8821AE in detail), RTL8821CE.md
tests/      host tests of wlan/sta.c (from the iwlwifi driver)
tools/      gen_tbl.py (RTL8821A tables), gen_tbl88.py (RTL8821C tables)
```

Source comments that name `rtl8821au/src/...` mean the RTL8821AU driver in
[OS2World/DRV-NET-RTLUSBWifi](https://github.com/OS2World/DRV-NET-RTLUSBWifi/tree/main/rtl8821au/src), where the
RTL8821AE driver started.

## Licence

GPL-2.0-only (`COPYING`). The chip code derives from Realtek's Linux drivers: the vendor 8821au driver (GPL-2.0,
through `rtl8821au/`), rtlwifi (GPL-2.0) and rtw88 (GPL-2.0 OR BSD-3-Clause, used here under the GPL); the OS/2 and
station code is the iwlwifi driver's (GPL-2.0-only). The firmware is Realtek's binary, redistributable under its own
licence. See [PROVENANCE.md](PROVENANCE.md).
