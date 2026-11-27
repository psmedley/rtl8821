/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * r88_trx.c - the PCIe data path of the RTL8821CE back end: buffer-descriptor rings, transmit, receive, interrupt,
 * service path.
 *
 * The RTL8821C uses the "new TRX flow" of Realtek's PCIe chips (Linux rtw88 pci.c): every queue is a ring of buffer
 * descriptors at REG_xxx_DESA with its length in REG_xxx_NUM and a pair of indexes in REG_xxx_IDX (host index in bits
 * 11:0, hardware index in bits 27:16). There is no OWN bit (except on the one-entry beacon queue): the host writes
 * its index to hand entries over, and reads the hardware index to see which ones the chip has finished.
 *   TX buffer descriptor (16 bytes, two segments): [len 48 | psb_len] [addr of the 48-byte TX descriptor]
 *                                                  [frame length]     [addr of the frame]
 *      psb_len = number of 128-byte pieces of descriptor + frame (bit 15 = OWN on the beacon queue).
 *   RX buffer descriptor (8 bytes): [buffer size | rx tag] [buffer address]; the buffer receives the 24-byte RX
 *      descriptor, then shift bytes, the PHY status (drvinfo) and the frame.
 * Here the 48-byte TX descriptor and the frame are one DMA buffer (descriptor first), as Linux pushes the
 * descriptor in front of the skb.
 * Contexts as in the RTL8821AE back end (rtw/rtw_trx.c): hw_irq at interrupt time masks the chip, hw_service drains
 * the receive ring (task time or context hook, never blocks), r88_tx_frame may run at interrupt time.
 */
#include "drv.h"
#include "r88.h"

#define RX_N        32
#define RX_BUF      4096
#define Q_BE        0
#define Q_MG        1
#define Q_H2C       2
#define NQ          3

USHORT g_trx_on;
USHORT g_rx_frames, g_rx_crc, g_rx_c2h, g_rx_big, g_rx_rdu, g_svc_runs, g_tx_ok[3], g_tx_full[3];
ULONG  g_isr0, g_isr1;
UCHAR  g_rx_rate;

typedef struct {
   DMA    ring;
   DMA   *buf;
   USHORT n, size, wp, lock, idx_reg;
} txq_t;

static DMA    g_rxd, g_rxb[RX_N];
static USHORT g_rxrp;
static DMA    g_bufs_be[32], g_bufs_mg[8], g_bufs_h2c[8];
static txq_t  g_q[NQ] = {
   { {0}, g_bufs_be, 32, TXD88 + 1600, 0, 0, TXBD_IDX_BEQ },
   { {0}, g_bufs_mg,  8, TXD88 + 512,  0, 0, TXBD_IDX_MGMTQ },
   { {0}, g_bufs_h2c, 8, TXD88 + 32,   0, 0, TXBD_IDX_H2CQ },
};
static DMA    g_bcn_ring, g_bcn_buf, g_dummy;
static USHORT g_svc_lock, g_svc_again, g_irq_masked, g_irq_en;
static ULONG  g_imr0 = IMR_ROK | IMR_RDU | IMR_C2HCMD;
static ULONG  g_imr1 = IMR1_TXFOVW | IMR1_RXFOVW;

/* ---- allocation and set-up ---------------------------------------------------------------------------------- */
int r88_trx_alloc(void)
{
   static USHORT done;
   USHORT q, i;

   if (done)
      return 0;
   if (dma_get_al(RX_N * RXBD88, 256, &g_rxd))
      return -1;
   for (i = 0; i < RX_N; i++)
      if (dma_get_al(RX_BUF, 64, &g_rxb[i]))
         return -2;
   for (q = 0; q < NQ; q++) {
      if (dma_get_al((USHORT)(g_q[q].n * TXBD88), 256, &g_q[q].ring))
         return -3;
      for (i = 0; i < g_q[q].n; i++)
         if (dma_get_al(g_q[q].size, 64, &g_q[q].buf[i]))
            return -4;
   }
   if (dma_get_al(TXBD88, 256, &g_bcn_ring) || dma_get_al(TXD88 + 4096, 64, &g_bcn_buf) ||
       dma_get_al(2 * TXBD88, 256, &g_dummy))
      return -5;
   done = 1;
   return 0;
}

