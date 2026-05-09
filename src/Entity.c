#include "Entity.h"
#include "ExtMath.h"
#include "World.h"
#include "Block.h"
#include "Event.h"
#include "Game.h"
#include "Camera.h"
#include "Audio.h"
#include "Platform.h"
#include "Funcs.h"
#include "Graphics.h"
#include "Lighting.h"
#include "Http.h"
#include "Chat.h"
#include "Model.h"
#include "Input.h"
#include "InputHandler.h"
#include "Gui.h"
#include "Inventory.h"
#include "Stream.h"
#include "Bitmap.h"
#include "Logger.h"
#include "Options.h"
#include "Errors.h"
#include "Utils.h"
#include "EntityRenderers.h"
#include "Protocol.h"
#include "Server.h"
#include "HeldBlockRenderer.h"
#include "Particle.h"
#include "SurvivalItems.h"
#include "Audio.h"

const char* const NameMode_Names[NAME_MODE_COUNT]   = { "None", "Hovered", "All", "AllHovered", "AllUnscaled" };
const char* const ShadowMode_Names[SHADOW_MODE_COUNT] = { "None", "SnapToBlock", "Circle", "CircleAll" };


/*########################################################################################################################*
*---------------------------------------------------------Entity----------------------------------------------------------*
*#########################################################################################################################*/
static PackedCol Entity_GetColor(struct Entity* e) {
	Vec3 eyePos = Entity_GetEyePosition(e);
	IVec3 pos; IVec3_Floor(&pos, &eyePos);
	return Lighting.Color(pos.x, pos.y, pos.z);
}

void Entity_Init(struct Entity* e) {
	static const cc_string model = String_FromConst("humanoid");
	Vec3_Set(e->ModelScale, 1,1,1);
	e->Flags      = ENTITY_FLAG_HAS_MODELVB;
	e->uScale     = 1.0f;
	e->vScale     = 1.0f;
	e->PushStrength = 1.0f;
	e->_skinReqID = 0;
	e->SkinRaw[0] = '\0';
	e->NameRaw[0] = '\0';
	Entity_SetModel(e, &model);
}

void Entity_SetName(struct Entity* e, const cc_string* name) {
	EntityNames_Delete(e);
	String_CopyToRawArray(e->NameRaw, name);
}

Vec3 Entity_GetEyePosition(struct Entity* e) {
	Vec3 pos = e->Position; pos.y += Entity_GetEyeHeight(e); return pos;
}

float Entity_GetEyeHeight(struct Entity* e) {
	return e->Model->GetEyeY(e) * e->ModelScale.y;
}

void Entity_GetTransform(struct Entity* e, Vec3 pos, Vec3 scale, struct Matrix* m) {
	struct Matrix tmp;
	Matrix_Scale(m, scale.x, scale.y, scale.z);

	if (e->RotZ != 0.0f) {
		Matrix_RotateZ( &tmp, -e->RotZ * MATH_DEG2RAD);
		Matrix_MulBy(m, &tmp);
	}
	if (e->RotX != 0.0f) {
		Matrix_RotateX( &tmp, -e->RotX * MATH_DEG2RAD);
		Matrix_MulBy(m, &tmp);
	}
	if (e->RotY != 0.0f) {
		Matrix_RotateY( &tmp, -e->RotY * MATH_DEG2RAD);
		Matrix_MulBy(m, &tmp);
	}

	Matrix_Translate(&tmp, pos.x, pos.y, pos.z);
	Matrix_MulBy(m,  &tmp);
	/* return scale * rotZ * rotX * rotY * translate; */
}

void Entity_GetPickingBounds(struct Entity* e, struct AABB* bb) {
	AABB_Offset(bb, &e->ModelAABB, &e->Position);
}

void Entity_GetBounds(struct Entity* e, struct AABB* bb) {
	AABB_Make(bb, &e->Position, &e->Size);
}

static void Entity_ParseScale(struct Entity* e, const cc_string* scale) {
	float value;
	if (!Convert_ParseFloat(scale, &value)) return;
	value = max(value, 0.001f);

	/* local player doesn't allow giant model scales */
	/* (can't climb stairs, extremely CPU intensive collisions) */
	if (e->Flags & ENTITY_FLAG_MODEL_RESTRICTED_SCALE) {
		value = min(value, e->Model->maxScale);
	}
	Vec3_Set(e->ModelScale, value,value,value);
}

static void Entity_SetBlockModel(struct Entity* e, const cc_string* model) {
	static const cc_string block = String_FromConst("block");
	int raw = Block_Parse(model);

	if (raw == -1) {
		/* use default humanoid model */
		e->Model      = Models.Human;
	} else {	
		e->ModelBlock = (BlockID)raw;
		e->Model      = Model_Get(&block);
	}
}

void Entity_SetModel(struct Entity* e, const cc_string* model) {
	cc_string name, scale;
	Vec3_Set(e->ModelScale, 1,1,1);
	String_UNSAFE_Separate(model, '|', &name, &scale);

	/* 'giant' model kept for backwards compatibility */
	if (String_CaselessEqualsConst(&name, "giant")) {
		name = String_FromReadonly("humanoid");
		Vec3_Set(e->ModelScale, 2,2,2);
	}

	e->ModelBlock = BLOCK_AIR;
	e->Model      = Model_Get(&name);
	if (!e->Model) Entity_SetBlockModel(e, &name);

	Entity_ParseScale(e, &scale);
	Entity_UpdateModelBounds(e);

	if (e->Flags & ENTITY_FLAG_HAS_MODELVB)
		Gfx_DeleteDynamicVb(&e->ModelVB);
}

void Entity_UpdateModelBounds(struct Entity* e) {
	struct Model* model = e->Model;
	model->GetCollisionSize(e);
	model->GetPickingBounds(e);

	Vec3_Mul3By(&e->Size,          &e->ModelScale);
	Vec3_Mul3By(&e->ModelAABB.Min, &e->ModelScale);
	Vec3_Mul3By(&e->ModelAABB.Max, &e->ModelScale);
}

cc_bool Entity_TouchesAny(struct AABB* bounds, Entity_TouchesCondition condition) {
	IVec3 bbMin, bbMax;
	BlockID block;
	struct AABB blockBB;
	Vec3 v;
	int x, y, z;

	IVec3_Floor(&bbMin, &bounds->Min);
	IVec3_Floor(&bbMax, &bounds->Max);

	bbMin.x = max(bbMin.x, 0); bbMax.x = min(bbMax.x, World.MaxX);
	bbMin.y = max(bbMin.y, 0); bbMax.y = min(bbMax.y, World.MaxY);
	bbMin.z = max(bbMin.z, 0); bbMax.z = min(bbMax.z, World.MaxZ);

	for (y = bbMin.y; y <= bbMax.y; y++) { v.y = (float)y;
		for (z = bbMin.z; z <= bbMax.z; z++) { v.z = (float)z;
			for (x = bbMin.x; x <= bbMax.x; x++) { v.x = (float)x;

				block = World_GetBlock(x, y, z);
				Vec3_Add(&blockBB.Min, &v, &Blocks.MinBB[block]);
				Vec3_Add(&blockBB.Max, &v, &Blocks.MaxBB[block]);

				if (!AABB_Intersects(&blockBB, bounds)) continue;
				if (condition(block)) return true;
			}
		}
	}
	return false;
}

static cc_bool IsRopeCollide(BlockID b) { return Blocks.ExtendedCollide[b] == COLLIDE_CLIMB; }
cc_bool Entity_TouchesAnyRope(struct Entity* e) {
	struct AABB bounds; Entity_GetBounds(e, &bounds);
	bounds.Max.y += 0.5f / 16.0f;
	return Entity_TouchesAny(&bounds, IsRopeCollide);
}

static const Vec3 entity_liqExpand = { 0.25f/16.0f, 0.0f/16.0f, 0.25f/16.0f };
static cc_bool IsLavaCollide(BlockID b) { return Blocks.ExtendedCollide[b] == COLLIDE_LAVA; }
cc_bool Entity_TouchesAnyLava(struct Entity* e) {
	struct AABB bounds; Entity_GetBounds(e, &bounds);
	AABB_Offset(&bounds, &bounds, &entity_liqExpand);
	return Entity_TouchesAny(&bounds, IsLavaCollide);
}

static cc_bool IsWaterCollide(BlockID b) { return Blocks.ExtendedCollide[b] == COLLIDE_WATER; }
cc_bool Entity_TouchesAnyWater(struct Entity* e) {
	struct AABB bounds; Entity_GetBounds(e, &bounds);
	AABB_Offset(&bounds, &bounds, &entity_liqExpand);
	return Entity_TouchesAny(&bounds, IsWaterCollide);
}


/*########################################################################################################################*
*------------------------------------------------------Entity skins-------------------------------------------------------*
*#########################################################################################################################*/
/* Copies skin data from another entity */
static void Entity_CopySkin(struct Entity* dst, struct Entity* src) {
	dst->TextureId	= src->TextureId;	
	dst->SkinType	= src->SkinType;
	dst->uScale		= src->uScale;
	dst->vScale		= src->vScale;
}

/* Resets skin data for the given entity */
static void Entity_ResetSkin(struct Entity* e) {
	e->TextureId    = 0;
	e->uScale 		= 1.0f; 
	e->vScale 		= 1.0f;
}

static void CheckSkin_Unchecked(struct Entity* e) {
	cc_string skin, eSkin;
	struct Entity* other;
	cc_uint8 flags;
	int i;

	skin = String_FromRawArray(e->SkinRaw);
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) 
	{
		other = Entities.List[i];
		if (!other) continue;
		/* Don't bother checking for other == e, as e->state is UNCHECKED anyways */
		if (other->SkinFetchState < SKIN_FETCH_DOWNLOADING) continue;

		eSkin = String_FromRawArray(other->SkinRaw);
		if (!String_Equals(&skin, &eSkin)) continue;

		/* Another entity with same skin either finished or is downloading */
		if (other->SkinFetchState == SKIN_FETCH_COMPLETED) {
			Entity_CopySkin(e, other);
			e->SkinFetchState = SKIN_FETCH_COMPLETED;
		} else {
			e->SkinFetchState = SKIN_FETCH_WAITINGFOR;
		}
		return;
	}

	flags = e == &LocalPlayer_Instances[0].Base ? HTTP_FLAG_NOCACHE : 0;
	e->_skinReqID     = Http_AsyncGetSkin(&skin, flags);
	e->SkinFetchState = SKIN_FETCH_DOWNLOADING;
}

/* Copies or resets skin data for all entity with same skin */
static void Entity_SetSkinAll(struct Entity* source, cc_bool reset) {
	struct Entity* e;
	cc_string skin, eSkin;
	int i;
	
	skin = String_FromRawArray(source->SkinRaw);

	for (i = 0; i < ENTITIES_MAX_COUNT; i++) 
	{
		if (!Entities.List[i]) continue;

		e     = Entities.List[i];
		eSkin = String_FromRawArray(e->SkinRaw);
		if (!String_Equals(&skin, &eSkin)) continue;

		if (reset) {
			Entity_ResetSkin(e);
		} else {
			Entity_CopySkin(e, source);
		}
		e->SkinFetchState = SKIN_FETCH_COMPLETED;
	}
}

/* Clears hat area from a skin bitmap if it's completely white or black,
   so skins edited with Microsoft Paint or similiar don't have a solid hat */
static void Entity_ClearHat(struct Bitmap* bmp, cc_uint8 skinType) {
	int sizeX  = (bmp->width / 64) * 32;
	int yScale = skinType == SKIN_64x32 ? 32 : 64;
	int sizeY  = (bmp->height / yScale) * 16;
	int x, y;

	/* determine if we actually need filtering */
	for (y = 0; y < sizeY; y++) {
		BitmapCol* row = Bitmap_GetRow(bmp, y) + sizeX;
		for (x = 0; x < sizeX; x++) {
			if (BitmapCol_A(row[x]) != 255) return;
		}
	}

	/* only perform filtering when the entire hat is opaque */
	for (y = 0; y < sizeY; y++) {
		BitmapCol* row = Bitmap_GetRow(bmp, y) + sizeX;
		for (x = 0; x < sizeX; x++) {
			BitmapCol c = row[x];
			if (c == BITMAPCOLOR_WHITE || c == BITMAPCOLOR_BLACK) row[x] = 0;
		}
	}
}

