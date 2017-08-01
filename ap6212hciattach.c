/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <stdint.h>

#include "tty.h"
#include "hciattach.h"

struct uart_t {
	char *type;
	int  m_id;
	int  p_id;
	int  proto;
	int  init_speed;
	int  speed;
	int  flags;
	int  pm;
	char *bdaddr;
	int  (*init) (int fd, const struct uart_t *u, struct termios *ti);
};

#define FLOW_CTL	0x0001
#define AMP_DEV		0x0002
#define ENABLE_PM	1
#define DISABLE_PM	0

static volatile sig_atomic_t __io_canceled = 0;

static void sig_hup(int sig)
{
}

static void sig_term(int sig)
{
	__io_canceled = 1;
}

static void sig_alarm(int sig)
{
	fprintf(stderr, "Initialization timed out.\n");
	exit(1);
}

int set_speed(int fd, struct termios *ti, int speed)
{
	if (cfsetospeed(ti, tty_get_speed(speed)) < 0)
		return -errno;

	if (cfsetispeed(ti, tty_get_speed(speed)) < 0)
		return -errno;

	if (tcsetattr(fd, TCSANOW, ti) < 0)
		return -errno;

	return 0;
}

/*
 * Read an HCI event from the given file descriptor.
 */
int read_hci_event(int fd, unsigned char* buf, int size)
{
	int remain, r;
	int count = 0;

	if (size <= 0)
		return -1;

	/* The first byte identifies the packet type. For HCI event packets, it
	 * should be 0x04, so we read until we get to the 0x04. */
	while (1) {
		r = read(fd, buf, 1);
		if (r <= 0)
			return -1;
		if (buf[0] == 0x04)
			break;
	}
	count++;

	/* The next two bytes are the event code and parameter total length. */
	while (count < 3) {
		r = read(fd, buf + count, 3 - count);
		if (r <= 0)
			return -1;
		count += r;
	}

	/* Now we read the parameters. */
	if (buf[2] < (size - 3))
		remain = buf[2];
	else
		remain = size - 3;

	while ((count - 3) < remain) {
		r = read(fd, buf + count, remain - (count - 3));
		if (r <= 0)
			return -1;
		count += r;
	}

	return count;
}

static int bcm43xx(int fd, const struct uart_t *u, struct termios *ti)
{
	return bcm43xx_init(fd, u->init_speed, u->speed, ti, u->bdaddr);
}

/*
 * BCSP specific initialization
 */
static int bcsp_max_retries = 10;

struct uart_t uart_bcm43xx = {
	"bcm43xx",    0x0000, 0x0000, HCI_UART_H4,   115200, 3000000,
				FLOW_CTL, DISABLE_PM, NULL, bcm43xx
};

/* Initialize UART driver */
static int init_uart(char *dev, const struct uart_t *u, int send_break, int raw)
{
	struct termios ti;
	int fd, i;
	unsigned long flags = 0;

	if (raw)
		flags |= 1 << HCI_UART_RAW_DEVICE;

	if (u->flags & AMP_DEV)
		flags |= 1 << HCI_UART_CREATE_AMP;

	fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror("Can't open serial port");
		return -1;
	}

	tcflush(fd, TCIOFLUSH);

	if (tcgetattr(fd, &ti) < 0) {
		perror("Can't get port settings");
		goto fail;
	}

	cfmakeraw(&ti);

	ti.c_cflag |= CLOCAL;
	if (u->flags & FLOW_CTL)
		ti.c_cflag |= CRTSCTS;
	else
		ti.c_cflag &= ~CRTSCTS;

	if (tcsetattr(fd, TCSANOW, &ti) < 0) {
		perror("Can't set port settings");
		goto fail;
	}

	/* Set initial baudrate */
	if (set_speed(fd, &ti, u->init_speed) < 0) {
		perror("Can't set initial baud rate");
		goto fail;
	}

	tcflush(fd, TCIOFLUSH);

	if (send_break) {
		tcsendbreak(fd, 0);
		usleep(500000);
	}

	if (u->init && u->init(fd, u, &ti) < 0)
		goto fail;

	tcflush(fd, TCIOFLUSH);

	/* Set actual baudrate */
	if (set_speed(fd, &ti, u->speed) < 0) {
		perror("Can't set baud rate");
		goto fail;
	}

	/* Set TTY to N_HCI line discipline */
	i = N_HCI;
	if (ioctl(fd, TIOCSETD, &i) < 0) {
		perror("Can't set line discipline");
		goto fail;
	}

	if (flags && ioctl(fd, HCIUARTSETFLAGS, flags) < 0) {
		perror("Can't set UART flags");
		goto fail;
	}

	if (ioctl(fd, HCIUARTSETPROTO, u->proto) < 0) {
		perror("Can't set device");
		goto fail;
	}

	return fd;

