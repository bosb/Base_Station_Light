# No warranty. Everything here is provided as-is. Flashing, re-provisioning, or RF experiments can brick hardware or violate warranties/laws — you are responsible for what you do with it.

Download Pfoten.ino.esp32c3.bin from [Releases](https://github.com/bosb/Base_Station_Light/releases)

Connect to wifi original: Tractive_0xxxx pw: TheSunIsShiningTheWeatherIsSweet

Open: [http://basename.local](http://basename.local)

Select the downloaded file and upload

Wait until leds switch off and on

Press button for 3 seconds, until right led flashes fast 3 times

Connect to new wifi: Pfoten_0xxxx no pw needed for 10 minutes

Open: [http://pfoten.local](http://pfoten.local)

First download 2 files: Full flash (4 MB) and original firmware (800 kB) 
they can be used to switch back to original, if anything goes wrong.
Original firmware file can be used also on this web ota update, full flash would neeed to wire 2 connections on the board with 2 thin wires, no soldering needed.

For security go to /config, set your own ssid + password and for convenience you can connect the device to your wifi.
Save and reboot.

Explore

If anything goes wrong, press button for 3 seconds, AP is available without password for 10 minutes...

