/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * r88_hal.c - RTL8821CE bring-up: device table, power sequences, firmware download, MAC / FIFO set-up, eFuse,
 * Bluetooth coexistence (WiFi only), hw_up / hw_down, the status text and the debug commands of the back end.
 *
 * The order is rtw_power_on of Linux rtw88 (main.c), with the parts it calls:
 *   rtw_pci_setup (rings)  -> rtw_mac_power_on (mac.c: pre-system config, card enable flow, system config)
 *   -> rtw_download_firmware (mac.c: DMEM / IMEM / EMEM through the reserved page and the DDMA engine)
 *   -> eFuse (efuse.c; Linux reads it once at probe with the firmware running, as here)
 *   -> rtw_mac_init (TX DMA queue mapping, FIFO pages, auto LLT, H2C queue, rtw8821c_mac_init, driver info)
 *   -> rtw8821c_phy_set_param -> interrupts on -> H2C general / PHYDM info -> coexistence power-on and init
 *   -> security engine, RCR.
 * Copyright(c) 2018-2019 Realtek Corporation (rtw88, GPL-2.0 OR BSD-3-Clause).
 */
#include "drv.h"
#include "r88.h"

USHORT g_hw_ready, g_hw_failed, g_hw_state;
UCHAR  g_hw_mac[6];
UCHAR  g_hw_nss = 1;
USHORT g_r88_step, g_pwr_fail, g_fw_err;
USHORT g_fw_ver, g_fw_sub, g_fw_h2cfmt;
ULONG  g_fw_dbg7, g_mcufw;
UCHAR  g_cut, g_rfe, g_pkg1, g_rfe_btg, g_xtal = 0x20, g_swing2g, g_swing5g, g_thermal, g_boardopt, g_btset;
UCHAR  g_ef[512];
USHORT g_ef_ok;
USHORT g_rsvd_boundary;
static ULONG  g_syscfg;
static USHORT g_was_on, g_ant_ok;

/* ---- device table (rtw8821ce.c) ------------------------------------------------------------------------- */
static char *g_devname = "no Realtek RTL8821CE adapter";
static USHORT g_matched;

int hw_pci_match(USHORT ven, USHORT dev, USHORT subven, USHORT subdev)
{
   (void)subven; (void)subdev;
   if (ven != 0x10EC)
      return 0;
   if (dev == 0xC821 || dev == 0xB821) {
      g_matched = 1;
      g_devname = "Realtek RTL8821CE 802.11ac PCIe (1x1)";
      g_bar_cfg = 0x18;                         /* BAR2: memory registers (rtw_pci_io_mapping bar_id 2)        */
      g_bar_len = 0x00010000UL;
      return 1;
   }
   if (!g_matched && dev == 0x8821)
      g_devname = "Realtek RTL8821AE: a different chip, use RTL8821AE.OS2";
   return 0;
}

char *hw_name(void)    { return g_devname; }
char *hw_fw_name(void) { return "RTL8821CE.FW (Linux rtw88/rtw8821c_fw.bin)"; }

int hw_fw_is_mine(UCHAR FAR *h, USHORT n)        /* struct rtw_fw_hdr: signature 0x8821                         */
{
   return n >= 32 && h[0] == 0x21 && h[1] == 0x88;
}

UCHAR hw_vendor_oui(USHORT i)
{
   static const UCHAR rtk[3] = { 0x00, 0xE0, 0x4C };

   if (g_hw_mac[0] | g_hw_mac[1] | g_hw_mac[2])
      return g_hw_mac[i < 3 ? i : 0];
   return rtk[i < 3 ? i : 0];
}

/* ---- power sequences (rtw8821c.c, PCIe entries; every entry is for all cuts) ------------------------------- */
#define PW_WRITE 1
#define PW_POLL  2
#define PW_END   4

typedef struct { USHORT off; UCHAR cmd, msk, val; } pw_t;

static const pw_t g_pw_on[] = {                  /* card_enable_flow_8821c                                      */
   {0x0005, PW_WRITE, 0x98, 0x00},              /* carddis_to_cardemu                                          */
   {0x0300, PW_WRITE, 0xFF, 0x00},
   {0x0301, PW_WRITE, 0xFF, 0x00},
   {0x0005, PW_WRITE, 0x1C, 0x00},              /* cardemu_to_act                                              */
   {0x0075, PW_WRITE, 0x01, 0x01},
   {0x0006, PW_POLL,  0x02, 0x02},
   {0x0075, PW_WRITE, 0x01, 0x00},
   {0x0006, PW_WRITE, 0x01, 0x01},
   {0x0005, PW_WRITE, 0x80, 0x00},
   {0x0005, PW_WRITE, 0x18, 0x00},
   {0x0005, PW_WRITE, 0x01, 0x01},
   {0x0005, PW_POLL,  0x01, 0x00},
   {0x0020, PW_WRITE, 0x08, 0x08},
   {0x0074, PW_WRITE, 0x20, 0x20},
   {0x0022, PW_WRITE, 0x02, 0x00},
   {0x0062, PW_WRITE, 0xE0, 0xE0},
   {0x0061, PW_WRITE, 0xE0, 0x00},
   {0x007C, PW_WRITE, 0x02, 0x00},
   {0xFFFF, PW_END,   0x00, 0x00},
};

static const pw_t g_pw_off[] = {                 /* card_disable_flow_8821c                                     */
   {0x0093, PW_WRITE, 0x08, 0x00},              /* act_to_cardemu                                              */
   {0x001F, PW_WRITE, 0xFF, 0x00},
   {0x0049, PW_WRITE, 0x02, 0x00},
   {0x0006, PW_WRITE, 0x01, 0x01},
   {0x0002, PW_WRITE, 0x02, 0x00},
   {0x0005, PW_WRITE, 0x02, 0x02},
   {0x0005, PW_POLL,  0x02, 0x00},
   {0x0020, PW_WRITE, 0x08, 0x00},
   {0x0067, PW_WRITE, 0x20, 0x00},              /* cardemu_to_carddis                                          */
   {0x0005, PW_WRITE, 0x04, 0x04},
   {0x0081, PW_WRITE, 0xC0, 0x00},
   {0x0090, PW_WRITE, 0x02, 0x00},
   {0xFFFF, PW_END,   0x00, 0x00},
};

static int pw_poll(const pw_t *c)
{
   USHORT n;

   for (n = 0; n < 20000; n++) {                /* 50 us x RTW_PWR_POLLING_CNT                                 */
      if ((rtw_r8(c->off) & c->msk) == (c->val & c->msk))
         return 0;
      dd_udelay(50);
   }
   return -1;
}

