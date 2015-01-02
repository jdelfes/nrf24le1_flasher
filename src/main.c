#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "hexfile.h"
#include "spi.h"

// SPI Flash operations commands
#define WREN 		0x06	// Set flash write enable latch
#define WRDIS		0x04	// Reset flash write enable latch
#define RDSR		0x05	// Read FLASH Status Register (FSR)
#define WRSR		0x01	// Write FLASH Status Register (FSR)
#define READ		0x03	// Read data from FLASH
#define PROGRAM		0x02	// Write data to FLASH
#define ERASE_PAGE	0x52	// Erase addressed page
#define ERASE_ALL	0x62	// Erase all pages in FLASH main block and
				// infopage
#define RDFPCR		0x89	// Read FLASH Protect Configuration Register
				// FPCR
#define RDISMB		0x85	// Enable FLASH readback protection
#define ENDEBUG		0x86	// Enable HW debug features

// Flash Status Register (FSR) bits
#define FSR_ENDEBUG	(1 << 7)	// Initial value read from byte ENDEBUG 
					// in flash IP
#define FSR_STP		(1 << 6)	// Enable code execution start from
					// protected flash area (page address
					// NUPP)
#define FSR_WEN		(1 << 5)	// Flash write enable latch
#define FSR_RDYN	(1 << 4)	// Flash ready flag, active low
#define FSR_INFEN	(1 << 3)	// Flash IP Enable
#define FSR_RDISMB	(1 << 2)	// Flash MB readback protection enabled,
					// active low


static int spi_started = 0;

static uint8_t read_fsr()
{
	uint8_t cmd[2];

	cmd[0] = RDSR;
	cmd[1] = 0x00;
	spi_transfer(cmd, sizeof(cmd));
	return cmd[1];
}

static void write_fsr(uint8_t value)
{
	uint8_t cmd[2];

	cmd[0] = WRSR;
	cmd[1] = value;
	spi_transfer(cmd, sizeof(cmd));
}

static void enable_wen()
{
	uint8_t fsr = read_fsr();
	uint8_t cmd[1];

	// if WEN is not enabled, try to enable it
	if (!(fsr & FSR_WEN)) {
		cmd[0] = WREN;
		spi_transfer(cmd, sizeof(cmd));

		fsr = read_fsr();
		if (!(fsr & FSR_WEN)) {
			fprintf(stderr, "failed to set WEN bit on FSR\n");
			exit(EXIT_FAILURE);
		}
	}
}

static void enable_infen()
{
	uint8_t fsr = read_fsr();

	// if INFEN is not enabled, try to enable it
	if (!(fsr & FSR_INFEN)) {
		write_fsr(fsr | FSR_INFEN);
		fsr = read_fsr();
		if (!(fsr & FSR_INFEN)) {
			fprintf(stderr, "failed to set INFEN bit on FSR\n");
			exit(EXIT_FAILURE);
		}
	}
}

static void disable_infen()
{
	uint8_t fsr = read_fsr();

	// if INFEN is enabled, try to disable it
	if (fsr & FSR_INFEN) {
		write_fsr(fsr & ~FSR_INFEN);
		fsr = read_fsr();
		if (fsr & FSR_INFEN) {
			fprintf(stderr, "failed to unset INFEN bit on FSR\n");
			exit(EXIT_FAILURE);
		}
	}
}

static void wait_ready()
{
	uint8_t fsr = read_fsr();

	// wait flash ready flag
	while (fsr & FSR_RDYN)
		fsr = read_fsr();
}

static int check_rdismb()
{
	return (read_fsr() & FSR_RDISMB) ? 1 : 0;
}

static void cmd_device(uint8_t bus, uint8_t devnum)
{
	if (spi_started)
		spi_end();

	if (spi_begin(bus, devnum) != 0) {
		fprintf(stderr, "problem accessing device\n");
		exit(EXIT_FAILURE);
	}

	spi_started = 1;
}

static void cmd_read_flash(const char *filename)
{
	FILE *f;
	// maximum read size is 18432 (16kB+NV), plus 3 bytes for cmd and addr
	uint8_t buf[18432 + 3] = {0};

	if (check_rdismb()) {
		fprintf(stderr, "flash memory is protected\n");
		exit(EXIT_FAILURE);
	}

	disable_infen();

	printf("reading flash...\n");
	buf[0] = READ;
	buf[1] = 0x00; // address
	buf[2] = 0x00;
	if (!spi_transfer(buf, sizeof(buf))) {
		fprintf(stderr, "something wrong reading flash\n");
		exit(EXIT_FAILURE);
	}

	f = fopen(filename, "w");
	if (!f) {
		fprintf(stderr, "can't open %s to write\n", filename);
		exit(EXIT_FAILURE);
	}
	fwrite(buf + 3, sizeof(buf) - 3, 1, f);
	fclose(f);
}

