#include "CavFX3DSHudPolish.h"
#include "Graphics.h"
#include "Window.h"
#include "Platform.h"

#define CAVFX_HUD_TOP_SAFE_X 8
#define CAVFX_HUD_TOP_SAFE_Y 8

static int Hud_ScaleX(int v) {
	int s = Display_ScaleX(v);
	return s < 1 ? 1 : s;
}

static int Hud_ScaleY(int v) {
	int s = Display_ScaleY(v);
	return s < 1 ? 1 : s;
}

static int Hud_Clamp(int v, int min, int max) {
	if (v < min) return min;
	if (v > max) return max;
	return v;
}

static void Hud_DrawBox(int x, int y, int w, int h, PackedCol fill, PackedCol border) {
	Gfx_Draw2DFlat(x - 1,     y - 1,     w + 2, 1,     border);
	Gfx_Draw2DFlat(x - 1,     y + h,     w + 2, 1,     border);
	Gfx_Draw2DFlat(x - 1,     y - 1,     1,     h + 2, border);
	Gfx_Draw2DFlat(x + w,     y - 1,     1,     h + 2, border);
	Gfx_Draw2DFlat(x,         y,         w,     h,     fill);
}

/* 3DS-safe pixel heart. Simple rectangles only. */
static void Hud_DrawHeart(int x, int y, int s, int filled) {
	PackedCol red      = PackedCol_Make(222, 38, 38, 255);
	PackedCol darkRed  = PackedCol_Make(91,  16, 16, 255);
	PackedCol empty    = PackedCol_Make(45,  45, 45, 180);
	PackedCol outline  = PackedCol_Make(0,   0,  0,  220);
	PackedCol col      = filled ? red : empty;

	/* outline blob */
	Gfx_Draw2DFlat(x + 1*s, y + 0*s, 2*s, 1*s, outline);
	Gfx_Draw2DFlat(x + 5*s, y + 0*s, 2*s, 1*s, outline);
	Gfx_Draw2DFlat(x + 0*s, y + 1*s, 8*s, 3*s, outline);
	Gfx_Draw2DFlat(x + 1*s, y + 4*s, 6*s, 1*s, outline);
	Gfx_Draw2DFlat(x + 2*s, y + 5*s, 4*s, 1*s, outline);
	Gfx_Draw2DFlat(x + 3*s, y + 6*s, 2*s, 1*s, outline);

	/* fill */
	Gfx_Draw2DFlat(x + 1*s, y + 1*s, 2*s, 1*s, col);
	Gfx_Draw2DFlat(x + 5*s, y + 1*s, 2*s, 1*s, col);
	Gfx_Draw2DFlat(x + 1*s, y + 2*s, 6*s, 2*s, col);
	Gfx_Draw2DFlat(x + 2*s, y + 4*s, 4*s, 1*s, col);
	Gfx_Draw2DFlat(x + 3*s, y + 5*s, 2*s, 1*s, col);

	if (filled) Gfx_Draw2DFlat(x + 2*s, y + 2*s, 4*s, 1*s, darkRed);
}

static void Hud_DrawBubble(int x, int y, int s, int filled) {
	PackedCol blue    = PackedCol_Make(70,  170, 255, 220);
	PackedCol empty   = PackedCol_Make(45,  45,  55,  160);
	PackedCol shine   = PackedCol_Make(210, 240, 255, 230);
	PackedCol outline = PackedCol_Make(0,   0,   0,   200);
	PackedCol col     = filled ? blue : empty;

	Gfx_Draw2DFlat(x + 2*s, y + 0*s, 4*s, 1*s, outline);
	Gfx_Draw2DFlat(x + 1*s, y + 1*s, 6*s, 1*s, outline);
	Gfx_Draw2DFlat(x + 0*s, y + 2*s, 8*s, 4*s, outline);
	Gfx_Draw2DFlat(x + 1*s, y + 6*s, 6*s, 1*s, outline);
	Gfx_Draw2DFlat(x + 2*s, y + 7*s, 4*s, 1*s, outline);

	Gfx_Draw2DFlat(x + 2*s, y + 1*s, 4*s, 1*s, col);
	Gfx_Draw2DFlat(x + 1*s, y + 2*s, 6*s, 4*s, col);
	Gfx_Draw2DFlat(x + 2*s, y + 6*s, 4*s, 1*s, col);
	if (filled) Gfx_Draw2DFlat(x + 2*s, y + 2*s, 2*s, 1*s, shine);
}