static int pw_run(const pw_t *s)
{
   USHORT i;
   UCHAR  v;

   for (i = 0; s[i].cmd != PW_END; i++) {
      if (s[i].cmd == PW_WRITE) {
         v = rtw_r8(s[i].off);
         rtw_w8(s[i].off, (UCHAR)((v & (UCHAR)~s[i].msk) | (s[i].val & s[i].msk)));
      } else if (pw_poll(&s[i])) {
         v = rtw_r8(REG_SYS_PW_CTRL);           /* PCIe: toggle BIT_PFM_WOWL and try again                      */
         rtw_w8(REG_SYS_PW_CTRL, (UCHAR)(v | 0x08));
         rtw_w8(REG_SYS_PW_CTRL, (UCHAR)(v & ~0x08));
         if (pw_poll(&s[i])) {
            g_pwr_fail = (USHORT)(i + 1);
            return -1;
         }
      }
   }
   return 0;
}

/* rtw_mac_pre_system_cfg, 11ac chip on PCIe */
static void pre_system_cfg(void)
{
   rtw_w8(REG_RSV_CTRL, 0);
   w32_set(REG_HCI_OPT_CTRL, 0x00000100UL);     /* BIT_USB_SUS_DIS                                              */
   w32_set(REG_PAD_CTRL1, 0x30000000UL);        /* PAPE / LNAON WL-BT select                                    */
   w32_clr(REG_LED_CFG, 0x06000000UL);          /* PAPE_SEL_EN, LNAON_SEL_EN                                    */
   w32_set(REG_GPIO_MUXCFG, 0x00000004UL);      /* BIT_WLRFE_4_5_EN                                             */
   w8_clr(REG_SYS_FUNC_EN, 0x03);               /* BB off                                                       */
   w8_clr(REG_RF_CTRL, 0x07);                   /* RF off                                                       */
   w32_clr(REG_WLRF1, 0x07000000UL);
}

/* rtw_mac_power_switch: 1 = done, 0 = already in that state, -1 = failed */
static int power_switch(int on)
{
   int cur;

   if (rtw_r16(REG_MCUFW_CTRL) == 0xC078)       /* firmware still there: toggle RPWM                            */
      rtw_w8(RPWM_ADDR, (UCHAR)((rtw_r8(RPWM_ADDR) ^ 0x80) & 0x80));
   cur = rtw_r8(REG_CR) != 0xEA;
   if (cur == on)
      return 0;
   if (pw_run(on ? g_pw_on : g_pw_off))
      return -1;
   return 1;
}

static int mac_power_on(void)                    /* rtw_mac_power_on + __rtw_mac_init_system_cfg                */
{
   int rc;
   ULONG v;

   pre_system_cfg();
   rc = power_switch(1);
   if (rc == 0) {                               /* already on (warm boot): off and on again                     */
      g_was_on = 1;
      power_switch(0);
      pre_system_cfg();
      rc = power_switch(1);
   }
   if (rc < 0)
      return -1;
   w32_set(REG_CPU_DMEM_CON, 0x00010100UL);     /* BIT_WL_PLATFORM_RST | BIT_DDMA_EN                            */
   w8_set(REG_SYS_FUNC_EN + 1, 0xD8);           /* chip->sys_func_en                                            */
   rtw_w8(REG_CR_EXT + 3, (UCHAR)((rtw_r8(REG_CR_EXT + 3) & 0xF0) | 0x0C));
   v = rtw_r32(REG_MCUFW_CTRL);
   if (v & 0x00100000UL) {                      /* boot from flash off: the driver loads the firmware           */
      rtw_w32(REG_MCUFW_CTRL, v & ~0x00100000UL);
      w32_clr(REG_GPIO_MUXCFG, 0x00080000UL);
   }
   return 0;
}

/* ---- firmware download (mac.c __rtw_download_firmware) --------------------------------------------------------
 * The image is 64 bytes of header (struct rtw_fw_hdr), then DMEM, IMEM and (mem_usage bit 4) EMEM, each followed by
 * an 8-byte checksum. Each section goes in pieces of up to 4 KB: the piece is sent as reserved-page data through the
 * beacon queue into TX buffer page 0, then the DDMA engine copies it from the TX buffer (OCP 0x18780000 + 48, after
 * the TX descriptor) to the CPU memory address of the section, accumulating a checksum. */
#define OCP_TXBUF   0x18780000UL
#define OCP_DMEM    0x00200000UL

static int ddma(ULONG src, ULONG dst, ULONG ctrl)
{
   if (!hw_ready(REG_DDMA_CH0CTRL, 0x80000000UL, 0UL))
      return -1;
   rtw_w32(REG_DDMA_CH0SA, src);
   rtw_w32(REG_DDMA_CH0DA, dst);
   rtw_w32(REG_DDMA_CH0CTRL, ctrl);
   if (!hw_ready(REG_DDMA_CH0CTRL, 0x80000000UL, 0UL))
      return -1;
   return 0;
}

/* rtw_fw_write_data_rsvd_page: the piece, already in the beacon buffer, to page 0 */
static int rsvd_write(USHORT len)
{
   UCHAR cr1, txq2;
   int   rc = 0;

   rtw_w16(REG_FIFOPAGE_CTRL_2, 0x8000);        /* head page 0 | BIT_BCN_VALID_V1                               */
   cr1 = rtw_r8(REG_CR + 1);
   rtw_w8(REG_CR + 1, (UCHAR)(cr1 | 0x01));     /* BIT_ENSWBCN                                                  */
   txq2 = rtw_r8(REG_FWHW_TXQ_CTRL + 2);
   rtw_w8(REG_FWHW_TXQ_CTRL + 2, (UCHAR)(txq2 & ~0x40));   /* BIT_EN_BCNQ_DL off                                */
   r88_rsvd_kick(len);
   if (!hw_ready(REG_FIFOPAGE_CTRL_2, 0x00008000UL, 0x00008000UL))
      rc = -1;
   rtw_w16(REG_FIFOPAGE_CTRL_2, (USHORT)(g_rsvd_boundary | 0x8000));
   rtw_w8(REG_FWHW_TXQ_CTRL + 2, txq2);
   rtw_w8(REG_CR + 1, cr1);
   return rc;
}

