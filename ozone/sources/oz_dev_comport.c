//+++2003-11-18
//    Copyright (C) 2001,2002,2003  Mike Rieker, Beverly, MA USA
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; version 2 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//---2003-11-18

/************************************************************************/
/*									*/
/*  Communications port driver for 8250/16450/16550/16550A's		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_isa.h"
#include "oz_dev_timer.h"
#include "oz_io_comport.h"
#include "oz_io_console.h"
#include "oz_io_fs.h"
#include "oz_knl_debug.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logon.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

#define BEEPCODE 7	// character to send for beep

#define RBR 0			/* RO - receive buffer register */
#define THR 0			/* WO - transmitter holding register */
#define IER 1			/* interrupt enable register */
#define IER_DR 0x01		/* - receive interrupt enable */
#define IER_TR 0x02		/* - transmit interrupt enable */
#define IER_LS 0x04		/* - line status interrupt enable */
#define IER_MS 0x08		/* - modem status interrupt enable */
#define DLL 0			/* divisor latch (low order bits) */
#define DLH 1			/* divisor latch (high order bits) */
#define IIR 2			/* RO - interrupt identification register */
				/* - <0> : 0=int pending; 1=no int pending */
				/* - <1:2> : 0=modem status interrupt */
				/*           1=transmit interrupt */
				/*           2=receive interrupt */
				/*           3=line status interrupt */
				/* - <6:7> : set if FCR<0> is set */
#define FCR 2			/* WO - fifo control register */
#define FCR_OFF 0		/* - turn fifo off */
#define FCR_RCV_RES 0x03	/* - reset receiver fifo */
#define FCR_XMT_RES 0x05	/* - reset transmit fifo */
#define FCR_RCV_1   0x01	/* - set receive threshold */
#define FCR_RCV_4   0x41
#define FCR_RCV_8   0x81
#define FCR_RCV_14  0xC1
#define LCR 3			/* line control register */
#define LCR_M_WL 0x03		/* - word length */
#define LCR_WL_5 0x00		/*   5 bits */
#define LCR_WL_6 0x01
#define LCR_WL_7 0x02
#define LCR_WL_8 0x03
#define LCR_M_2S 0x04		/* - set for 2 stop bits (1.5 if word len 5) */
#define LCR_M_PM 0x38		/* - parity mode */
#define LCR_PM_N 0x00		/*   none */
#define LCR_PM_O 0x08		/*   odd */
#define LCR_PM_E 0x18		/*   even */
#define LCR_PM_M 0x28		/*   mark */
#define LCR_PM_S 0x38		/*   space */
#define LCR_BRK  0x40		/* transmit break */
#define LCR_DLAB 0x80		/* 0=RBR/THR/IER, 1=DLL/DLH */
#define MCR 4			/* modem control register */
#define MCR_DTR 0x01		/* - data terminal ready */
#define MCR_RTS 0x02		/* - request to send */
#define MCR_PEN 0x04		/* - enable the port */
#define MCR_IEN 0x08		/* - enable interrupts */
#define MCR_LBK 0x10		/* - enable loopback mode */
#define LSR 5			/* line status register */
#define LSR_DR 0x01		/* - receive data ready (at least 1 byte in receive fifo) */
#define LSR_OE 0x02		/* - receive overrun error */
#define LSR_PE 0x04		/* - receive parity error */
#define LSR_FE 0x08		/* - receive framing error */
#define LSR_BI 0x10		/* - receive break indicator */
#define LSR_TE 0x20		/* - transmit fifo completely empty */
#define LSR_TI 0x40		/* - transmitter idle */
#define LSR_RE 0x80		/* - at least one char in receive fifo has an error */
#define MSR 6			/* modem status register */
#define MSR_DCTS 0x01		/* - change in CTS state */
#define MSR_DDSR 0x02		/* - change in DSR state */
#define MSR_TERI 0x04		/* - RI went active */
#define MSR_DDCD 0x08		/* - change in DCD state */
#define MSR_CTS  0x10		/* - cleared to send */
#define MSR_DSR  0x20		/* - dataset is ready */
#define MSR_RI   0x40		/* - ring indicator */
#define MSR_DCD  0x80		/* - data carrier detect */
#define SCR 7			/* scratch register */

#define TXFIFOSIZE 15		/* transmitter fifo can handle 15 bytes at a time */