/* Ensures skin is a power of two size, resizing if needed. */
static cc_result EnsurePow2Skin(struct Entity* e, struct Bitmap* bmp) {
	struct Bitmap scaled;
	cc_uint32 stride;
	int width, height;
	int y;

	width  = Math_NextPowOf2(bmp->width);
	height = Math_NextPowOf2(bmp->height);
	if (width == bmp->width && height == bmp->height) return 0;

	scaled.width  = width; 
	scaled.height = height;
	scaled.scan0  = (BitmapCol*)Mem_TryAllocCleared(width * height, BITMAPCOLOR_SIZE);
	if (!scaled.scan0) return ERR_OUT_OF_MEMORY;

	e->uScale = (float)bmp->width  / width;
	e->vScale = (float)bmp->height / height;
	stride = bmp->width * 4;

	for (y = 0; y < bmp->height; y++) {
		BitmapCol* src = Bitmap_GetRow(bmp, y);
		BitmapCol* dst = Bitmap_GetRow(&scaled, y);
		Mem_Copy(dst, src, stride);
	}

	Mem_Free(bmp->scan0);
	*bmp = scaled;
	return 0;
}

static cc_result ApplySkin(struct Entity* e, struct Bitmap* bmp, struct Stream* src, cc_string* skin) {
	cc_result res;
	if ((res = Png_Decode(bmp, src))) return res;

	Gfx_DeleteTexture(&e->TextureId);
	if ((res = EnsurePow2Skin(e, bmp))) return res;
	e->SkinType = Utils_CalcSkinType(bmp);

	if (!Gfx_CheckTextureSize(bmp->width, bmp->height, 0)) {
		Chat_Add1("&cSkin %s is too large", skin);
	} else {
		if (e->Model->flags & MODEL_FLAG_CLEAR_HAT)
			Entity_ClearHat(bmp, e->SkinType);

		e->TextureId = Gfx_CreateTexture(bmp, TEXTURE_FLAG_MANAGED, false);
		Entity_SetSkinAll(e, false);
	}
	return 0;
}

static void LogInvalidSkin(cc_result res, const cc_string* skin, const cc_uint8* data, int size) {
	cc_string msg; char msgBuffer[256];
	String_InitArray(msg, msgBuffer);

	Logger_FormatWarn2(&msg, res, "decoding skin", skin, Platform_DescribeError);
	if (res != PNG_ERR_INVALID_SIG) { Logger_WarnFunc(&msg); return; }

	String_AppendConst(&msg, " (got ");
	String_AppendAll(  &msg, data, min(size, 8));
	String_AppendConst(&msg, ")");
	Logger_WarnFunc(&msg);
}

static void CheckSkin_Downloading(struct Entity* e) {
	struct HttpRequest item;
	struct Stream mem;
	struct Bitmap bmp;
	cc_string skin;
	cc_result res;

	if (!Http_GetResult(e->_skinReqID, &item)) return;
	Entity_SetSkinAll(e, true);
	if (!item.success) return;

	Stream_ReadonlyMemory(&mem, item.data, item.size);
	skin = String_FromRawArray(e->SkinRaw);

	if ((res = ApplySkin(e, &bmp, &mem, &skin))) {
		LogInvalidSkin(res, &skin, item.data, item.size);
	}

	Mem_Free(bmp.scan0);
	HttpRequest_Free(&item);
}

static void Entity_CheckSkin(struct Entity* e) {
	/* Don't check skin if don't have to */
	if (!e->Model->usesSkin) return;

	switch (e->SkinFetchState)
	{
	case SKIN_FETCH_UNCHECKED:
		CheckSkin_Unchecked(e); return;
	case SKIN_FETCH_WAITINGFOR:
		return; /* Waiting for another entity to download it */
	case SKIN_FETCH_DOWNLOADING:
		CheckSkin_Downloading(e); return;
	case SKIN_FETCH_COMPLETED:
		return; /* Nothing to do as skin has been downloaded */
	}
}

/* Returns whether this entity is currently waiting on given skin to download */
static CC_INLINE cc_bool IsWaitingForSkinToDownload(struct Entity* e, cc_string* skin) {
	cc_string eSkin;
	if (e->SkinFetchState != SKIN_FETCH_WAITINGFOR) return false;

	eSkin = String_FromRawArray(e->SkinRaw);
	return String_Equals(skin, &eSkin);
}

/* Transfers skin downloading responsibility to another entity */
static void TransferSkinDownload(struct Entity* e, struct Entity* src) {
	e->SkinFetchState = SKIN_FETCH_DOWNLOADING;
	e->_skinReqID     = src->_skinReqID;
}

/* Either transfers skin download or cancels it altogether */
static void DerefDownloadingSkin(struct Entity* src) {
	struct Entity* e;
	cc_string skin = String_FromRawArray(src->SkinRaw);
	int i;

	for (i = 0; i < ENTITIES_MAX_COUNT; i++) 
	{
		if (!Entities.List[i]) continue;
		e  = Entities.List[i];

		if (!IsWaitingForSkinToDownload(e, &skin)) continue;
		Platform_Log1("Transferring skin download: %s", &skin);
		TransferSkinDownload(e, src);
		return;
	}
	
	Platform_Log1("Cancelling skin download: %s", &skin);
	Http_TryCancel(src->_skinReqID);
}

/* Returns true if no other entities are sharing this skin texture */
static cc_bool CanDeleteTexture(struct Entity* except) {
	int i;
	if (!except->TextureId) return false;

	for (i = 0; i < ENTITIES_MAX_COUNT; i++)
	{
		if (!Entities.List[i] || Entities.List[i] == except)  continue;
		if (Entities.List[i]->TextureId == except->TextureId) return false;
	}
	return true;
}

CC_NOINLINE static void DeleteSkin(struct Entity* e) {
	if (CanDeleteTexture(e)) Gfx_DeleteTexture(&e->TextureId);
	if (e->SkinFetchState == SKIN_FETCH_DOWNLOADING) DerefDownloadingSkin(e);

	Entity_ResetSkin(e);
	e->SkinFetchState = SKIN_FETCH_UNCHECKED;
}

void Entity_SetSkin(struct Entity* e, const cc_string* skin) {
	cc_string tmp; char tmpBuffer[STRING_SIZE];
	DeleteSkin(e);

	if (Utils_IsUrlPrefix(skin)) {
		tmp = *skin;
		e->NonHumanSkin = true;
	} else {
		String_InitArray(tmp, tmpBuffer);
		String_AppendColorless(&tmp, skin);
		e->NonHumanSkin = false;
	}
	String_CopyToRawArray(e->SkinRaw, &tmp);
}

void Entity_LerpAngles(struct Entity* e, float t) {
	struct EntityLocation* prev = &e->prev;
	struct EntityLocation* next = &e->next;

	e->Pitch = Math_LerpAngle(prev->pitch, next->pitch, t);
	e->Yaw   = Math_LerpAngle(prev->yaw,   next->yaw,   t);
	e->RotX  = Math_LerpAngle(prev->rotX,  next->rotX,  t);
	e->RotY  = Math_LerpAngle(prev->rotY,  next->rotY,  t);
	e->RotZ  = Math_LerpAngle(prev->rotZ,  next->rotZ,  t);
}


/*########################################################################################################################*
*--------------------------------------------------------Entities---------------------------------------------------------*
*#########################################################################################################################*/
struct _EntitiesData Entities;

#define MOBS_FIRST (MAX_NET_PLAYERS + MAX_LOCAL_PLAYERS)
#define DROPPED_ITEMS_FIRST (MOBS_FIRST + MAX_MOBS)
#define DROPPED_ITEM_PICKUP_DELAY 0.5f
#define DROPPED_ITEM_LIFETIME 300.0f
#define DROPPED_ITEM_PICKUP_RADIUS 1.25f

struct DroppedItem {
	struct Entity Base;
	struct CollisionsComp Collisions;
	BlockID Block;
	float Age;
	int EntityID;
};
static struct DroppedItem DroppedItems[MAX_DROPPED_ITEMS];

static void DroppedItem_Despawn(struct Entity* e) {
	DeleteSkin(e);
	EntityNames_Delete(e);

	if (e->Flags & ENTITY_FLAG_HAS_MODELVB)
		Gfx_DeleteDynamicVb(&e->ModelVB);
}

static void DroppedItem_TryPickup(struct DroppedItem* item) {
	struct Entity* e = &item->Base;
	struct Entity* p = &Entities.CurPlayer->Base;
	Vec3 delta;
	float radius;

	if (item->Age < DROPPED_ITEM_PICKUP_DELAY) return;
	Vec3_Sub(&delta, &e->Position, &p->Position);
	radius = DROPPED_ITEM_PICKUP_RADIUS;
	if (Vec3_LengthSquared(&delta) > radius * radius) return;
	if (!Inventory_TryAdd(item->Block)) return;

	Server_SendPickedItem(item->Block, e->Position);
	Entities_Remove(item->EntityID);
}

static void DroppedItem_Tick(struct Entity* e, float delta) {
	struct DroppedItem* item = (struct DroppedItem*)e;
	item->Age += delta;
	if (!World.Loaded || item->Age >= DROPPED_ITEM_LIFETIME) {
		Entities_Remove(item->EntityID);
		return;
	}

	e->Velocity.y -= 0.04f;
	if (e->Velocity.y < -0.5f) e->Velocity.y = -0.5f;
	Collisions_MoveAndWallSlide(&item->Collisions);
	Vec3_AddBy(&e->Position, &e->Velocity);

	if (item->Collisions.HitXMin || item->Collisions.HitXMax) e->Velocity.x = 0.0f;
	if (item->Collisions.HitZMin || item->Collisions.HitZMax) e->Velocity.z = 0.0f;
	if (item->Collisions.HitYMin || item->Collisions.HitYMax) e->Velocity.y = 0.0f;

	e->Velocity.x *= e->OnGround ? 0.70f : 0.98f;
	e->Velocity.z *= e->OnGround ? 0.70f : 0.98f;
	e->RotY = (float)(Game.Time * 90.0);
	DroppedItem_TryPickup(item);
}

static void DroppedItem_SetLocation(struct Entity* e, struct LocationUpdate* update) {
	if (update->flags & LU_HAS_POS) e->Position = update->pos;
	if (update->flags & LU_HAS_PITCH) e->Pitch = update->pitch;
	if (update->flags & LU_HAS_YAW) e->Yaw = update->yaw;
	if (update->flags & LU_HAS_ROTX) e->RotX = update->rotX;
	if (update->flags & LU_HAS_ROTZ) e->RotZ = update->rotZ;
}

static void DroppedItem_RenderModel(struct Entity* e, float delta, float t) {
	struct DroppedItem* item = (struct DroppedItem*)e;
	struct VertexTextured* vertices;
	TextureRec rec;
	TextureLoc loc;
	Vec2 size;
	Vec3 pos;
	int col, row;

	if (!SurvivalItem_IsItem(item->Block)) {
		Model_Render(e->Model, e);
		return;
	}

	if (!Gui.ItemsTex) return;

	loc = SurvivalItem_TextureLoc(item->Block);
	col = ((int)loc) & 15;
	row = ((int)loc) >> 4;

	rec.u1 = col / 16.0f;
	rec.v1 = row / 16.0f;
	rec.u2 = (col + 1) / 16.0f;
	rec.v2 = (row + 1) / 16.0f;

	size.x = 0.45f;
	size.y = 0.45f;
	pos    = e->Position;
	pos.y += 0.10f;

	if (!e->ModelVB) e->ModelVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, 4);

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_BindTexture(Gui.ItemsTex);

	vertices = (struct VertexTextured*)Gfx_LockDynamicVb(e->ModelVB, VERTEX_FORMAT_TEXTURED, 4);
	Particle_DoRender(&size, &pos, &rec, PACKEDCOL_WHITE, vertices);
	Gfx_UnlockDynamicVb(e->ModelVB);

	Gfx_SetAlphaTest(true);
	Gfx_SetAlphaBlending(true);
	Gfx_DrawVb_IndexedTris(4);
	Gfx_SetAlphaBlending(false);
	Gfx_SetAlphaTest(false);
}