static void Hud_DrawArmorPip(int x, int y, int s, int filled) {
	PackedCol full    = PackedCol_Make(170, 170, 190, 255);
	PackedCol empty   = PackedCol_Make(50,  50,  60,  160);
	PackedCol outline = PackedCol_Make(0,   0,   0,   210);
	PackedCol col     = filled ? full : empty;

	Gfx_Draw2DFlat(x + 1*s, y + 0*s, 6*s, 1*s, outline);
	Gfx_Draw2DFlat(x + 0*s, y + 1*s, 8*s, 3*s, outline);
	Gfx_Draw2DFlat(x + 1*s, y + 4*s, 6*s, 2*s, outline);
	Gfx_Draw2DFlat(x + 2*s, y + 6*s, 4*s, 1*s, outline);

	Gfx_Draw2DFlat(x + 1*s, y + 1*s, 6*s, 2*s, col);
	Gfx_Draw2DFlat(x + 2*s, y + 3*s, 4*s, 2*s, col);
	Gfx_Draw2DFlat(x + 3*s, y + 5*s, 2*s, 1*s, col);
}

static void Hud_DrawRowHearts(int x, int y, int value, int maxValue) {
	int i, hearts, filled;
	int s = Hud_ScaleX(2);
	int step = 10 * s;

	maxValue = maxValue <= 0 ? 20 : maxValue;
	value    = Hud_Clamp(value, 0, maxValue);
	hearts   = maxValue / 2;
	if (hearts > 10) hearts = 10;
	filled   = (value + 1) / 2;

	for (i = 0; i < hearts; i++) Hud_DrawHeart(x + i * step, y, s, i < filled);
}

static void Hud_DrawRowBubbles(int x, int y, int value, int maxValue) {
	int i, bubbles, filled;
	int s = Hud_ScaleX(2);
	int step = 10 * s;

	maxValue = maxValue <= 0 ? 20 : maxValue;
	value    = Hud_Clamp(value, 0, maxValue);
	bubbles  = maxValue / 2;
	if (bubbles > 10) bubbles = 10;
	filled   = (value + 1) / 2;

	for (i = 0; i < bubbles; i++) Hud_DrawBubble(x + i * step, y, s, i < filled);
}

static void Hud_DrawRowArmor(int x, int y, int value, int maxValue) {
	int i, pips, filled;
	int s = Hud_ScaleX(2);
	int step = 10 * s;

	maxValue = maxValue <= 0 ? 20 : maxValue;
	value    = Hud_Clamp(value, 0, maxValue);
	pips     = maxValue / 2;
	if (pips > 10) pips = 10;
	filled   = (value + 1) / 2;

	for (i = 0; i < pips; i++) Hud_DrawArmorPip(x + i * step, y, s, i < filled);
}

void CavFX3DSHudPolish_RenderTop(const struct CavFX3DSHudPolish_State* hud) {
	int x, y, rowH;
	if (!hud) return;

#ifdef CC_BUILD_3DS
	/* Force survival status HUD to top screen. */
	Gfx_3DS_SetRenderScreen(TOP_SCREEN);
#endif

	x    = Hud_ScaleX(CAVFX_HUD_TOP_SAFE_X);
	y    = Hud_ScaleY(CAVFX_HUD_TOP_SAFE_Y);
	rowH = Hud_ScaleY(20);

	/* small transparent backing to reduce shimmer against bright world colors */
	Hud_DrawBox(x - Hud_ScaleX(3), y - Hud_ScaleY(3), Hud_ScaleX(108),
		Hud_ScaleY(hud->showArmor ? 57 : (hud->showOxygen ? 38 : 19)),
		PackedCol_Make(0, 0, 0, 70), PackedCol_Make(0, 0, 0, 130));

	Hud_DrawRowHearts(x, y, hud->health, hud->maxHealth);
	y += rowH;

	if (hud->showOxygen) {
		Hud_DrawRowBubbles(x, y, hud->oxygen, hud->maxOxygen);
		y += rowH;
	}
	if (hud->showArmor) {
		Hud_DrawRowArmor(x, y, hud->armor, hud->maxArmor);
	}
}
