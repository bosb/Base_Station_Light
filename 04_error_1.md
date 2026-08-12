## Error #1 not existing ap.pmk in NVS
Opencode figured it out ... came to the same conclusion I got; I told it, that that passphrase does not work.
Then I gave access to the device, told not to modify bytes on it...
Then I gave it a second device, where I also tested already some own code, which I allowed full write access.
Then for about 6h it went on, tried different things, I learned while watching, some interesting findings along the way.
It started using ghidra cli....also some insights for me...

Then I had the idea to fire up a third device, suprisingly I could connect to it, I got the update page - WHUT? 😵‍💫
So I could point out to opencode what is wrong there and that the passphrase should work.
That put the efford to a new focus.
It was then playing with both devices.

Somehow the 'broken' device was missing ap.pmk in NVS. It would be written, if ap.ssid in NVS would be different than the computedone based on Mac address;
Since ssid was right, no need to rewrite, no pmk, no connection possible...selfhealing not triggered.

Opencode developed a nice tooling for writing valid entries in NVS.

Better on to next section: What do we need to know to develop an application that behaves like this existing firmware, and where should it be improved.
...but before I stumbled about error #2

Update:
Also if the custom firmware if flashed via OTA on to the original firmware, If I would keep the same ssid + challenge, a connection is not possible.
I would have to change the challenge then, or just start without challenge set.
Not further investigated, workaround is to set no challenge on long button press, to get in.
