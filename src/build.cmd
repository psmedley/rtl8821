@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Build RTL8821AE.OS2 and RTL8821CE.OS2 on OS/2 / ArcaOS with Open Watcom 2.0 (binp on PATH, WATCOM set) + NASM.
rem usage: build    (run in src; objects go to obj\ae\ and obj\ce\, the drivers to RTL8821AE.OS2 and RTL8821CE.OS2)
rem The compiler flags are written out on each wcc line: CMD.EXE does not expand %VAR% the same way
rem inside a FOR command, and wcc then sees the flags as extra file names (E1139).
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binp;C:\usr\bin;%PATH%
set beginlibpath=%WATCOM%\binp\dll
set INCLUDE=
if not exist obj mkdir obj
if not exist obj\ae mkdir obj\ae
if not exist obj\ce mkdir obj\ce
rem ---- RTL8821AE (back end rtw)
for %%f in (hdr lib ndisx end) do nasm -f obj os2\%%f.asm -o obj\ae\%%f.obj
for %%f in (strat pci mem ndis genmac dbg) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=rtw -fo=obj\ae\%%f.obj os2\%%f.c
for %%f in (sta) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=rtw -fo=obj\ae\%%f.obj wlan\%%f.c
for %%f in (rtw_io rtw_hal rtw_phy rtw_trx rtw_sta rtw_tbl) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=rtw -fo=obj\ae\%%f.obj rtw\%%f.c
wlink @rtl8821ae.lnk
rem ---- RTL8821CE (back end rtw88, -dR88)
for %%f in (hdr lib ndisx end) do nasm -f obj os2\%%f.asm -o obj\ce\%%f.obj
for %%f in (strat pci mem ndis genmac dbg) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -dR88 -i=rtw88 -fo=obj\ce\%%f.obj os2\%%f.c
for %%f in (sta) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -dR88 -i=rtw88 -fo=obj\ce\%%f.obj wlan\%%f.c
for %%f in (r88_io r88_hal r88_phy r88_trx r88_sta r88_tbl) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -dR88 -i=rtw88 -fo=obj\ce\%%f.obj rtw88\%%f.c
wlink @rtl8821ce.lnk
