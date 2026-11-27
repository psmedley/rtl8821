/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * r88.h - the Realtek RTL8821CE (PCIe) back end: registers, descriptors and the interfaces between its files.
 *
 * The RTL8821C is a different chip from the RTL8821A of the rtw/ back end: Linux drives it with rtw88, not rtlwifi.
 * This back end follows rtw88 (drivers/net/wireless/realtek/rtw88/: main.c, mac.c, pci.c, fw.c, phy.c, efuse.c, tx.c,
 * sec.c, coex.c, rtw8821c.c, rtw8821c_table.c; Copyright(c) 2018-2019 Realtek Corporation, GPL-2.0 OR BSD-3-Clause)
 * and implements the same wlan/hw.h contract as the RTL8821AE back end. The station-side ideas (scan by channel
 * sweep, CCMP header from the driver, rate ladder, receiver gain) are those of the rtw/ back end.
 *
 * Files:  r88_io.c   register helpers, RF access, firmware commands (H2C mailbox and H2C packets)
 *         r88_hal.c  device table, power sequences, firmware download (reserved page + DDMA), MAC / FIFO set-up,
 *                    eFuse, BT coexistence (WiFi only), hw_up / hw_down, status, debug commands
 *         r88_phy.c  register tables, channel switch, transmit power, IQ calibration, receiver gain
 *         r88_trx.c  PCIe buffer-descriptor rings, transmit, receive, interrupt, service path
 *         r88_sta.c  the station operations of hw.h
 *         r88_tbl.c  the register tables (tools/gen_tbl88.py)
 */
#ifndef R88_H
#define R88_H

