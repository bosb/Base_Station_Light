# Base_Station_Light V1.1
🧐 Reverse Engineering 🐾 ESP32-C3 Base Station 🛜 TheSunIsShiningTheWeatherIsSweet ☼

[Target](https://www.ecosia.org/search?q=base+station+kostenlos): ESP32-C3 FH4

Provides a dummy wifi access point for another device, that will go into powersave mode 🤷.

My goals:
a. figure out the WPA2 password from the running firmware
b. 3 LED GPIOs + switch GPIO
c. the 2 ? on the 8 pin header
d. get ota working, so no device opening needed

Steps taken so far:

![Board](board.png)
![Open](open.png)

- ✅ for now to get first contact: [Enable USB-Serial/JTAG](01_USB_JTAG.md)
- read flash
- decompile
