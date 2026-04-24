/*
 * Some code borrowed from sg-utils.
 *
 * Copyright (c) 2004 Christophe Varoqui
 */
#include <sys/ioctl.h>
#include <sys/types.h>

#include <errno.h>

#include "checkers.h"
#include "async_checker.h"
#include "sg_include.h"

#define TUR_CMD_LEN 6
#define HEAVY_CHECK_COUNT       10

enum {
	MSG_TUR_TRANSITIONING = CHECKER_FIRST_MSGID,
};

#define IDX_(x) (MSG_ ## x - CHECKER_FIRST_MSGID)
const char *libcheck_msgtable[] = {
	[IDX_(TUR_TRANSITIONING)] = " reports path is transitioning",
	NULL,
};

int libcheck_async_func(struct runner_data *rdata)
{
	struct sg_io_hdr io_hdr;
	unsigned char turCmdBlk[TUR_CMD_LEN] = { 0x00, 0, 0, 0, 0, 0 };
	unsigned char sense_buffer[32];
	int retry_tur = 5;

retry:
	memset(&io_hdr, 0, sizeof (struct sg_io_hdr));
	memset(&sense_buffer, 0, 32);
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = sizeof (turCmdBlk);
	io_hdr.mx_sb_len = sizeof (sense_buffer);
	io_hdr.dxfer_direction = SG_DXFER_NONE;
	io_hdr.cmdp = turCmdBlk;
	io_hdr.sbp = sense_buffer;
	io_hdr.timeout = rdata->timeout * 1000;
	io_hdr.pack_id = 0;
	if (ioctl(rdata->fd, SG_IO, &io_hdr) < 0) {
		if (errno == ENOTTY) {
			rdata->msgid = CHECKER_MSGID_UNSUPPORTED;
			return PATH_WILD;
		}
		rdata->msgid = CHECKER_MSGID_DOWN;
		return PATH_DOWN;
	}
	if ((io_hdr.status & 0x7e) == 0x18) {
		/*
		 * SCSI-3 arrays might return
		 * reservation conflict on TUR
		 */
		rdata->msgid = CHECKER_MSGID_UP;
		return PATH_UP;
	}
	if (io_hdr.info & SG_INFO_OK_MASK) {
		int key = 0, asc, ascq;

		switch (io_hdr.host_status) {
		case DID_OK:
		case DID_NO_CONNECT:
		case DID_BAD_TARGET:
		case DID_ABORT:
		case DID_TRANSPORT_FAILFAST:
			break;
		default:
			/* Driver error, retry */
			if (--retry_tur)
				goto retry;
			break;
		}
		if (io_hdr.sb_len_wr > 3) {
			if (io_hdr.sbp[0] == 0x72 || io_hdr.sbp[0] == 0x73) {
				key = io_hdr.sbp[1] & 0x0f;
				asc = io_hdr.sbp[2];
				ascq = io_hdr.sbp[3];
			} else if (io_hdr.sb_len_wr > 13 &&
				   ((io_hdr.sbp[0] & 0x7f) == 0x70 ||
				    (io_hdr.sbp[0] & 0x7f) == 0x71)) {
				key = io_hdr.sbp[2] & 0x0f;
				asc = io_hdr.sbp[12];
				ascq = io_hdr.sbp[13];
			}
		}
		if (key == 0x6) {
			/* Unit Attention, retry */
			if (--retry_tur)
				goto retry;
		}
		else if (key == 0x2) {
			/* Not Ready */
			/* Note: Other ALUA states are either UP or DOWN */
			if (asc == 0x04 && ascq == 0x0b) {
				/*
				 * LOGICAL UNIT NOT ACCESSIBLE,
				 * TARGET PORT IN STANDBY STATE
				 */
				rdata->msgid = CHECKER_MSGID_GHOST;
				return PATH_GHOST;
			} else if (asc == 0x04 && ascq == 0x0a) {
				/*
				 * LOGICAL UNIT NOT ACCESSIBLE,
				 * ASYMMETRIC ACCESS STATE TRANSITION
				 */
				rdata->msgid = MSG_TUR_TRANSITIONING;
				return PATH_PENDING;
			}
		} else if (key == 0x5) {
			/* Illegal request */
			if (asc == 0x25 && ascq == 0x00) {
				/*
				 * LUN NOT SUPPORTED: unmapped at target.
				 * Signals pp->disconnected, becomes PATH_DOWN.
				 */
				rdata->msgid = CHECKER_MSGID_DISCONNECTED;
				return PATH_DISCONNECTED;
			}
		}
		rdata->msgid = CHECKER_MSGID_DOWN;
		return PATH_DOWN;
	}
	rdata->msgid = CHECKER_MSGID_UP;
	return PATH_UP;
}