static cc_bool DroppedItem_ShouldRenderName(struct Entity* e) { return false; }

static const struct EntityVTABLE droppedItem_VTABLE = {
	DroppedItem_Tick,        DroppedItem_Despawn,         DroppedItem_SetLocation, Entity_GetColor,
	DroppedItem_RenderModel, DroppedItem_ShouldRenderName
};

static void DroppedItem_ClearAll(void) {
	int i;
	for (i = DROPPED_ITEMS_FIRST; i < ENTITIES_MAX_COUNT; i++) {
		Entities_Remove(i);
	}
}

static cc_bool DroppedItem_SpawnCore(BlockID block, Vec3 pos, Vec3 vel) {
	struct DroppedItem* item;
	struct Entity* e;
	cc_string model; char modelBuffer[STRING_SIZE];
	int i, raw, blockRaw;

	if (block == BLOCK_AIR || Blocks.Draw[block] == DRAW_GAS) return false;
	for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
		raw = DROPPED_ITEMS_FIRST + i;
		if (Entities.List[raw]) continue;

		item = &DroppedItems[i];
		Mem_Set(item, 0, sizeof(struct DroppedItem));
		e    = &item->Base;
		Entity_Init(e);
		e->VTABLE = &droppedItem_VTABLE;
		e->Flags |= ENTITY_FLAG_HAS_MODELVB;
		e->ShouldRender = true;

		String_InitArray(model, modelBuffer);
		blockRaw = block;
		String_Format1(&model, "%i", &blockRaw);
		Entity_SetModel(e, &model);
		Vec3_Set(e->ModelScale, 0.35f, 0.35f, 0.35f);
		Entity_UpdateModelBounds(e);

		e->Position = pos;
		e->Velocity = vel;

		item->Block    = block;
		item->Age      = 0.0f;
		item->EntityID = raw;
		item->Collisions.Entity = e;
		item->Collisions.StepSize = 0.0f;
		Entities.List[raw] = e;
		Event_RaiseInt(&EntityEvents.Added, raw);
		return true;
	}
	return false;
}

cc_bool DroppedItem_Spawn(BlockID block) {
	struct LocalPlayer* p = Entities.CurPlayer;
	Vec3 pos, vel, dir;

	dir = Vec3_GetDirVector(p->Base.Yaw * MATH_DEG2RAD, p->Base.Pitch * MATH_DEG2RAD);
	pos = Entity_GetEyePosition(&p->Base);
	pos.x += dir.x * 0.6f;
	pos.y += dir.y * 0.6f - 0.25f;
	pos.z += dir.z * 0.6f;
	Vec3_Set(vel, dir.x * 0.22f, dir.y * 0.22f + 0.12f, dir.z * 0.22f);

	return DroppedItem_SpawnCore(block, pos, vel);
}

cc_bool DroppedItem_SpawnAt(BlockID block, Vec3 pos) {
	Vec3 vel;
	Vec3_Set(vel, 0.0f, 0.12f, 0.0f);
	return DroppedItem_SpawnCore(block, pos, vel);
}

cc_bool DroppedItem_SpawnAtVelocity(BlockID block, Vec3 pos, Vec3 vel) {
	return DroppedItem_SpawnCore(block, pos, vel);
}

cc_bool DroppedItem_RemoveNearest(BlockID block, Vec3 pos) {
	struct DroppedItem* item;
	struct Entity* e;
	Vec3 delta;
	float dist, bestDist = 64.0f;
	int i, best = -1;

	for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
		item = &DroppedItems[i];
		if (item->Block != block) continue;
		if (item->EntityID < DROPPED_ITEMS_FIRST) continue;
		e = Entities.List[item->EntityID];
		if (e != &item->Base) continue;

		Vec3_Sub(&delta, &e->Position, &pos);
		dist = Vec3_LengthSquared(&delta);
		if (dist >= bestDist) continue;
		bestDist = dist;
		best     = i;
	}

	if (best < 0) return false;
	Entities_Remove(DroppedItems[best].EntityID);
	return true;
}



/*########################################################################################################################*
*-----------------------------------------------------Survival mobs-------------------------------------------------------*
*#########################################################################################################################*/
#define MOB_YAW_OFFSET          90.0f
#define MOB_SPAWN_INTERVAL      10.0f
#define MOB_MAX_ALIVE           14
#define MOB_MAX_HOSTILE         8
#define MOB_MAX_PASSIVE         6

/* Survival day/night cycle. One full cycle is 4 minutes for testing.
 * 0.0 = sunrise, 0.25 = noon, 0.50 = sunset, 0.75 = midnight. */
#define SURVIVAL_DAY_LENGTH     240.0f
#define MOB_DESPAWN_RANGE       96.0f
#define MOB_CHASE_RANGE         24.0f
#define MOB_ATTACK_COOLDOWN     1.0f
#define MOB_WANDER_SPEED        0.035f
#define MOB_TURN_SPEED          0.14f
#define MOB_CLOSE_STOP_SCALE    0.90f
#define MOB_STUCK_JUMP_TIME     0.25f
#define MOB_JUMP_COOLDOWN       0.65f
#define MOB_LEDGE_JUMP_VEL      0.34f
#define MOB_SPIDER_JUMP_VEL     0.42f
#define MOB_KNOCKBACK_TIME      0.28f
#define MOB_KNOCKBACK_FRICTION  0.88f
#define MOB_BURN_INTERVAL       1.0f
#define MOB_BURN_DAMAGE         1

#define MOB_KIND_ZOMBIE         0
#define MOB_KIND_SKELETON       1
#define MOB_KIND_CREEPER        2
#define MOB_KIND_SPIDER         3
#define MOB_KIND_PIG            4
#define MOB_KIND_SHEEP          5

struct Mob {
	struct Entity Base;
	struct CollisionsComp Collisions;
	int EntityID;
	int Kind;
	int Health;
	int Damage;
	float Speed;
	float AttackRange;
	float AttackCooldown;
	float WanderCooldown;
	float StuckTimer;
	float JumpCooldown;
	float KnockbackTimer;
	float BurnTimer;
	Vec3 WanderDir;
};
static struct Mob Mobs[MAX_MOBS];
static RNGState mob_rnd;
static cc_bool mob_rndSeeded;
static float mob_spawnTimer;
static float survival_dayTime = 0.0f;
static float survival_envTimer;

static void LocalPlayer_Damage(struct LocalPlayer* p, int damage);
static void Mob_Tick(struct Entity* e, float delta);

static void Mob_SeedRandom(void) {
	if (mob_rndSeeded) return;
	Random_SeedFromCurrentTime(&mob_rnd);
	mob_rndSeeded = true;
}

static void Mob_Despawn(struct Entity* e) {
	DeleteSkin(e);
	EntityNames_Delete(e);
	if (e->Flags & ENTITY_FLAG_HAS_MODELVB) Gfx_DeleteDynamicVb(&e->ModelVB);
}

static void Mob_SetLocation(struct Entity* e, struct LocationUpdate* update) {
	if (update->flags & LU_HAS_POS) e->Position = update->pos;
	if (update->flags & LU_HAS_PITCH) e->Pitch = update->pitch;
	if (update->flags & LU_HAS_YAW) {
		e->Yaw  = update->yaw;
		e->RotY = update->yaw;
	}
	if (update->flags & LU_HAS_ROTX) e->RotX = update->rotX;
	if (update->flags & LU_HAS_ROTZ) e->RotZ = update->rotZ;
}

static void Mob_RenderModel(struct Entity* e, float delta, float t) {
	AnimatedComp_GetCurrent(e, t);
	Model_Render(e->Model, e);
}

static cc_bool Mob_ShouldRenderName(struct Entity* e) { return false; }

static const struct EntityVTABLE mob_VTABLE = {
	Mob_Tick,        Mob_Despawn,         Mob_SetLocation, Entity_GetColor,
	Mob_RenderModel, Mob_ShouldRenderName
};

static void Mob_FaceDirection(struct Entity* e, const Vec3* dir) {
	float yaw;
	if (Math_AbsF(dir->x) < 0.0001f && Math_AbsF(dir->z) < 0.0001f) return;

	/* ClassicalSharp heading convention, plus CavFX mob model forward-axis correction. */
	yaw = (float)(Math_Atan2f(dir->x, dir->z) * MATH_RAD2DEG);
	yaw += MOB_YAW_OFFSET;

	/* Snap-turn like ClassicalSharp Survival. */
	e->RotY  = yaw;
	e->Yaw   = yaw;
	e->Pitch = 0.0f;
}

static cc_bool Mob_IsHostileKind(int kind) {
	return kind == MOB_KIND_ZOMBIE || kind == MOB_KIND_SKELETON || kind == MOB_KIND_CREEPER || kind == MOB_KIND_SPIDER;
}

static cc_bool Mob_IsPassiveKind(int kind) {
	return kind == MOB_KIND_PIG || kind == MOB_KIND_SHEEP;
}

static cc_bool Survival_IsNight(void) {
	/* Night begins after sunset and ends at sunrise. */
	return survival_dayTime >= 0.50f;
}

static void SurvivalDayNight_Tick(float delta) {
	float nightBlend;
	PackedCol skyDay, fogDay, cloudsDay, sunDay;
	PackedCol skyNight, fogNight, cloudsNight, sunNight;

	if (!Game_SurvivalMode || !World.Loaded) return;

	survival_dayTime += delta / SURVIVAL_DAY_LENGTH;
	while (survival_dayTime >= 1.0f) survival_dayTime -= 1.0f;

	/* Only update environment 4 times per second to avoid spamming EnvVarChanged events. */
	survival_envTimer += delta;
	if (survival_envTimer < 0.25f) return;
	survival_envTimer = 0.0f;

	/* Smooth darkness curve: 0 at day, 1 at midnight. */
	if (survival_dayTime < 0.50f) nightBlend = 0.0f;
	else {
		nightBlend = (survival_dayTime - 0.50f) / 0.50f;
		if (nightBlend > 0.5f) nightBlend = 1.0f - nightBlend;
		nightBlend *= 2.0f;
	}

	skyDay     = PackedCol_Make(0x99, 0xCC, 0xFF, 0xFF);
	fogDay     = PackedCol_Make(0xFF, 0xFF, 0xFF, 0xFF);
	cloudsDay  = PackedCol_Make(0xFF, 0xFF, 0xFF, 0xFF);
	sunDay     = PackedCol_Make(0xFF, 0xFF, 0xFF, 0xFF);

	skyNight    = PackedCol_Make(0x10, 0x18, 0x30, 0xFF);
	fogNight    = PackedCol_Make(0x18, 0x18, 0x24, 0xFF);
	cloudsNight = PackedCol_Make(0x45, 0x45, 0x55, 0xFF);
	sunNight    = PackedCol_Make(0x55, 0x60, 0x80, 0xFF);

	Env_SetSkyCol(PackedCol_Lerp(skyDay,    skyNight,    nightBlend));
	Env_SetFogCol(PackedCol_Lerp(fogDay,    fogNight,    nightBlend));
	Env_SetCloudsCol(PackedCol_Lerp(cloudsDay, cloudsNight, nightBlend));
	Env_SetSunCol(PackedCol_Lerp(sunDay,    sunNight,    nightBlend));
}

static void Mob_MoveTowards(struct Mob* mob, const Vec3* dir, float speed) {
	struct Entity* e = &mob->Base;
	Vec3 move = *dir;
	float lenSq, len;

	move.y = 0.0f;
	lenSq = Vec3_LengthSquared(&move);
	if (lenSq <= 0.0001f) {
		e->Velocity.x = 0.0f;
		e->Velocity.z = 0.0f;
		return;
	}

	len = Math_SqrtF(lenSq);
	move.x /= len;
	move.z /= len;
	e->Velocity.x = move.x * speed;
	e->Velocity.z = move.z * speed;
	Mob_FaceDirection(e, &move);
}


