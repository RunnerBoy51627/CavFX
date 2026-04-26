#include "SelOutlineRenderer.h"
#include "PackedCol.h"
#include "Graphics.h"
#include "Game.h"
#include "Event.h"
#include "Picking.h"
#include "Funcs.h"
#include "Camera.h"
#include "Options.h"
#include "TexturePack.h"
#include "Block.h"

static GfxResourceID selOutline_vb;
static GfxResourceID break_vb;
static float base_size;
static PackedCol color;
#define SELOUTLINE_NUM_VERTICES (16 * 6)
#define BREAKING_NUM_VERTICES (4 * 6)
#define BREAKING_TEX_BASE 240

#define SelOutline_Y(y)\
0,y,1,  0,y,2,  1,y,2,  1,y,1,\
3,y,1,  3,y,2,  2,y,2,  2,y,1,\
0,y,0,  0,y,1,  3,y,1,  3,y,0,\
0,y,3,  0,y,2,  3,y,2,  3,y,3,

#define SelOutline_X(x)\
x,1,0,  x,2,0,  x,2,1,  x,1,1,\
x,1,3,  x,2,3,  x,2,2,  x,1,2,\
x,0,0,  x,1,0,  x,1,3,  x,0,3,\
x,3,0,  x,2,0,  x,2,3,  x,3,3,

#define SelOutline_Z(z)\
0,1,z,  0,2,z,  1,2,z,  1,1,z,\
3,1,z,  3,2,z,  2,2,z,  2,1,z,\
0,0,z,  0,1,z,  3,1,z,  3,0,z,\
0,3,z,  0,2,z,  3,2,z,  3,3,z,


static void BuildMesh(struct RayTracer* selected) {
	static const cc_uint8 indices[288] = {
		SelOutline_Y(0) SelOutline_Y(3) /* YMin, YMax */
		SelOutline_X(0) SelOutline_X(3) /* XMin, XMax */
		SelOutline_Z(0) SelOutline_Z(3) /* ZMin, ZMax */
	};
	
	struct VertexColoured* ptr;
	int i;
	Vec3 delta;
	float dist, offset;
	float size, scale;
	Vec3 coords[4];

	Vec3_Sub(&delta, &Camera.CurrentPos, &selected->Min);
	dist = Vec3_LengthSquared(&delta);

	offset = 0.01f;
	if (dist < 4.0f * 4.0f) offset = 0.00625f;
	if (dist < 2.0f * 2.0f) offset = 0.00500f;

	scale = 1.0f / 16.0f;
	if (dist < 32.0f * 32.0f) scale = 1.0f / 32.0f;
	if (dist < 16.0f * 16.0f) scale = 1.0f / 64.0f;
	if (dist <  8.0f *  8.0f) scale = 1.0f / 96.0f;
	if (dist <  4.0f *  4.0f) scale = 1.0f / 128.0f;
	if (dist <  2.0f *  2.0f) scale = 1.0f / 192.0f;
	size = base_size * scale;
	
	/*  How a face is laid out: 
	                 #--#-------#--#<== OUTER_MAX (3)
	                 |  |       |  |
	                 |  #-------#<===== INNER_MAX (2)
	                 |  |       |  |
					 |  |       |  |
	                 |  |       |  |
	(1) INNER_MIN =====>#-------#  |
	                 |  |       |  |
	(0) OUTER_MIN ==>#--#-------#--#

	- these are used to fake thick lines, by making the lines appear slightly inset
	- note: actual difference between inner and outer is much smaller than the diagram
	*/
	Vec3_Add1(&coords[0], &selected->Min, -offset);
	Vec3_Add1(&coords[1], &coords[0],      size);
	Vec3_Add1(&coords[3], &selected->Max,  offset);
	Vec3_Add1(&coords[2], &coords[3],     -size);
	
	ptr = (struct VertexColoured*)Gfx_LockDynamicVb(selOutline_vb, 
									VERTEX_FORMAT_COLOURED, SELOUTLINE_NUM_VERTICES);
	for (i = 0; i < Array_Elems(indices); i += 3, ptr++) 
	{
		ptr->x   = coords[indices[i + 0]].x;
		ptr->y   = coords[indices[i + 1]].y;
		ptr->z   = coords[indices[i + 2]].z;
		ptr->Col = color;
	}
	Gfx_UnlockDynamicVb(selOutline_vb);
}

void SelOutlineRenderer_Render(struct RayTracer* selected, cc_bool dirty) {
	if (Gfx.LostContext) return;

	if (!selOutline_vb)
		selOutline_vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_COLOURED, SELOUTLINE_NUM_VERTICES);
	
	Gfx_SetAlphaBlending(true);
	Gfx_SetDepthWrite(false);
	Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);

	if (dirty) BuildMesh(selected);
	else Gfx_BindDynamicVb(selOutline_vb);

	Gfx_DrawVb_IndexedTris(SELOUTLINE_NUM_VERTICES);
	Gfx_SetDepthWrite(true);
	Gfx_SetAlphaBlending(false);
}

static void Break_SetVertex(struct VertexTextured* v, float x, float y, float z, float u, float vv) {
	v->x = x; v->y = y; v->z = z;
	v->Col = PACKEDCOL_WHITE;
	v->U = u; v->V = vv;
}