static int fw_section(ULONG off, ULONG dst, ULONG size)
{
   ULONG  done = 0, ctrl;
   USHORT n;
   UCHAR  fc;

   w32_set(REG_DDMA_CH0CTRL, 0x02000000UL);     /* BIT_DDMACH0_RESET_CHKSUM_STS                                 */
   while (done < size) {
      n = (USHORT)((size - done) > 4096UL ? 4096 : (size - done));
      if (blob_read(&g_fwblob, off + done, r88_rsvd_buf(), n))
         return -2;
      if (rsvd_write(n)) { g_fw_err = 3; return -3; }
      ctrl = 0xA0000000UL | (ULONG)n;           /* OWN | CHKSUM_EN | length                                     */
      if (done)
         ctrl |= 0x01000000UL;                  /* CHKSUM_CONT                                                  */
      if (ddma(OCP_TXBUF + TXD88, dst + done, ctrl)) { g_fw_err = 4; return -4; }
      done += n;
   }
   fc = rtw_r8(REG_MCUFW_CTRL);                 /* check_fw_checksum                                            */
   if (rtw_r32(REG_DDMA_CH0CTRL) & 0x08000000UL) {
      fc = (UCHAR)((fc | (dst < OCP_DMEM ? 0x08 : 0x20)) & ~(dst < OCP_DMEM ? 0x10 : 0x40));
      rtw_w8(REG_MCUFW_CTRL, fc);
      g_fw_err = 5;
      return -5;
   }
   rtw_w8(REG_MCUFW_CTRL, (UCHAR)(fc | (dst < OCP_DMEM ? 0x18 : 0x60)));
   return 0;
}

static void wlan_cpu(int on)
{
   if (on) {
      w8_set(REG_RSV_CTRL + 1, 0x01);           /* BIT_WLMCU_IOIF                                               */
      w8_set(REG_SYS_FUNC_EN + 1, 0x04);        /* BIT_FEN_CPUEN                                                */
   } else {
      w8_clr(REG_SYS_FUNC_EN + 1, 0x04);
      w8_clr(REG_RSV_CTRL + 1, 0x01);
   }
}

static int fw_download(void)
{
   static UCHAR h[64];
   ULONG  dsz, isz, esz, daddr, iaddr, eaddr, lte;
   UCHAR  b_pq, b_cr, b_bcn;
   USHORT b_fifo, w;
   ULONG  b_rqpn;
   int    rc;

   g_fw_err = 1;
   if (blob_read(&g_fwblob, 0UL, (UCHAR FAR *)h, 64))
      return -1;
   g_fw_ver = get16((UCHAR FAR *)h + 4);
   g_fw_sub = h[6];
   g_fw_h2cfmt = get16((UCHAR FAR *)h + 0x1C);
   daddr = get32((UCHAR FAR *)h + 0x20) & 0x7FFFFFFFUL;
   dsz   = get32((UCHAR FAR *)h + 0x24) + 8;
   isz   = get32((UCHAR FAR *)h + 0x30) + 8;
   esz   = (h[0x18] & 0x10) ? get32((UCHAR FAR *)h + 0x34) + 8 : 0UL;
   eaddr = get32((UCHAR FAR *)h + 0x38) & 0x7FFFFFFFUL;
   iaddr = get32((UCHAR FAR *)h + 0x3C) & 0x7FFFFFFFUL;
   if (64UL + dsz + isz + esz != g_fwblob.len)
      return -1;                                /* check_firmware_size                                          */
   g_fw_err = 2;
   if (ltecoex_read(0x38, &lte))
      return -1;
   wlan_cpu(0);
   /* download_firmware_reg_backup: only the high queue, beacon functions off */
   b_pq = rtw_r8(REG_TXDMA_PQ_MAP + 1);
   rtw_w8(REG_TXDMA_PQ_MAP + 1, 0xC0);          /* HIQ -> high priority (RTW_DMA_MAPPING_HIGH << 6)             */
   b_cr = rtw_r8(REG_CR);
   rtw_w8(REG_CR, 0x05);                        /* HCI_TXDMA_EN | TXDMA_EN                                      */
   rtw_w32(REG_H2CQ_CSR, 0x80000000UL);
   b_fifo = rtw_r16(REG_FIFOPAGE_INFO_1);
   b_rqpn = rtw_r32(REG_RQPN_CTRL_2) | 0x80000000UL;
   rtw_w16(REG_FIFOPAGE_INFO_1, 0x0200);
   rtw_w32(REG_RQPN_CTRL_2, b_rqpn);
   b_bcn = rtw_r8(REG_BCN_CTRL);
   rtw_w8(REG_BCN_CTRL, (UCHAR)((b_bcn & ~0x08) | 0x10));
   /* download_firmware_reset_platform */
   w8_clr(REG_CPU_DMEM_CON + 2, 0x01);
   w8_clr(REG_SYS_CLK_CTRL + 1, 0x40);
   w8_set(REG_CPU_DMEM_CON + 2, 0x01);
   w8_set(REG_SYS_CLK_CTRL + 1, 0x40);
   /* start_download_firmware */
   w = (USHORT)((rtw_r16(REG_MCUFW_CTRL) & 0x3800) | 0x0001);   /* BIT_MCUFWDL_EN                              */
   rtw_w16(REG_MCUFW_CTRL, w);
   rc = fw_section(64UL, daddr, dsz);
   if (!rc)
      rc = fw_section(64UL + dsz, iaddr, isz);
   if (!rc && esz)
      rc = fw_section(64UL + dsz + isz, eaddr, esz);
   /* download_firmware_reg_restore */
   rtw_w8(REG_TXDMA_PQ_MAP + 1, b_pq);
   rtw_w8(REG_CR, b_cr);
   rtw_w32(REG_H2CQ_CSR, 0x80000000UL);
   rtw_w16(REG_FIFOPAGE_INFO_1, b_fifo);
   rtw_w32(REG_RQPN_CTRL_2, b_rqpn);
   rtw_w8(REG_BCN_CTRL, b_bcn);
   if (rc) {
      w8_clr(REG_MCUFW_CTRL, 0x01);
      w8_set(REG_SYS_FUNC_EN + 1, 0x04);
      return rc;
   }
   /* download_firmware_end_flow */
   rtw_w32(REG_TXDMA_STATUS, 0x00000004UL);     /* BTI_PAGE_OVF                                                 */
   w = rtw_r16(REG_MCUFW_CTRL);
   if ((w & 0x0050) == 0x0050)
      rtw_w16(REG_MCUFW_CTRL, (USHORT)((w | 0x4000) & ~0x0001));   /* FW_DW_RDY, MCUFWDL_EN off               */
   wlan_cpu(1);
   g_fw_err = 6;
   if (ltecoex_write(0x38, lte))
      return -6;
   /* download_firmware_validate: (MCUFW_CTRL & 0xCFFF) == FW_READY (0xC078); Linux waits 10 ms, here up to 500 */
   {
      USHORT n;

      for (n = 0; n < 250; n++) {
         if ((rtw_r16(REG_MCUFW_CTRL) & 0xCFFF) == 0xC078)
            break;
         r88_mdelay(2);
      }
      g_mcufw = rtw_r32(REG_MCUFW_CTRL);
      g_fw_dbg7 = rtw_r32(REG_FW_DBG7);
      if (n >= 250) {
         g_fw_err = 7;                          /* FW_DBG7 & 0xFFFFFF00 == 0xFAAAAA00: invalid key             */
         return -7;
      }
   }
   r88_trx_setup();                             /* reset descriptors and indexes (rtw_hci_setup)               */
   g_h2c_box = 0;
   g_h2c_seq = 0;
   h2c_cmd(0x000001D1UL, 0UL);                  /* rtw_fw_set_recover_bt_device (8821C on PCIe)                */
   g_fw_err = 0;
   return 0;
}

