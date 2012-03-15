/*

Copyright (C) 2012 Russ Dill <Russ.Dill@gmail.com>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>

#ifdef DEBUG
#define dbg(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define dbg(fmt, ...) do {} while(0)
#endif

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define INIT_C_CC "\003\034\177\025\004\0\1\0\021\023\032\0\022\017\027\026\0"

struct thread_data {
	int fds[2];
	int port_fd;
	int has_ios;
	struct termios ios;
	pthread_mutex_t mutex;
	char pathname[0];
};

static int (*orig_open)(const char *, int, mode_t);
static int (*orig_open64)(const char *, int, mode_t);
static int (*orig_close)(int);
static int (*orig_ioctl)(int, unsigned long int, char *);

static char **ttys;
static struct thread_data *ports[4096];

static pthread_mutex_t ports_mutex = PTHREAD_MUTEX_INITIALIZER;

static const struct termios tty_std_termios = {
	.c_iflag = ICRNL | IXON,
	.c_oflag = OPOST | ONLCR,
	.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL,
	.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK |
		   ECHOCTL | ECHOKE | IEXTEN,
	.c_cc = INIT_C_CC,
	.c_line = 0,
	.c_ispeed = 9600,
	.c_ospeed = 9600,
};

__attribute__((constructor))
static void ttypersist_init(void)
{
	char *tty_env;
	int i;
	int n;
	int count;

	orig_open = dlsym(RTLD_NEXT, "open");
	if (!orig_open) {
		fprintf(stderr, "dlsym(open): %s\n", dlerror());
		exit(EXIT_FAILURE);
	}

	orig_open64 = dlsym(RTLD_NEXT, "open64");
	if (!orig_open64) {
		fprintf(stderr, "dlsym(open64): %s\n", dlerror());
		exit(EXIT_FAILURE);
	}

	orig_close = dlsym(RTLD_NEXT, "close");
	if (!orig_close) {
		fprintf(stderr, "dlsym(close): %s\n", dlerror());
		exit(EXIT_FAILURE);
	}

	orig_ioctl = dlsym(RTLD_NEXT, "ioctl");
	if (!orig_ioctl) {
		fprintf(stderr, "dlsym(ioctl): %s\n", dlerror());
		exit(EXIT_FAILURE);
	}

	tty_env = getenv("TTYS");
	count = tty_env != 0;
	for (i = 0; tty_env && tty_env[i]; i++)
		if (tty_env[i] == ':') count++;

	ttys = calloc(count + 1, sizeof(char *));

	n = 0;
	if (tty_env)
		ttys[n++] = tty_env;
	for (i = 0; tty_env && tty_env[i]; i++)
		if (tty_env[i] == ':') {
			tty_env[i] = '\0';
			ttys[n++] = tty_env + i + 1;
		}
}

static void *persist_thread(void *arg)
{
	struct thread_data *data = arg;
	int ret;
	int init = 0;
	char c;
	int ready_out = 0;	/* Out to port */
	int ready_in = 0;	/* In from port */

	ret = fcntl(data->fds[1], F_GETFL, 0);
	fcntl(data->fds[1], F_SETFL, ret | O_NONBLOCK);

