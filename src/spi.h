#ifndef SPI_H
#define SPI_H

int spi_begin(uint8_t bus, uint8_t port);
void spi_end();
int spi_transfer(uint8_t *bytes, size_t size);


#endif // SPI_H