typedef struct { OZ_Devunit *devunit;			/* comport's devunit */
                 const char *name;			/* devunit's device name */
                 const char *devtype;			/* dev type string ("16550A", etc) */
                 OZ_Iochan *conclass_iochan;		/* the class driver I/O channel */
                 OZ_IO_comport_setup comport_setup;	/* setup parameters */
                 int suspwriteflag;			/* set to suspend writing */
                 uLong iobase;				/* comport's io base address */
                 int irqlevel;				/* comport's irq level */
                 OZ_Smplock *smplock;			/* irq level's smp lock */
                 OZ_Dev_Isa_Irq *isa_irq;		/* ISA irq block */

                 uLong writesize;			/* number of bytes remaining to be written */
                 const char *writebuff;			/* points to next character to be written */
                 void *write_param;			/* class driver's write parameter */
                 int sendbeep;				/* send a beep (bell) iff set */

                 uByte rxfifolevel;			/* FCR_RCV_1, _4, _8 or _14 */
                 uByte lcr;
                 uWord dl;				/* line speed divisor */
               } Devex;

typedef struct { int mountingchan;
                 uByte class_area[1];
               } Chnex;

typedef struct { uByte class_area[1];
               } Iopex;

static uLong comport_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int comport_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void comport_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong comport_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                            OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static OZ_Devfunc comport_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, NULL, NULL, NULL, 
                                        comport_assign, comport_deassign, comport_abort, comport_start, NULL };

static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;

static int initialized = 0;

static uLong domountvol (Devex *devex, OZ_Procmode procmode, uLong as, void *ap);
static void comport_read_start (void *devexv, int start);
static uLong comport_disp_start (void *devexv, void *write_param, uLong size, char *buff);
static void comport_disp_suspend (void *devexv, int suspend);
static void comport_kbd_rah_full (void *devexv, int full);
static void comport_terminate (void *devexv);
static uLong comport_getsetmode (void *devexv, void *getset_param, uLong size, OZ_Console_modebuff *buff);
static void uartinterrupt (void *devexv, OZ_Mchargs *mchargs);
static void startwrite (Devex *devex);
static void initdev (uLong iobase, uLong irqmask);
static const char *detect_uart (uLong iobase);
static int detect_irq (uLong iobase, uLong irqmask);
static void setdevdesc (Devex *devex, const char *classname);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_comport_init ()

{
  Devex *devex;

  if (!initialized) {
    oz_knl_printk ("oz_dev_comport_init\n");

    /* Set up driver database entries */

    devclass  = oz_knl_devclass_create (OZ_IO_COMPORT_CLASSNAME, OZ_IO_COMPORT_BASE, OZ_IO_COMPORT_MASK, "oz_dev_comport");
    devdriver = oz_knl_devdriver_create (devclass, "oz_dev_comport");

    /* Probe the common comport addresses */

    initdev (0x3F8, 0x18);
    initdev (0x2F8, 0x18);
    initdev (0x3E8, 0x18);
    initdev (0x2E8, 0x18);

    /* Probe the not-so-common comport addresses */

#if 0000
    initdev (0x3220, 0x18);
    initdev (0x3228, 0x18);
    initdev (0x4220, 0x18);
    initdev (0x4228, 0x18);
    initdev (0x5220, 0x18);
    initdev (0x5228, 0x18);
#endif

    /* Now we're initialized */

    initialized = 1;
  }
}

/************************************************************************/
/*									*/
/*  We just pass all the functab calls directly to the class driver	*/
/*									*/
/*  Class drivers do not have clonecre/clonedel.  They use 		*/
/*  OZ_IO_COMPORT_SETUP in place of clonecre, and deassigning that 	*/
/*  channel takes the place of clonedel.				*/
/*									*/
/************************************************************************/

/* A channel is being assigned to the device */

static uLong comport_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Devex *devex;

  devex = devexv;
  ((Chnex *)chnexv) -> mountingchan = 0;
  if (devex -> comport_setup.class_functab == NULL) return (OZ_SUCCESS);
  return ((*(devex -> comport_setup.class_functab -> assign)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area, procmode));
}

/* A channel is being deassigned from the device */

static int comport_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Devex *devex;

  devex = devexv;
  if ((((Chnex *)chnexv) -> mountingchan) || (devex -> comport_setup.class_functab == NULL)) return (0);
  return ((*(devex -> comport_setup.class_functab -> deassign)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area));
}

