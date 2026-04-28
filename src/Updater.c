#include "Core.h"
#include "String_.h"
#include "Updater.h"
#include "Chat.h"
#include "Constants.h"
#include "Http.h"
#include "Game.h"
#include <stdio.h>

#include <string.h>

#define CAVFX_REPO_URL "https://github.com/RunnerBoy51627/CavFX/releases/latest"
#define CAVFX_API_URL  "https://api.github.com/repos/RunnerBoy51627/CavFX/releases/latest"

#if defined CC_BUILD_WIN
#include <windows.h>
#endif

#if defined CC_BUILD_MACOS || defined CC_BUILD_LINUX
#include <stdlib.h>
#endif

static cc_bool updater_checked;
static int updater_req_id = -1;
static struct ScheduledTask2 updater_task;

static cc_bool Updater_ParseTagName(const char* json, char* tag, int tagSize) {
	const char* key;
	const char* start;
	const char* end;
	int len;

	key = strstr(json, "\"tag_name\"");
	if (!key) return false;

	start = strchr(key, ':');
	if (!start) return false;

	start = strchr(start, '"');
	if (!start) return false;
	start++;

	end = strchr(start, '"');
	if (!end) return false;

	len = (int)(end - start);
	if (len <= 0) return false;
	if (len >= tagSize) len = tagSize - 1;

	memcpy(tag, start, len);
	tag[len] = '\0';
	return true;
}

static cc_bool Updater_IsSameVersion(const char* latest) {
	/* Works even if GAME_APP_VER is "0.0.1_01 Test 1"
	   and GitHub tag is "0.0.1_01" */
	return strstr(GAME_APP_VER, latest) == GAME_APP_VER;
}

static cc_bool Updater_PollTask(struct ScheduledTask2* task) {
	struct HttpRequest req;
	char latest[64];

	if (updater_req_id < 0) return true;
	if (!Http_GetResult(updater_req_id, &req)) return false;

	updater_req_id = -1;

	if (!req.success) {
		/* Keep this quiet if you don't want startup spam when offline */
		Chat_AddRaw("&eUpdater: Could not check for updates.");
		HttpRequest_Free(&req);
		return true;
	}

	if (req.data && Updater_ParseTagName((const char*)req.data, latest, sizeof(latest))) {
		if (!Updater_IsSameVersion(latest)) {
			Updater_ShowUpdatePopup(latest);
		}
	}
	else {
		Chat_AddRaw("&eUpdater: Could not parse GitHub release info.");
	}

	HttpRequest_Free(&req);
	return true;
}

void Updater_CheckLatestOnline(void) {
	cc_string url = String_FromReadonly(CAVFX_API_URL);

	if (updater_req_id >= 0) return;

	updater_req_id = Http_AsyncGetData(&url, HTTP_FLAG_NOCACHE);
	if (updater_req_id < 0) {
		Chat_AddRaw("&eUpdater: Could not start update check.");
		return;
	}

	updater_task.interval = 0.25f;
	updater_task.callback = Updater_PollTask;
	ScheduledTask2_Add(&updater_task);
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

void Updater_OpenLatestRelease(void) {
#if defined CC_BUILD_WIN
	ShellExecuteA(NULL, "open", CAVFX_REPO_URL, NULL, NULL, SW_SHOWNORMAL);

#elif defined CC_BUILD_MACOS
	char cmd[512];
	String_Format1(&((cc_string) { cmd, 0, sizeof(cmd) }), "open \"%c\"", CAVFX_REPO_URL);
	system(cmd);

#elif defined CC_BUILD_LINUX
	char cmd[512];
	String_Format1(&((cc_string) { cmd, 0, sizeof(cmd) }), "xdg-open \"%c\"", CAVFX_REPO_URL);
	system(cmd);

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

	Updater_CheckLatestOnline();
}