
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static char enabledPath[1024];

static int FileExists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void BuildPath(void) {
#ifdef _WIN32
    char exe[1024];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char* slash = strrchr(exe, '\\');
    if (slash) *slash = '\0';

    snprintf(enabledPath, sizeof(enabledPath),
        "%s\\ForgedCore\\forged.enabled", exe);
#else
    snprintf(enabledPath, sizeof(enabledPath),
        "ForgedCore/forged.enabled");
#endif
}

int DisableForged_Fix(void) {
    BuildPath();

#ifdef _WIN32
    if (DeleteFileA(enabledPath)) {
        return 1;
    }
#else
    if (remove(enabledPath) == 0) {
        return 1;
    }
#endif

    return !FileExists(enabledPath);
}
