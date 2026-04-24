// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *****************************************************************************
 *                                                                           *
 *     (C)  Copyright 2007 Hewlett-Packard Development Company, L.P          *
 *                                                                           *
 *****************************************************************************
*/

/*
 *  This program originally derived from and inspired by
 *  Christophe Varoqui's tur.c, part of libchecker.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "checkers.h"
#include "async_checker.h"

#include "cciss.h"

#define TUR_CMD_LEN 6
#define HEAVY_CHECK_COUNT       10

int libcheck_async_func(struct runner_data *rdata)
{
	int rc;
	int ret;
	unsigned int lun = 0;
	LogvolInfo_struct    lvi;       // logical "volume" info
	IOCTL_Command_struct cic;       // cciss ioctl command

	if ((rdata->fd) < 0) {
		rdata->msgid = CHECKER_MSGID_NO_FD;
		ret = -1;
		goto out;
	}

	rc = ioctl(rdata->fd, CCISS_GETLUNINFO, &lvi);
	if ( rc != 0) {
		perror("Error: ");
		fprintf(stderr, "cciss TUR  failed in CCISS_GETLUNINFO: %s\n",
			strerror(errno));
		rdata->msgid = CHECKER_MSGID_DOWN;
		ret = PATH_DOWN;
		goto out;
	} else {
		lun = lvi.LunID;
	}

	memset(&cic, 0, sizeof(cic));
	cic.LUN_info.LogDev.VolId = lun & 0x3FFFFFFF;
	cic.LUN_info.LogDev.Mode = 0x01; /* logical volume addressing */
	cic.Request.CDBLen = 6;  /* need to try just 2 bytes here */
	cic.Request.Type.Type =  TYPE_CMD; // It is a command.
	cic.Request.Type.Attribute = ATTR_SIMPLE;
	cic.Request.Type.Direction = XFER_NONE;
	cic.Request.Timeout = 0;

	cic.Request.CDB[0] = 0;
	cic.Request.CDB[1] = 0;
	cic.Request.CDB[2] = 0;
	cic.Request.CDB[3] = 0;
	cic.Request.CDB[4] = 0;
	cic.Request.CDB[5] = 0;

	rc = ioctl(rdata->fd, CCISS_PASSTHRU, &cic);
	if (rc < 0) {
		fprintf(stderr, "cciss TUR  failed: %s\n",
			strerror(errno));
		rdata->msgid = CHECKER_MSGID_DOWN;
		ret = PATH_DOWN;
		goto out;
	}

	if ((cic.error_info.CommandStatus | cic.error_info.ScsiStatus )) {
		rdata->msgid = CHECKER_MSGID_DOWN;
		ret = PATH_DOWN;
		goto out;
	}

	rdata->msgid = CHECKER_MSGID_UP;

	ret = PATH_UP;
out:
	return(ret);
}
