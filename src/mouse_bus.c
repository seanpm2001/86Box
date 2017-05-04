/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Implementation of Bus Mouse devices.
 *
 *		These mice devices were made by both Microsoft (InPort) and
 *		Logitech. Sadly, they did not use the same I/O protocol, but
 *		they were close enough to fit into a single implementation.
 *
 *		Although the Minix driver blindly took IRQ5, the board seems
 *		to be able to tell the driver what IRQ it is set for. When
 *		testing on MS-DOS (6.22), the 'mouse.exe' driver did not want
 *		to start, and only after disassembling it and inspecting the
 *		code it was discovered that driver actually does use the IRQ
 *		reporting feature. In a really, really weird way, too: it
 *		sets up the board, and then reads the CTRL register which is
 *		supposed to return that IRQ value. Depending on whether or 
 *		not the FREEZE bit is set, it has to return either the two's
 *		complemented (negated) value, or (if clear) just the value.
 *		The mouse.com driver reads both values 10,000 times, and
 *		then makes up its mind.  Maybe an effort to 'debounce' the
 *		reading of the DIP switches?  Oh-well.
 *
 *		Based on an early driver for MINIX 1.5.
 *		Based on the 86Box PS/2 mouse driver as a framework.
 *
 * NOTE:	Still have to add the code for the MS InPort mouse, which
 *		is very similar.  Almost done, but not ready for release.
 *
 * Version:	@(#)mouse_bus.c	1.0.3	2017/04/22
 *
 * Author:	Fred N. van Kempen, <decwiz@yahoo.com>
 *		Copyright 1989-2017 Fred N. van Kempen.
 */
#include <stdio.h>
#include <stdlib.h>
#include "ibm.h"
#include "io.h"
#include "pic.h"
#include "mouse.h"
#include "mouse_bus.h"
#include "plat-mouse.h"


#define ENABLE_3BTN		1		/* enable 3-button mode */


/* Register definitions (based on Logitech info.) */
#define	LTMOUSE_DATA	0			/* DATA register */
#define	LTMOUSE_MAGIC	1			/* signature magic register */
# define MAGIC_BYTE1	0xa5			/* most drivers use this */
# define MAGIC_BYTE2	0x5a			/* some drivers use this */
# define MAGIC_MSBYTE1	0xde			/* indicates MS InPort */
# define MAGIC_MSBYTE2	0xad
#define	LTMOUSE_CTRL	2			/* CTRL register */
# define CTRL_FREEZE	0x80			/* do not sample when set */
# define CTRL_RD_Y_HI	0x60			/* plus FREEZE */
# define CTRL_RD_Y_LO	0x40			/* plus FREEZE */
# define CTRL_RD_X_HI	0x20			/* plus FREEZE */
# define CTRL_RD_X_LO	0x00			/* plus FREEZE */
# define CTRL_RD_MASK	0x60
# define CTRL_IDISABLE	0x10
# define CTRL_IENABLE	0x00
# define CTRL_DFLT	(CTRL_IDISABLE)
#define	LTMOUSE_CONFIG	3			/* CONFIG register */
# define CONFIG_DFLT	0x91			/* 8255 controller config */

#define	MSMOUSE_CTRL	0			/* CTRL register */
#define	MSMOUSE_DATA	1			/* DATA register */
#define	MSMOUSE_MAGIC	2			/* MAGIC register */
#define	MSMOUSE_CONFIG	3			/* CONFIG register */


/* Our mouse device. */
typedef struct {
    uint16_t	port;				/* I/O port range start */
    uint16_t	portlen;			/* length of I/O port range */
    int8_t	irq;				/* IRQ channel to use */
    uint8_t	flags;				/* device flags */

    uint8_t	r_magic;			/* MAGIC register */
    uint8_t	r_ctrl;				/* CONTROL register (WR) */
    uint8_t	r_intr;				/* INTSTAT register (RO) */
    uint8_t	r_conf;				/* CONFIG register */

    int8_t	x, y;				/* current mouse status */
    uint8_t	but;
} mouse_bus_t;
#define MOUSE_ENABLED	0x80			/* device is enabled for use */
#define MOUSE_LOGITECH	0x40			/* running as Logitech mode */
#define MOUSE_CMDFLAG	0x01			/* next wr is a command (MS) */


/* Handle a write to the control register. */
static void
wctrl(mouse_bus_t *ms, uint8_t val)
{
    uint8_t b = (ms->r_ctrl ^ val);

    if (b & CTRL_FREEZE) {
	/* Hold the sampling while we do something. */
	if (! (val & CTRL_FREEZE)) {
		/* Reset current state. */
		ms->x = ms->y = 0;
		if (ms->but)	/* allow one more POLL for button-release */
			ms->but = 0x80;
	}
    }

    if (b & CTRL_IDISABLE) {
	/* Disable or enable interrupts. */
	/* (we don't do anything for that here..) */
    }

    /* Save new register value. */
    ms->r_ctrl = val;
}


/* Handle a WRITE operation to one of our registers. */
static void
busmouse_write(uint16_t port, uint8_t val, void *priv)
{
    mouse_bus_t *ms = (mouse_bus_t *)priv;

#if 0
    pclog("BUSMOUSE: write(%d,%02x)\n", port-ms->port, val);
#endif

    switch (port-ms->port) {
	case LTMOUSE_DATA:	/* [00] data register */
		break;

	case LTMOUSE_MAGIC:	/* [01] magic data register */
		if (val == MAGIC_BYTE1 || val == MAGIC_BYTE2) {
			ms->flags |= MOUSE_LOGITECH;
			ms->r_magic = val;
		}
		break;

	case LTMOUSE_CTRL:	/* [02] control register */
		wctrl(ms, val);
		break;

	case LTMOUSE_CONFIG:	/* [03] config register */
		ms->r_conf = val;
		break;

	default:
		break;
    }
}


/* Handle a READ from a Microsoft-mode register. */
static uint8_t
ms_read(mouse_bus_t *ms, uint16_t port)
{
    uint8_t r = 0xff;

    switch (port) {
	case MSMOUSE_CTRL:	/* [00] control register */
		r = ms->r_ctrl;
		break;

	case MSMOUSE_DATA:	/* [01] data register */
		break;

	case MSMOUSE_MAGIC:	/* [02] magic data register */
		/*
		 * Drivers for the InPort controllers usually start
		 * by reading this register. If they find 0xDE here,
		 * they will continue their probe, otherwise no go.
		 */
		r = ms->r_magic;

		/* For the InPort, switch magic bytes. */
		if (ms->r_magic == MAGIC_MSBYTE1)
			ms->r_magic = MAGIC_MSBYTE2;
		  else
			ms->r_magic = MAGIC_MSBYTE1;
		break;

	case MSMOUSE_CONFIG:	/* [03] config register */
		r = ms->r_conf;
		break;

	default:
		break;
    }

#if 0
    pclog("BUSMOUSE: msread(%d): %02x\n", port, r);
#endif

    return(r);
}


/* Handle a READ from a LOGITECH-mode register. */
static uint8_t
lt_read(mouse_bus_t *ms, uint16_t port)
{
    uint8_t r = 0xff;

    switch (port) {
	case LTMOUSE_DATA:	/* [00] data register */
		if (! (ms->r_ctrl & CTRL_FREEZE)) {
			r = 0x00;
		} else switch(ms->r_ctrl & CTRL_RD_MASK) {
			case CTRL_RD_X_LO:	/* X, low bits */
				/*
				 * Some drivers expect the buttons to
				 * be in this byte. Others want it in
				 * the Y-LO byte.  --FvK
				 */
				r = 0x07;
				if (ms->but & 0x01)	/*LEFT*/
					r &= ~0x04;
				if (ms->but & 0x02)	/*RIGHT*/
					r &= ~0x01;
#if ENABLE_3BTN
				if (ms->but & 0x04)	/*MIDDLE*/
					r &= ~0x02;
#endif
				r <<= 5;
				r |= (ms->x & 0x0f);
				break;

			case CTRL_RD_X_HI:	/* X, high bits */
				r = (ms->x >> 4) & 0x0f;
				break;

			case CTRL_RD_Y_LO:	/* Y, low bits */
				r = (ms->y & 0x0f);
				break;

			case CTRL_RD_Y_HI:	/* Y, high bits */
				/*
				 * Some drivers expect the buttons to
				 * be in this byte. Others want it in
				 * the X-LO byte.  --FvK
				 */
				r = 0x07;
				if (ms->but & 0x01)	/*LEFT*/
					r &= ~0x04;
				if (ms->but & 0x02)	/*RIGHT*/
					r &= ~0x01;
#if ENABLE_3BTN
				if (ms->but & 0x04)	/*MIDDLE*/
					r &= ~0x02;
#endif
				r <<= 5;
				r |= (ms->y >> 4) & 0x0f;
				break;
		}
		break;

	case LTMOUSE_MAGIC:	/* [01] magic data register */
		/*
		 * Logitech drivers start out by blasting their magic
		 * value (0xA5) into this register, and then read it
		 * back to see if that worked. If it did (and we do
		 * support this) the controller is assumed to be a
		 * Logitech-protocol one, and not InPort.
		 */
		r = ms->r_magic;
		break;

	case LTMOUSE_CTRL:	/* [02] control register */
		/*
		 * This is the weird stuff mentioned in the file header
		 * above. Microsoft's "mouse.exe" does some whacky stuff
		 * to extract the configured IRQ channel from the board.
		 *
		 * First, it reads the current value, and then re-reads
		 * it another 10,000 (yes, really) times. It keeps track
		 * of whether or not the data has changed, most likely
		 * to de-bounce reading of a DIP switch for example. This
		 * first value is assumed to be the 2's complement of the
		 * actual IRQ value.
		 * Next, it does this a second time, but now with the 
		 * IDISABLE bit clear (so, interrupts enabled), which is
		 * our cue to return the regular (not complemented) value
		 * to them.
		 *
		 * Since we have to fake the initial value and the settling
		 * of the data a bit later on, we first return a bunch of
		 * invalid ("random") data, and then the real value.
		 *
		 * Yes, this is weird.  --FvK
		 */
		if (ms->r_intr++ < 250)
			/* Still settling, return invalid data. */
			r = (ms->r_ctrl&CTRL_IDISABLE)?0xff:0x00;
		  else {
			/* OK, all good, return correct data. */
			r = (ms->r_ctrl&CTRL_IDISABLE)?-ms->irq:ms->irq;
			ms->r_intr = 0;
		}
		break;

	case LTMOUSE_CONFIG:	/* [03] config register */
		r = ms->r_conf;
		break;

	default:
		break;
    }

#if 0
    pclog("BUSMOUSE: ltread(%d): %02x\n", port, r);
#endif

    return(r);
}


/* Handle a READ operation from one of our registers. */
static uint8_t
busmouse_read(uint16_t port, void *priv)
{
    mouse_bus_t *ms = (mouse_bus_t *)priv;
    uint8_t r;

    if (ms->flags & MOUSE_LOGITECH)
	r = lt_read(ms, port - ms->port);
      else
	r = ms_read(ms, port - ms->port);

    return(r);
}


/* The emulator calls us with an update on the host mouse device. */
static uint8_t
busmouse_poll(int x, int y, int z, int b, void *priv)
{
    mouse_bus_t *ms = (mouse_bus_t *)priv;

#if 0
    pclog("BUSMOUSE: poll(%d,%d,%d, %02x)\n", x, y, z, b);
#endif

    /* Return early if nothing to do. */
    if (!x && !y && !z && (ms->but == b)) return(1);

    /* If we are not interested, return. */
    if (!(ms->flags & MOUSE_ENABLED) ||
	(ms->r_ctrl & CTRL_FREEZE)) return(0);

    /* Add the delta to our state. */
    x += ms->x;
    if (x > 127)
	x = 127;
    if (x < -128)
	x = -128;
    ms->x = (int8_t)x;

    y += ms->y;
    if (y > 127)
	y = 127;
    if (y < -128)
	y = -128;
    ms->y = (int8_t)y;

    ms->but = b;

    /* All set, generate an interrupt. */
    if (! (ms->r_ctrl & CTRL_IDISABLE))
		picint(1 << ms->irq);

    return(0);
}


/* Release all resources held by the device. */
static void
busmouse_close(void *priv)
{
    mouse_bus_t *ms = (mouse_bus_t *)priv;

    /* Release our I/O range. */
    io_removehandler(ms->port, ms->portlen,
		     busmouse_read, NULL, NULL, busmouse_write, NULL, NULL,
		     ms);

    free(ms);
}


/* Initialize the device for use by the user. */
static void *
busmouse_init(void)
{
    mouse_bus_t *ms;

    ms = (mouse_bus_t *)malloc(sizeof(mouse_bus_t));
    memset(ms, 0x00, sizeof(mouse_bus_t));
    ms->port = BUSMOUSE_PORT;
    ms->portlen = BUSMOUSE_PORTLEN;
#if BUSMOUSE_IRQ
    ms->irq = BUSMOUSE_IRQ;
#else
    ms->irq = -1;
#endif

    pclog("Logitech/Microsoft Bus Mouse, I/O=%04x, IRQ=%d\n",
					ms->port, ms->irq);
    /* Initialize registers. */
    ms->r_magic = MAGIC_MSBYTE1;
    ms->r_conf = CONFIG_DFLT;
    ms->r_ctrl = CTRL_DFLT;

    /*
     * Technically this is not possible, but we fake that we
     * did a power-up initialization with default config as
     * set in the "conf" register.  Emulators rock! --FvK
     */
    ms->flags = MOUSE_ENABLED;

    /* Request an I/O range. */
    io_sethandler(ms->port, ms->portlen,
		  busmouse_read, NULL, NULL, busmouse_write, NULL, NULL,
		  ms);

    /* Return our private data to the I/O layer. */
    return(ms);
}


mouse_t mouse_bus = {
    "Bus Mouse",
    "msbus",
    MOUSE_TYPE_BUS,
    busmouse_init,
    busmouse_close,
    busmouse_poll
};
