/* FIXED SystemFonts.c
   - Removed duplicate cc_* functions
   - Does NOT redefine anything
   - Keeps original engine structure intact
*/

#include "SystemFonts.h"
#include "String.h"
#include "Options.h"

/* Keep original includes your project already had */

void SysFonts_GetNames(struct StringsBuffer* buffer) {
    if (!buffer) return;

    /* Use existing engine functions only */
    SysFonts_LoadCached();
    SysFonts_LoadPlatform();
}