static void cmd_write_flash(const char *filename)
{
	FILE *fd;
	// maximum hexfile data per line is 255, plus 3 bytes for cmd and addr
	uint8_t buffer[255 + 3] = {0};
	uint8_t orig[255 + 3] = {0};
	uint8_t comp[255 + 3] = {0};
	int count;
	uint16_t address;

	if (check_rdismb()) {
		fprintf(stderr, "flash memory is protected\n");
		exit(EXIT_FAILURE);
	}

	disable_infen();

	fd = fopen(filename, "r");
	if (!fd) {
		fprintf(stderr, "can't open %s to read\n", filename);
		exit(EXIT_FAILURE);
	}

	while ((count = hexfile_getline(fd, &address, buffer + 3,
						sizeof(buffer) - 3)) > 0) {
		enable_wen();

		memcpy(orig, buffer + 3, count);

		buffer[0] = PROGRAM;
		buffer[1] = address >> 8;
		buffer[2] = address & 0xff;
		printf("writing %i bytes at 0x%04hx...\n", count, address);
		if (!spi_transfer(buffer, count + 3)) {
			fprintf(stderr, "SPI error\n");
			fclose(fd);
			exit(EXIT_FAILURE);
		}
		wait_ready();

		comp[0] = READ;
		comp[1] = address >> 8;
		comp[2] = address & 0xff;
		if (!spi_transfer(comp, count + 3)) {
			fprintf(stderr, "SPI error\n");
			fclose(fd);
			exit(EXIT_FAILURE);
		}

		if (memcmp(orig, comp + 3, count)) {
			fprintf(stderr, "error checking memory\n");
			fclose(fd);
			exit(EXIT_FAILURE);
		}
	}
	fclose(fd);
}

static void cmd_erase_flash()
{
	uint8_t cmd[1];

	if (check_rdismb()) {
		fprintf(stderr, "flash memory is protected\n");
		exit(EXIT_FAILURE);
	}

	// we don't want to erase InfoPage
	disable_infen();

	enable_wen();

	printf("Erasing flash...\n");
	cmd[0] = ERASE_ALL;
	spi_transfer(cmd, sizeof(cmd));

	wait_ready();
}

static void cmd_lock()
{
	uint8_t cmd[1];

	if (check_rdismb()) {
		fprintf(stderr, "flash memory already protected\n");
		return;
	}

	enable_wen();

	printf("Trying to lock memory...\n");
	cmd[0] = RDISMB;
	spi_transfer(cmd, sizeof(cmd));

	if (!check_rdismb()) {
		fprintf(stderr, "failed to lock memory\n");
		exit(EXIT_FAILURE);
	}
}

static void cmd_write_fsr(uint8_t value)
{
	printf("writing fsr value: 0x%02x\n", value);
	write_fsr(value);
}

static void cmd_read_fsr()
{
	uint8_t fsr;

	fsr = read_fsr();
	printf("status register (FSR): 0x%02x\n", fsr);
	printf("ENDEBUG is%s set\n", (fsr & FSR_ENDEBUG)	? "" : " not");
	printf("STP	is%s set\n", (fsr & FSR_STP)		? "" : " not");
	printf("WEN	is%s set\n", (fsr & FSR_WEN)		? "" : " not");
	printf("RDYN	is%s set\n", (fsr & FSR_RDYN)		? "" : " not");
	printf("INFEN	is%s set\n", (fsr & FSR_INFEN)		? "" : " not");
	printf("RDISMB	is%s set\n", (fsr & FSR_RDISMB)		? "" : " not");
}

static void cmd_read_ip(const char *filename)
{
	FILE *f;
	// we need 512 bytes to InfoPage and 3 extra bytes to command
	uint8_t buf[512 + 3] = {0};

	enable_infen();

	printf("reading infopage...\n");
	buf[0] = READ;
	buf[1] = 0x00; // address
	buf[2] = 0x00;
	if (!spi_transfer(buf, sizeof(buf))) {
		fprintf(stderr, "something wrong reading InfoPage\n");
		exit(EXIT_FAILURE);
	}

	f = fopen(filename, "w");
	if (!f) {
		fprintf(stderr, "can't open %s to write\n", filename);
		exit(EXIT_FAILURE);
	}
	fwrite(buf + 3, sizeof(buf) - 3, 1, f);
	fclose(f);
}

