/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * strat.c - strategy routine, INIT and the clock of the RTL8821AE / RTL8821CE OS/2 drivers (from qc_strat.c of the QCA6174 driver).
 *
 * Adapted from rt_strat.c of the OS2World RTL88EU driver (GPL-2.0-only), written from the IBM Physical
 * Device Driver Reference (strategy commands, request packets, INIT, INIT COMPLETE, Save_Message).
 *
 *   INIT          - DevHelp entry, switches, resident end, DMA memory and selectors, find the PCI function,
 *                   register the NDIS MAC with the Protocol Manager.
 *   OPEN / CLOSE  - start / finish a firmware upload or a debug session (dd_dbg.c).
 *   WRITE         - firmware upload ("copy /b RTL8821AE.FW \DEV\WRND32$", RTL8821CE.FW) or a debug command line.
 *   READ          - the driver status as KEY=VALUE text ("copy \DEV\WRND32$ con").
 *   GENIOCTL      - the GenMAC OID interface (dd_nic.c); the chip is brought up on the first request.
 */
#include "drv.h"
#include "ndis.h"

#define MSG_REPLACEMENT_STRING  1178
static struct {
   USHORT id;
   USHORT count;
   PSZ    text;
} gInitMsg = { MSG_REPLACEMENT_STRING, 1, 0 };
static USHORT g_opt_v, g_opt_q, g_opt_w, g_opt_bad, g_opt_b, g_opt_stop;

/* Boot diagnostics for a machine without a serial port. With /B the driver beeps the number of the step it has
 * reached (n short beeps, then a pause), so a hang can be located by counting the last group. /Sn finishes INIT
 * successfully after step n without doing the rest, to find the step that makes the boot hang.
 *   1 arguments read          4 BAR0 windows mapped            8 INIT COMPLETE entered
 *   2 DMA memory and selectors 5 NDIS MAC registered           9 INIT COMPLETE done
 *   3 PCI scan                6 PROTOCOL.INI read; 7 INIT done */
void dd_step(USHORT n)
{
   USHORT i;

   tcom_always('['); tcom_always('S'); tcom_always((char)('0' + n / 10)); tcom_always((char)('0' + n % 10));
   tcom_always(']');
   if (!g_opt_b)
      return;
   if (n >= 10) {                                    /* sub-steps: one LONG beep, then (n - 10) short ones      */
      dd_speaker(1);
      for (i = 0; i < 6; i++)
         dd_iodelay(60000);
      dd_speaker(0);
      dd_iodelay(60000);
      n = (USHORT)(n - 10);
   }
   for (i = 0; i < n; i++) {
      dd_speaker(1);
      dd_iodelay(50000);
      dd_speaker(0);
      dd_iodelay(50000);
   }
   for (i = 0; i < 6; i++)                          /* pause between groups                                    */
      dd_iodelay(60000);
}

#define STOP_HERE(n)  (g_opt_stop == (n))

static void say(PSZ s)
{
   PSZ p;

   if (g_opt_q)
      return;
   gInitMsg.text = s;
   rt_save_message((void *)&gInitMsg);
   for (p = s; *p; p++)
      tcom_always(*p);
   tcom_always(13); tcom_always(10);
}

/* "name 10EC:C821 bus 03 dev 00 fn 00 IRQ 11 BAR 7D00000": what INIT found, shown at boot WITHOUT /V so that a user
 * whose adapter does not appear can tell whether INIT ran and where it stopped (/Q silences it). */
static char g_foundmsg[96];

static char *put_hex(char *d, ULONG v, USHORT digits)
{
   static const char hx[] = "0123456789ABCDEF";
   U32    u;
   USHORT i, sh, n;

   u.l = v;
   for (i = digits; i > 0; i--) {
      sh = (USHORT)((i - 1) * 4);
      n = (USHORT)((sh >= 16 ? (u.w[1] >> (sh - 16)) : (u.w[0] >> sh)) & 15);
      *d++ = hx[n];
   }
   return d;
}