/* ---- registers (rtw88 reg.h, rtw8821c.h, pci.h) ----------------------------------------------------------- */
#define REG_SYS_FUNC_EN      0x0002
#define REG_SYS_PW_CTRL      0x0004
#define REG_SYS_CLK_CTRL     0x0008
#define REG_RSV_CTRL         0x001C
#define REG_RF_CTRL          0x001F
#define REG_AFE_XTAL_CTRL    0x0024           /* also REG_AFE_CTRL1 (MAC clock select, bits 21:20)            */
#define REG_AFE_PLL_CTRL     0x0028
#define REG_EFUSE_CTRL       0x0030
#define REG_LDO_EFUSE_CTRL   0x0034
#define REG_GPIO_MUXCFG      0x0040
#define REG_LED_CFG          0x004C
#define REG_PAD_CTRL1        0x0064
#define REG_CTRL_TYPE        0x0067
#define REG_SYS_SDIO_CTRL    0x0070
#define REG_HCI_OPT_CTRL     0x0074
#define REG_MCUFW_CTRL       0x0080
#define REG_WIFI_BT_INFO     0x00AA
#define REG_HIMR0            0x00B0
#define REG_HISR0            0x00B4
#define REG_HIMR1            0x00B8
#define REG_HISR1            0x00BC
#define REG_WLRF1            0x00EC
#define REG_SYS_CFG1         0x00F0
#define REG_CR               0x0100
#define REG_TXDMA_PQ_MAP     0x010C
#define REG_TRXFF_BNDY       0x0114
#define REG_RXFF_BNDY        0x011C
#define REG_HMETFR           0x01CC
#define REG_HMEBOX0          0x01D0
#define REG_HMEBOX0_EX       0x01F0
#define REG_FIFOPAGE_CTRL_2  0x0204
#define REG_AUTO_LLT_V1      0x0208
#define REG_TXDMA_OFFSET_CHK 0x020C
#define REG_TXDMA_STATUS     0x0210
#define REG_RQPN_CTRL_2      0x022C
#define REG_FIFOPAGE_INFO_1  0x0230
#define REG_FIFOPAGE_INFO_2  0x0234
#define REG_FIFOPAGE_INFO_3  0x0238
#define REG_FIFOPAGE_INFO_4  0x023C
#define REG_FIFOPAGE_INFO_5  0x0240
#define REG_H2C_HEAD         0x0244
#define REG_H2C_TAIL         0x0248
#define REG_H2C_READ_ADDR    0x024C
#define REG_H2C_INFO         0x0254
#define REG_FWHW_TXQ_CTRL    0x0420
#define REG_BCNQ_BDNY_V1     0x0424
#define REG_RRSR             0x0440
#define REG_CCK_CHECK        0x0454
#define REG_AMPDU_MAX_TIME_V1 0x0455
#define REG_BCNQ1_BDNY_V1    0x0456
#define REG_TX_HANG_CTRL     0x045E
#define REG_INIRTS_RATE_SEL  0x0480
#define REG_DATA_SC          0x0483
#define REG_QUEUE_CTRL       0x04C6
#define REG_PROT_MODE_CTRL   0x04C8
#define REG_BAR_MODE_CTRL    0x04CC
#define REG_PRECNT_CTRL      0x04E5
#define REG_EDCA_VO_PARAM    0x0500
#define REG_EDCA_VI_PARAM    0x0504
#define REG_PIFS             0x0512
#define REG_SIFS             0x0514
#define REG_SLOT             0x051B
#define REG_TX_PTCL_CTRL     0x0520
#define REG_TXPAUSE          0x0522
#define REG_TBTT_PROHIBIT    0x0540
#define REG_RD_NAV_NXT       0x0544
#define REG_BCN_CTRL         0x0550
#define REG_BCN_INTERVAL     0x0554
#define REG_DRVERLYINT       0x0558
#define REG_BCNDMATIM        0x0559
#define REG_USTIME_TSF       0x055C
#define REG_RXTSF_OFFSET_CCK 0x055E
#define REG_TIMER0_SRC_SEL   0x05B4
#define REG_TCR              0x0604
#define REG_RCR              0x0608
#define REG_RX_PKT_LIMIT     0x060C
#define REG_RX_DRVINFO_SZ    0x060F
#define REG_MACID            0x0610
#define REG_BSSID            0x0618
#define REG_USTIME_EDCA      0x0638
#define REG_ACKTO_CCK        0x0639
#define REG_WMAC_TRXPTCL_CTL 0x0668
#define REG_WMAC_TRXPTCL_CTL_H 0x066C
#define REG_CAMCMD           0x0670
#define REG_CAMWRITE         0x0674
#define REG_SECCFG           0x0680
#define REG_RXFLTMAP0        0x06A0
#define REG_RXFLTMAP2        0x06A4
#define REG_AID              0x06A8
#define REG_BT_COEX_TABLE0   0x06C0
#define REG_BT_COEX_TABLE1   0x06C4
#define REG_BT_COEX_BRK      0x06C8
#define REG_BT_COEX_TABLE_H  0x06CC
#define REG_SND_PTCL_CTRL    0x0718
#define REG_BT_COEX_V2       0x0762
#define REG_BT_STAT_CTRL     0x0778
#define REG_BT_TDMA_TIME     0x0790
#define REG_WMAC_OPTION_FUNCTION 0x07D0
#define REG_CPU_DMEM_CON     0x1080
#define REG_FW_DBG7          0x10FC
#define REG_CR_EXT           0x1100
#define REG_HIMR3            0x10B8
#define REG_HISR3            0x10BC
#define REG_H2C_PKT_READADDR 0x10D0
#define REG_H2C_PKT_WRITEADDR 0x10D4
#define REG_DDMA_CH0SA       0x1200
#define REG_DDMA_CH0DA       0x1204
#define REG_DDMA_CH0CTRL     0x1208
#define REG_H2CQ_CSR         0x1330
#define REG_FAST_EDCA_VOVI   0x1448
#define REG_FAST_EDCA_BEBK   0x144C
#define REG_LTECOEX_CTRL     0x1700
#define REG_LTECOEX_WDATA    0x1704
#define REG_LTECOEX_RDATA    0x1708

/* PCIe (rtw88 pci.h) */
#define RTK_PCI_CTRL         0x0300
#define TXBD_DESA_BCNQ       0x0308
#define TXBD_DESA_MGMTQ      0x0310
#define TXBD_DESA_VOQ        0x0318
#define TXBD_DESA_VIQ        0x0320
#define TXBD_DESA_BEQ        0x0328
#define TXBD_DESA_BKQ        0x0330
#define RXBD_DESA_MPDUQ      0x0338
#define TXBD_DESA_HI0Q       0x0340
#define TXBD_NUM_MGMTQ       0x0380
#define RXBD_NUM_MPDUQ       0x0382
#define TXBD_BCN_WORK        0x0383
#define TXBD_NUM_VOQ         0x0384
#define TXBD_NUM_VIQ         0x0386
#define TXBD_NUM_BEQ         0x0388
#define TXBD_NUM_BKQ         0x038A
#define TXBD_NUM_HI0Q        0x038C
#define TXBD_RWPTR_CLR       0x039C
#define TXBD_IDX_VOQ         0x03A0
#define TXBD_IDX_VIQ         0x03A4
#define TXBD_IDX_BEQ         0x03A8
#define TXBD_IDX_BKQ         0x03AC
#define TXBD_IDX_MGMTQ       0x03B0
#define RXBD_IDX_MPDUQ       0x03B4
#define TXBD_IDX_HI0Q        0x03B8
#define RPWM_ADDR            0x03D9
#define REG_MDIO_V1          0x03F4
#define REG_PCIE_MIX_CFG     0x03F8
#define TXBD_DESA_H2CQ       0x1320
#define TXBD_NUM_H2CQ        0x1328
#define TXBD_IDX_H2CQ        0x132C
#define TXBD_H2CQ_CSR        0x1330

