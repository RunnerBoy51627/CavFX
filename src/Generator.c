#include "Generator.h"
#include "BlockID.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Platform.h"
#include "World.h"
#include "Utils.h"
#include "Game.h"
#include "Screens.h"
#include "Window.h"
#include "Entity.h"
#include "MapRenderer.h"
#include "EnvRenderer.h"
#include "Lighting.h"
#include "Chat.h"

static const struct MapGenerator* gen_active;
BlockRaw* Gen_Blocks;

volatile float Gen_CurrentProgress;
volatile const char* Gen_CurrentState;
volatile static cc_bool gen_done;
static cc_bool gen_suppressDone;

/* There are two main types of multitasking: */
/*  - Pre-emptive multitasking (system automatically switches between threads) */
/*  - Cooperative multitasking (threads must be manually switched by the app) */
/*                                                                             */
/* Systems only supporting cooperative multitasking can be problematic though: */
/*   If the whole map generation was performed as a single function call, */
/*     then the game thread would not get run at all until map generation */
/*     completed - which is not a great user experience. */
/*   To avoid that, on these systems, map generation may be divided into */
/*     a series of steps so that ClassiCube can periodically switch back */
/*     to the game thread to ensure that the game itself still (slowly) runs. */
#ifdef CC_BUILD_COOPTHREADED
static int gen_step;
static cc_uint64 lastRender;

#define GEN_COOP_BEGIN \
	cc_uint64 curTime; \
	switch (gen_step) {

#define GEN_COOP_STEP(index, step) \
	case index: \
		step; \
		gen_step++; \
		curTime = Stopwatch_Measure(); \
		if (Stopwatch_ElapsedMS(lastRender, curTime) > 100) { lastRender = curTime; return; }
		/* Switch back to game thread if more than 100 milliseconds since it was last run */

#define GEN_COOP_END \
	}

static void Gen_Run(void) {
	gen_step = 0;
	lastRender = Stopwatch_Measure();
	gen_active->Generate();
}

cc_bool Gen_IsDone(void) {
	/* Resume map generation if incomplete */
	if (!gen_done) gen_active->Generate();
	return gen_done;
}
#else
/* For systems supporting preemptive threading, there's no point */
/* bothering with all the cooperative tasking shenanigans */
#define GEN_COOP_BEGIN
#define GEN_COOP_STEP(index, step) step;
#define GEN_COOP_END

static void Gen_DoGen(void) {
	gen_active->Generate();
}

static void Gen_Run(void) {
	void* thread;
	Thread_Run(&thread, Gen_DoGen, 128 * 1024, "Map gen");
	Thread_Detach(thread);
}

cc_bool Gen_IsDone(void) { return gen_done; }
#endif

static void Gen_Reset(void) {
	Gen_CurrentProgress = 0.0f;
	Gen_CurrentState    = "";
	gen_done = false;
}

void Gen_Start(const struct MapGenerator* gen, int seed,
				int width, int height, int length) {	
	World_NewMap();
	World_SetDimensions(width, height, length);
	World.Seed = seed;

	gen_active = gen;
	Gen_Reset();
	Gen_Blocks = (BlockRaw*)Mem_TryAlloc(World.Volume, 1);

	if (!Gen_Blocks || !gen->Prepare(seed)) {
		Window_ShowDialog("Out of memory", "Not enough free memory to generate a map that large.\nTry a smaller size.");
		gen_done = true;
	} else {
		Gen_Run();
	}

	GeneratingScreen_Show();
}


/*########################################################################################################################*
*-----------------------------------------------------Flatgrass gen-------------------------------------------------------*
*#########################################################################################################################*/
static void FlatgrassGen_MapSet(int yBeg, int yEnd, BlockRaw block) {
	cc_uint32 oneY = (cc_uint32)World.OneY;
	BlockRaw* ptr = Gen_Blocks;
	int y, yHeight;

	yBeg = max(yBeg, 0); yEnd = max(yEnd, 0);
	yHeight = (yEnd - yBeg) + 1;
	Gen_CurrentProgress = 0.0f;

	for (y = yBeg; y <= yEnd; y++) {
		Mem_Set(ptr + y * oneY, block, oneY);
		Gen_CurrentProgress = (float)(y - yBeg) / yHeight;
	}
}

static cc_bool FlatgrassGen_Prepare(int seed) {
	return true;
}

static void FlatgrassGen_Generate(void) {
	Gen_CurrentState = "Setting air blocks";
	FlatgrassGen_MapSet(World.Height / 2, World.MaxY, BLOCK_AIR);

	Gen_CurrentState = "Setting dirt blocks";
	FlatgrassGen_MapSet(0, World.Height / 2 - 2, BLOCK_DIRT);

	Gen_CurrentState = "Setting grass blocks";
	FlatgrassGen_MapSet(World.Height / 2 - 1, World.Height / 2 - 1, BLOCK_GRASS);

	gen_done = true;
}

const struct MapGenerator FlatgrassGen = {
	FlatgrassGen_Prepare,
	FlatgrassGen_Generate
};


/*########################################################################################################################*
*---------------------------------------------------Noise generation------------------------------------------------------*
*#########################################################################################################################*/
#define NOISE_TABLE_SIZE 512
static void ImprovedNoise_Init(cc_uint8* p, RNGState* rnd) {
	cc_uint8 tmp;
	int i, j;
	for (i = 0; i < 256; i++) { p[i] = i; }

	/* shuffle randomly using fisher-yates */
	for (i = 0; i < 256; i++) {
		j   = Random_Range(rnd, i, 256);
		tmp = p[i]; p[i] = p[j]; p[j] = tmp;
	}

	for (i = 0; i < 256; i++) {
		p[i + 256] = p[i];
	}
}

/* Normally, calculating Grad involves a function call + switch. However, the table combinations
  can be directly packed into a set of bit flags (where each 2 bit combination indicates either -1, 0 1).
  This avoids needing to call another function that performs branching */
#define X_FLAGS 0x46552222
#define Y_FLAGS 0x2222550A
#define Grad(hash, x, y) (((X_FLAGS >> (hash)) & 3) - 1) * (x) + (((Y_FLAGS >> (hash)) & 3) - 1) * (y);

static float ImprovedNoise_Calc(const cc_uint8* p, float x, float y) {
	int xFloor, yFloor, X, Y;
	float u, v;
	int A, B, hash;
	float g22, g12, c1;
	float g21, g11, c2;

	xFloor = x >= 0 ? (int)x : (int)x - 1;
	yFloor = y >= 0 ? (int)y : (int)y - 1;
	X = xFloor & 0xFF; Y = yFloor & 0xFF;
	x -= xFloor;       y -= yFloor;

	u = x * x * x * (x * (x * 6 - 15) + 10); /* Fade(x) */
	v = y * y * y * (y * (y * 6 - 15) + 10); /* Fade(y) */
	A = p[X] + Y; B = p[X + 1] + Y;

	hash = (p[p[A]] & 0xF) << 1;
	g22  = Grad(hash, x,     y); /* Grad(p[p[A], x,     y) */
	hash = (p[p[B]] & 0xF) << 1;
	g12  = Grad(hash, x - 1, y); /* Grad(p[p[B], x - 1, y) */
	c1   = g22 + u * (g12 - g22);

	hash = (p[p[A + 1]] & 0xF) << 1;
	g21  = Grad(hash, x,     y - 1); /* Grad(p[p[A + 1], x,     y - 1) */
	hash = (p[p[B + 1]] & 0xF) << 1;
	g11  = Grad(hash, x - 1, y - 1); /* Grad(p[p[B + 1], x - 1, y - 1) */
	c2   = g21 + u * (g11 - g21);

	return c1 + v * (c2 - c1);
}