static cc_bool Mob_ShouldBurnInSun(struct Mob* mob) {
	struct Entity* e = &mob->Base;
	IVec3 eye;

	/* Minecraft-style rule: only undead mobs burn, and only in direct sun. */
	if (mob->Kind != MOB_KIND_ZOMBIE && mob->Kind != MOB_KIND_SKELETON) return false;
	if (Survival_IsNight()) return false;
	if (Env.Weather != WEATHER_SUNNY) return false;
	if (Entity_TouchesAnyWater(e)) return false;

	/* Check the mob eye/head block. Lighting.IsLit means there is sky light here,
	 * so caves, shade, and covered mobs are safe. */
	IVec3_Floor(&eye, &e->Position);
	eye.y += (int)Entity_GetEyeHeight(e);
	return Lighting.IsLit(eye.x, eye.y, eye.z);
}

static void Mob_DropDeathLoot(struct Mob* mob) {
	Vec3 pos, vel;
	int i, count;

	pos = mob->Base.Position;
	pos.y += 0.35f;

	if (mob->Kind == MOB_KIND_PIG) {
		Mob_SeedRandom();
		count = 1 + (int)(Random_Float(&mob_rnd) * 3.0f);
		if (count > 3) count = 3;

		for (i = 0; i < count; i++) {
			vel.x = (Random_Float(&mob_rnd) - 0.5f) * 0.18f;
			vel.y = 0.12f + Random_Float(&mob_rnd) * 0.10f;
			vel.z = (Random_Float(&mob_rnd) - 0.5f) * 0.18f;
			DroppedItem_SpawnAtVelocity(SURVIVAL_ITEM_RAW_PORKCHOP, pos, vel);
		}
	}
}

static void Mob_Damage(struct Mob* mob, int damage) {
	if (damage <= 0) damage = 1;
	mob->Health -= damage;
	if (mob->Health <= 0) {
		Mob_DropDeathLoot(mob);
		Entities_Remove(mob->EntityID);
	}
}

static void Mob_ApplyKnockback(struct Mob* mob, const Vec3* from, float strength, float lift) {
	Vec3 knock;
	float lenSq, len;

	Vec3_Sub(&knock, &mob->Base.Position, from);
	knock.y = 0.0f;
	lenSq = Vec3_LengthSquared(&knock);
	if (lenSq <= 0.0001f) {
		knock.x = Math_SinF(mob->Base.Yaw * MATH_DEG2RAD);
		knock.z = Math_CosF(mob->Base.Yaw * MATH_DEG2RAD);
		lenSq = Vec3_LengthSquared(&knock);
	}
	if (lenSq <= 0.0001f) return;

	len = Math_SqrtF(lenSq);
	mob->Base.Velocity.x = (knock.x / len) * strength;
	mob->Base.Velocity.z = (knock.z / len) * strength;
	if (mob->Base.Velocity.y < lift) mob->Base.Velocity.y = lift;
	mob->KnockbackTimer = MOB_KNOCKBACK_TIME;
	mob->StuckTimer = 0.0f;
}

static void Mob_Wander(struct Mob* mob, float delta) {
	Mob_SeedRandom();
	mob->WanderCooldown -= delta;
	if (mob->WanderCooldown <= 0.0f) {
		mob->WanderCooldown = 1.0f + Random_Float(&mob_rnd) * 2.0f;
		mob->WanderDir.x = Random_Float(&mob_rnd) - 0.5f;
		mob->WanderDir.y = 0.0f;
		mob->WanderDir.z = Random_Float(&mob_rnd) - 0.5f;
	}
	Mob_MoveTowards(mob, &mob->WanderDir, MOB_WANDER_SPEED);
}

static void Mob_Tick(struct Entity* e, float delta) {
	struct Mob* mob = (struct Mob*)e;
	struct LocalPlayer* player = Entities.CurPlayer;
	Vec3 oldPos, diff, chase;
	float distSq, attackRange, movedX, movedZ, movedSq, intendedSq;
	cc_bool hitWall, wantsMove;

	if (!Game_SurvivalMode || !World.Loaded || !player) {
		Entities_Remove(mob->EntityID);
		return;
	}

	oldPos = e->Position;
	if (mob->AttackCooldown > 0.0f) mob->AttackCooldown -= delta;
	if (mob->JumpCooldown > 0.0f) mob->JumpCooldown -= delta;
	if (mob->KnockbackTimer > 0.0f) mob->KnockbackTimer -= delta;

	if (Mob_ShouldBurnInSun(mob)) {
		mob->BurnTimer += delta;
		if (mob->BurnTimer >= MOB_BURN_INTERVAL) {
			mob->BurnTimer = 0.0f;
			Mob_Damage(mob, MOB_BURN_DAMAGE);
			if (e->VTABLE == NULL || Entities.List[mob->EntityID] != e) return;
		}
	} else {
		mob->BurnTimer = 0.0f;
	}

	Vec3_Sub(&diff, &player->Base.Position, &e->Position);
	distSq = Vec3_LengthSquared(&diff);

	if (distSq > MOB_DESPAWN_RANGE * MOB_DESPAWN_RANGE) {
		Entities_Remove(mob->EntityID);
		return;
	}

	if (mob->KnockbackTimer > 0.0f) {
		/* Do not let AI overwrite knockback immediately after the player hits a mob. */
		e->Velocity.x *= MOB_KNOCKBACK_FRICTION;
		e->Velocity.z *= MOB_KNOCKBACK_FRICTION;
	} else if (Mob_IsPassiveKind(mob->Kind)) {
		Mob_Wander(mob, delta);
	} else if (distSq <= MOB_CHASE_RANGE * MOB_CHASE_RANGE) {
		attackRange = mob->AttackRange;
		chase = diff;
		chase.y = 0.0f;

		/* Stop trying to run through the player. This avoids close-range orbiting/jitter. */
		if (distSq <= (attackRange * MOB_CLOSE_STOP_SCALE) * (attackRange * MOB_CLOSE_STOP_SCALE)) {
			e->Velocity.x = 0.0f;
			e->Velocity.z = 0.0f;
			Mob_FaceDirection(e, &chase);
		} else if (mob->Kind == MOB_KIND_SKELETON && distSq < 5.0f * 5.0f) {
			chase.x = -chase.x;
			chase.z = -chase.z;
			Mob_MoveTowards(mob, &chase, mob->Speed * 0.80f);
		} else {
			Mob_MoveTowards(mob, &chase, mob->Speed);
		}
	} else {
		Mob_Wander(mob, delta);
	}

	/* Keep mobs from sinking like bricks in water, but still let them fall normally on land. */
	if (Entity_TouchesAnyWater(e)) {
		e->Velocity.y -= 0.015f;
		if (e->Velocity.y < -0.20f) e->Velocity.y = -0.20f;
	} else {
		e->Velocity.y -= 0.04f;
		if (e->Velocity.y < -0.5f) e->Velocity.y = -0.5f;
	}

	intendedSq = e->Velocity.x * e->Velocity.x + e->Velocity.z * e->Velocity.z;
	wantsMove  = intendedSq > 0.0001f;

	Collisions_MoveAndWallSlide(&mob->Collisions);
	Vec3_AddBy(&e->Position, &e->Velocity);

	movedX  = e->Position.x - oldPos.x;
	movedZ  = e->Position.z - oldPos.z;
	movedSq = movedX * movedX + movedZ * movedZ;
	hitWall = mob->Collisions.HitXMin || mob->Collisions.HitXMax || mob->Collisions.HitZMin || mob->Collisions.HitZMax;

	/*
	 * IMPORTANT: In CavFX/ClassiCube collision naming, HitYMax means the mob landed
	 * on top of a block. HitYMin is head/ceiling collision. The old mob code used
	 * HitYMin as ground, so the jump check almost never became true.
	 */
	if (mob->Collisions.HitYMax) e->OnGround = true;

	if (wantsMove && movedSq < 0.000025f) mob->StuckTimer += delta;
	else mob->StuckTimer = 0.0f;

	/* Jump up one-block ledges instead of freezing at block edges. */
	if ((hitWall || mob->StuckTimer >= MOB_STUCK_JUMP_TIME) && e->OnGround && mob->JumpCooldown <= 0.0f) {
		e->Velocity.y = mob->Kind == MOB_KIND_SPIDER ? MOB_SPIDER_JUMP_VEL : MOB_LEDGE_JUMP_VEL;
		mob->JumpCooldown = MOB_JUMP_COOLDOWN;
		mob->StuckTimer = 0.0f;
		e->OnGround = false;
	} else {
		if (mob->Collisions.HitXMin || mob->Collisions.HitXMax) e->Velocity.x = 0.0f;
		if (mob->Collisions.HitZMin || mob->Collisions.HitZMax) e->Velocity.z = 0.0f;
	}

	/* Hit ceiling while moving up. */
	if (mob->Collisions.HitYMin && e->Velocity.y > 0.0f) e->Velocity.y = 0.0f;
	/* Landed on ground while falling. */
	if (mob->Collisions.HitYMax && e->Velocity.y < 0.0f) e->Velocity.y = 0.0f;

	AnimatedComp_Update(e, oldPos, e->Position, delta);

	/* LAN host is authoritative for mobs: broadcast movement/state snapshots. */
	if (Server_IsLANHosted()) {
		Server_SendMobSnapshot((EntityID)mob->EntityID, (cc_uint8)mob->Kind, e->Position, e->Velocity, e->Yaw, e->Pitch, mob->Health);
	}

	attackRange = mob->AttackRange;
	if (Mob_IsHostileKind(mob->Kind) && distSq <= attackRange * attackRange && mob->AttackCooldown <= 0.0f) {
		mob->AttackCooldown = MOB_ATTACK_COOLDOWN;
		LocalPlayer_Damage(player, mob->Damage);
	}
}

static int Mob_CountAlive(void) {
	int i, count = 0;
	for (i = 0; i < MAX_MOBS; i++) {
		if (Mobs[i].EntityID >= MOBS_FIRST && Entities.List[Mobs[i].EntityID] == &Mobs[i].Base) count++;
	}
	return count;
}

static int Mob_CountKindGroup(cc_bool hostile) {
	int i, count = 0;
	for (i = 0; i < MAX_MOBS; i++) {
		if (Mobs[i].EntityID < MOBS_FIRST || Entities.List[Mobs[i].EntityID] != &Mobs[i].Base) continue;
		if (hostile && Mob_IsHostileKind(Mobs[i].Kind)) count++;
		if (!hostile && Mob_IsPassiveKind(Mobs[i].Kind)) count++;
	}
	return count;
}

static cc_bool Mob_SpawnCore(Vec3 pos, int kind) {
	struct Mob* mob;
	struct Entity* e;
	cc_string model;
	int i, raw;

	for (i = 0; i < MAX_MOBS; i++) {
		raw = MOBS_FIRST + i;
		if (Entities.List[raw]) continue;

		mob = &Mobs[i];
		Mem_Set(mob, 0, sizeof(struct Mob));
		e = &mob->Base;
		Entity_Init(e);
		e->VTABLE = &mob_VTABLE;
		e->Flags |= ENTITY_FLAG_HAS_MODELVB;
		e->ShouldRender = true;
		e->Position = pos;
		Vec3_Set(e->Velocity, 0.0f, 0.0f, 0.0f);

		mob->Kind = kind;
		mob->EntityID = raw;
		mob->Health = 20;
		mob->Damage = 2;
		mob->Speed = 0.065f;
		mob->AttackRange = 1.20f;
		mob->WanderCooldown = 0.0f;
		mob->StuckTimer = 0.0f;
		mob->JumpCooldown = 0.0f;
		Vec3_Set(mob->WanderDir, 0.0f, 0.0f, 1.0f);

		if (kind == MOB_KIND_SKELETON) {
			model = String_FromReadonly("skeleton");
			mob->Speed = 0.060f;
			mob->AttackRange = 1.35f;
		} else if (kind == MOB_KIND_CREEPER) {
			model = String_FromReadonly("creeper");
			mob->Speed = 0.060f;
			mob->Damage = 6;
			mob->AttackRange = 1.60f;
		} else if (kind == MOB_KIND_SPIDER) {
			model = String_FromReadonly("spider");
			mob->Health = 16;
			mob->Speed = 0.085f;
			mob->AttackRange = 1.30f;
		} else if (kind == MOB_KIND_PIG) {
			model = String_FromReadonly("pig");
			mob->Health = 10;
			mob->Damage = 0;
			mob->Speed = 0.040f;
			mob->AttackRange = 0.0f;
		} else if (kind == MOB_KIND_SHEEP) {
			model = String_FromReadonly("sheep");
			mob->Health = 8;
			mob->Damage = 0;
			mob->Speed = 0.038f;
			mob->AttackRange = 0.0f;
		} else {
			model = String_FromReadonly("zombie");
		}

		Entity_SetModel(e, &model);
		Entity_UpdateModelBounds(e);
		e->Yaw = MOB_YAW_OFFSET;
		e->RotY = MOB_YAW_OFFSET;

		mob->Collisions.Entity = e;
		/* Mobs should visibly jump ledges instead of silently step-sliding up them. */
		mob->Collisions.StepSize = 0.0f;

		Entities.List[raw] = e;
		Event_RaiseInt(&EntityEvents.Added, raw);

		/* If this host spawns a mob while LAN is open, push the spawn immediately.
		 * Movement snapshots still continue from Mob_Tick afterwards. */
		if (Server_IsLANHosted()) {
			Server_SendMobSnapshot((EntityID)raw, (cc_uint8)kind, e->Position, e->Velocity, e->Yaw, e->Pitch, mob->Health);
		}
		return true;
	}
	return false;
}

