/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * r88_phy.c - RTL8821C PHY: register tables, channel switch (20 MHz), transmit power, IQ calibration, receiver gain.
 *
 * From Linux rtw88 (rtw8821c.c: rtw8821c_phy_set_param, rtw8821c_set_channel and its _bb / _bb_swing / _rf / _rxdfir
 * parts, rtw8821c_switch_rf_set, rtw8821c_do_iqk, rtw8821c_set_tx_power_index; mac.c rtw_set_channel_mac; phy.c
 * rtw_phy_load_tables, rtw_phy_cfg_*, rtw_phy_get_2g/5g_tx_power_index, rtw_get_channel_group;
 * Copyright(c) 2018-2019 Realtek Corporation, GPL-2.0 OR BSD-3-Clause).
 * Simplified against Linux: 20 MHz only; transmit power from the eFuse base indexes only (no by-rate table, no
 * regulatory limit table, no thermal tracking) - the approach of the RTL8821AU / AE drivers; no SRRC CCK filter.
 */
#include "drv.h"
#include "r88.h"

UCHAR  g_channel, g_band = 2, g_iqk_mode = 1;
USHORT g_iqk_n, g_iqk_ok, g_iqk_ms;
UCHAR  g_txp_cck, g_txp_ofdm, g_txp_ht;
USHORT g_tblvar;
static ULONG g_ch_param[3];                      /* TXSF2, TXSF6, TXFILTER after the tables (CCK TX filter)   */

/* ---- tables ----------------------------------------------------------------------------------------------- */
static void bb_delay_or_write(USHORT a, ULONG v)  /* rtw_phy_cfg_bb */
{
   if (a == 0xFE)      r88_mdelay(50);
   else if (a == 0xFD) r88_mdelay(5);
   else if (a == 0xFC) r88_mdelay(1);
   else if (a == 0xFB) dd_udelay(50);
   else if (a == 0xFA) dd_udelay(5);
   else if (a == 0xF9) dd_udelay(1);
   else                rtw_w32(a, v);
}

static void load_tables(void)                    /* rtw_phy_load_tables */
{
   const r88_tblset_t *s;
   USHORT i;

   g_tblvar = r88_tbl_variant(g_rfe, g_pkg1);
   s = &g88_tbls[g_tblvar];
   for (i = 0; i < g88_mac_n; i++)
      rtw_w8(g88_mac_tbl[i].a, g88_mac_tbl[i].v);
   for (i = 0; i < g88_bb_n; i++)
      bb_delay_or_write(g88_bb_tbl[i].a, g88_bb_tbl[i].v);
   for (i = 0; i < s->nagc; i++)
      rtw_w32(s->agc[i].a, s->agc[i].v);
   if (g_rfe == 2 || g_rfe == 4)                 /* rtw8821c_rfe_defs: RTW_DEF_RFE_EXT(..., btg 2)           */
      for (i = 0; i < s->nagcbtg; i++)
         rtw_w32(s->agcbtg[i].a, s->agcbtg[i].v);
   for (i = 0; i < s->nrf; i++) {                /* rtw_phy_cfg_rf                                           */
      USHORT a = s->rf[i].a;

      if (a == 0xFFE)
         r88_mdelay(50);
      else if (a == 0xFE)
         dd_udelay(100);
      else {
         rf_write((UCHAR)a, s->rf[i].v);
         dd_udelay(1);
      }
   }
}