static void Break_Quad(struct VertexTextured** ptr, Vec3 a, Vec3 b, Vec3 c, Vec3 d, TextureRec uv) {
	struct VertexTextured* v = *ptr;
	Break_SetVertex(v++, a.x, a.y, a.z, uv.u1, uv.v2);
	Break_SetVertex(v++, b.x, b.y, b.z, uv.u1, uv.v1);
	Break_SetVertex(v++, c.x, c.y, c.z, uv.u2, uv.v1);
	Break_SetVertex(v++, d.x, d.y, d.z, uv.u2, uv.v2);
	*ptr = v;
}

static void BuildBreakingMesh(struct RayTracer* selected, TextureRec uv) {
	struct VertexTextured* data;
	struct VertexTextured* ptr;
	Vec3 min, max;
	float eps = 0.003f;

	Vec3_Add1(&min, &selected->Min, -eps);
	Vec3_Add1(&max, &selected->Max,  eps);
	data = (struct VertexTextured*)Gfx_LockDynamicVb(break_vb, VERTEX_FORMAT_TEXTURED, BREAKING_NUM_VERTICES);
	ptr  = data;

	/* Y min, Y max */
	Break_Quad(&ptr, Vec3_Create3(min.x, min.y, max.z), Vec3_Create3(min.x, min.y, min.z), Vec3_Create3(max.x, min.y, min.z), Vec3_Create3(max.x, min.y, max.z), uv);
	Break_Quad(&ptr, Vec3_Create3(min.x, max.y, min.z), Vec3_Create3(min.x, max.y, max.z), Vec3_Create3(max.x, max.y, max.z), Vec3_Create3(max.x, max.y, min.z), uv);
	/* X min, X max */
	Break_Quad(&ptr, Vec3_Create3(min.x, min.y, min.z), Vec3_Create3(min.x, max.y, min.z), Vec3_Create3(min.x, max.y, max.z), Vec3_Create3(min.x, min.y, max.z), uv);
	Break_Quad(&ptr, Vec3_Create3(max.x, min.y, max.z), Vec3_Create3(max.x, max.y, max.z), Vec3_Create3(max.x, max.y, min.z), Vec3_Create3(max.x, min.y, min.z), uv);
	/* Z min, Z max */
	Break_Quad(&ptr, Vec3_Create3(max.x, min.y, min.z), Vec3_Create3(max.x, max.y, min.z), Vec3_Create3(min.x, max.y, min.z), Vec3_Create3(min.x, min.y, min.z), uv);
	Break_Quad(&ptr, Vec3_Create3(min.x, min.y, max.z), Vec3_Create3(min.x, max.y, max.z), Vec3_Create3(max.x, max.y, max.z), Vec3_Create3(max.x, min.y, max.z), uv);

	Gfx_UnlockDynamicVb(break_vb);
}

void SelOutlineRenderer_RenderBreaking(struct RayTracer* selected, float progress) {
	TextureRec uv;
	TextureLoc loc;
	int stage, atlasIndex;
	if (Gfx.LostContext || progress <= 0.0f) return;
	if (Blocks.Draw[selected->block] == DRAW_GAS) return;

	stage = (int)(progress * 10.0f);
	if (stage < 0) stage = 0;
	if (stage > 9) stage = 9;
	loc = BREAKING_TEX_BASE + stage;
	if (Atlas2D_TileY(loc) >= Atlas2D.RowsCount) return;

	uv = Atlas1D_TexRec(loc, 1, &atlasIndex);
	if (!break_vb) break_vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, BREAKING_NUM_VERTICES);
	BuildBreakingMesh(selected, uv);

	Gfx_SetAlphaTest(true);
	Gfx_SetAlphaBlending(true);
	Gfx_SetDepthWrite(false);
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_BindDynamicVb(break_vb);
	Atlas1D_Bind(atlasIndex);
	Gfx_DrawVb_IndexedTris(BREAKING_NUM_VERTICES);
	Gfx_SetDepthWrite(true);
	Gfx_SetAlphaBlending(false);
	Gfx_SetAlphaTest(false);
}


/*########################################################################################################################*
*-----------------------------------------------SelOutlineRenderer component----------------------------------------------*
*#########################################################################################################################*/
static void OnContextLost(void* obj) {
	Gfx_DeleteDynamicVb(&selOutline_vb);
	Gfx_DeleteDynamicVb(&break_vb);
}

static void OnInit(void) {
	int opacity;
	cc_uint8 rgb[3];
	Event_Register_(&GfxEvents.ContextLost, NULL, OnContextLost);

	base_size = Options_GetFloat(OPT_SELECTED_BLOCK_OUTLINE_SCALE, 1, 16, 1);
	opacity   = Options_GetInt(OPT_SELECTED_BLOCK_OUTLINE_OPACITY, 0, 255, 102);

	if (Options_GetColor(OPT_SELECTED_BLOCK_OUTLINE_COLOR, rgb)) {
		color = PackedCol_Make(rgb[0], rgb[1], rgb[2], opacity);
	} else {
		color = PackedCol_Make(0, 0, 0, opacity); /* Black by default */
	}
}

static void OnFree(void) { OnContextLost(NULL); }

struct IGameComponent SelOutlineRenderer_Component = {
	OnInit, /* Init */
	OnFree, /* Free */
};