cc_bool Mob_SpawnZombie(Vec3 pos)   { return Mob_SpawnCore(pos, MOB_KIND_ZOMBIE); }
cc_bool Mob_SpawnSkeleton(Vec3 pos) { return Mob_SpawnCore(pos, MOB_KIND_SKELETON); }
cc_bool Mob_SpawnCreeper(Vec3 pos)  { return Mob_SpawnCore(pos, MOB_KIND_CREEPER); }
cc_bool Mob_SpawnSpider(Vec3 pos)   { return Mob_SpawnCore(pos, MOB_KIND_SPIDER); }
cc_bool Mob_SpawnPig(Vec3 pos)      { return Mob_SpawnCore(pos, MOB_KIND_PIG); }
cc_bool Mob_SpawnSheep(Vec3 pos)    { return Mob_SpawnCore(pos, MOB_KIND_SHEEP); }


/* Called by Server.c when a LAN client receives a host-authoritative mob snapshot. */
void Mob_SetSyncedState(EntityID id, cc_uint8 kind, Vec3 pos, Vec3 vel, float yaw, float pitch, int health) {
	struct Mob* mob;
	struct Entity* e;
	cc_string model;
	int slot;

	if (id < MOBS_FIRST || id >= DROPPED_ITEMS_FIRST) return;
	slot = id - MOBS_FIRST;
	if (slot < 0 || slot >= MAX_MOBS) return;

	mob = &Mobs[slot];
	e = &mob->Base;

	/* Create the mob locally if this is the first snapshot for it. */
	if (Entities.List[id] != e) {
		if (Entities.List[id]) Entities_Remove(id);

		Mem_Set(mob, 0, sizeof(struct Mob));
		Entity_Init(e);
		e->VTABLE = &mob_VTABLE;
		e->Flags |= ENTITY_FLAG_HAS_MODELVB;
		e->ShouldRender = true;

		mob->EntityID = id;
		mob->Kind = kind;
		mob->Health = health;
		mob->Damage = 2;
		mob->Speed = 0.065f;
		mob->AttackRange = 1.20f;
		mob->WanderCooldown = 0.0f;
		mob->StuckTimer = 0.0f;
		mob->JumpCooldown = 0.0f;
		Vec3_Set(mob->WanderDir, 0.0f, 0.0f, 1.0f);

		if (kind == MOB_KIND_SKELETON) {
			model = String_FromReadonly("skeleton");
			mob->Speed = 0.060f;
			mob->AttackRange = 1.35f;
		} else if (kind == MOB_KIND_CREEPER) {
			model = String_FromReadonly("creeper");
			mob->Speed = 0.060f;
			mob->Damage = 6;
			mob->AttackRange = 1.60f;
		} else if (kind == MOB_KIND_SPIDER) {
			model = String_FromReadonly("spider");
			mob->Health = 16;
			mob->Speed = 0.085f;
			mob->AttackRange = 1.30f;
		} else if (kind == MOB_KIND_PIG) {
			model = String_FromReadonly("pig");
			mob->Health = 10;
			mob->Damage = 0;
			mob->Speed = 0.040f;
			mob->AttackRange = 0.0f;
		} else if (kind == MOB_KIND_SHEEP) {
			model = String_FromReadonly("sheep");
			mob->Health = 8;
			mob->Damage = 0;
			mob->Speed = 0.038f;
			mob->AttackRange = 0.0f;
		} else {
			model = String_FromReadonly("zombie");
		}

		Entity_SetModel(e, &model);
		Entity_UpdateModelBounds(e);
		mob->Collisions.Entity = e;
		mob->Collisions.StepSize = 0.0f;

		Entities.List[id] = e;
		Event_RaiseInt(&EntityEvents.Added, id);
	}

	mob->Kind   = kind;
	mob->Health = health;
	e->Position = pos;
	e->Velocity = vel;
	e->Yaw      = yaw;
	e->RotY     = yaw;
	e->Pitch    = pitch;
}

/* Called by Server.c when the LAN host says a mob was removed/dead. */
void Mob_RemoveSynced(EntityID id) {
	if (id < MOBS_FIRST || id >= DROPPED_ITEMS_FIRST) return;
	if (!Entities.List[id]) return;
	Entities_Remove(id);
}

/* Called by Server.c after a LAN client finishes joining, so late joiners see current mobs. */
void Mob_SendAllSnapshots(void) {
	int i;
	for (i = 0; i < MAX_MOBS; i++) {
		struct Mob* mob = &Mobs[i];
		struct Entity* e = &mob->Base;
		int id = MOBS_FIRST + i;
		if (mob->EntityID != id) continue;
		if (Entities.List[id] != e) continue;
		Server_SendMobSnapshot((EntityID)id, (cc_uint8)mob->Kind, e->Position, e->Velocity, e->Yaw, e->Pitch, mob->Health);
	}
}

cc_bool Mob_AttackClosest(float reach, int damage) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Entity* e;
	Vec3 eyePos, dir;
	float t0, t1, bestDist;
	int i, best = -1;

	if (!Game_SurvivalMode || !p) return false;
	eyePos = Entity_GetEyePosition(&p->Base);
	dir = Vec3_GetDirVector(p->Base.Yaw * MATH_DEG2RAD, p->Base.Pitch * MATH_DEG2RAD);
	bestDist = reach;

	for (i = MOBS_FIRST; i < DROPPED_ITEMS_FIRST; i++) {
		e = Entities.List[i];
		if (!e) continue;
		if (!Intersection_RayIntersectsRotatedBox(eyePos, dir, e, &t0, &t1)) continue;
		if (t0 < 0.0f) t0 = t1;
		if (t0 < 0.0f || t0 > bestDist) continue;
		bestDist = t0;
		best = i;
	}
	if (best < 0) return false;

	/* Make the first-person hand/held block swing when hitting a mob. */
	HeldBlockRenderer_ClickAnim(false);

	{
		struct Mob* mob = (struct Mob*)Entities.List[best];

		Mob_Damage(mob, damage);
		if (Entities.List[best] == &mob->Base) {
			Mob_ApplyKnockback(mob, &p->Base.Position, 0.36f, 0.18f);
		}
	}
	return true;
}

void Mob_ClearAll(void) {
	int i;
	for (i = MOBS_FIRST; i < DROPPED_ITEMS_FIRST; i++) Entities_Remove(i);
}

static void Mob_TryAutoSpawn(float delta) {
	struct LocalPlayer* player = Entities.CurPlayer;
	Vec3 pos;
	float angle, radius;
	int kind;
	cc_bool night;

	SurvivalDayNight_Tick(delta);

	if (!Game_SurvivalMode || !World.Loaded || !player) {
		mob_spawnTimer = 0.0f;
		return;
	}
	if (Mob_CountAlive() >= MOB_MAX_ALIVE) return;

	Mob_SeedRandom();
	mob_spawnTimer += delta;
	if (mob_spawnTimer < MOB_SPAWN_INTERVAL) return;
	mob_spawnTimer = 0.0f;

	night = Survival_IsNight();
	if (night && Mob_CountKindGroup(true) >= MOB_MAX_HOSTILE) return;
	if (!night && Mob_CountKindGroup(false) >= MOB_MAX_PASSIVE) return;

	angle  = Random_Float(&mob_rnd) * (float)(MATH_PI * 2.0);
	radius = 12.0f + Random_Float(&mob_rnd) * 10.0f;
	pos = player->Base.Position;
	pos.x += Math_CosF(angle) * radius;
	pos.z += Math_SinF(angle) * radius;
	pos.y += 1.0f;

	if (night) {
		kind = (int)(Random_Float(&mob_rnd) * 4.0f);
		if (kind == MOB_KIND_SKELETON) Mob_SpawnSkeleton(pos);
		else if (kind == MOB_KIND_CREEPER) Mob_SpawnCreeper(pos);
		else if (kind == MOB_KIND_SPIDER) Mob_SpawnSpider(pos);
		else Mob_SpawnZombie(pos);
	} else {
		kind = (int)(Random_Float(&mob_rnd) * 2.0f);
		if (kind == 0) Mob_SpawnPig(pos);
		else Mob_SpawnSheep(pos);
	}
}

static cc_bool Entities_Tick(struct ScheduledTask2* task) {
	int i;

	Mob_TryAutoSpawn(task->interval);
	for (i = 0; i < ENTITIES_MAX_COUNT; i++)
	{
		if (!Entities.List[i]) continue;
		Entities.List[i]->VTABLE->Tick(Entities.List[i], task->interval);
	}
	return true;
}

void Entities_RenderModels(float delta, float t) {
	int i;
	Gfx_SetAlphaTest(true);
	
	for (i = 0; i < ENTITIES_MAX_COUNT; i++)
	{
		if (!Entities.List[i]) continue;
		Entities.List[i]->VTABLE->RenderModel(Entities.List[i], delta, t);
	}
	Gfx_SetAlphaTest(false);
}

static void Entities_ContextLost(void* obj) {
	struct Entity* entity;
	int i;

	for (i = 0; i < ENTITIES_MAX_COUNT; i++)
	{
		entity = Entities.List[i];
		if (!entity) continue;

		if (entity->Flags & ENTITY_FLAG_HAS_MODELVB)
			Gfx_DeleteDynamicVb(&entity->ModelVB);

		if (!Gfx.ManagedTextures)
			DeleteSkin(entity);
	}
}
/* No OnContextCreated, skin textures remade when needed */

void Entities_Remove(int id) {
	struct Entity* e = Entities.List[id];
	if (!e) return;

	Event_RaiseInt(&EntityEvents.Removed, id);
	e->VTABLE->Despawn(e);
	Entities.List[id] = NULL;

	/* TODO: Move to EntityEvents.Removed callback instead */
	if (id < TABLIST_MAX_NAMES && TabList_EntityLinked_Get(id)) {
		TabList_Remove(id);
		TabList_EntityLinked_Reset(id);
	}
}

int Entities_GetClosest(struct Entity* src) {
	Vec3 eyePos = Entity_GetEyePosition(src);
	Vec3 dir    = Vec3_GetDirVector(src->Yaw * MATH_DEG2RAD, src->Pitch * MATH_DEG2RAD);
	float closestDist = -200; /* NOTE: was previously positive infinity */
	int targetID = -1;

	float t0, t1;
	int i;

	for (i = 0; i < ENTITIES_MAX_COUNT; i++) /* because we don't want to pick against local player */
	{
		struct Entity* e = Entities.List[i];
		if (!e || e == &Entities.CurPlayer->Base) continue;
		if (!Intersection_RayIntersectsRotatedBox(eyePos, dir, e, &t0, &t1)) continue;

		if (targetID < 0 || t0 < closestDist) {
			closestDist = t0;
			targetID    = i;
		}
	}
	return targetID;
}

static void Player_Despawn(struct Entity* e) {
	DeleteSkin(e);
	EntityNames_Delete(e);

	if (e->Flags & ENTITY_FLAG_HAS_MODELVB)
		Gfx_DeleteDynamicVb(&e->ModelVB);
}