struct OctaveNoise { cc_uint8 p[8][NOISE_TABLE_SIZE]; int octaves; };
static void OctaveNoise_Init(struct OctaveNoise* n, RNGState* rnd, int octaves) {
	int i;
	n->octaves = octaves;
	
	for (i = 0; i < octaves; i++) {
		ImprovedNoise_Init(n->p[i], rnd);
	}
}

static float OctaveNoise_Calc(const struct OctaveNoise* n, float x, float y) {
	float amplitude = 1, freq = 1;
	float sum = 0;
	int i;

	for (i = 0; i < n->octaves; i++) {
		sum += ImprovedNoise_Calc(n->p[i], x * freq, y * freq) * amplitude;
		amplitude *= 2.0f;
		freq *= 0.5f;
	}
	return sum;
}


struct CombinedNoise { struct OctaveNoise noise1, noise2; };
static void CombinedNoise_Init(struct CombinedNoise* n, RNGState* rnd, int octaves1, int octaves2) {
	OctaveNoise_Init(&n->noise1, rnd, octaves1);
	OctaveNoise_Init(&n->noise2, rnd, octaves2);
}

static float CombinedNoise_Calc(const struct CombinedNoise* n, float x, float y) {
	float offset = OctaveNoise_Calc(&n->noise2, x, y);
	return OctaveNoise_Calc(&n->noise1, x + offset, y);
}


/*########################################################################################################################*
*----------------------------------------------------Notchy map gen-------------------------------------------------------*
*#########################################################################################################################*/
static int waterLevel, minHeight;
static cc_int16* heightmap;
static RNGState rnd;

static void NotchyGen_FillOblateSpheroid(int x, int y, int z, float radius, BlockRaw block) {
	int xBeg = Math_Floor(max(x - radius, 0));
	int xEnd = Math_Floor(min(x + radius, World.MaxX));
	int yBeg = Math_Floor(max(y - radius, 0));
	int yEnd = Math_Floor(min(y + radius, World.MaxY));
	int zBeg = Math_Floor(max(z - radius, 0));
	int zEnd = Math_Floor(min(z + radius, World.MaxZ));

	float radiusSq = radius * radius;
	int index;
	int xx, yy, zz, dx, dy, dz;

	for (yy = yBeg; yy <= yEnd; yy++) { dy = yy - y;
		for (zz = zBeg; zz <= zEnd; zz++) { dz = zz - z;
			for (xx = xBeg; xx <= xEnd; xx++) { dx = xx - x;

				if ((dx * dx + 2 * dy * dy + dz * dz) < radiusSq) {
					index = World_Pack(xx, yy, zz);
					if (Gen_Blocks[index] == BLOCK_STONE)
						Gen_Blocks[index] = block;
				}
			}
		}
	}
}

#if CC_BUILD_MAXSTACK <= (32 * 1024)
	#define STACK_FAST 512
#else
	#define STACK_FAST 8192
#endif

static void NotchyGen_FloodFill(int index, BlockRaw block) {
	int* stack;
	int stack_default[STACK_FAST]; /* avoid allocating memory if possible */
	int count = 0, limit = STACK_FAST;
	int x, y, z;

	stack = stack_default;
	if (index < 0) return; /* y below map, don't bother starting */
	stack[count++] = index;

	while (count) {
		index = stack[--count];

		if (Gen_Blocks[index] != BLOCK_AIR) continue;
		Gen_Blocks[index] = block;

		x = index  % World.Width;
		y = index  / World.OneY;
		z = (index / World.Width) % World.Length;

		/* need to increase stack */
		if (count >= limit - FACE_COUNT) {
			Utils_Resize((void**)&stack, &limit, 4, STACK_FAST, STACK_FAST);
		}

		if (x > 0)          { stack[count++] = index - 1; }
		if (x < World.MaxX) { stack[count++] = index + 1; }
		if (z > 0)          { stack[count++] = index - World.Width; }
		if (z < World.MaxZ) { stack[count++] = index + World.Width; }
		if (y > 0)          { stack[count++] = index - World.OneY; }
	}
	if (limit > STACK_FAST) Mem_Free(stack);
}


static void NotchyGen_CreateHeightmap(void) {
	float hLow, hHigh, height;
	int hIndex = 0, adjHeight;
	int x, z;

#if CC_BUILD_MAXSTACK <= (16 * 1024)
	struct NoiseBuffer { 
		struct CombinedNoise n1, n2;
		struct OctaveNoise n3;
	};
	void* mem = TempMem_Alloc(sizeof(struct NoiseBuffer));

	struct NoiseBuffer* buf  = (struct NoiseBuffer*)mem;
	struct CombinedNoise* n1 = &buf->n1;
	struct CombinedNoise* n2 = &buf->n2;
	struct OctaveNoise*   n3 = &buf->n3;
#else
	struct CombinedNoise _n1, *n1 = &_n1;
	struct CombinedNoise _n2, *n2 = &_n2;
	struct OctaveNoise   _n3, *n3 = &_n3;
#endif

	CombinedNoise_Init(n1, &rnd, 8, 8);
	CombinedNoise_Init(n2, &rnd, 8, 8);	
	OctaveNoise_Init(n3,   &rnd, 6);

	Gen_CurrentState = "Building heightmap";
	for (z = 0; z < World.Length; z++) {
		Gen_CurrentProgress = (float)z / World.Length;

		for (x = 0; x < World.Width; x++) {
			hLow   = CombinedNoise_Calc(n1, x * 1.3f, z * 1.3f) / 6 - 4;
			height = hLow;

			if (OctaveNoise_Calc(n3, (float)x, (float)z) <= 0) {
				hHigh = CombinedNoise_Calc(n2, x * 1.3f, z * 1.3f) / 5 + 6;
				height = max(hLow, hHigh);
			}

			height *= 0.5f;
			if (height < 0) height *= 0.8f;

			adjHeight = (int)(height + waterLevel);
			minHeight = min(adjHeight, minHeight);
			heightmap[hIndex++] = adjHeight;
		}
	}
}

static int NotchyGen_CreateStrataFast(void) {
	cc_uint32 oneY = (cc_uint32)World.OneY;
	int stoneHeight, airHeight;
	int y;

	Gen_CurrentProgress = 0.0f;
	Gen_CurrentState    = "Filling map";
	/* Make lava layer at bottom */
	Mem_Set(Gen_Blocks, BLOCK_STILL_LAVA, oneY);

	/* Invariant: the lowest value dirtThickness can possible be is -14 */
	stoneHeight = minHeight - 14;
	/* We can quickly fill in bottom solid layers */
	for (y = 1; y <= stoneHeight; y++) {
		Mem_Set(Gen_Blocks + y * oneY, BLOCK_STONE, oneY);
		Gen_CurrentProgress = (float)y / World.Height;
	}

	/* Fill in rest of map wih air */
	airHeight = max(0, stoneHeight) + 1;
	for (y = airHeight; y < World.Height; y++) {
		Mem_Set(Gen_Blocks + y * oneY, BLOCK_AIR, oneY);
		Gen_CurrentProgress = (float)y / World.Height;
	}

	/* if stoneHeight is <= 0, then no layer is fully stone */
	return max(stoneHeight, 1);
}

