# RTL8821CE back end (src/rtw88/)

The RTL8821CE (PCI `10EC:C821`, `10EC:B821`) is the RTL8821C chip, a newer design than the RTL8821A of the
RTL8821AE: Linux drives it with **rtw88**, not rtlwifi. `RTL8821CE.OS2` is the same driver container as
`RTL8821AE.OS2` (the iwlwifi driver's `os2/` and `wlan/`, built with `-dR88` so it is named RTL8821CE) with this back
end behind `wlan/hw.h`. The station logic (scan by channel sweep, join steps, keys from the supplicant, CCMP header
from the driver, fixed-rate ladder, receiver gain, beacon supervision) is the RTL8821AE back end's; every chip
register sequence follows rtw88.

## 1. What is different from the RTL8821AE

| | RTL8821AE (rtw/, rtlwifi) | RTL8821CE (rtw88/, rtw88) |
|---|---|---|
| CPU / firmware | 8051, 28-31 KB image written page by page into 0x1000 | WLAN CPU with IMEM / DMEM / EMEM; the 136 KB image goes in 4 KB pieces through the beacon queue into the TX buffer, and the on-chip DDMA engine copies each piece to CPU memory with a running checksum |
| DMA | descriptor chains with an OWN bit, doorbell bits in 0x300 | buffer-descriptor rings: host / hardware index pairs per queue (0x3A0..0x3B8, H2C 0x132C), descriptor and frame as two segments |
| TX descriptor | 40 bytes | 48 bytes |
| packet buffer | LLT written entry by entry | automatic LLT (`REG_AUTO_LLT_V1`), FIFO page counts per priority, a reserved area for firmware use and an H2C queue |
| firmware commands | 4-byte mailboxes | 8-byte mailboxes and 32-byte H2C packets on their own ring (general info, PHYDM info, IQK) |
| RF access | 3-wire write, slow read through the BB | direct read at 0x2800 + 4 x reg, SIPI write at 0xC90 |
| tables | one set (rtlwifi / vendor), interface-conditional | rtw88 tables with conditions on RFE type, package, interface; two variants for PCIe (`tools/gen_tbl88.py`) |
| Bluetooth combo | not touched | coexistence set to "WiFi only": GNT_WL high, GNT_BT low through the LTE-coex indirect registers, WiFi owns the RF path, antenna switch to the WLG side |
| IQ calibration | optional firmware command, C2H report | firmware IQK on request (H2C packet), finished when RF 0x08 reads 0xABCDE |

## 2. Files

| File | Role | rtw88 source |
|---|---|---|
| `r88.h` | registers, descriptor constants, interfaces | `reg.h`, `pci.h`, `rtw8821c.h`, `fw.h`, `tx.h`, `rx.h` |
| `r88_io.c` | read-modify-write helpers, `hw_ready` (`check_hw_ready`), RF read / write, LTE-coex indirect access, H2C mailbox and packet | `hci.h`, `util.c`, `phy.c`, `fw.c` |
| `r88_hal.c` | device table, power sequences, pre-system / system config, firmware download, eFuse, MAC / FIFO / H2C set-up, coexistence, `hw_up` / `hw_down`, status, debug commands | `main.c`, `mac.c`, `efuse.c`, `coex.c`, `rtw8821c.c` |
| `r88_phy.c` | table loading, channel switch (BB, swing, MAC clock, RF band and RF switch set, RX DFIR), TX power from the eFuse, IQK, receiver gain | `rtw8821c.c`, `phy.c`, `mac.c` |
| `r88_trx.c` | rings, TX (BE, management, H2C, beacon / reserved page), RX, interrupt, service path | `pci.c`, `tx.c`, `rtw8821c.c` (RX descriptor, PHY status) |
| `r88_sta.c` | hw.h station operations: scan, join, leave, keys, frames, rates, link supervision | `sec.c`, `mac80211.c` (port 0, keys), `fw.c` (media status) |
| `r88_tbl.c` | MAC (138), BB (1669), AGC (386), AGC-BTG (128), RF-A (586) entries, two variants | `rtw8821c_table.c` |

## 3. Bring-up (`hw_up`; `STATE=` / `R88 step=`)

| Step | What |
|---|---|
| 1 | firmware file present |
| 2 | PCI: memory + bus master, INTx on, ASPM off; BAR2 mapped |
| 3 | rings allocated, interrupt hooked (or polled); chip answers (`REG_SYS_CFG1`); PCIe PHY parameter (MDIO 0x09 = 0x6380), completion timeout off |
| 4 | rings to the chip (`rtw_pci_setup`), pre-system config, power-on flow (a chip still on is powered off first), system config (DDMA on, CPU domain, boot from flash off) |
| 5 | firmware: WLAN CPU off, high queue and beacon functions set up for the download, platform reset, DMEM / IMEM / EMEM through reserved page + DDMA, checksums, `FW_DW_RDY`, CPU on, wait for `REG_MCUFW_CTRL` = `C078`, rings reset, H2C "recover BT device" |
| 6 | eFuse: MAC address (0xD0), RFE type and package (0xCA), crystal, swing, board / BT options, TX power bases |
| 7 | MAC: TX DMA queue mapping, FIFO pages (512 x 128 bytes, 52 reserved from page 460), auto LLT, H2C queue (pages 500-507), `rtw8821c_mac_init`, driver info / PHY status |
| 8 | PHY: BB / RF on, tables (variant by RFE / package; AGC-BTG for RFE 2 and 4), crystal, channel 1 |
| 9 | interrupts on, H2C general info (firmware TX boundary 48) and PHYDM info (RFE, 1T1R, cut, antenna A), coexistence (WiFi only) |
| 10 | security engine (`SECCFG` 0xCF: default-key search), port 0 address, net type "no link", receive filter |
| 64 | **READY** |

## 4. Memory

DMA memory from `os2/mem.c`: RX ring (32 x 8 bytes) and 32 buffers of 4 KB; BE ring 32 entries, management 8, H2C 8,
each entry with its own buffer (48-byte descriptor + frame: 1648 / 560 / 80 bytes); the beacon queue's one entry with a
4144-byte buffer (it carries the firmware pieces); one two-entry ring for the queues that are not used (BK, VI, VO,
HI0). About 200 KB. The firmware file is kept in `g_fwblob` (os2/mem.c). DGROUP about 56 KB (the register tables are
24 KB of it), code about 39 KB.

## 5. Station details

* RX filter: `RCR` = rtw88's default without `APP_FCS` (wlan/sta.c expects frames without the FCS); "our BSS only"
  (`CBSSID_DATA | CBSSID_BCN`) once associated.
* Join: keys cleared, BSSID written (mac80211 sets it before authenticating), channel, firmware IQK; after the
  association: net type "associated" (CR bits 17:16 = 2), AID (0x6A8), beacon interval, H2C media status "connect"
  for MAC id 0. Leave: the reverse.
* Keys: default-key search on, as rtw88: group keys in CAM entries 0-3 by key id (broadcast address, group bit), the
  pairwise key in entry 4 with the AP's address. Eight words per entry.
* TX descriptor: MAC id 0, QSEL TID 0 (data, BE ring) or management, fixed rate (`USE_RATE`, no fallback), CCMP by
  the chip (`SEC_TYPE` 3) with the 8-byte header from the driver; management frames numbered by the chip
  (`EN_HWSEQ`), data frames with wlan/sta.c's sequence number (`SW_SEQ`).
* RX: [24-byte descriptor][shift][PHY status][frame]; C2H reports are counted and dropped (nothing waits for one:
  the IQK end is seen in RF 0x08). Signal strength: page 0 (CCK) from the LNA / VGA indexes and the board's LNA gain
  table, page 1 from path-A PWDB - 110.

## 6. What to look at first (open questions for the first hardware runs)

1. **Power-on.** `pwrfail=` names the entry of `g_pw_on` (r88_hal.c) that timed out; the driver already retries a
   poll with `BIT_PFM_WOWL` toggled, as rtw88 does on PCIe.
2. **Firmware download.** `FWERR=`: 1 header / file size, 2 LTE-coex register not ready, 3 the reserved-page write
   did not complete (`BCN_VALID` never set: beacon ring, `TXBD_BCN_WORK`), 4 DDMA did not finish, 5 checksum wrong,
   6 LTE-coex restore, 7 the firmware did not start (`MCUFW=` should end in `C078`; `FWDBG7=FAAAAAxx` = invalid key).
   This is the step with the least precedent in the other OS/2 drivers.
3. **MAC set-up.** `STATE=0007` failing: 1 = auto LLT did not finish, 2 = H2C queue pointers differ.
4. **Receive.** After a scan, `RX frames=` should count beacons. If not: `RXBDIDX=` (hardware index in bits 27:16
   should move), `HISR0` bit 0 (`ROK`), `rdu=`. Without interrupts the driver still runs from the GenMAC requests.
5. **Transmit.** `TX mgmt=` counts probe requests queued; `MGIDX=` shows host index (bits 11:0) and hardware index
   (27:16): if the hardware index never follows, the TX ring set-up or the descriptor is wrong.
6. **Antenna.** `ant=1` means the antenna switch was set for the board's RFE type. If nothing is heard at all, the
   coexistence grants are the first suspect (`r 1700`-`1708`, the LTE-coex indirect registers; 0x38 is the grant word).
7. **Unsupported RFE types.** Linux supports RFE 0, 2, 4, 6; other boards get the default tables and a warning.
