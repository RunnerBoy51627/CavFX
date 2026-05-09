#include "HeldBlockRenderer.h"
#include "Block.h"
#include "Game.h"
#include "Inventory.h"
#include "Graphics.h"
#include "Camera.h"
#include "ExtMath.h"
#include "Event.h"
#include "Entity.h"
#include "Model.h"
#include "Options.h"
#include "Gui.h"
#include "SurvivalItems.h"
#include "TexturePack.h"
#include "Bitmap.h"
#include "Stream.h"
#include "String_.h"

cc_bool HeldBlockRenderer_Show;
#if CC_BUILD_FPU_MODE >= CC_FPU_MODE_REDUCED
static BlockID held_block;
static struct Entity held_entity;
static struct Matrix held_blockProj;
static GfxResourceID held_itemVB;
static struct Bitmap held_itemsBmp;
static cc_bool held_itemsBmpTried;

static cc_bool held_animating, held_breaking, held_swinging, held_continuousMining;
static float held_swingY;
static float held_time, held_period = 0.25f;
static BlockID held_lastBlock;
static cc_bool held_swayInit;
static float held_lastYaw, held_lastPitch;
static float held_swayX, held_swayY;

cc_bool HeldBlockRenderer_IsEating;
static float held_eatTime;
static float held_eatBlend;

static float ClampFloat(float value, float min, float max) {
	if (value < min) return min;
	if (value > max) return max;
	return value;
}

static float AngleDelta(float current, float last) {
	float delta = current - last;

	if (delta > 180.0f)  delta -= 360.0f;
	if (delta < -180.0f) delta += 360.0f;
	return delta;
}

void HeldBlockRenderer_StartEating(void) {
	HeldBlockRenderer_IsEating = true;
	held_eatTime = 0.0f;
}

void HeldBlockRenderer_StopEating(void) {
	HeldBlockRenderer_IsEating = false;
}

void HeldBlockRenderer_TickEating(float delta) {
	float target = HeldBlockRenderer_IsEating ? 1.0f : 0.0f;
	float lerp   = ClampFloat(delta * 12.0f, 0.0f, 1.0f);

	if (HeldBlockRenderer_IsEating) held_eatTime += delta;
	held_eatBlend = Math_Lerp(held_eatBlend, target, lerp);
	if (!HeldBlockRenderer_IsEating && held_eatBlend < 0.01f) {
		held_eatBlend = 0.0f;
		held_eatTime  = 0.0f;
	}
}

/* Minecraft 1.0.0-ish eating: item repeatedly moves up/in toward camera. */
static void HeldBlockRenderer_EatAnimation(void) {
	float cycle, bob, shove, ease;

	ease = held_eatBlend;
	if (ease <= 0.001f) return;

	/* Minecraft 1.0.0-ish bite motion, but smoothed so it does not snap in. */
	cycle = held_eatTime * 7.5f;
	bob   = Math_AbsF(Math_SinF(cycle * MATH_PI));
	bob   = bob * bob;
	shove = Math_SinF(cycle * MATH_PI * 0.5f);

	held_entity.Position.x += (-0.30f + Math_SinF(cycle * MATH_PI * 2.0f) * 0.030f) * ease;
	held_entity.Position.y += ( 0.22f + bob * 0.26f) * ease;
	held_entity.Position.z += ( 0.42f + bob * 0.20f + shove * 0.035f) * ease;

	held_entity.RotX += (42.0f + bob * 32.0f) * ease;
	held_entity.RotY += (16.0f + shove * 14.0f) * ease;
	held_entity.Yaw  += (24.0f + bob * 14.0f) * ease;
}



/* Since not using Entity_SetModel, which normally automatically does this */
static void SetHeldModel(struct Model* model) {
#ifdef CC_BUILD_CONSOLE
	static int maxVertices;
	if (model->maxVertices <= maxVertices) return;

	maxVertices = model->maxVertices;
	Gfx_DeleteDynamicVb(&held_entity.ModelVB);
#endif
}

