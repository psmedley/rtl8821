/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * r88_sta.c - the station operations of wlan/hw.h for the RTL8821CE: scan, join, leave, frames, keys, rates, link
 * supervision.
 *
 * The structure is the RTL8821AE back end's (rtw/rtw_sta.c, from the RTL8821AU driver); the chip specifics follow
 * Linux rtw88:
 *   - keys (sec.c rtw_sec_write_cam): eight words per CAM entry, written highest first through 0x674 / 0x670;
 *     word 0 = key id | type (AES 4) << 2 | group << 6 | valid << 15 | addr[0..1]; default-key search is on
 *     (SECCFG 0xCF), so group keys sit in entries 0..3 by key id (broadcast address) and the pairwise key from entry 4;
 *   - port 0 (mac80211.c rtw_vif_port[0]): address 0x610, BSSID 0x618, net type CR bits 17:16 (0 no link,
 *     2 associated station), AID 0x6A8 bits 10:0;
 *   - H2C MEDIA_STATUS_RPT (0x01) when the link comes and goes (rtw_fw_media_status_report).
 * Data frames use a fixed rate in the descriptor (USE_RATE, no fallback) chosen by the signal-strength ladder, as in
 * the other drivers of this project; rtw88 lets the firmware's rate adaptation choose instead (not used here).
 */
#include "drv.h"
#include "r88.h"

UCHAR  g_curch = 1;
UCHAR  g_ctlrate;
UCHAR  g_txrate = 8, g_txrate_auto = 1;
USHORT g_keys_p, g_keys_g, g_keys_dup, g_scan_n;

static USHORT g_pn[3];
static UCHAR  g_ptk_last[16];
static UCHAR  g_ptk_ok;
static ULONG  g_next_chk;

/* ---- rates (see rtw/rtw_sta.c) ----------------------------------------------------------------------------- */
static const UCHAR       g_ra_rate[7] = { 9, 8, 7, 6, 4, 3, 0 };
static const signed char g_ra_thr[6]  = { -55, -65, -72, -77, -82, -86 };
static UCHAR  g_ra_idx = 1;
static USHORT g_ra_cnt;

static UCHAR adapt_rate(void)
{
   short r;
   UCHAR t;

   if (!g_txrate_auto)
      return g_txrate;
   if ((g_ra_cnt++ & 31) == 0) {
      r = sta_rssi();
      for (t = 0; t < 6; t++)
         if (r >= g_ra_thr[t])
            break;
      if (t < g_ra_idx) {
         if (r >= g_ra_thr[t] + 3)
            g_ra_idx = t;
      } else if (t > g_ra_idx) {
         if (r < g_ra_thr[g_ra_idx] - 3)
            g_ra_idx = t;
      }
   }
   if (g_curch > 14 && g_ra_idx > 4)
      return RATE_6M;
   return g_ra_rate[g_ra_idx];
}

static UCHAR ctl_rate(void)
{
   if (g_ctlrate)
      return g_ctlrate;
   return (UCHAR)(g_curch > 14 ? RATE_6M : RATE_1M);
}

static UCHAR rate_for(UCHAR FAR *b, USHORT bl, USHORT flags)
{
   if (flags & HWTX_EAPOL)
      return ctl_rate();
   if (bl >= 8 && b[0] == 0xAA && b[1] == 0xAA) {
      if (b[6] == 0x08 && b[7] == 0x06) return ctl_rate();
      if (b[6] == 0x88 && b[7] == 0x8E) return ctl_rate();
      if (bl > 8 + 24 && b[6] == 0x08 && b[7] == 0x00 && b[8 + 9] == 17 &&
          ((b[8 + 20] == 0 && (b[8 + 21] == 67 || b[8 + 21] == 68)) ||
           (b[8 + 22] == 0 && (b[8 + 23] == 67 || b[8 + 23] == 68))))
         return ctl_rate();
   }
   return adapt_rate();
}

/* ---- keys ------------------------------------------------------------------------------------------------------ */
static void cam_write(UCHAR entry, USHORT w0lo, UCHAR FAR *mac, UCHAR FAR *key)
{
   U32    v, c;
   int    j;
   USHORT k;

   for (j = 7; j >= 0; j--) {
      if (j == 0) {
         v.w[0] = w0lo;
         v.b[2] = mac[0]; v.b[3] = mac[1];
      } else if (j == 1) {
         for (k = 0; k < 4; k++) v.b[k] = mac[2 + k];
      } else if (j >= 6) {
         v.l = 0;
      } else {
         for (k = 0; k < 4; k++) v.b[k] = key ? key[(j - 2) * 4 + k] : 0;
      }
      rtw_w32(REG_CAMWRITE, v.l);
      c.w[0] = (USHORT)(((USHORT)entry << 3) + (USHORT)j);
      c.w[1] = 0x8001;                          /* polling | write enable                                       */
      rtw_w32(REG_CAMCMD, c.l);
      for (k = 0; k < 100 && (rtw_r32(REG_CAMCMD) & 0x80000000UL); k++)
         dd_udelay(2);
   }
}

