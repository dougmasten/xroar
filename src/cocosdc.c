/* CoCoSDC cartridge.
 *
 * Copyright 2026 Ciaran Anscomb
 *
 * This file is part of XRoar, a Dragon/Tandy CoCo emulator.
 *
 * XRoar is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * See COPYING.GPL for redistribution conditions.
 */

/* Sources
 *
 * - CoCo SDC User Guide (Darren Atkinson)
 * - CoCo SDC Technical Info
 * - VCC's CoCo SDC emulation (sdc.cpp)
 * - Disassembly of SDC-DOS 1.75 and the MCU firmware
 */

/* The CoCo SDC is a floppy controller replacement.  In "FDC emulation" mode
 * it looks like a WD1793 based disk controller, emulated here with the same
 * WD2793 and virtual drives as the RS-DOS cartridge.  Storing $43 in the
 * control latch ($FF40) switches it to "command mode", where $FF48-$FF4B
 * become a command interface for reading and writing logical sectors of the
 * mounted images and for operations on the SD card's files.  SDF images,
 * which hold raw tracks for copy protected disks, are accessed through FDC
 * emulation instead: the undocumented 'e' command tells SDC-DOS which drives
 * hold them.
 *
 * SDC drives 0 and 1 are XRoar's floppy drives 0 and 1, so -load-fd0 and
 * -load-fd1 mount images in them.  The SD card is a host directory given
 * with -load-hd0; commands that mount images or list directories work
 * within it, and its STARTUP.CFG mounts images at power up.  As on the real
 * card, only names that fit 8.3 are visible, matched case-insensitively, and
 * "M:" accepts wildcards and a name without its extension.  A file can
 * also be mounted as an array of raw 256 byte blocks ("m:"), accessible by
 * logical sector number or streaming but not through FDC emulation.
 * Mounting a directory in drive 0 mounts a disk set: images in it named
 * "FOO1.DSK" to "FOO9.DSK", switched with the '#' and '+' commands.
 *
 * The 128K of flash is eight 16K banks mapped at $C000-$FEFF.  By default
 * bank 0 holds SDC-DOS, bank 1 RS-DOS, bank 4 "dplus-sdc" and bank 5 a
 * DragonDOS compatible DOS.  A ROM image given with -cart-rom fills banks from
 * 0 instead.  Writing $FF43 selects the bank (low three bits).  Reading $FF43
 * returns the bank number in the low three bits and the top five bits of the
 * last value written to $FF42, which is how software detects the SDC.  Flash
 * programming is not emulated: writes to the flash are ignored.
 *
 * Commands take time, as on the real controller: BUSY stays set until a
 * command completes, and READY is only set once a block can be transferred.
 * The delays were measured on real hardware.  The MCU reads and writes the SD
 * card through a single 512 byte buffer, so a logical sector in the same SD
 * block as the last one is quick, and a modified block is written back when
 * another is needed.  How long the card itself takes varies a lot from card
 * to card; the times used here are typical.
 */

#include "top-config.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "array.h"
#include "delegate.h"
#include "events.h"
#include "sds.h"
#include "xalloc.h"

#include "cart.h"
#include "logging.h"
#include "messenger.h"
#include "part.h"
#include "rombank.h"
#include "romlist.h"
#include "serialise.h"
#include "ui.h"
#include "vdisk.h"
#include "vdrive.h"
#include "wd279x.h"
#include "xroar.h"

#define SDC_BANK_SIZE (0x4000)
#define SDC_NBANKS (8)

// Status register bits
#define STA_BUSY     (0x01)
#define STA_READY    (0x02)
#define STA_INVALID  (0x04)
#define STA_CRC      (0x08)
#define STA_NOTFOUND (0x10)
#define STA_INUSE    (0x20)
#define STA_FAILED   (0x80)

// Firmware version reported by the 'V' command (BCD)
#define SDC_VERSION (0x0130)

#define SDC_SECTORS_PER_TRACK (18)

// Command timing, measured on a real CoCo SDC.  These are set by the MCU and
// were the same with every card tried.
#define T_ACCEPT  EVENT_US(45)    // until a command or block is accepted
#define T_EXT     EVENT_US(220)   // extended command not using the card
#define T_INFO    EVENT_US(260)   // 'I' and 'C'
#define T_LIST    EVENT_US(140)   // "L:" (the directory is read by '>')
#define T_HIT     EVENT_US(100)   // read a sector in the buffered SD block
#define T_WRITE   EVENT_US(300)   // write a sector into the buffered block
#define T_STREAM1 EVENT_US(420)   // first block of a stream
#define T_STREAM  EVENT_US(220)   // each later block, once the last is read
#define T_ABORT   EVENT_US(1720)  // abort a stream
#define T_SCAN    EVENT_US(680)   // look for a disk in the set's directory
#define T_BLINK   EVENT_MS(380)   // blink the LED once (38 x 10ms delays)

// These depend on the SD card: typical values.
#define T_SD_READ  EVENT_US(1300)  // read a 512 byte block
#define T_SD_WRITE EVENT_US(3000)  // write a 512 byte block
#define T_DIR      (4 * T_SD_READ)  // find a file
#define T_DIRPAGE  (3 * T_SD_READ)  // a page of directory listing

#define SD_BLOCK_NONE (~0U)
// The directory entry of the image in a drive, read by 'I' and 'Q'
#define SD_BLOCK_ENTRY(drive) (((drive) << 24) | 0xffffff)
// A new directory's cluster is zeroed: 32K clusters are usual on FAT32 cards
#define SD_CLUSTER_BLOCKS (64)

struct sdc_dirent {
	char name[8];
	char ext[3];
	uint8_t attr;
	uint32_t size;
};

struct cocosdc {
	struct cart cart;

	// FDC emulation
	unsigned latch_old;
	unsigned latch_drive_select;
	bool latch_density;
	bool drq_flag;
	bool intrq_flag;
	bool halt_enable;
	struct WD279X *fdc;
	struct vdrive_interface *vdrive_interface;

	// Flash
	unsigned bank;
	uint8_t flash_data;

	// Command mode
	bool cmd_mode;
	uint8_t cmd;
	uint8_t status;
	uint8_t param[3];
	uint8_t buf[256];
	unsigned buf_pos;
	unsigned buf_count;  // bytes left to transfer
	bool buf_rx;  // block is being sent by the CoCo
	bool byte_mode;  // block transferred 8 bits at a time through $FF4B
	uint8_t flags;  // MCU option byte, set by the undocumented 'g' command

	// Timing
	struct event ready_event;  // ends the current command's delay
	uint8_t ready_status;  // status once the delay is over
	unsigned ready_count;  // and bytes to transfer
	event_ticks delay;  // accumulated by the current command
	unsigned sd_block;  // block in the MCU's buffer: drive << 24 | block
	bool sd_dirty;  // buffered block has been modified

	// SD card
	int msgr_client_id;
	char *sd_root;
	sds cur_dir;  // relative to sd_root, no leading or trailing '/'
	uint8_t path_status;  // why resolve_path() last failed
	struct {
		sds path;  // host path of a file mounted as raw blocks
		FILE *raw;  // open if mounted as raw blocks ("m:" or "n:")
		bool raw_ro;
		bool written;  // directory entry to update on unmount
	} drv[2];
	sds set_dir;  // host directory of the disk set in drive 0
	unsigned set_num;  // disk number in the set (1-9)

	// Streaming ($90/$91): 512 byte blocks of a mounted file
	FILE *stream;
	uint8_t sbuf[512];
	unsigned spos;
	struct sdc_dirent *listing;
	unsigned listing_n;
	unsigned listing_pos;
};