static void HeldBlockRenderer_RenderModel(void) {
	struct Model* model;

	Gfx_SetFaceCulling(true);
	Gfx_SetDepthTest(false);
	/* Gfx_SetDepthWrite(false); */
	/* TODO: Need to properly reallocate per model VB here */

	if (SurvivalItem_IsItem(held_block) || Blocks.Draw[held_block] == DRAW_GAS) {
		model = Entities.CurPlayer->Base.Model;
		SetHeldModel(model);
		Vec3_Set(held_entity.ModelScale, 1.0f, 1.0f, 1.0f);

		Model_RenderArm(model, &held_entity);
		Gfx_SetAlphaTest(false);
	}
	else {
		model = Models.Block;
		SetHeldModel(model);
		Vec3_Set(held_entity.ModelScale, 0.4f, 0.4f, 0.4f);

		Gfx_SetupAlphaState(Blocks.Draw[held_block]);
		Model_Render(model, &held_entity);
		Gfx_RestoreAlphaState(Blocks.Draw[held_block]);
	}
	
	Gfx_SetDepthTest(true);
	/* Gfx_SetDepthWrite(true); */
	Gfx_SetFaceCulling(false);
}

#define HELD_ITEM_TILE_SIZE    16
#define HELD_ITEM_MAX_VERTICES (HELD_ITEM_TILE_SIZE * HELD_ITEM_TILE_SIZE * 24)

static cc_bool HeldItem_LoadBitmapPath(const cc_string* path) {
	cc_filepath fp;
	struct Stream stream;
	cc_result res;

	Platform_EncodePath(&fp, path);
	res = Stream_OpenPath(&stream, &fp);
	if (res) return false;

	res = Png_Decode(&held_itemsBmp, &stream);
	stream.Close(&stream);
	return res == 0 && held_itemsBmp.scan0;
}

static cc_bool HeldItem_EnsureBitmap(void) {
	static const cc_string mainPath = String_FromConst("texpacks/default/items.png");
	static const cc_string preloadPath = String_FromConst("src/preload/texpacks/default/items.png");

	if (held_itemsBmp.scan0) return true;
	if (held_itemsBmpTried) return false;

	held_itemsBmpTried = true;
	if (HeldItem_LoadBitmapPath(&mainPath)) return true;
	if (HeldItem_LoadBitmapPath(&preloadPath)) return true;
	return false;
}

static cc_bool HeldItem_IsOpaque(int srcX, int srcY) {
	BitmapCol col;
	if (srcX < 0 || srcY < 0 || srcX >= held_itemsBmp.width || srcY >= held_itemsBmp.height) return false;

	col = Bitmap_GetPixel(&held_itemsBmp, srcX, srcY);
	return BitmapCol_A(col) >= 128;
}

static Vec3 HeldItem_Point(float x, float y, float z, const Vec3* origin) {
	Vec3 v = Vec3_Create3(x, y, z);

	v = Vec3_RotateZ(v, -42.0f * MATH_DEG2RAD);
	v = Vec3_RotateY(v, -24.0f * MATH_DEG2RAD);
	v = Vec3_RotateX(v,  12.0f * MATH_DEG2RAD);
	Vec3_AddBy(&v, origin);
	return v;
}

static void HeldItem_SetVertex(struct VertexTextured* v, Vec3 pos, float u, float vv, PackedCol col) {
	v->x = pos.x; v->y = pos.y; v->z = pos.z;
	v->Col = col; v->U = u; v->V = vv;
}

static void HeldItem_AddQuad(struct VertexTextured** ptr, Vec3 a, Vec3 b, Vec3 c, Vec3 d,
							 TextureRec uv, PackedCol col) {
	struct VertexTextured* v = *ptr;

	HeldItem_SetVertex(&v[0], a, uv.u1, uv.v1, col);
	HeldItem_SetVertex(&v[1], b, uv.u2, uv.v1, col);
	HeldItem_SetVertex(&v[2], c, uv.u2, uv.v2, col);
	HeldItem_SetVertex(&v[3], d, uv.u1, uv.v2, col);
	*ptr += 4;
}

static int HeldItem_CountVertices(int baseX, int baseY) {
	int x, y, count;

	count = 0;
	for (y = 0; y < HELD_ITEM_TILE_SIZE; y++) {
		for (x = 0; x < HELD_ITEM_TILE_SIZE; x++) {
			if (!HeldItem_IsOpaque(baseX + x, baseY + y)) continue;

			count += 8; /* front + back */
			if (!HeldItem_IsOpaque(baseX + x - 1, baseY + y)) count += 4;
			if (!HeldItem_IsOpaque(baseX + x + 1, baseY + y)) count += 4;
			if (!HeldItem_IsOpaque(baseX + x, baseY + y - 1)) count += 4;
			if (!HeldItem_IsOpaque(baseX + x, baseY + y + 1)) count += 4;
		}
	}
	return count;
}

