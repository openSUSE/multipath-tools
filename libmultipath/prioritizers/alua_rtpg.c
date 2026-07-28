/*
 * (C) Copyright IBM Corp. 2004, 2005   All Rights Reserved.
 *
 * rtpg.c
 *
 * Tool to make use of a SCSI-feature called Asymmetric Logical Unit Access.
 * It determines the ALUA state of a device and prints a priority value to
 * stdout.
 *
 * Author(s): Jan Kunigk
 *            S. Bader <shbader@de.ibm.com>
 *
 * This file is released under the GPL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <libudev.h>

#define __user
#include <scsi/sg.h>

#include "../structs.h"
#include "../prio.h"
#include "../discovery.h"
#include "alua_rtpg.h"

#define SENSE_BUFF_LEN  32
#define SGIO_TIMEOUT     60000

/*
 * Macro used to print debug messaged.
 */
#if DEBUG > 0
#define PRINT_DEBUG(f, a...) \
		fprintf(stderr, "DEBUG: " f, ##a)
#else
#define PRINT_DEBUG(f, a...)
#endif

/*
 * Optionally print the commands sent and the data received a hex dump.
 */
#if DEBUG > 0
#if DEBUG_DUMPHEX > 0
#define PRINT_HEX(p, l)	print_hex(p, l)
void
print_hex(unsigned char *p, unsigned long len)
{
	int	i;

	for(i = 0; i < len; i++) {
		if (i % 16 == 0)
			printf("%04x: ", i);
		printf("%02x%s", p[i], (((i + 1) % 16) == 0) ? "\n" : " ");
	}
	printf("\n");
}
#else
#define PRINT_HEX(p, l)
#endif
#else
#define PRINT_HEX(p, l)
#endif

/*
 * Returns 0 if the SCSI command either was successful or if the an error was
 * recovered, otherwise 1. (definitions taken from sg_err.h)
 */
#define SCSI_CHECK_CONDITION    0x2
#define SCSI_COMMAND_TERMINATED 0x22
#define SG_ERR_DRIVER_SENSE     0x08
#define RECOVERED_ERROR 0x01

static int
scsi_error(struct sg_io_hdr *hdr)
{
	/* Treat SG_ERR here to get rid of sg_err.[ch] */
	hdr->status &= 0x7e;

	if (
		(hdr->status == 0)        &&
		(hdr->host_status == 0)   &&
		(hdr->driver_status == 0)
	) {
		return 0;
	}

	if (
		(hdr->status == SCSI_CHECK_CONDITION)    ||
		(hdr->status == SCSI_COMMAND_TERMINATED) ||
		((hdr->driver_status & 0xf) == SG_ERR_DRIVER_SENSE)
	) {
		if (hdr->sbp && (hdr->sb_len_wr > 2)) {
			int		sense_key;
			unsigned char *	sense_buffer = hdr->sbp;

			if (sense_buffer[0] & 0x2)
				sense_key = sense_buffer[1] & 0xf;
			else
				sense_key = sense_buffer[2] & 0xf;

			if (sense_key == RECOVERED_ERROR)
				return 0;
		}
	}

	return 1;
}

/*
 * Helper function to setup and run a SCSI inquiry command.
 */
int
do_inquiry(int fd, int evpd, unsigned int codepage,
	   void *resp, int resplen, unsigned int timeout)
{
	struct inquiry_command	cmd;
	struct sg_io_hdr	hdr;
	unsigned char		sense[SENSE_BUFF_LEN];

	memset(&cmd, 0, sizeof(cmd));
	cmd.op = OPERATION_CODE_INQUIRY;
	if (evpd) {
		inquiry_command_set_evpd(&cmd);
		cmd.page = codepage;
	}
	set_uint16(cmd.length, resplen);
	PRINT_HEX((unsigned char *) &cmd, sizeof(cmd));

	memset(&hdr, 0, sizeof(hdr));
	hdr.interface_id	= 'S';
	hdr.cmdp		= (unsigned char *) &cmd;
	hdr.cmd_len		= sizeof(cmd);
	hdr.dxfer_direction	= SG_DXFER_FROM_DEV;
	hdr.dxferp		= resp;
	hdr.dxfer_len		= resplen;
	hdr.sbp			= sense;
	hdr.mx_sb_len		= sizeof(sense);
	hdr.timeout		= get_prio_timeout(timeout, SGIO_TIMEOUT);

	if (ioctl(fd, SG_IO, &hdr) < 0) {
		PRINT_DEBUG("do_inquiry: IOCTL failed!\n");
		return -RTPG_INQUIRY_FAILED;
	}

	if (scsi_error(&hdr)) {
		PRINT_DEBUG("do_inquiry: SCSI error!\n");
		return -RTPG_INQUIRY_FAILED;
	}
	if (hdr.resid < 0 || (unsigned int)hdr.resid > hdr.dxfer_len)
		/* resid failed sanity check*/
		return -RTPG_RTPG_FAILED;
	PRINT_HEX((unsigned char *) resp, resplen);

	return (int)hdr.dxfer_len - hdr.resid;
}

/*
 * This function returns the support for target port groups by evaluating the
 * data returned by the standard inquiry command.
 */
int
get_target_port_group_support(int fd, unsigned int timeout)
{
	struct inquiry_data	inq;
	int			rc;

	memset((unsigned char *)&inq, 0, sizeof(inq));
	rc = do_inquiry(fd, 0, 0x00, &inq, sizeof(inq), timeout);
	if (rc > (int)offsetof(struct inquiry_data, b5))
		return inquiry_data_get_tpgs(&inq);
	else
		return -RTPG_INQUIRY_FAILED;
}

int
get_target_port_group(struct path * pp, unsigned int timeout)
{
	unsigned char *buf;
	struct vpd83_dscr *	dscr;
	int rc, data_len;
	int			buflen, scsi_buflen;

	buflen = VPD_BUFLEN;
	buf = (unsigned char *)malloc(buflen);
	if (!buf) {
		PRINT_DEBUG("malloc failed: could not allocate"
			     "%u bytes\n", buflen);
		return -RTPG_RTPG_FAILED;
	}

	memset(buf, 0, buflen);
	rc = do_inquiry(pp->fd, 1, 0x83, buf, buflen, timeout);
	if (rc < 0)
		goto out;

	if (rc < 4) {
		PRINT_DEBUG("do_inquiry only returned %d bytes", rc);
		rc = -RTPG_RTPG_FAILED;
		goto out;
	}
	scsi_buflen = (buf[2] << 8 | buf[3]) + 4;
	if (scsi_buflen > VPD_BUFLEN)
		scsi_buflen = VPD_BUFLEN;
	if (rc < scsi_buflen) {
		free(buf);
		buf = (unsigned char *)malloc(scsi_buflen);
		if (!buf) {
			PRINT_DEBUG("malloc failed: could not allocate"
				    "%u bytes\n", scsi_buflen);
			return -RTPG_RTPG_FAILED;
		}
		buflen = scsi_buflen;
		memset(buf, 0, buflen);
		rc = do_inquiry(pp->fd, 1, 0x83, buf, buflen, timeout);
		if (rc < 0)
			goto out;
	}

	data_len = rc;
	if (data_len < scsi_buflen)
		PRINT_DEBUG("inquiry data trucated. Read %d of %d bytes",
			    data_len, scsi_buflen);
	rc = -RTPG_NO_TPG_IDENTIFIER;
	for (dscr = (struct vpd83_dscr *)&buf[4];
	     (const unsigned char *)(dscr + 1) <= buf + data_len &&
	     dscr->data + dscr->length <= buf + data_len;
	     dscr = (struct vpd83_dscr *)(dscr->data + dscr->length)) {
		if (vpd83_dscr_istype(dscr, IDTYPE_TARGET_PORT_GROUP)) {
			struct vpd83_tpg_dscr *p;
			if (rc != -RTPG_NO_TPG_IDENTIFIER) {
				PRINT_DEBUG("get_target_port_group: more "
					    "than one TPG identifier found!\n");
				continue;
			}
			if (dscr->length < sizeof(struct vpd83_tpg_dscr)) {
				PRINT_DEBUG("%s: descr->length too small. "
					    "got %u. need %zu",
					    __func__, dscr->length,
					    sizeof(struct vpd83_tpg_dscr));
				continue;
			}
			p  = (struct vpd83_tpg_dscr *)dscr->data;
			rc = get_uint16(p->tpg);
		}
	}

	if (rc == -RTPG_NO_TPG_IDENTIFIER) {
		PRINT_DEBUG("get_target_port_group: "
			    "no TPG identifier found!\n");
	}
out:
	free(buf);
	return rc;
}

int do_rtpg(int fd, void *resp, long resplen, unsigned int timeout,
	    unsigned int *len_p)
{
	struct rtpg_command	cmd;
	struct sg_io_hdr	hdr;
	unsigned char		sense[SENSE_BUFF_LEN];

	memset(&cmd, 0, sizeof(cmd));
	cmd.op			= OPERATION_CODE_RTPG;
	rtpg_command_set_service_action(&cmd);
	set_uint32(cmd.length, resplen);
	PRINT_HEX((unsigned char *) &cmd, sizeof(cmd));

	memset(&hdr, 0, sizeof(hdr));
	hdr.interface_id	= 'S';
	hdr.cmdp		= (unsigned char *) &cmd;
	hdr.cmd_len		= sizeof(cmd);
	hdr.dxfer_direction	= SG_DXFER_FROM_DEV;
	hdr.dxferp		= resp;
	hdr.dxfer_len		= resplen;
	hdr.mx_sb_len		= sizeof(sense);
	hdr.sbp			= sense;
	hdr.timeout		= get_prio_timeout(timeout, SGIO_TIMEOUT);

	if (ioctl(fd, SG_IO, &hdr) < 0)
		return -RTPG_RTPG_FAILED;

	if (scsi_error(&hdr)) {
		PRINT_DEBUG("do_rtpg: SCSI error!\n");
		return -RTPG_RTPG_FAILED;
	}
	if (hdr.resid < 0 || (unsigned int)hdr.resid > hdr.dxfer_len)
		/* resid failed sanity check*/
		return -RTPG_RTPG_FAILED;
	if (len_p)
		*len_p = hdr.dxfer_len - hdr.resid;
	PRINT_HEX(resp, resplen);

	return 0;
}

int
get_asymmetric_access_state(int fd, unsigned int tpg, unsigned int timeout)
{
	unsigned char *buf;
	struct rtpg_tpg_dscr *	dscr;
	int			rc;
	unsigned int buflen, data_len;
	uint64_t		scsi_buflen;

	buflen = VPD_BUFLEN;
	buf = (unsigned char *)malloc(buflen);
	if (!buf) {
		PRINT_DEBUG ("malloc failed: could not allocate"
			"%u bytes\n", buflen);
		return -RTPG_RTPG_FAILED;
	}
	memset(buf, 0, buflen);
	rc = do_rtpg(fd, buf, buflen, timeout, &data_len);
	if (rc < 0) {
		PRINT_DEBUG("%s: do_rtpg returned %d", __func__, rc);
		goto out;
	}
	if (data_len < 4) {
		PRINT_DEBUG("do_rtpg only returned %u bytes", data_len);
		rc = -RTPG_RTPG_FAILED;
		goto out;
	}
	scsi_buflen = get_uint32(&buf[0]) + 4;
	if (scsi_buflen > UINT_MAX)
		scsi_buflen = UINT_MAX;
	if (buflen < scsi_buflen) {
		free(buf);
		buf = (unsigned char *)malloc(scsi_buflen);
		if (!buf) {
			PRINT_DEBUG ("malloc failed: could not allocate"
				"%u bytes\n", scsi_buflen);
			return -RTPG_RTPG_FAILED;
		}
		buflen = scsi_buflen;
		memset(buf, 0, buflen);
		rc = do_rtpg(fd, buf, buflen, timeout, &data_len);
		if (rc < 0)
			goto out;
	}
	if ((uint64_t)data_len < get_uint32(&buf[0]) + 4)
		PRINT_DEBUG("rtpg data truncated. Read %u of %" PRIu64 " bytes",
			    data_len, (uint64_t)get_uint32(&buf[0]) + 4);

	rc   = -RTPG_TPG_NOT_FOUND;
	for (dscr = (struct rtpg_tpg_dscr *)&buf[4];
	     (unsigned char *)(dscr + 1) <= buf + data_len &&
	     (unsigned char *)(dscr->data + dscr->port_count) <= buf + data_len;
	     dscr = (struct rtpg_tpg_dscr *)(dscr->data + dscr->port_count)) {
		if (get_uint16(dscr->tpg) == tpg) {
			if (rc != -RTPG_TPG_NOT_FOUND) {
				PRINT_DEBUG("get_asymmetric_access_state: "
					"more than one entry with same port "
					"group.\n");
			} else {
				PRINT_DEBUG("pref=%i\n", dscr->b0);
				rc = rtpg_tpg_dscr_get_aas(dscr);
			}
		}
	}
out:
	free(buf);
	return rc;
}
