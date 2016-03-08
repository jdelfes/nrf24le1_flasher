#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include "hexfile.h"
#include "spi.h"

#include "nrf_flash.h"

#define USE_HIGHEST_POSSIBLE_OFFSET	0xffff

#define HIGHEST_POSSIBLE_STRING "highest-possible"
#define MATCH_WITH_OFFSET_STRING "match-with-offset"


static int spi_started = 0;
static bool verbose = false;
static char directory_backup[FILENAME_MAX] = ".";

static const char *id_as_string(uint8_t *id)
{
	static char ids[CHIP_ID_SIZE*3+1];
	snprintf(ids, sizeof(ids), "%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8,
			 id[0], id[1], id[2], id[3], id[4]);
	return ids;
}

static int write_ip_backup(uint8_t *ip)
{
	char filename[FILENAME_MAX];
	char str[32];
	struct tm *tm_time;
	time_t rawtime;

	time ( &rawtime );
	tm_time = localtime ( &rawtime );

	strftime(str, sizeof(str), "%Y%m%d_%H%M%S", tm_time);

	sprintf(filename, "%s/ip_backup-%s.%s.bin",
			directory_backup, str, id_as_string(ip+CHIP_ID_OFFSET));

	int out = open(filename,  O_WRONLY | O_CREAT, 0644);
	if (out == -1) {
		perror("open");
		return -3;
	}
	ssize_t w = write(out, ip, PAGE_SIZE);
	close(out);
	if (w != PAGE_SIZE) {
		printf("Size of wrote info page is invalid, is %zu, should be %d\n", w, PAGE_SIZE);
		return -6;
	}
	return 0;
}

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


