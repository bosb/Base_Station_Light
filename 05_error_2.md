# Error #2 OTA: FAIL: Flash Read Failed
Another error along the way came up:
I could use the OTA webpage one time, using the original firmware binary.
After that all successive OTAs failed with webresponse FAIL and serial log Flash Read Failed
No reason found, no difference found, fixed by flashing back whole 4MB image taken after first start -
to stay with original firmware, also fixed by flashing custom firmware.
Not a real problem I guess, since the original firmware is in all cases working as a WIFI beacon, the original function got not broken, yet.
For custom experiments the 2 conections for the USB data need to be made in some cases - be prepared.

One idea was the flash frequency reduce from 80MHz tu 40MHz, but that didn't convince me, since the OTA was alwayss working with custom firmware....

I dumped whole flash after the failure, but bits in ota slot matched the bits uploaded...