static const struct ser_struct ser_struct_cocosdc[] = {
	SER_ID_STRUCT_NEST(1, &cart_ser_struct_data),
	SER_ID_STRUCT_ELEM(2, struct cocosdc, latch_drive_select),
	SER_ID_STRUCT_ELEM(3, struct cocosdc, latch_density),
	SER_ID_STRUCT_ELEM(4, struct cocosdc, drq_flag),
	SER_ID_STRUCT_ELEM(5, struct cocosdc, intrq_flag),
	SER_ID_STRUCT_ELEM(6, struct cocosdc, halt_enable),
	SER_ID_STRUCT_ELEM(7, struct cocosdc, bank),
	SER_ID_STRUCT_ELEM(8, struct cocosdc, flash_data),
	SER_ID_STRUCT_ELEM(9, struct cocosdc, cmd_mode),
	SER_ID_STRUCT_ELEM(10, struct cocosdc, status),
	SER_ID_STRUCT_ELEM(11, struct cocosdc, flags),
	SER_ID_STRUCT_TYPE(12, ser_type_event, struct cocosdc, ready_event),
	SER_ID_STRUCT_ELEM(13, struct cocosdc, ready_status),
	SER_ID_STRUCT_ELEM(14, struct cocosdc, ready_count),
	SER_ID_STRUCT_ELEM(15, struct cocosdc, sd_block),
	SER_ID_STRUCT_ELEM(16, struct cocosdc, sd_dirty),
};

