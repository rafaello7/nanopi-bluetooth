/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014 Intel Corporation. All rights reserved.
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
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "hciattach.h"

#define HCI_COMMAND_PKT		0x01
/* BD Address */
typedef struct {
	uint8_t b[6];
} __attribute__((packed)) bdaddr_t;

#define BCM43XX_CLOCK_48 1
#define BCM43XX_CLOCK_24 2

#define CMD_SUCCESS 0x00

#define CC_MIN_SIZE 7

#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))

static int bcm43xx_read_local_name(int fd, char *name, size_t size)
{
	unsigned char cmd[] = { HCI_COMMAND_PKT, 0x14, 0x0C, 0x00 };
	unsigned char *resp;
	unsigned int name_len;

	resp = malloc(size + CC_MIN_SIZE);
	if (!resp)
		return -1;

	tcflush(fd, TCIOFLUSH);

	if (write(fd, cmd, sizeof(cmd)) != sizeof(cmd)) {
		fprintf(stderr, "Failed to write read local name command\n");
		goto fail;
	}

	if (read_hci_event(fd, resp, size) < CC_MIN_SIZE) {
		fprintf(stderr, "Failed to read local name, invalid HCI event\n");
		goto fail;
	}

	if (resp[4] != cmd[1] || resp[5] != cmd[2] || resp[6] != CMD_SUCCESS) {
		fprintf(stderr, "Failed to read local name, command failure\n");
		goto fail;
	}

	name_len = resp[2] - 1;

	strncpy(name, (char *) &resp[7], MIN(name_len, size));
	name[size - 1] = 0;

	free(resp);
	return 0;

fail:
	free(resp);
	return -1;
}

static int bcm43xx_reset(int fd)
{
	unsigned char cmd[] = { HCI_COMMAND_PKT, 0x03, 0x0C, 0x00 };
	unsigned char resp[CC_MIN_SIZE];

	if (write(fd, cmd, sizeof(cmd)) != sizeof(cmd)) {
		fprintf(stderr, "Failed to write reset command\n");
		return -1;
	}

	if (read_hci_event(fd, resp, sizeof(resp)) < CC_MIN_SIZE) {
		fprintf(stderr, "Failed to reset chip, invalid HCI event\n");
		return -1;
	}

	if (resp[4] != cmd[1] || resp[5] != cmd[2] || resp[6] != CMD_SUCCESS) {
		fprintf(stderr, "Failed to reset chip, command failure\n");
		return -1;
	}

	return 0;
}

int bachk(const char *str)
{
	if (!str)
		return -1;

	if (strlen(str) != 17)
		return -1;

	while (*str) {
		if (!isxdigit(*str++))
			return -1;

		if (!isxdigit(*str++))
			return -1;

		if (*str == 0)
			break;

		if (*str++ != ':')
			return -1;
	}

	return 0;
}

int str2ba(const char *str, bdaddr_t *ba)
{
	int i;

	if (bachk(str) < 0) {
		memset(ba, 0, sizeof(*ba));
		return -1;
	}

	for (i = 5; i >= 0; i--, str += 3)
		ba->b[i] = strtol(str, NULL, 16);

	return 0;
}

static int bcm43xx_set_bdaddr(int fd, const char *bdaddr)
{
	unsigned char cmd[] =
		{ HCI_COMMAND_PKT, 0x01, 0xfc, 0x06, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00 };
	unsigned char resp[CC_MIN_SIZE];

	printf("Set BDADDR UART: %s\n", bdaddr);

	if (str2ba(bdaddr, (bdaddr_t *) (&cmd[4])) < 0) {
		fprintf(stderr, "Incorrect bdaddr\n");
		return -1;
	}

	tcflush(fd, TCIOFLUSH);

	if (write(fd, cmd, sizeof(cmd)) != sizeof(cmd)) {
		fprintf(stderr, "Failed to write set bdaddr command\n");
		return -1;
	}

	if (read_hci_event(fd, resp, sizeof(resp)) < CC_MIN_SIZE) {
		fprintf(stderr, "Failed to set bdaddr, invalid HCI event\n");
		return -1;
	}

	if (resp[4] != cmd[1] || resp[5] != cmd[2] || resp[6] != CMD_SUCCESS) {
		fprintf(stderr, "Failed to set bdaddr, command failure\n");
		return -1;
	}

	return 0;
}

static int bcm43xx_set_clock(int fd, unsigned char clock)
{
	unsigned char cmd[] = { HCI_COMMAND_PKT, 0x45, 0xfc, 0x01, 0x00 };
	unsigned char resp[CC_MIN_SIZE];

	printf("Set Controller clock (%d)\n", clock);

	cmd[4] = clock;

	tcflush(fd, TCIOFLUSH);

	if (write(fd, cmd, sizeof(cmd)) != sizeof(cmd)) {
		fprintf(stderr, "Failed to write update clock command\n");
		return -1;
	}

	if (read_hci_event(fd, resp, sizeof(resp)) < CC_MIN_SIZE) {
		fprintf(stderr, "Failed to update clock, invalid HCI event\n");
		return -1;
	}

	if (resp[4] != cmd[1] || resp[5] != cmd[2] || resp[6] != CMD_SUCCESS) {
		fprintf(stderr, "Failed to update clock, command failure\n");
		return -1;
	}

	return 0;
}