/* Abort an I/O function */

static void comport_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Devex *devex;

  devex = devexv;
  (*(devex -> comport_setup.class_functab -> abort)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area, ioop, ((Iopex *)iopexv) -> class_area, procmode);
}

/* Start an I/O function */

static uLong comport_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                                OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Devex *devex;
  uLong sts;

  devex = devexv;

  switch (funcode) {

    /* Mountvol is used to establish connection with class driver (console, mouse, ppp, etc.) and set line parameters */

    case OZ_IO_FS_MOUNTVOL: {
      sts = domountvol (devex, procmode, as, ap);
      if (sts == OZ_SUCCESS) ((Chnex *)chnexv) -> mountingchan = 1;
      return (sts);
    }

    /* Dismount turns port offline */

    case OZ_IO_FS_DISMOUNT: {
      OZ_Iochan *conclass_iochan;

      oz_dev_isa_outb (0, devex -> iobase + IER);		/* clear interrupt enables */
      conclass_iochan = devex -> conclass_iochan;		/* get class driver channel */
      devex -> conclass_iochan = NULL;
      if (conclass_iochan == NULL) return (OZ_NOTMOUNTED);	/* error if none set up */
      setdevdesc (devex, "<dismounted>");
      oz_knl_iochan_increfc (conclass_iochan, -1);		/* ok, close the channel */
      return (OZ_SUCCESS);					/* successful */
    }

    /* Set port parameters */

#if 0000
    case OZ_IO_COMPORT_SETPARAM: {
      OZ_IO_comport_setparam comport_setparam;

      movc4 (as, ap, sizeof comport_setparam, &comport_setparam);
      if (comport_setparam.baudrate != 0) devex -> baudrate = comport_setparam.baudrate;
      if (comport_setparam.databits != 0) devex -> databits = comport_setparam.databits;
      if (comport_setparam.parity   != 0) devex -> parity   = comport_setparam.parity;
      if (comport_setparam.stopbits != 0) devex -> stopbits = comport_setparam.stopbits;
      ?? set the line ??
      return (OZ_SUCCESS);
    }
#endif
  }

  /* All others pass through to class driver.  If none is set up, return 'device offline' status. */

  if (devex -> conclass_iochan == NULL) return (OZ_DEVOFFLINE);
  return ((*(devex -> comport_setup.class_functab -> start)) (devunit, devex -> comport_setup.class_devex, iochan, 
                                                              ((Chnex *)chnexv) -> class_area, procmode, ioop, 
                                                              ((Iopex *)iopexv) -> class_area, funcode, as, ap));
}

/************************************************************************/
/*									*/
/*  Establish link to class driver and set line parameters		*/
/*									*/
/*	MOUNT "<class_driver> 						*/
/*		[-baud <baud_rate>] 					*/
/*		[-databits 5/6/7/8] 					*/
/*		[-parity none/even/odd/mark/space] 			*/
/*		[-stopbits 1/1.5/2]" 					*/
/*			<comport>					*/
/*									*/
/************************************************************************/

static uLong domountvol (Devex *devex, OZ_Procmode procmode, uLong as, void *ap)

