# SPDX-License-Identifier: GPL-2.0-only
# Cross build on Linux with Open Watcom 2.0 (binl64 on PATH, WATCOM set) and NASM:  make
# On OS/2 use build.cmd (same flags). Two drivers come out of this tree:
#   RTL8821AE.OS2  Realtek RTL8821AE (PCI 10EC:8821)        back end rtw/   objects in obj/ae/
#   RTL8821CE.OS2  Realtek RTL8821CE (PCI 10EC:C821, B821)  back end rtw88/ objects in obj/ce/ (built with -dR88)
#
#   os2/   OS/2 plumbing: device header, DevHelp glue, strategy routine, NDIS MAC, GenMAC OIDs, DMA memory, PCI
#          (from the iwlwifi OS/2 driver, github.com/psmedley/iwlwifi)
#   wlan/  the chip-independent station (scan table, join, keys, EAPOL, Ethernet <-> 802.11) and hw.h
#   rtw/   the RTL8821AE back end (chip layer from the RTL8821AU driver, PCIe DMA from Linux rtlwifi)
#   rtw88/ the RTL8821CE back end (Linux rtw88)
CFLAGS = -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan
ASM    = hdr lib ndisx end
OS2C   = strat pci mem ndis genmac dbg
WLANC  = sta
RTWC   = rtw_io rtw_hal rtw_phy rtw_trx rtw_sta rtw_tbl
R88C   = r88_io r88_hal r88_phy r88_trx r88_sta r88_tbl
HDRS   = os2/drv.h os2/os2dd.h os2/ndis.h wlan/hw.h wlan/wlan.h

AE_OBJS = $(addprefix obj/ae/,$(addsuffix .obj,$(ASM) $(OS2C) $(WLANC) $(RTWC)))
CE_OBJS = $(addprefix obj/ce/,$(addsuffix .obj,$(ASM) $(OS2C) $(WLANC) $(R88C)))

all: RTL8821AE.OS2 RTL8821CE.OS2

RTL8821AE.OS2: $(AE_OBJS) rtl8821ae.lnk
	wlink @rtl8821ae.lnk

RTL8821CE.OS2: $(CE_OBJS) rtl8821ce.lnk
	wlink @rtl8821ce.lnk

obj/ae obj/ce:
	mkdir -p $@

obj/ae/%.obj: os2/%.asm | obj/ae
	nasm -f obj $< -o $@
obj/ce/%.obj: os2/%.asm | obj/ce
	nasm -f obj $< -o $@

obj/ae/%.obj: os2/%.c $(HDRS) rtw/rtw.h | obj/ae
	wcc $(CFLAGS) -i=rtw -fo=$@ $<
obj/ae/%.obj: wlan/%.c $(HDRS) rtw/rtw.h | obj/ae
	wcc $(CFLAGS) -i=rtw -fo=$@ $<
obj/ae/%.obj: rtw/%.c $(HDRS) rtw/rtw.h | obj/ae
	wcc $(CFLAGS) -i=rtw -fo=$@ $<

obj/ce/%.obj: os2/%.c $(HDRS) rtw88/r88.h | obj/ce
	wcc $(CFLAGS) -dR88 -i=rtw88 -fo=$@ $<
obj/ce/%.obj: wlan/%.c $(HDRS) rtw88/r88.h | obj/ce
	wcc $(CFLAGS) -dR88 -i=rtw88 -fo=$@ $<
obj/ce/%.obj: rtw88/%.c $(HDRS) rtw88/r88.h | obj/ce
	wcc $(CFLAGS) -dR88 -i=rtw88 -fo=$@ $<

clean:
	rm -rf obj RTL8821AE.OS2 RTL8821AE.map RTL8821CE.OS2 RTL8821CE.map

.PHONY: all clean