/* ---- eFuse (efuse.c rtw_dump_physical_efuse_map + rtw_dump_logical_efuse_map; rtw8821c_read_efuse) ----------- */
static int ef_byte(USHORT addr, UCHAR *out)
{
   static ULONG c;
   USHORT n;

   c = rtw_r32(REG_EFUSE_CTRL);
   c &= ~(0x000000FFUL | 0x0003FF00UL);
   c |= ((ULONG)(addr & 0x3FF)) << 8;
   rtw_w32(REG_EFUSE_CTRL, c & ~0x80000000UL);
   for (n = 0; n < 10000; n++) {
      dd_udelay(1);
      c = rtw_r32(REG_EFUSE_CTRL);
      if (c & 0x80000000UL)
         break;
   }
   if (!(c & 0x80000000UL))
      return -1;
   *out = (UCHAR)c;
   return 0;
}

static void efuse_read(void)
{
   static UCHAR h1, h2, b0, b1;
   USHORT p = 0, i, blk, w, li;
   UCHAR  wen;

   for (i = 0; i < 512; i++) g_ef[i] = 0xFF;
   g_ef_ok = 0;
   w32_mask(REG_LDO_EFUSE_CTRL, 0x00000300UL, 0UL);   /* WiFi bank                                            */
   w8_clr(REG_LDO_EFUSE_CTRL + 3, 0x80);              /* cfg_ldo25(false)                                     */
   while (p + 1 < 512 - 96) {                          /* physical size - protected size                       */
      if (ef_byte(p, &h1) || ef_byte((USHORT)(p + 1), &h2))
         return;
      if (h1 == 0xFF || ((h1 & 0x1F) == 0x0F && h2 == 0xFF))
         break;
      if ((h1 & 0x1F) == 0x0F) {
         blk = (USHORT)(((h2 & 0xF0) >> 1) | ((h1 >> 5) & 0x07));
         wen = (UCHAR)(h2 & 0x0F);
         p += 2;
      } else {
         blk = (USHORT)((h1 & 0xF0) >> 4);
         wen = (UCHAR)(h1 & 0x0F);
         p += 1;
      }
      for (w = 0; w < 4; w++) {
         if (wen & (1 << w))
            continue;
         li = (USHORT)((blk << 3) + (w << 1));
         if (p + 1 > 512 - 96 || li + 1 > 511)
            return;
         if (ef_byte(p, &b0) || ef_byte((USHORT)(p + 1), &b1))
            return;
         g_ef[li] = b0;
         g_ef[li + 1] = b1;
         p += 2;
      }
   }
   g_ef_ok = 1;
   g_rfe = (UCHAR)(g_ef[0xCA] & 0x1F);
   g_pkg1 = (UCHAR)((g_ef[0xCA] & 0x20) ? 1 : 0);
   if (g_ef[0xCA] == 0xFF) { g_rfe = 0; g_pkg1 = 0; }
   g_rfe_btg = (UCHAR)(g_rfe == 2 || g_rfe == 4 || g_rfe == 7 || g_rfe == 0x0A || g_rfe == 0x0C || g_rfe == 0x0F);
   g_xtal = (UCHAR)(g_ef[0xB9] == 0xFF ? 0 : g_ef[0xB9]);
   g_thermal = g_ef[0xBA];
   g_boardopt = (UCHAR)(g_ef[0xC1] == 0xFF ? 0 : g_ef[0xC1]);
   g_btset = g_ef[0xC3];
   g_swing2g = (UCHAR)(g_ef[0xC6] == 0xFF ? 0 : g_ef[0xC6]);
   g_swing5g = (UCHAR)(g_ef[0xC7] == 0xFF ? 0 : g_ef[0xC7]);
   if (!(g_ef[0xD0] & 1) && (g_ef[0xD0] & g_ef[0xD1] & g_ef[0xD2] & g_ef[0xD3] & g_ef[0xD4] & g_ef[0xD5]) != 0xFF)
      for (i = 0; i < 6; i++)
         g_hw_mac[i] = g_ef[0xD0 + i];         /* struct rtw8821ce_efuse: mac_addr at 0xD0                     */
}