static cc_bool HeldBlockRenderer_IsPlantLikeItem(BlockID item);
static void HeldBlockRenderer_ApplyItemHoldPose(Vec3* p, const Vec3* origin);

static void HeldItem_AddPixel(struct VertexTextured** ptr, int baseX, int baseY, int px, int py,
							  const Vec3* origin, cc_bool frontOnly) {
	TextureRec uv;
	PackedCol frontCol, backCol, sideCol;
	Vec3 a, b, c, d, e, f, g, h;
	float x1, y1, x2, y2;
	float z1, z2, size;
	float invW, invH;

	size = 0.045f;
	z1   = -0.025f; z2 = 0.025f;
	x1   = (px - 8) * size; x2 = x1 + size;
	y2   = (8 - py) * size; y1 = y2 - size;
	invW = 1.0f / held_itemsBmp.width; invH = 1.0f / held_itemsBmp.height;

	uv.u1 = (baseX + px)       * invW; uv.v1 = (baseY + py)       * invH;
	uv.u2 = (baseX + px + 1.0f) * invW; uv.v2 = (baseY + py + 1.0f) * invH;

	a = HeldItem_Point(x1, y2, z1, origin);
	b = HeldItem_Point(x2, y2, z1, origin);
	c = HeldItem_Point(x2, y1, z1, origin);
	d = HeldItem_Point(x1, y1, z1, origin);
	e = HeldItem_Point(x1, y2, z2, origin);
	f = HeldItem_Point(x2, y2, z2, origin);
	g = HeldItem_Point(x2, y1, z2, origin);
	h = HeldItem_Point(x1, y1, z2, origin);

	if (!HeldBlockRenderer_IsPlantLikeItem(held_block)) {
		HeldBlockRenderer_ApplyItemHoldPose(&a, origin);
		HeldBlockRenderer_ApplyItemHoldPose(&b, origin);
		HeldBlockRenderer_ApplyItemHoldPose(&c, origin);
		HeldBlockRenderer_ApplyItemHoldPose(&d, origin);
		HeldBlockRenderer_ApplyItemHoldPose(&e, origin);
		HeldBlockRenderer_ApplyItemHoldPose(&f, origin);
		HeldBlockRenderer_ApplyItemHoldPose(&g, origin);
		HeldBlockRenderer_ApplyItemHoldPose(&h, origin);
	}

	frontCol = PACKEDCOL_WHITE;
	backCol  = PackedCol_Make(150, 150, 150, 255);
	sideCol  = PackedCol_Make(115, 115, 115, 255);

	if (frontOnly) {
		HeldItem_AddQuad(ptr, a, b, c, d, uv, frontCol);
		return;
	}

	HeldItem_AddQuad(ptr, f, e, h, g, uv, backCol);
	if (!HeldItem_IsOpaque(baseX + px - 1, baseY + py)) HeldItem_AddQuad(ptr, e, a, d, h, uv, sideCol);
	if (!HeldItem_IsOpaque(baseX + px + 1, baseY + py)) HeldItem_AddQuad(ptr, b, f, g, c, uv, sideCol);
	if (!HeldItem_IsOpaque(baseX + px, baseY + py - 1)) HeldItem_AddQuad(ptr, e, f, b, a, uv, sideCol);
	if (!HeldItem_IsOpaque(baseX + px, baseY + py + 1)) HeldItem_AddQuad(ptr, d, c, g, h, uv, sideCol);
}

static cc_bool HeldBlockRenderer_IsPlantLikeItem(BlockID item) {
	/* Flowers should stay upright like Minecraft's plant items. */
#ifdef SURVIVAL_ITEM_ROSE
	if (item == SURVIVAL_ITEM_ROSE) return true;
#endif
#ifdef SURVIVAL_ITEM_YELLOW_FLOWER
	if (item == SURVIVAL_ITEM_YELLOW_FLOWER) return true;
#endif
	return false;
}

static void HeldBlockRenderer_ApplyItemHoldPose(Vec3* p, const Vec3* origin) {
	float x, y, angle, cosA, sinA;

	x = p->x - origin->x;
	y = p->y - origin->y;

	/* Diagonal Minecraft-held item/tool angle instead of straight-up. */
	angle = -38.0f * MATH_DEG2RAD;
	cosA  = Math_CosF(angle);
	sinA  = Math_SinF(angle);

	p->x = origin->x + x * cosA - y * sinA;
	p->y = origin->y + x * sinA + y * cosA;
}