fail:
	close(fd);
	return -1;
}

static void usage(void)
{
	printf("ap6212attach - bluetooth initialization utility for ap6212\n");
	printf("Usage:\n");
	printf("\tap6212attach [-n] [-p] [-b] [-r] [-t timeout] [-s initial_speed]"
			" <tty> [speed] [flow|noflow]"
			" [sleep|nosleep] [bdaddr]\n");
}

int main(int argc, char *argv[])
{
	int detach, printpid, raw, opt, i, n, ld, err;
	int to = 10;
	int init_speed = 0;
	int send_break = 0;
	pid_t pid;
	struct sigaction sa;
	struct pollfd p;
	sigset_t sigs;
	char dev[PATH_MAX];

	detach = 1;
	printpid = 0;
	raw = 0;

	while ((opt=getopt(argc, argv, "bnpt:s:r")) != EOF) {
		switch(opt) {
		case 'b':
			send_break = 1;
			break;

		case 'n':
			detach = 0;
			break;

		case 'p':
			printpid = 1;
			break;

		case 't':
			to = atoi(optarg);
			break;

		case 's':
			init_speed = atoi(optarg);
			break;

		case 'r':
			raw = 1;
			break;

		default:
			usage();
			exit(1);
		}
	}

	n = argc - optind;
	if (n < 2) {
		usage();
		exit(1);
	}

	for (n = 0; optind < argc; n++, optind++) {
		char *opt;

		opt = argv[optind];

		switch(n) {
		case 0:
			dev[0] = 0;
			if (!strchr(opt, '/'))
				strcpy(dev, "/dev/");

			if (strlen(opt) > PATH_MAX - (strlen(dev) + 1)) {
				fprintf(stderr, "Invalid serial device\n");
				exit(1);
			}

			strcat(dev, opt);
			break;

		case 1:
			uart_bcm43xx.speed = atoi(argv[optind]);
			break;

		case 2:
			if (!strcmp("flow", argv[optind]))
				uart_bcm43xx.flags |=  FLOW_CTL;
			else
				uart_bcm43xx.flags &= ~FLOW_CTL;
			break;

		case 3:
			if (!strcmp("sleep", argv[optind]))
				uart_bcm43xx.pm = ENABLE_PM;
			else
				uart_bcm43xx.pm = DISABLE_PM;
			break;

		case 4:
			uart_bcm43xx.bdaddr = argv[optind];
			break;
		}
	}

	/* If user specified a initial speed, use that instead of
	   the hardware's default */
	if (init_speed)
		uart_bcm43xx.init_speed = init_speed;

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags   = SA_NOCLDSTOP;
	sa.sa_handler = sig_alarm;
	sigaction(SIGALRM, &sa, NULL);

	/* 10 seconds should be enough for initialization */
	alarm(to);
	bcsp_max_retries = to;

	n = init_uart(dev, &uart_bcm43xx, send_break, raw);
	if (n < 0) {
		perror("Can't initialize device");
		exit(1);
	}

	printf("Device setup complete\n");

	alarm(0);

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags   = SA_NOCLDSTOP;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);

	sa.sa_handler = sig_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);

	sa.sa_handler = sig_hup;
	sigaction(SIGHUP, &sa, NULL);

	if (detach) {
		if ((pid = fork())) {
			if (printpid)
				printf("%d\n", pid);
			return 0;
		}

		for (i = 0; i < 20; i++)
			if (i != n)
				close(i);
	}

	p.fd = n;
	p.events = POLLERR | POLLHUP;

	sigfillset(&sigs);
	sigdelset(&sigs, SIGCHLD);
	sigdelset(&sigs, SIGPIPE);
	sigdelset(&sigs, SIGTERM);
	sigdelset(&sigs, SIGINT);
	sigdelset(&sigs, SIGHUP);

	while (!__io_canceled) {
		p.revents = 0;
		err = ppoll(&p, 1, NULL, &sigs);
		if (err < 0 && errno == EINTR)
			continue;
		if (err)
			break;
	}

	/* Restore TTY line discipline */
	ld = N_TTY;
	if (ioctl(n, TIOCSETD, &ld) < 0) {
		perror("Can't restore line discipline");
		exit(1);
	}

	return 0;
}