/* ---- MAC set-up (mac.c rtw_mac_init) ------------------------------------------------------------------------ */
static int mac_init(void)
{
   USHORT n;
   ULONG  v, wp, rp;

   /* txdma_queue_mapping, rqpn_table_8821c[1]: VO, VI normal; BE, BK low; MG extra; HI high */
   rtw_w16(REG_TXDMA_PQ_MAP, 0xC5A0);
   rtw_w8(REG_CR, 0);
   rtw_w8(REG_CR, 0xFF);                        /* MAC_TRX_ENABLE                                               */
   rtw_w32(REG_H2CQ_CSR, 0x80000000UL);
   /* priority_queue_cfg: 512 pages of 128 bytes; 52 reserved (driver 8, H2C extra info 24, static info 8, H2C queue 8,
    * firmware TX buffer 4) from page 460; page_table_8821c[1]: high 16, normal 16, low 16, extra 14, gap 1 */
   g_rsvd_boundary = 460;
   rtw_w16(REG_FIFOPAGE_INFO_1, 16);
   rtw_w16(REG_FIFOPAGE_INFO_2, 16);
   rtw_w16(REG_FIFOPAGE_INFO_3, 16);
   rtw_w16(REG_FIFOPAGE_INFO_4, 14);
   rtw_w16(REG_FIFOPAGE_INFO_5, 460 - 16 - 16 - 16 - 14 - 1);
   w32_set(REG_RQPN_CTRL_2, 0x80000000UL);      /* BIT_LD_RQPN                                                  */
   rtw_w16(REG_FIFOPAGE_CTRL_2, 460);
   w8_set(REG_FWHW_TXQ_CTRL + 2, 0x10);         /* BIT_EN_WR_FREE_TAIL                                          */
   rtw_w16(REG_BCNQ_BDNY_V1, 460);
   rtw_w16(REG_FIFOPAGE_CTRL_2 + 2, 460);
   rtw_w16(REG_BCNQ1_BDNY_V1, 460);
   rtw_w32(REG_RXFF_BNDY, 16384UL - 256 - 1);   /* rxff_size - C2H_PKT_BUF - 1                                  */
   w8_set(REG_AUTO_LLT_V1, 0x01);               /* BIT_AUTO_INIT_LLT_V1                                         */
   if (!hw_ready(REG_AUTO_LLT_V1, 0x00000001UL, 0UL))
      return -1;
   rtw_w8(REG_CR + 3, 0);
   /* init_h2c: the H2C queue in pages 500..507 */
   v = (rtw_r32(REG_H2C_HEAD) & 0xFFFC0000UL) | (500UL << 7);
   rtw_w32(REG_H2C_HEAD, v);
   v = (rtw_r32(REG_H2C_READ_ADDR) & 0xFFFC0000UL) | (500UL << 7);
   rtw_w32(REG_H2C_READ_ADDR, v);
   v = (rtw_r32(REG_H2C_TAIL) & 0xFFFC0000UL) | ((500UL << 7) + 1024UL);
   rtw_w32(REG_H2C_TAIL, v);
   rtw_w8(REG_H2C_INFO, (UCHAR)((rtw_r8(REG_H2C_INFO) & 0xFC) | 0x01));
   rtw_w8(REG_H2C_INFO, (UCHAR)((rtw_r8(REG_H2C_INFO) & 0xFB) | 0x04));
   rtw_w8(REG_TXDMA_OFFSET_CHK + 1, (UCHAR)((rtw_r8(REG_TXDMA_OFFSET_CHK + 1) & 0x7F) | 0x80));
   wp = rtw_r32(REG_H2C_PKT_WRITEADDR) & 0x3FFFFUL;
   rp = rtw_r32(REG_H2C_PKT_READADDR) & 0x3FFFFUL;
   if (wp != rp)
      return -2;                                /* "H2C queue mismatch"                                         */
   /* rtw8821c_mac_init */
   rtw_w8(REG_AMPDU_MAX_TIME_V1, 0x70);
   w8_set(REG_TX_HANG_CTRL, 0x04);              /* BIT_EN_EOF_V1                                                */
   n = 0x1E4 | 0x0800;                          /* WLAN_PRE_TXCNT_TIME_TH | BIT_EN_PRECNT                       */
   rtw_w8(REG_PRECNT_CTRL, (UCHAR)n);
   rtw_w8(REG_PRECNT_CTRL + 1, (UCHAR)(n >> 8));
   rtw_w32(REG_PROT_MODE_CTRL, 0x202008FFUL);   /* RTS length 0xFF, RTS time 8, max agg 0x20, RTS max agg 0x20  */
   rtw_w16(REG_BAR_MODE_CTRL + 2, 0x0801);      /* BAR retry 1, RA try rate agg limit 8                         */
   rtw_w8(REG_FAST_EDCA_VOVI, 0x06);
   rtw_w8(REG_FAST_EDCA_VOVI + 2, 0x06);
   rtw_w8(REG_FAST_EDCA_BEBK, 0x06);
   rtw_w8(REG_FAST_EDCA_BEBK + 2, 0x06);
   w8_set(REG_INIRTS_RATE_SEL, 0x20);
   w8_clr(REG_TIMER0_SRC_SEL, 0x70);            /* BIT_TSFT_SEL_TIMER0                                          */
   rtw_w16(REG_TXPAUSE, 0);
   rtw_w8(REG_SLOT, 0x09);
   rtw_w8(REG_PIFS, 0x19);
   rtw_w32(REG_SIFS, 0x10100E0AUL);             /* CCK cont 0xA, OFDM cont 0xE, CCK TRX 0x10, OFDM TRX 0x10     */
   rtw_w16(REG_EDCA_VO_PARAM + 2, 0x0186);
   rtw_w16(REG_EDCA_VI_PARAM + 2, 0x03BC);
   rtw_w32(REG_RD_NAV_NXT, 0x001B0005UL);       /* WLAN_NAV_CFG                                                 */
   rtw_w16(REG_RXTSF_OFFSET_CCK, 0x3030);
   w8_set(REG_BCN_CTRL, 0x08);                  /* BIT_EN_BCN_FUNCTION                                          */
   rtw_w32(REG_TBTT_PROHIBIT, 0x00006404UL);    /* WLAN_TBTT_PROHIBIT | hold 0x64 << 8                          */
   rtw_w8(REG_DRVERLYINT, 0x04);
   rtw_w8(REG_BCNDMATIM, 0x02);
   w8_clr(REG_TX_PTCL_CTRL + 1, 0x10);          /* BIT_SIFS_BK_EN >> 8                                          */
   rtw_w32(REG_RXFLTMAP0, 0x0FFFFFFFUL);
   rtw_w16(REG_RXFLTMAP2, 0xFFFF);
   rtw_w32(REG_RCR, R88_RCR_SCAN);
   rtw_w8(REG_RX_PKT_LIMIT, 12288 >> 9);
   rtw_w8(REG_TCR + 2, 0x30);
   rtw_w8(REG_TCR + 1, 0x30);
   rtw_w8(REG_ACKTO_CCK, 0x40);
   w8_set(REG_WMAC_TRXPTCL_CTL_H, 0x02);
   w8_set(REG_SND_PTCL_CTRL, 0x40);             /* BIT_DIS_CHK_VHTSIGB_CRC                                      */
   rtw_w32(REG_WMAC_OPTION_FUNCTION + 8, 0xB0810041UL);
   rtw_w8(REG_WMAC_OPTION_FUNCTION + 4, 0x98);
   /* rtw_drv_info_cfg */
   rtw_w8(REG_RX_DRVINFO_SZ, 4);
   rtw_w8(REG_TRXFF_BNDY + 1, (UCHAR)((rtw_r8(REG_TRXFF_BNDY + 1) & 0xF0) | 0x0F));
   w32_set(REG_RCR, 0x10000000UL);              /* BIT_APP_PHYSTS                                               */
   w32_clr(REG_WMAC_OPTION_FUNCTION + 4, 0x00000300UL);
   return 0;
}

