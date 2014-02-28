#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "hexfile.h"

static uint8_t checksum(const char *line, int size)
{
	int i;
	uint8_t byte, res = 0;

	for (i = 0; i < size; i += 2) {
		byte = 0;
		sscanf(line + i, "%02hhx", &byte);
		res += byte;
	}

	return 0x100 - res;
}

int hexfile_getline(FILE *fd, uint16_t *address, uint8_t *dest, size_t n)
{
	char line[522];
	uint8_t count = 0, type = 0, cksum = 0;
	int r, i;

	fgets(line, sizeof(line), fd);

	if (line[0] != ':') {
		fprintf(stderr, "line can't be parsed: %s\n", line);
		return -1;
	}

	r = sscanf(line, ":%02hhx%04hx%02hhx", &count, address, &type);
	if (r != 3) {
		fprintf(stderr, "line parse error: %s\n", line);
		return -2;
	}

	if (count > n) {
		fprintf(stderr, "internal buffer too small to handle: %s\n",
									line);
		return -3;
	}

	if (strlen(line) < 11 + count) {
		fprintf(stderr, "line too small: %zu, should be %i\n",
						strlen(line), 11 + count);
		return -4;
	}

	// test checksum
	r = sscanf(line + 9 + 2 * count, "%02hhx", &cksum);
	if ((r != 1) || (checksum(line + 1, 2 * count + 8) != cksum)) {
		fprintf(stderr, "checksum error: %s\n", line);
		return -5;
	}

	if (type == 0) { // data record
		for (i = 0; i < count; i++) {
			uint8_t b;

			r = sscanf(line + 9 + i * 2, "%02hhx", &b);
			if (r != 1) {
				fprintf(stderr, "data error: %s\n", line);
				return -6;
			}

			dest[i] = b;
		}
		return count;
	}

	if (type != 1) { // unknown type, not implemented
		fprintf(stderr, "unknown type on line: %s\n", line);
		return -666;
	}

	return 0;
}

