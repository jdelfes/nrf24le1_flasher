#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "spi.h"
#include "nrf_flash.h"

static uint8_t sfr = 0;

static uint8_t memory[PAGE_SIZE * PAGES_CNT] = {0};
static uint8_t ip[PAGE_SIZE] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa , 0x80, 0x81, 0x82, 0x83, 0x84, 0};

static void program(const uint16_t address, const size_t size, const uint8_t *data)
{
	uint8_t *ptr = sfr & FSR_INFEN ? ip : memory;
	for (size_t i = 0; i < size; i++) {
		ptr[address + i] = ptr[address + i] & data[i];
	}
}

int spi_begin(uint8_t bus, uint8_t port)
{
	fprintf(stderr, "Warning! SPI EMULATOR is used, no operations with real NRF device are performed.\n");
	return 0;
}

void spi_end()
{
	fprintf(stderr, "Warning! SPI EMULATOR is used, no operations with real NRF device are performed.\n");
}

int spi_transfer(uint8_t *bytes, size_t size)
{
    uint8_t *ptr = sfr & FSR_INFEN ? ip : memory;

	switch (bytes[0])
	{
	case WREN:
		sfr |= FSR_WEN;
		break;
	case WRSR:
		sfr = bytes[1];
		break;
	case RDSR:
		bytes[1] = sfr & ~FSR_RDYN;
		break;
	case ERASE_PAGE:
        memset(&ptr[PAGE_SIZE * bytes[1]], 0xff, PAGE_SIZE);
		break;
	case ERASE_ALL:
		memset(&memory[0], 0xff, sizeof(memory));
		memset(&ip[0], 0xff, sizeof(ip));
		break;
	case PROGRAM:
		program(bytes[1]<<8 | bytes[2], size - 3, &bytes[3]);
		break;
	case READ:
        memcpy(bytes + 3, &ptr[(bytes[1]<<8 | bytes[2])], size - 3);
		break;
	default:
		printf("Unknown command 0x%x\n", bytes[0]);
	}

	return size;
}

int spi_transfer_sg(uint8_t op, uint16_t address, uint8_t *bytes, size_t size)
{
	switch (op)
	{
	case PROGRAM:
		program(address, size, bytes);
		break;
	case READ:
		{
            uint8_t *ptr = sfr & FSR_INFEN ? ip : memory;
			memcpy(bytes, &ptr[address], size);
		}
		break;
	}

	return size;
}
