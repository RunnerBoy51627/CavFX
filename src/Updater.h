#ifndef CC_UPDATER_H
#define CC_UPDATER_H

void Updater_OpenLatestRelease(void);
void Updater_ShowUpdateInfo(void);
void Updater_CheckOnStartup(void);
void Updater_CheckLatestOnline(void);
void Updater_ShowUpdatePopup(const char* latestVersion);

#endif