/* ---- Bluetooth coexistence, WiFi only (coex.c rtw_coex_power_on_setting + rtw_coex_init_hw_config(wifi_only),
 * rtw8821c_coex_cfg_*): the antenna and the PTA grant go to WiFi; OS/2 does not run the Bluetooth half. ----------- */
static void ltecoex_mask(USHORT off, ULONG mask, ULONG val)
{
   ULONG v, m = mask;

   if (ltecoex_read(off, &v))
      return;
   while (m && !(m & 1UL)) {
      m = shr1(m);
      val = shl1(val);
   }
   ltecoex_write(off, (v & ~mask) | (val & mask));
}

static void coex_ant_switch_wlg(void)            /* rtw8821c_coex_cfg_ant_switch(BBSW, WLG)                     */
{
   UCHAR pol = (UCHAR)(g_rfe == 3 || g_rfe == 11 || g_rfe == 4 || g_rfe == 12);
   UCHAR wlg_at_btg = (UCHAR)(g_rfe == 2 || g_rfe == 10 || g_rfe == 7 || g_rfe == 15 || g_rfe == 4 || g_rfe == 12);
   UCHAR rv;

   if (g_rfe == 5 || g_rfe == 13 || g_rfe == 6 || g_rfe == 14)
      return;                                   /* no antenna switch on the board                               */
   w32_clr(REG_LED_CFG, 0x00800000UL);          /* BIT_DPDT_SEL_EN                                              */
   w32_set(REG_LED_CFG, 0x01000000UL);          /* BIT_DPDT_WL_SEL                                              */
   w8_mask(0x0CB4, 0xFF, 0x77);                 /* REG_RFE_CTRL8: DPDT_CTRL_PIN                                 */
   if (wlg_at_btg) {                            /* the WLG path is behind the BT path: switch to WLA or WLG_BT  */
      if (pol)
         rv = 0x2;                              /* COEX_SWITCH_TO_WLA with inverted polarity                    */
      else
         rv = (UCHAR)((g_rfe != 4 && g_rfe != 2) ? 0x3 : 0x2);   /* COEX_SWITCH_TO_WLG_BT                       */
   } else
      rv = (UCHAR)(pol ? 0x1 : 0x2);            /* COEX_SWITCH_TO_WLG                                           */
   w32_mask(0x0CB4, 0xF0000000UL, rv);
   w8_set(REG_CTRL_TYPE, 0x30);                 /* BIT_CTRL_TYPE1 | BIT_CTRL_TYPE2                              */
   g_ant_ok = 1;
}

static void coex_init(void)
{
   /* rtw_coex_power_on_setting */
   w8_set(REG_SYS_FUNC_EN, 0x03);
   w32_clr(REG_PAD_CTRL1, 0x00100000UL | 0x01000000UL | 0x00008000UL);   /* gnt debug: BTGP_SPI/JTAG, LED1DIS   */
   w32_clr(REG_GPIO_MUXCFG, 0x00080000UL);      /* BIT_FSPI_EN                                                  */
   w32_clr(REG_SYS_SDIO_CTRL, 0x00040000UL | 0x08000000UL);   /* BIT_SDIO_INT, BIT_DBG_GNT_WL_BT                 */
   /* rtw_coex_init_hw_config: rtw8821c_coex_cfg_init */
   w8_set(REG_BCN_CTRL, 0x08);
   w8_mask(REG_BT_TDMA_TIME, 0x3F, 0x05);
   rtw_w8(REG_BT_STAT_CTRL, 0x01);
   w32_set(REG_GPIO_MUXCFG, 0x00000020UL | 0x00000200UL);   /* BT PTA                                           */
   w8_set(REG_QUEUE_CTRL, 0x10);                /* BIT_PTA_WL_TX_EN                                             */
   w8_clr(REG_QUEUE_CTRL, 0x20);                /* BIT_PTA_EDCCA_EN                                             */
   w16_set(REG_BT_COEX_V2, 0x1000);             /* BIT_GNT_BT_POLARITY                                          */
   w8_mask(REG_BT_COEX_TABLE_H + 3, 0x08, 1);   /* beacon queue high priority                                   */
   w8_mask(REG_BT_COEX_TABLE_H + 0, 0x08, 1);   /* WL priority: TX response (bit 3)                             */
   w8_mask(REG_BT_COEX_TABLE_H + 0, 0x10, 1);   /* TX beacon (bit 4)                                            */
   w8_mask(REG_BT_COEX_TABLE_H + 3, 0x08, 1);   /* TX beacon queue (bit 27)                                     */
   /* COEX_SET_ANT_WONLY: GNT_BT software low, GNT_WL software high, WiFi owns the path, antenna to WLG */
   ltecoex_mask(0x38, 0x0000C000UL, 1UL);
   ltecoex_mask(0x38, 0x00000C00UL, 1UL);
   ltecoex_mask(0x38, 0x00003000UL, 3UL);
   ltecoex_mask(0x38, 0x00000300UL, 3UL);
   w8_set(REG_SYS_SDIO_CTRL + 3, 0x04);         /* BIT_LTE_MUX_CTRL_PATH >> 24                                  */
   coex_ant_switch_wlg();
   rtw_w16(REG_WIFI_BT_INFO, 0x8003);           /* scoreboard: active | on/off, BIT_BT_INT_EN                   */
   if (g_btset & 0x01) {                        /* shared antenna: coex table case 1                            */
      rtw_w32(REG_BT_COEX_TABLE0, 0x55555555UL);
      rtw_w32(REG_BT_COEX_TABLE1, 0x55555555UL);
      rtw_w32(REG_BT_COEX_BRK, 0xF0FFFFFFUL);
   }
}

/* ---- bring-up ----------------------------------------------------------------------------------------------- */
static void step(USHORT s)
{
   g_r88_step = s;
   g_hw_state = s;
   tcom('{'); th8((UCHAR)s); tcom('}');
}

static int fail(const char *what, ULONG v)
{
   dd_err(what, v);
   g_hw_failed = 1;
   g_trx_on = 0;
   return -1;
}