{
  char c, classname[OZ_DEVUNIT_NAMESIZE+64], *p;
  int i, usedup;
  OZ_Iochan *conclass_iochan;
  OZ_IO_fs_mountvol fs_mountvol;
  uLong baudrate, parity, stopbits, sts, wordlen;

  movc4 (as, ap, sizeof fs_mountvol, &fs_mountvol);

  sts = oz_knl_section_ugetz (procmode, sizeof classname, fs_mountvol.devname, classname, NULL);
  if (sts != OZ_SUCCESS) return (sts);

  baudrate = 9600;							// default 9600 baud
  parity   = LCR_PM_N;							// default no parity
  stopbits = 0;								// default 1 stopbit
  wordlen  = LCR_WL_8;							// default 8 databits

  for (p = classname; *p > ' '; p ++) {}				// skip over class driver name
  if (*p != 0) {							// see if any parameters given
    *(p ++) = 0;							// if so, null terminate class driver name
    while (1) {								// repeat while there are more parameters to do
      while ((*p != 0) && (*p <= ' ')) p ++;				// skip leading spaces
      if (*p == 0) break;						// stop scanning if no more parameters
      if ((strncasecmp (p, "-baud", 5) == 0) && (p[5] <= ' ')) {	// check for '-baud'
        p += 6;
        while ((*p != 0) && (*p <= ' ')) p ++;				// skip trailing spaces
        baudrate = oz_hw_atoi (p, &usedup);				// decode the given number
        if (p[usedup] > ' ') goto badbaudrate;				// make sure it terminated properly
        if (baudrate < 2) goto badbaudrate;				// can't fit these in 16-bit divisor
        i  = 115200 / baudrate;						// get what the divisor would be
        i *= baudrate;							// re-derive baud rate
        if ((i < 109440) || (i > 120960)) goto badbaudrate;		// this should be within 5% or we can't do it
        p += usedup;							// ok, on to next parameter
        continue;
      }
      if ((strncasecmp (p, "-databits", 9) == 0) && (p[9] <= ' ')) {	// check for '-databits'
        p += 10;
        while ((*p != 0) && (*p <= ' ')) p ++;				// skip trailing spaces
        wordlen = 0xFF;							// assume it won't decode
        if ((p[0] == '5') && (p[1] <= ' ')) wordlen = LCR_WL_5;
        if ((p[0] == '6') && (p[1] <= ' ')) wordlen = LCR_WL_6;
        if ((p[0] == '7') && (p[1] <= ' ')) wordlen = LCR_WL_7;
        if ((p[0] == '8') && (p[1] <= ' ')) wordlen = LCR_WL_8;
        if (wordlen == 0xFF) goto baddatabits;				// bad if it didn't decode
        while (*p > ' ') p ++;						// ok, on to next parameter
        continue;
      }
      if ((strncasecmp (p, "-parity", 7) == 0) && (p[7] <= ' ')) {	// check for '-parity'
        p += 8;
        while ((*p != 0) && (*p <= ' ')) p ++;				// skip trailing spaces
        parity = 0xFF;							// assume it won't decode
        if ((strncasecmp (p, "none",  4) == 0) && (p[4] <= ' ')) parity = LCR_PM_N;
        if ((strncasecmp (p, "odd",   3) == 0) && (p[3] <= ' ')) parity = LCR_PM_O;
        if ((strncasecmp (p, "even",  4) == 0) && (p[4] <= ' ')) parity = LCR_PM_E;
        if ((strncasecmp (p, "mark",  4) == 0) && (p[4] <= ' ')) parity = LCR_PM_M;
        if ((strncasecmp (p, "space", 5) == 0) && (p[5] <= ' ')) parity = LCR_PM_S;
        if (parity == 0xFF) goto badparity;				// bad if it didn't decode
        while (*p > ' ') p ++;						// ok, on to next parameter
        continue;
      }
      if ((strncasecmp (p, "-stopbits", 9) == 0) && (p[9] <= ' ')) {	// check for '-stopbits'
        p += 10;
        while ((*p != 0) && (*p <= ' ')) p ++;				// skip trailing spaces
        stopbits = 0xFF;						// assume it won't decode
        if ((p[0] == '1') && (p[1] <= ' ')) stopbits = 0;
        if ((wordlen == LCR_WL_5) && (p[0] == '1') && (p[1] == '.') && (p[2] == '5') && (p[3] <= ' ')) stopbits = LCR_M_2S;
        if ((wordlen != LCR_WL_5) && (p[0] == '2') && (p[1] <= ' ')) stopbits = LCR_M_2S;
        if (stopbits == 0xFF) goto badstopbits;				// bad if it didn't decode
        while (*p > ' ') p ++;						// ok, on to next parameter
        continue;
      }
    }
  }

  /* Send port parameters to class driver, get class driver parameters back */

  sts = oz_knl_iochan_crbynm (classname, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &conclass_iochan);
  if (sts != OZ_SUCCESS) return (sts);

  sts = oz_knl_io (conclass_iochan, OZ_IO_COMPORT_SETUP, sizeof devex -> comport_setup, &(devex -> comport_setup));
  if (sts != OZ_SUCCESS) {
    oz_knl_iochan_increfc (conclass_iochan, -1);
    return (sts);
  }

  if (comport_functable.chn_exsize < sizeof (Chnex) + devex -> comport_setup.class_functab -> chn_exsize) {
    comport_functable.chn_exsize = sizeof (Chnex) + devex -> comport_setup.class_functab -> chn_exsize;
  }
  if (comport_functable.iop_exsize < sizeof (Iopex) + devex -> comport_setup.class_functab -> iop_exsize) {
    comport_functable.iop_exsize = sizeof (Iopex) + devex -> comport_setup.class_functab -> iop_exsize;
  }

  devex -> conclass_iochan = conclass_iochan;

  /* Set up port parameters */

  devex -> lcr = parity | stopbits | wordlen;					/* lcr is combo of these three params */
  devex -> dl  = 115200 / baudrate;						/* set up the baud rate in here */
  devex -> rxfifolevel = FCR_RCV_1;

  /* Initialize device and enable interrupts */

  oz_dev_isa_outb (devex -> lcr | LCR_DLAB, devex -> iobase + LCR);		/* set line params, select DLH/DLL registers */
  oz_dev_isa_outb (devex -> dl, devex -> iobase + DLL);				/* program divisor latch (line speed) */
  oz_dev_isa_outb (devex -> dl >> 8, devex -> iobase + DLH);
  oz_dev_isa_outb (devex -> lcr, devex -> iobase + LCR);			/* select RBR/THR registers */
  oz_dev_isa_outb (0, devex -> iobase + MCR);					/* make sure loopback is clear */
  oz_dev_isa_outb (FCR_RCV_RES | FCR_XMT_RES | devex -> rxfifolevel, devex -> iobase + FCR); /* set receive fifo threshold = 1 char */
  oz_dev_isa_outb (IER_DR | IER_TR | IER_LS | IER_MS, devex -> iobase + IER);	/* set interrupt enables */

  setdevdesc (devex, classname);
  return (OZ_SUCCESS);

badbaudrate:
  oz_knl_printk ("oz_dev_comport: bad baud rate %s\n", p);
  return (OZ_BADPARAM);
baddatabits:
  oz_knl_printk ("oz_dev_comport: bad data bits %s\n", p);
  return (OZ_BADPARAM);
badparity:
  oz_knl_printk ("oz_dev_comport: bad parity %s\n", p);
  return (OZ_BADPARAM);
badstopbits:
  oz_knl_printk ("oz_dev_comport: bad stop bits %s\n", p);
  return (OZ_BADPARAM);
}