static void NotchyGen_CreateStrata(void) {
	int dirtThickness, dirtHeight;
	int minStoneY, stoneHeight;
	int hIndex = 0, maxY = World.MaxY, index = 0;
	int x, y, z;
	struct OctaveNoise n;

	/* Try to bulk fill bottom of the map if possible */
	minStoneY = NotchyGen_CreateStrataFast();
	OctaveNoise_Init(&n, &rnd, 8);

	Gen_CurrentState = "Creating strata";
	for (z = 0; z < World.Length; z++) {
		Gen_CurrentProgress = (float)z / World.Length;

		for (x = 0; x < World.Width; x++) {
			dirtThickness = (int)(OctaveNoise_Calc(&n, (float)x, (float)z) / 24 - 4);
			dirtHeight    = heightmap[hIndex++];
			stoneHeight   = dirtHeight + dirtThickness;

			stoneHeight = min(stoneHeight, maxY);
			dirtHeight  = min(dirtHeight,  maxY);

			index = World_Pack(x, minStoneY, z);
			for (y = minStoneY; y <= stoneHeight; y++) {
				Gen_Blocks[index] = BLOCK_STONE; index += World.OneY;
			}

			stoneHeight = max(stoneHeight, 0);
			index = World_Pack(x, (stoneHeight + 1), z);
			for (y = stoneHeight + 1; y <= dirtHeight; y++) {
				Gen_Blocks[index] = BLOCK_DIRT; index += World.OneY;
			}
		}
	}
}

static void NotchyGen_CarveCaves(void) {
	int cavesCount, caveLen;
	float caveX, caveY, caveZ;
	float theta, deltaTheta, phi, deltaPhi;
	float caveRadius, radius;
	int cenX, cenY, cenZ;
	int i, j;

	cavesCount       = World.Volume / 8192;
	Gen_CurrentState = "Carving caves";
	for (i = 0; i < cavesCount; i++) {
		Gen_CurrentProgress = (float)i / cavesCount;

		caveX = (float)Random_Next(&rnd, World.Width);
		caveY = (float)Random_Next(&rnd, World.Height);
		caveZ = (float)Random_Next(&rnd, World.Length);

		caveLen = (int)(Random_Float(&rnd) * Random_Float(&rnd) * 200.0f);
		theta   = Random_Float(&rnd) * 2.0f * MATH_PI; deltaTheta = 0.0f;
		phi     = Random_Float(&rnd) * 2.0f * MATH_PI; deltaPhi   = 0.0f;
		caveRadius = Random_Float(&rnd) * Random_Float(&rnd);

		for (j = 0; j < caveLen; j++) {
			caveX += Math_SinF(theta) * Math_CosF(phi);
			caveZ += Math_CosF(theta) * Math_CosF(phi);
			caveY += Math_SinF(phi);

			theta      = theta + deltaTheta * 0.2f;
			deltaTheta = deltaTheta * 0.9f + Random_Float(&rnd) - Random_Float(&rnd);
			phi        = phi * 0.5f + deltaPhi * 0.25f;
			deltaPhi   = deltaPhi  * 0.75f + Random_Float(&rnd) - Random_Float(&rnd);
			if (Random_Float(&rnd) < 0.25f) continue;

			cenX = (int)(caveX + (Random_Next(&rnd, 4) - 2) * 0.2f);
			cenY = (int)(caveY + (Random_Next(&rnd, 4) - 2) * 0.2f);
			cenZ = (int)(caveZ + (Random_Next(&rnd, 4) - 2) * 0.2f);

			radius = (World.Height - cenY) / (float)World.Height;
			radius = 1.2f + (radius * 3.5f + 1.0f) * caveRadius;
			radius = radius * Math_SinF(j * MATH_PI / caveLen);
			NotchyGen_FillOblateSpheroid(cenX, cenY, cenZ, radius, BLOCK_AIR);
		}
	}
}

static void NotchyGen_CarveOreVeins(float abundance, const char* state, BlockRaw block) {
	int numVeins, veinLen;
	float veinX, veinY, veinZ;
	float theta, deltaTheta, phi, deltaPhi;
	float radius;
	int i, j;

	numVeins         = (int)(World.Volume * abundance / 16384);
	Gen_CurrentState = state;
	for (i = 0; i < numVeins; i++) {
		Gen_CurrentProgress = (float)i / numVeins;

		veinX = (float)Random_Next(&rnd, World.Width);
		veinY = (float)Random_Next(&rnd, World.Height);
		veinZ = (float)Random_Next(&rnd, World.Length);

		veinLen = (int)(Random_Float(&rnd) * Random_Float(&rnd) * 75 * abundance);
		theta = Random_Float(&rnd) * 2.0f * MATH_PI; deltaTheta = 0.0f;
		phi   = Random_Float(&rnd) * 2.0f * MATH_PI; deltaPhi   = 0.0f;

		for (j = 0; j < veinLen; j++) {
			veinX += Math_SinF(theta) * Math_CosF(phi);
			veinZ += Math_CosF(theta) * Math_CosF(phi);
			veinY += Math_SinF(phi);

			theta      = deltaTheta * 0.2f;
			deltaTheta = deltaTheta * 0.9f + Random_Float(&rnd) - Random_Float(&rnd);
			phi        = phi * 0.5f + deltaPhi * 0.25f;
			deltaPhi   = deltaPhi   * 0.9f + Random_Float(&rnd) - Random_Float(&rnd);

			radius = abundance * Math_SinF(j * MATH_PI / veinLen) + 1.0f;
			NotchyGen_FillOblateSpheroid((int)veinX, (int)veinY, (int)veinZ, radius, block);
		}
	}
}

static void NotchyGen_FloodFillWaterBorders(void) {
	int waterY = waterLevel - 1;
	int index1, index2;
	int x, z;
	Gen_CurrentState = "Flooding edge water";

	index1 = World_Pack(0, waterY, 0);
	index2 = World_Pack(0, waterY, World.Length - 1);
	for (x = 0; x < World.Width; x++) {
		Gen_CurrentProgress = 0.0f + ((float)x / World.Width) * 0.5f;

		NotchyGen_FloodFill(index1, BLOCK_STILL_WATER);
		NotchyGen_FloodFill(index2, BLOCK_STILL_WATER);
		index1++; index2++;
	}

	index1 = World_Pack(0,             waterY, 0);
	index2 = World_Pack(World.Width - 1, waterY, 0);
	for (z = 0; z < World.Length; z++) {
		Gen_CurrentProgress = 0.5f + ((float)z / World.Length) * 0.5f;

		NotchyGen_FloodFill(index1, BLOCK_STILL_WATER);
		NotchyGen_FloodFill(index2, BLOCK_STILL_WATER);
		index1 += World.Width; index2 += World.Width;
	}
}

static void NotchyGen_FloodFillWater(void) {
	int numSources;
	int i, x, y, z;

	numSources       = World.Width * World.Length / 800;
	Gen_CurrentState = "Flooding water";
	for (i = 0; i < numSources; i++) {
		Gen_CurrentProgress = (float)i / numSources;

		x = Random_Next(&rnd, World.Width);
		z = Random_Next(&rnd, World.Length);
		y = waterLevel - Random_Range(&rnd, 1, 3);
		NotchyGen_FloodFill(World_Pack(x, y, z), BLOCK_STILL_WATER);
	}
}

static void NotchyGen_FloodFillLava(void) {
	int numSources;
	int i, x, y, z;

	numSources       = World.Width * World.Length / 20000;
	Gen_CurrentState = "Flooding lava";
	for (i = 0; i < numSources; i++) {
		Gen_CurrentProgress = (float)i / numSources;

		x = Random_Next(&rnd, World.Width);
		z = Random_Next(&rnd, World.Length);
		y = (int)((waterLevel - 3) * Random_Float(&rnd) * Random_Float(&rnd));
		NotchyGen_FloodFill(World_Pack(x, y, z), BLOCK_STILL_LAVA);
	}
}

