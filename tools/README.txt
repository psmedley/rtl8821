gen_tbl.py flattens the RTL8821A MAC / baseband / AGC / RF tables of the Realtek Linux driver
(hal/phydm/rtl8821a/halhwimg8821a_*.c, from morrownr/8821au-20210708). Run it with Python 3:

  python3 gen_tbl.py <REF> <OUT> pcie          the tables of this driver (src/rtw/rtw_tbl.c)
  python3 gen_tbl.py <REF> <OUT>               the USB tables of the RTL8821AU driver (rtl8821_tbl.c)

It comes from OS2World/DRV-NET-RTLUSBWifi (tools/), where the USB driver lives. The two outputs differ only in some
RF-A entries (the PA / LNA bias tables are conditional on the interface).
