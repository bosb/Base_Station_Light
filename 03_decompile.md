# Disassemble, .... red hering .... frustration ...

Since the passphrase did not work for me to identfy to WIFI from device, I thought the passphrase found is somehow modified in the code before starting the acess point...

Spoiler 🤫: it is not - it is the passphrase, but data on my device is corrupt, so startup of AP uses unknown passphrase...
AP available, connection impossible....

🚧 So the detour here leads to nothing usefull, some personal learninngs, frustrations....

## Ghidra
I always wanted to use/understand ghidra
Take from https://github.com/NationalSecurityAgency/ghidra/releases
Ghidra 11.3.1
because we deppend on:
https://github.com/saibotk/ghidra-esp32-flash-loader/releases
v1.1.0
which is build for 11.3.1 and I did not manage to build it myself.

Follow instructions from https://github.com/saibotk/ghidra-esp32-flash-loader#installation

Ghidra will complain about some tools, which are untrusted....
(I use a Mac)
```
$ xattr -w com.apple.lastuseddate\#PS E\?pj  ~/Downloads/ghidra_12.1.2_PUBLIC/GPL/*/os/mac_arm_64/*
$ xattr -w com.apple.lastuseddate\#PS E\?pj  ~/Downloads/ghidra_12.1.2_PUBLIC/Ghidra/Features/*/os/mac_arm_64/*
```

Create project in ghidra, import firmware.bin

Good luck ;-)

## Rebuild firmware parts with Arduino IDE
The original firmware is also created with Arduino ESP as the found strings indicate,
i also build an example, attached, to see how the config/starting of softAP looks like.
🤓☝️ Idea was to compare the known decompile with symbols to the naked firmware from the device...

I always found ssid/passphrase strings, but not a real reference in the code that/where they will be used...

Somehow lost, for 2 days, tried to find some tutorials, similar problems, develop a structured plan, but not very motivated....🤕

## Opencode 'KI'
Then I just gave up and handed over to [opencode](https://opencode.ai/), throwed the firmware at it, explained I want to get the wifi passphrase and the call that setup/config the AP.
I give always access to cli tools, started in a messy directory with the demo project, the flash dumps, directory of 
https://github.com/wilco375/ESP-Firmware-Toolbox

## Update: 
Oh, my §"$"$"§§2 🤬, default suggestion on ghidra import elf file brought me to a complete wrong track.
I was confused by ?? and assembler not matching gaps.
Import suggests its Xtensa:LE:32:default:default 🤔❓
While watching opencode I saw its diassembling with RISCV - asked about it - answer ESP32 is Xtensa, but ESP32C3 is RISCV 😲
-> RISCV:LE:32:default:gcc
Now gives much better result 😡 - even the ssid strings I searched have now usage reference 💥