/* PCIe PHY parameter (rtw_pci_phy_cfg: pcie_gen1_param_8821c, MDIO 0x09 = 0x6380) and the completion timeout off */
static void pcie_cfg(void)
{
   USHORT n;
   UCHAR  cap;
   USHORT guard = 0;

   rtw_w16(REG_MDIO_V1, 0x6380);
   rtw_w8(REG_PCIE_MIX_CFG, 0x09);
   rtw_w8(REG_PCIE_MIX_CFG + 3, 0);             /* page 0, gen 1                                                */
   w32_set(REG_PCIE_MIX_CFG, 0x00000020UL);     /* BIT_MDIO_WFLAG_V1                                            */
   for (n = 0; n < 20 && (rtw_r32(REG_PCIE_MIX_CFG) & 0x20UL); n++)
      dd_udelay(10);
   cap = (UCHAR)(pci_cfg_rd16(0x34) & 0xFC);
   while (cap && guard++ < 48) {
      U32 c;

      c.l = pci_cfg_rd32(cap);
      if (c.b[0] == 0x10) {                     /* PCI Express capability: Device Control 2 at +28h             */
         pci_cfg_wr16((UCHAR)(cap + 0x28), (USHORT)(pci_cfg_rd16((UCHAR)(cap + 0x28)) | 0x0010));
         break;
      }
      cap = (UCHAR)(c.b[1] & 0xFC);
   }
}

int hw_up(void)
{
   static UCHAR body[8];
   USHORT i;
   int    rc;

   g_hw_failed = 0;
   g_hw_ready = 0;
   g_trx_on = 0;
   step(1);
   if (!g_fwblob.done)
      return fail("the firmware file has not been written to the device", 0UL);
   step(2);
   pci_enable();
   if (bar_map())
      return fail("the register BAR could not be mapped", g_bar0);
   step(3);
   if (r88_trx_alloc())
      return fail("not enough DMA memory for the rings", (ULONG)dma_free_bytes());
   if (hook_init() || irq_hook())
      tstr("[r88: no interrupt: polled]");
   r88_irq_disable();
   g_syscfg = rtw_r32(REG_SYS_CFG1);
   if (g_syscfg == 0UL || g_syscfg == 0xFFFFFFFFUL)
      return fail("the adapter does not answer (REG_SYS_CFG1)", g_syscfg);
   g_cut = (UCHAR)((LO16(g_syscfg) >> 12) & 0x0F);
   pcie_cfg();
   step(4);
   r88_trx_setup();
   if (mac_power_on())
      return fail("MAC power-on failed (power sequence entry)", (ULONG)g_pwr_fail);
   step(5);
   g_trx_on = 1;                                /* the beacon ring carries the firmware                         */
   rc = fw_download();
   g_trx_on = 0;
   if (rc)
      return fail("firmware download failed (FWERR in the status)", (ULONG)g_fw_err);
   step(6);
   efuse_read();
   if (!g_cfgmac && (g_hw_mac[0] | g_hw_mac[1] | g_hw_mac[2] | g_hw_mac[3] | g_hw_mac[4] | g_hw_mac[5])) {
      fmemcpy((UCHAR FAR *)g_mymac, (UCHAR FAR *)g_hw_mac, 6);
      ndis_set_mac(g_mymac);
   }
   if (g_rfe != 0 && g_rfe != 2 && g_rfe != 4 && g_rfe != 6)
      tstr("[r88: RFE type not supported by Linux either; default tables]");
   step(7);
   if ((rc = mac_init()) != 0)
      return fail("MAC set-up failed (1 auto LLT, 2 H2C queue)", (ULONG)(USHORT)(-rc));
   step(8);
   r88_phy_set_param();
   r88_set_channel(1);
   g_curch = 1;
   step(9);
   g_trx_on = 1;
   r88_irq_enable();                            /* rtw_hci_start                                                */
   fmemset((UCHAR FAR *)body, 0, sizeof(body));
   body[2] = (UCHAR)(508 - 460);                /* general info: firmware TX boundary                          */
   h2c_pkt(0x0D, (UCHAR FAR *)body, 4);
   body[0] = g_rfe;                             /* PHYDM info: RFE, RF type 1T1R (4), cut, antennas A / A       */
   body[1] = 4;
   body[2] = g_cut;
   body[3] = 0x11;
   h2c_pkt(0x11, (UCHAR FAR *)body, 8);
   coex_init();
   step(10);
   w16_set(REG_CR, 0x0200);                     /* rtw_sec_enable_sec_engine: RTW_SEC_ENGINE_EN                 */
   rtw_w16(REG_SECCFG, (USHORT)(rtw_r16(REG_SECCFG) | 0x00CF));
   r88_keys_reset();
   for (i = 0; i < 6; i++)
      rtw_w8(REG_MACID + i, g_mymac[i]);        /* port 0 address (rtw_vif_port_config)                         */
   w32_mask(REG_CR, 0x00030000UL, 0UL);         /* port 0 net type: no link                                     */
   rtw_w8(REG_BCN_CTRL, 0x08);                  /* station: BIT_EN_BCN_FUNCTION                                 */
   rtw_w32(REG_RCR, R88_RCR_SCAN);
   r88_dig_set(0x20);
   r88_sta_reset();
   step(0x64);
   g_hw_ready = 1;
   tstr("[r88: ready, firmware "); th16(g_fw_ver); tcom('.'); th8((UCHAR)g_fw_sub); tstr("]"); tnl();
   return 0;
}

void hw_down(void)
{
   if (!g_hw_ready && !g_hw_state)
      return;
   if (g_joined)
      sta_leave();
   g_hw_ready = 0;
   r88_irq_disable();
   g_trx_on = 0;
   if (g_nbarsel) {
      rtw_w16(REG_WIFI_BT_INFO, 0x8000);        /* rtw_coex_power_off_setting                                   */
      power_switch(0);
   }
   g_hw_state = 0;
}