static void NotchyGen_CreateSurfaceLayer(void) {	
	int hIndex = 0, index;
	BlockRaw above;
	int x, y, z;
#if CC_BUILD_MAXSTACK <= (16 * 1024)
	struct NoiseBuffer { 
		struct OctaveNoise n1, n2;
	};
	struct NoiseBuffer* buf = TempMem_Alloc(sizeof(struct NoiseBuffer));
	struct OctaveNoise* n1 = &buf->n1;
	struct OctaveNoise* n2 = &buf->n2;
#else
	struct OctaveNoise _n1, _n2;
	struct OctaveNoise* n1 = &_n1;
	struct OctaveNoise* n2 = &_n2;
#endif

	OctaveNoise_Init(n1, &rnd, 8);
	OctaveNoise_Init(n2, &rnd, 8);

	Gen_CurrentState = "Creating surface";
	for (z = 0; z < World.Length; z++) {
		Gen_CurrentProgress = (float)z / World.Length;

		for (x = 0; x < World.Width; x++) {
			y = heightmap[hIndex++];
			if (y < 0 || y >= World.Height) continue;

			index = World_Pack(x, y, z);
			above = y >= World.MaxY ? BLOCK_AIR : Gen_Blocks[index + World.OneY];

			/* TODO: update heightmap */
			if (above == BLOCK_STILL_WATER && (OctaveNoise_Calc(n2, (float)x, (float)z) > 12)) {
				Gen_Blocks[index] = BLOCK_GRAVEL;
			} else if (above == BLOCK_AIR) {
				Gen_Blocks[index] = (y <= waterLevel && (OctaveNoise_Calc(n1, (float)x, (float)z) > 8)) ? BLOCK_SAND : BLOCK_GRASS;
			}
		}
	}
}

static void NotchyGen_PlantFlowers(void) {
	int numPatches;
	BlockRaw block;
	int patchX,  patchZ;
	int flowerX, flowerY, flowerZ;
	int i, j, k, index;

	if (Game_Version.Version < VERSION_0023) return;
	numPatches       = World.Width * World.Length / 3000;
	Gen_CurrentState = "Planting flowers";

	for (i = 0; i < numPatches; i++) {
		Gen_CurrentProgress = (float)i / numPatches;

		block  = (BlockRaw)(BLOCK_DANDELION + Random_Next(&rnd, 2));
		patchX = Random_Next(&rnd, World.Width);
		patchZ = Random_Next(&rnd, World.Length);

		for (j = 0; j < 10; j++) {
			flowerX = patchX; flowerZ = patchZ;
			for (k = 0; k < 5; k++) {
				flowerX += Random_Next(&rnd, 6) - Random_Next(&rnd, 6);
				flowerZ += Random_Next(&rnd, 6) - Random_Next(&rnd, 6);

				if (!World_ContainsXZ(flowerX, flowerZ)) continue;
				flowerY = heightmap[flowerZ * World.Width + flowerX] + 1;
				if (flowerY <= 0 || flowerY >= World.Height) continue;

				index = World_Pack(flowerX, flowerY, flowerZ);
				if (Gen_Blocks[index] == BLOCK_AIR && Gen_Blocks[index - World.OneY] == BLOCK_GRASS)
					Gen_Blocks[index] = block;
			}
		}
	}
}

static void NotchyGen_PlantMushrooms(void) {
	int numPatches, groundHeight;
	BlockRaw block;
	int patchX, patchY, patchZ;
	int mushX,  mushY,  mushZ;
	int i, j, k, index;

	if (Game_Version.Version < VERSION_0023) return;
	numPatches       = World.Volume / 2000;
	Gen_CurrentState = "Planting mushrooms";

	for (i = 0; i < numPatches; i++) {
		Gen_CurrentProgress = (float)i / numPatches;

		block  = (BlockRaw)(BLOCK_BROWN_SHROOM + Random_Next(&rnd, 2));
		patchX = Random_Next(&rnd, World.Width);
		patchY = Random_Next(&rnd, World.Height);
		patchZ = Random_Next(&rnd, World.Length);

		for (j = 0; j < 20; j++) {
			mushX = patchX; mushY = patchY; mushZ = patchZ;
			for (k = 0; k < 5; k++) {
				mushX += Random_Next(&rnd, 6) - Random_Next(&rnd, 6);
				mushZ += Random_Next(&rnd, 6) - Random_Next(&rnd, 6);

				if (!World_ContainsXZ(mushX, mushZ)) continue;
				groundHeight = heightmap[mushZ * World.Width + mushX];
				if (mushY >= (groundHeight - 1)) continue;

				index = World_Pack(mushX, mushY, mushZ);
				if (Gen_Blocks[index] == BLOCK_AIR && Gen_Blocks[index - World.OneY] == BLOCK_STONE)
					Gen_Blocks[index] = block;
			}
		}
	}
}

static void NotchyGen_PlantTrees(void) {
	int numPatches;
	int patchX, patchZ;
	int treeX, treeY, treeZ;
	int treeHeight, index, count;
	BlockRaw under;
	int i, j, k, m;

	IVec3 coords[TREE_MAX_COUNT];
	BlockRaw blocks[TREE_MAX_COUNT];

	Tree_Blocks = Gen_Blocks;
	Tree_Rnd    = &rnd;

	numPatches       = World.Width * World.Length / 4000;
	Gen_CurrentState = "Planting trees";
	for (i = 0; i < numPatches; i++) {
		Gen_CurrentProgress = (float)i / numPatches;

		patchX = Random_Next(&rnd, World.Width);
		patchZ = Random_Next(&rnd, World.Length);

		for (j = 0; j < 20; j++) {
			treeX = patchX; treeZ = patchZ;
			for (k = 0; k < 20; k++) {
				treeX += Random_Next(&rnd, 6) - Random_Next(&rnd, 6);
				treeZ += Random_Next(&rnd, 6) - Random_Next(&rnd, 6);

				if (!World_ContainsXZ(treeX, treeZ) || Random_Float(&rnd) >= 0.25f) continue;
				treeY = heightmap[treeZ * World.Width + treeX] + 1;
				if (treeY >= World.Height) continue;
				treeHeight = 5 + Random_Next(&rnd, 3);

				index = World_Pack(treeX, treeY, treeZ);
				under = treeY > 0 ? Gen_Blocks[index - World.OneY] : BLOCK_AIR;

				if (under == BLOCK_GRASS && TreeGen_CanGrow(treeX, treeY, treeZ, treeHeight)) {
					count = TreeGen_Grow(treeX, treeY, treeZ, treeHeight, coords, blocks);

					for (m = 0; m < count; m++) {
						index = World_Pack(coords[m].x, coords[m].y, coords[m].z);
						Gen_Blocks[index] = blocks[m];
					}
				}
			}
		}
	}
}

static cc_bool NotchyGen_Prepare(int seed) {
	Random_Seed(&rnd, seed);
	waterLevel = World.Height / 2;	
	minHeight  = World.Height;

	heightmap  = (cc_int16*)Mem_TryAlloc(World.Width * World.Length, 2);
	return heightmap != NULL;
}

static void NotchyGen_Generate(void) {
	GEN_COOP_BEGIN
		GEN_COOP_STEP( 0, NotchyGen_CreateHeightmap() );
		GEN_COOP_STEP( 1, NotchyGen_CreateStrata() );
		GEN_COOP_STEP( 2, NotchyGen_CarveCaves() );
		GEN_COOP_STEP( 3, NotchyGen_CarveOreVeins(0.9f, "Carving coal ore", BLOCK_COAL_ORE) );
		GEN_COOP_STEP( 4, NotchyGen_CarveOreVeins(0.7f, "Carving iron ore", BLOCK_IRON_ORE) );
		GEN_COOP_STEP( 5, NotchyGen_CarveOreVeins(0.5f, "Carving gold ore", BLOCK_GOLD_ORE) );

		GEN_COOP_STEP( 6, NotchyGen_FloodFillWaterBorders() );
		GEN_COOP_STEP( 7, NotchyGen_FloodFillWater() );
		GEN_COOP_STEP( 8, NotchyGen_FloodFillLava() );

		GEN_COOP_STEP( 9, NotchyGen_CreateSurfaceLayer() );
		GEN_COOP_STEP(10, NotchyGen_PlantFlowers() );
		GEN_COOP_STEP(11, NotchyGen_PlantMushrooms() );
		GEN_COOP_STEP(12, NotchyGen_PlantTrees() );
	GEN_COOP_END

	Mem_Free(heightmap);
	heightmap = NULL;
	if (!gen_suppressDone) gen_done = true;
}

