/*
 * Original author : tridge@samba.org, January 2002
 *
 * Copyright (c) 2005 Christophe Varoqui
 * Copyright (c) 2005 Alasdair Kergon, Redhat
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif
#include "mpath_cmd.h"

#include "uxsock.h"
#include "debug.h"

/*
 * Code is similar with mpath_recv_reply() with data size limitation
 * and debug-able malloc.
 * When limit == 0, it means no limit on data size, used for socket client
 * to receiving data from multipathd.
 */
static int _recv_packet(int fd, char **buf, unsigned int timeout,
			ssize_t limit);

#include "../libmpathcmd/mpath_fill_sockaddr.c"

/*
 * create a unix domain socket and start listening on it
 * return a file descriptor open on the socket
 */
int ux_socket_listen(const char *name)
{
	int fd;
	size_t len;
	struct sockaddr_un addr;

	/* This is after the PID check, so unlinking should be fine */
	if (name[0] != '@' && unlink(name) == -1 && errno != ENOENT)
		condlog(1, "Failed to unlink %s", name);

	fd = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (fd == -1) {
		condlog(3, "Couldn't create ux_socket, error %d", errno);
		return -1;
	}

	len = mpath_fill_sockaddr__(&addr, name);
	if (bind(fd, (struct sockaddr *)&addr, len) == -1) {
		condlog(3, "Couldn't bind to ux_socket, error %d", errno);
		close(fd);
		return -1;
	}

	/*
	 * Socket needs to have rw permissions for everone.
	 * SO_PEERCRED makes sure that only root can modify things.
	 */
	if (name[0] != '@' &&
	    chmod(name, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) == -1)
		condlog(3, "failed to set permissions on %s: %s", name, strerror(errno));

	if (listen(fd, 10) == -1) {
		condlog(3, "Couldn't listen to ux_socket, error %d", errno);
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * send a packet in length prefix format
 */
int send_packet(int fd, const char *buf)
{
	if (mpath_send_cmd(fd, buf) < 0)
		return -errno;
	return 0;
}

static int _recv_packet(int fd, char **buf, unsigned int timeout, ssize_t limit)
{
	int err = 0;
	ssize_t len = 0;

	*buf = NULL;
	len = mpath_recv_reply_len(fd, timeout);
	if (len == 0)
		return len;
	if (len < 0)
		return -errno;
	if ((limit > 0) && (len > limit))
		return -EINVAL;
	(*buf) = calloc(1, len);
	if (!*buf)
		return -ENOMEM;
	err = mpath_recv_reply_data(fd, *buf, len, timeout);
	if (err != 0) {
		free(*buf);
		(*buf) = NULL;
		return -errno;
	}
	return err;
}

/*
 * receive a packet in length prefix format
 */
int recv_packet(int fd, char **buf, unsigned int timeout)
{
	return _recv_packet(fd, buf, timeout, 0 /* no limit */);
}