static void pn_reset(void)
{
   USHORT fl = rt_irq_save();

   g_pn[0] = 1; g_pn[1] = g_pn[2] = 0;
   rt_irq_restore(fl);
}

void r88_keys_reset(void)
{
   rtw_w32(REG_CAMCMD, 0xC0000000UL);           /* polling | clear: every entry invalid                         */
   g_ptk_ok = 0;
   fmemset((UCHAR FAR *)g_ptk_last, 0, 16);
   pn_reset();
}

int hw_key_set(UCHAR FAR *key, USHORT klen, USHORT pairwise, UCHAR idx)
{
   static UCHAR k[16], bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
   USHORT i;

   if (klen != 16)
      return -1;
   fmemcpy((UCHAR FAR *)k, key, 16);
   if (pairwise) {
      g_keys_p++;
      if (g_ptk_ok && fmemeq((UCHAR FAR *)g_ptk_last, (UCHAR FAR *)k, 16)) {
         g_keys_dup++;                          /* the same key again: keep the packet numbers (rtw/rtw_sta.c) */
         return 0;
      }
      cam_write(4, (USHORT)(0x8000 | (4 << 2)), (UCHAR FAR *)g_jbssid, (UCHAR FAR *)k);
      pn_reset();
      for (i = 0; i < 16; i++) g_ptk_last[i] = k[i];
      g_ptk_ok = 1;
   } else {
      g_keys_g++;
      idx = (UCHAR)(idx & 3);
      cam_write(idx, (USHORT)(0x8000 | 0x0040 | (4 << 2) | idx), (UCHAR FAR *)bcast, (UCHAR FAR *)k);
   }
   return 0;
}

int hw_key_clear(USHORT pairwise, UCHAR idx)
{
   static UCHAR nomac[6];

   cam_write((UCHAR)(pairwise ? 4 : (idx & 3)), 0, (UCHAR FAR *)nomac, 0);
   if (pairwise)
      g_ptk_ok = 0;
   return 0;
}

/* ---- frames ------------------------------------------------------------------------------------------------------- */
int hw_mgmt_tx(UCHAR FAR *f, USHORT len)
{
   USHORT fl = TXF_HWSEQ;

   if (len < 24)
      return -1;
   if (f[4] & 1)
      fl |= TXF_BCAST;
   return r88_tx_frame(QSEL_MGMT, f, 24, 0, f + 24, (USHORT)(len - 24), ctl_rate(), fl) ? -1 : 0;
}

/* under the station's transmit lock with interrupts off (wlan/sta.c tx_eth): packet number and ring entry together */
int hw_data_tx(UCHAR FAR *hdr, USHORT hlen, UCHAR FAR *body, USHORT blen, USHORT flags)
{
   static UCHAR iv[8];
   USHORT tf = 0;
   UCHAR  rate = rate_for(body, blen, flags);
   int    rc;

   if (hdr[4] & 1)
      tf |= TXF_BCAST;
   if (flags & HWTX_PROTECT) {
      iv[0] = (UCHAR)g_pn[0];  iv[1] = (UCHAR)(g_pn[0] >> 8);
      iv[2] = 0;               iv[3] = 0x20;                    /* ExtIV, key id 0                              */
      iv[4] = (UCHAR)g_pn[1];  iv[5] = (UCHAR)(g_pn[1] >> 8);
      iv[6] = (UCHAR)g_pn[2];  iv[7] = (UCHAR)(g_pn[2] >> 8);
      rc = r88_tx_frame(QSEL_TID0, hdr, hlen, (UCHAR FAR *)iv, body, blen, rate, (USHORT)(tf | TXF_SEC));
      if (rc == 0 && ++g_pn[0] == 0 && ++g_pn[1] == 0)
         g_pn[2]++;
   } else
      rc = r88_tx_frame(QSEL_TID0, hdr, hlen, 0, body, blen, rate, tf);
   return rc;
}

/* ---- scan (as rtw/rtw_sta.c) ------------------------------------------------------------------------------------ */
static const UCHAR g_chlist[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                  36, 40, 44, 48, 52, 56, 60, 64,
                                  100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
                                  149, 153, 157, 161, 165 };

static void tx_probe(UCHAR ch)
{
   static UCHAR f[64];
   static const UCHAR r24[8] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
   static const UCHAR x24[4] = { 0x30, 0x48, 0x60, 0x6C };
   static const UCHAR r5[8]  = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
   USHORT i, n;

   fmemset((UCHAR FAR *)f, 0, sizeof(f));
   f[0] = 0x40;
   for (i = 0; i < 6; i++) {
      f[4 + i]  = 0xFF;
      f[10 + i] = g_mymac[i];
      f[16 + i] = 0xFF;
   }
   n = 24;
   f[n++] = 0; f[n++] = 0;
   f[n++] = 1; f[n++] = 8;
   for (i = 0; i < 8; i++) f[n++] = ch > 14 ? r5[i] : r24[i];
   if (ch <= 14) {
      f[n++] = 50; f[n++] = 4;
      for (i = 0; i < 4; i++) f[n++] = x24[i];
      f[n++] = 3; f[n++] = 1; f[n++] = ch;
   }
   hw_mgmt_tx((UCHAR FAR *)f, n);
}