const struct MapGenerator NotchyGen = {
	NotchyGen_Prepare,
	NotchyGen_Generate
};


/*########################################################################################################################*
*--------------------------------------------------Experimental world types-----------------------------------------------*
*#########################################################################################################################*/
static cc_bool IslandsGen_Prepare(int seed) {
	return NotchyGen_Prepare(seed);
}

static cc_bool ExpGen_IsTerrainBlock(BlockRaw block) {
	if (block == BLOCK_AIR || block == BLOCK_WATER || block == BLOCK_STILL_WATER) return false;
	if (block == BLOCK_LAVA || block == BLOCK_STILL_LAVA) return false;
	if (block == BLOCK_LOG || block == BLOCK_LEAVES || block == BLOCK_SAPLING) return false;
	if (block == BLOCK_DANDELION || block == BLOCK_ROSE) return false;
	if (block == BLOCK_BROWN_SHROOM || block == BLOCK_RED_SHROOM) return false;
	return true;
}

static int ExpGen_ColumnTop(int x, int z) {
	int y, index;
	BlockRaw block;
	for (y = World.MaxY; y >= 0; y--) {
		index = World_Pack(x, y, z);
		block = Gen_Blocks[index];
		if (ExpGen_IsTerrainBlock(block)) return y;
	}
	return 0;
}

static void ExpGen_BuildLandColumn(int x, int z, int topY, BlockRaw surface) {
	int y, index;
	if (topY < 2) topY = 2;
	if (topY > World.MaxY - 2) topY = World.MaxY - 2;

	for (y = 0; y < World.Height; y++) {
		index = World_Pack(x, y, z);
		if (y == 0)              Gen_Blocks[index] = BLOCK_STONE;
		else if (y < topY - 4)   Gen_Blocks[index] = BLOCK_STONE;
		else if (y < topY)       Gen_Blocks[index] = BLOCK_DIRT;
		else if (y == topY)      Gen_Blocks[index] = surface;
		else                    Gen_Blocks[index] = BLOCK_AIR;
	}
}

static void ExpGen_BuildWaterColumn(int x, int z, int topY, int sea, BlockRaw surface) {
	int y, index;
	BlockRaw block;

	if (topY < 2) topY = 2;
	if (topY > World.MaxY - 3) topY = World.MaxY - 3;

	for (y = 0; y < World.Height; y++) {
		index = World_Pack(x, y, z);
		if (y == 0)             block = BLOCK_STONE;
		else if (y < topY - 4)  block = BLOCK_STONE;
		else if (y < topY)      block = BLOCK_DIRT;
		else if (y == topY)     block = surface;
		else if (y <= sea)      block = BLOCK_STILL_WATER;
		else                   block = BLOCK_AIR;
		Gen_Blocks[index] = block;
	}
}

static cc_uint32 ExpGen_Hash(int x, int z, int salt) {
	cc_uint32 h = (cc_uint32)World.Seed;
	h ^= (cc_uint32)(x * 374761393);
	h ^= (cc_uint32)(z * 668265263);
	h ^= (cc_uint32)(salt * 1442695041);
	h = (h ^ (h >> 13)) * 1274126177;
	return h ^ (h >> 16);
}

static float ExpGen_ValueNoise(int x, int z, int salt) {
	return (ExpGen_Hash(x, z, salt) & 0xFFFF) / 65535.0f;
}

static float ExpGen_SmoothNoise(float x, float z, float scale, int salt) {
	int x0, z0, x1, z1;
	float fx, fz, a, b, c, d, ab, cd;
	x /= scale; z /= scale;
	x0 = Math_Floor(x); z0 = Math_Floor(z);
	x1 = x0 + 1;        z1 = z0 + 1;
	fx = x - x0;        fz = z - z0;

	/* Smoothstep, so patches blend instead of becoming checkerboard/static. */
	fx = fx * fx * (3.0f - 2.0f * fx);
	fz = fz * fz * (3.0f - 2.0f * fz);

	a  = ExpGen_ValueNoise(x0, z0, salt);
	b  = ExpGen_ValueNoise(x1, z0, salt);
	c  = ExpGen_ValueNoise(x0, z1, salt);
	d  = ExpGen_ValueNoise(x1, z1, salt);
	ab = Math_Lerp(a, b, fx);
	cd = Math_Lerp(c, d, fx);
	return Math_Lerp(ab, cd, fz);
}

static float ExpGen_Fbm(float x, float z, float scale, int salt) {
	float n = 0.0f;
	n += ExpGen_SmoothNoise(x, z, scale,       salt)     * 0.55f;
	n += ExpGen_SmoothNoise(x, z, scale * 0.5f, salt+13) * 0.30f;
	n += ExpGen_SmoothNoise(x, z, scale * 0.25f,salt+29) * 0.15f;
	return n;
}

static BlockRaw ExpGen_SurfaceFromPatch(int x, int z, float scale, float sandCut, float gravelCut) {
	float mat = ExpGen_Fbm((float)x, (float)z, scale, 8001);
	if (mat < sandCut)   return BLOCK_SAND;
	if (mat < gravelCut) return BLOCK_GRAVEL;
	return BLOCK_GRASS;
}

static float IslandsGen_Blob(float x, float z, float cx, float cz, float rx, float rz) {
	float dx = (x - cx) / rx;
	float dz = (z - cz) / rz;
	float v  = 1.0f - (dx * dx + dz * dz);
	return v < 0.0f ? 0.0f : v;
}

static float IslandsGen_Mask(int x, int z) {
	float fx = (float)x, fz = (float)z;
	float w = (float)World.Width, l = (float)World.Length;
	float cx = w * 0.50f, cz = l * 0.52f;
	float dx, dz, dist, mask, warp, sat, v;

	/* Indev Island = Inland terrain cut by a strong island falloff.
	   Warp x/z before measuring distance so the coastline is not a perfect circle. */
	warp  = (ExpGen_Fbm(fx, fz, 86.0f, 4411) - 0.5f) * 34.0f;
	dx    = (fx + warp) - cx;
	warp  = (ExpGen_Fbm(fx, fz, 91.0f, 4427) - 0.5f) * 30.0f;
	dz    = (fz + warp) - cz;

	/* Slightly elliptical, fills most of the map like the classic preview. */
	dx /= w * 0.43f;
	dz /= l * 0.36f;
	dist = Math_SqrtF(dx * dx + dz * dz);

	mask = 1.0f - dist;
	if (mask < 0.0f) mask = 0.0f;

	/* Smooth, broad beaches. Squaring was making the land too tiny/round. */
	mask = mask * (1.65f - 0.65f * mask);

	/* Ragged coast, but low frequency only -- no TV static shoreline. */
	mask += (ExpGen_Fbm(fx, fz, 44.0f, 4501) - 0.5f) * 0.22f;
	mask += (ExpGen_Fbm(fx, fz, 21.0f, 4517) - 0.5f) * 0.08f;

	/* Satellite islands around the main landmass. */
	sat = 0.0f;
	v = IslandsGen_Blob(fx, fz, w * 0.18f, l * 0.23f, w * 0.095f, l * 0.070f); if (v > sat) sat = v;
	v = IslandsGen_Blob(fx, fz, w * 0.80f, l * 0.30f, w * 0.115f, l * 0.075f); if (v > sat) sat = v;
	v = IslandsGen_Blob(fx, fz, w * 0.73f, l * 0.78f, w * 0.095f, l * 0.085f); if (v > sat) sat = v;
	v = IslandsGen_Blob(fx, fz, w * 0.34f, l * 0.86f, w * 0.080f, l * 0.055f); if (v > sat) sat = v;
	v = IslandsGen_Blob(fx, fz, w * 0.58f, l * 0.14f, w * 0.065f, l * 0.050f); if (v > sat) sat = v;

	if (sat > mask) mask = sat * 0.82f;
	return mask;
}