/* ---- status ---------------------------------------------------------------------------------------------------- */
void hw_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void))
{
   ps("R88 step="); ph(g_r88_step, 2); ps(" warm="); ph(g_was_on, 1); ps(" SYSCFG1="); ph(g_syscfg, 8);
   ps(" cut="); ph(g_cut, 1); ps(" pwrfail="); ph(g_pwr_fail, 2); nl();
   ps("FW ver="); ph(g_fw_ver, 4); ps(" sub="); ph(g_fw_sub, 2); ps(" h2cfmt="); ph(g_fw_h2cfmt, 4);
   ps(" FWERR="); ph(g_fw_err, 1); ps(" MCUFW="); ph(g_mcufw, 8); ps(" FWDBG7="); ph(g_fw_dbg7, 8);
   ps(" H2CBUSY="); ph(g_h2c_busy, 4); ps(" H2CSEQ="); ph(g_h2c_seq, 4); nl();
   ps("EFUSE ok="); ph(g_ef_ok, 1); ps(" rfe="); ph(g_rfe, 2); ps(" pkg1="); ph(g_pkg1, 1); ps(" btg="); ph(g_rfe_btg, 1);
   ps(" tables="); ph(g_tblvar, 1); ps(" xtal="); ph(g_xtal, 2); ps(" board="); ph(g_boardopt, 2); ps(" bt="); ph(g_btset, 2);
   ps(" swing="); ph(g_swing2g, 2); ph(g_swing5g, 2); ps(" thermal="); ph(g_thermal, 2); ps(" ant="); ph(g_ant_ok, 1); nl();
   ps("PHY ch="); ph(g_channel, 2); ps(" band="); ph(g_band, 1); ps(" igi="); ph(g_igi, 2);
   ps(" txp cck/ofdm/ht="); ph(g_txp_cck, 2); ps("/"); ph(g_txp_ofdm, 2); ps("/"); ph(g_txp_ht, 2);
   ps(" IQK mode="); ph(g_iqk_mode, 1); ps(" n="); ph(g_iqk_n, 4); ps(" ok="); ph(g_iqk_ok, 4); ps(" ms="); ph(g_iqk_ms, 4); nl();
   ps("REGS CR="); ph(g_nbarsel ? rtw_r32(REG_CR) : 0UL, 8); ps(" RCR="); ph(g_nbarsel ? rtw_r32(REG_RCR) : 0UL, 8);
   ps(" HIMR0="); ph(g_nbarsel ? rtw_r32(REG_HIMR0) : 0UL, 8); ps(" HISR0="); ph(g_nbarsel ? rtw_r32(REG_HISR0) : 0UL, 8);
   ps(" PCICTRL="); ph(g_nbarsel ? rtw_r32(RTK_PCI_CTRL) : 0UL, 8); ps(" RXBDIDX="); ph(g_nbarsel ? rtw_r32(RXBD_IDX_MPDUQ) : 0UL, 8);
   ps(" BEIDX="); ph(g_nbarsel ? rtw_r32(TXBD_IDX_BEQ) : 0UL, 8); ps(" MGIDX="); ph(g_nbarsel ? rtw_r32(TXBD_IDX_MGMTQ) : 0UL, 8); nl();
   ps("TRX on="); ph(g_trx_on, 1); ps(" rxrp="); ph(r88_trx_state(0), 2); ps(" rxwp="); ph(g_nbarsel ? r88_trx_state(5) : 0, 2);
   ps(" bewp="); ph(r88_trx_state(1), 2); ps(" mgwp="); ph(r88_trx_state(2), 2); ps(" h2cwp="); ph(r88_trx_state(3), 2);
   ps(" masked="); ph(r88_trx_state(4), 1); ps(" svc="); ph(g_svc_runs, 4); ps(" isr0/1="); ph(g_isr0, 8); ps("/"); ph(g_isr1, 8); nl();
   ps("RX frames="); ph(g_rx_frames, 4); ps(" crc="); ph(g_rx_crc, 4); ps(" c2h="); ph(g_rx_c2h, 4); ps(" big="); ph(g_rx_big, 4);
   ps(" rdu="); ph(g_rx_rdu, 4); ps(" rate="); ph(g_rx_rate, 2); nl();
   ps("TX be="); ph(g_tx_ok[0], 4); ps(" mgmt="); ph(g_tx_ok[1], 4); ps(" h2c="); ph(g_tx_ok[2], 4);
   ps(" full be/mgmt/h2c="); ph(g_tx_full[0], 4); ps("/"); ph(g_tx_full[1], 4); ps("/"); ph(g_tx_full[2], 4);
   ps(" rate="); ph(g_txrate_auto ? 0xFF : g_txrate, 2); ps(" ctl="); ph(g_ctlrate, 2);
   ps(" keys p/g/dup="); ph(g_keys_p, 2); ph(g_keys_g, 2); ph(g_keys_dup, 2); ps(" scans="); ph(g_scan_n, 4); nl();
}

/* ---- debug commands -------------------------------------------------------------------------------------------- */
static char *skip(char *p)
{
   while (*p == ' ') p++;
   return p;
}

static ULONG hexv(char **pp)
{
   char  *p = skip(*pp);
   ULONG  v = 0;

   for (;; p++) {
      char   c = *p;
      USHORT d;

      if (c >= '0' && c <= '9') d = (USHORT)(c - '0');
      else if (c >= 'a' && c <= 'f') d = (USHORT)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = (USHORT)(c - 'A' + 10);
      else break;
      v = (v << 4) | d;
   }
   *pp = p;
   return v;
}

static int is(char *c, const char *w)
{
   while (*w)
      if (*c++ != *w++)
         return 0;
   return *c == ' ' || *c == 0;
}

int hw_cmd(char *c)
{
   static char *p;
   ULONG  a, v;

   if (!g_nbarsel)
      return 0;
   p = c;
   while (*p && *p != ' ') p++;
   if (is(c, "w1") || is(c, "w2") || is(c, "w4")) {
      a = hexv(&p); v = hexv(&p);
      if (c[1] == '1') rtw_w8((USHORT)a, (UCHAR)v);
      else if (c[1] == '2') rtw_w16((USHORT)a, (USHORT)v);
      else rtw_w32((USHORT)a, v);
   } else if (is(c, "r1")) {
      a = hexv(&p);
      tstr("[r1 "); th16((USHORT)a); tcom('='); th8(rtw_r8((USHORT)a)); tcom(']'); tnl();
   } else if (is(c, "rf")) {
      a = hexv(&p);
      if (*skip(p)) { v = hexv(&p); rf_write((UCHAR)a, v); }
      tstr("[rf "); th8((UCHAR)a); tcom('='); th32(rf_read((UCHAR)a)); tcom(']'); tnl();
   } else if (is(c, "ch")) {
      if (g_hw_ready && !g_joined) { v = hexv(&p); r88_set_channel((UCHAR)v); g_curch = (UCHAR)v; }
   } else if (is(c, "iqk")) {
      g_iqk_mode = (UCHAR)hexv(&p);
      if (g_hw_ready) r88_iqk();
   } else if (is(c, "dig")) {
      v = hexv(&p);
      g_dig_auto = (UCHAR)(v == 0);
      if (v) r88_dig_set((UCHAR)v);
   } else if (is(c, "rate")) {
      v = hexv(&p);
      g_txrate_auto = (UCHAR)(v == 0xFF);
      if (v != 0xFF) g_txrate = (UCHAR)v;
   } else if (is(c, "ctl")) {
      g_ctlrate = (UCHAR)hexv(&p);
   } else if (is(c, "down")) {
      hw_down();
   } else
      return 0;
   return 1;
}