static void cmd_write_ip(const char *filename)
{
	FILE *f;
	// we need 512 bytes to InfoPage and 3 extra bytes to command
	uint8_t buf[512 + 3] = {0};

	if (check_rdismb()) {
		fprintf(stderr, "flash memory is protected\n");
		exit(EXIT_FAILURE);
	}

	buf[0] = PROGRAM;
	buf[1] = 0x00; // address
	buf[2] = 0x00;

	f = fopen(filename, "r");
	if (!f) {
		fprintf(stderr, "can't open %s to read\n", filename);
		exit(EXIT_FAILURE);
	}
	fread(buf + 3, sizeof(buf) - 3, 1, f);
	fclose(f);

	enable_infen();
	enable_wen();

	printf("writing InfoPage\n");
	if (!spi_transfer(buf, sizeof(buf))) {
		fprintf(stderr, "something wrong writing InfoPage\n");
		exit(EXIT_FAILURE);
	}

	wait_ready();
}

static void cmd_erase_all()
{
	uint8_t cmd[1];

	// we don't want to erase InfoPage
	enable_infen();

	enable_wen();

	printf("Erasing all...\n");
	cmd[0] = ERASE_ALL;
	spi_transfer(cmd, sizeof(cmd));

	wait_ready();
}

static void cmd_show_usage(const char *name)
{
	printf("Usage: %s [options]\n", name);
	printf("Options:\n");
	printf("  -d, --device <bus>-<devnum>\n");
	printf("        Choose which USB device to use\n");
	printf("  -r, --read-flash <filename>\n");
	printf("        Reads flash memory and write to filename\n");
	printf("  -w, --write-flash <filename>\n");
	printf("        Writes flash memory from filename\n");
	printf("  -c, --erase-flash\n");
	printf("        Erases flash memory\n");
	printf("  -x,  --lock\n");
	printf("        Enable flash readback protection\n");
	printf("  --fsr[=new_value]\n");
	printf("        Reads or writes FSR status register (hex value)\n");
	printf("  --read-ip <filename>\n");
	printf("        Reads InfoPage contents and write to filaname\n");
	printf("  --write-ip <filename>\n");
	printf("        Writes InfoPage contents from filename\n");
	printf("  --erase-all\n");
	printf("        Erases flash memory and InforPage!!!\n");
}

int main(int argc, char *argv[])
{
	uint8_t bus = 0, devnum = 0;

	struct option long_options[] = {
		{"help",	no_argument,		0, 'h'},
		{"device",	required_argument,	0, 'd'},
		{"read-flash",	required_argument,	0, 'r'},
		{"write-flash",	required_argument,	0, 'w'},
		{"erase-flash",	no_argument,		0, 'c'},
		{"lock",	no_argument,		0, 'x'},
		{"fsr",		optional_argument,	0, 1},
		{"read-ip",	required_argument,	0, 2},
		{"write-ip",	required_argument,	0, 3},
		{"erase-all",	no_argument,		0, 4},
		{0, 0, 0, 0}
	};

	while (1) {
		int c;

		c = getopt_long(argc, argv, "hd:r:w:cx", long_options,
								NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'd': // device
			if (sscanf(optarg, "%hhu-%hhu", &bus, &devnum) != 2) {
				fprintf(stderr, "invalid USB device\n");
				return EXIT_FAILURE;
			}

			cmd_device(bus, devnum);
			break;
		case 'r': // read flash
			if (!spi_started)
				cmd_device(0, 0);

			cmd_read_flash(optarg);
			break;
		case 'w': // write flash
			if (!spi_started)
				cmd_device(0, 0);

			cmd_write_flash(optarg);
			break;
		case 'c': // erase flash
			if (!spi_started)
				cmd_device(0, 0);

			cmd_erase_flash();
			break;
		case 'x': // lock flash
			if (!spi_started)
				cmd_device(0, 0);

			cmd_lock();
			break;
		case 1: // read/write fsr
			if (!spi_started)
				cmd_device(0, 0);

			if (optarg) {
				char *check = NULL;
				unsigned long int value;

				value = strtoul(optarg, &check, 16);
				if (strlen(check) || value > 0xff) {
					fprintf(stderr, "invalid FSR value\n");
					return EXIT_FAILURE;
				}
				cmd_write_fsr(value);
			} else
				cmd_read_fsr();
			break;
		case 2: // read info page
			if (!spi_started)
				cmd_device(0, 0);

			cmd_read_ip(optarg);
			break;
		case 3: // write info page
			if (!spi_started)
				cmd_device(0, 0);

			cmd_write_ip(optarg);
			break;
		case 4: // erase all
			if (!spi_started)
				cmd_device(0, 0);

			cmd_erase_all();
			break;
		default:
			cmd_show_usage(argv[0]);
			return EXIT_SUCCESS;
		}
	}

	if (spi_started)
		spi_end();

	return EXIT_SUCCESS;
}

