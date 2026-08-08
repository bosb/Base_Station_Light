# Base_Station_Light V1.1
🧐 Reverse Engineering 🐾 ESP32-C3 Base Station 🛜 TheSunIsShiningTheWeatherIsSweet ☼

[Target](https://www.ecosia.org/search?q=base+station+kostenlos): ESP32-C3 FH4

Provides a dummy wifi access point for another device, that will go into powersave mode 🤷.

My goals:
1. figure out the WPA2 password from the running firmware
2. 3 LED GPIOs + switch GPIO ✅ [Arduino Demo](Button.ino)
3. the 2 ? on the 8 pin header (1/2)
4. ✅ get ota working, so no device opening needed
5. provide alternative firmware

Steps taken so far:

![Open](open.png)
![Board](board.png)

- ✅ for now to get first contact: [Enable USB-Serial/JTAG](01_USB_JTAG.md)
- ✅ [read flash](02_flash_dump.md)
- 😩 [decompile](03_decompile.md)
- ✅ strange error, AP start, but not possible to connect [error_1](04_error_1.md)
- 😕 another, OTA just works one time with stock firmware [error_2](05_error_2.md)
- document/specify the firmware [reverse engineering](06_reverse_engineering.md)
- provide alternative firmware sketch, better/a little more secure 

# No warranty. Everything here is provided as-is. Flashing, re-provisioning, or RF experiments can brick hardware or violate warranties/laws — you are responsible for what you do with it.