static void IslandsGen_Generate(void) {
	int x, z, topY, sea, oldTop;
	float mask, large, medium, detail, lake, beachNoise;
	BlockRaw surface;
	cc_bool canPlantTrees;

	gen_suppressDone = true;
	NotchyGen_Generate();
	gen_suppressDone = false;

	sea = World.Height / 2;
	Gen_CurrentState = "Building Indev island";

	Mem_Free(heightmap);
	heightmap = (cc_int16*)Mem_TryAlloc(World.Width * World.Length, 2);
	canPlantTrees = heightmap != NULL;

	for (z = 0; z < World.Length; z++) {
		for (x = 0; x < World.Width; x++) {
			mask = IslandsGen_Mask(x, z);

			/* Ocean outside the island mask. */
			if (mask < 0.135f) {
				int y, index;
				for (y = 0; y < World.Height; y++) {
					index = World_Pack(x, y, z);
					Gen_Blocks[index] = y <= sea ? BLOCK_STILL_WATER : BLOCK_AIR;
				}
				if (canPlantTrees) heightmap[z * World.Width + x] = 0;
				continue;
			}

			oldTop = ExpGen_ColumnTop(x, z);

			/* Same style as Inland, then multiplied by island falloff. */
			large  = (ExpGen_Fbm((float)x, (float)z, 112.0f, 4601) - 0.5f) * 18.0f;
			medium = (ExpGen_Fbm((float)x, (float)z,  52.0f, 4631) - 0.5f) *  8.0f;
			detail = (ExpGen_Fbm((float)x, (float)z,  25.0f, 4661) - 0.5f) *  3.0f;

			topY = sea + 7 + (int)((large + medium + detail) * (0.45f + mask * 0.95f));

			/* Use some original Notchy terrain in the center so CavFX still feels familiar. */
			if (mask > 0.34f) topY = (topY * 3 + oldTop) / 4;

			/* Coastlines slope down into beaches instead of forming harsh walls. */
			if (mask < 0.32f) {
				topY = sea - 1 + (int)((mask - 0.135f) * 28.0f);
			}

			/* A few interior ponds/lagoons like the preview. */
			lake = ExpGen_Fbm((float)x, (float)z, 74.0f, 4701);
			if (mask > 0.38f && lake < 0.18f && topY < sea + 9) topY = sea - 1;

			/* Classic stepped Indev look. */
			topY = (topY / 2) * 2;

			/* Beaches on the coast and around low water, grass inland. */
			beachNoise = ExpGen_Fbm((float)x, (float)z, 39.0f, 4801);
			if (topY <= sea + 2 || mask < 0.25f) {
				surface = BLOCK_SAND;
			} else if (beachNoise > 0.88f && mask > 0.42f) {
				surface = BLOCK_GRAVEL;
			} else {
				surface = BLOCK_GRASS;
			}

			ExpGen_BuildWaterColumn(x, z, topY, sea, surface);
			if (canPlantTrees) heightmap[z * World.Width + x] = topY;
		}
		Gen_CurrentProgress = (float)z / World.Length;
	}

	if (canPlantTrees) {
		Gen_CurrentState = "Planting island trees";
		NotchyGen_PlantTrees();
		Mem_Free(heightmap);
		heightmap = NULL;
	}

	gen_done = true;
}

const struct MapGenerator IslandsGen = {
	IslandsGen_Prepare,
	IslandsGen_Generate
};

static cc_bool InlandsGen_Prepare(int seed) {
	return NotchyGen_Prepare(seed);
}

static void InlandsGen_BuildColumn(int x, int z, int topY, int sea, BlockRaw surface) {
	int y, index;
	BlockRaw block;

	if (topY < 2) topY = 2;
	if (topY > World.MaxY - 3) topY = World.MaxY - 3;

	for (y = 0; y < World.Height; y++) {
		index = World_Pack(x, y, z);

		if (y == 0) {
			block = BLOCK_STONE;
		} else if (y < topY - 4) {
			block = BLOCK_STONE;
		} else if (y < topY) {
			block = BLOCK_DIRT;
		} else if (y == topY) {
			block = surface;
		} else if (y <= sea) {
			block = BLOCK_STILL_WATER;
		} else {
			block = BLOCK_AIR;
		}

		Gen_Blocks[index] = block;
	}
}

static BlockRaw InlandsGen_Surface(int x, int z, int topY, int sea) {
	float broad, gravel;

	/* Beaches and lake beds are sandy, like the old Indev screenshots. */
	if (topY <= sea + 1) return BLOCK_SAND;

	/* Broad, coherent material patches. Keep grass dominant. */
	broad  = ExpGen_Fbm((float)x, (float)z, 76.0f, 12001);
	gravel = ExpGen_Fbm((float)x, (float)z, 34.0f, 12077);

	if (broad < 0.17f) return BLOCK_SAND;
	if (broad > 0.84f || gravel > 0.82f) return BLOCK_GRAVEL;
	return BLOCK_GRASS;
}

static void InlandsGen_Generate(void) {
	int x, z, topY, sea, oldTop;
	float large, medium, detail, lake, edge, dx, dz, maxDist;
	BlockRaw surface;
	cc_bool canPlantTrees;

	gen_suppressDone = true;
	NotchyGen_Generate();
	gen_suppressDone = false;

	sea = World.Height / 2;
	Gen_CurrentState = "Building Indev inland";

	Mem_Free(heightmap);
	heightmap = (cc_int16*)Mem_TryAlloc(World.Width * World.Length, 2);
	canPlantTrees = heightmap != NULL;

	for (z = 0; z < World.Length; z++) {
		for (x = 0; x < World.Width; x++) {
			oldTop = ExpGen_ColumnTop(x, z);

			/* Low-frequency hills first. This is the big fix: shape the land as a heightmap,
			   instead of choosing sand/gravel/grass per block like TV static. */
			large  = (ExpGen_Fbm((float)x, (float)z, 118.0f, 11001) - 0.5f) * 18.0f;
			medium = (ExpGen_Fbm((float)x, (float)z,  54.0f, 11031) - 0.5f) *  8.0f;
			detail = (ExpGen_Fbm((float)x, (float)z,  24.0f, 11061) - 0.5f) *  3.0f;
			topY   = sea + 5 + (int)(large + medium + detail);

			/* Blend in some of the original Notchy terrain so it still feels like CavFX/ClassiCube. */
			topY = (topY * 3 + oldTop) / 4;

			/* Inland is not an island map, but the original preview still has lakes/river dents.
			   Lower some valley noise into water while keeping the map mostly land. */
			lake = ExpGen_Fbm((float)x, (float)z, 86.0f, 11111);
			if (lake < 0.205f && topY < sea + 8) topY = sea - 2;
			else if (lake < 0.255f && topY < sea + 6) topY = sea;

			/* Avoid the old ocean-border feel by slightly lifting the outer rim into land. */
			dx = Math_AbsF((float)x - (float)World.Width  * 0.5f) / ((float)World.Width  * 0.5f);
			dz = Math_AbsF((float)z - (float)World.Length * 0.5f) / ((float)World.Length * 0.5f);
			maxDist = dx > dz ? dx : dz;
			if (maxDist > 0.88f && topY < sea + 3) topY = sea + 3 + (int)((maxDist - 0.88f) * 10.0f);

			/* Indev-ish terrace steps, matching the layered look from the reference image. */
			topY = (topY / 2) * 2;

			surface = InlandsGen_Surface(x, z, topY, sea);
			InlandsGen_BuildColumn(x, z, topY, sea, surface);

			if (canPlantTrees) {
				heightmap[z * World.Width + x] = topY;
			}
		}
		Gen_CurrentProgress = (float)z / World.Length;
	}

	/* Add trees after rebuilding the terrain so the map starts to resemble the real Inland preview. */
	if (canPlantTrees) {
		Gen_CurrentState = "Planting inland trees";
		NotchyGen_PlantTrees();
		Mem_Free(heightmap);
		heightmap = NULL;
	}

	gen_done = true;
}
const struct MapGenerator InlandsGen = {
	InlandsGen_Prepare,
	InlandsGen_Generate
};