reconnect:
	if (data->port_fd != -1)
		close(data->port_fd);

	pthread_mutex_lock(&data->mutex);
	do {
		pthread_mutex_unlock(&data->mutex);
		if (init) {
			usleep(250 * 1000);
			do {
				ret = read(data->fds[1], &c, 1);
				if (ret == 0 || (ret < 0 && errno != EAGAIN))
					goto exit_thread;
			} while (ret == 1);
		}
		pthread_mutex_lock(&data->mutex);

		data->port_fd = orig_open(data->pathname, O_RDWR, 0);
		if (data->port_fd < 0) {
			dbg("%s (%m)\n", data->pathname);
			if (errno != ENODEV &&
			    errno != ENOENT &&
			    errno != EACCES && /* May be changed soon... */
			    errno != EINVAL) {
				pthread_mutex_unlock(&data->mutex);
				goto exit_thread;
			}
		}
		if (!init && data->port_fd == -1) {
			c = 0;
			if (write(data->fds[1], &c, 1)) {}
			init = 1;
		}
	} while (data->port_fd == -1);

	dbg("%s opened\n", data->pathname);

	ret = fcntl(data->port_fd, F_GETFL, 0);
	fcntl(data->port_fd, F_SETFL, ret | O_NONBLOCK);

	/* There may be a race with another process, oh well. */
	if (data->has_ios)
		orig_ioctl(data->port_fd, TCSETSF, (char *) &data->ios);

	if (!init) {
		c = 0;
		if (write(data->fds[1], &c, 1)) {}
		init = 1;
	}

	pthread_mutex_unlock(&data->mutex);

	for (;;) {
		fd_set rfds;
		fd_set wfds;
		int n;

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		if (ready_out)
			FD_SET(data->fds[1], &rfds);
		else
			FD_SET(data->port_fd, &wfds);

		if (ready_in)
			FD_SET(data->port_fd, &rfds);
		else
			FD_SET(data->fds[1], &wfds);

		n = max(data->port_fd, data->fds[1]);
		ret = select(n + 1, &rfds, &wfds, NULL, NULL);
		if (ret == 0) {
			/* Timeout? */
		} else if (ret > 0) {
			if (ready_in && FD_ISSET(data->port_fd, &rfds)) {

				ret = read(data->port_fd, &c, 1);
				if (!ret || (ret < 0 && errno == ENODEV))
					goto reconnect;
				else if (ret < 0 && errno != EAGAIN)
					goto exit_thread;
				else if (ret == 1) {
					ret = write(data->fds[1], &c, 1);
					if (ret != 1)
						goto exit_thread;
					ready_in--;
				}
			} else if (!ready_in && FD_ISSET(data->fds[1], &wfds))
				ready_in++;

			if (ready_out && FD_ISSET(data->fds[1], &rfds)) {

				ret = read(data->fds[1], &c, 1);
				if (!ret || (ret < 0 && errno != EAGAIN))
					goto exit_thread;
				else if (ret == 1) {
					ret = write(data->port_fd, &c, 1);
					if (ret < 0 && errno == ENODEV)
						goto reconnect;
					else if (ret != 1)
						goto exit_thread;
					ready_out--;
				}
			} else if (!ready_out && FD_ISSET(data->port_fd, &wfds))
				ready_out++;
		} else
			goto exit_thread;
	}

exit_thread:
	pthread_mutex_lock(&ports_mutex);
	pthread_mutex_lock(&data->mutex);
	ports[data->fds[0]] = NULL;
	pthread_mutex_unlock(&ports_mutex);
	pthread_mutex_unlock(&data->mutex);
	close(data->port_fd);
	close(data->fds[1]);
	free(data);

	return NULL;
}

static int persist_open(const char *pathname, int flags)
{
	int ret;
	int fd;
	char c;
	struct thread_data *data;
	pthread_t thread;

	data = malloc(sizeof(struct thread_data) + strlen(pathname) + 1);
	strcpy(data->pathname, pathname);
	pthread_mutex_init(&data->mutex, NULL);
	data->has_ios = 0;
	data->port_fd = -1;

	ret = socketpair(AF_LOCAL, SOCK_STREAM, 0, data->fds);
	if (ret < 0)
		goto err_free;

	fd = data->fds[0];
	if (fd >= 4096) {
		ret = -1;
		goto err_free;
	}

	while (ports[fd])
		usleep(1);

	pthread_mutex_lock(&ports_mutex);
	ret = pthread_create(&thread, NULL, persist_thread, data);
	if (ret < 0)
		goto err_close;

	ports[fd] = data;
	pthread_mutex_unlock(&ports_mutex);

	if (read(fd, &c, 1)) {}

	return fd;

err_close:
	pthread_mutex_unlock(&ports_mutex);
	close(data->fds[0]);
	close(data->fds[1]);

err_free:
	free(data);
	return ret;
}

int open64(const char *pathname, int flags, ...)
{
	int i;

	for (i = 0; ttys[i] && strcmp(ttys[i], pathname); i++);
	if (!ttys[i]) {
		mode_t mode;

		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);

		return orig_open64(pathname, flags, mode);
	}

	return persist_open(pathname, flags);
}