/*########################################################################################################################*
*--------------------------------------------------------TabList----------------------------------------------------------*
*#########################################################################################################################*/
struct _TabListData TabList;

/* Removes the names from the names buffer for the given id. */
static void TabList_Delete(EntityID id) {
	int i, index;
	index = TabList.NameOffsets[id];
	if (!index) return;

	StringsBuffer_Remove(&TabList._buffer, index - 1);
	StringsBuffer_Remove(&TabList._buffer, index - 2);
	StringsBuffer_Remove(&TabList._buffer, index - 3);

	/* Indices after this entry need to be shifted down */
	for (i = 0; i < TABLIST_MAX_NAMES; i++) {
		if (TabList.NameOffsets[i] > index) TabList.NameOffsets[i] -= 3;
	}
}

void TabList_Remove(EntityID id) {
	TabList_Delete(id);
	TabList.NameOffsets[id] = 0;
	TabList.GroupRanks[id]  = 0;
	Event_RaiseInt(&TabListEvents.Removed, id);
}

void TabList_Set(EntityID id, const cc_string* player_, const cc_string* list, const cc_string* group, cc_uint8 rank) {
	cc_string oldPlayer, oldList, oldGroup;
	cc_uint8 oldRank;
	struct Event_Int* events;

	/* Player name shouldn't have colour codes */
	/*  (intended for e.g. tab autocomplete) */
	cc_string player; char playerBuffer[STRING_SIZE];
	String_InitArray(player, playerBuffer);
	String_AppendColorless(&player, player_);
	
	if (TabList.NameOffsets[id]) {
		oldPlayer = TabList_UNSAFE_GetPlayer(id);
		oldList   = TabList_UNSAFE_GetList(id);
		oldGroup  = TabList_UNSAFE_GetGroup(id);
		oldRank   = TabList.GroupRanks[id];

		/* Don't redraw the tab list if nothing changed */
		if (String_Equals(&player, &oldPlayer) && String_Equals(list, &oldList)
			&& String_Equals(group, &oldGroup) && rank == oldRank) return;

		events = &TabListEvents.Changed;
	} else {
		events = &TabListEvents.Added;
	}
	TabList_Delete(id);

	StringsBuffer_Add(&TabList._buffer, &player);
	StringsBuffer_Add(&TabList._buffer, list);
	StringsBuffer_Add(&TabList._buffer, group);

	TabList.NameOffsets[id] = TabList._buffer.count;
	TabList.GroupRanks[id]  = rank;
	Event_RaiseInt(events, id);
}

static void Tablist_Init(void) {
	TabList_Set(ENTITIES_SELF_ID, &Game_Username, &Game_Username, &String_Empty, 0);
}

static void TabList_Clear(void) {
	Mem_Set(TabList.NameOffsets, 0, sizeof(TabList.NameOffsets));
	Mem_Set(TabList.GroupRanks,  0, sizeof(TabList.GroupRanks));
	StringsBuffer_Clear(&TabList._buffer);
}

struct IGameComponent TabList_Component = {
	Tablist_Init,  /* Init  */
	TabList_Clear, /* Free  */
	TabList_Clear  /* Reset */
};


/*########################################################################################################################*
*------------------------------------------------------LocalPlayer--------------------------------------------------------*
*#########################################################################################################################*/
struct LocalPlayer LocalPlayer_Instances[MAX_LOCAL_PLAYERS];
static cc_bool hackPermMsgs;
static cc_bool survivalSprintFovActive;
static float survivalSprintFovCurrent;
static struct LocalPlayerInput* sources_head;
static struct LocalPlayerInput* sources_tail;

void LocalPlayerInput_Add(struct LocalPlayerInput* source) {
	LinkedList_Append(source, sources_head, sources_tail);
}

void LocalPlayerInput_Remove(struct LocalPlayerInput* source) {
	struct LocalPlayerInput* cur;
	LinkedList_Remove(source, cur, sources_head, sources_tail);
}

float LocalPlayer_JumpHeight(struct LocalPlayer* p) {
	return (float)PhysicsComp_CalcMaxHeight(p->Physics.JumpVel);
}

void LocalPlayer_SetInterpPosition(struct LocalPlayer* p, float t) {
	if (!(p->Hacks.WOMStyleHacks && p->Hacks.Noclip)) {
		Vec3_Lerp(&p->Base.Position, &p->Base.prev.pos, &p->Base.next.pos, t);
	}
	Entity_LerpAngles(&p->Base, t);
}

static void LocalPlayer_HandleInput(struct LocalPlayer* p, float* xMoving, float* zMoving) {
	struct HacksComp* hacks = &p->Hacks;
	struct LocalPlayerInput* input;
	if (Gui.InputGrab) return;

	/* keyboard input, touch, joystick, etc */
	for (input = sources_head; input; input = input->next) {
		input->GetMovement(p, xMoving, zMoving);
	}
	*xMoving *= 0.98f;
	*zMoving *= 0.98f;

	if (hacks->WOMStyleHacks && hacks->Enabled && hacks->CanNoclip) {
		if (hacks->Noclip) {
			/* need a { } block because it's a macro */
			Vec3_Set(p->Base.Velocity, 0,0,0);
		}
		HacksComp_SetNoclip(hacks, hacks->_noclipping);
	}
}

static void LocalPlayer_SetLocation(struct Entity* e, struct LocationUpdate* update) {
	struct LocalPlayer* p = (struct LocalPlayer*)e;
	LocalInterpComp_SetLocation(&p->Interp, update, e);
}

static void LocalPlayer_DoRespawn(struct LocalPlayer* p);

static void LocalPlayer_Heal(struct LocalPlayer* p) {
	p->Health = 20;
	p->Stamina = 100.0f;
	p->Oxygen = 20.0f;
	p->Underwater = false;
	p->SurvivalOxygenCooldown = 0.0f;
	p->HurtTilt = 0.0f;
	p->HurtTiltDir = 1.0f;
	p->Sprinting = false;
	p->SprintHeld = false;
	p->SprintKeyDown = false;
	p->SprintExhausted = false;
	p->Sneaking = false;
	p->SurvivalDamageCooldown = 0.0f;
	p->SurvivalLavaCooldown   = 0.0f;
}

static void LocalPlayer_ApplyDamage(struct LocalPlayer* p, int damage, cc_bool useIFrames) {
	if (!Game_SurvivalMode || damage <= 0) return;
	if (p->Health <= 0) return;
	if (useIFrames && p->SurvivalDamageCooldown > 0.0f) return;

	p->Health -= damage;
	Audio_PlayOuchSound();
	if (p->Health < 0) p->Health = 0;

	if (useIFrames) p->SurvivalDamageCooldown = 0.5f;

	p->HurtTilt = 8.0f;
	p->HurtTiltDir = p->HurtTiltDir >= 0.0f ? -1.0f : 1.0f;

	if (p->Health > 0) return;

	Chat_AddRaw("&cYou died");
	LocalPlayer_DoRespawn(p);
	LocalPlayer_Heal(p);
}

static void LocalPlayer_Damage(struct LocalPlayer* p, int damage) {
	LocalPlayer_ApplyDamage(p, damage, true);
}

static void LocalPlayer_UpdateHurtTilt(struct LocalPlayer* p, float delta) {
	if (p->HurtTilt <= 0.0f) return;
	p->HurtTilt -= delta * 28.0f;
	if (p->HurtTilt < 0.0f) p->HurtTilt = 0.0f;
}

static void LocalPlayer_UpdateSprintFov(cc_bool sprinting, float delta) {
	int sprintFov = min(Camera.DefaultFov + 12, 110);
	float target = sprinting ? (float)sprintFov : (float)Camera.DefaultFov;
	float t = delta * 10.0f;
	int fov;

	if (t > 1.0f) t = 1.0f;

	if (sprinting) {
		if (!survivalSprintFovActive) {
			survivalSprintFovActive = true;
			survivalSprintFovCurrent = (float)Camera.Fov;
		}
	} else if (!survivalSprintFovActive) {
		return;
	}

	survivalSprintFovCurrent += (target - survivalSprintFovCurrent) * t;
	fov = (int)(survivalSprintFovCurrent + 0.5f);
	Camera_SetFov(fov);

	if (!sprinting && Math_AbsF(survivalSprintFovCurrent - target) < 0.5f) {
		Camera_SetFov(Camera.DefaultFov);
		survivalSprintFovCurrent = (float)Camera.DefaultFov;
		survivalSprintFovActive = false;
	}
}

static void LocalPlayer_UpdateSprint(struct LocalPlayer* p, float delta, float xMoving, float zMoving) {
	struct Entity* e = &p->Base;
	cc_bool movingForward;
	cc_bool canSprint;

	if (!Game_SurvivalMode || !World.Loaded) {
		p->Sprinting = false;
		p->SprintExhausted = false;
		p->Hacks.BaseHorSpeed = 1.0f;
		LocalPlayer_UpdateSprintFov(false, delta);
		return;
	}

	movingForward = zMoving < 0.0f;
	canSprint     = e->OnGround || p->Sprinting;
	p->SprintExhausted = false;
	p->Stamina = 100.0f;
	p->Sprinting = p->SprintHeld && !p->Sneaking && movingForward && canSprint;
	p->Hacks.BaseHorSpeed = p->Sprinting ? 1.45f : (p->Sneaking ? 0.35f : 1.0f);
	LocalPlayer_UpdateSprintFov(p->Sprinting, delta);
}


static cc_bool LocalPlayer_IsHeadUnderwater(struct LocalPlayer* p) {
	Vec3 eyePos = Entity_GetEyePosition(&p->Base);
	IVec3 pos;
	BlockID block;

	IVec3_Floor(&pos, &eyePos);
	block = World_SafeGetBlock(pos.x, pos.y, pos.z);
	return Blocks.ExtendedCollide[block] == COLLIDE_WATER;
}

static void LocalPlayer_UpdateOxygen(struct LocalPlayer* p, float delta) {
	if (!Game_SurvivalMode || !World.Loaded) {
		p->Underwater = false;
		p->Oxygen = 20.0f;
		p->SurvivalOxygenCooldown = 0.0f;
		return;
	}

	p->Underwater = LocalPlayer_IsHeadUnderwater(p);

	if (p->SurvivalOxygenCooldown > 0.0f) {
		p->SurvivalOxygenCooldown -= delta;
		if (p->SurvivalOxygenCooldown < 0.0f) p->SurvivalOxygenCooldown = 0.0f;
	}

	if (p->Underwater) {
		p->Oxygen -= delta;
		if (p->Oxygen < 0.0f) p->Oxygen = 0.0f;

		if (p->Oxygen <= 0.0f && p->SurvivalOxygenCooldown <= 0.0f) {
			p->SurvivalOxygenCooldown = 1.0f;
			LocalPlayer_ApplyDamage(p, 2, false);
		}
	} else {
		p->Oxygen += delta * 4.0f;
		if (p->Oxygen >= 20.0f) p->SurvivalOxygenCooldown = 0.0f;
	}
}

static void LocalPlayer_UpdateSurvival(struct LocalPlayer* p, float delta, cc_bool wasOnGround) {
	struct Entity* e = &p->Base;
	float fallSpeed;
	int fallDamage;

	if (!Game_SurvivalMode || !World.Loaded) return;

	if (p->SurvivalDamageCooldown > 0.0f) {
		p->SurvivalDamageCooldown -= delta;
		if (p->SurvivalDamageCooldown < 0.0f) p->SurvivalDamageCooldown = 0.0f;
	}

	if (p->SurvivalLavaCooldown > 0.0f) {
		p->SurvivalLavaCooldown -= delta;
		if (p->SurvivalLavaCooldown < 0.0f) p->SurvivalLavaCooldown = 0.0f;
	}

	LocalPlayer_UpdateOxygen(p, delta);

	if (Entity_TouchesAnyLava(e) && p->SurvivalLavaCooldown <= 0.0f) {
		p->SurvivalLavaCooldown = 0.75f;
		LocalPlayer_ApplyDamage(p, 4, false);
	}

	if ((p->Hacks.Noclip && p->Hacks.Enabled) || p->Hacks.Flying) return;

	if (!wasOnGround && e->OnGround && p->OldVelocity.y < -0.65f) {
		fallSpeed = -p->OldVelocity.y;
		fallDamage = (int)((fallSpeed - 0.65f) * 10.0f);

		if (fallDamage > 0) {
			LocalPlayer_Damage(p, fallDamage);
		}
	}
}