static void rx_arm(USHORT i)
{
   UCHAR FAR *d = DPTR(g_rxd) + i * RXBD88;

   put16(d, RX_BUF);                         /* buffer size                                                */
   put16(d + 2, 0);                          /* rx tag (written by the chip)                               */
   put32(d + 4, g_rxb[i].phys);
}

/* rtw_pci_setup: rtw_pci_reset_buf_desc + rtw_pci_dma_reset */
void r88_trx_setup(void)
{
   static const USHORT dum_desa[4] = { TXBD_DESA_BKQ, TXBD_DESA_VIQ, TXBD_DESA_VOQ, TXBD_DESA_HI0Q };
   static const USHORT dum_num[4]  = { TXBD_NUM_BKQ, TXBD_NUM_VIQ, TXBD_NUM_VOQ, TXBD_NUM_HI0Q };
   USHORT i;

   for (i = 0; i < RX_N; i++)
      rx_arm(i);
   g_rxrp = 0;
   for (i = 0; i < NQ; i++) {
      fmemset(DPTR(g_q[i].ring), 0, (USHORT)(g_q[i].n * TXBD88));
      g_q[i].wp = 0;
      g_q[i].lock = 0;
   }
   fmemset(DPTR(g_bcn_ring), 0, TXBD88);
   fmemset(DPTR(g_dummy), 0, 2 * TXBD88);

   rtw_w8(RTK_PCI_CTRL + 3, (UCHAR)(rtw_r8(RTK_PCI_CTRL + 3) | 0xF7));
   rtw_w32(TXBD_DESA_BCNQ, g_bcn_ring.phys);
   rtw_w16(TXBD_NUM_H2CQ, g_q[Q_H2C].n);
   rtw_w32(TXBD_DESA_H2CQ, g_q[Q_H2C].ring.phys);
   for (i = 0; i < 4; i++) {                 /* BK, VI, VO, HI0: a two-entry ring that is never filled      */
      rtw_w16(dum_num[i], 2);
      rtw_w32(dum_desa[i], g_dummy.phys);
   }
   rtw_w16(TXBD_NUM_BEQ, g_q[Q_BE].n);
   rtw_w32(TXBD_DESA_BEQ, g_q[Q_BE].ring.phys);
   rtw_w16(TXBD_NUM_MGMTQ, g_q[Q_MG].n);
   rtw_w32(TXBD_DESA_MGMTQ, g_q[Q_MG].ring.phys);
   rtw_w16(RXBD_NUM_MPDUQ, RX_N);
   rtw_w32(RXBD_DESA_MPDUQ, g_rxd.phys);
   rtw_w32(TXBD_RWPTR_CLR, 0xFFFFFFFFUL);    /* every read / write index to 0                              */
   w32_set(TXBD_H2CQ_CSR, 0x00010100UL);     /* H2C queue: host and hardware index cleared                 */
   w32_set(RTK_PCI_CTRL, 0x00108000UL);      /* BIT_RST_TRXDMA_INTF | BIT_RX_TAG_EN                        */
}

/* ---- interrupts ---------------------------------------------------------------------------------------------- */
void r88_irq_enable(void)
{
   g_irq_masked = 0;
   g_irq_en = 1;
   rtw_w32(REG_HIMR0, g_imr0);
   rtw_w32(REG_HIMR1, g_imr1);
   rtw_w32(REG_HIMR3, 0UL);
}

void r88_irq_disable(void)
{
   g_irq_en = 0;
   rtw_w32(REG_HIMR0, 0UL);
   rtw_w32(REG_HIMR1, 0UL);
   rtw_w32(REG_HIMR3, 0UL);
}

USHORT hw_irq(void)
{
   ULONG r0, r1;

   if (!g_trx_on || !g_irq_en)
      return 0;                              /* our interrupts are off: a shared line's interrupt is not ours */
   r0 = rtw_r32(REG_HISR0);
   if (r0 == 0xFFFFFFFFUL)
      return 0;
   r1 = rtw_r32(REG_HISR1);
   if (!(r0 & g_imr0) && !(r1 & g_imr1))
      return 0;
   rtw_w32(REG_HIMR0, 0UL);                  /* masked until the context hook has run                        */
   rtw_w32(REG_HIMR1, 0UL);
   g_irq_masked = 1;
   return 1;
}