int hw_scan(void)
{
   USHORT i;
   UCHAR  ch;

   if (!g_hw_ready)
      return -1;
   g_scan_n++;
   if (g_joined)
      return 0;
   r88_dig_set(0x20);
   rtw_w32(REG_RCR, R88_RCR_SCAN);
   for (i = 0; i < sizeof(g_chlist); i++) {
      ULONG t0;

      ch = g_chlist[i];
      r88_set_channel(ch);
      g_curch = ch;
      hw_service();
      if (!(ch >= 52 && ch <= 144)) {
         dd_msleep(5);
         tx_probe(ch);
      }
      t0 = dd_ms();
      while ((long)(dd_ms() - t0) < 110L) {
         hw_service();
         dd_msleep(4);
      }
   }
   sta_age();
   return 0;
}

/* ---- join ---------------------------------------------------------------------------------------------------------- */
int hw_chan_ht40(UCHAR ch)  { (void)ch; return 0; }
int hw_chan_vht80(UCHAR ch) { (void)ch; return 0; }

static void media_status(int connect)           /* H2C MEDIA_STATUS_RPT, MAC id 0                                */
{
   h2c_cmd(0x01UL | (connect ? 0x100UL : 0UL), 0UL);
}

int hw_join_prepare(struct net_s *n)
{
   USHORT i;

   if (!g_hw_ready)
      return -1;
   r88_keys_reset();
   for (i = 0; i < 6; i++)
      rtw_w8(REG_BSSID + i, n->bssid[i]);       /* mac80211 sets the BSSID before the authentication             */
   rtw_w32(REG_RCR, R88_RCR_SCAN);
   r88_set_channel(n->ch);
   g_curch = n->ch;
   r88_iqk();                                   /* rtw_ops_mgd_prepare_tx: calibration before associating       */
   g_ra_idx = 1;
   g_ra_cnt = 0;
   dd_msleep(10);
   return 0;
}

int hw_join_complete(void)
{
   w32_mask(REG_CR, 0x00030000UL, 2UL);         /* net type: associated station (RTW_NET_MGD_LINKED)            */
   w32_mask(REG_AID, 0x000007FFUL, (ULONG)g_jaid);
   rtw_w16(REG_BCN_INTERVAL, g_jbcn);
   rtw_w32(REG_RCR, R88_RCR_LINKED);
   media_status(1);
   g_jnss = 1;
   g_next_chk = dd_ms() + 1000UL;
   return 0;
}

void hw_leave(void)
{
   USHORT i;

   if (!g_hw_ready)
      return;
   media_status(0);
   w32_mask(REG_CR, 0x00030000UL, 0UL);
   w32_mask(REG_AID, 0x000007FFUL, 0UL);
   for (i = 0; i < 6; i++)
      rtw_w8(REG_BSSID + i, 0);
   rtw_w32(REG_RCR, R88_RCR_SCAN);
   r88_keys_reset();
   r88_dig_set(0x20);
}

void r88_sta_reset(void)
{
   g_ptk_ok = 0;
   pn_reset();
   g_ra_idx = 1;
}

/* service path, once a second: receiver gain and beacon supervision (10 s without a beacon = the AP is gone) */
void r88_link_tick(void)
{
   ULONG  now = dd_ms();
   USHORT k, seen = 0;

   if ((long)(now - g_next_chk) < 0L)
      return;
   g_next_chk = now + 1000UL;
   r88_dig_tick();
   if (!g_joined)
      return;
   for (k = 0; k < g_nnet; k++)
      if (fmemeq((UCHAR FAR *)g_net[k].bssid, (UCHAR FAR *)g_jbssid, 6)) {
         if ((long)(now - g_net[k].seen) < 10000L)
            seen = 1;
         break;
      }
   if (!seen)
      sta_lost();
}

/* ---- aggregation: the MAC answers A-MPDUs with block acks itself; no transmit aggregation ----------------------- */
int    hw_ba_start(UCHAR tid, USHORT ssn, USHORT win) { (void)tid; (void)ssn; (void)win; return 0; }
int    hw_ba_stop(UCHAR tid)                         { (void)tid; return 0; }
int    hw_txagg_active(void)                          { return 0; }
int    hw_txagg_ok(void)                              { return 0; }
USHORT hw_qos_sn(void)                                { return 0; }
void   hw_txagg_offer(UCHAR tid, USHORT win)          { (void)tid; (void)win; }
void   hw_txagg_stop(UCHAR tid)                       { (void)tid; }