static void HeldBlockRenderer_RenderItemFlat(void) {
	struct VertexTextured* v;
	TextureLoc loc;
	TextureRec uv;
	Vec3 origin, a, b, c, d;
	int col, row;
	float size;

	if (!Gui.ItemsTex) return;

	loc = SurvivalItem_TextureLoc(held_block);
	col = ((int)loc) & 15;
	row = ((int)loc) >> 4;

	uv.u1 = col / 16.0f;       uv.v1 = row / 16.0f;
	uv.u2 = (col + 1) / 16.0f; uv.v2 = (row + 1) / 16.0f;

	if (!held_itemVB) held_itemVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, 4);

	size     = 0.38f;
	origin.x = held_entity.Position.x + 0.12f;
	origin.y = held_entity.Position.y + 0.26f;
	origin.z = held_entity.Position.z - 0.12f;

	a = HeldItem_Point(-size,  size, 0.0f, &origin);
	b = HeldItem_Point( size,  size, 0.0f, &origin);
	c = HeldItem_Point( size, -size, 0.0f, &origin);
	d = HeldItem_Point(-size, -size, 0.0f, &origin);

	if (!HeldBlockRenderer_IsPlantLikeItem(held_block)) {
		HeldBlockRenderer_ApplyItemHoldPose(&a, &origin);
		HeldBlockRenderer_ApplyItemHoldPose(&b, &origin);
		HeldBlockRenderer_ApplyItemHoldPose(&c, &origin);
		HeldBlockRenderer_ApplyItemHoldPose(&d, &origin);
	}

	v = (struct VertexTextured*)Gfx_LockDynamicVb(held_itemVB, VERTEX_FORMAT_TEXTURED, 4);
	HeldItem_SetVertex(&v[0], a, uv.u1, uv.v1, PACKEDCOL_WHITE);
	HeldItem_SetVertex(&v[1], b, uv.u2, uv.v1, PACKEDCOL_WHITE);
	HeldItem_SetVertex(&v[2], c, uv.u2, uv.v2, PACKEDCOL_WHITE);
	HeldItem_SetVertex(&v[3], d, uv.u1, uv.v2, PACKEDCOL_WHITE);
	Gfx_UnlockDynamicVb(held_itemVB);

	Gfx_SetDepthTest(false);
	Gfx_SetFaceCulling(false);
	Gfx_SetAlphaTest(true);
	Gfx_SetAlphaBlending(true);
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_LoadMatrix(MATRIX_VIEW, &Gfx.View);
	Gfx_BindDynamicVb(held_itemVB);
	Gfx_BindTexture(Gui.ItemsTex);
	Gfx_DrawVb_IndexedTris(4);
	Gfx_SetAlphaTest(false);
	Gfx_SetAlphaBlending(false);
	Gfx_SetDepthTest(true);
}

static void HeldBlockRenderer_RenderItem(void) {
	struct VertexTextured* v, *ptr;
	TextureLoc loc;
	int col, row;
	int baseX, baseY;
	int x, y, count;
	Vec3 origin;

#ifdef CC_BUILD_3DS
	HeldBlockRenderer_RenderItemFlat();
	return;
#endif

	if (!Gui.ItemsTex || !HeldItem_EnsureBitmap()) return;
	loc = SurvivalItem_TextureLoc(held_block);
	col = ((int)loc) & 15;
	row = ((int)loc) >> 4;
	baseX = col * HELD_ITEM_TILE_SIZE;
	baseY = row * HELD_ITEM_TILE_SIZE;

	count = HeldItem_CountVertices(baseX, baseY);
	if (!count) return;

	if (!held_itemVB) held_itemVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, HELD_ITEM_MAX_VERTICES);

	origin.x = held_entity.Position.x + 0.12f;
	origin.y = held_entity.Position.y + 0.26f;
	origin.z = held_entity.Position.z - 0.12f;

	v = (struct VertexTextured*)Gfx_LockDynamicVb(held_itemVB, VERTEX_FORMAT_TEXTURED, count);
	ptr = v;
	for (y = 0; y < HELD_ITEM_TILE_SIZE; y++) {
		for (x = 0; x < HELD_ITEM_TILE_SIZE; x++) {
			if (!HeldItem_IsOpaque(baseX + x, baseY + y)) continue;
			HeldItem_AddPixel(&ptr, baseX, baseY, x, y, &origin, false);
		}
	}
	for (y = 0; y < HELD_ITEM_TILE_SIZE; y++) {
		for (x = 0; x < HELD_ITEM_TILE_SIZE; x++) {
			if (!HeldItem_IsOpaque(baseX + x, baseY + y)) continue;
			HeldItem_AddPixel(&ptr, baseX, baseY, x, y, &origin, true);
		}
	}
	Gfx_UnlockDynamicVb(held_itemVB);

	Gfx_SetDepthTest(false);
	Gfx_SetFaceCulling(false);
	Gfx_SetAlphaTest(true);
	Gfx_SetAlphaBlending(true);
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_LoadMatrix(MATRIX_VIEW, &Gfx.View);
	Gfx_BindDynamicVb(held_itemVB);
	Gfx_BindTexture(Gui.ItemsTex);
	Gfx_DrawVb_IndexedTris(count);
	Gfx_SetAlphaTest(false);
	Gfx_SetAlphaBlending(false);
	Gfx_SetDepthTest(true);
}

