CC = gcc
CFLAGS += $(shell pkg-config libftdi libusb --cflags)
CFLAGS += -g -Wall
#CFLAGS += -O2 -s
LDFLAGS = $(shell pkg-config libftdi libusb --libs)
INCLUDES = 

TARGET = nrf24le1_flasher

SRCS = \
	src/hexfile.c \
	src/main.c \
	src/spi_ft232r.c

HEADERS = \
	src/hexfile.h \
	src/spi.h

OBJS = $(SRCS:.c=.o)

.PHONY: depend clean

all: $(TARGET)

$(TARGET): $(OBJS) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TARGET) $(OBJS) $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	$(RM) $(OBJS) $(TARGET)

depend: $(SRCS)
	makedepend $(INCLUDES) $^
