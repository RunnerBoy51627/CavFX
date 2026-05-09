#include "ForgedMenuBridge.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define INSTALLER_DLL_NAME "CavFXForgedInstaller.dll"

static int showInstallPopup = 0;

static int FileExists(const char* path) {
    FILE* f;
    if (!path || !path[0]) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int WriteTextFile(const char* path, const char* text) {
    FILE* f;
    if (!path || !path[0]) return 0;

    f = fopen(path, "wb");
    if (!f) return 0;

    if (text) fwrite(text, 1, strlen(text), f);
    fclose(f);
    return 1;
}

static int DeleteFileCompat(const char* path) {
    if (!path || !path[0]) return 1;
    if (!FileExists(path)) return 1;

#ifdef _WIN32
    DeleteFileA(path);
#else
    remove(path);
#endif

    return !FileExists(path);
}

static void GetExeDir(char* out, int outSize) {
#ifdef _WIN32
    char* slash;

    if (!out || outSize <= 0) return;
    out[0] = '\0';

    GetModuleFileNameA(NULL, out, outSize);

    slash = strrchr(out, '\\');
    if (slash) *slash = '\0';
#else
    snprintf(out, outSize, ".");
#endif
}

static void MakePath(char* out, int outSize, const char* dir, const char* file) {
#ifdef _WIN32
    snprintf(out, outSize, "%s\\%s", dir, file);
#else
    snprintf(out, outSize, "%s/%s", dir, file);
#endif
}

static void MakeExePath(char* out, int outSize, const char* subPath) {
    char exeDir[1024];

    GetExeDir(exeDir, sizeof(exeDir));

#ifdef _WIN32
    snprintf(out, outSize, "%s\\%s", exeDir, subPath);
#else
    snprintf(out, outSize, "%s/%s", exeDir, subPath);
#endif
}

/*
    These path lists are intentionally repeated instead of being clever.
    CavFX has been built/launched from multiple folders, so this avoids
    the "created in one folder, checked in another" bug.
*/

static int AnyCoreExists(void) {
    char p[1024];

    MakeExePath(p, sizeof(p), "ForgedCore\\forged.core");
    if (FileExists(p)) return 1;

    if (FileExists("ForgedCore/forged.core")) return 1;
    if (FileExists("./ForgedCore/forged.core")) return 1;
    if (FileExists("src/x64/Debug/ForgedCore/forged.core")) return 1;
    if (FileExists("./src/x64/Debug/ForgedCore/forged.core")) return 1;
    if (FileExists("x64/Debug/ForgedCore/forged.core")) return 1;
    if (FileExists("./x64/Debug/ForgedCore/forged.core")) return 1;
    if (FileExists("../ForgedCore/forged.core")) return 1;
    if (FileExists("../../ForgedCore/forged.core")) return 1;

    return 0;
}

static int AnyInstallerExists(void) {
    char p[1024];

    MakeExePath(p, sizeof(p), "plugins\\CavFXForgedInstaller.dll");
    if (FileExists(p)) return 1;

    if (FileExists("plugins/CavFXForgedInstaller.dll")) return 1;
    if (FileExists("./plugins/CavFXForgedInstaller.dll")) return 1;
    if (FileExists("src/x64/Debug/plugins/CavFXForgedInstaller.dll")) return 1;
    if (FileExists("./src/x64/Debug/plugins/CavFXForgedInstaller.dll")) return 1;
    if (FileExists("x64/Debug/plugins/CavFXForgedInstaller.dll")) return 1;
    if (FileExists("./x64/Debug/plugins/CavFXForgedInstaller.dll")) return 1;
    if (FileExists("../plugins/CavFXForgedInstaller.dll")) return 1;
    if (FileExists("../../plugins/CavFXForgedInstaller.dll")) return 1;

    return 0;
}

static const char* FindInstallerPath(void) {
    static char p[1024];

    MakeExePath(p, sizeof(p), "plugins\\CavFXForgedInstaller.dll");
    if (FileExists(p)) return p;

    if (FileExists("plugins/CavFXForgedInstaller.dll")) return "plugins/CavFXForgedInstaller.dll";
    if (FileExists("./plugins/CavFXForgedInstaller.dll")) return "./plugins/CavFXForgedInstaller.dll";
    if (FileExists("src/x64/Debug/plugins/CavFXForgedInstaller.dll")) return "src/x64/Debug/plugins/CavFXForgedInstaller.dll";
    if (FileExists("./src/x64/Debug/plugins/CavFXForgedInstaller.dll")) return "./src/x64/Debug/plugins/CavFXForgedInstaller.dll";
    if (FileExists("x64/Debug/plugins/CavFXForgedInstaller.dll")) return "x64/Debug/plugins/CavFXForgedInstaller.dll";
    if (FileExists("./x64/Debug/plugins/CavFXForgedInstaller.dll")) return "./x64/Debug/plugins/CavFXForgedInstaller.dll";
    if (FileExists("../plugins/CavFXForgedInstaller.dll")) return "../plugins/CavFXForgedInstaller.dll";
    if (FileExists("../../plugins/CavFXForgedInstaller.dll")) return "../../plugins/CavFXForgedInstaller.dll";

    return NULL;
}

static int AnyDisabledExists(void) {
    char p[1024];

    MakeExePath(p, sizeof(p), "ForgedCore\\forged.disabled");
    if (FileExists(p)) return 1;

    if (FileExists("ForgedCore/forged.disabled")) return 1;
    if (FileExists("./ForgedCore/forged.disabled")) return 1;
    if (FileExists("src/x64/Debug/ForgedCore/forged.disabled")) return 1;
    if (FileExists("./src/x64/Debug/ForgedCore/forged.disabled")) return 1;
    if (FileExists("x64/Debug/ForgedCore/forged.disabled")) return 1;
    if (FileExists("./x64/Debug/ForgedCore/forged.disabled")) return 1;
    if (FileExists("../ForgedCore/forged.disabled")) return 1;
    if (FileExists("../../ForgedCore/forged.disabled")) return 1;

    return 0;
}

static int AnyEnabledExists(void) {
    char p[1024];

    MakeExePath(p, sizeof(p), "ForgedCore\\forged.enabled");
    if (FileExists(p)) return 1;

    if (FileExists("ForgedCore/forged.enabled")) return 1;
    if (FileExists("./ForgedCore/forged.enabled")) return 1;
    if (FileExists("src/x64/Debug/ForgedCore/forged.enabled")) return 1;
    if (FileExists("./src/x64/Debug/ForgedCore/forged.enabled")) return 1;
    if (FileExists("x64/Debug/ForgedCore/forged.enabled")) return 1;
    if (FileExists("./x64/Debug/ForgedCore/forged.enabled")) return 1;
    if (FileExists("../ForgedCore/forged.enabled")) return 1;
    if (FileExists("../../ForgedCore/forged.enabled")) return 1;

    return 0;
}

static void DeleteAllEnabledFlags(void) {
    char p[1024];

    MakeExePath(p, sizeof(p), "ForgedCore\\forged.enabled");
    DeleteFileCompat(p);

    DeleteFileCompat("ForgedCore/forged.enabled");
    DeleteFileCompat("./ForgedCore/forged.enabled");
    DeleteFileCompat("src/x64/Debug/ForgedCore/forged.enabled");
    DeleteFileCompat("./src/x64/Debug/ForgedCore/forged.enabled");
    DeleteFileCompat("x64/Debug/ForgedCore/forged.enabled");
    DeleteFileCompat("./x64/Debug/ForgedCore/forged.enabled");
    DeleteFileCompat("../ForgedCore/forged.enabled");
    DeleteFileCompat("../../ForgedCore/forged.enabled");
}

static void DeleteAllDisabledFlags(void) {
    char p[1024];

    MakeExePath(p, sizeof(p), "ForgedCore\\forged.disabled");
    DeleteFileCompat(p);

    DeleteFileCompat("ForgedCore/forged.disabled");
    DeleteFileCompat("./ForgedCore/forged.disabled");
    DeleteFileCompat("src/x64/Debug/ForgedCore/forged.disabled");
    DeleteFileCompat("./src/x64/Debug/ForgedCore/forged.disabled");
    DeleteFileCompat("x64/Debug/ForgedCore/forged.disabled");
    DeleteFileCompat("./x64/Debug/ForgedCore/forged.disabled");
    DeleteFileCompat("../ForgedCore/forged.disabled");
    DeleteFileCompat("../../ForgedCore/forged.disabled");
}

static int WriteDisabledFlagEverywhere(void) {
    char p[1024];
    int wrote = 0;

    MakeExePath(p, sizeof(p), "ForgedCore\\forged.disabled");
    wrote |= WriteTextFile(p, "CavFX Forged disabled.\n");

    wrote |= WriteTextFile("ForgedCore/forged.disabled", "CavFX Forged disabled.\n");
    wrote |= WriteTextFile("./ForgedCore/forged.disabled", "CavFX Forged disabled.\n");
    wrote |= WriteTextFile("src/x64/Debug/ForgedCore/forged.disabled", "CavFX Forged disabled.\n");
    wrote |= WriteTextFile("./src/x64/Debug/ForgedCore/forged.disabled", "CavFX Forged disabled.\n");
    wrote |= WriteTextFile("x64/Debug/ForgedCore/forged.disabled", "CavFX Forged disabled.\n");
    wrote |= WriteTextFile("./x64/Debug/ForgedCore/forged.disabled", "CavFX Forged disabled.\n");
    wrote |= WriteTextFile("../ForgedCore/forged.disabled", "CavFX Forged disabled.\n");
    wrote |= WriteTextFile("../../ForgedCore/forged.disabled", "CavFX Forged disabled.\n");

    return wrote;
}

static int WriteEnabledFlagEverywhere(void) {
    char p[1024];
    int wrote = 0;

    MakeExePath(p, sizeof(p), "ForgedCore\\forged.enabled");
    wrote |= WriteTextFile(p, "CavFX Forged enabled.\n");

    wrote |= WriteTextFile("ForgedCore/forged.enabled", "CavFX Forged enabled.\n");
    wrote |= WriteTextFile("./ForgedCore/forged.enabled", "CavFX Forged enabled.\n");
    wrote |= WriteTextFile("src/x64/Debug/ForgedCore/forged.enabled", "CavFX Forged enabled.\n");
    wrote |= WriteTextFile("./src/x64/Debug/ForgedCore/forged.enabled", "CavFX Forged enabled.\n");
    wrote |= WriteTextFile("x64/Debug/ForgedCore/forged.enabled", "CavFX Forged enabled.\n");
    wrote |= WriteTextFile("./x64/Debug/ForgedCore/forged.enabled", "CavFX Forged enabled.\n");
    wrote |= WriteTextFile("../ForgedCore/forged.enabled", "CavFX Forged enabled.\n");
    wrote |= WriteTextFile("../../ForgedCore/forged.enabled", "CavFX Forged enabled.\n");

    return wrote;
}

int ForgedMenu_CoreExists(void) {
    return AnyCoreExists();
}

int ForgedMenu_InstallerExists(void) {
    return AnyInstallerExists();
}

int ForgedMenu_IsEnabled(void) {
    if (!ForgedMenu_CoreExists()) return 0;

    /*
        disabled wins over enabled.
        This fixes old enabled files forcing the menu to stay enabled.
    */
    if (AnyDisabledExists()) return 0;

    /*
        If no disabled flag exists, Forged is considered ON.
        Old forged.enabled is supported but no longer required.
    */
    return 1;
}

ForgedMenuState ForgedMenu_GetState(void) {
    if (!ForgedMenu_CoreExists()) {
        return ForgedMenu_InstallerExists()
            ? FORGED_MENU_STATE_CAN_INSTALL
            : FORGED_MENU_STATE_INSTALLER_MISSING;
    }

    return ForgedMenu_IsEnabled()
        ? FORGED_MENU_STATE_ENABLED
        : FORGED_MENU_STATE_DISABLED;
}

int ForgedMenu_ButtonEnabled(void) {
    ForgedMenuState state = ForgedMenu_GetState();

    return state == FORGED_MENU_STATE_CAN_INSTALL
        || state == FORGED_MENU_STATE_ENABLED
        || state == FORGED_MENU_STATE_DISABLED;
}

const char* ForgedMenu_GetButtonText(void) {
    switch (ForgedMenu_GetState()) {
        case FORGED_MENU_STATE_CAN_INSTALL:       return "Install Forged";
        case FORGED_MENU_STATE_ENABLED:           return "Disable Forged";
        case FORGED_MENU_STATE_DISABLED:          return "Enable Forged";
        case FORGED_MENU_STATE_INSTALLER_MISSING:
        default:                                  return "Enable Forged (Installer missing)";
    }
}

const char* ForgedMenu_GetStatusText(void) {
    switch (ForgedMenu_GetState()) {
        case FORGED_MENU_STATE_CAN_INSTALL:       return "Forged installer detected.";
        case FORGED_MENU_STATE_ENABLED:           return "Forged is enabled.";
        case FORGED_MENU_STATE_DISABLED:          return "Forged is disabled.";
        case FORGED_MENU_STATE_INSTALLER_MISSING:
        default:                                  return "Forged installer missing.";
    }
}

int ForgedMenu_DisableForged(void) {
    if (!ForgedMenu_CoreExists()) return 0;

    DeleteAllEnabledFlags();

    if (!WriteDisabledFlagEverywhere()) {
        return 0;
    }

    return !ForgedMenu_IsEnabled();
}

int ForgedMenu_EnableForged(void) {
    if (!ForgedMenu_CoreExists()) return 0;

    DeleteAllDisabledFlags();

    /*
        Also write old enabled flags so older menu code still sees ON.
    */
    WriteEnabledFlagEverywhere();

    return ForgedMenu_IsEnabled();
}

static int RunInstaller(void) {
#ifdef _WIN32
    const char* path;
    HMODULE lib;
    FARPROC proc;
    typedef int (*InstallFunc)(void);
    InstallFunc install;

    path = FindInstallerPath();
    if (!path) return 0;

    lib = LoadLibraryA(path);
    if (!lib) return 0;

    proc = GetProcAddress(lib, "CavFXForged_Install");
    if (!proc) {
        FreeLibrary(lib);
        return 0;
    }

    install = (InstallFunc)proc;
    return install();
#else
    return 0;
#endif
}

int ForgedMenu_OnButtonClick(void) {
    switch (ForgedMenu_GetState()) {
        case FORGED_MENU_STATE_CAN_INSTALL:
            showInstallPopup = 1;
            return 1;

        case FORGED_MENU_STATE_ENABLED:
            return ForgedMenu_DisableForged();

        case FORGED_MENU_STATE_DISABLED:
            return ForgedMenu_EnableForged();

        case FORGED_MENU_STATE_INSTALLER_MISSING:
        default:
            return 0;
    }
}

int ForgedMenu_ShouldShowInstallPopup(void) {
    return showInstallPopup;
}

const char* ForgedMenu_GetPopupTitle(void) {
    return "Install CavFX Forged?";
}

const char* ForgedMenu_GetPopupBody(void) {
    return "This will install CavFX Forged Core. Restart CavFX after installation.";
}

int ForgedMenu_ConfirmInstall(void) {
    int ok;

    showInstallPopup = 0;
    ok = RunInstaller();

    return ok;
}

int ForgedMenu_CancelInstall(void) {
    showInstallPopup = 0;
    return 1;
}
