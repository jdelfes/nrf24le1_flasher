#ifndef HEXFILE_H
#define HEXFILE_H

int hexfile_getline(FILE *fd, uint16_t *address, uint8_t *dest, size_t n, bool address_as_offset, uint16_t address_offset);

#endif // HEXFILE_H