/* rtw8821c_phy_set_param */
void r88_phy_set_param(void)
{
   UCHAR v, cap;

   v = (UCHAR)(rtw_r8(REG_SYS_FUNC_EN) | 0x40);              /* FEN_PCIEA                                      */
   rtw_w8(REG_SYS_FUNC_EN, v);
   v |= 0x03;                                                /* toggle BB reset                                */
   rtw_w8(REG_SYS_FUNC_EN, v);
   v &= (UCHAR)~0x03;
   rtw_w8(REG_SYS_FUNC_EN, v);
   v |= 0x03;
   rtw_w8(REG_SYS_FUNC_EN, v);
   rtw_w8(REG_RF_CTRL, 0x07);                                /* RF_EN | RF_RSTB | RF_SDM_RSTB                  */
   dd_udelay(10);
   rtw_w8(REG_WLRF1 + 3, 0x07);
   dd_udelay(10);
   w32_clr(0x0808, 0x30000000UL);                            /* REG_RXPSEL: BIT_RX_PSEL_RST before the tables  */
   load_tables();
   cap = (UCHAR)(g_xtal & 0x3F);
   w32_mask(REG_AFE_XTAL_CTRL, 0x7E000000UL, cap);
   w32_mask(REG_AFE_PLL_CTRL, 0x0000007EUL, cap);
   w32_mask(0x0A2C, 0x00440000UL, 0UL);                      /* REG_CCK0_FAREPORT bits 18, 22                  */
   w32_set(0x0808, 0x30000000UL);
   g_ch_param[0] = rtw_r32(0x0A24);                          /* REG_TXSF2                                      */
   g_ch_param[1] = rtw_r32(0x0A28);                          /* REG_TXSF6                                      */
   g_ch_param[2] = rtw_r32(0x0AAC);                          /* REG_TXFILTER                                   */
   g_igi = (UCHAR)(rtw_r8(0x0C50) & 0x7F);
   g_band = 2;
}

/* ---- channel ------------------------------------------------------------------------------------------------ */
/* rtw8821c_switch_rf_set */
#define RF_SET_BTG 0
#define RF_SET_WLG 1
#define RF_SET_WLA 2

static void switch_rf_set(UCHAR set)
{
   ULONG r;

   w32_set(0x1080, 0x00010000UL);                            /* REG_DMEM_CTRL: BIT_WL_RST                      */
   w32_set(0x0000, 0x04000000UL);                            /* REG_SYS_CTRL: BIT_FEN_EN                       */
   r = rtw_r32(0x0CB8);                                      /* REG_RFECTL                                     */
   if (set == RF_SET_BTG) {
      r |= 0x00010000UL;                                     /* B_BTG_SWITCH                                   */
      r &= ~(0x00040000UL | 0x00500000UL | 0x00200000UL | 0x00800000UL);
      w32_mask(0x0A84, 0x00FF0000UL, 0x0EUL);                /* REG_ENRXCCA byte 2 = BTG_CCA                   */
      w32_mask(0x0A80, 0x0000FFFFUL, 0xFC84UL);              /* REG_ENTXCCK low word = BTG_LNA                 */
   } else if (set == RF_SET_WLG) {
      r |= 0x00500000UL | 0x00200000UL;                      /* B_WL_SWITCH | B_WLG_SWITCH                     */
      r &= ~(0x00010000UL | 0x00040000UL | 0x00800000UL);
      w32_mask(0x0A84, 0x00FF0000UL, 0x12UL);                /* WLG_CCA                                        */
      w32_mask(0x0A80, 0x0000FFFFUL, 0x7532UL);              /* WLG_LNA                                        */
   } else {
      r |= 0x00500000UL | 0x00800000UL;                      /* B_WL_SWITCH | B_WLA_SWITCH                     */
      r &= ~(0x00010000UL | 0x00040000UL | 0x00200000UL);
   }
   rtw_w32(0x0CB8, r);
}

