#include "Core.h"
#include "String_.h"
#include "Updater.h"
#include "Chat.h"
#include "Constants.h"
#include "Http.h"
#include "Game.h"

#include <stdio.h>
#include <string.h>

#define CAVFX_REPO_URL    "https://github.com/RunnerBoy51627/CavFX/releases/latest"
#define CAVFX_VERSION_URL "https://raw.githubusercontent.com/RunnerBoy51627/CavFX/master/version.txt"

#if defined CC_BUILD_WIN
#include <windows.h>
#endif

#if defined CC_BUILD_MACOS || defined CC_BUILD_LINUX
#include <stdlib.h>
#endif

static cc_bool updater_checked;
static int updater_req_id = -1;
static struct ScheduledTask2 updater_task;

static void Updater_TrimVersion(char* ver) {
	int len;

	if (!ver) return;
	len = (int)strlen(ver);

	while (len > 0) {
		char c = ver[len - 1];
		if (c != '\r' && c != '\n' && c != ' ' && c != '\t') break;
		ver[--len] = '\0';
	}
}

static cc_bool Updater_CopyVersionText(struct HttpRequest* req, char* dst, int dstSize) {
	int len;

	if (!req || !req->data || !req->size || dstSize <= 1) return false;

	len = (int)req->size;
	if (len >= dstSize) len = dstSize - 1;

	memcpy(dst, req->data, len);
	dst[len] = '\0';
	Updater_TrimVersion(dst);

	return dst[0] != '\0';
}

static cc_bool Updater_IsSameVersion(const char* latest) {
	/* This still works if GAME_APP_VER is "0.0.1_01 Test 1"
	   and version.txt only says "0.0.1_01". */
	return latest && strstr(GAME_APP_VER, latest) == GAME_APP_VER;
}

void Updater_ShowUpdatePopup(const char* latestVersion) {
#if defined CC_BUILD_WIN
	char msg[256];

	sprintf(msg,
		"A new CavFX update is available!\n\nCurrent: %s\nLatest: %s\n\nOpen the download page?",
		GAME_APP_VER, latestVersion);

	if (MessageBoxA(NULL, msg, "CavFX Updater", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
		Updater_OpenLatestRelease();
	}

#elif defined CC_BUILD_WII
	Chat_AddRaw("&eCavFX update available! Update manually from GitHub.");

#elif defined CC_BUILD_3DS
	Chat_AddRaw("&eCavFX update available! Install the new CIA manually for now.");

#else
	Chat_AddRaw("&eCavFX update available! Type /update to open releases.");
#endif
}

static cc_bool Updater_PollTask(struct ScheduledTask2* task) {
	struct HttpRequest req;
	char latest[64];

	if (updater_req_id < 0) return true;
	if (!Http_GetResult(updater_req_id, &req)) return false;

	updater_req_id = -1;

	if (!req.success) {
		Chat_AddRaw("&eUpdater: Could not check for updates.");
		HttpRequest_Free(&req);
		return true;
	}

	if (Updater_CopyVersionText(&req, latest, sizeof(latest))) {
		if (!Updater_IsSameVersion(latest)) {
			Updater_ShowUpdatePopup(latest);
		}
	} else {
		Chat_AddRaw("&eUpdater: Could not read version.txt.");
	}

	HttpRequest_Free(&req);
	return true;
}

void Updater_CheckLatestOnline(void) {
	cc_string url = String_FromReadonly(CAVFX_VERSION_URL);

	if (updater_req_id >= 0) return;

	updater_req_id = Http_AsyncGetData(&url, HTTP_FLAG_NOCACHE);
	if (updater_req_id < 0) {
		Chat_AddRaw("&eUpdater: Could not start update check.");
		return;
	}

	updater_task.accumulator = 0.0f;
	updater_task.interval = 0.25f;
	updater_task.callback = Updater_PollTask;
	ScheduledTask2_Add(&updater_task);
}

void Updater_OpenLatestRelease(void) {
#if defined CC_BUILD_WIN
	ShellExecuteA(NULL, "open", CAVFX_REPO_URL, NULL, NULL, SW_SHOWNORMAL);

#elif defined CC_BUILD_MACOS
	system("open \"" CAVFX_REPO_URL "\"");

#elif defined CC_BUILD_LINUX
	system("xdg-open \"" CAVFX_REPO_URL "\"");

#elif defined CC_BUILD_WII
	Chat_AddRaw("&eUpdater: Wii manual update only for now.");

#elif defined CC_BUILD_3DS
	Chat_AddRaw("&eUpdater: 3DS CIA updater planned.");

#else
	Chat_AddRaw("&eUpdater: Unsupported platform.");
#endif
}

void Updater_ShowUpdateInfo(void) {
	Chat_AddRaw("&eOpening latest CavFX release page.");
	Updater_OpenLatestRelease();
}

void Updater_CheckOnStartup(void) {
	if (updater_checked) return;
	updater_checked = true;

#ifdef CC_NIGHTLY_BUILD
	/* Nightly builds use commit IDs, so do not compare them against stable version.txt. */
	return;
#endif

	Updater_CheckLatestOnline();
}