/* ---- transmit ------------------------------------------------------------------------------------------------- */
static void bd_fill(UCHAR FAR *bd, ULONG phys, USHORT flen, USHORT own)
{
   USHORT psb = (USHORT)(((ULONG)TXD88 + flen - 1) >> 7) + 1;   /* 128-byte pieces                            */

   fmemset(bd, 0, TXBD88);
   put16(bd, TXD88);
   put16(bd + 2, (USHORT)(psb | (own ? 0x8000 : 0)));
   put32(bd + 4, phys);
   put16(bd + 8, flen);
   put32(bd + 12, phys + TXD88);
}

/* entries the chip has not finished yet leave avail_desc() = len - (wp - rp) - 1 free */
static USHORT q_free(txq_t *t)
{
   USHORT rp = (USHORT)(rtw_r16((USHORT)(t->idx_reg + 2)) & 0x0FFF);

   if (rp >= t->n)
      return 0;
   if (rp > t->wp)
      return (USHORT)(rp - t->wp - 1);
   return (USHORT)(t->n - t->wp + rp - 1);
}

/* the 48-byte TX descriptor (rtw_tx_fill_tx_desc) */
static void txd_fill(UCHAR FAR *d, USHORT len, UCHAR qsel, UCHAR rate, USHORT flags, USHORT seq)
{
   ULONG w;

   fmemset(d, 0, TXD88);
   w = (ULONG)len | ((ULONG)TXD88 << 16) | 0x04000000UL;          /* TXPKTSIZE, OFFSET, LS                 */
   if (flags & TXF_BCAST)
      w |= 0x01000000UL;                                         /* BMC                                   */
   if (flags & TXF_HWSEQ)
      w |= 0x80000000UL;                                         /* DISQSELSEQ                            */
   put32(d, w);
   w = ((ULONG)qsel << 8) | ((ULONG)(rate < RATE_6M ? 8 : 7) << 16);   /* MACID 0, QSEL, RATE_ID B_20M / G   */
   if (flags & TXF_SEC)
      w |= 0x00C00000UL;                                         /* SEC_TYPE 3 = CCMP                     */
   put32(d + 4, w);
   if (flags & TXF_RPT)
      put32(d + 8, 0x00080000UL);                                /* SPE_RPT                               */
   put32(d + 12, 0x00000500UL);                                  /* USE_RATE | DISDATAFB                  */
   put32(d + 16, (ULONG)(rate & 0x7F));
   if (flags & TXF_HWSEQ)
      put32(d + 32, 0x00008000UL);                               /* EN_HWSEQ                              */
   else
      put32(d + 36, (ULONG)(seq & 0x0FFF) << 12);                /* SW_SEQ                                */
}

int r88_tx_frame(UCHAR qsel, UCHAR FAR *h, USHORT hl, UCHAR FAR *iv, UCHAR FAR *b, USHORT bl, UCHAR rate, USHORT flags)
{
   USHORT q = (qsel == QSEL_MGMT) ? Q_MG : Q_BE;
   txq_t *t = &g_q[q];
   USHORT fl, len, i;
   UCHAR FAR *p;

   if (!g_trx_on)
      return -1;
   len = (USHORT)(hl + (iv ? 8 : 0) + bl);
   if ((USHORT)(len + TXD88) > t->size)
      return -1;
   fl = rt_irq_save();
   if (rt_lock_acquire(&t->lock, (USHORT)(rt_cpu_id() + 1))) {
      rt_irq_restore(fl);
      return -2;
   }
   if (!q_free(t)) {
      g_tx_full[q]++;
      rt_lock_release(&t->lock);
      rt_irq_restore(fl);
      return -2;
   }
   i = t->wp;
   p = DPTR(t->buf[i]);
   txd_fill(p, len, qsel, rate, flags, (USHORT)(get16(h + 22) >> 4));
   fmemcpy(p + TXD88, h, hl);
   if (iv)
      fmemcpy(p + TXD88 + hl, iv, 8);
   fmemcpy(p + TXD88 + hl + (iv ? 8 : 0), b, bl);
   bd_fill(DPTR(t->ring) + i * TXBD88, t->buf[i].phys, len, 0);
   t->wp = (USHORT)((i + 1) % t->n);
   rtw_w16(t->idx_reg, t->wp);                                  /* kick (rtw_pci_tx_kick_off_queue)      */
   g_tx_ok[q]++;
   rt_lock_release(&t->lock);
   rt_irq_restore(fl);
   return 0;
}