static void set_channel_bb(UCHAR ch)
{
   ULONG v;

   if (ch <= 14) {
      w32_mask(0x0808, 0x10000000UL, 1UL);                   /* REG_RXPSEL bit 28                              */
      w32_mask(0x0454, 0x00000080UL, 0UL);                   /* REG_CCK_CHECK bit 7                            */
      w32_mask(0x0A80, 0x00040000UL, 0UL);                   /* REG_ENTXCCK bit 18                             */
      w32_mask(0x0814, 0x0000FC00UL, 15UL);                  /* REG_RXCCAMSK                                   */
      w32_mask(0x0C1C, 0x00000F00UL, 0UL);                   /* REG_TXSCALE_A: AGC table 0                     */
      w32_mask(0x0860, 0x1FFE0000UL, 0x96AUL);               /* REG_CLKTRK                                     */
      if (ch == 14) {                                        /* CCK TX filter                                  */
         rtw_w32(0x0A24, 0x0000B81CUL);
         w32_mask(0x0A28, 0x0000FFFFUL, 0UL);
         rtw_w32(0x0AAC, 0x00003667UL);
      } else {
         rtw_w32(0x0A24, g_ch_param[0]);
         w32_mask(0x0A28, 0x0000FFFFUL, g_ch_param[1] & 0xFFFFUL);
         rtw_w32(0x0AAC, g_ch_param[2]);
      }
   } else {
      w32_mask(0x0A80, 0x00040000UL, 1UL);
      w32_mask(0x0454, 0x00000080UL, 1UL);
      w32_mask(0x0808, 0x10000000UL, 0UL);
      w32_mask(0x0814, 0x0000FC00UL, 15UL);
      if (ch <= 64)       w32_mask(0x0C1C, 0x00000F00UL, 1UL);
      else if (ch <= 144) w32_mask(0x0C1C, 0x00000F00UL, 2UL);
      else                w32_mask(0x0C1C, 0x00000F00UL, 3UL);
      if (ch <= 48)       w32_mask(0x0860, 0x1FFE0000UL, 0x494UL);
      else if (ch <= 64)  w32_mask(0x0860, 0x1FFE0000UL, 0x453UL);
      else if (ch <= 116) w32_mask(0x0860, 0x1FFE0000UL, 0x452UL);
      else                w32_mask(0x0860, 0x1FFE0000UL, 0x412UL);
   }
   v = rtw_r32(0x08AC);                                      /* REG_ADCCLK, 20 MHz                             */
   v = (v & 0xFFCFFC00UL) | 0x10010000UL;
   rtw_w32(0x08AC, v);
   w32_mask(0x08C4, 0x40000000UL, 1UL);                      /* REG_ADC160 bit 30                              */
}

static void set_channel_bb_swing(UCHAR ch)                   /* rtw8821c_get_bb_swing                          */
{
   static const USHORT s2s[4] = { 0x200, 0x16A, 0x101, 0x0B6 };
   UCHAR sw = ch <= 14 ? g_swing2g : g_swing5g;

   if (sw > 9)
      sw = 0;
   w32_mask(0x0C1C, 0xFFE00000UL, (ULONG)s2s[sw / 3]);
}

static void set_channel_mac(UCHAR ch)                        /* rtw_set_channel_mac, 20 MHz                    */
{
   rtw_w8(REG_DATA_SC, 0);
   w32_clr(REG_WMAC_TRXPTCL_CTL, 0x00000180UL);              /* BIT_RFMOD: 20 MHz                              */
   w32_clr(REG_AFE_XTAL_CTRL, 0x00300000UL);                 /* MAC clock 80 MHz (MAC_CLK_HW_DEF_80M)          */
   rtw_w8(REG_USTIME_TSF, 80);
   rtw_w8(REG_USTIME_EDCA, 80);
   if (ch > 14) w8_set(REG_CCK_CHECK, 0x80);
   else         w8_clr(REG_CCK_CHECK, 0x80);
}

