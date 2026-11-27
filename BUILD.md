# Building

Open Watcom C 2.0 (16-bit `wcc`, `wlink`) and NASM. Nothing from the IBM DDK.

## Linux (cross build)

```sh
export WATCOM=/path/to/open-watcom-2
export PATH=$WATCOM/binl64:$WATCOM/binl:$PATH
export INCLUDE=
make -C src
```

The results are `src/RTL8821AE.OS2` and `src/RTL8821CE.OS2` (16-bit NE modules) with their link maps; objects go to
`obj/ae/` and `obj/ce/` (the `os2/` and `wlan/` sources are compiled once per driver; the RTL8821CE build adds
`-dR88 -i=rtw88`, the RTL8821AE build `-i=rtw`). `make -C src clean` removes `obj/` and the outputs. The current Open Watcom v2 snapshot (`ow-snapshot.tar.xz` from the project's GitHub
releases) works.

## OS/2 / ArcaOS

Open Watcom `binp` and NASM on the `PATH`, `WATCOM` set, `INCLUDE` empty; in `src` run `build.cmd`.

## Flags and rules

`wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=rtw` (RTL8821AE)
`wcc ... -i=os2 -i=wlan -dR88 -i=rtw88` (RTL8821CE)

* `-2`: 80286 code. 32-bit registers are only used in `os2/lib.asm` (MMIO, port I/O, locks).
* `-ms -zu`: small model, SS is not DS. **Never pass the address of a stack variable** to a function that takes a near
  pointer: the compiler warns "W112 Pointer truncated" - treat every such warning as a bug (make the variable `static`).
* `UCHAR FAR *a, *b;` declares `b` as a *near* pointer. Write `UCHAR FAR *a, FAR *b;`.
* No C runtime (`-zl`, `nodefaultlibs`): no 32-bit multiply, divide or variable shift (`u32_div()` in `os2/dbg.c`
  divides; `shl1` / `shr1` in `rtw/rtw_io.c` shift by one). Constant shifts of ULONGs are fine.
* `hdr.obj` must be linked first (device header at offset 0), `end.obj` last (resident-end markers).
* DGROUP (header, data, BSS) must stay below 64 KB: about 36 KB for the RTL8821AE, about 56 KB for the RTL8821CE (its
  register tables are 24 KB of that - add statics with care). The code segments are about 37 and 39 KB. Rings and
  buffers live in DMA memory.

## Generated tables

RTL8821CE: `src/rtw88/r88_tbl.c` is written by `tools/gen_tbl88.py` from Linux rtw88's `rtw8821c_table.c`:

```sh
python3 tools/gen_tbl88.py <linux>/drivers/net/wireless/realtek/rtw88/rtw8821c_table.c src/rtw88/r88_tbl.c
```

RTL8821AE: `src/rtw/rtw_tbl.c` is written by `tools/gen_tbl.py` from the Realtek vendor tables (`hal/phydm/rtl8821a/
halhwimg8821a_mac.c`, `_bb.c`, `_rf.c` of morrownr/8821au-20210708), evaluated for a PCIe interface:

```sh
python3 tools/gen_tbl.py <8821au-20210708>/hal/phydm/rtl8821a src/rtw/rtw_tbl.c pcie
```

## Host tests

`sh tests/llc/run.sh` and `sh tests/ba_reorder/run.sh` (gcc) test the Ethernet <-> 802.11 conversion and the Block Ack
reorder buffer of `src/wlan/sta.c`.
