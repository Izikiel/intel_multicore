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
/*  ps2mouse port driver for 486's					*/
/*									*/
/*  It handles the physical I/O to the mouse				*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_dev_console_486.h"
#include "oz_io_mouse.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

#define MOUSE_IRQ 12
#define MOUSE_BUF_SIZE 2048

/* Aux controller ports */

#define AUX_INPUT_PORT	0x60		/* Aux device output buffer */
#define AUX_OUTPUT_PORT	0x60		/* Aux device input buffer */
#define AUX_COMMAND	0x64		/* Aux device command buffer */
#define AUX_STATUS	0x64		/* Aux device status reg */

/* Commands written to AUX_COMMAND */

#define AUX_CMD_WRITE	0x60		/* value to write to controller */
					/* - the next byte written to AUX_OUTPUT_PORT is a 'command byte' */
#define AUX_DISABLE	0xa7		/* disable aux device */
#define AUX_ENABLE	0xa8		/* enable aux device */
#define AUX_MAGIC_WRITE	0xd4		/* value to send aux device data */
					/* - the next byte written to AUX_OUTPUT_PORT is transmitted to mouse */

/* Aux controller status bits                                               */
/* - bit definitions:                                                       */
/*   <0> : 0=no byte is waiting in AUX_INPUT_PORT                           */
/*         1=there is a byte waiting in AUX_INPUT_PORT to be read           */
/*   <1> : 0=it is ok to write a new byte to AUX_COMMAND or AUX_OUTPUT_PORT */
/*         1=ctrlr is busy with byte in AUX_COMMAND or AUX_OUTPUT_PORT      */
/*   <2> : 0=self test failed                                               */
/*         1=self test passed                                               */
/*   <3> : 0=last byte written was to AUX_DATA_PORT                         */
/*         1=last byte written was to AUX_COMMAND                           */
/*   <4> : 0=keyboard is disabled                                           */
/*         1=keyboard is enabled                                            */
/*   <5> : 0=byte in AUX_INPUT_PORT is from the keyboard                    */
/*         1=byte in AUX_INPUT_PORT is from the mouse                       */
/*   <6> : timeout error flag                                               */
/*   <7> : parity error flag                                                */

#define AUX_OBUF_FULL	0x21		/* output buffer (from device) full */
#define AUX_IBUF_FULL	0x02		/* input buffer (to device) full */

/* Aux controller commands - sent following AUX_CMD_WRITE */
/* - bit definitions:                                     */
/*   <0> : 0=disable keyboard interrupt                   */
/*         1=enable keyboard interrupt                    */
/*   <1> : 0=disable mouse interrupt                      */
/*         1=enable mouse interrupt                       */
/*   <2> : 0=self test failed                             */
/*         1=self test succeeded                          */
/*   <3> : 0=PC/AT inhibit override (1=enabled always)    */
/*         (must be 0 on PS/2 systems)                    */
/*   <4> : 0=no action                                    */
/*         1=disable keyboard                             */
/*   <5> : 0=no action                                    */
/*         1=disable mouse                                */
/*   <6> : pc compatibility mode                          */
/*         1=translate kbd codes to PC scan codes         */
/*   <7> : must be zero                                   */

#define AUX_INTS_ON	0x47		/* 'command byte' : enable controller mouse interrupts */
#define AUX_INTS_OFF	0x65		/* 'command byte' : disable controller mouse interrupts */

/* Aux device commands - sent following AUX_MAGIC_WRITE */

#define AUX_SET_RES	0xe8		/* set resolution */
#define AUX_SET_SCALE11	0xe6		/* set 1:1 scaling */
#define AUX_SET_SCALE21	0xe7		/* set 2:1 scaling */
#define AUX_GET_SCALE	0xe9		/* get scaling factor */
#define AUX_SET_STREAM	0xea		/* set stream mode */
#define AUX_SET_SAMPLE	0xf3		/* set sample rate */
#define AUX_ENABLE_DEV	0xf4		/* enable aux device */
#define AUX_DISABLE_DEV	0xf5		/* disable aux device */
#define AUX_RESET	0xff		/* reset aux device */

/* Iopex */

typedef struct Iopex Iopex;

struct Iopex { Iopex *next;		/* link to next in readqh/qt */
               OZ_Ioop *ioop;		/* corresponding ioop */
               OZ_Lowipl *lowipl;	/* NULL if still pending, else it has been posted via oz_knl_iodone */
               OZ_IO_mouse_read mouse_read;
             };

/* I/O dispatch table */

