Installing AVGN 8-bit on PS Vita
You need: your own copy of the game on Steam (Windows version), a Windows PC, and a Vita with HENkaku/Ensō and VitaShell. About 350 MB free on the Vita’s memory card.

Step 1: convert your game files (on the PC, once)

Unzip the download. Copy AVGN8bit-Vita-Patcher.bat into your game folder. In Steam: right-click the game → Manage → Browse local files.
Double-click it. If Windows says “Windows protected your PC”, click More info → Run anyway.
Wait about 3–10 minutes, and don’t click inside the window. It makes a new folder called AVGN8bit next to the patcher. Your game files aren’t changed.
Step 2: put it on the Vita

In VitaShell, press Select for USB mode and connect the Vita to the PC.
Copy the AVGN8bit folder into ux0:data/. You should end up with ux0:data/AVGN8bit/data_vita.win.
Copy AVGN8bit-Vita-1.10.vpk anywhere on the memory card. Press Select to leave USB mode, then select the VPK in VitaShell and install it.
Step 3: play
Start AVGN 8-bit from the home screen. The first start takes about 25 seconds, with a loading icon in the corner.

If it doesn’t start

It closes right away: ur0:data/libshacccg.suprx is missing. Install ShaRKBR33D from VitaShell, connect to Wi-Fi, and run it once.
“The game files are missing”: the folder isn’t at ux0:data/AVGN8bit, or data_vita.win is inside a subfolder.
The patcher says “not the supported version”: let Steam verify the game files (Properties → Installed Files → Verify), then run it again.
Saves and settings are kept in ux0:data/AVGN8bit (folders Save and Settings).