int open(const char *pathname, int flags, ...)
{
	int i;

	for (i = 0; ttys[i] && strcmp(ttys[i], pathname); i++);
	if (!ttys[i]) {
		mode_t mode;

		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);

		return orig_open(pathname, flags, mode);
	}

	return persist_open(pathname, flags);
}

static void read_ios(struct thread_data *data, char *arg)
{
	if (data->port_fd != -1)
		if (!orig_ioctl(data->port_fd, TCGETS, (char *) &data->ios))
			data->has_ios = 1;
	if (!data->has_ios) {
		memcpy(&data->ios, &tty_std_termios, sizeof(data->ios));
		data->has_ios = 1;
	}
	if (arg)
		memcpy(arg, &data->ios, sizeof(data->ios));
}

static void write_ios(struct thread_data *data, int request, char *arg)
{
	int ret = 0;
	if (data->port_fd != -1)
		ret = orig_ioctl(data->port_fd, request, arg);
	if (!ret) {
		memcpy(&data->ios, arg, sizeof(data->ios));
		data->has_ios = 1;
	}
}

int ioctl(int d, unsigned long int request, ...)
{
	struct thread_data *data;
	char *arg;
	int ret = 0;

	va_list ap;
	va_start(ap, request);
	arg = va_arg(ap, char *);
	va_end(ap);

	pthread_mutex_lock(&ports_mutex);
	if (!ports[d]) {
		pthread_mutex_unlock(&ports_mutex);
		return orig_ioctl(d, request, arg);
	}
	data = ports[d];
	pthread_mutex_lock(&data->mutex);
	pthread_mutex_unlock(&ports_mutex);

	switch (request) {
	case TCGETS:
		read_ios(data, arg);
		break;

	case TCSETS:
	case TCSETSW:
	case TCSETSF:
		write_ios(data, request, arg);
		break;

	/* Don't save the state for these, if we lose the tty,
	 * its as if another process toggled the state */
	case TCSBRK:
	case TCSBRKP:
	case FIONREAD:
	case TIOCOUTQ:
	case TCFLSH:
	case TCXONC:
	case TIOCSBRK:
	case TIOCCBRK:
	case TIOCMSET:
	case TIOCMBIC:
	case TIOCMBIS:
		if (data->port_fd != -1)
			orig_ioctl(data->port_fd, request, arg);
		break;

	case TIOCMGET:
		if (data->port_fd != -1)
			orig_ioctl(data->port_fd, request, arg);
		else
			*((int *) arg) = 0;
		break;

	case TIOCGSOFTCAR:
		read_ios(data, NULL);
		*((int *) arg) = !!(data->ios.c_cflag & CLOCAL);
		break;

	case TIOCSSOFTCAR:
		if (data->has_ios) {
			int bit = *((int *) arg) ? CLOCAL : 0;
			data->ios.c_cflag &= ~CLOCAL;
			data->ios.c_cflag |= bit;
		}
		if (data->port_fd != -1)
			orig_ioctl(data->port_fd, request, arg);
		break;

	default:
		ret = -1;
		errno = EINVAL;
	}

	pthread_mutex_unlock(&data->mutex);
	return ret;
}

int tcgetattr(int fd, struct termios *termios_p)
{
	return ioctl(fd, TCGETS, termios_p);
}

int tcsetattr(int fd, int optional_action, const struct termios *termios_p)
{
	int request = 0;
	switch (optional_action) {
	case TCSANOW:
		request = TCSETS;
		break;
	case TCSADRAIN:
		request = TCSETSW;
		break;
	case TCSAFLUSH:
		request = TCSETSF;
		break;
	default:
		errno = -EINVAL;
		return -1;
	}
	return ioctl(fd, request, termios_p);
}

int tcsendbreak(int fd, int duration)
{
	if (duration > 0)
		return ioctl(fd, TCSBRKP, (duration + 99) / 100);
	else
		return ioctl(fd, TCSBRK, 0);
}

int tcdrain(int fd)
{
	return ioctl(fd, TCSBRK, 1);
}

int tcflush(int fd, int queue_selector)
{
	return ioctl(fd, TCFLSH, queue_selector);
}

int tcflow(int fd, int action)
{
	return ioctl(fd, TCXONC, action);
}