static uLong ps2mouse_486_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int ps2mouse_486_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void ps2mouse_486_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong ps2mouse_486_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                                OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static OZ_Devfunc ps2mouse_486_functable = { 0, 0, sizeof (Iopex), 0, NULL, NULL, NULL, ps2mouse_486_assign, 
                                             ps2mouse_486_deassign, ps2mouse_486_abort, ps2mouse_486_start, NULL };

/* Internal static data */

static OZ_Devclass  *devclass;
static OZ_Devdriver *devdriver;
static OZ_Devunit   *devunit;

static int        initialized = 0;	/* set when initialization was successful */
static int            enabled = 0;	/* number of channels assigned to device */
static Iopex          *readqh = NULL;	/* queue of read requests */
static Iopex         **readqt = &readqh;
static OZ_Lowipl      *lowipl = NULL;	/* NULL: lowipl is running; else: lowipl can be called by isr */
static OZ_Smplock *smplock_mo = NULL;	/* smplock for the irq level */

static int rahrem = 0;			/* index in rahbuf where the oldest byte is that has yet to be copied to user buffer */
static int rahins = 0;			/* index in rahbuf where new bytes from the interrupt routine will be stored */
static uByte rahbuf[MOUSE_BUF_SIZE];	/* the 'read-ahead' bytes */

static OZ_Hw486_irq_many mo_irq_many;	/* interrupt block */

/* Internal routines */

static int mouse_interrupt (void *dummy, OZ_Mchargs *mchargs);
static void lowiplroutine (void *dummy, OZ_Lowipl *li);
static void read_iodone (void *iopexv, int finok, uLong *status_r);
static int aux_write_ack (int val);
static int aux_write_cmd (int val);
static int aux_write_dev (int val);
static int poll_aux_status (void);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_ps2mouse_init ()

{
  const char *p;
  uLong kb;

  if (initialized == 0) {
    oz_knl_printk ("oz_dev_ps2mouse_486_init\n");

    /* Make sure keyboard driver gets initialized first */

    if (oz_dev_keyboard_smplock == NULL) return;

    /* Initialize my database */

    if (smplock_mo == NULL) {
      devclass   = oz_knl_devclass_create (OZ_IO_MOUSE_CLASSNAME, OZ_IO_MOUSE_BASE, OZ_IO_MOUSE_MASK, "oz_dev_ps2mouse_486");
      devdriver  = oz_knl_devdriver_create (devclass, "oz_dev_ps2mouse_486");
      devunit    = oz_knl_devunit_create (devdriver, "ps2mouse", "PS/2 mouse on 486's", &ps2mouse_486_functable, 0, oz_s_secattr_sysdev);
      lowipl     = oz_knl_lowipl_alloc ();
      mo_irq_many.entry = mouse_interrupt;
      mo_irq_many.param = NULL;
      mo_irq_many.descr = "ps2 mouse";
      smplock_mo = oz_hw486_irq_many_add (MOUSE_IRQ, &mo_irq_many);
      if (smplock_mo == NULL) {
        oz_knl_printk ("oz_dev_ps2mouse: unable to set up interrupt\n");
        initialized = -1;
        return;
      }
    }

    /* Initialize the device */

    kb = oz_hw_smplock_wait (oz_dev_keyboard_smplock);	/* keep keyboard from using ports */

#if defined INITIALIZE_DEVICE
    p = "enabling mouse interface";
    if (!poll_aux_status ()) goto timedout;			/* wait for chip ready */
    oz_hw486_outb (AUX_ENABLE, AUX_COMMAND);			/* enable mouse interface */

    p = "setting sample rate";
    if (aux_write_ack (AUX_SET_SAMPLE) < 0) goto timedout;	/* set sample rate to ... */
    if (aux_write_ack (100) < 0) goto timedout;			/* ... 100 samples/sec */

    p = "setting resolution";
    if (aux_write_ack (AUX_SET_RES) < 0) goto timedout;		/* set resolution to ... */
    if (aux_write_ack (3) < 0) goto timedout;			/* ... 8 counts per mm */

    p = "setting scaling factor";
    if (aux_write_ack (AUX_SET_SCALE21) < 0) goto timedout;	/* 2:1 scaling */
#endif

    /* Disable device (until a channel is assigned to it) */

    p = "disabling device";
    if (!poll_aux_status ()) goto timedout;			/* wait for chip ready */
    oz_hw486_outb (AUX_DISABLE, AUX_COMMAND);			/* disable mouse interface */
    p = "disabling interrupts";
    if (!aux_write_cmd (AUX_INTS_OFF)) goto timedout;		/* turn off aux interrupts */

    oz_hw_smplock_clr (oz_dev_keyboard_smplock, kb);

    /* We are now initialized */

    initialized = 1;
  }

  return;

  /* Some stage of init failed - output message and leave initialized = 0 */

timedout:
  oz_hw_smplock_clr (oz_dev_keyboard_smplock, kb);		/* release keyboard lock */
  oz_knl_printk ("oz_dev_ps2mouse assign: timed out %s\n", p);	/* output error message */
}