static void SetMatrix(void) {
	struct Entity* p = &Entities.CurPlayer->Base;
	struct Matrix lookAt;
	Vec3 eye = { 0,0,0 }; eye.y = Entity_GetEyeHeight(p);

	Matrix_Translate(&lookAt, -eye.x, -eye.y, -eye.z);
	Matrix_Mul(&Gfx.View, &lookAt, &Camera.TiltM);
}

static void ResetHeldState(void) {
	/* Based off details from http://pastebin.com/KFV0HkmD (Thanks goodlyay!) */
	struct Entity* p = &Entities.CurPlayer->Base;
	Vec3 eye = { 0,0,0 }; eye.y = Entity_GetEyeHeight(p);
	held_entity.Position = eye;

	held_entity.Position.x -= Camera.BobbingHor;
	held_entity.Position.y -= Camera.BobbingVer;
	held_entity.Position.z -= Camera.BobbingHor;

	held_entity.Yaw   = -45.0f; held_entity.RotY = -45.0f;
	held_entity.Pitch = 0.0f;   held_entity.RotX = 0.0f;
	held_entity.ModelBlock   = held_block;

	held_entity.SkinType     = p->SkinType;
	held_entity.TextureId    = p->TextureId;
	held_entity.NonHumanSkin = p->NonHumanSkin;
	held_entity.uScale       = p->uScale;
	held_entity.vScale       = p->vScale;
}

static void SetBaseOffset(void) {
	cc_bool sprite = Blocks.Draw[held_block] == DRAW_SPRITE;
	Vec3 normalOffset = { 0.56f, -0.72f, -0.72f };
	Vec3 spriteOffset = { 0.46f, -0.52f, -0.72f };
	Vec3 offset = sprite ? spriteOffset : normalOffset;

	Vec3_AddBy(&held_entity.Position, &offset);
	if (!sprite && Blocks.Draw[held_block] != DRAW_GAS) {
		float height = Blocks.MaxBB[held_block].y - Blocks.MinBB[held_block].y;
		held_entity.Position.y += 0.2f * (1.0f - height);
	}
}

static void ApplyHandSway(float delta) {
	struct Entity* p = &Entities.CurPlayer->Base;
	float yawDelta, pitchDelta;
	float targetX, targetY, lerp;

	if (!held_swayInit) {
		held_lastYaw   = p->Yaw;
		held_lastPitch = p->Pitch;
		held_swayInit  = true;
	}

	yawDelta   = AngleDelta(p->Yaw,   held_lastYaw);
	pitchDelta = AngleDelta(p->Pitch, held_lastPitch);
	held_lastYaw   = p->Yaw;
	held_lastPitch = p->Pitch;

	targetX = ClampFloat(-yawDelta   * 0.035f, -0.10f, 0.10f);
	targetY = ClampFloat( pitchDelta * 0.030f, -0.08f, 0.08f);
	lerp    = ClampFloat(delta * 14.0f, 0.0f, 1.0f);

	held_swayX = Math_Lerp(held_swayX, targetX, lerp);
	held_swayY = Math_Lerp(held_swayY, targetY, lerp);

	held_entity.Position.x += held_swayX;
	held_entity.Position.y += held_swayY;
	held_entity.RotY       += held_swayX * 180.0f;
	held_entity.Yaw        += held_swayX * 80.0f;
	held_entity.RotX       += held_swayY * 165.0f;
}

