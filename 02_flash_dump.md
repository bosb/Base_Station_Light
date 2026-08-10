# serial output from 8 pin header tx/rx:

```
ESP-ROM:esp32c3-api1-20210207
Build:Feb  7 2021
rst:0x1 (POWERON),boot:0xd (SPI_FAST_FLASH_BOOT)
SPIWP:0xee
mode:DIO, clock div:1
load:0x3fcd5810,len:0x438
load:0x403cc710,len:0x918
load:0x403ce710,len:0x25f4
entry 0x403cc710
Version: 1.1.5

MAC: xx:xx:xx:xx:xx:xx
SSID: Tractive_XXXXX
PowerLevel: 0
PowerLevel: 1
PowerLevel: 2
```

# esptool

For further communication install [esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32/)

`$ esptool flash-id`
```
esptool v5.3.1
Connected to ESP32-C3 on /dev/cu.usbmodem101:
Chip type:          ESP32-C3 (QFN32) (revision v0.4)
Features:           Wi-Fi, BT 5 (LE), Single Core, 160MHz, Embedded Flash 4MB (XMC)
Crystal frequency:  40MHz
USB mode:           USB-Serial/JTAG
MAC:                xx:xx:xx:xx:xx:xx

Stub flasher running.

Flash Memory Information:
=========================
Manufacturer: 46
Device: 4016
Detected flash size: 4MB
```

`$ esptool  get-security-info`
```
...
Security Information:
=====================
Flags: 0x00000000 (0b0)
Key Purposes: (0, 0, 0, 0, 0, 0, 12)
  BLOCK_KEY0 - USER/EMPTY
  BLOCK_KEY1 - USER/EMPTY
  BLOCK_KEY2 - USER/EMPTY
  BLOCK_KEY3 - USER/EMPTY
  BLOCK_KEY4 - USER/EMPTY
  BLOCK_KEY5 - USER/EMPTY
Chip ID: 5
API Version: 3
Secure Boot: Disabled
Flash Encryption: Disabled
SPI Boot Crypt Count (SPI_BOOT_CRYPT_CNT): 0x0
```

`$ esptool read-flash 0 ALL flash.bin`

`$ esptool image-info flash.bin`
```
Image size: 4194304 bytes
Detected image type: ESP32-C3

ESP32-C3 Image Header
=====================
Image version: 1
Entry point: 0x403cc710
Segments: 3
Flash size: 4MB
Flash freq: 80m
Flash mode: DIO

ESP32-C3 Extended Image Header
==============================
WP pin: 0xee (disabled)
Flash pins drive settings: clk_drv: 0x0, q_drv: 0x0, d_drv: 0x0, cs0_drv: 0x0, hd_drv: 0x0, wp_drv: 0x0
Chip ID: 5 (ESP32-C3)
Minimal chip revision: v0.0, (legacy min_rev = 0)
Maximal chip revision: v655.35

Segments Information
====================
Segment   Length   Load addr   File offs  Memory types
-------  -------  ----------  ----------  ------------
      0  0x00438  0x3fcd5810  0x00000018  DRAM, BYTE_ACCESSIBLE
      1  0x00918  0x403cc710  0x00000458  IRAM
      2  0x025f4  0x403ce710  0x00000d78  IRAM

ESP32-C3 Image Footer
=====================
Checksum: 0xf0 (valid)
Validation hash: 9eb3ec3d24d3d8f7c3f05247dda7b2474ac976700b65c8ead6e3b9108da0d8f2 (valid)
```

# ESP-Firmware-Toolbox

So whats in the flash?

<https://medium.com/@wilcovanbeijnum/tutorial-hacking-and-patching-firmware-of-esp32-based-iot-devices-c12ba71a6522>
-> <https://github.com/wilco375/ESP-Firmware-Toolbox>

`(venv) $ python3 esp32knife/esp32knife.py --chip=esp32c3 load_from_file flash.bin`

Mentioning only the interesting parts:

## parsed/bootloader.bin.elf
```
Entry point: 403cc710
real partition size: 13216
Segment 1 : len 0x00438 load 0x3fcd5810 file_offs 0x00000018 DRAM,BYTE_ACCESSIBLE
Segment 2 : len 0x00918 load 0x403cc710 file_offs 0x00000458 IRAM
Segment 3 : len 0x025f4 load 0x403ce710 file_offs 0x00000d78 IRAM
Checksum: f0 (valid)
```

## parsed/partitions.csv
```
# ESP-IDF Partition Table
# Name, Type, SubType, Offset, Size, Flags
nvs,data,nvs,0x9000,20K,
otadata,data,ota,0xe000,8K,
app0,app,ota_0,0x10000,1280K,
app1,app,ota_1,0x150000,1280K,
spiffs,data,spiffs,0x290000,1408K,
coredump,data,coredump,0x3f0000,64K,
```

## parsed/part.0.nvs.csv
```
# NVS csv file
key,type,encoding,value
...
sta.ssid,data,base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
sta.pswd,data,base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
...
sta.apsw,data,base64,BQA=
...
ap.ssid,data,base64,DgAAAFRyYWN0aXZlX1hYWFhYAAAAAAAAAAAAA
ap.passwd,data,base64,VGhlU3VuSXNTaGluaW5nVGhlV2VhdGhlcklzU3dlZXQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
...
```
-> **TheSunIsShiningTheWeatherIsSweet**

Used it to connect, but did not get connection with it :-(
because on this the following entry got missing (error #1), which makes connection impossible:
```
ap.pmk,data,base64,N/tL1EBPRGrZeGYMNjkfO7DsEWF5NDDZK/q+kvjCNmI=
```

## parsed/part.2.app0.elf 
```
Entry point: 403821f6
real partition size: 829600
Segment 1 : len 0x236f8 load 0x3c0a0020 file_offs 0x00000018 DROM
Segment 2 : len 0x0398c load 0x3fc8e800 file_offs 0x00023718 DRAM,BYTE_ACCESSIBLE
Segment 3 : len 0x08f64 load 0x40380000 file_offs 0x000270ac IRAM
Segment 4 : len 0x95014 load 0x42000020 file_offs 0x00030018 IROM
Segment 5 : len 0x05840 load 0x40388f64 file_offs 0x000c5034 IRAM
Checksum: 41 (valid)
```

some more strings:
`$ parsed/part.2.app0.elf | more`
```
esp-idf: v4.4.5 ac5d805d0e
arduino-lib-builder
16:38:52
Jun 12 2023
v4.4.5
```

`$ strings parsed/part.2.app0.seg1 | grep BaseStation >` [update.html](https://bosb.github.io/Base_Station_Light/update.html)

Thats my suspect there is OTA hidden, if a connect gets successfull....