/************************************************************************/
/*									*/
/*  A channel is being assigned to the device				*/
/*									*/
/*  If this is the first channel, turn the device on			*/
/*									*/
/************************************************************************/

static uLong ps2mouse_486_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  const char *p;
  uLong kb;

  /* Make sure init routine succeeded, if not try it again */

  if (initialized <= 0) {
    oz_knl_printk ("oz_dev_ps2mouse assign: device failed to initialize - reattempting\n");
    oz_dev_ps2mouse_init ();
    if (initialized <= 0) {
      oz_knl_printk ("oz_dev_ps2mouse assign: still no good\n");
      return (OZ_DEVOFFLINE);
    }
  }

  /* If this is the only channel, enable the device */

  kb = oz_hw_smplock_wait (oz_dev_keyboard_smplock);		/* keep keyboard interrupt routine out during this stuff */
  if (enabled == 0) {
    p = "enabling device";
    if (!poll_aux_status ()) goto timedout;			/* wait for device idle */
    oz_hw486_outb (AUX_ENABLE, AUX_COMMAND);			/* enable aux */
    p = "enabling interface";
    if (!aux_write_dev (AUX_ENABLE_DEV)) goto timedout;		/* enable aux device */
    p = "enabling interrupts";
    if (!aux_write_cmd (AUX_INTS_ON)) goto timedout;		/* enable controller ints */
  }

  /* Either way, all done */

  enabled ++;							/* set enable count */
  oz_hw_smplock_clr (oz_dev_keyboard_smplock, kb);		/* release keyboard routine */
  return (OZ_SUCCESS);						/* successful */

  /* It timed out somewhere, return faillure status */

timedout:
  oz_hw_smplock_clr (oz_dev_keyboard_smplock, kb);		/* release keyboard lock */
  oz_knl_printk ("oz_dev_ps2mouse assign: timed out %s\n", p);	/* output error message */
  return (OZ_IOFAILED);						/* failure */
}

/************************************************************************/
/*									*/
/*  A channel is being deassigned from the device			*/
/*									*/
/*  If this is the last channel assigned to device, it is disabled	*/
/*									*/
/************************************************************************/

static int ps2mouse_486_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  uLong kb;

  kb = oz_hw_smplock_wait (oz_dev_keyboard_smplock);		/* keep keyboard interrupt routine out during this stuff */
  if (-- enabled == 0) {					/* see if this is the last channel to be deassigned */
    aux_write_cmd (AUX_INTS_OFF);				/* disable controller ints */
    poll_aux_status ();						/* make sure chip is ready */
    oz_hw486_outb (AUX_DISABLE, AUX_COMMAND);			/* disable mouse interface */
  }
  oz_hw_smplock_clr (oz_dev_keyboard_smplock, kb);		/* release lock */
  return (0);							/* successful, don't try again */
}

/************************************************************************/
/*									*/
/*  Abort an I/O function						*/
/*									*/
/************************************************************************/

static void ps2mouse_486_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Iopex *iopex, **liopex;
  uLong mo;

scan:
  mo = oz_hw_smplock_wait (smplock_mo);								/* lock queues */
  for (liopex = &readqh; (iopex = *liopex) != NULL; liopex = &(iopex -> next)) {		/* scan the queue */
    if ((iopex -> lowipl == NULL) && oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) { /* see if request is abortable */
      *liopex = iopex -> next;									/* if so, remove from queue */
      oz_hw_smplock_clr (smplock_mo, mo);							/* release lock */
      oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);				/* post the request */
      goto scan;										/* re-scan queue for more */
    }
  }
  oz_hw_smplock_clr (smplock_mo, mo);								/* nothing abortable, unlock */
}

/************************************************************************/
/*									*/
/*  Start an I/O function						*/
/*									*/
/************************************************************************/