static void OnProjectionChanged(void* obj) {
	float fov = 70.0f * MATH_DEG2RAD;
	float aspectRatio = (float)Game.Width / (float)Game.Height;
	Gfx_CalcPerspectiveMatrix(&held_blockProj, fov, aspectRatio, (float)Game_ViewDistance);
}

/* Based off incredible gifs from (Thanks goodlyay!)
	https://dl.dropboxusercontent.com/s/iuazpmpnr89zdgb/slowBreakTranslate.gif
	https://dl.dropboxusercontent.com/s/z7z8bset914s0ij/slowBreakRotate1.gif
	https://dl.dropboxusercontent.com/s/pdq79gkzntquld1/slowBreakRotate2.gif
	https://dl.dropboxusercontent.com/s/w1ego7cy7e5nrk1/slowBreakFull.gif

	https://github.com/ClassiCube/ClassicalSharp/wiki/Dig-animation-details
*/
static void HeldBlockRenderer_DigAnimation(void) {
	float sinHalfCircle, sinHalfCircleWeird;
	float t, sqrtLerpPI;

	t = held_time / held_period;
	sinHalfCircle = Math_SinF(t * MATH_PI);
	sqrtLerpPI    = Math_SqrtF(t) * MATH_PI;

	held_entity.Position.x -= Math_SinF(sqrtLerpPI)     * 0.4f;
	held_entity.Position.y += Math_SinF(sqrtLerpPI * 2) * 0.2f;
	held_entity.Position.z -= sinHalfCircle            * 0.2f;

	sinHalfCircleWeird = Math_SinF(t * t * MATH_PI);
	held_entity.RotY  -= Math_SinF(sqrtLerpPI) * 80.0f;
	held_entity.Yaw   -= Math_SinF(sqrtLerpPI) * 80.0f;
	held_entity.RotX  += sinHalfCircleWeird    * 20.0f;
}

static void HeldBlockRenderer_ResetAnim(cc_bool setLastHeld, float period) {
	held_time = 0.0f; held_swingY = 0.0f;
	held_animating = false; held_swinging = false;
	held_period = period;
	if (setLastHeld) { held_lastBlock = Inventory_SelectedBlock; }
}

static PackedCol HeldBlockRenderer_GetCol(struct Entity* entity) {
	struct Entity* player;
	PackedCol col;
	float adjPitch, t, scale;

	player = &Entities.CurPlayer->Base;
	col    = player->VTABLE->GetCol(player);

	/* Adjust pitch so angle when looking straight down is 0. */
	adjPitch = player->Pitch - 90.0f;
	if (adjPitch < 0.0f) adjPitch += 360.0f;

	/* Adjust color so held block is brighter when looking straight up */
	t     = Math_AbsF(adjPitch - 180.0f) / 180.0f;
	scale = Math_Lerp(0.9f, 0.7f, t);
	return PackedCol_Scale(col, scale);
}

void HeldBlockRenderer_ClickAnim(cc_bool digging) {
	/* TODO: timing still not quite right, rotate2 still not quite right */
	HeldBlockRenderer_ResetAnim(true, digging ? 0.35 : 0.25);
	held_continuousMining = false;
	held_swinging  = false;
	held_breaking  = digging;
	held_animating = true;
	/* Start place animation at bottom of cycle */
	if (!digging) held_time = held_period / 2;
}

void HeldBlockRenderer_SetMining(cc_bool mining) {
	if (!mining) {
		held_continuousMining = false;
		if (held_breaking) HeldBlockRenderer_ResetAnim(true, 0.25f);
		return;
	}

	if (held_continuousMining && held_breaking && held_animating) return;
	HeldBlockRenderer_ResetAnim(true, 0.35f);
	held_continuousMining = true;
	held_swinging  = false;
	held_breaking  = true;
	held_animating = true;
}

static void DoSwitchBlockAnim(void* obj) {
	if (held_swinging) {
		/* Like graph -sin(x) : x=0.5 and x=2.5 have same y values,
		   but increasing x causes y to change in opposite directions */
		if (held_time > held_period * 0.5f) {
			held_time = held_period - held_time;
		}
	} else {
		if (held_block == Inventory_SelectedBlock) return;
		HeldBlockRenderer_ResetAnim(false, 0.25);
		held_animating = true;
		held_swinging = true;
	}
}