static void set_channel_rf(UCHAR ch)
{
   ULONG r18 = rf_read(0x18);

   r18 &= ~(0x00010300UL | 0x000000FFUL | 0x00060000UL | 0x00000C00UL);   /* band, channel, RFSI, BW          */
   if (ch > 14)
      r18 |= 0x00010100UL;                                   /* RF18_BAND_5G                                   */
   r18 |= (ULONG)ch;
   if (ch >= 100 && ch <= 140)
      r18 |= 0x00020000UL;                                   /* RF18_RFSI_GE                                   */
   else if (ch > 140)
      r18 |= 0x00040000UL;                                   /* RF18_RFSI_GT                                   */
   r18 |= 0x00000C00UL;                                      /* RF18_BW_20M                                    */
   if (ch <= 14) {
      switch_rf_set((UCHAR)(g_rfe_btg ? RF_SET_BTG : RF_SET_WLG));
      rf_mask(0xDF, 0x00000040UL, 1UL);                      /* RF_LUTDBG bit 6                                */
      rf_mask(0x64, 0x0000000FUL, 0xFUL);
   } else {
      switch_rf_set(RF_SET_WLA);
      rf_mask(0xDF, 0x00000040UL, 0UL);
   }
   rf_write(0x18, r18);
   rf_mask(0xB8, 0x00080000UL, 0UL);                         /* RF_XTALX2 bit 19 toggled                       */
   rf_mask(0xB8, 0x00080000UL, 1UL);
}

static void set_channel_rxdfir(void)                         /* 20 MHz                                         */
{
   w32_mask(0x0948, 0x30000000UL, 2UL);                      /* REG_ACBB0                                      */
   w32_mask(0x094C, 0x30000000UL, 2UL);                      /* REG_ACBBRXFIR                                  */
   w32_mask(0x0C20, 0x80000000UL, 1UL);                      /* REG_TXDFIR                                     */
   w32_mask(0x08F0, 0x80000000UL, 0UL);                      /* REG_CHFIR                                      */
}

/* ---- transmit power ------------------------------------------------------------------------------------------
 * eFuse, path A (struct rtw8821c_efuse / rtw_txpwr_idx): 2.4 GHz at 0x10: cck_base[6], bw40_base[5], ht_1s_diff
 * (0x1B: OFDM low nibble, BW20 high nibble, signed); 5 GHz at 0x22: bw40_base[14], ht_1s_diff (0x30, same layout).
 * RFE types 2 and 4 take the 2.4 GHz table of path B (0x3A). Per rate (rtw_phy_get_2g/5g_tx_power_index, 20 MHz,
 * one stream): CCK = cck_base, OFDM = bw40_base + OFDM diff, HT / VHT = bw40_base + BW20 diff. The index goes to
 * every rate of the TX AGC table (0x1D00 + rate, four rates per dword: rtw8821c_set_tx_power_index). */
static int sx4(UCHAR n)
{
   n &= 0x0F;
   return (n & 8) ? (int)n - 16 : (int)n;
}

static UCHAR clamp(int v)
{
   if (v < 0) v = 0;
   if (v > 0x3F) v = 0x3F;
   return (UCHAR)v;
}

static UCHAR grp(UCHAR ch)
{
   if (ch <= 14) {
      if (ch <= 2) return 0;
      if (ch <= 5) return 1;
      if (ch <= 8) return 2;
      if (ch <= 11) return 3;
      return 4;
   }
   if (ch <= 42) return 0;
   if (ch <= 50) return 1;
   if (ch <= 58) return 2;
   if (ch <= 64) return 3;
   if (ch <= 106) return 4;
   if (ch <= 114) return 5;
   if (ch <= 122) return 6;
   if (ch <= 130) return 7;
   if (ch <= 138) return 8;
   if (ch <= 144) return 9;
   if (ch <= 155) return 10;
   if (ch <= 161) return 11;
   if (ch <= 171) return 12;
   return 13;
}

static void txagc_write(USHORT rate0, USHORT n, UCHAR idx)
{
   U32    u;
   USHORT r;

   for (r = rate0; r < rate0 + n; r = (USHORT)(r + 4)) {
      u.b[0] = u.b[1] = u.b[2] = u.b[3] = idx;
      rtw_w32((USHORT)(0x1D00 + (r & 0xFC)), u.l);
   }
}