/************************************************************************/
/*									*/
/*  This routine is called by the class driver when it is starting or 	*/
/*  finishing a read request						*/
/*									*/
/************************************************************************/

static void comport_read_start (void *devexv, int start)

{ }

/************************************************************************/
/*									*/
/*  This routine is called by the class driver when it wants to 	*/
/*  display something.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = size of message to display				*/
/*	buff = address of message to display				*/
/*									*/
/*    Output:								*/
/*									*/
/*	comport_disp_start = OZ_SUCCESS : completed synchronously	*/
/*	                   OZ_QUEUEFULL : can't accept new request	*/
/*									*/
/************************************************************************/

static uLong comport_disp_start (void *devexv, void *write_param, uLong size, char *buff)

{
  Devex *devex;

  devex = devexv;
  if (devex -> suspwriteflag) return (OZ_QUEUEFULL);		/* if we're suspended, don't accept anything */
  if (devex -> writesize != 0) return (OZ_QUEUEFULL);		/* if we're already busy, don't accept anything */
  if (size == 0) return (OZ_SUCCESS);				/* null buffer = instant success */
  devex -> writesize   = size;					/* get the size of stuff to write */
  devex -> writebuff   = buff;					/* get pointer to stuff to write */
  devex -> write_param = write_param;
  startwrite (devex);						/* start writing it */
  return (OZ_STARTED);						/* we will call back when write is complete */
}

/************************************************************************/
/*									*/
/*  The class driver calls this routine when it wants us to stop 	*/
/*  displaying whatever it has told us to display, or when it wants us 	*/
/*  to resume.								*/
/*									*/
/************************************************************************/

static void comport_disp_suspend (void *devexv, int suspend)

{
  Devex *devex;

  devex = devexv;
  devex -> suspwriteflag = suspend;					/* set new value for the flag */
  if (!suspend && (devex -> writesize != 0)) startwrite (devex);	/* maybe restart writing */
}