/* interrupts */
#define IMR_ROK              0x00000001UL
#define IMR_RDU              0x00000002UL
#define IMR_C2HCMD           0x00000400UL
#define IMR1_TXFOVW          0x00000200UL
#define IMR1_RXFOVW          0x00000100UL

/* receive configuration: rtw88's hal.rcr without APP_FCS (wlan/sta.c wants frames without the FCS): APP_MIC |
 * APP_ICV | APP_PHYSTS | VHT_DACK | PKTCTL_DLEN | HTC_LOC_CTRL | AB | AM | APM; linked: + CBSSID_DATA | CBSSID_BCN */
#define R88_RCR_SCAN         0x7410400EUL
#define R88_RCR_LINKED       0x741040CEUL

/* descriptors */
#define TXD88                48               /* tx_pkt_desc_sz                                              */
#define TXBD88               16               /* tx_buf_desc_sz: two 8-byte segments                         */
#define RXD88                24               /* rx_pkt_desc_sz                                              */
#define RXBD88               8                /* rx_buf_desc_sz                                              */
#define QSEL_TID0            0
#define QSEL_BEACON          16
#define QSEL_MGMT            18
#define QSEL_H2C             19
#define RATE_1M              0x00
#define RATE_6M              0x04

/* ---- r88_io.c --------------------------------------------------------------------------------------------- */
#define rtw_r8(a)        mmio_rd8((ULONG)(USHORT)(a))
#define rtw_r16(a)       mmio_rd16((ULONG)(USHORT)(a))
#define rtw_r32(a)       mmio_rd((ULONG)(USHORT)(a))
#define rtw_w8(a, v)     mmio_wr8((ULONG)(USHORT)(a), (UCHAR)(v))
#define rtw_w16(a, v)    mmio_wr16((ULONG)(USHORT)(a), (USHORT)(v))
#define rtw_w32(a, v)    mmio_wr((ULONG)(USHORT)(a), (ULONG)(v))
void   r88_mdelay(USHORT ms);
ULONG  shl1(ULONG v);
ULONG  shr1(ULONG v);
void   w8_set(USHORT a, UCHAR b);
void   w8_clr(USHORT a, UCHAR b);
void   w16_set(USHORT a, USHORT b);
void   w16_clr(USHORT a, USHORT b);
void   w32_set(USHORT a, ULONG b);
void   w32_clr(USHORT a, ULONG b);
void   w32_mask(USHORT a, ULONG mask, ULONG data);   /* data into the bits of mask (read-modify-write)            */
ULONG  r32_mask(USHORT a, ULONG mask);
void   w8_mask(USHORT a, UCHAR mask, UCHAR data);
int    hw_ready(USHORT a, ULONG mask, ULONG target);  /* check_hw_ready: 1 = the bits reached target (10 ms)       */
ULONG  rf_read(UCHAR reg);                           /* path A, direct (0x2800 + 4 * reg), 20 bits                */
void   rf_write(UCHAR reg, ULONG data);              /* path A through SIPI (0xC90)                               */
void   rf_mask(UCHAR reg, ULONG mask, ULONG data);
int    h2c_cmd(ULONG w0, ULONG w1);                  /* H2C mailbox command; 0 = written                          */
int    h2c_pkt(UCHAR sub_id, UCHAR FAR *body, USHORT blen);   /* H2C packet (32 bytes) on the H2C ring          */
int    ltecoex_write(USHORT off, ULONG v);
int    ltecoex_read(USHORT off, ULONG FAR *v);       /* far: callers pass stack variables (SS != DS) */
extern UCHAR  g_h2c_box;
extern USHORT g_h2c_seq, g_h2c_busy;