static void txpower(UCHAR ch)
{
   UCHAR g = grp(ch), base, d;

   if (!g_ef_ok)
      return;                                                /* no eFuse: the table defaults stay              */
   if (ch <= 14) {
      USHORT o = (USHORT)((g_rfe == 2 || g_rfe == 4) ? 0x3A : 0x10);

      UCHAR cck = g_ef[o + (ch == 14 ? 5 : g)];

      base = g_ef[o + 6 + g];
      d = g_ef[o + 11];
      if (cck == 0xFF || base == 0xFF)
         return;                                             /* not programmed: the table defaults stay        */
      g_txp_cck = clamp(cck);
      g_txp_ofdm = clamp((int)base + sx4(d));
      g_txp_ht = clamp((int)base + sx4((UCHAR)(d >> 4)));
      txagc_write(0x00, 4, g_txp_cck);                       /* CCK 1 - 11                                     */
   } else {
      base = g_ef[0x22 + g];
      d = g_ef[0x30];
      if (base == 0xFF)
         return;
      g_txp_ofdm = clamp((int)base + sx4(d));
      g_txp_ht = clamp((int)base + sx4((UCHAR)(d >> 4)));
   }
   txagc_write(0x04, 8, g_txp_ofdm);                         /* OFDM 6 - 54                                    */
   txagc_write(0x0C, 8, g_txp_ht);                           /* HT MCS 0 - 7                                   */
   txagc_write(0x2C, 12, g_txp_ht);                          /* VHT 1SS MCS 0 - 9 (and the two slots after)    */
}

/* rtw_set_channel / rtw8821c_set_channel for 20 MHz */
void r88_set_channel(UCHAR ch)
{
   set_channel_bb(ch);
   set_channel_bb_swing(ch);
   set_channel_mac(ch);
   set_channel_rf(ch);
   set_channel_rxdfir();
   txpower(ch);
   g_channel = ch;
   g_band = (UCHAR)(ch > 14);
}

/* ---- IQ calibration by the firmware (rtw8821c_do_iqk): H2C packet IQK (sub id 0x0E, clear 0, segment 1 when
 * associated), then up to 6 s for the firmware to write 0xABCDE into RF 0x08 (RF_DTXLOK), which is then cleared.
 * Linux runs it before every association (mgd_prepare_tx); here from hw_join_prepare. g_iqk_mode 0 turns it off. */
void r88_iqk(void)
{
   static UCHAR body[1];
   ULONG  t0;
   USHORT n;

   if (!g_iqk_mode)
      return;
   body[0] = 0;
   g_iqk_n++;
   t0 = dd_ms();
   if (h2c_pkt(0x0E, (UCHAR FAR *)body, 1))
      return;
   for (n = 0; n < 300; n++) {
      if (rf_read(0x08) == 0xABCDEUL)
         break;
      hw_service();
      r88_mdelay(20);
   }
   rf_write(0x08, 0UL);
   g_iqk_ms = (USHORT)(dd_ms() - t0);
   if (n < 300)
      g_iqk_ok++;
}

/* ---- receiver gain (IGI, 0xC50 bits 6:0; dig_min 0x1C) - the RTL8821AE back end's rule ------------------------ */
UCHAR g_dig_auto = 1;
UCHAR g_igi = 0x20;
static ULONG g_dig_next;

void r88_dig_set(UCHAR v)
{
   if (v == g_igi)
      return;
   w32_mask(0x0C50, 0x0000007FUL, v);
   g_igi = v;
}

void r88_dig_tick(void)
{
   ULONG now = dd_ms();
   int   t, d, r;

   if (!g_dig_auto || !g_hw_ready || (long)(now - g_dig_next) < 0L)
      return;
   g_dig_next = now + 2000UL;
   t = 0x20;
   if (g_joined) {
      r = sta_rssi();
      if (r < 0) {
         t = r + 90;
         if (t < 0x20) t = 0x20;
         if (t > 0x3A) t = 0x3A;
      }
   }
   d = t - (int)g_igi;
   if (d >= -1 && d <= 1)
      return;
   if (d > 4)  d = 4;
   if (d < -4) d = -4;
   r88_dig_set((UCHAR)((int)g_igi + d));
}