static int bcm43xx_set_speed(int fd, struct termios *ti, uint32_t speed)
{
	unsigned char cmd[] =
		{ HCI_COMMAND_PKT, 0x18, 0xfc, 0x06, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00 };
	unsigned char resp[CC_MIN_SIZE];

	if (speed > 3000000 && bcm43xx_set_clock(fd, BCM43XX_CLOCK_48))
		return -1;

	printf("Set Controller UART speed to %d bit/s\n", speed);

	cmd[6] = (uint8_t) (speed);
	cmd[7] = (uint8_t) (speed >> 8);
	cmd[8] = (uint8_t) (speed >> 16);
	cmd[9] = (uint8_t) (speed >> 24);

	tcflush(fd, TCIOFLUSH);

	if (write(fd, cmd, sizeof(cmd)) != sizeof(cmd)) {
		fprintf(stderr, "Failed to write update baudrate command\n");
		return -1;
	}

	if (read_hci_event(fd, resp, sizeof(resp)) < CC_MIN_SIZE) {
		fprintf(stderr, "Failed to update baudrate, invalid HCI event\n");
		return -1;
	}

	if (resp[4] != cmd[1] || resp[5] != cmd[2] || resp[6] != CMD_SUCCESS) {
		fprintf(stderr, "Failed to update baudrate, command failure\n");
		return -1;
	}

	if (set_speed(fd, ti, speed) < 0) {
		perror("Can't set host baud rate");
		return -1;
	}

	return 0;
}

static int bcm43xx_load_firmware(int fd, const char *fw)
{
	unsigned char cmd[] = { HCI_COMMAND_PKT, 0x2e, 0xfc, 0x00 };
	struct timespec tm_mode = { 0, 50000 };
	struct timespec tm_ready = { 0, 100000000 };
	unsigned char resp[CC_MIN_SIZE];
	unsigned char tx_buf[1024];
	int len, fd_fw, n;

	printf("Flash firmware %s\n", fw);

	fd_fw = open(fw, O_RDONLY);
	if (fd_fw < 0) {
		fprintf(stderr, "Unable to open firmware (%s)\n", fw);
		return -1;
	}

	tcflush(fd, TCIOFLUSH);

	if (write(fd, cmd, sizeof(cmd)) != sizeof(cmd)) {
		fprintf(stderr, "Failed to write download mode command\n");
		goto fail;
	}

	if (read_hci_event(fd, resp, sizeof(resp)) < CC_MIN_SIZE) {
		fprintf(stderr, "Failed to load firmware, invalid HCI event\n");
		goto fail;
	}

	if (resp[4] != cmd[1] || resp[5] != cmd[2] || resp[6] != CMD_SUCCESS) {
		fprintf(stderr, "Failed to load firmware, command failure\n");
		goto fail;
	}

	/* Wait 50ms to let the firmware placed in download mode */
	nanosleep(&tm_mode, NULL);

	tcflush(fd, TCIOFLUSH);

	while ((n = read(fd_fw, &tx_buf[1], 3))) {
		if (n < 0) {
			fprintf(stderr, "Failed to read firmware\n");
			goto fail;
		}

		tx_buf[0] = HCI_COMMAND_PKT;

		len = tx_buf[3];

		if (read(fd_fw, &tx_buf[4], len) < 0) {
			fprintf(stderr, "Failed to read firmware\n");
			goto fail;
		}

		if (write(fd, tx_buf, len + 4) != (len + 4)) {
			fprintf(stderr, "Failed to write firmware\n");
			goto fail;
		}

		read_hci_event(fd, resp, sizeof(resp));
		tcflush(fd, TCIOFLUSH);
	}

	/* Wait for firmware ready */
	nanosleep(&tm_ready, NULL);

	close(fd_fw);
	return 0;

fail:
	close(fd_fw);
	return -1;
}

int bcm43xx_init(int fd, int def_speed, int speed, struct termios *ti,
		const char *bdaddr)
{
	char chip_name[40];
	char fw_path[] = "/lib/firmware/brcm/bcm43438a0.hcd";

	printf("bcm43xx_init\n");

	if (bcm43xx_reset(fd))
		return -1;

	if (bcm43xx_read_local_name(fd, chip_name, sizeof(chip_name)))
		return -1;
	printf("chip name: %s\n", chip_name);

	if (bcm43xx_set_speed(fd, ti, speed))
		return -1;

	if (bcm43xx_load_firmware(fd, fw_path))
		return -1;

	/* Controller speed has been reset to def speed */
	if (set_speed(fd, ti, def_speed) < 0) {
		perror("Can't set host baud rate");
		return -1;
	}

	if (bcm43xx_reset(fd))
		return -1;

	if (bdaddr)
		bcm43xx_set_bdaddr(fd, bdaddr);

	if (bcm43xx_set_speed(fd, ti, speed))
		return -1;

	return 0;
}