/*########################################################################################################################*
*--------------------------------------------CavFX experimental infinite chunks-------------------------------------------*
*#########################################################################################################################*/

#define CAVFX_INF_CHUNK_SIZE 16
#define CAVFX_INF_WORLD_SIZE 256
#define CAVFX_INF_SHIFT_CHUNKS 4
#define CAVFX_INF_BORDER 56
#define CAVFX_INF_SHIFT_COOLDOWN 1.75

cc_bool CavFXInfiniteChunks_Active;
static int cavfx_inf_seed;
static int cavfx_inf_originChunkX, cavfx_inf_originChunkZ;
static cc_bool cavfx_inf_loadedOnce;
static cc_bool cavfx_inf_shifting;
static double cavfx_inf_nextShiftTime;

static cc_uint32 CavFXInf_Hash(int x, int z, int salt) {
	cc_uint32 h = (cc_uint32)(x * 374761393u) ^ (cc_uint32)(z * 668265263u) ^ (cc_uint32)(salt * 1442695041u);
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static int CavFXInf_ValueNoise(int x, int z, int scale, int salt) {
	int gx = x >= 0 ? x / scale : -((-x + scale - 1) / scale);
	int gz = z >= 0 ? z / scale : -((-z + scale - 1) / scale);
	int lx = x - gx * scale;
	int lz = z - gz * scale;
	int a = (int)(CavFXInf_Hash(gx,     gz,     salt) & 255);
	int b = (int)(CavFXInf_Hash(gx + 1, gz,     salt) & 255);
	int c = (int)(CavFXInf_Hash(gx,     gz + 1, salt) & 255);
	int d = (int)(CavFXInf_Hash(gx + 1, gz + 1, salt) & 255);
	int ab = a + ((b - a) * lx) / scale;
	int cd = c + ((d - c) * lx) / scale;
	return ab + ((cd - ab) * lz) / scale;
}

static int CavFXInf_HeightAt(int worldX, int worldZ) {
	int base   = CavFXInf_ValueNoise(worldX, worldZ, 64, cavfx_inf_seed + 11);
	int hills  = CavFXInf_ValueNoise(worldX, worldZ, 24, cavfx_inf_seed + 23);
	int detail = CavFXInf_ValueNoise(worldX, worldZ,  9, cavfx_inf_seed + 37);
	int h = 29 + ((base - 128) / 14) + ((hills - 128) / 18) + ((detail - 128) / 48);
	if (h < 8) h = 8;
	if (h > World.Height - 6) h = World.Height - 6;
	return h;
}

static BlockRaw CavFXInf_TopBlockAt(int worldX, int worldZ, int height, int waterLevel) {
	int mat = CavFXInf_ValueNoise(worldX, worldZ, 32, cavfx_inf_seed + 51);
	if (height <= waterLevel + 1) return BLOCK_SAND;
	if (mat > 218) return BLOCK_GRAVEL;
	if (mat < 36)  return BLOCK_SAND;
	return BLOCK_GRASS;
}

static void CavFXInf_GenColumn(BlockRaw* blocks, int localX, int localZ, int worldX, int worldZ) {
	int y, index;
	int height = CavFXInf_HeightAt(worldX, worldZ);
	int waterLevel = World.Height / 2;
	BlockRaw top = CavFXInf_TopBlockAt(worldX, worldZ, height, waterLevel);
	for (y = 0; y < World.Height; y++) {
		BlockRaw block = BLOCK_AIR;
		if (y == 0) block = BLOCK_BEDROCK;
		else if (y < height - 4) block = BLOCK_STONE;
		else if (y < height - 1) block = BLOCK_DIRT;
		else if (y == height - 1) block = top;
		else if (y <= waterLevel) block = BLOCK_STILL_WATER;
		index = World_Pack(localX, y, localZ);
		blocks[index] = block;
	}
}

static void CavFXInf_GenerateWindow(BlockRaw* blocks) {
	int x, z, worldX, worldZ;
	int baseX = cavfx_inf_originChunkX * CAVFX_INF_CHUNK_SIZE;
	int baseZ = cavfx_inf_originChunkZ * CAVFX_INF_CHUNK_SIZE;
	Gen_CurrentState = "Generating infinite chunk window";
	for (z = 0; z < World.Length; z++) {
		for (x = 0; x < World.Width; x++) {
			worldX = baseX + x;
			worldZ = baseZ + z;
			CavFXInf_GenColumn(blocks, x, z, worldX, worldZ);
		}
		Gen_CurrentProgress = (float)z / World.Length;
	}
}

static void CavFXInf_CopyColumn(BlockRaw* dst, BlockRaw* src, int dstX, int dstZ, int srcX, int srcZ) {
	int y;
	for (y = 0; y < World.Height; y++) {
		dst[World_Pack(dstX, y, dstZ)] = src[World_Pack(srcX, y, srcZ)];
	}
}

static void CavFXInf_ShiftWindow(int dxBlocks, int dzBlocks) {
	BlockRaw* oldBlocks;
	int x, z, srcX, srcZ, worldX, worldZ;
	int baseX = cavfx_inf_originChunkX * CAVFX_INF_CHUNK_SIZE;
	int baseZ = cavfx_inf_originChunkZ * CAVFX_INF_CHUNK_SIZE;

	if (!World.Loaded || !World.Blocks) return;
	if (cavfx_inf_shifting) return;
	cavfx_inf_shifting = true;

	/* Copy old window first. Regenerating the entire map every shift caused ugly visible pop-in
	   and patchy terrain artifacts. This keeps old chunks and only generates the new strips. */
	oldBlocks = (BlockRaw*)Mem_TryAlloc(World.Volume, 1);
	if (!oldBlocks) {
		/* Safer fallback: do not rebuild the entire map on allocation failure.
		   Full regeneration here can spike CPU/RAM hard enough to freeze weaker systems. */
		cavfx_inf_shifting = false;
		cavfx_inf_nextShiftTime = Game.Time + 5.0;
		Chat_AddRaw("&cInfinite chunks: shift skipped because memory allocation failed.");
		return;
	}
	Mem_Copy(oldBlocks, World.Blocks, World.Volume);

	for (z = 0; z < World.Length; z++) {
		for (x = 0; x < World.Width; x++) {
			srcX = x + dxBlocks;
			srcZ = z + dzBlocks;

			if (srcX >= 0 && srcX < World.Width && srcZ >= 0 && srcZ < World.Length) {
				CavFXInf_CopyColumn(World.Blocks, oldBlocks, x, z, srcX, srcZ);
			} else {
				worldX = baseX + x;
				worldZ = baseZ + z;
				CavFXInf_GenColumn(World.Blocks, x, z, worldX, worldZ);
			}
		}
	}

	Mem_Free(oldBlocks);

	/* Heavy operations are intentionally done only once per accepted shift.
	   v6 also rate-limits shifts so these cannot spam every frame. */
	Lighting.Refresh();
	MapRenderer_Refresh();
	cavfx_inf_nextShiftTime = Game.Time + CAVFX_INF_SHIFT_COOLDOWN;
	cavfx_inf_shifting = false;
}

void CavFXInfiniteChunks_Disable(void) {
	CavFXInfiniteChunks_Active = false;
	cavfx_inf_loadedOnce = false;
	cavfx_inf_shifting = false;
	cavfx_inf_nextShiftTime = 0.0;
	EnvRenderer_CavFXResetClouds();
}

void CavFXInfiniteChunks_Begin(int seed) {
	EnvRenderer_CavFXResetClouds();
	cavfx_inf_seed = seed;
	cavfx_inf_originChunkX = -(CAVFX_INF_WORLD_SIZE / CAVFX_INF_CHUNK_SIZE) / 2;
	cavfx_inf_originChunkZ = -(CAVFX_INF_WORLD_SIZE / CAVFX_INF_CHUNK_SIZE) / 2;
	CavFXInfiniteChunks_Active = true;
	cavfx_inf_loadedOnce = false;
	cavfx_inf_shifting = false;
	cavfx_inf_nextShiftTime = 0.0;
}

static cc_bool CavFXInf_Prepare(int seed) {
	CavFXInfiniteChunks_Begin(seed);
	return true;
}

static void CavFXInf_Generate(void) {
	CavFXInf_GenerateWindow(Gen_Blocks);
	gen_done = true;
}

const struct MapGenerator CavFXInfiniteGen = {
	CavFXInf_Prepare,
	CavFXInf_Generate
};

static void CavFXInf_ShiftPlayer(int dxBlocks, int dzBlocks) {
	struct LocalPlayer* p = Entities.CurPlayer;
	if (!p) return;
	p->Base.Position.x -= dxBlocks;
	p->Base.Position.z -= dzBlocks;
	p->Spawn.x         -= dxBlocks;
	p->Spawn.z         -= dzBlocks;
	p->Base.prev.pos.x -= dxBlocks; p->Base.next.pos.x -= dxBlocks;
	p->Base.prev.pos.z -= dzBlocks; p->Base.next.pos.z -= dzBlocks;
	EnvRenderer_CavFXShiftClouds(-dxBlocks, -dzBlocks);
}

static void CavFXInf_CheckShift(struct ScheduledTask* task) {
	struct LocalPlayer* p;
	int dxChunks = 0, dzChunks = 0;
	int dxBlocks, dzBlocks;
	if (!CavFXInfiniteChunks_Active || !World.Loaded || !World.Blocks) return;
	if (cavfx_inf_shifting) return;
	if (Game.Time < cavfx_inf_nextShiftTime) return;
	p = Entities.CurPlayer;
	if (!p) return;

	if (!cavfx_inf_loadedOnce) {
		cavfx_inf_loadedOnce = true;
		Chat_AddRaw("&eInfinite chunk window v1 active: terrain streams by recentring the map.");
	}

	if (p->Base.Position.x < CAVFX_INF_BORDER) dxChunks = -CAVFX_INF_SHIFT_CHUNKS;
	if (p->Base.Position.z < CAVFX_INF_BORDER) dzChunks = -CAVFX_INF_SHIFT_CHUNKS;
	if (p->Base.Position.x > World.Width  - CAVFX_INF_BORDER) dxChunks =  CAVFX_INF_SHIFT_CHUNKS;
	if (p->Base.Position.z > World.Length - CAVFX_INF_BORDER) dzChunks =  CAVFX_INF_SHIFT_CHUNKS;
	if (!dxChunks && !dzChunks) return;

	cavfx_inf_originChunkX += dxChunks;
	cavfx_inf_originChunkZ += dzChunks;
	dxBlocks = dxChunks * CAVFX_INF_CHUNK_SIZE;
	dzBlocks = dzChunks * CAVFX_INF_CHUNK_SIZE;
	CavFXInf_ShiftPlayer(dxBlocks, dzBlocks);
	CavFXInf_ShiftWindow(dxBlocks, dzBlocks);
}

static void CavFXInfComp_Init(void) {
	/* Stable v6: check less often and rate-limit accepted shifts. */
	ScheduledTask_Add(0.75, CavFXInf_CheckShift);
}

static void CavFXInfComp_OnNewMap(void) {
	if (!CavFXInfiniteChunks_Active) cavfx_inf_loadedOnce = false;
}

struct IGameComponent CavFXInfiniteChunks_Component = {
	CavFXInfComp_Init,  /* Init */
	NULL,              /* Free */
	NULL,              /* Reset */
	CavFXInfComp_OnNewMap, /* OnNewMap */
	NULL               /* OnNewMapLoaded */
};


/*########################################################################################################################*
*----------------------------------------------------Tree generation------------------------------------------------------*
*#########################################################################################################################*/
BlockRaw* Tree_Blocks;
RNGState* Tree_Rnd;

cc_bool TreeGen_CanGrow(int treeX, int treeY, int treeZ, int treeHeight) {
	int baseHeight = treeHeight - 4;
	int index;
	int x, y, z;

	/* check tree base */
	for (y = treeY; y < treeY + baseHeight; y++) {
		for (z = treeZ - 1; z <= treeZ + 1; z++) {
			for (x = treeX - 1; x <= treeX + 1; x++) {

				if (!World_Contains(x, y, z)) return false;
				index = World_Pack(x, y, z);
				if (Tree_Blocks[index] != BLOCK_AIR) return false;
			}
		}
	}

	/* and also check canopy */
	for (y = treeY + baseHeight; y < treeY + treeHeight; y++) {
		for (z = treeZ - 2; z <= treeZ + 2; z++) {
			for (x = treeX - 2; x <= treeX + 2; x++) {

				if (!World_Contains(x, y, z)) return false;
				index = World_Pack(x, y, z);
				if (Tree_Blocks[index] != BLOCK_AIR) return false;
			}
		}
	}
	return true;
}

#define TreeGen_Place(xVal, yVal, zVal, block)\
coords[count].x = (xVal); coords[count].y = (yVal); coords[count].z = (zVal);\
blocks[count] = block; count++;

int TreeGen_Grow(int treeX, int treeY, int treeZ, int height, IVec3* coords, BlockRaw* blocks) {
	int topStart = treeY + (height - 2);
	int count = 0;
	int xx, zz, x, y, z;

	/* leaves bottom layer */
	for (y = treeY + (height - 4); y < topStart; y++) {
		for (zz = -2; zz <= 2; zz++) {
			for (xx = -2; xx <= 2; xx++) {
				x = treeX + xx; z = treeZ + zz;

				if (Math_AbsI(xx) == 2 && Math_AbsI(zz) == 2) {
					if (Random_Float(Tree_Rnd) >= 0.5f) {
						TreeGen_Place(x, y, z, BLOCK_LEAVES);
					}
				} else {
					TreeGen_Place(x, y, z, BLOCK_LEAVES);
				}
			}
		}
	}

	/* leaves top layer */
	for (; y < treeY + height; y++) {
		for (zz = -1; zz <= 1; zz++) {
			for (xx = -1; xx <= 1; xx++) {
				x = xx + treeX; z = zz + treeZ;

				if (xx == 0 || zz == 0) {
					TreeGen_Place(x, y, z, BLOCK_LEAVES);
				} else if (y == topStart && Random_Float(Tree_Rnd) >= 0.5f) {
					TreeGen_Place(x, y, z, BLOCK_LEAVES);
				}
			}
		}
	}

	/* place trunk */
	for (y = 0; y < height - 1; y++) {
		TreeGen_Place(treeX, treeY + y, treeZ, BLOCK_LOG);
	}

	/* then place dirt */
	TreeGen_Place(treeX, treeY - 1, treeZ, BLOCK_DIRT);

	return count;
}