/* An H2C packet (rtw_pci_write_data_h2c): descriptor with only the size and QSEL_H2C, then the packet. Task time. */
int r88_h2c_frame(UCHAR FAR *pkt, USHORT len)
{
   txq_t *t = &g_q[Q_H2C];
   UCHAR FAR *p;
   USHORT i, n;

   for (n = 0; n < 50 && !q_free(t); n++)
      r88_mdelay(1);
   if (!q_free(t)) {
      g_tx_full[Q_H2C]++;
      return -2;
   }
   i = t->wp;
   p = DPTR(t->buf[i]);
   fmemset(p, 0, TXD88);
   put32(p, (ULONG)len);
   put32(p + 4, (ULONG)QSEL_H2C << 8);
   fmemcpy(p + TXD88, pkt, len);
   bd_fill(DPTR(t->ring) + i * TXBD88, t->buf[i].phys, len, 0);
   t->wp = (USHORT)((i + 1) % t->n);
   rtw_w16(TXBD_IDX_H2CQ, t->wp);
   g_tx_ok[Q_H2C]++;
   return 0;
}

/* The beacon queue carries reserved-page data (the firmware download): one entry with its OWN bit, started by
 * TXBD_BCN_WORK bit 4 (rtw_pci_write_data_rsvd_page). The descriptor is the one rtw_tx_rsvd_page_pkt_info_update
 * builds for RSVD_BEACON on 2.4 GHz: QSEL beacon, 1 Mbit/s fixed, hardware sequence. */
UCHAR FAR *r88_rsvd_buf(void)
{
   return DPTR(g_bcn_buf) + TXD88;
}

int r88_rsvd_kick(USHORT len)
{
   UCHAR FAR *p = DPTR(g_bcn_buf);

   if (len > 4096)
      return -1;
   fmemset(p, 0, TXD88);
   put32(p, (ULONG)len | ((ULONG)TXD88 << 16) | 0x04000000UL | 0x80000000UL);   /* size, offset, LS, DISQSELSEQ */
   put32(p + 4, ((ULONG)QSEL_BEACON << 8) | (8UL << 16));                      /* QSEL beacon, RATE_ID B_20M  */
   put32(p + 12, 0x00000500UL);                                                 /* USE_RATE | DISDATAFB        */
   put32(p + 32, 0x00008000UL);                                                 /* EN_HWSEQ                    */
   bd_fill(DPTR(g_bcn_ring), g_bcn_buf.phys, len, 1);
   rtw_w8(TXBD_BCN_WORK, (UCHAR)(rtw_r8(TXBD_BCN_WORK) | 0x10));               /* BIT_PCI_BCNQ_FLAG           */
   return 0;
}

int r88_rsvd_page(UCHAR FAR *data, USHORT len)
{
   if (len > 4096)
      return -1;
   fmemcpy(r88_rsvd_buf(), data, len);
   return r88_rsvd_kick(len);
}

/* ---- receive --------------------------------------------------------------------------------------------------
 * RX descriptor (rtw88 rx.h): dw0 PKT_LEN[13:0] CRC32[14] ICV_ERR[15] DRV_INFO_SIZE[19:16] (x 8) ENC_TYPE[22:20]
 * SHIFT[25:24] PHYST[26] SWDEC[27]; dw2 C2H[28]; dw3 RX_RATE[6:0]; dw5 TSFL.
 * Signal strength from the PHY status (rtw8821c.c query_phy_status): page 0 (CCK) LNA / VGA indexes and the LNA gain
 * table of the board, page 1 (OFDM / HT / VHT) path A PWDB - 110. */
static const signed char g_lna0[8]  = { 22, 8, -6, -22, -31, -40, -46, -52 };
static const signed char g_lna1[16] = { 10, 6, 2, -2, -6, -10, -14, -17, -20, -24, -28, -31, -34, -37, -40, -44 };

static short rx_rssi(UCHAR FAR *ph)
{
   short p;

   if ((ph[0] & 0x0F) == 0) {
      UCHAR vga = (UCHAR)(ph[13] & 0x1F);
      UCHAR lna = (UCHAR)(((ph[13] >> 5) & 0x07) | ((ph[14] & 0x80) ? 0x08 : 0));

      if (g_rfe == 0)
         p = (short)(lna < 8 ? g_lna0[lna] : -120);
      else
         p = (short)g_lna1[lna & 15];
      p = (short)(p - 2 * vga);
   } else
      p = (short)((short)ph[1] - 110);
   if (p > 0) p = 0;
   if (p < -110) p = -110;
   return p;
}

