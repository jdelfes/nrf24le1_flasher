nRF24LE1 Flasher
================

This software is used to read, write or erase flash memory of Nordic nRF24LE1
chips. Product information can be found at [Nordic site][1].
If you are looking for a free SDK to programming that chip, you can find a good
one from Brennen at [his blog][2]

Right now, we are using a FTDI FT232R serial<->usb chip to send SPI commands to
nRF24LE1. But it's very simple to port to another platforms like Raspberry PI,
Beaglebone etc. Product information can be found at [ftdi site][3].


nRF24LE1 pinout
---------------

The pinout of this product it's very dependent of your manufacturer design, but
the common ones on chinese sites are the 24 pins format with pins like this:

|Left pins|  Right pins  |
|---------|--------------|
|   3v3   | P0.2 - FSCK  |
|   RESET | P0.3 - FMOSI |
|   RXEN  | P0.4 - FMISO |
|   P0.0  | P0.5 - FCSN  |
|   P0.1  | P0.6         |
|   PROG  | GND          |

The 3v3 is the pin 1 (normally have a square around it).

If you have another design or chip version, please make sure you are following
that pins (extracted from nRF24LE1 spec):

|       | 24pin-4x4 | 32pin-5x5 | 48pin-7x7 |
|-------|-----------|-----------|-----------|
| FCSN  |    P0.5   |    P1.1   |    P2.0   |
| FMISO |    P0.4   |    P1.0   |    P1.6   |
| FMOSI |    P0.3   |    P0.7   |    P1.5   |
| FSCK  |    P0.2   |    P0.5   |    P1.2   |


FTDI FT232R
-----------

With this chip, we have 8 "GPIOs" to do a bitbang SPI implementation. You need
to check if your breakout can configure to 3v3 operation.
We hook each pin of nRF24LE1 on FT232R as described:

| nRF24LE1 | FT232R |
|----------|--------|
|   FCSN   |   RXD  |
|   FMISO  |   RTS  |
|   FMOSI  |   CTS  |
|   FSCK   |   DTR  |
|   RESET  |   DSR  |
|   PROG   |   DCD  |
|   GND    |   GND  |
|   3v3    |  VCCIO |

Note that the pin FCSN is used as TX too, so we can hook RX on TXD of FT232R and
we have serial already.


Dependencies
------------

The only dependency is libftdi1 and libusb-1.0.


Usage
-----

First hook all pins on nRF24LE1 and FT232R, so plug on USB and type on terminal:

```
$ lsusb -d 0403:6001
```

It will list every FTDI device connected at your PC, something like:

```
Bus 004 Device 048: ID 0403:6001 Future Technology Devices International, Ltd FT232 USB-Serial (UART) IC 
```

If you see more that one entry, please go to "Multiple FTDI Devices" section.

You need permission to access that USB device. You can become root, use sudo or
change permission of /dev/bus/usb/$BUS/$DEV specific.

The first task to do is a backup of your InfoPage!!!

```
# nrf24le1_flasher --read-ip ip_bkp.img
```

Just make sure that ip_bkp.img have 512 bytes, so you can save that file as your
personal backup.

Now to write a new program:

```
# nrf24le1_flasher --erase-all --write-flash my_program.hex --write-ip ip_bkp.img
```

This command clear all memory, writes your program and restore your InfoPage
that you saved before.

If you want to know other options, use the "--help" parameter.


Multiple FTDI Devices
---------------------

If you have more than one FTDI plugged, you need to choose which one to be used.
Type this command on terminal:

```
$ lsusb -t
```

It will list devices like:

```
/:  Bus 04.Port 1: Dev 1, Class=root_hub, Driver=uhci_hcd/2p, 12M
    |__ Port 2: Dev 48, If 0, Class=Vendor Specific Class, Driver=ftdi_sio, 12M
```

Now we know the bus (4) and the port (2) of our FTDI device, so we can pass it
to nrf24le1_flasher:

```
# nrf24le1_flasher -d 4-2 --read-ip ip_bkp.img
```

Another programmers/flashers
----------------------------

There are some options over there, like a [kernel module][4] from Eder, a
[Raspberry PI version][5] from Derek and an [Arduino programmer][6] from Dean.


[1]: http://www.nordicsemi.com/eng/Products/2.4GHz-RF/nRF24LE1
[2]: http://blog.diyembedded.com/2010/06/nrf24le1-sdk-for-sdcc.html
[3]: http://www.ftdichip.com/Products/ICs/FT232R.htm
[4]: https://github.com/hltrd/nrf24le1
[5]: https://github.com/derekstavis/nrf24le1-libbcm2835
[6]: https://github.com/DeanCording/nRF24LE1_Programmer

