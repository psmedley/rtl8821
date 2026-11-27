/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * r88_io.c - register helpers of the RTL8821CE back end, RF access, firmware commands.
 *
 * From Linux rtw88 (hci.h rtw_write*_set/clr/mask, util.c check_hw_ready and the LTE-coex indirect access, phy.c
 * rtw_phy_read_rf / rtw_phy_write_rf_reg_sipi, fw.c rtw_fw_send_h2c_command / rtw_fw_send_h2c_packet;
 * Copyright(c) 2018-2019 Realtek Corporation, GPL-2.0 OR BSD-3-Clause).
 * Only 32-bit operations the 16-bit compiler inlines are used (no variable shifts, no multiply / divide).
 */
#include "drv.h"
#include "r88.h"

void r88_mdelay(USHORT ms)
{
   dd_msleep(ms ? ms : 1);
}

ULONG shl1(ULONG v)
{
   U32 x;

   x.l = v;
   x.w[1] = (USHORT)((x.w[1] << 1) | (x.w[0] >> 15));
   x.w[0] = (USHORT)(x.w[0] << 1);
   return x.l;
}

ULONG shr1(ULONG v)
{
   U32 x;

   x.l = v;
   x.w[0] = (USHORT)((x.w[0] >> 1) | (x.w[1] << 15));
   x.w[1] = (USHORT)(x.w[1] >> 1);
   return x.l;
}

void w8_set(USHORT a, UCHAR b)   { rtw_w8(a, rtw_r8(a) | b); }
void w8_clr(USHORT a, UCHAR b)   { rtw_w8(a, rtw_r8(a) & (UCHAR)~b); }
void w16_set(USHORT a, USHORT b) { rtw_w16(a, rtw_r16(a) | b); }
void w16_clr(USHORT a, USHORT b) { rtw_w16(a, rtw_r16(a) & (USHORT)~b); }
void w32_set(USHORT a, ULONG b)  { rtw_w32(a, rtw_r32(a) | b); }
void w32_clr(USHORT a, ULONG b)  { rtw_w32(a, rtw_r32(a) & ~b); }

void w32_mask(USHORT a, ULONG mask, ULONG data)
{
   ULONG v, m;

   if (mask == 0xFFFFFFFFUL) {
      rtw_w32(a, data);
      return;
   }
   m = mask;
   while (m && !(m & 1UL)) {
      m = shr1(m);
      data = shl1(data);
   }
   v = rtw_r32(a);
   rtw_w32(a, (v & ~mask) | (data & mask));
}

ULONG r32_mask(USHORT a, ULONG mask)
{
   ULONG v = rtw_r32(a) & mask;

   while (mask && !(mask & 1UL)) {
      mask = shr1(mask);
      v = shr1(v);
   }
   return v;
}

void w8_mask(USHORT a, UCHAR mask, UCHAR data)
{
   UCHAR m = mask;

   while (m && !(m & 1)) {
      m = (UCHAR)(m >> 1);
      data = (UCHAR)(data << 1);
   }
   rtw_w8(a, (UCHAR)((rtw_r8(a) & (UCHAR)~mask) | (data & mask)));
}

/* check_hw_ready: 1000 polls 10 us apart */
int hw_ready(USHORT a, ULONG mask, ULONG target)
{
   USHORT n;

   for (n = 0; n < 1000; n++) {
      if ((rtw_r32(a) & mask) == target)
         return 1;
      dd_udelay(10);
   }
   return 0;
}

/* ---- RF path A (rf_base_addr 0x2800, rf_sipi_addr 0xC90) ----------------------------------------------------- */
ULONG rf_read(UCHAR reg)
{
   return rtw_r32((USHORT)(0x2800 + ((USHORT)reg << 2))) & 0x000FFFFFUL;
}

void rf_write(UCHAR reg, ULONG data)
{
   U32 x;

   x.l = data;
   x.w[1] = (USHORT)((x.w[1] & 0x000F) | ((USHORT)reg << 4));
   rtw_w32(0x0C90, x.l & 0x0FFFFFFFUL);
   dd_udelay(13);
}

void rf_mask(UCHAR reg, ULONG mask, ULONG data)
{
   ULONG m = mask, v;

   if (mask == 0x000FFFFFUL) {
      rf_write(reg, data);
      return;
   }
   while (m && !(m & 1UL)) {
      m = shr1(m);
      data = shl1(data);
   }
   v = rf_read(reg);
   rf_write(reg, (v & ~mask) | (data & mask));
}

/* ---- LTE-coex indirect registers (util.c ltecoex_read_reg / ltecoex_reg_write) ------------------------------- */
#define LTECOEX_READY  0x20000000UL

int ltecoex_read(USHORT off, ULONG FAR *v)
{
   if (!hw_ready(REG_LTECOEX_CTRL, LTECOEX_READY, LTECOEX_READY))
      return -1;
   rtw_w32(REG_LTECOEX_CTRL, 0x800F0000UL | off);
   *v = rtw_r32(REG_LTECOEX_RDATA);
   return 0;
}

int ltecoex_write(USHORT off, ULONG v)
{
   if (!hw_ready(REG_LTECOEX_CTRL, LTECOEX_READY, LTECOEX_READY))
      return -1;
   rtw_w32(REG_LTECOEX_WDATA, v);
   rtw_w32(REG_LTECOEX_CTRL, 0xC00F0000UL | off);
   return 0;
}

/* ---- firmware commands --------------------------------------------------------------------------------------
 * Mailbox (rtw_fw_send_h2c_command): four boxes used in turn; a box is free when its bit in REG_HMETFR is clear;
 * the extension word (0x1F0 + 4n) first, then the box (0x1D0 + 4n) with the command id in byte 0.
 * Packet (rtw_fw_send_h2c_packet): 32 bytes on the H2C ring: category 1, command 0xFF, sub id, total length,
 * sequence number, then the parameters from byte 8. */
UCHAR  g_h2c_box;
USHORT g_h2c_seq, g_h2c_busy;

int h2c_cmd(ULONG w0, ULONG w1)
{
   UCHAR  box = g_h2c_box, bit = (UCHAR)(1 << box);
   USHORT n;

   for (n = 0; n < 30 && (rtw_r8(REG_HMETFR) & bit); n++)
      dd_udelay(100);
   if (n >= 30) {
      g_h2c_busy++;
      return -1;
   }
   rtw_w32(REG_HMEBOX0_EX + ((USHORT)box << 2), w1);
   rtw_w32(REG_HMEBOX0 + ((USHORT)box << 2), w0);
   g_h2c_box = (UCHAR)((box + 1) & 3);
   return 0;
}

int h2c_pkt(UCHAR sub_id, UCHAR FAR *body, USHORT blen)
{
   static UCHAR p[32];

   if (blen > 24)
      return -1;
   fmemset((UCHAR FAR *)p, 0, sizeof(p));
   p[0] = 0x01;                                  /* H2C_PKT_CATEGORY                                         */
   p[1] = 0xFF;                                  /* H2C_PKT_CMD_ID                                           */
   p[2] = sub_id;
   p[4] = (UCHAR)(8 + blen);                     /* total length                                             */
   p[6] = (UCHAR)g_h2c_seq;
   p[7] = (UCHAR)(g_h2c_seq >> 8);
   if (blen)
      fmemcpy((UCHAR FAR *)p + 8, body, blen);
   g_h2c_seq++;
   return r88_h2c_frame((UCHAR FAR *)p, 32);
}