static void LocalPlayer_Tick(struct Entity* e, float delta) {
	struct LocalPlayer* p = (struct LocalPlayer*)e;
	struct HacksComp* hacks = &p->Hacks;
	float xMoving = 0, zMoving = 0;
	cc_bool wasOnGround;
	Vec3 headingVelocity;

	if (!World.Loaded) return;
	p->Collisions.StepSize = hacks->FullBlockStep && hacks->Enabled && hacks->CanSpeed ? 1.0f : 0.5f;
	p->OldVelocity = e->Velocity;
	wasOnGround    = e->OnGround;

	LocalInterpComp_AdvanceState(&p->Interp, e);
	LocalPlayer_HandleInput(p, &xMoving, &zMoving);
	LocalPlayer_UpdateSprint(p, delta, xMoving, zMoving);
	hacks->Floating = hacks->Noclip || hacks->Flying;
	if (!hacks->Floating && hacks->CanBePushed) PhysicsComp_DoEntityPush(e);

	/* Immediate stop in noclip mode */
	if (!hacks->NoclipSlide && (hacks->Noclip && xMoving == 0 && zMoving == 0)) {
		Vec3_Set(e->Velocity, 0,0,0);
	}

	PhysicsComp_UpdateVelocityState(&p->Physics);
	headingVelocity = Vec3_RotateY3(xMoving, 0, zMoving, e->Yaw * MATH_DEG2RAD);
	PhysicsComp_PhysicsTick(&p->Physics, headingVelocity);

	/* Fixes high jump, when holding down a movement key, jump, fly, then let go of fly key */
	if (p->Hacks.Floating) e->Velocity.y = 0.0f;

	e->next.pos = e->Position; e->Position = e->prev.pos;
	AnimatedComp_Update(e, e->prev.pos, e->next.pos, delta);
	TiltComp_Update(p, &p->Tilt, delta);
	LocalPlayer_UpdateHurtTilt(p, delta);
	LocalPlayer_UpdateSurvival(p, delta, wasOnGround);

	Entity_CheckSkin(&p->Base);
	SoundComp_Tick(p, wasOnGround);
}

static void LocalPlayer_RenderModel(struct Entity* e, float delta, float t) {
	struct LocalPlayer* p = (struct LocalPlayer*)e;
	AnimatedComp_GetCurrent(e, t);

	if (!Camera.Active->isThirdPerson && p == Entities.CurPlayer) return;
	Model_Render(e->Model, e);
}

static cc_bool LocalPlayer_ShouldRenderName(struct Entity* e) {
	return Camera.Active->isThirdPerson;
}

static void LocalPlayer_CheckJumpVelocity(void* obj) {
	struct LocalPlayer* p = (struct LocalPlayer*)obj;
	if (!HacksComp_CanJumpHigher(&p->Hacks)) {
		p->Physics.JumpVel = p->Physics.ServerJumpVel;
	}
}

static const struct EntityVTABLE localPlayer_VTABLE = {
	LocalPlayer_Tick,        Player_Despawn,         LocalPlayer_SetLocation, Entity_GetColor,
	LocalPlayer_RenderModel, LocalPlayer_ShouldRenderName
};
static void LocalPlayer_Init(struct LocalPlayer* p, int index) {
	struct HacksComp* hacks = &p->Hacks;

	Entity_Init(&p->Base);
	Entity_SetName(&p->Base, &Game_Username);
	Entity_SetSkin(&p->Base, &Game_Username);
	Event_Register_(&UserEvents.HackPermsChanged, p, LocalPlayer_CheckJumpVelocity);

	p->Collisions.Entity = &p->Base;
	HacksComp_Init(hacks);
	PhysicsComp_Init(&p->Physics, &p->Base);
	TiltComp_Init(&p->Tilt);

	p->Base.Flags |= ENTITY_FLAG_MODEL_RESTRICTED_SCALE;
	p->ReachDistance = 5.0f;
	p->Physics.Hacks = &p->Hacks;
	p->Physics.Collisions = &p->Collisions;
	p->Base.VTABLE   = &localPlayer_VTABLE;
	p->index = index;
	LocalPlayer_Heal(p);

	hacks->Enabled = !Game_PureClassic && Options_GetBool(OPT_HACKS_ENABLED, true);
	if (Game_ClassicMode) return;

	hacks->SpeedMultiplier = Options_GetFloat(OPT_SPEED_FACTOR,  0.1f, 50.0f, 10.0f);
	hacks->PushbackPlacing = Options_GetBool(OPT_PUSHBACK_PLACING, false);
	hacks->NoclipSlide     = Options_GetBool(OPT_NOCLIP_SLIDE,     false);
	hacks->WOMStyleHacks   = Options_GetBool(OPT_WOM_STYLE_HACKS,  false);
	hacks->FullBlockStep   = Options_GetBool(OPT_FULL_BLOCK_STEP,  false);
	p->Physics.UserJumpVel = Options_GetFloat(OPT_JUMP_VELOCITY, 0.0f, 52.0f, 0.42f);
	p->Physics.JumpVel     = p->Physics.UserJumpVel;
	hackPermMsgs           = Options_GetBool(OPT_HACK_PERM_MSGS, true);
}

void LocalPlayer_ResetJumpVelocity(struct LocalPlayer* p) {
	cc_bool higher = HacksComp_CanJumpHigher(&p->Hacks);

	p->Physics.JumpVel       = higher ? p->Physics.UserJumpVel : 0.42f;
	p->Physics.ServerJumpVel = p->Physics.JumpVel;
}

static void LocalPlayer_Reset(struct LocalPlayer* p) {
	p->ReachDistance = 5.0f;
	p->Stamina = 100.0f;
	p->Oxygen = 20.0f;
	p->Underwater = false;
	p->SurvivalOxygenCooldown = 0.0f;
	p->HurtTilt = 0.0f;
	p->HurtTiltDir = 1.0f;
	p->Sprinting = false;
	p->SprintHeld = false;
	p->SprintKeyDown = false;
	p->SprintExhausted = false;
	p->Sneaking = false;
	Vec3_Set(p->Base.Velocity, 0,0,0);
	LocalPlayer_ResetJumpVelocity(p);
}

static void LocalPlayers_Reset(void) {
	int i;
	for (i = 0; i < Game_NumStates; i++)
	{
		LocalPlayer_Reset(&LocalPlayer_Instances[i]);
	}
}

static void LocalPlayer_OnNewMap(struct LocalPlayer* p) {
	Vec3_Set(p->Base.Velocity, 0,0,0);
	Vec3_Set(p->OldVelocity,   0,0,0);

	p->_warnedRespawn = false;
	p->_warnedFly     = false;
	p->_warnedNoclip  = false;
	p->_warnedZoom    = false;
	LocalPlayer_Heal(p);
}

static void LocalPlayers_OnNewMap(void) {
	int i;
	for (i = 0; i < Game_NumStates; i++)
	{
		LocalPlayer_OnNewMap(&LocalPlayer_Instances[i]);
	}
	Mob_ClearAll();
	DroppedItem_ClearAll();
}

static cc_bool LocalPlayer_IsSolidCollide(BlockID b) { return Blocks.Collide[b] == COLLIDE_SOLID; }

static void LocalPlayer_DoRespawn(struct LocalPlayer* p) {
	struct EntityLocation* prev;
	struct LocationUpdate update;
	struct AABB bb;
	Vec3 spawn = p->Spawn;
	IVec3 pos;
	BlockID block;
	float height, spawnY;
	int y;

	if (!World.Loaded) return;
	IVec3_Floor(&pos, &spawn);	

	/* Spawn player at highest solid position to match vanilla Minecraft classic */
	/* Only when player can noclip, since this can let you 'clip' to above solid blocks */
	if (p->Hacks.CanNoclip) {
		AABB_Make(&bb, &spawn, &p->Base.Size);
		for (y = pos.y; y <= World.Height; y++) {
			spawnY = Respawn_HighestSolidY(&bb);

			if (spawnY == RESPAWN_NOT_FOUND) {
				block   = World_SafeGetBlock(pos.x, y, pos.z);
				height  = Blocks.Collide[block] == COLLIDE_SOLID ? Blocks.MaxBB[block].y : 0.0f;
				spawn.y = y + height + ENTITY_ADJUSTMENT;
				break;
			}
			bb.Min.y += 1.0f; bb.Max.y += 1.0f;
		}
	}

	prev = &p->Base.prev;
	CPE_SendNotifyPositionAction(3, prev->pos.x, prev->pos.y, prev->pos.z);

	/* Adjust the position to be slightly above the ground, so that */
	/*  it's obvious to the player that they are being respawned */
	spawn.y += 2.0f/16.0f;

	update.flags = LU_HAS_POS | LU_HAS_YAW | LU_HAS_PITCH;
	update.pos   = spawn;
	update.yaw   = p->SpawnYaw;
	update.pitch = p->SpawnPitch;
	p->Base.VTABLE->SetLocation(&p->Base, &update);

	Vec3_Set(p->Base.Velocity, 0,0,0);
	/* Update onGround, otherwise if 'respawn' then 'space' is pressed, you still jump into the air if onGround was true before */
	Entity_GetBounds(&p->Base, &bb);
	bb.Min.y -= 0.01f; bb.Max.y = bb.Min.y;
	p->Base.OnGround = Entity_TouchesAny(&bb, LocalPlayer_IsSolidCollide);
	LocalPlayer_Heal(p);
}

static cc_bool LocalPlayer_HandleRespawn(int key, struct InputDevice* device) {
	struct LocalPlayer* p = &LocalPlayer_Instances[device->mappedIndex];
	if (Gui.InputGrab) return false;
	
	if (p->Hacks.CanRespawn) {
		LocalPlayer_DoRespawn(p);
		return true;
	} else if (!p->_warnedRespawn) {
		p->_warnedRespawn = true;
		if (hackPermMsgs) Chat_AddRaw("&cRespawning is currently disabled");
	}
	return false;
}

static cc_bool LocalPlayer_HandleSetSpawn(int key, struct InputDevice* device) {
	struct LocalPlayer* p = &LocalPlayer_Instances[device->mappedIndex];
	if (Gui.InputGrab) return false;
	
	if (p->Hacks.CanRespawn) {

		if (!p->Hacks.CanNoclip && !p->Base.OnGround) {
			Chat_AddRaw("&cCannot set spawn midair when noclip is disabled");
			return false;
		}

		/* Spawn is normally centered to match vanilla Minecraft classic */
		if (!p->Hacks.CanNoclip) {
			/* Don't want to use Position because it is interpolated between prev and next. */
			/* This means it can be halfway between stepping up a stair and clip through the floor. */
			p->Spawn   = p->Base.prev.pos;
		} else {
			p->Spawn.x = Math_Floor(p->Base.Position.x) + 0.5f;
			p->Spawn.y = p->Base.Position.y;
			p->Spawn.z = Math_Floor(p->Base.Position.z) + 0.5f;
		}
		
		p->SpawnYaw   = p->Base.Yaw;
		if (!Game_ClassicMode) p->SpawnPitch = p->Base.Pitch;

		CPE_SendNotifyPositionAction(4, p->Spawn.x, p->Spawn.y, p->Spawn.z);
	}
	return LocalPlayer_HandleRespawn(key, device);
}

static cc_bool LocalPlayer_HandleFly(int key, struct InputDevice* device) {
	struct LocalPlayer* p = &LocalPlayer_Instances[device->mappedIndex];
	if (Gui.InputGrab) return false;

	if (p->Hacks.CanFly && p->Hacks.Enabled) {
		HacksComp_SetFlying(&p->Hacks, !p->Hacks.Flying);
		return true;
	} else if (!p->_warnedFly) {
		p->_warnedFly = true;
		if (hackPermMsgs) Chat_AddRaw("&cFlying is currently disabled");
	}
	return false;
}

