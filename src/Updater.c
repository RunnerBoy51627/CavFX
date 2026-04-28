#include "Core.h"
#include "Logger.h"
#include "Updater.h"

#define CAVFX_REPO_URL "https://github.com/RunnerBoy51627/CavFX/releases/latest"

#if defined CC_BUILD_WIN
#include <windows.h>
#endif

#if defined CC_BUILD_MACOS
#include <stdlib.h>
#endif

#if defined CC_BUILD_LINUX
#include <stdlib.h>
#endif

void Updater_OpenLatestRelease(void) {
#if defined CC_BUILD_WIN
	ShellExecuteA(NULL, "open", CAVFX_REPO_URL, NULL, NULL, SW_SHOWNORMAL);

#elif defined CC_BUILD_MACOS
	char cmd[512];
	String_Format1(cmd, sizeof(cmd), "open \"%c\"", CAVFX_REPO_URL);
	system(cmd);

#elif defined CC_BUILD_LINUX
	char cmd[512];
	String_Format1(cmd, sizeof(cmd), "xdg-open \"%c\"", CAVFX_REPO_URL);
	system(cmd);

#elif defined CC_BUILD_WII
	Logger_SimpleLog("Updater: Wii auto-update not supported yet. Download manually from GitHub.");

#elif defined CC_BUILD_3DS
	Logger_SimpleLog("Updater: 3DS CIA updater not implemented yet. Planned: download CIA, install, then exit to HOME Menu.");

#else
	Logger_SimpleLog("Updater: Unsupported platform.");
#endif
}

void Updater_ShowUpdateInfo(void) {
	Logger_SimpleLog("Checking updates: open latest CavFX release page.");
	Updater_OpenLatestRelease();
}