static char *put_str(char *d, const char *s)
{
   while (*s)
      *d++ = *s++;
   return d;
}

static void say_found(void)
{
   char *d = g_foundmsg;

   d = put_str(d, DRV_NAME ": found ");
   d = put_hex(d, g_pci_ven, 4);
   *d++ = ':';
   d = put_hex(d, g_pci_dev, 4);
   d = put_str(d, " bus ");
   d = put_hex(d, (USHORT)(g_pci_bdf >> 8), 2);
   d = put_str(d, " dev ");
   d = put_hex(d, (USHORT)((g_pci_bdf >> 3) & 0x1F), 2);
   d = put_str(d, " fn ");
   d = put_hex(d, (USHORT)(g_pci_bdf & 7), 1);
   d = put_str(d, " IRQ ");
   d = put_hex(d, g_pci_irq, 2);
   d = put_str(d, " registers at ");
   d = put_hex(d, g_bar0, 8);
   *d = 0;
   say((PSZ)g_foundmsg);
}

/* ---- clock --------------------------------------------------------------------------------------- */
static ULONG FAR *g_msp;

ULONG dd_ms(void)
{
   PVOID pv;
   union { PVOID p; USHORT w[2]; } u;

   if (!g_msp) {
      static PVOID spv;

      if (rt_dh_getdosvar(DOSVAR_SYSINFOSEG, 0, (PPVOID)&spv))
         return 0;
      pv = spv;
      u.p = pv;
      u.w[0] = 4;                                 /* msecs in the global information segment             */
      u.w[1] = *(USHORT FAR *)pv;                 /* GetDOSVar returns a pointer to the selector         */
      g_msp = (ULONG FAR *)u.p;
   }
   return *g_msp;
}

#define EVT_SLEEP 0x51434101UL

void dd_msleep(USHORT ms)
{
   rt_dh_procblock(EVT_SLEEP, (ULONG)ms, WAIT_UNINTERRUPTIBLE);
}

/* ---- switches ------------------------------------------------------------------------------------ */
static void parse_args(PSZ p)
{
   char c;

   if (!p)
      return;
   while (*p) {
      if (*p == '/') {
         c = (char)(p[1] & 0xDF);
         if (c == 'V')      g_opt_v = 1;
         else if (c == 'Q') g_opt_q = 1;
         else if (c == 'W') g_opt_w = 1;
         else if (c == 'B') g_opt_b = 1;
         else if (c == 'S' && p[2] >= '1' && p[2] <= '9') g_opt_stop = (USHORT)(p[2] - '0');
         else if (c == 'A' && (p[2] & 0xDF) == 'A') ;
         else               g_opt_bad = 1;
      }
      p++;
   }
}