/************************************************************************/
/*									*/
/*  The class driver calls this routine when its read-ahead buffer is 	*/
/*  full.								*/
/*									*/
/*    Input:								*/
/*									*/
/*	full = 0 : read-ahead buffer is no longer full			*/
/*	       1 : read-ahead buffer is full				*/
/*									*/
/*  Does a beep if full=1.						*/
/*									*/
/************************************************************************/

static void comport_kbd_rah_full (void *devexv, int full)

{
  if (full) {
    ((Devex *)devexv) -> sendbeep = 1;
    startwrite (devexv);
  }
}

/************************************************************************/
/*									*/
/*  The class driver calls this when all channels have been deassigned 	*/
/*  from the device.  We don't try to clean up, just leave stuff as is.	*/
/*									*/
/************************************************************************/

static void comport_terminate (void *devexv)

{}

/************************************************************************/
/*									*/
/*  Get / Set modes							*/
/*									*/
/************************************************************************/

static uLong comport_getsetmode (void *devexv, void *getset_param, uLong size, OZ_Console_modebuff *buff)

{
  Devex *devex;
  OZ_Console_modebuff modebuff;

  devex = devexv;
  movc4 (size, buff, sizeof modebuff, &modebuff);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  This routine processes uart interrupts				*/
/*									*/
/************************************************************************/

static void uartinterrupt (void *devexv, OZ_Mchargs *mchargs)

{
  Devex *devex;
  uByte iir;
  uLong iobase;

  devex  = devexv;
  iobase = devex -> iobase;

  iir = oz_dev_isa_inb (iobase + IIR);
  switch (iir & 0x07) {

    /* Modem status change */

    case 0: {
      uByte msr;

      msr = oz_dev_isa_inb (iobase + MSR);
      if (msr & MSR_DCTS) oz_knl_printk ("oz_dev_comport: %s CTS %s\n", devex -> name, (msr & MSR_CTS) ? "active" : "dropped");
      if (msr & MSR_DDSR) oz_knl_printk ("oz_dev_comport: %s DSR %s\n", devex -> name, (msr & MSR_DSR) ? "active" : "dropped");
      if (msr & MSR_TERI) oz_knl_printk ("oz_dev_comport: %s RI went active\n", devex -> name);
      if (msr & MSR_DDCD) oz_knl_printk ("oz_dev_comport: %s DCD %s\n", devex -> name, (msr & MSR_DCD) ? "active" : "dropped");
      break;
    }

    /* Transmitter ready - transmit up to txfifosize characters from what's waiting to transmit */

    case 2: {
      startwrite (devex);
      break;
    }

    /* Receiver ready */

    case 4: {
      uByte lsr, nchars, newfifolevel, rbr;

      /* Process whatever we can get out of the receive fifo */

      nchars = 0;
      while ((lsr = oz_dev_isa_inb (iobase + LSR)) & (LSR_DR | LSR_OE | LSR_PE | LSR_FE | LSR_BI)) {
        if (lsr & LSR_DR) {
          rbr = oz_dev_isa_inb (iobase + RBR);
          (*(devex -> comport_setup.class_kbd_char)) (devex -> comport_setup.class_param, rbr);
          nchars ++;
        }
      }

      /* If fifo timed out, set new fifo level based on how many chars were actually received */

      newfifolevel = devex -> rxfifolevel;

      if (iir & 0x08) {
        if (nchars >= 14) newfifolevel = FCR_RCV_14;
        else if (nchars >= 8) newfifolevel = FCR_RCV_8;
        else if (nchars >= 4) newfifolevel = FCR_RCV_4;
        else newfifolevel = 1;
      }

      /* Otherwise, increase fifo level by one notch */

      else if (newfifolevel < FCR_RCV_14) newfifolevel += FCR_RCV_8 - FCR_RCV_4;

      /* If fifo level changes, output new value to chip */

      if (newfifolevel != devex -> rxfifolevel) {
        devex -> rxfifolevel = newfifolevel;
        oz_dev_isa_outb (newfifolevel, iobase + FCR);
      }

      break;
    }

    /* Line status change */

    case 6: {
      uByte lsr;

      lsr = oz_dev_isa_inb (iobase + LSR);
      oz_knl_printk ("oz_dev_comport: %s LSR 0x%2.2X\n", lsr);
      break;
    }

    /* Low bit set, no interrupt pending */
  }
}

/* Start writing data indicated by devex -> writesize/buff.  Call completion routine when done. */

static void startwrite (Devex *devex)

{
  int i;
  uByte lsr;
  uLong iobase;

  iobase = devex -> iobase;

  while (devex -> sendbeep || ((devex -> writesize > 0) && !(devex -> suspwriteflag))) {
    lsr = oz_dev_isa_inb (iobase + LSR);				// check line status register
    if (!(lsr & LSR_TE)) break;						// exit loop if transmitter bussy
    if (devex -> sendbeep) {						// see if we need to send a beep
      oz_dev_isa_outb (BEEPCODE, iobase + THR);				// if so, send it
      devex -> sendbeep = 0;						// ... and don't send another
      continue;
    }
    i = devex -> writesize;						// no beep, see how much data to send
    if (i > TXFIFOSIZE) i = TXFIFOSIZE;					// not more than the fifo can handle
#ifdef OZ_HW_TYPE_486
    oz_dev_isa_outsb (i, devex -> writebuff, iobase + THR);		// fill the fifo
#else
    {
      int j;

      for (j = 0; j < i; j ++) oz_dev_isa_outb (devex -> writebuff[i], iobase + THR);
    }
#endif
    devex -> writesize -= i;						// that much less to do
    devex -> writebuff += i;
    if (devex -> writesize == 0) {					// maybe tell class driver we're all done
      (*(devex -> comport_setup.class_displayed)) (devex -> comport_setup.class_param, devex -> write_param, OZ_SUCCESS);
    }
  }
}

/************************************************************************/
/*									*/
/*  Set up new device							*/
/*									*/
/*    Input:								*/
/*									*/
/*	iobase   = base I/O address of the ports to check for		*/
/*	irqmask  = mask of the possible irq's				*/
/*	smplevel = softint						*/
/*									*/
/************************************************************************/

static void initdev (uLong iobase, uLong irqmask)

{
  char unitname[24];
  const char *devtype;
  Devex *devex;
  int irqlevel;
  OZ_Devunit *devunit;

  /* See if there is an uart at the supplied iobase address */

  devtype = detect_uart (iobase);
  if (devtype == NULL) return;

  /* See what IRQ it is at */

  irqlevel = detect_irq (iobase, irqmask);
  if (irqlevel < 0) {
    oz_knl_printk ("oz_dev_comport_init: unable to determine irq of %s at iobase 0x%X irqmask 0x%X\n", devtype, iobase, irqmask);
    return;
  }

  oz_knl_printk ("oz_dev_comport_init: found %s at iobase 0x%X, irq %d\n", devtype, iobase, irqlevel);

  /* Create the devunit */

  oz_sys_sprintf (sizeof unitname, unitname, "comport.%X", iobase);
  devunit = oz_knl_devunit_create (devdriver, unitname, unitname, &comport_functable, 0, oz_s_secattr_sysdev);
  devex   = oz_knl_devunit_ex (devunit);

  /* Set up context block */

  memset (devex, 0, sizeof *devex);

  devex -> devunit  = devunit;
  devex -> name     = oz_knl_devunit_devname (devunit);
  devex -> devtype  = devtype;
  devex -> iobase   = iobase;
  devex -> irqlevel = irqlevel;
  devex -> isa_irq  = oz_dev_isa_irq_alloc (irqlevel, uartinterrupt, devex);
  devex -> smplock  = oz_dev_isa_irq_smplock (devex -> isa_irq);

  setdevdesc (devex, "<dismounted>");

  devex -> comport_setup.port_devunit      = devunit;
  devex -> comport_setup.port_param        = devex;
  devex -> comport_setup.port_read_start   = comport_read_start;
  devex -> comport_setup.port_disp_start   = comport_disp_start;
  devex -> comport_setup.port_disp_suspend = comport_disp_suspend;
  devex -> comport_setup.port_kbd_rah_full = comport_kbd_rah_full;
  devex -> comport_setup.port_terminate    = comport_terminate;
  devex -> comport_setup.port_getsetmode   = comport_getsetmode;
  devex -> comport_setup.port_lkprm        = devex -> smplock;
  devex -> comport_setup.port_lockdb       = (void *)oz_hw_smplock_wait;
  devex -> comport_setup.port_unlkdb       = (void *)oz_hw_smplock_clr;
}

/* Detect an UART at the given address                               */
/* Return a string for the chip name, or NULL if can't figure it out */

static const char *detect_uart (uLong iobase)

{
  uByte olddata;

  olddata = oz_dev_isa_inb (iobase + MCR);
  oz_dev_isa_outb (0x10, iobase + MCR);
  if ((oz_dev_isa_inb (iobase + MSR) & 0xF0) != 0) return (NULL);
  oz_dev_isa_outb (0x1F, iobase + MCR);
  if ((oz_dev_isa_inb (iobase + MSR) & 0xF0) != 0xF0) return (NULL);
  oz_dev_isa_outb (olddata, iobase + MCR);

  /* Look for the scratch register */

  olddata = oz_dev_isa_inb (iobase + SCR);
  oz_dev_isa_outb (0x55, iobase + SCR);
  if (oz_dev_isa_inb (iobase + SCR) != 0x55) return ("8250");
  oz_dev_isa_outb (0xAA, iobase + SCR);
  if (oz_dev_isa_inb (iobase + SCR) != 0xAA) return ("8250");
  oz_dev_isa_outb (olddata, iobase + SCR);

  /* Check to see if there's a FIFO */

  oz_dev_isa_outb (1, iobase + FCR);			/* enable the fifo */
  olddata = oz_dev_isa_inb (iobase + IIR);		/* read the interrupt ident register */
  oz_dev_isa_outb (0, iobase + FCR);			/* disable the fifo */
  if (!(olddata & 0x80)) return ("16450");		/* check for type of fifo */
  if (!(olddata & 0x40)) return ("16550");
  return ("16550A");
}

/* Determine the IRQ the particular UART is connected to */
/* Return either the IRQ number or -1 if can't do it     */

static void detect_irq_hit (void *irqflagv, OZ_Mchargs *mchargs);

static int detect_irq (uLong iobase, uLong irqmask)

{
  char irqcopy[16], irqflags[16];
  int hi, i, retries;
  OZ_Dev_Isa_Irq *irqblocks[16];

  /* Set up to capture all 'irqmask' IRQ's */

  memset (irqblocks, 0, sizeof irqblocks);
  for (i = 16; -- i >= 0;) {
    if (irqmask & (1 << i)) irqblocks[i] = oz_dev_isa_irq_alloc (i, detect_irq_hit, irqflags + i);
  }

  /* Try up to 5 times to capture the interrupt */

  for (retries = 5; -- retries > 0;) {
    oz_dev_isa_outb (0, iobase + IER);			/* disable all uart interrupts */
    while (!(oz_dev_isa_inb (iobase + LSR) & 0x20)) {}	/* wait for the THR to be empty */
    oz_dev_isa_outb (0x0F, iobase + MCR);		/* connect uart to irq line */
    memset (irqflags, 0, sizeof irqflags);		/* clear the flags */
    oz_dev_isa_outb (2, iobase + IER);			/* enable uart to request interrupt */
    oz_hw_stl_microwait (500, NULL, NULL);		/* wait 500uS for an interrupt */
    memcpy (irqcopy, irqflags, sizeof irqcopy);		/* copy the flags so more can't set on us */
    oz_dev_isa_outb (0, iobase + IER);			/* turn off uart interrupts */
    for (hi = 16; -- hi >= 0;) if (irqcopy[hi]) break;	/* loop through all 16 bits of the mask */
    if (hi >= 0) {
      for (i = hi; -- i >= 0;) if (irqcopy[i]) break;
      if (i < 0) break;
      hi = -1;
    }
  }

  /* Disconnect the interrupts */

  for (i = 16; -- i >= 0;) {
    if (irqblocks[i] != NULL) oz_dev_isa_irq_free (irqblocks[i]);
  }

  /* Return whether or not we were successful */

  return (hi);
}

static void detect_irq_hit (void *irqflagv, OZ_Mchargs *mchargs)

{
  *(char *)irqflagv = 1;
}

/* Setup device description string */

static void setdevdesc (Devex *devex, const char *classname)

{
  char unitdesc[64];

  oz_sys_sprintf (sizeof unitdesc, unitdesc, "%s iobase %X, irq %d : %s", 
	devex -> devtype, devex -> iobase, devex -> irqlevel, classname);
  oz_knl_devunit_rename (devex -> devunit, NULL, unitdesc);
}