static int read_ip_write_backup(void)
{
    uint8_t from_flash[PAGE_SIZE];
    if (!spi_transfer_sg(READ, 0, &from_flash[0], sizeof(from_flash))) {
        fprintf(stderr, "something wrong reading InfoPage\n");
        exit(EXIT_FAILURE);
    }
    return write_ip_backup(from_flash);
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

static void cmd_device(uint8_t bus, uint8_t port)
{
	if (spi_started)
		spi_end();

	if (spi_begin(bus, port) != 0) {
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

	f = fopen(filename, "wb");
	if (!f) {
		fprintf(stderr, "can't open %s to write\n", filename);
		exit(EXIT_FAILURE);
	}
	fwrite(buf + 3, sizeof(buf) - 3, 1, f);
	fclose(f);
}

struct wri_data {
	int count;
};

struct nrf_page {
	bool is_erase_needed;
	bool is_write_needed;

	unsigned int bytes_to_write;

	struct wri_data writes[PAGE_SIZE];
};

void show_writes(struct nrf_page *pages)
{
	for (int j = 0; j < PAGES_CNT; j++) {
		printf("page %d, bytes to write %d\n", j, pages[j].bytes_to_write);
		if (pages[j].bytes_to_write == 0)
			continue;
		printf("   ");
		for (int i = 0; i < PAGE_SIZE; i++) {
			int cnt = pages[j].writes[i].count;
			if (cnt == 0) {
				printf("... ");
			} else {
				printf("%03d ", cnt);
			}
			if (i != 0 && i % 32 == 31)
				printf("\n   ");
			
			if (cnt > 0) {
				for (int j = 0; j < cnt - 1; j++) {
					printf("*** ");
					
					i++;
					if (i != 0 && i % 32 == 31)
						printf("\n   ");
				}
			}
		}
		printf("\n");
	}
	printf("\n");
}

static void compact_writes(struct nrf_page *pages)
{
	for (int j = 0; j < PAGES_CNT; j++) {
		for (int prev = 0;;) {
			int current = prev + pages[j].writes[prev].count;
			if (current >= PAGE_SIZE)
				break;
			if (pages[j].writes[current].count != 0) {
				pages[j].writes[prev].count += pages[j].writes[current].count;
				pages[j].writes[current].count = 0;
			} else {
				for (prev = current; prev < PAGE_SIZE; prev++) {
					if (pages[j].writes[prev].count != 0)
						break;
				}
				if (prev == PAGE_SIZE)
					break;
			}
		}
	}
}

static int write_page_smart(struct nrf_page *page, int no, uint8_t *storage,
                            bool create_backup)
{
	uint8_t from_flash[PAGE_SIZE];
	uint16_t address = PAGE_SIZE * no;

	printf("* page %d, to write %u\n", no, page->bytes_to_write);
	
	printf("   checking, if write is needed... ");
	if (!spi_transfer_sg(READ, address, from_flash, sizeof(from_flash))) {
		fprintf(stderr, "SPI error during READ page %d\n", no);
		return -1;
	}

	// Check if write is needed.
	for (int i = 0; i < PAGE_SIZE; i++) {
		if (memcmp(&storage[i], &from_flash[i], page->writes[i].count) != 0) {
			page->is_write_needed = true;
			break;
		}
	}
	
	if ( ! page->is_write_needed ) {
		printf("write is not needed\n");
		return 0;
	}
	printf("write is needed\n");
	
	// Check if erase page is needed.
	printf("   checking, if erase is needed... ");
	for (int i = 0; i < PAGE_SIZE; i++) {
		for (int j = 0; j < page->writes[i].count; j++) {
			int pos = i + j;
			if (~from_flash[pos] & storage[pos]) {
				page->is_erase_needed = true;
				break;
			}
		}
	}

    if (create_backup) {
        write_ip_backup(from_flash);
    }

	// Erase page
	if (page->is_erase_needed) {
		printf("erase is needed\n");

		printf("   erasing... ");
		
		enable_wen();

		uint8_t cmd[] = {ERASE_PAGE, no};
		if (!spi_transfer(cmd, sizeof(cmd))) {
			fprintf(stderr, "SPI error\n");
			return -1;
		}
		wait_ready();
		printf("OK\n");
	} else
		printf("erase is not needed\n");

	// Prepare page data to write
	for (int i = 0; i < PAGE_SIZE; i++) {
		memcpy(&from_flash[i], &storage[i], page->writes[i].count);
	}

	printf("   writing... ");
	enable_wen();
	if (!spi_transfer_sg(PROGRAM, address, from_flash, sizeof(from_flash))) {
		fprintf(stderr, "SPI error during PROGRAM page %d\n", no);
		return -1;
	}
	wait_ready();
	printf("OK\n");

	printf("   verifying... ");
	uint8_t comp[PAGE_SIZE];
	if (!spi_transfer_sg(READ, address, comp, sizeof(comp))) {
		fprintf(stderr, "SPI error during READ page %d\n", no);
		return -1;
	}
	
	if (memcmp(from_flash, comp, PAGE_SIZE) != 0) {
		fprintf(stderr, "%s: error checking memory\n", __FUNCTION__);
		return -1;
	}
	printf("OK\n");

	return 0;
}

static int set_nupp(uint8_t nupp)
{
    struct nrf_page page = {0};
    uint8_t storage[PAGE_SIZE];

    page.bytes_to_write = 1;
    page.writes[NUPP_OFFSET].count = 1;
    storage[NUPP_OFFSET] = nupp;

    enable_infen();
    int ret = write_page_smart(&page, 0, storage, true);
    disable_infen();
    return ret;
}

static void cmd_write_smart_flash(const char *filename, uint16_t address_offset, 
                  bool extra_verification, bool dont_check_nupp, bool nupp_match_with_offset)
{
	if (check_rdismb()) {
		fprintf(stderr, "flash memory is protected\n");
		exit(EXIT_FAILURE);
	}
	
	uint8_t nupp;
	if (dont_check_nupp) {
		nupp = PAGES_CNT;
	} else {
		enable_infen();
		if (!spi_transfer_sg(READ, NUPP_OFFSET, &nupp, sizeof(nupp))) {
			fprintf(stderr, "SPI error during NUPP READ\n");
			exit(EXIT_FAILURE);
		}
		disable_infen();

		if (nupp > PAGES_CNT)
			nupp = PAGES_CNT;

		printf("Size limit based on NUPP for flashing image is %u bytes.\n", PAGE_SIZE * nupp);
	}

	FILE *fd = fopen(filename, "r");
	if (!fd) {
		fprintf(stderr, "can't open %s to read\n", filename);
		exit(EXIT_FAILURE);
	}

	int count;
	uint16_t address;
	uint8_t storage[FLASH_SIZE];
	if (address_offset == USE_HIGHEST_POSSIBLE_OFFSET) {
		uint16_t bin_size = 0;
		while ((count = hexfile_getline(fd, &address, storage, sizeof(storage), false, 0)) > 0) {
			uint16_t v = address + count;
			if (bin_size < v) {
				bin_size = v;
			}
		}
		
		address_offset = FLASH_SIZE - (bin_size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
		fseek(fd, 0, SEEK_SET);
		printf("address_offset 0x%x\n", address_offset);
	}
	
	struct nrf_page pages[PAGES_CNT];
	memset(pages, 0, sizeof(pages));
	while ((count = hexfile_getline(fd, &address, storage, nupp * PAGE_SIZE, true, address_offset)) > 0) {
		int page_no = address>>9;
		int offset = address & 0x1FF;
		int over = count - (PAGE_SIZE - offset);
		
		if (over > 0) {
			count -= over;
			pages[page_no + 1].writes[0].count = over;
			pages[page_no + 1].bytes_to_write += over;
		}

		pages[page_no].writes[offset].count = count;
		pages[page_no].bytes_to_write += count;
	}
	fclose(fd);

	if (count < 0) {
		fprintf(stderr, "hexfile_getline failed with error %d\n", count);
		exit(EXIT_FAILURE);
	}
	
	if (verbose)
		show_writes(pages);

	compact_writes(pages);

	if (verbose)
		show_writes(pages);

	for (int j = 0; j < PAGES_CNT; j++) {
		if (pages[j].bytes_to_write) {
            if (write_page_smart(&pages[j], j, &storage[PAGE_SIZE * j], false) != 0)
				exit(EXIT_FAILURE);
		}
	}
	
	// Extra verification to check if new method gives the same result as 'classic' one.
	if (extra_verification) {
		printf("Extra verification - comparison results of smart write method and 'classic' one...\n");
		FILE *fd = fopen(filename, "r");
		if (!fd) {
			fprintf(stderr, "can't open %s to read\n", filename);
			exit(EXIT_FAILURE);
		}
		
		uint8_t buffer[255];
		int count;
		uint16_t address;
		unsigned int errors_cnt = 0;
		while ((count = hexfile_getline(fd, &address, buffer, sizeof(buffer), false, 0)) > 0) {
			address += address_offset;
			
			uint8_t from_flash[255];
			if (!spi_transfer_sg(READ, address, from_flash, count)) {
				fprintf(stderr, "SPI error\n");
				fclose(fd);
				exit(EXIT_FAILURE);
			}
			
			if (memcmp(buffer, from_flash, count) != 0) {
				fprintf(stderr, "Verification failed - address %x, count %d (with offset added)\n", address, count);
				errors_cnt++;
			}
		}
		fclose(fd);

		if (errors_cnt == 0) {
			printf("Verification succeeded.\n");
		} else {
			printf("Verification failed with %u errors. It looks like internal program bug.\n"
				"Please use 'classic' method: --erase-flash --write-flash instead.\n", 
				errors_cnt);
		}
	}
	
	if (nupp_match_with_offset) {
		set_nupp(address_offset / PAGE_SIZE);
	}
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
						sizeof(buffer) - 3, false, 0)) > 0) {
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

	f = fopen(filename, "wb");
	if (!f) {
		fprintf(stderr, "can't open %s to write\n", filename);
		exit(EXIT_FAILURE);
	}
	fwrite(buf + 3, sizeof(buf) - 3, 1, f);
	fclose(f);
}

static void cmd_write_ip(const char *filename, bool dont_check_id)
{
	FILE *f;
	uint8_t buf[PAGE_SIZE];

	if (check_rdismb()) {
		fprintf(stderr, "flash memory is protected\n");
		exit(EXIT_FAILURE);
	}

	buf[0] = PROGRAM;
	buf[1] = 0x00; // address
	buf[2] = 0x00;

	f = fopen(filename, "rb");
	if (!f) {
		fprintf(stderr, "can't open %s to read\n", filename);
		exit(EXIT_FAILURE);
	}
	fread(buf, sizeof(buf), 1, f);
	fclose(f);

	enable_infen();

	if (!dont_check_id) {
		uint8_t id[CHIP_ID_SIZE];
		if (!spi_transfer_sg(READ, CHIP_ID_OFFSET, &id[0], sizeof(id))) {
			fprintf(stderr, "something wrong writing InfoPage\n");
			exit(EXIT_FAILURE);
		}

		if (memcmp(&id[0], &buf[CHIP_ID_OFFSET], CHIP_ID_SIZE) != 0) {
			fprintf(stderr, "ids don't match\n");
			exit(EXIT_FAILURE);
		}
	}
	if (read_ip_write_backup() != 0) {
		fprintf(stderr, "Creating IP backup file is failed.\n");
		exit(EXIT_FAILURE);
	}

	enable_wen();

	printf("writing InfoPage\n");
	if (!spi_transfer_sg(PROGRAM, 0, buf, sizeof(buf))) {
		fprintf(stderr, "something wrong writing InfoPage\n");
		exit(EXIT_FAILURE);
	}

	wait_ready();
	
	uint8_t from_flash[PAGE_SIZE];
	if (!spi_transfer_sg(READ, 0, &from_flash[0], sizeof(from_flash))) {
		fprintf(stderr, "something wrong reading InfoPage\n");
		exit(EXIT_FAILURE);
	}

	if (memcmp(&from_flash[0], &buf[0], sizeof(from_flash)) != 0) {
		printf("Memory doesn't match.\n");
	}
}

static void cmd_erase_all()
{
	uint8_t cmd[1];

	// we don't want to erase InfoPage
	enable_infen();
	if (read_ip_write_backup() != 0) {
		fprintf(stderr, "Creating IP backup file is failed.\n");
		exit(EXIT_FAILURE);
	}

	enable_wen();

	printf("Erasing all...\n");
	cmd[0] = ERASE_ALL;
	spi_transfer(cmd, sizeof(cmd));

	wait_ready();
}

int get_nrf_config(const char *dbfilename, const char *id, struct infopage_config *cfg)
{
	FILE *fp = fopen(dbfilename, "r");
	if (fp == NULL) {
		perror("Open of database file");
		return -1;
	}

	char fid[30];
	while ( fscanf(fp, "%s %"SCNx8":%"SCNx8":%"SCNx8":%"SCNx8":%"SCNx8" %"SCNx8" %"SCNx8"", fid,
				   &cfg->tx_addr[0], &cfg->tx_addr[1], &cfg->tx_addr[2], &cfg->tx_addr[3], &cfg->tx_addr[4],
				   &cfg->channel, &cfg->power) != EOF)
	{
		if (strcmp(id, fid) == 0) {
			printf("Configuration for <%s>: address %"PRIx8":%"PRIx8":%"PRIx8":%"PRIx8":%"PRIx8", channel %"PRIx8", power %"PRIx8"\n",
				   fid, cfg->tx_addr[0], cfg->tx_addr[1], cfg->tx_addr[2], cfg->tx_addr[3], cfg->tx_addr[4],
					cfg->channel, cfg->power);
			fclose(fp);
			return 0;
		}
	}
	fclose(fp);
	fprintf(stderr, "Error: chip id is not found in database.\n"
					"Please add entry for device %s to %s file.\n", id, dbfilename);
	return -2;
}

static const char *cmd_get_id(void)
{
    uint8_t id[CHIP_ID_SIZE];
    enable_infen();
    if (!spi_transfer_sg(READ, CHIP_ID_OFFSET, &id[0], sizeof(id))) {
        fprintf(stderr, "SPI error during CHIP ID read\n");
        exit(EXIT_FAILURE);
    }
    disable_infen();

    return id_as_string(id);
}

static void cmd_write_user_area(const char *filename)
{
    struct infopage_config cfg;

    if (get_nrf_config(filename, cmd_get_id(), &cfg) != 0) {
        exit(EXIT_FAILURE);
    }

    struct nrf_page page = {0};
    uint8_t storage[PAGE_SIZE];

    page.bytes_to_write = sizeof(struct infopage_config);
    page.writes[USER_AREA_OFFSET].count = sizeof(struct infopage_config);
    memcpy(&storage[USER_AREA_OFFSET], &cfg, sizeof(struct infopage_config));

    enable_infen();
    write_page_smart(&page, 0, storage, true);
    disable_infen();
//    return ret;
}

static void cmd_show_usage(const char *name)
{
	printf("Usage: %s [options]\n", name);
	printf("Options:\n");
	printf("  -v, --verbose\n");
	printf("        Show debugging information\n");
	printf("  -d, --device <bus>-<port>\n");
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
	printf("        Reads InfoPage contents and write to filename\n");
	printf("  --write-ip <filename>\n");
	printf("        Writes InfoPage contents from filename\n");
	printf("  --erase-all\n");
	printf("        Erases flash memory and InforPage!!!\n");
	printf("  -s, --write-flash-smart <filename>\n"
			"        Writes flash memory from a filename smartly, i.e. write and erase only if needed.\n");
	printf("  -p, --dont-check-nupp\n"
			"        If set, a tool does not check if HEX data will overwrite a code in protected area.\n");
	printf("  -z, --dont-check-id\n"
			"        If set, a tool does not check if id from ip file is the same as currently in InfoPage.\n");
	printf("  -o, --offset=offset\n"
			"        Set a decimal or hexadecimal (with 0x prefix) value, which should be added to \n"
			"        address read from a HEX file.\n"
			"        If value for offset is '%s', the tool calculates offset itself.\n"
			"        Works only if offset is set BEFORE the option\n", HIGHEST_POSSIBLE_STRING);
	printf("  -n, --set-nupp=nupp\n"
			"        Set a decimal or hexadecimal (with 0x prefix) value of NRF's Number of Unprotected\n"
			"        pages.\n"
			"        If value of nupp is '%s', the tool calculates nupp based on offset,\n"
			"        passed in --offset option.\n", MATCH_WITH_OFFSET_STRING);
	printf("  -e, --extra-verification\n"
			"        if set to 1, comparison results of smart method and classic is performed\n"
			"        Works only if set BEFORE --write-flash-smart option\n");
	printf("  -u, --write-userarea <db_filename>\n"
			"        Write a user area of InfoPage, values are taken from db_filename, where read id is used\n"
			"        as a key.\n");
	printf("  -i, --read-id\n"
			"        read and print on terminal a NRF id from InfoPage area.\n");
	printf("  -b, --backup-dir\n"
			"        path for directory in which, backup file of InfoPage will be saved. Current\n"
			"        directory is a default.\n");
}

int main(int argc, char *argv[])
{
	uint8_t bus = 0, port = 0;
	int offset = 0;
	bool dont_check_nupp = false;
	bool dont_check_id = false;
	bool extra_verification = true;
	bool nupp_match_with_offset = false;
	uint8_t nupp = 0xff;

	struct option long_options[] = {
		{"help",	no_argument,		0, 'h'},
		{"verbose",	no_argument,		0, 'v'},
		{"device",	required_argument,	0, 'd'},
		{"read-flash",	required_argument,	0, 'r'},
		{"write-flash",	required_argument,	0, 'w'},
		{"write-flash-smart",	required_argument,	0, 's'},
		{"extra-verification", required_argument, 0, 'e'},
		{"erase-flash",	no_argument,		0, 'c'},
		{"lock",	no_argument,		0, 'x'},
		{"fsr",		optional_argument,	0, 1},
		{"read-ip",	required_argument,	0, 2},
		{"write-ip",	required_argument,	0, 3},
		{"erase-all",	no_argument,		0, 4},
		{"offset",	required_argument,	0, 'o'},
		{"dont-check-nupp",	no_argument,	0, 'p'},
		{"dont-check-id",	no_argument,	0, 'z'},
		{"set-nupp",	required_argument,	0, 'n'},
		{"write-userarea",	required_argument,	0, 'u'},
		{"read-id",	no_argument,	0, 'i'},
		{"backup-dir", required_argument,	0, 'b'},
		{0, 0, 0, 0}
	};

	while (1) {
		int c;

		c = getopt_long(argc, argv, "hvd:r:w:s:e:cxo:pn:u:ib:z", long_options,
						NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'd': // device
			if (sscanf(optarg, "%hhu-%hhu", &bus, &port) != 2) {
				fprintf(stderr, "invalid USB device\n");
				return EXIT_FAILURE;
			}

			cmd_device(bus, port);
			break;
		case 'v': // verbose
			verbose = true;
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
		case 'o': // offset
			if (strcmp(optarg, HIGHEST_POSSIBLE_STRING) == 0) {
				offset = USE_HIGHEST_POSSIBLE_OFFSET;
			} else if (sscanf(optarg, "0x%x", &offset) == 1) {
			} else if (sscanf(optarg, "%d", &offset) != 1) {
				fprintf(stderr, "offset value '%s' is invalid.\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 's': // write smart flash
			if (!spi_started)
				cmd_device(0, 0);
			
			cmd_write_smart_flash(optarg, offset, extra_verification, dont_check_nupp, nupp_match_with_offset);
			break;
		case 'e': // extra verification
			extra_verification = atoi(optarg);
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

			cmd_write_ip(optarg, dont_check_id);
			break;
		case 4: // erase all
			if (!spi_started)
				cmd_device(0, 0);

			cmd_erase_all();
			break;
		case 'p': // dont check nupp
			dont_check_nupp = true;
			break;
		case 'z': // dont check id
			dont_check_id = true;
			break;
		case 'n': // set nupp
			if (strcmp(optarg, MATCH_WITH_OFFSET_STRING) == 0) {
				nupp_match_with_offset = true;
				dont_check_nupp = true;
			} else if (sscanf(optarg, "0x%" SCNx8, &nupp) == 1
					   || sscanf(optarg, "%" SCNu8, &nupp) == 1)
			{
				if (!spi_started)
					cmd_device(0, 0);

				set_nupp(nupp);
			} else {
				fprintf(stderr, "set-nupp value '%s' is invalid\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'u': // write user area
			if (!spi_started)
				cmd_device(0, 0);

			cmd_write_user_area(optarg);
			break;
		case 'i': // write user area
			if (!spi_started)
				cmd_device(0, 0);

			const char *id = cmd_get_id();
			printf("id: %s\n", id);
			break;
		case 'b': // directory backup
			if (!spi_started)
				cmd_device(0, 0);

			strncpy(directory_backup, optarg, sizeof(directory_backup));
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