/* ---- INIT ---------------------------------------------------------------------------------------- */
static void rt_init(RP_INIT FAR *rp)
{
   USHORT nrc;

   rt_devhelp = rp->p1.DevHlp;
   tcom_always('I'); tcom_always('0');
   parse_args(rp->Args);
   rp->Data1 = 0;
   rp->p1.End.CodeEnd = rt_code_end();
   rp->p1.End.DataEnd = rt_data_end();
   dd_step(1);
   if (STOP_HERE(1)) {
      rp->rph.Status |= STDON;
      return;
   }

   if (mem_init()) {
      say(DRV_NAME ": could not be loaded: no memory for DMA buffers or selectors.");
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   tcom_always('I'); tcom_always('1');
   dd_step(2);
   if (STOP_HERE(2)) {
      rp->rph.Status |= STDON;
      return;
   }
   if (pci_find()) {
      say(DRV_NAME ": could not be loaded: no " DRV_CHIP " adapter found.");
      say((PSZ)hw_name());                       /* names a recognised device of a family not driven yet */
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   if (!g_bar0) {
      say(DRV_NAME ": could not be loaded: the adapter's registers are above 4 GB or not memory-mapped (BAR2).");
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   say_found();
   dd_step(3);
   if (STOP_HERE(3)) {
      rp->rph.Status |= STDON;
      return;
   }
   /* BAR0 is mapped at bring-up (task time), not here */
   tcom_always('I'); tcom_always('2');
   dd_step(4);
   if (STOP_HERE(4)) {
      rp->rph.Status |= STDON;
      return;
   }
   nrc = ndis_init();
   dd_step(5);
   if (STOP_HERE(5)) {
      rp->rph.Status |= STDON;
      return;
   }
   ndis_find_lan();
   tcom_always('I'); tcom_always('3');
   dd_step(6);
   if (STOP_HERE(6)) {
      rp->rph.Status |= STDON;
      return;
   }

   if (g_opt_bad)
      say(DRV_NAME ": warning - unknown option ignored (valid: /V /Q /W /B /S1../S9 /AA).");
   if (g_opt_v) {
      say(DRV_NAME ": " DRV_CHIP " PCIe WLAN driver for OS/2.");
      say((PSZ)hw_name());
      say(DRV_NAME ": Version " DRV_VERSION "  Build " DRV_BUILD "  Date " __DATE__ " " __TIME__ ".");
      say(nrc == 0 ? (PSZ)DRV_NAME ": NDIS MAC registered with the Protocol Manager."
                   : (PSZ)DRV_NAME ": NDIS MAC not registered (no [" DRV_NIF "] in PROTOCOL.INI?).");
      say(DRV_NAME ": GenMAC device WRND32$. Copy the firmware file to it at boot:");
      say((PSZ)hw_fw_name());
   }
   if (g_opt_w)
      say(DRV_NAME ": the boot pauses 30 seconds after all drivers are loaded.");
   dd_step(7);
   say(nrc == 0 ? (PSZ)DRV_NAME ": ready; copy the firmware file to \\DEV\\WRND32$ (STARTUP.CMD)."
                : (PSZ)DRV_NAME ": loaded, but the NDIS MAC is not registered (no [" DRV_NIF "] in PROTOCOL.INI, or PROTMAN$ not loaded before this driver?).");
   rp->rph.Status |= STDON;
}

static void rt_initdone(RPH FAR *rp)
{
   USHORT i;
   dd_step(8);

   if (g_opt_w && !g_opt_q)
      for (i = 0; i < 30 * 20; i++)
         dd_msleep(50);
   dd_step(9);
   rp->Status |= STDON;
}

/* ---- the strategy routine ------------------------------------------------------------------------- */
void __cdecl rt_strategy(RPH FAR *rp)
{
   rp->Status = 0;

   switch (rp->Cmd) {
   case CMD_INIT:
      rt_init((RP_INIT FAR *)rp);
      break;
   case CMD_INITCOMPLETE:
      rt_initdone(rp);
      break;
   case CMD_OPEN:
      dd_dev_open(((RP_OPENCLOSE FAR *)rp)->sfn);
      rp->Status |= STDON;
      break;
   case CMD_CLOSE:
      dd_dev_close(((RP_OPENCLOSE FAR *)rp)->sfn);
      rp->Status |= STDON;
      break;
   case CMD_WRITE: {
      RP_RW FAR *rw = (RP_RW FAR *)rp;

      dd_dev_write(rw->sfn, rw->Xfer, rw->Count);
      rp->Status |= STDON;
      break;
   }
   case CMD_READ: {
      RP_RW FAR *rw = (RP_RW FAR *)rp;

      rw->Count = dd_dev_read(rw->sfn, rw->Xfer, rw->Count);
      rp->Status |= STDON;
      break;
   }
   case CMD_GENIOCTL:
      dd_genmac_ioctl((RP_GENIOCTL FAR *)rp);
      rp->Status |= STDON;
      break;
   case CMD_SHUTDOWN:
      dd_shutdown();
      rp->Status |= STDON;
      break;
   default:
      rp->Status |= STDON;
      break;
   }
}
