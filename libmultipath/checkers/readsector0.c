/*
 * Copyright (c) 2004, 2005 Christophe Varoqui
 */
#include <sys/types.h>
#include "checkers.h"
#include "libsg.h"
#include "async_checker.h"

int libcheck_async_func(struct runner_data *rdata)
{
	unsigned char buf[4096];
	unsigned char sbuf[SENSE_BUFF_LEN];
	int ret;

	ret = sg_read(rdata->fd, &buf[0], 4096, &sbuf[0], SENSE_BUFF_LEN,
		      rdata->timeout);

	switch (ret)
	{
	case PATH_DOWN:
		rdata->msgid = CHECKER_MSGID_DOWN;
		break;
	case PATH_UP:
		rdata->msgid = CHECKER_MSGID_UP;
		break;
	default:
		break;
	}
	return ret;
}