static void rx_service(void)
{
   USHORT wp, n, pl, drv, sh, fl;
   UCHAR FAR *d, FAR *f;
   ULONG w0;

   wp = (USHORT)(((ULONG)rtw_r32(RXBD_IDX_MPDUQ) >> 16) & 0x0FFF);
   if (wp >= RX_N)
      return;
   for (n = 0; n < RX_N && g_rxrp != wp; n++) {
      d = DPTR(g_rxb[g_rxrp]);
      w0 = get32(d);
      pl  = (USHORT)(LO16(w0) & 0x3FFF);
      drv = (USHORT)((d[2] & 0x0F) * 8);
      sh  = (USHORT)(d[3] & 0x03);
      if (pl == 0 || (ULONG)RXD88 + sh + drv + pl > RX_BUF)
         g_rx_big++;
      else if (d[11] & 0x10)                 /* C2H report (the driver waits for none of them: IQK is polled) */
         g_rx_c2h++;
      else if (d[1] & 0xC0)                  /* CRC32 / ICV error                                            */
         g_rx_crc++;
      else {
         f = d + RXD88 + sh + drv;
         g_rx_frames++;
         fl = 0;
         if ((f[1] & 0x40) && !(d[3] & 0x08) && (d[2] & 0x70))
            fl |= RXF_DECRYPTED;             /* the chip decrypted it: CCMP header and MIC still in place     */
         if ((f[0] & 0x0C) == 0x08)
            g_rx_rate = (UCHAR)(d[12] & 0x7F);
         sta_rx_frame(f, pl, g_curch, (d[3] & 0x04) && drv ? rx_rssi(d + RXD88 + sh) : 0, fl, get32(d + 20));
      }
      rx_arm(g_rxrp);
      g_rxrp = (USHORT)((g_rxrp + 1) % RX_N);
   }
   rtw_w16(RXBD_IDX_MPDUQ, g_rxrp);          /* host read index: the entries are free again                  */
}

static void service_once(void)
{
   ULONG r0, r1, r3;

   r0 = rtw_r32(REG_HISR0);
   if (r0 == 0xFFFFFFFFUL)
      return;
   r1 = rtw_r32(REG_HISR1);
   r3 = rtw_r32(REG_HISR3);
   if (r0) rtw_w32(REG_HISR0, r0);           /* write-1-to-clear (rtw_pci_irq_recognized)                    */
   if (r1) rtw_w32(REG_HISR1, r1);
   if (r3) rtw_w32(REG_HISR3, r3);
   if (r0) g_isr0 = r0;
   if (r1) g_isr1 = r1;
   if (r0 & IMR_RDU) g_rx_rdu++;
   rx_service();
   r88_link_tick();
   sta_rx_tick();
}

void hw_service(void)
{
   USHORT id = (USHORT)(rt_cpu_id() + 1);

   if (!g_trx_on)
      return;
   if (rt_lock_try(&g_svc_lock, id)) {
      g_svc_again = 1;
      return;
   }
   do {
      g_svc_again = 0;
      g_svc_runs++;
      service_once();
   } while (g_svc_again);
   rt_lock_release(&g_svc_lock);
}

void hw_hook(void)
{
   hw_service();
   if (g_irq_masked && g_trx_on && g_irq_en) {
      g_irq_masked = 0;
      rtw_w32(REG_HIMR0, g_imr0);
      rtw_w32(REG_HIMR1, g_imr1);
   }
}

int hw_wait(USHORT *flag, USHORT ms)
{
   ULONG t0 = dd_ms();

   for (;;) {
      hw_service();
      if (*flag)
         return 0;
      if ((long)(dd_ms() - t0) > (long)ms)
         return -1;
      dd_msleep(2);
   }
}

USHORT r88_trx_state(USHORT what)
{
   switch (what) {
   case 0: return g_rxrp;
   case 1: return g_q[Q_BE].wp;
   case 2: return g_q[Q_MG].wp;
   case 3: return g_q[Q_H2C].wp;
   case 4: return g_irq_masked;
   case 5: return (USHORT)(((ULONG)rtw_r32(RXBD_IDX_MPDUQ) >> 16) & 0x0FFF);
   default: return 0;
   }
}
