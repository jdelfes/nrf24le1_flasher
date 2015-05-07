#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ftdi.h>
#include <libusb.h>
#include "spi.h"

#define LOW	0
#define HIGH	1

// pins defs
#define PIN_FCSN	(1 << 1)	/* RXD */	// nRF2LE1 Chip select
#define PIN_FMISO	(1 << 2)	/* RTS */
#define PIN_FMOSI	(1 << 3)	/* CTS */
#define PIN_FSCK	(1 << 4)	/* DTR */
#define PIN_RESET	(1 << 5)	/* DSR */	// nRF24LE1 Reset
#define PIN_PROG	(1 << 6)	/* DCD */	// nRF24LE1 Program
#define PINS_OUT	(PIN_PROG|PIN_RESET|PIN_FCSN|PIN_FSCK|PIN_FMOSI)

#define BYTES_PER_BIT 4
#define FTDI_READ_FIFO_SIZE 96
//#define delay_ns(t)
#define delay_us(t) usleep(t)
#define delay_ms(t) usleep(t * 1000)
#define delay_s(t) sleep(t)


static struct ftdi_context *ftdi;
static uint8_t pin_state = 0;

static uint8_t digitalWrite(unsigned char pin, int value)
{
	uint8_t v;

	if (value)
		pin_state |= pin;
	else
		pin_state &= ~pin;

	ftdi_write_data(ftdi, &pin_state, sizeof(pin_state));
	ftdi_read_data(ftdi, &v, sizeof(v));

	return v;
}

static void prog_begin()
{
	digitalWrite(PIN_PROG, HIGH);
	digitalWrite(PIN_RESET, LOW);
	delay_us(1); // we need only 0.2 us
	digitalWrite(PIN_RESET, HIGH);
	delay_ms(2); // we need to wait at least 1.5 ms before send commands
}

static void prog_end()
{
	digitalWrite(PIN_PROG, LOW);
	digitalWrite(PIN_RESET, LOW);
	delay_us(1);
	digitalWrite(PIN_RESET, HIGH);
}

int spi_begin(uint8_t bus, uint8_t port)
{
	int ret;

	ftdi = ftdi_new();
	if (ftdi == 0) {
		fprintf(stderr, "ftdi_new failed\n");
		return -1;
	}

	if (bus > 0) {
		struct ftdi_device_list *list = NULL;
		struct ftdi_device_list *p;

		ret = ftdi_usb_find_all(ftdi, &list, 0, 0);
		if (ret < 0) {
			fprintf(stderr, "unable to list devices: %d (%s)\n",
					ret, ftdi_get_error_string(ftdi));
			ftdi_free(ftdi);
			return -2;
		}

		p = list;
		while (p) {
			if (bus == libusb_get_bus_number(p->dev) &&
				port == libusb_get_port_number(p->dev)) {
				ret = ftdi_usb_open_dev(ftdi, p->dev);
				break;
			}
			p = p->next;
		}

		ftdi_list_free(&list);

		if (!p) {
			fprintf(stderr, "dev on bus %i and port %i not found\n",
								bus, port);
			ftdi_free(ftdi);
			return -3;
		}
	} else
		ret = ftdi_usb_open(ftdi, 0x0403, 0x6001);

	if (ret < 0 && ret != -5) {
		fprintf(stderr, "unable to open ftdi device: %d (%s)\n", ret,
						ftdi_get_error_string(ftdi));
		ftdi_free(ftdi);
		return -4;
	}

	ret = ftdi_set_bitmode(ftdi, PINS_OUT, BITMODE_SYNCBB);
	if (ret != 0) {
		fprintf(stderr, "unable to set bitmode: %d (%s)\n", ret,
						ftdi_get_error_string(ftdi));
		ftdi_free(ftdi);
		return -5;
	}

	ret = ftdi_set_baudrate(ftdi, 9600);
	if (ret != 0) {
		fprintf(stderr, "unable to set baudrate: %d (%s)\n", ret,
						ftdi_get_error_string(ftdi));
		ftdi_disable_bitbang(ftdi);
		ftdi_free(ftdi);
		return -6;
	}

	digitalWrite(PIN_PROG, LOW);
	digitalWrite(PIN_FSCK, LOW);
	digitalWrite(PIN_FCSN, HIGH);
	digitalWrite(PIN_FMOSI, LOW);
	digitalWrite(PIN_RESET, HIGH);

	prog_begin();

	return 0;
}

void spi_end()
{
	prog_end();

	ftdi_disable_bitbang(ftdi);
	ftdi_free(ftdi);
}

static int spi_buf_w(const uint8_t *b, size_t s)
{
	const size_t total_size = s * 8 * BYTES_PER_BIT;
	int j = 0, pos;
	uint8_t *buf = calloc(1, total_size);

	for (pos = 0; pos < s; pos++) {
		uint8_t bit;

		// most significant bit first
		for (bit = (1 << 7); bit > 0; bit >>= 1) {
			if (b[pos] & bit) {
				pin_state |= PIN_FMOSI;
				buf[j++] = pin_state;
			} else {
				pin_state &= ~PIN_FMOSI;
				buf[j++] = pin_state;
			}
			pin_state |= PIN_FSCK;
			buf[j++] = pin_state;

#if BYTES_PER_BIT == 4
			buf[j++] = pin_state;
#endif

			pin_state &= ~PIN_FSCK;
			buf[j++] = pin_state;
		}
	}
	j = ftdi_write_data(ftdi, buf, j);
	free(buf);

	return j / 8 / BYTES_PER_BIT;
}

static int spi_buf_r(uint8_t *b, size_t s)
{
	const size_t total_size = s * 8 * BYTES_PER_BIT;
	int j = 0, pos;
	uint8_t *buf = calloc(1, total_size);

	if (ftdi_read_data(ftdi, buf, total_size) != total_size) {
		fprintf(stderr, "problem reading device\n");
		free(buf);
		return 0;
	}

	for (pos = 0; pos < s; pos++) {
		uint8_t v = 0;
		uint8_t bit;

		// most significant bit first
		for (bit = (1 << 7); bit > 0; bit >>= 1) {
			j += BYTES_PER_BIT - 1;
			if (buf[j++] & PIN_FMISO)
				v |= bit;
		}

		b[pos] = v;
	}

	free(buf);
	return j / 8 / BYTES_PER_BIT;
}

int spi_transfer(uint8_t *bytes, size_t size)
{
	int pos;
	int sz = FTDI_READ_FIFO_SIZE / 8 / BYTES_PER_BIT;

	digitalWrite(PIN_FCSN, LOW);
	for (pos = 0; pos < size; pos += sz) {
		int cksize = ((size - pos) < sz) ? (size - pos) : sz;

		if (spi_buf_w(bytes + pos, cksize) != cksize) {
			fprintf(stderr, "error writing spi\n");
			digitalWrite(PIN_FCSN, HIGH);
			return pos;
		}

		if (spi_buf_r(bytes + pos, cksize) != cksize) {
			fprintf(stderr, "error reading spi\n");
			digitalWrite(PIN_FCSN, HIGH);
			return pos;
		}
	}

	digitalWrite(PIN_FCSN, HIGH);
	return size;
}