static void OnBlockChanged(void* obj, IVec3 coords, BlockID old, BlockID now) {
	if (now == BLOCK_AIR) return;
	HeldBlockRenderer_ClickAnim(false);
}

static void DoAnimation(float delta, float lastSwingY) {
	float t;
	if (!held_animating) return;

	if (held_swinging || !held_breaking) {
		t = held_time / held_period;
		held_swingY = -0.4f * Math_SinF(t * MATH_PI);
		held_entity.Position.y += held_swingY;

		if (held_swinging) {
			/* i.e. the block has gone to bottom of screen and is now returning back up. 
			   At this point we switch over to the new held block. */
			if (held_swingY > lastSwingY) held_lastBlock = held_block;
			held_block = held_lastBlock;
			held_entity.ModelBlock = held_block;
		}
	} else {
		HeldBlockRenderer_DigAnimation();
	}
	
	held_time += delta;
	if (held_time > held_period) {
		if (held_continuousMining && held_breaking) {
			held_time = Math_Mod1(held_time / held_period) * held_period;
		} else {
			HeldBlockRenderer_ResetAnim(true, 0.25f);
		}
	}
}

void HeldBlockRenderer_Render(float delta) {
	float lastSwingY;
	struct Matrix view;
	if (!HeldBlockRenderer_Show) return;

	lastSwingY  = held_swingY;
	held_swingY = 0.0f;
	held_block  = Inventory_SelectedBlock;
	view = Gfx.View;

	Gfx_LoadMatrix(MATRIX_PROJ, &held_blockProj);
	SetMatrix();

	ResetHeldState();
	DoAnimation(delta, lastSwingY);
	ApplyHandSway(delta);
	SetBaseOffset();
	HeldBlockRenderer_EatAnimation();
	if (!Camera.Active->isThirdPerson) {
		if (SurvivalItem_IsItem(held_block)) {
			HeldBlockRenderer_RenderItem();
		} else {
			HeldBlockRenderer_RenderModel();
		}
	}

	Gfx.View = view;
	Gfx_LoadMatrix(MATRIX_PROJ, &Gfx.Projection);
}


static void OnContextLost(void* obj) {
	Gfx_DeleteDynamicVb(&held_entity.ModelVB);
	Gfx_DeleteDynamicVb(&held_itemVB);
}

static void OnFree(void) {
	OnContextLost(NULL);
	Mem_Free(held_itemsBmp.scan0);
	held_itemsBmp.scan0 = NULL;
}

static const struct EntityVTABLE heldEntity_VTABLE = {
	NULL, NULL, NULL, HeldBlockRenderer_GetCol,
	NULL, NULL
};
static void OnInit(void) {
	Entity_Init(&held_entity);
	held_entity.VTABLE  = &heldEntity_VTABLE;
	held_entity.NoShade = true;

	HeldBlockRenderer_Show = Options_GetBool(OPT_SHOW_BLOCK_IN_HAND, true);
	held_lastBlock         = Inventory_SelectedBlock;

	Event_Register_(&GfxEvents.ProjectionChanged, NULL, OnProjectionChanged);
	Event_Register_(&UserEvents.HeldBlockChanged, NULL, DoSwitchBlockAnim);
	Event_Register_(&UserEvents.BlockChanged,     NULL, OnBlockChanged);
	Event_Register_(&GfxEvents.ContextLost,       NULL, OnContextLost);
}
#else
cc_bool HeldBlockRenderer_IsEating;
void HeldBlockRenderer_StartEating(void) { HeldBlockRenderer_IsEating = true; }
void HeldBlockRenderer_StopEating(void) { HeldBlockRenderer_IsEating = false; }
void HeldBlockRenderer_TickEating(float delta) { }
void HeldBlockRenderer_ClickAnim(cc_bool digging) { }
void HeldBlockRenderer_SetMining(cc_bool mining) { }
void HeldBlockRenderer_Render(float delta) { }

static void OnInit(void) { }
static void OnFree(void) { }
#endif

struct IGameComponent HeldBlockRenderer_Component = {
	OnInit, /* Init  */
	OnFree  /* Free  */
};
