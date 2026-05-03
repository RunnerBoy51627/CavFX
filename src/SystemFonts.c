#include "SystemFonts.h"
#include "String.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

/* ========= FreeType compatibility wrappers ========= */

int cc_strncmp(const char* a, const char* b, size_t n) {
    if (!a) a = "";
    if (!b) b = "";
    return strncmp(a, b, n);
}

int cc_strcmp(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    return strcmp(a, b);
}

size_t cc_strlen(const char* a) {
    return a ? strlen(a) : 0;
}

char* cc_strstr(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    return (char*)strstr(a, b);
}

int cc_memcmp(const void* a, const void* b, size_t n) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    return memcmp(a, b, n);
}

void* cc_memchr(const void* a, int c, size_t n) {
    if (!a) return NULL;
    return memchr(a, c, n);
}

void cc_qsort(void* v, size_t count, size_t size,
    int (*comp)(const void*, const void*)) {
    if (!v || !count || !size || !comp) return;
    qsort(v, count, size, comp);
}

/* ========= SAFE FONT SYSTEM ========= */

void SysFonts_GetNames(struct StringsBuffer* buffer) {
    if (!buffer) return;

#if defined(CC_BUILD_WIN) || defined(_WIN32)
    /* Prevent Windows nightly crash */
    return;
#endif

    SysFonts_LoadCached();
    SysFonts_LoadPlatform();
}
