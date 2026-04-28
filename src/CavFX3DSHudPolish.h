#ifndef CAVFX_3DS_HUD_POLISH_H
#define CAVFX_3DS_HUD_POLISH_H

#include "Core.h"

/*
 * CavFX3DSHudPolish
 * ------------------
 * Tiny, 3DS-safe HUD module for moving survival HUD pieces away from
 * the bottom screen. It draws with flat rectangles only: no custom fonts,
 * no generated text textures, no dynamic number textures.
 *
 * Call CavFX3DSHudPolish_RenderTop(...) while the TOP screen is selected,
 * after world rendering and before switching to the bottom inventory/touch UI.
 */

struct CavFX3DSHudPolish_State {
	int health;      /* current health, usually 0..20 */
	int maxHealth;   /* max health, usually 20 */
	int oxygen;      /* current oxygen/air, usually 0..20 */
	int maxOxygen;   /* max oxygen/air, usually 20 */
	int armor;       /* current armor points, usually 0..20 */
	int maxArmor;    /* max armor points, usually 20 */
	cc_bool showOxygen;
	cc_bool showArmor;
};

void CavFX3DSHudPolish_RenderTop(const struct CavFX3DSHudPolish_State* hud);

#endif