static uLong ps2mouse_486_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                                 OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  int rlen;
  Iopex *iopex;
  uLong mo, sts;

  iopex = iopexv;
  iopex -> ioop = ioop;

  switch (funcode) {

    case OZ_IO_MOUSE_READ: {

      /* Copy and pad parameter block */

      movc4 (as, ap, sizeof iopex -> mouse_read, &(iopex -> mouse_read));

      /* Lock buffers in memory */

      sts = oz_knl_ioop_lockw (ioop, iopex -> mouse_read.size, iopex -> mouse_read.buff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, sizeof *(iopex -> mouse_read.rlen), iopex -> mouse_read.rlen, NULL, NULL, NULL);

      /* Complete synchronously if there is something in queue, else queue request */

      if (sts == OZ_SUCCESS) {
        mo = oz_hw_smplock_wait (smplock_mo);					/* block mouse interrupts */
        rlen = rahins - rahrem;							/* see how many chars are in read-ahead buffer */
        if (rlen < 0) rlen = sizeof rahbuf - rahrem;
        if ((readqh == NULL) && (rlen > 0)) {					/* see if we have anything */
          if (rlen > iopex -> mouse_read.size) rlen = iopex -> mouse_read.size;	/* if so, don't get more than caller wants */
          memcpy (iopex -> mouse_read.buff, rahbuf + rahrem, rlen);		/* copy out to caller's buffer */
          rahrem += rlen;							/* increment index */
          if (rahrem == sizeof rahbuf) rahrem = 0;				/* maybe wrap it */
          *(iopex -> mouse_read.rlen) = rlen;					/* return number of bytes we got */
        } else {
          *readqt = iopex;							/* queue is busy or no data, queue this on end */
          readqt = &(iopex -> next);
          iopex -> next = NULL;
          iopex -> lowipl = NULL;						/* it hasn't been posted yet */
          sts = OZ_STARTED;							/* tell caller request was queued */
        }
        oz_hw_smplock_clr (smplock_mo, mo);					/* unblock mouse interrupts */
      }
      break;
    }

    default: {
      sts = OZ_BADIOFUNC;
      break;
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  This routine processes mouse interrupts				*/
/*									*/
/*  It runs with the smplock_mo lock set				*/
/*									*/
/************************************************************************/

static int mouse_interrupt (void *dummy, OZ_Mchargs *mchargs)

{
  int i;
  uByte st;

						/* note: this routine is written such that the keyboard */
						/* interrupt can happen at any point - the keyboard int */
						/* routine is written such that if it sees 0x21 as the  */
						/* status it will not try to process it                 */

  while (1) {
    st = oz_hw486_inb (AUX_STATUS);		/* get status register */
    if ((st & 0x21) != 0x21) break;		/* stop if no mouse char ready */
    i = rahins;					/* if so, get index */
    rahbuf[i++] = oz_hw486_inb (AUX_INPUT_PORT);	/* store in read-ahead buffer */
    if (i == sizeof rahbuf) i = 0;		/* increment with wrap */
    if (i != rahrem) rahins = i;		/* save iff read-ahead didn't overflow */
    if ((readqh != NULL) && (lowipl != NULL)) {	/* call lowiplroutine if a read is queued */
      oz_knl_lowipl_call (lowipl, lowiplroutine, NULL);
      lowipl = NULL;
    }
  }

  return (0);
}

/************************************************************************/
/*									*/
/*  This routine gets called at softint level when there are bytes in 	*/
/*  the read-ahead buffer and there are read requests in the queue	*/
/*									*/
/************************************************************************/

static void lowiplroutine (void *dummy, OZ_Lowipl *li)

{
  Iopex *iopex;
  uLong mo;

  mo = oz_hw_smplock_wait (smplock_mo);				/* lock access to request queue */
  if (((iopex = readqh) == NULL) || (rahins == rahrem)) {	/* see if any request and any data for it */
    lowipl = li;						/* if not, just re-enable the lowipl routine */
    iopex  = NULL;						/* ... and don't post request */
  } else {
    iopex -> lowipl = li;					/* if so, keep abort route off of it */
  }
  oz_hw_smplock_clr (smplock_mo, mo);				/* release lock */
  if (iopex != NULL) {						/* see if there was something there to post */
    oz_knl_iodone (iopex -> ioop, OZ_SUCCESS, NULL, read_iodone, iopex); /* if so, post it */
  }
}

/************************************************************************/
/*									*/
/*  We are back in the original process of the top request on the 	*/
/*  queue.								*/
/*									*/
/************************************************************************/

static void read_iodone (void *iopexv, int finok, uLong *status_r)

{
  int rlen;
  Iopex *iopex;
  uLong mo;

  iopex = iopexv;

  /* Copy data and return length back to caller's buffer */

  mo = oz_hw_smplock_wait (smplock_mo);					/* block mouse interrupts */
  if (finok) {								/* make sure we are in request's process */
    rlen = rahins - rahrem;						/* see how many chars are in read-ahead buffer */
    if (rlen < 0) rlen = sizeof rahbuf - rahrem;			/* if wrap, just get number of contig chars at end */
    if (rlen > 0) {							/* see if we have anything */
      if (rlen > iopex -> mouse_read.size) rlen = iopex -> mouse_read.size; /* if so, don't get more than caller wants */
      memcpy (iopex -> mouse_read.buff, rahbuf + rahrem, rlen);		/* copy out to caller's buffer */
      rahrem += rlen;							/* increment index */
      if (rahrem == sizeof rahbuf) rahrem = 0;				/* maybe wrap it */
    }
    *(iopex -> mouse_read.rlen) = rlen;					/* return number of bytes we got */
  }

  /* Unlink request from queue */

  if (iopex != readqh) oz_crash ("oz_dev_ps2mouse read_iodone: request not on top of queue");
  readqh = iopex -> next;						/* unlink request from queue */
  if (readqh == NULL) readqt = &readqh;
  oz_hw_smplock_clr (smplock_mo, mo);					/* unblock mouse interrupts */

  /* Maybe another request can be processed now that we removed the last one's data           */
  /* If not, the lowipl pointer gets put back in the global location so the isr can see it    */
  /* Else, the lowipl pointer gets put in the new top request and the top request gets posted */

  lowiplroutine (NULL, iopex -> lowipl);
}

/************************************************************************/
/*									*/
/*  Write to device & handle returned ack				*/
/*									*/
/*    Output:								*/
/*									*/
/*	aux_write_ack = < 0 : failure					*/
/*	               else : ack code					*/
/*									*/
/************************************************************************/

#if defined INITIALIZE_DEVICE
static int aux_write_ack (int val)

{
  int retries;
  uByte db, st;

  if (!aux_write_cmd (val)) return (-1);	/* write command code */
  for (retries = 1000000; -- retries > 0;) {
    st = oz_hw486_inb (AUX_STATUS);		/* check status */
    if (!(st & 0x01)) continue;			/* keep looping until we get something */
    db = oz_hw486_inb (AUX_INPUT_PORT);		/* read it */
    if (!(st & 0x20)) continue;			/* skip if from keyboard (it gets lost) */
    return (db);				/* from mouse, return it */
  }
  return (-1);					/* took too long, return failure */
}
#endif /* INITIALIZE_DEVICE */

/************************************************************************/
/*									*/
/*  Write 'command byte' in the controller				*/
/*									*/
/************************************************************************/

static int aux_write_cmd (int val)

{
  if (!poll_aux_status ()) return (0);		/* wait for chip ready */
  oz_hw486_outb (AUX_CMD_WRITE, AUX_COMMAND);	/* tell it we are going to write a command byte */
  if (!poll_aux_status ()) return (0);		/* wait for chip ready */
  oz_hw486_outb (val, AUX_OUTPUT_PORT);		/* send it the command byte */
  return (1);
}

/************************************************************************/
/*									*/
/*  Transmit data byte to mouse						*/
/*									*/
/************************************************************************/

static int aux_write_dev (int val)

{
  if (!poll_aux_status ()) return (0);		/* wait for chip ready */
  oz_hw486_outb (AUX_MAGIC_WRITE, AUX_COMMAND);	/* write command that says next byte gets xmtd to mouse */
  if (!poll_aux_status ()) return (0);		/* wait for chip ready */
  oz_hw486_outb (val, AUX_OUTPUT_PORT);		/* write data to mouse */
  return (1);
}

/************************************************************************/
/*									*/
/*  Wait for there to be nothing in input or output ports		*/
/*  If there is something waiting to be read, flush it			*/
/*									*/
/*    Output:								*/
/*									*/
/*	poll_aux_status = 0 : failed (timed out)			*/
/*	                  1 : successful				*/
/*									*/
/************************************************************************/

static int poll_aux_status (void)

{
  int retries;
  uByte st;

  for (retries = 1000000; -- retries > 0;) {	/* only wait so long */
    st = oz_hw486_inb (AUX_STATUS);		/* read the status port */
    if (!(st & 0x02)) return (1);		/* if it is able to accept something, we're done */
    if (st & 0x01) oz_hw486_inb (AUX_INPUT_PORT);	/* else, if it is hung-up with an byte, flush it */
  }
  return (0);					/* ran through all retries, return failure */
}
