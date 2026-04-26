To see debug log messages in Citra:

1) Make sure log level set to "Debug.Emulated:Debug"

---

Commands used to generate the .bin files:

`bannertool makebanner -i banner.png -a audio.wav -o banner.bin`

`bannertool makesmdh -s CavFX -l CavFX -p YetRunnerGamez -i icon.png -o icon.bin`

----

Debug log messages output to debug service, so may be possible to see from console via remote gdb

----

The 3DS port reads resources from `sdmc:/3ds/ClassiCube/`.
Building copies `src/preload` to `build/3ds/sdroot/3ds/ClassiCube`.

When testing in Azahar, pass `AZAHAR_SD_ROOT` to also copy preload files directly to Azahar's SD card folder:

`make -f misc/3ds/Makefile AZAHAR_SD_ROOT="/path/to/azahar/sdmc"`

`EMU_SD_ROOT` also works as a generic emulator SD card path.