/* ---- r88_hal.c -------------------------------------------------------------------------------------------- */
extern USHORT g_r88_step, g_pwr_fail, g_fw_err;
extern USHORT g_fw_ver, g_fw_sub, g_fw_h2cfmt;
extern ULONG  g_fw_dbg7, g_mcufw;
extern UCHAR  g_cut, g_rfe, g_pkg1, g_rfe_btg, g_xtal, g_swing2g, g_swing5g, g_thermal, g_boardopt, g_btset;
extern UCHAR  g_ef[512];
extern USHORT g_ef_ok;
extern USHORT g_rsvd_boundary;

/* ---- r88_phy.c -------------------------------------------------------------------------------------------- */
typedef struct { USHORT a; UCHAR v; } r88_mac_t;
typedef struct { USHORT a; ULONG v; } r88_bb_t;
typedef struct { USHORT a; ULONG v; } r88_rf_t;
typedef struct {
   const r88_bb_t *agc;    USHORT nagc;
   const r88_bb_t *agcbtg; USHORT nagcbtg;
   const r88_rf_t *rf;     USHORT nrf;
} r88_tblset_t;
extern const r88_mac_t    g88_mac_tbl[];
extern const USHORT       g88_mac_n;
extern const r88_bb_t     g88_bb_tbl[];
extern const USHORT       g88_bb_n;
extern const r88_tblset_t g88_tbls[];
USHORT r88_tbl_variant(UCHAR rfe, UCHAR pkg1);
extern USHORT g_tblvar;

void   r88_phy_set_param(void);                 /* BB / RF on, tables, crystal (rtw8821c_phy_set_param)       */
void   r88_set_channel(UCHAR ch);               /* 20 MHz (rtw8821c_set_channel + rtw_phy_set_tx_power_level) */
void   r88_iqk(void);                           /* firmware IQK (H2C packet), waits for RF 0x08 = 0xABCDE     */
extern UCHAR  g_channel, g_band, g_iqk_mode;
extern USHORT g_iqk_n, g_iqk_ok, g_iqk_ms;
extern UCHAR  g_txp_cck, g_txp_ofdm, g_txp_ht;
void   r88_dig_set(UCHAR v);
void   r88_dig_tick(void);
extern UCHAR  g_dig_auto, g_igi;

/* ---- r88_trx.c -------------------------------------------------------------------------------------------- */
int    r88_trx_alloc(void);
void   r88_trx_setup(void);                     /* rtw_pci_setup: ring addresses, indexes cleared, DMA reset  */
void   r88_irq_enable(void);
void   r88_irq_disable(void);
int    r88_rsvd_page(UCHAR FAR *data, USHORT len);   /* one frame through the beacon queue (reserved page)    */
UCHAR FAR *r88_rsvd_buf(void);                  /* the beacon buffer's payload (fill it, then r88_rsvd_kick)  */
int    r88_rsvd_kick(USHORT len);
int    r88_h2c_frame(UCHAR FAR *pkt, USHORT len);    /* the H2C ring                                          */
int    r88_tx_frame(UCHAR qsel, UCHAR FAR *h, USHORT hl, UCHAR FAR *iv, UCHAR FAR *b, USHORT bl, UCHAR rate,
                    USHORT flags);              /* management (QSEL_MGMT) or data (QSEL_TID0); 0 queued, -2 full */
#define TXF_BCAST    0x0001
#define TXF_SEC      0x0002
#define TXF_RPT      0x0004
#define TXF_HWSEQ    0x0008
extern USHORT g_trx_on;
extern USHORT g_rx_frames, g_rx_crc, g_rx_c2h, g_rx_big, g_rx_rdu, g_svc_runs, g_tx_ok[3], g_tx_full[3];
extern ULONG  g_isr0, g_isr1;
extern UCHAR  g_rx_rate;
USHORT r88_trx_state(USHORT what);

/* ---- r88_sta.c -------------------------------------------------------------------------------------------- */
extern UCHAR  g_curch, g_ctlrate, g_txrate, g_txrate_auto;
extern USHORT g_keys_p, g_keys_g, g_keys_dup, g_scan_n;
void   r88_sta_reset(void);
void   r88_keys_reset(void);
void   r88_link_tick(void);

#endif
