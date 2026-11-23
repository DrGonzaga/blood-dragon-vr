BLOOD DRAGON VR  -  version <version>
(mod build <bd>. Beta, like its author.)
A full VR mod for Far Cry 3 Blood Dragon: 6DoF head tracking, motion-controlled guns, VR hands,
bow, throwing, a synthwave VR menu and lots more.

WHAT YOU NEED
* Far Cry 3 Blood Dragon on Steam, updated, and started once without the mod.
  Other versions (Ubisoft Connect, GOG, old patches) are untested; Setup warns you if yours differs.
* A PC VR headset with a 32-bit OpenXR runtime. Tested on Quest 3 with Virtual Desktop
  (set Virtual Desktop's OpenXR runtime to VDXR).
* Windows 10 or 11.

INSTALL (EASY WAY)
1. Close the game.
2. Run BloodDragonVR-Setup-<version>.exe. If Windows says "Windows protected your PC",
   click More info, then Run anyway (the program isn't signed). Click Yes on the admin prompt.
3. Choose 1 Install, check the game folder, and press Enter on INSTALL NOW.
4. Read Setup tips (NVIDIA settings matter), then start the game from the new desktop shortcut.

Setup backs up every file it changes into the game's BloodDragonVR\backup folder.
To update, run a newer Setup and choose Install (tick "Keep my current settings" to keep yours).

INSTALL (BY HAND)
Copy everything in the "Manual install\bin" folder into the game's bin folder
(...\steamapps\common\Far Cry 3 Blood Dragon\bin). Optional but recommended: apply a
4 GB (large address aware) patch to bin\fc3_blooddragon_d3d11.exe. Start that exe directly.

UNINSTALL
Run Setup and choose Uninstall, or use Windows Settings > Apps > Blood Dragon VR.
By hand: delete bin\version.dll (or bin\dinput8.dll if Setup used that name), bin\fc3vr.ini,
bin\fc3vr.log, bin\bdvr-prompts.txt and the bin\bdvr-pictures folder.

IN THE GAME
* Click both thumbsticks together to open the VR menu. Its Controls page shows every button.
* "Blood Dragon VR controls.pdf" has the same controls pictures to read before you play.
* The game's button prompts show the VR controls. Their wording is in bin\bdvr-prompts.txt (plain text, editable).
* Settings live in bin\fc3vr.ini. The log is bin\fc3vr.log: please include it with bug reports.

KNOWN LIMITS
* Made and tested on one PC (Quest 3, Virtual Desktop, NVIDIA). Other headsets should work but are
  untested. The picture starts at your headset's recommended size; for a sharper one raise
  Render scale on the VR menu's Graphics page (Save for next start, then restart).
* The game's online features are switched off by the mod (Ubisoft's servers for this game are closed).

CREDITS
Vibe coded with Claude by the submissive lab-rat DrGonzaga
Far Cry 3 Blood Dragon is (c) Ubisoft. This mod contains no game files.
Third-party code and the menu font: see THIRD-PARTY-NOTICES.txt.