static cc_bool LocalPlayer_HandleNoclip(int key, struct InputDevice* device) {
	struct LocalPlayer* p = &LocalPlayer_Instances[device->mappedIndex];
	p->Hacks._noclipping = true;
	if (Gui.InputGrab) return false;

	if (p->Hacks.CanNoclip && p->Hacks.Enabled) {
		if (p->Hacks.WOMStyleHacks) return true; /* don't handle this here */
		if (p->Hacks.Noclip) p->Base.Velocity.y = 0;

		HacksComp_SetNoclip(&p->Hacks, !p->Hacks.Noclip);
		return true;
	} else if (!p->_warnedNoclip) {
		p->_warnedNoclip = true;
		if (hackPermMsgs) Chat_AddRaw("&cNoclip is currently disabled");
	}
	return false;
}

static cc_bool LocalPlayer_HandleJump(int key, struct InputDevice* device) {
	struct LocalPlayer* p = &LocalPlayer_Instances[device->mappedIndex];
	struct HacksComp* hacks     = &p->Hacks;
	struct PhysicsComp* physics = &p->Physics;
	int maxJumps;
	if (Gui.InputGrab) return false;
	physics->Jumping = true;

	if (!p->Base.OnGround && !(hacks->Flying || hacks->Noclip)) {
		maxJumps = hacks->CanDoubleJump && hacks->WOMStyleHacks ? 2 : 0;
		maxJumps = max(maxJumps, hacks->MaxJumps - 1);

		if (physics->MultiJumps < maxJumps) {
			PhysicsComp_DoNormalJump(physics);
			physics->MultiJumps++;
		}
		return true;
	}
	return false;
}


static cc_bool LocalPlayer_TriggerHalfSpeed(int key, struct InputDevice* device) {
	struct LocalPlayer* p = &LocalPlayer_Instances[device->mappedIndex];
	struct HacksComp* hacks = &p->Hacks;
	cc_bool touch = device->type == INPUT_DEVICE_TOUCH;
	if (Gui.InputGrab) return false;

	if (Game_SurvivalMode) {
		p->Sneaking = true;
		p->Sprinting = false;
		return true;
	}
	hacks->HalfSpeeding = (!touch || !hacks->HalfSpeeding) && hacks->Enabled;
	return true;
}

static cc_bool LocalPlayer_TriggerSpeed(int key, struct InputDevice* device) {
	struct LocalPlayer* p = &LocalPlayer_Instances[device->mappedIndex];
	struct HacksComp* hacks = &p->Hacks;
	cc_bool touch = device->type == INPUT_DEVICE_TOUCH;
	if (Gui.InputGrab) return false;

	if (Game_SurvivalMode) {
		p->SprintHeld = true;
		p->SprintKeyDown = true;
		return true;
	}
	hacks->Speeding = (!touch || !hacks->Speeding) && hacks->Enabled;
	return true;
}

static void LocalPlayer_ReleaseHalfSpeed(int key, struct InputDevice* device) {
	struct LocalPlayer* p = &LocalPlayer_Instances[device->mappedIndex];
	struct HacksComp* hacks = &p->Hacks;
	if (Game_SurvivalMode) {
		p->Sneaking = false;
		return;
	}
	if (device->type != INPUT_DEVICE_TOUCH) hacks->HalfSpeeding = false;
}

static void LocalPlayer_ReleaseSpeed(int key, struct InputDevice* device) {
	struct LocalPlayer* p = &LocalPlayer_Instances[device->mappedIndex];
	struct HacksComp* hacks = &p->Hacks;
	if (Game_SurvivalMode) {
		p->SprintHeld = false;
		p->SprintKeyDown = false;
		return;
	}
	if (device->type != INPUT_DEVICE_TOUCH) hacks->Speeding = false;
}


static cc_bool LocalPlayer_TriggerFlyUp(int key, struct InputDevice* device) {
	struct HacksComp* hacks = &LocalPlayer_Instances[device->mappedIndex].Hacks;
	if (Gui.InputGrab) return false;
	
	hacks->FlyingUp = true;
	return hacks->CanFly && hacks->Enabled;
}

static cc_bool LocalPlayer_TriggerFlyDown(int key, struct InputDevice* device) {
	struct HacksComp* hacks = &LocalPlayer_Instances[device->mappedIndex].Hacks;
	if (Gui.InputGrab) return false;
	
	hacks->FlyingDown = true;
	return hacks->CanFly && hacks->Enabled;
}

static void LocalPlayer_ReleaseFlyUp(int key, struct InputDevice* device) {
	LocalPlayer_Instances[device->mappedIndex].Hacks.FlyingUp   = false;
}

static void LocalPlayer_ReleaseFlyDown(int key, struct InputDevice* device) {
	LocalPlayer_Instances[device->mappedIndex].Hacks.FlyingDown = false;
}

static void LocalPlayer_ReleaseJump(int key, struct InputDevice* device) {
	LocalPlayer_Instances[device->mappedIndex].Physics.Jumping = false;
}

static void LocalPlayer_ReleaseNoclip(int key, struct InputDevice* device) {
	LocalPlayer_Instances[device->mappedIndex].Hacks._noclipping = false;
}

static void LocalPlayer_HookBinds(void) {
	Bind_OnTriggered[BIND_FLY]       = LocalPlayer_HandleFly;
	Bind_OnTriggered[BIND_NOCLIP]    = LocalPlayer_HandleNoclip;
	Bind_OnTriggered[BIND_JUMP]      = LocalPlayer_HandleJump;

	Bind_OnTriggered[BIND_HALF_SPEED] = LocalPlayer_TriggerHalfSpeed;
	Bind_OnTriggered[BIND_SPEED]      = LocalPlayer_TriggerSpeed;
	Bind_OnReleased[BIND_HALF_SPEED]  = LocalPlayer_ReleaseHalfSpeed;
	Bind_OnReleased[BIND_SPEED]       = LocalPlayer_ReleaseSpeed;

	Bind_OnTriggered[BIND_FLY_UP]   = LocalPlayer_TriggerFlyUp;
	Bind_OnTriggered[BIND_FLY_DOWN] = LocalPlayer_TriggerFlyDown;
	Bind_OnReleased[BIND_FLY_UP]    = LocalPlayer_ReleaseFlyUp;
	Bind_OnReleased[BIND_FLY_DOWN]  = LocalPlayer_ReleaseFlyDown;

	Bind_OnReleased[BIND_JUMP]    = LocalPlayer_ReleaseJump;
	Bind_OnReleased[BIND_NOCLIP]  = LocalPlayer_ReleaseNoclip;
}

cc_bool LocalPlayer_CheckCanZoom(struct LocalPlayer* p) {
	if (p->Hacks.CanFly) return true;

	if (!p->_warnedZoom) {
		p->_warnedZoom = true;
		if (hackPermMsgs) Chat_AddRaw("&cCannot zoom camera out as flying is currently disabled");
	}
	return false;
}

void LocalPlayers_MoveToSpawn(struct LocationUpdate* update) {
	struct LocalPlayer* p;
	int i;
	
	for (i = 0; i < Game_NumStates; i++)
	{
		p = &LocalPlayer_Instances[i];
		p->Base.VTABLE->SetLocation(&p->Base, update);
		
		if (update->flags & LU_HAS_POS)   p->Spawn      = update->pos;
		if (update->flags & LU_HAS_YAW)   p->SpawnYaw   = update->yaw;
		if (update->flags & LU_HAS_PITCH) p->SpawnPitch = update->pitch;
	}
	
	/* TODO: This needs to be before new map... */
	Camera.CurrentPos = Camera.Active->GetPosition(0.0f);
}

void LocalPlayer_CalcDefaultSpawn(struct LocalPlayer* p, struct LocationUpdate* update) {
	float x = (World.Width  / 2) + 0.5f;
	float z = (World.Length / 2) + 0.5f;

	update->flags = LU_HAS_POS | LU_HAS_YAW | LU_HAS_PITCH;
	update->pos   = Respawn_FindSpawnPosition(x, z, p->Base.Size);
	update->yaw   = 0.0f;
	update->pitch = 0.0f;
}


/*########################################################################################################################*
*-------------------------------------------------------NetPlayer---------------------------------------------------------*
*#########################################################################################################################*/
struct NetPlayer NetPlayers_List[MAX_NET_PLAYERS];

static void NetPlayer_SetLocation(struct Entity* e, struct LocationUpdate* update) {
	struct NetPlayer* p = (struct NetPlayer*)e;
	NetInterpComp_SetLocation(&p->Interp, update, e);
}

static void NetPlayer_Tick(struct Entity* e, float delta) {
	struct NetPlayer* p = (struct NetPlayer*)e;
	NetInterpComp_AdvanceState(&p->Interp, e);

	Entity_CheckSkin(e);
	AnimatedComp_Update(e, e->prev.pos, e->next.pos, delta);
}

static void NetPlayer_RenderModel(struct Entity* e, float delta, float t) {
	Vec3_Lerp(&e->Position, &e->prev.pos, &e->next.pos, t);
	Entity_LerpAngles(e, t);

	AnimatedComp_GetCurrent(e, t);
	e->ShouldRender = Model_ShouldRender(e);
	/* Original classic only shows players up to 64 blocks away */
	if (Game_ClassicMode) e->ShouldRender &= Model_RenderDistance(e) <= 64 * 64;

	if (e->ShouldRender) Model_Render(e->Model, e);
}

static cc_bool NetPlayer_ShouldRenderName(struct Entity* e) {
	float distance;
	int threshold;
	if (!e->ShouldRender) return false;

	distance  = Model_RenderDistance(e);
	threshold = Entities.NamesMode == NAME_MODE_ALL_UNSCALED ? 8192 * 8192 : 32 * 32;
	return distance <= (float)threshold;
}

static const struct EntityVTABLE netPlayer_VTABLE = {
	NetPlayer_Tick,        Player_Despawn,       NetPlayer_SetLocation, Entity_GetColor,
	NetPlayer_RenderModel, NetPlayer_ShouldRenderName
};
void NetPlayer_Init(struct NetPlayer* p) {
	Mem_Set(p, 0, sizeof(struct NetPlayer));
	Entity_Init(&p->Base);
	p->Base.Flags |= ENTITY_FLAG_CLASSIC_ADJUST;
	p->Base.VTABLE = &netPlayer_VTABLE;
}


/*########################################################################################################################*
*---------------------------------------------------Entities component----------------------------------------------------*
*#########################################################################################################################*/
static void Entities_Init(void) {
	int i;
	Event_Register_(&GfxEvents.ContextLost, NULL, Entities_ContextLost);

	Entities.NamesMode = Options_GetEnum(OPT_NAMES_MODE, NAME_MODE_HOVERED,
		NameMode_Names, Array_Elems(NameMode_Names));
	if (Game_ClassicMode) Entities.NamesMode = NAME_MODE_HOVERED;

	Entities.ShadowsMode = Options_GetEnum(OPT_ENTITY_SHADOW, SHADOW_MODE_NONE,
		ShadowMode_Names, Array_Elems(ShadowMode_Names));
	if (Game_ClassicMode) Entities.ShadowsMode = SHADOW_MODE_NONE;

	for (i = 0; i < Game_NumStates; i++)
	{
		LocalPlayer_Init(&LocalPlayer_Instances[i], i);
		Entities.List[MAX_NET_PLAYERS + i] = &LocalPlayer_Instances[i].Base;
	}
	for (; i < MAX_LOCAL_PLAYERS; i++)
	{
		Entities.List[MAX_NET_PLAYERS + i] = NULL;
	}
	Entities.CurPlayer = &LocalPlayer_Instances[0];
	LocalPlayer_HookBinds();

	Game_Tasks.entities.interval = GAME_DEF_TICKS;
	Game_Tasks.entities.callback = Entities_Tick;
	ScheduledTask2_Add(&Game_Tasks.entities);
}

static void Entities_Free(void) {
	int i;
	for (i = 0; i < ENTITIES_MAX_COUNT; i++)
	{
		Entities_Remove(i);
	}
	sources_head = NULL;
}

struct IGameComponent Entities_Component = {
	Entities_Init,  /* Init  */
	Entities_Free,  /* Free  */
	LocalPlayers_Reset,    /* Reset */
	LocalPlayers_OnNewMap, /* OnNewMap */
};