static const struct ser_struct_data cocosdc_ser_struct_data = {
	.elems = ser_struct_cocosdc,
	.num_elems = ARRAY_N_ELEMENTS(ser_struct_cocosdc),
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void cocosdc_config_complete(struct cart_config *);

static uint8_t cocosdc_read(struct cart *c, uint16_t A, bool P2, bool R2, uint8_t D);
static uint8_t cocosdc_write(struct cart *c, uint16_t A, bool P2, bool R2, uint8_t D);
static void cocosdc_reset(struct cart *c, bool hard);
static void cocosdc_detach(struct cart *c);
static bool cocosdc_has_interface(struct cart *c, const char *ifname);
static void cocosdc_attach_interface(struct cart *c, const char *ifname, void *intf);

static void set_drq(void *sptr, bool value);
static void set_intrq(void *sptr, bool value);
static void latch_write(struct cocosdc *d, unsigned D);

static void cmd_write(struct cocosdc *d, unsigned reg, uint8_t D);
static uint8_t cmd_read(struct cocosdc *d, unsigned reg);
static void stop_stream(struct cocosdc *d);
static void load_startup_cfg(struct cocosdc *d);
static void ready_done(void *sptr);

static void cocosdc_ui_set_hd_filename(void *sptr, int tag, void *smsg);

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// CoCo SDC part creation

static struct part *cocosdc_allocate(void);
static void cocosdc_initialise(struct part *p, void *options);
static bool cocosdc_finish(struct part *p);
static void cocosdc_free(struct part *p);

static const struct partdb_entry_funcs cocosdc_funcs = {
	.allocate = cocosdc_allocate,
	.initialise = cocosdc_initialise,
	.finish = cocosdc_finish,
	.free = cocosdc_free,

	.ser_struct_data = &cocosdc_ser_struct_data,

	.is_a = dragon_cart_is_a,
};

const struct cart_partdb_entry cocosdc_part = { .partdb_entry = { .name = "cocosdc", .description = "Darren Atkinson | CoCo SDC", .funcs = &cocosdc_funcs }, .config_complete = cocosdc_config_complete };

static struct part *cocosdc_allocate(void) {
	struct cocosdc *d = part_new(sizeof(*d));
	struct cart *c = &d->cart;
	struct part *p = &c->part;

	*d = (struct cocosdc){0};

	cart_rom_init(c);

	c->detach = cocosdc_detach;
	c->read = cocosdc_read;
	c->write = cocosdc_write;
	c->reset = cocosdc_reset;
	c->has_interface = cocosdc_has_interface;
	c->attach_interface = cocosdc_attach_interface;

	event_init(&d->ready_event, MACHINE_EVENT_LIST, DELEGATE_AS0(void, ready_done, d));
	d->sd_block = SD_BLOCK_NONE;

	return p;
}

static void cocosdc_initialise(struct part *p, void *options) {
	struct cart_config *cc = options;
	assert(cc != NULL);

	cart_rom_initialise(p, options);
	part_add_component(p, part_create("WD2793", "WD2793"), "FDC");
}

// Load a ROM image into a flash bank, unless the bank already holds one.

static void load_bank(struct rombank *rb, unsigned bank, const char *name) {
	if (rb->d[bank])
		return;
	sds tmp = romlist_find(name);
	if (tmp)
		rombank_load_image(rb, bank, tmp, 0);
	sdsfree(tmp);
}

static bool cocosdc_finish(struct part *p) {
	struct cocosdc *d = (struct cocosdc *)p;
	struct cart *c = &d->cart;

	d->fdc = (struct WD279X *)part_component_by_id_is_a(p, "FDC", "WD2793");
	if (d->fdc == NULL) {
		return 0;
	}

	// A ROM image given with -cart-rom fills banks from 0, otherwise bank
	// 0 holds SDC-DOS.  Banks still empty get the default layout.
	c->ROM = rombank_new(8, SDC_BANK_SIZE, SDC_NBANKS);
	load_bank(c->ROM, 0, c->config->rom ? c->config->rom : "sdcdos");
	load_bank(c->ROM, 1, "@rsdos");
	load_bank(c->ROM, 4, "dplus-sdc");
	load_bank(c->ROM, 5, "@dragondos_compat");
	rombank_report(c->ROM, "cocosdc", "Flash");

	if (!cart_finish(p)) {
		return 0;
	}

	if (d->ready_event.next == &d->ready_event) {
		event_queue(&d->ready_event);
	}

	d->cur_dir = sdsempty();
	d->msgr_client_id = messenger_client_register();
	ui_messenger_join_group(d->msgr_client_id, ui_tag_hd_filename, MESSENGER_NOTIFY_DELEGATE(cocosdc_ui_set_hd_filename, d));

	return 1;
}

static void cocosdc_free(struct part *p) {
	struct cocosdc *d = (struct cocosdc *)p;
	event_dequeue(&d->ready_event);
	messenger_client_unregister(d->msgr_client_id);
	free(d->sd_root);
	sdsfree(d->cur_dir);
	for (unsigned i = 0; i < 2; i++) {
		sdsfree(d->drv[i].path);
		if (d->drv[i].raw)
			fclose(d->drv[i].raw);
	}
	sdsfree(d->set_dir);
	if (d->stream)
		fclose(d->stream);
	free(d->listing);
	cart_rom_free(p);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void cocosdc_config_complete(struct cart_config *cc) {
	(void)cc;
}

static void cocosdc_ui_set_hd_filename(void *sptr, int tag, void *smsg) {
	struct cocosdc *d = sptr;
	struct ui_state_message *uimsg = smsg;
	assert(tag == ui_tag_hd_filename);
	if (uimsg->value != 0)
		return;
	free(d->sd_root);
	d->sd_root = NULL;
	sdsclear(d->cur_dir);
	if (uimsg->data) {
		struct stat st;
		if (stat(uimsg->data, &st) == 0 && S_ISDIR(st.st_mode)) {
			d->sd_root = xstrdup(uimsg->data);
			load_startup_cfg(d);
		} else {
			LOG_MOD_WARN("cocosdc", "SD card must be a directory: %s\n", (const char *)uimsg->data);
		}
	}
}

static void cocosdc_reset(struct cart *c, bool hard) {
	struct cocosdc *d = (struct cocosdc *)c;
	cart_rom_reset(c, hard);
	wd279x_reset(d->fdc);
	d->latch_old = -1;
	d->latch_drive_select = -1;
	d->drq_flag = d->intrq_flag = 0;
	d->cmd_mode = 0;
	d->status = 0;
	d->buf_count = 0;
	event_dequeue(&d->ready_event);
	stop_stream(d);
	if (hard) {
		d->bank = 0;
		d->flash_data = 0;
		d->flags = 0;
		d->sd_block = SD_BLOCK_NONE;
		d->sd_dirty = 0;
		if (d->sd_root)
			load_startup_cfg(d);
	}
	latch_write(d, 0);
}

static void cocosdc_detach(struct cart *c) {
	struct cocosdc *d = (struct cocosdc *)c;
	vdrive_disconnect(d->vdrive_interface);
	wd279x_disconnect(d->fdc);
	cart_rom_detach(c);
}

static uint8_t cocosdc_read(struct cart *c, uint16_t A, bool P2, bool R2, uint8_t D) {
	struct cocosdc *d = (struct cocosdc *)c;
	if (R2) {
		if ((A & 0x3fff) < 0x3f00)
			rombank_d8(c->ROM, (d->bank << 14) | (A & 0x3fff), &D);
		return D;
	}
	if (!P2) {
		return D;
	}
	unsigned reg = A & 0xf;
	if (reg & 0x8) {
		if (d->cmd_mode)
			return cmd_read(d, reg & 3);
		return wd279x_read(d->fdc, A);
	}
	switch (reg) {
	case 0x2:
		return d->flash_data;
	case 0x3:
		return d->bank | (d->flash_data & 0xf8);
	default:
		break;
	}
	return D;
}

static uint8_t cocosdc_write(struct cart *c, uint16_t A, bool P2, bool R2, uint8_t D) {
	struct cocosdc *d = (struct cocosdc *)c;
	if (R2) {
		return D;
	}
	if (!P2) {
		return D;
	}
	unsigned reg = A & 0xf;
	if (reg & 0x8) {
		if (d->cmd_mode)
			cmd_write(d, reg & 3, D);
		else
			wd279x_write(d->fdc, A, D);
		return D;
	}
	switch (reg) {
	case 0x0:
		if (D == 0x43) {
			d->cmd_mode = 1;
			LOG_MOD_DEBUG(3, "cocosdc", "command mode\n");
		} else {
			if (d->cmd_mode) {
				LOG_MOD_DEBUG(3, "cocosdc", "FDC emulation mode\n");
				// $FF4B keeps the last response byte: SDC-DOS
				// reads the 'e' result after leaving command mode
				wd279x_write(d->fdc, 3, d->param[2]);
			}
			d->cmd_mode = 0;
			d->buf_count = 0;
			event_dequeue(&d->ready_event);
			stop_stream(d);
			latch_write(d, D);
		}
		break;
	case 0x2:
		d->flash_data = D;
		break;
	case 0x3:
		// bit 7 enables flash programming, which isn't emulated
		if (!(D & 0x80))
			d->bank = D & 7;
		break;
	default:
		break;
	}
	return D;
}

static bool cocosdc_has_interface(struct cart *c, const char *ifname) {
	return c && (0 == strcmp(ifname, "floppy"));
}

static void cocosdc_attach_interface(struct cart *c, const char *ifname, void *intf) {
	if (!c || (0 != strcmp(ifname, "floppy")))
		return;
	struct cocosdc *d = (struct cocosdc *)c;
	d->vdrive_interface = intf;

	d->fdc->set_dirc = DELEGATE_AS1(void, bool, d->vdrive_interface->set_dirc, d->vdrive_interface);
	d->fdc->set_dden = DELEGATE_AS1(void, bool, d->vdrive_interface->set_dden, d->vdrive_interface);
	d->fdc->set_drq = DELEGATE_AS1(void, bool, set_drq, d);
	d->fdc->set_intrq = DELEGATE_AS1(void, bool, set_intrq, d);
	d->fdc->step = DELEGATE_AS0(void, d->vdrive_interface->step, d->vdrive_interface);
	d->fdc->write = DELEGATE_AS1(void, uint8, d->vdrive_interface->write, d->vdrive_interface);
	d->fdc->skip = DELEGATE_AS0(void, d->vdrive_interface->skip, d->vdrive_interface);
	d->fdc->read = DELEGATE_AS0(uint8, d->vdrive_interface->read, d->vdrive_interface);
	d->fdc->write_idam = DELEGATE_AS0(void, d->vdrive_interface->write_idam, d->vdrive_interface);
	d->fdc->time_to_next_byte = DELEGATE_AS0(unsigned, d->vdrive_interface->time_to_next_byte, d->vdrive_interface);
	d->fdc->time_to_next_idam = DELEGATE_AS0(unsigned, d->vdrive_interface->time_to_next_idam, d->vdrive_interface);
	d->fdc->next_idam = DELEGATE_AS0(uint8p, d->vdrive_interface->next_idam, d->vdrive_interface);
	d->fdc->update_connection = DELEGATE_AS0(void, d->vdrive_interface->update_connection, d->vdrive_interface);

	d->vdrive_interface->tr00 = DELEGATE_AS1(void, bool, wd279x_tr00, d->fdc);
	d->vdrive_interface->index_pulse = DELEGATE_AS1(void, bool, wd279x_index_pulse, d->fdc);
	d->vdrive_interface->write_protect = DELEGATE_AS1(void, bool, wd279x_write_protect, d->fdc);
	wd279x_update_connection(d->fdc);

	// tied high
	wd279x_ready(d->fdc, 1);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// FDC emulation: the same control latch as the RS-DOS cartridge

static void latch_write(struct cocosdc *d, unsigned D) {
	struct cart *c = (struct cart *)d;
	unsigned new_drive_select = 0;
	D ^= 0x20;
	if (D & 0x01) {
		new_drive_select = 0;
	} else if (D & 0x02) {
		new_drive_select = 1;
	} else if (D & 0x04) {
		new_drive_select = 2;
	} else if (D & 0x40) {
		new_drive_select = 3;
		D &= ~0x40;  // prevent interpreting as side select
	}
	d->latch_old = D;
	d->latch_drive_select = new_drive_select;
	if (d->vdrive_interface) {
		d->vdrive_interface->set_sso(d->vdrive_interface, (D & 0x40) ? 1 : 0);
		d->vdrive_interface->set_drive(d->vdrive_interface, d->latch_drive_select);
	}
	d->latch_density = D & 0x20;
	wd279x_set_dden(d->fdc, !d->latch_density);
	if (!d->latch_density && d->intrq_flag) {
		DELEGATE_CALL(c->signal_nmi, 1);
	}
	d->halt_enable = (D & 0x80) && !d->intrq_flag;
	DELEGATE_CALL(c->signal_halt, d->halt_enable && !d->drq_flag);
}

static void set_drq(void *sptr, bool value) {
	struct cocosdc *d = sptr;
	struct cart *c = &d->cart;
	d->drq_flag = value;
	DELEGATE_CALL(c->signal_halt, d->halt_enable && !d->drq_flag);
}

static void set_intrq(void *sptr, bool value) {
	struct cocosdc *d = sptr;
	struct cart *c = &d->cart;
	d->intrq_flag = value;
	if (value) {
		d->halt_enable = 0;
		DELEGATE_CALL(c->signal_halt, 0);
		if (!d->latch_density && d->intrq_flag) {
			DELEGATE_CALL(c->signal_nmi, 1);
		}
	} else {
		DELEGATE_CALL(c->signal_nmi, 0);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// SD card: host paths

// Convert a host file name to 8.3 form.  Returns false if it doesn't fit.

static bool name_83(const char *s, char name[8], char ext[3]) {
	memset(name, ' ', 8);
	memset(ext, ' ', 3);
	if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) {
		memcpy(name, s, strlen(s));
		return 1;
	}
	const char *dot = strrchr(s, '.');
	size_t nlen = dot ? (size_t)(dot - s) : strlen(s);
	size_t elen = dot ? strlen(dot + 1) : 0;
	if (nlen < 1 || nlen > 8 || elen > 3)
		return 0;
	for (size_t i = 0; i < nlen; i++) {
		if (s[i] == '.' || s[i] == ' ' || (unsigned char)s[i] > 0x7e)
			return 0;
		name[i] = toupper((unsigned char)s[i]);
	}
	for (size_t i = 0; i < elen; i++) {
		if ((unsigned char)dot[1 + i] > 0x7e || dot[1 + i] == ' ')
			return 0;
		ext[i] = toupper((unsigned char)dot[1 + i]);
	}
	return 1;
}

// Convert a name given by the CoCo to an 11 character 8.3 pattern, as the
// firmware does: upper case, padded with spaces, and if wild is set, '*' and
// '?' as wildcards ('*' fills the rest of the name or extension with '?').
// Returns false if the name isn't valid.

static bool pattern_83(const char *s, bool wild, char pat[11], bool *has_wild) {
	memset(pat, ' ', 11);
	*has_wild = 0;
	unsigned i = 0;
	bool ext = 0;
	for (; *s; s++) {
		char c = *s;
		if (c == '*') {
			if (!wild)
				return 0;
			unsigned lim = (i < 8) ? 8 : 11;
			for (; i < lim; i++)
				pat[i] = '?';
			*has_wild = 1;
			continue;
		}
		if (c == '.') {
			if (ext || i >= 9)
				return 0;
			ext = 1;
			i = 8;
			continue;
		}
		if (i >= 11 || (unsigned char)c < ' ')
			return 0;
		if (c == '?') {
			if (!wild)
				return 0;
			*has_wild = 1;
		} else if (strchr("\"/:<>\\|+,;=[]", c)) {
			return 0;
		}
		pat[i++] = toupper((unsigned char)c);
	}
	return pat[0] != ' ';
}

// Find the entry in a host directory matching an 8.3 pattern.  Only host
// names that fit 8.3 are visible, and they match case-insensitively (FAT
// names aren't case sensitive).  If several match, the first in name order
// stands in for the first in the directory.  Returns a new sds, or NULL.

static sds find_entry(const char *dir, const char pat[11]) {
	DIR *dh = opendir(dir);
	if (!dh)
		return NULL;
	sds found = NULL;
	char best[11];
	struct dirent *de;
	while ((de = readdir(dh))) {
		char name[11];
		if (de->d_name[0] == '.' || !name_83(de->d_name, name, name + 8))
			continue;
		bool match = 1;
		for (unsigned i = 0; i < 11 && match; i++)
			match = (pat[i] == '?' || pat[i] == name[i]);
		if (!match || (found && memcmp(name, best, 11) >= 0))
			continue;
		sdsfree(found);
		found = sdsnew(de->d_name);
		memcpy(best, name, 11);
	}
	closedir(dh);
	return found;
}

// Resolve an SDC path name to a host path, like the firmware.  Flags apply
// to the last element:
//
// - RP_NEW:     it need not exist (the 8.3 form of the name is used)
// - RP_WILD:    it may contain wildcards
// - RP_ANYEXT:  a missing extension matches any
//
// rel, if not NULL, receives the path relative to the SD root.  Returns a new
// sds, or NULL with the reason in d->path_status.

#define RP_NEW    (1 << 0)
#define RP_WILD   (1 << 1)
#define RP_ANYEXT (1 << 2)

static sds resolve_path(struct cocosdc *d, const char *path, unsigned flags, sds *rel) {
	d->path_status = STA_FAILED | STA_NOTFOUND;
	if (!d->sd_root)
		return NULL;
	sds r = (path[0] == '/') ? sdsempty() : sdsdup(d->cur_dir);
	int ncomp;
	sds *comp = sdssplitlen(path, strlen(path), "/", 1, &ncomp);
	// The last element that isn't empty (a trailing '/' is allowed)
	int last = ncomp - 1;
	while (last > 0 && sdslen(comp[last]) == 0)
		last--;
	bool ok = 1;
	for (int i = 0; i < ncomp && ok; i++) {
		if (sdslen(comp[i]) == 0 || strcmp(comp[i], ".") == 0)
			continue;
		if (strcmp(comp[i], "..") == 0) {
			char *slash = strrchr(r, '/');
			if (slash)
				sdsrange(r, 0, slash - r - 1);
			else
				sdsclear(r);
			continue;
		}
		char pat[11];
		bool has_wild;
		if (!pattern_83(comp[i], i == last && (flags & RP_WILD), pat, &has_wild)) {
			d->path_status = STA_FAILED | STA_INVALID;
			ok = 0;
			break;
		}
		if (i == last && (flags & RP_ANYEXT) && memcmp(pat + 8, "   ", 3) == 0)
			memset(pat + 8, '?', 3);
		sds dir = sdscatfmt(sdsnew(d->sd_root), "/%S", r);
		sds found = find_entry(dir, pat);
		sdsfree(dir);
		if (!found) {
			if (i != last || !(flags & RP_NEW) || has_wild) {
				ok = 0;
				break;
			}
			// A new file gets the 8.3 form of its name
			int n = 8;
			while (n > 0 && pat[n - 1] == ' ')
				n--;
			found = sdsnewlen(pat, n);
			if (pat[8] != ' ') {
				int e = 11;
				while (pat[e - 1] == ' ')
					e--;
				found = sdscat(found, ".");
				found = sdscatlen(found, pat + 8, e - 8);
			}
		}
		if (sdslen(r))
			r = sdscat(r, "/");
		r = sdscatsds(r, found);
		sdsfree(found);
	}
	sdsfreesplitres(comp, ncomp);
	if (!ok) {
		sdsfree(r);
		return NULL;
	}
	sds host = sdscatfmt(sdsnew(d->sd_root), "/%S", r);
	if (rel)
		*rel = r;
	else
		sdsfree(r);
	return host;
}

// Fill a 32 byte FAT style directory record for a host file.

static void fat_record(uint8_t *rec, const char *host) {
	memset(rec, 0, 32);
	const char *leaf = strrchr(host, '/');
	leaf = leaf ? leaf + 1 : host;
	name_83(leaf, (char *)rec, (char *)rec + 8);
	struct stat st;
	if (stat(host, &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			rec[11] = 0x10;
		} else {
			uint32_t size = st.st_size;
			rec[28] = size;
			rec[29] = size >> 8;
			rec[30] = size >> 16;
			rec[31] = size >> 24;
		}
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// SD card: directory listings

static bool match_part(const char *pat, size_t plen, const char *s, size_t slen) {
	size_t i = 0;
	for (; i < plen; i++) {
		if (pat[i] == '*')
			return 1;
		char c = i < slen ? s[i] : ' ';
		if (pat[i] != '?' && toupper((unsigned char)pat[i]) != c)
			return 0;
	}
	for (; i < slen; i++) {
		if (s[i] != ' ')
			return 0;
	}
	return 1;
}

static bool match_83(const char *pat, const struct sdc_dirent *e) {
	const char *dot = strchr(pat, '.');
	size_t nlen = dot ? (size_t)(dot - pat) : strlen(pat);
	if (!match_part(pat, nlen, e->name, 8))
		return 0;
	if (!dot)
		return memcmp(e->ext, "   ", 3) == 0 || (nlen > 0 && pat[nlen - 1] == '*');
	return match_part(dot + 1, strlen(dot + 1), e->ext, 3);
}

static int dirent_cmp(const void *a, const void *b) {
	const struct sdc_dirent *ea = a, *eb = b;
	int r = memcmp(ea->name, eb->name, 8);
	return r ? r : memcmp(ea->ext, eb->ext, 3);
}

static uint8_t start_listing(struct cocosdc *d, const char *arg) {
	free(d->listing);
	d->listing = NULL;
	d->listing_n = d->listing_pos = 0;

	// Split into directory and wildcard pattern
	const char *slash = strrchr(arg, '/');
	sds dirpath = slash ? sdsnewlen(arg, slash - arg + 1) : sdsempty();
	const char *pat = slash ? slash + 1 : arg;
	if (!*pat)
		pat = "*.*";
	sds rel = NULL;
	sds host = resolve_path(d, sdslen(dirpath) ? dirpath : ".", 0, &rel);
	sdsfree(dirpath);
	if (!host)
		return d->path_status;
	DIR *dh = opendir(host);
	if (!dh) {
		sdsfree(host);
		sdsfree(rel);
		return STA_FAILED | STA_NOTFOUND;
	}
	unsigned alloc = 0;
	struct dirent *de;
	while ((de = readdir(dh))) {
		struct sdc_dirent e = {0};
		// The root directory has no "." or ".." entries
		if (de->d_name[0] == '.' && (sdslen(rel) == 0 || (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))))
			continue;
		if (!name_83(de->d_name, e.name, e.ext))
			continue;
		if (!match_83(pat, &e))
			continue;
		sds p = sdscatfmt(sdsdup(host), "/%s", de->d_name);
		struct stat st;
		if (stat(p, &st) == 0) {
			if (S_ISDIR(st.st_mode))
				e.attr = 0x10;
			else
				e.size = st.st_size;
		}
		sdsfree(p);
		if (d->listing_n == alloc) {
			alloc = alloc ? alloc * 2 : 32;
			d->listing = xrealloc(d->listing, alloc * sizeof(*d->listing));
		}
		d->listing[d->listing_n++] = e;
	}
	closedir(dh);
	sdsfree(host);
	sdsfree(rel);
	if (d->listing_n)
		qsort(d->listing, d->listing_n, sizeof(*d->listing), dirent_cmp);
	return 0;
}

static uint8_t listing_page(struct cocosdc *d, uint8_t *page) {
	memset(page, 0, 256);
	for (unsigned i = 0; i < 16 && d->listing_pos < d->listing_n; i++) {
		struct sdc_dirent *e = &d->listing[d->listing_pos++];
		uint8_t *rec = page + i * 16;
		memcpy(rec, e->name, 8);
		memcpy(rec + 8, e->ext, 3);
		rec[11] = e->attr;
		rec[12] = e->size >> 24;
		rec[13] = e->size >> 16;
		rec[14] = e->size >> 8;
		rec[15] = e->size;
	}
	return 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Command mode: timing

// Write back the buffered block if it has been modified.

static void sd_flush(struct cocosdc *d) {
	if (d->sd_dirty) {
		d->delay += T_SD_WRITE;
		d->sd_dirty = 0;
	}
}

// Bring the block holding a drive's directory entry into the buffer.

static void sd_entry(struct cocosdc *d, unsigned drive) {
	if (d->sd_block != SD_BLOCK_ENTRY(drive)) {
		sd_flush(d);
		d->delay += T_SD_READ;
		d->sd_block = SD_BLOCK_ENTRY(drive);
	}
}

// Bring the SD block holding a logical sector into the buffer.  A sector
// write needs the whole block too, so it's read first.

static void sd_access(struct cocosdc *d, unsigned drive, unsigned lsn, bool write) {
	unsigned block = (drive << 24) | (lsn >> 1);
	if (block != d->sd_block) {
		sd_flush(d);
		d->delay += T_SD_READ;
		d->sd_block = block;
	}
	if (write) {
		d->sd_dirty = 1;
		d->drv[drive].written = 1;
	}
}

// Commands are performed straight away, but the result (status and any block
// to transfer) is hidden until the accumulated delay is over.  BUSY is set
// until then.

static void defer(struct cocosdc *d) {
	if (d->delay < T_ACCEPT)
		d->delay = T_ACCEPT;
	d->ready_status = d->status;
	d->ready_count = d->buf_count;
	d->buf_count = 0;
	d->status = STA_BUSY;
	event_queue_dt(&d->ready_event, d->delay);
}

static void ready_done(void *sptr) {
	struct cocosdc *d = sptr;
	d->status = d->ready_status;
	d->buf_count = d->ready_count;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Command mode: mounted images

static struct vdisk *drive_disk(struct cocosdc *d, unsigned drive) {
	if (!d->vdrive_interface)
		return NULL;
	return vdrive_disk_in_drive(d->vdrive_interface, drive);
}

// SDF images hold raw tracks, for disks a sector dump can't represent.

static bool drive_sdf(struct cocosdc *d, unsigned drive) {
	struct vdisk *disk = drive_disk(d, drive);
	return disk && disk->filetype == FILETYPE_SDF;
}

// Map a logical sector number to a cylinder, head and sector.  If ss is set,
// the LSN assumes a single-sided disk.

static bool lsn_chs(struct vdisk *disk, unsigned lsn, bool ss, unsigned *cyl, unsigned *head, unsigned *sector) {
	unsigned heads = disk->num_heads ? disk->num_heads : 1;
	unsigned track = lsn / SDC_SECTORS_PER_TRACK;
	*sector = lsn % SDC_SECTORS_PER_TRACK + 1;
	if (ss || heads == 1) {
		*cyl = track;
		*head = 0;
	} else {
		*cyl = track / heads;
		*head = track % heads;
	}
	return *cyl < disk->num_cylinders;
}

// Read or write a 256 byte block of a file mounted as raw blocks.  The last
// block of a file that isn't a multiple of 256 bytes is padded with zeroes.

static uint8_t raw_read(struct cocosdc *d, unsigned drive, unsigned lsn) {
	FILE *f = d->drv[drive].raw;
	memset(d->buf, 0, 256);
	if (fseeko(f, (off_t)lsn * 256, SEEK_SET) != 0)
		return STA_FAILED | STA_NOTFOUND;
	size_t n = fread(d->buf, 1, 256, f);
	return n > 0 ? 0 : (STA_FAILED | STA_NOTFOUND);
}

static uint8_t raw_write(struct cocosdc *d, unsigned drive, unsigned lsn) {
	FILE *f = d->drv[drive].raw;
	if (d->drv[drive].raw_ro)
		return STA_FAILED | STA_INUSE;
	if (fseeko(f, (off_t)lsn * 256, SEEK_SET) != 0 || fwrite(d->buf, 1, 256, f) != 256)
		return STA_FAILED | STA_NOTFOUND;
	fflush(f);
	return 0;
}

static uint8_t read_lsn(struct cocosdc *d, unsigned drive, unsigned lsn, bool ss) {
	if (d->drv[drive].raw)
		return raw_read(d, drive, lsn);
	struct vdisk *disk = drive_disk(d, drive);
	if (!disk)
		return STA_FAILED | STA_NOTFOUND;
	unsigned cyl, head, sector;
	if (!lsn_chs(disk, lsn, ss, &cyl, &head, &sector))
		return STA_FAILED | STA_NOTFOUND;
	// vdisk_read_sector() doesn't check the data CRC reliably, so CRC
	// errors aren't reported.
	struct vdisk_ctx *ctx = vdisk_ctx_new(disk);
	bool ok = vdisk_read_sector(ctx, cyl, head, sector, 256, d->buf);
	vdisk_ctx_free(ctx);
	return ok ? 0 : (STA_FAILED | STA_NOTFOUND);
}

static uint8_t write_lsn(struct cocosdc *d, unsigned drive, unsigned lsn, bool ss) {
	if (d->drv[drive].raw)
		return raw_write(d, drive, lsn);
	struct vdisk *disk = drive_disk(d, drive);
	if (!disk)
		return STA_FAILED | STA_NOTFOUND;
	if (disk->write_protect)
		return STA_FAILED | STA_INUSE;
	unsigned cyl, head, sector;
	if (!lsn_chs(disk, lsn, ss, &cyl, &head, &sector))
		return STA_FAILED | STA_NOTFOUND;
	struct vdisk_ctx *ctx = vdisk_ctx_new(disk);
	bool ok = vdisk_write_sector(ctx, cyl, head, sector, 256, d->buf);
	vdisk_ctx_free(ctx);
	return ok ? 0 : (STA_FAILED | STA_NOTFOUND);
}

// Host path of the file in a drive.  Disk images may also have been
// inserted some other way (-load-fd0, the UI), so their path comes from the
// drive.

static const char *drive_path(struct cocosdc *d, unsigned drive) {
	if (d->drv[drive].raw)
		return d->drv[drive].path;
	struct vdisk *disk = drive_disk(d, drive);
	return disk ? disk->filename : NULL;
}

static void unmount(struct cocosdc *d, unsigned drive) {
	if ((d->sd_block >> 24) == drive) {
		sd_flush(d);
		d->sd_block = SD_BLOCK_NONE;
	}
	if (d->drv[drive].written) {
		// Update the file's directory entry
		d->delay += T_SD_READ + T_SD_WRITE;
		d->drv[drive].written = 0;
	}
	if (d->drv[drive].raw) {
		fclose(d->drv[drive].raw);
		d->drv[drive].raw = NULL;
	}
	sdsfree(d->drv[drive].path);
	d->drv[drive].path = NULL;
	if (drive_disk(d, drive))
		xroar_eject_disk(drive);
	if (drive == 0) {
		sdsfree(d->set_dir);
		d->set_dir = NULL;
		d->set_num = 0;
	}
}

// Fill a newly created disk image.  $FF4A gives the number of sides: zero
// makes a blank 35 track single sided DSK (630 sectors), one or two an SDF
// with the number of tracks in $FF49 and no sectors, ready to be formatted.

static uint8_t new_image(struct cocosdc *d, FILE *f) {
	unsigned sides = d->param[1];
	unsigned tracks = d->param[0];
	if (sides == 0) {
		uint8_t blank[256];
		memset(blank, 0xff, sizeof(blank));
		for (unsigned i = 0; i < 35 * SDC_SECTORS_PER_TRACK; i++)
			fwrite(blank, 1, sizeof(blank), f);
		// Every block of the image: that takes the real controller
		// most of a second
		d->delay += (35 * SDC_SECTORS_PER_TRACK / 2) * T_SD_WRITE;
		return 0;
	}
	// The firmware also accepts zero tracks, with odd results
	if (sides > 2 || tracks < 1 || tracks > 240)
		return STA_FAILED | STA_CRC;
	// The header, then a record for each track with an empty sector
	// table.  The real controller only writes the table and the last
	// block of each record, leaving whatever the card held in between.
	uint8_t rec[6656];
	memset(rec, 0, sizeof(rec));
	memcpy(rec, "SDF1", 4);
	rec[4] = tracks;
	rec[5] = sides;
	fwrite(rec, 1, 512, f);
	memset(rec, 0, 8);
	for (unsigned i = 0; i < tracks * sides; i++)
		fwrite(rec, 1, sizeof(rec), f);
	d->delay += (2 * tracks * sides + 1) * T_SD_WRITE;
	return 0;
}

// Disk sets: mounting a directory puts the disk images inside it named like
// "FOOn.DSK", n from 1 to 9, in drive 0.  The '#' command then selects a
// disk by number, and '+' the next one.

// Find a member of a disk set: an image whose name ends with the digit ch.
// The first in name order wins.  Returns a new sds, or NULL.

static sds set_member(const char *dir, char ch) {
	DIR *dh = opendir(dir);
	if (!dh)
		return NULL;
	struct sdc_dirent best = {0};
	sds found = NULL;
	struct dirent *de;
	while ((de = readdir(dh))) {
		struct sdc_dirent e = {0};
		if (de->d_name[0] == '.' || !name_83(de->d_name, e.name, e.ext))
			continue;
		int i = 7;
		while (i > 0 && e.name[i] == ' ')
			i--;
		if (e.name[i] != ch)
			continue;
		if (found && dirent_cmp(&e, &best) >= 0)
			continue;
		sds p = sdscatfmt(sdsnew(dir), "/%s", de->d_name);
		struct stat st;
		if (stat(p, &st) != 0 || S_ISDIR(st.st_mode)) {
			sdsfree(p);
			continue;
		}
		sdsfree(found);
		found = p;
		best = e;
	}
	closedir(dh);
	return found;
}

// Mount a disk of the set in drive 0: disk number mode (1-9), or if that's
// zero, the next disk that exists after the current one.  If the requested
// disk isn't there, the previous one is mounted again, but the command still
// fails.  If nothing could be mounted, the set is forgotten.

static uint8_t set_select(struct cocosdc *d, unsigned mode) {
	if (!d->set_dir || mode > 9)
		return STA_FAILED | STA_NOTFOUND;
	// Unmounting drive 0 forgets the set
	sds dir = d->set_dir;
	d->set_dir = NULL;
	unsigned num = d->set_num;
	unsigned tries = mode ? 2 : 9;
	// Remounting the same image doesn't read its first block again
	sds old = drive_path(d, 0) ? sdsnew(drive_path(d, 0)) : NULL;
	do {
		unmount(d, 0);
		char ch;
		if (mode) {
			ch = '0' + mode;
		} else {
			if (++num >= 10)
				num = 1;
			ch = '0' + num;
		}
		d->delay += T_SCAN;
		sds host = set_member(dir, ch);
		const char *other = drive_path(d, 1);
		if (host && !(other && strcmp(other, host) == 0)) {
			if (!old || strcmp(old, host) != 0)
				d->delay += T_SD_READ;
			sd_flush(d);
			xroar_insert_disk_file(0, host);
		}
		sdsfree(host);
		if (drive_disk(d, 0)) {
			// The set's directory, holding the image's entry, is
			// left in the buffer
			d->sd_block = SD_BLOCK_ENTRY(0);
			sdsfree(old);
			d->set_dir = dir;
			d->set_num = ch - '0';
			return (mode && tries == 1) ? (STA_FAILED | STA_NOTFOUND) : 0;
		}
		if (mode && num)
			mode = num;
	} while (--tries);
	sdsfree(old);
	sdsfree(dir);
	return STA_FAILED | STA_NOTFOUND;
}

// Mount the disk set in directory host (consumed).  Only drive 0 can hold a
// set, and only "M:" mounts one.

static uint8_t mount_set(struct cocosdc *d, unsigned drive, sds host, bool not_m) {
	if (not_m) {
		sdsfree(host);
		return STA_FAILED | STA_INUSE;
	}
	unmount(d, drive);
	if (drive != 0) {
		sdsfree(host);
		return STA_FAILED | STA_NOTFOUND;
	}
	d->set_dir = host;
	d->set_num = 0;
	// Reading the directory, which then stays in the MCU's buffer
	d->delay += T_SD_READ;
	return set_select(d, 0);
}

// Mount a file: as a disk image through the floppy drives, or as raw blocks
// (raw files can't be accessed through FDC emulation).  If create is set,
// the file is created first if it doesn't exist (see new_image()).

static uint8_t mount_file(struct cocosdc *d, unsigned drive, const char *path, bool raw, bool create) {
	if (!*path) {
		unmount(d, drive);
		return 0;
	}
	d->delay += T_DIR;
	// "M:" takes the first match of wildcards, and if nothing matches,
	// tries again with a missing extension matching any.  "N:" needs the
	// exact name, and creates the file if it's not found.
	sds host;
	if (create) {
		host = resolve_path(d, path, RP_NEW, NULL);
	} else {
		host = resolve_path(d, path, RP_WILD, NULL);
		if (!host && d->path_status == (STA_FAILED | STA_NOTFOUND))
			host = resolve_path(d, path, RP_WILD | RP_ANYEXT, NULL);
	}
	if (!host)
		return d->path_status;
	struct stat st;
	bool created = 0;
	if (stat(host, &st) != 0) {
		if (!create) {
			sdsfree(host);
			return STA_FAILED | STA_NOTFOUND;
		}
		FILE *f = fopen(host, "wb");
		if (!f) {
			sdsfree(host);
			return STA_FAILED | STA_INVALID;
		}
		// The new directory entry
		d->delay += T_SD_WRITE;
		uint8_t r = raw ? 0 : new_image(d, f);
		fclose(f);
		created = !raw;
		if (r) {
			// Like the real controller, this leaves the empty file
			sdsfree(host);
			return r;
		}
	} else if (S_ISDIR(st.st_mode)) {
		return mount_set(d, drive, host, raw || create);
	}
	const char *other = drive_path(d, drive ^ 1);
	if (other && strcmp(other, host) == 0) {
		sdsfree(host);
		return STA_FAILED | STA_INUSE;
	}
	unmount(d, drive);
	// Mounting leaves the file's first block in the MCU's buffer
	sd_flush(d);
	d->sd_block = drive << 24;
	if (raw) {
		bool ro = 0;
		FILE *f = fopen(host, "r+b");
		if (!f) {
			f = fopen(host, "rb");
			ro = 1;
		}
		if (!f) {
			sdsfree(host);
			return STA_FAILED | STA_NOTFOUND;
		}
		d->drv[drive].raw = f;
		d->drv[drive].raw_ro = ro;
		d->drv[drive].path = host;
		return 0;
	}
	xroar_insert_disk_file(drive, host);
	sdsfree(host);
	if (!drive_disk(d, drive))
		return STA_FAILED | STA_INVALID;
	if (created) {
		// The firmware writes the new image's first block last, and
		// its directory entry needs the final size
		d->sd_dirty = 1;
		d->drv[drive].written = 1;
	}
	return 0;
}

// STARTUP.CFG in the SD card's root directory lists images for the
// controller to mount at power up: "0=path" and "1=path" for the drives and
// "D=path" for the current directory.  Drives that already hold an image
// (given on the command line, say) are left alone.

static uint8_t set_dir(struct cocosdc *d, const char *path);

static void load_startup_cfg(struct cocosdc *d) {
	sds host = resolve_path(d, "/STARTUP.CFG", 0, NULL);
	if (!host)
		return;
	FILE *f = fopen(host, "rb");
	sdsfree(host);
	if (!f)
		return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == 0 || line[1] != '=')
			continue;
		const char *arg = line + 2;
		switch (toupper((unsigned char)line[0])) {
		case '0': case '1': {
			unsigned drive = line[0] - '0';
			if (!drive_path(d, drive) && mount_file(d, drive, arg, 0, 0) == 0)
				LOG_MOD_DEBUG(1, "cocosdc", "STARTUP.CFG: drive %u = %s\n", drive, arg);
			break;
		}
		case 'D':
			set_dir(d, arg);
			break;
		default:
			break;
		}
	}
	fclose(f);
}

static uint8_t set_dir(struct cocosdc *d, const char *path) {
	d->delay += T_DIR;
	sds rel = NULL;
	// Wildcards are allowed, but the first match must be a directory
	sds host = resolve_path(d, *path ? path : "/", RP_WILD, &rel);
	if (!host)
		return d->path_status;
	struct stat st;
	bool ok = stat(host, &st) == 0 && S_ISDIR(st.st_mode);
	sdsfree(host);
	if (!ok) {
		sdsfree(rel);
		return STA_FAILED | STA_NOTFOUND;
	}
	sdsfree(d->cur_dir);
	d->cur_dir = rel;
	return 0;
}

// Rename a file or directory.  Both path names are relative to the current
// directory, so a file can also be moved.  As on FAT, the new name must be a
// valid 8.3 name that doesn't exist already, and a mounted file can't be
// renamed.

static uint8_t rename_file(struct cocosdc *d, const char *from, const char *to) {
	d->delay += 2 * T_DIR + T_SD_WRITE;
	sds old = resolve_path(d, from, 0, NULL);
	uint8_t old_status = d->path_status;
	sds new = resolve_path(d, to, RP_NEW, NULL);
	char name[8], ext[3];
	struct stat st;
	uint8_t r = 0;
	if (!old) {
		r = old_status;
	} else if (!new) {
		r = d->path_status;
	} else if (!name_83(strrchr(new, '/') + 1, name, ext)) {
		r = STA_FAILED | STA_INVALID;
	} else if (strcmp(old, new) == 0) {
		// Same name (resolved case-insensitively): nothing to do
	} else if (stat(new, &st) == 0) {
		r = STA_FAILED | STA_INUSE;
	} else {
		for (unsigned i = 0; i < 2; i++) {
			const char *path = drive_path(d, i);
			if (path && strcmp(path, old) == 0)
				r = STA_FAILED | STA_INUSE;
		}
		if (!r && rename(old, new) != 0)
			r = STA_FAILED | STA_NOTFOUND;
	}
	sdsfree(old);
	sdsfree(new);
	return r;
}

static uint8_t sd_command(struct cocosdc *d, unsigned drive) {
	char *arg = (char *)d->buf + 2;
	d->buf[255] = 0;
	if (!d->sd_root)
		return STA_FAILED | STA_NOTFOUND;
	if (d->buf[1] != ':')
		return STA_FAILED | STA_INVALID;
	switch (d->buf[0]) {
	case 'M': case 'm':
		return mount_file(d, drive, arg, d->buf[0] == 'm', 0);
	case 'N': case 'n':
		return mount_file(d, drive, arg, d->buf[0] == 'n', 1);
	case 'D':
		return set_dir(d, arg);
	case 'L':
		d->delay += T_LIST;
		return start_listing(d, arg);
	case 'K': {
		// As in the firmware, an existing directory is fine, but not
		// a file of that name
		d->delay += T_DIR;
		sds host = resolve_path(d, arg, RP_NEW, NULL);
		struct stat st;
		uint8_t r;
		if (!host) {
			r = d->path_status;
		} else if (stat(host, &st) == 0) {
			r = S_ISDIR(st.st_mode) ? 0 : (STA_FAILED | STA_INUSE);
		} else {
			r = (mkdir(host, 0777) == 0) ? 0 : (STA_FAILED | STA_NOTFOUND);
			// Zeroing the new cluster dominates, then the entry, the
			// FAT and the "." and ".." entries
			if (!r)
				d->delay += SD_CLUSTER_BLOCKS * T_SD_WRITE + 2 * (T_SD_READ + T_SD_WRITE) + T_SD_WRITE;
		}
		sdsfree(host);
		return r;
	}
	case 'R': {
		// The new name follows the old one's terminating NUL
		const char *to = arg + strlen(arg) + 1;
		if (to >= (char *)d->buf + 256)
			return STA_FAILED | STA_INVALID;
		return rename_file(d, arg, to);
	}
	case 'X': {
		d->delay += T_DIR + T_SD_WRITE;
		sds host = resolve_path(d, arg, 0, NULL);
		uint8_t r = !host ? d->path_status : (remove(host) == 0) ? 0 : (STA_FAILED | STA_NOTFOUND);
		sdsfree(host);
		return r;
	}
	default:
		LOG_MOD_DEBUG(1, "cocosdc", "unsupported command '%c:'\n", d->buf[0]);
		return STA_FAILED | STA_INVALID;
	}
}

// Start sending a block to the CoCo, or finish with a status.

static void send_block(struct cocosdc *d, uint8_t status) {
	if (status) {
		d->status = status;
		d->buf_count = 0;
		return;
	}
	d->buf_pos = 0;
	d->buf_count = 256;
	d->buf_rx = 0;
	d->status = STA_BUSY | STA_READY;
}

static void ext_command(struct cocosdc *d, unsigned drive) {
	switch (d->param[0]) {
	case 'I': {
		// Info for mounted image
		d->delay = T_INFO;
		const char *path = drive_path(d, drive);
		if (!path) {
			d->status = STA_FAILED | STA_NOTFOUND;
			return;
		}
		sd_entry(d, drive);
		memset(d->buf, 0, 256);
		fat_record(d->buf, path);
		if (drive_sdf(d, drive))
			d->buf[11] |= 0x04;
		send_block(d, 0);
		return;
	}
	case 'Q': {
		// Size of mounted image in sectors: the file size / 256
		d->delay = T_INFO;
		const char *path = drive_path(d, drive);
		struct stat st;
		if (!path || stat(path, &st) != 0) {
			d->status = STA_FAILED | STA_NOTFOUND;
			return;
		}
		sd_entry(d, drive);
		unsigned n = st.st_size / 256;
		d->param[0] = n >> 16;
		d->param[1] = n >> 8;
		d->param[2] = n;
		d->status = 0;
		return;
	}
	case 'C': {
		// Current directory leaf
		d->delay = T_INFO;
		memset(d->buf, 0, 256);
		if (sdslen(d->cur_dir) == 0) {
			send_block(d, 0);
			d->status |= STA_FAILED | STA_NOTFOUND;
			return;
		}
		sds host = sdscatfmt(sdsnew(d->sd_root ? d->sd_root : ""), "/%S", d->cur_dir);
		fat_record(d->buf, host);
		sdsfree(host);
		send_block(d, 0);
		return;
	}
	case '>':
		d->delay = T_DIRPAGE;
		if (!d->listing) {
			d->status = STA_FAILED;
			return;
		}
		send_block(d, listing_page(d, d->buf));
		return;
	case 'e':
		// Undocumented, sent by SDC-DOS before opening files: response
		// byte 3 flags the drives (bit per drive select line) it should
		// access through FDC emulation instead of logical sectors.
		// Those are the drives holding SDF images.
		d->delay = T_EXT;
		d->param[2] = (drive_sdf(d, 0) ? 1 : 0) | (drive_sdf(d, 1) ? 2 : 0);
		d->status = 0;
		return;
	case 'g':
		// Undocumented: update the MCU option byte, flags = (flags AND
		// byte 2) OR byte 3.  SDC-DOS clears bit 0 for RUN @n and sets
		// it for RUN @n,R before switching bank.  The firmware tests bit
		// 7 in its FDC emulation; nothing here depends on either bit.
		d->delay = T_EXT;
		d->flags = (d->flags & d->param[1]) | d->param[2];
		d->status = 0;
		return;
	case '+':
		// Next disk of the set, then blink its number on the LED
		d->param[1] = 0;
		d->param[2] = 1;
		// fall through
	case '#': {
		// Disk of the set given in byte 2 (0 for the next one), the LED
		// blinking its number if bit 0 of byte 3 is set
		d->delay = T_EXT;
		d->status = (drive == 0) ? set_select(d, d->param[1]) : (STA_FAILED | STA_NOTFOUND);
		if (!d->status && (d->param[2] & 1))
			d->delay += d->set_num * T_BLINK;
		return;
	}
	case 'V':
		d->delay = T_EXT;
		d->param[1] = SDC_VERSION >> 8;
		d->param[2] = SDC_VERSION & 0xff;
		d->status = 0;
		return;
	default:
		LOG_MOD_DEBUG(1, "cocosdc", "unsupported extended command $%02x '%c'\n", d->param[0], d->param[0]);
		d->status = STA_FAILED;
		return;
	}
}

// A block sent by the CoCo has been received.

static void block_received(struct cocosdc *d) {
	unsigned drive = d->cmd & 1;
	unsigned lsn = (d->param[0] << 16) | (d->param[1] << 8) | d->param[2];
	d->delay = 0;
	switch (d->cmd & 0xf0) {
	case 0xa0:
		d->delay = T_WRITE;
		d->status = write_lsn(d, drive, lsn, d->cmd & 2);
		if (!d->status)
			sd_access(d, drive, lsn, 1);
		break;
	case 0xe0:
		d->status = sd_command(d, drive);
		break;
	default:
		d->status = 0;
		break;
	}
	defer(d);
}

// Streaming: the mounted file is read continuously in 512 byte blocks (the SD
// card's native size), starting at the block given in the parameters.  BUSY
// stays set while the stream lasts and READY marks each block; a short last
// block is padded with zeroes.  The next block is ready shortly after the last
// byte of a block is read.  The stream ends at the end of the file (BUSY
// clears), on command $D0, or when command mode is left.

static void stop_stream(struct cocosdc *d) {
	if (d->stream) {
		fclose(d->stream);
		d->stream = NULL;
	}
}

static bool next_stream_block(struct cocosdc *d) {
	memset(d->sbuf, 0, sizeof(d->sbuf));
	if (fread(d->sbuf, 1, sizeof(d->sbuf), d->stream) == 0) {
		stop_stream(d);
		d->status = 0;
		return 0;
	}
	d->spos = 0;
	d->status = STA_BUSY | STA_READY;
	return 1;
}

static void start_stream(struct cocosdc *d, unsigned drive, unsigned block, bool byte_mode) {
	stop_stream(d);
	const char *path = drive_path(d, drive);
	FILE *f = path ? fopen(path, "rb") : NULL;
	if (!f || fseeko(f, (off_t)block * 512, SEEK_SET) != 0) {
		if (f)
			fclose(f);
		d->status = STA_FAILED;
		return;
	}
	d->stream = f;
	d->byte_mode = byte_mode;
	// Streaming uses the MCU's buffer
	sd_flush(d);
	d->sd_block = SD_BLOCK_NONE;
	d->delay += T_STREAM1;
	next_stream_block(d);
}

static void command(struct cocosdc *d, uint8_t cmd) {
	unsigned drive = cmd & 1;
	unsigned lsn = (d->param[0] << 16) | (d->param[1] << 8) | d->param[2];
	bool streaming = d->stream != NULL;
	d->cmd = cmd;
	d->buf_count = 0;
	d->delay = 0;
	event_dequeue(&d->ready_event);
	stop_stream(d);
	LOG_MOD_DEBUG(3, "cocosdc", "command $%02x params $%02x $%02x $%02x\n", cmd, d->param[0], d->param[1], d->param[2]);
	switch (cmd & 0xf0) {
	case 0x80:
		d->byte_mode = cmd & 4;
		d->delay = T_HIT;
		send_block(d, read_lsn(d, drive, lsn, cmd & 2));
		if (!(d->status & STA_FAILED))
			sd_access(d, drive, lsn, 0);
		break;
	case 0xa0:
	case 0xe0:
		d->byte_mode = (cmd & 0xf0) == 0xa0 && (cmd & 8);
		d->buf_pos = 0;
		d->buf_count = 256;
		d->buf_rx = 1;
		d->status = STA_BUSY | STA_READY;
		d->delay = T_ACCEPT;
		break;
	case 0xc0:
		d->byte_mode = 0;
		ext_command(d, drive);
		break;
	case 0x90:
		start_stream(d, drive, lsn, cmd & 4);
		break;
	case 0xd0:
		// After aborting a stream, the real controller reports $08
		d->status = streaming ? STA_CRC : 0;
		d->delay = streaming ? T_ABORT : 0;
		break;
	default:
		LOG_MOD_DEBUG(1, "cocosdc", "unsupported command $%02x\n", cmd);
		d->status = STA_FAILED;
		break;
	}
	defer(d);
}

static void cmd_write(struct cocosdc *d, unsigned reg, uint8_t D) {
	if (reg == 0) {
		command(d, D);
		return;
	}
	if (d->buf_count && d->buf_rx && (reg == 3 || (reg == 2 && !d->byte_mode))) {
		d->buf[d->buf_pos++] = D;
		if (--d->buf_count == 0)
			block_received(d);
		return;
	}
	d->param[reg - 1] = D;
}

static uint8_t cmd_read(struct cocosdc *d, unsigned reg) {
	if (reg == 0)
		return d->status;
	if (d->stream && (d->status & STA_READY) && (reg == 3 || (reg == 2 && !d->byte_mode))) {
		uint8_t v = d->sbuf[d->spos++];
		if (d->spos == sizeof(d->sbuf)) {
			d->delay = T_STREAM;
			next_stream_block(d);
			defer(d);
		}
		return v;
	}
	if (d->buf_count && !d->buf_rx && (reg == 3 || (reg == 2 && !d->byte_mode))) {
		uint8_t v = d->buf[d->buf_pos++];
		if (--d->buf_count == 0 && !(d->status & STA_FAILED))
			d->status = 0;
		else if (d->buf_count == 0)
			d->status &= ~(STA_BUSY | STA_READY);
		return v;
	}
	return d->param[reg - 1];
}
