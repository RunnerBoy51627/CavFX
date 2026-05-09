#include "Inventory.h"
#include "Funcs.h"
#include "Game.h"
#include "Block.h"
#include "Event.h"
#include "Chat.h"
#include "Protocol.h"
#include "SurvivalItems.h"

struct _InventoryData Inventory;
int Inventory_CreativePage = CREATIVE_PAGE_BLOCKS;

static cc_bool Inventory_IsMiscBlock(BlockID block) {
	switch (block) {
	case BLOCK_AIR:
	case BLOCK_SAPLING:
	case BLOCK_DANDELION:
	case BLOCK_ROSE:
	case BLOCK_BROWN_SHROOM:
	case BLOCK_RED_SHROOM:
	case BLOCK_TNT:
	case BLOCK_BOOKSHELF:
	case BLOCK_ROPE:
	case BLOCK_FIRE:
	case BLOCK_CRATE:
		return true;
	default:
		return false;
	}
}

const char* Inventory_CreativePageName(void) {
	switch (Inventory_CreativePage) {
	case CREATIVE_PAGE_ITEMS:  return "Items";
	case CREATIVE_PAGE_MISC:   return "Misc";
	default:                   return "Blocks";
	}
}

void Inventory_SetCreativePage(int page) {
	if (page < 0) page = CREATIVE_PAGE_COUNT - 1;
	if (page >= CREATIVE_PAGE_COUNT) page = 0;
	Inventory_CreativePage = page;
}

void Inventory_NextCreativePage(void) { Inventory_SetCreativePage(Inventory_CreativePage + 1); }
void Inventory_PrevCreativePage(void) { Inventory_SetCreativePage(Inventory_CreativePage - 1); }

cc_bool Inventory_CreativePageAllows(BlockID block) {
	if (block == BLOCK_AIR) return false;
	if (SurvivalItem_IsItem(block)) return Inventory_CreativePage == CREATIVE_PAGE_ITEMS;
	if (Inventory_IsMiscBlock(block)) return Inventory_CreativePage == CREATIVE_PAGE_MISC;
	return Inventory_CreativePage == CREATIVE_PAGE_BLOCKS;
}

cc_bool Inventory_CheckChangeSelected(void) {
	if (!Inventory.CanChangeSelected) {
		Chat_AddRaw("&cThe server has forbidden you from changing your held block.");
		return false;
	}
	return true;
}

void Inventory_SetSelectedIndex(int index) {
	if (!Inventory_CheckChangeSelected()) return;
	if (index < 0) index = 0;
	if (index >= INVENTORY_BLOCKS_PER_HOTBAR) index = INVENTORY_BLOCKS_PER_HOTBAR - 1;
	Inventory.SelectedIndex = index;
	Event_RaiseVoid(&UserEvents.HeldBlockChanged);
}

void Inventory_SetSlot(int index, BlockID block, int count) {
	if (index < 0 || index >= INVENTORY_BLOCKS_PER_HOTBAR) return;
	Inventory_SetRawSlot(Inventory.Offset + index, block, count);
}

void Inventory_SetRawSlot(int raw, BlockID block, int count) {
	if (raw < 0 || raw >= Array_Elems(Inventory.Table)) return;
	if (Game_SurvivalMode && raw >= INVENTORY_SURVIVAL_SLOTS) return;
	if (block == BLOCK_AIR || count <= 0) {
		Inventory.Table[raw]  = BLOCK_AIR;
		Inventory.Counts[raw] = 0;
	} else {
		Inventory.Table[raw]  = block;
		Inventory.Counts[raw] = (cc_uint8)min(count, INVENTORY_MAX_STACK);
	}
}

cc_bool Inventory_CanAddToRawSlot(int raw, BlockID block) {
	if (raw < 0 || raw >= INVENTORY_SURVIVAL_SLOTS) return false;
	if (block == BLOCK_AIR) return false;
	return Inventory.Table[raw] == BLOCK_AIR || Inventory.Table[raw] == block;
}

int Inventory_AddToRawSlot(int raw, BlockID block, int count) {
	int space, amount;
	if (!Inventory_CanAddToRawSlot(raw, block) || count <= 0) return 0;

	if (Inventory.Table[raw] == BLOCK_AIR) {
		amount = min(INVENTORY_MAX_STACK, count);
		Inventory_SetRawSlot(raw, block, amount);
		return amount;
	}

	space = INVENTORY_MAX_STACK - Inventory.Counts[raw];
	amount = min(space, count);
	Inventory.Counts[raw] += (cc_uint8)amount;
	return amount;
}

void Inventory_SetHotbarIndex(int index) {
	if (!Inventory_CheckChangeSelected() || Game_ClassicMode || Game_SurvivalMode) return;
	Inventory.Offset = index * INVENTORY_BLOCKS_PER_HOTBAR;
	Event_RaiseVoid(&UserEvents.HeldBlockChanged);
}

void Inventory_SwitchHotbar(void) {
	int index = Inventory.Offset == 0 ? 1 : 0;
	Inventory_SetHotbarIndex(index);
}

void Inventory_SetSelectedBlock(BlockID block) {
	int i;
	if (!Inventory_CheckChangeSelected()) return;

	/* Swap with currently selected block if given block is already in the hotbar */
	for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
		if (Inventory_Get(i) != block) continue;
		Inventory_SetSlot(i, Inventory_SelectedBlock, Inventory_SelectedCount);
		break;
	}

	Inventory_Set(Inventory.SelectedIndex, block);
	Event_RaiseVoid(&UserEvents.HeldBlockChanged);
	CPE_SendNotifyAction(NOTIFY_ACTION_BLOCK_LIST_SELECTED, block);
}

void Inventory_PickBlock(BlockID block) {
	int i, raw, target;
	BlockID oldBlock;
	cc_uint8 oldCount;
	if (!Inventory_CheckChangeSelected() || Inventory_SelectedBlock == block) return;

	if (Game_SurvivalMode) {
		for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
			if (Inventory.Table[i] == block && Inventory.Counts[i]) {
				Inventory_SetSelectedIndex(i); return;
			}
		}

		raw = -1;
		for (i = INVENTORY_BLOCKS_PER_HOTBAR; i < INVENTORY_SURVIVAL_SLOTS; i++) {
			if (Inventory.Table[i] == block && Inventory.Counts[i]) { raw = i; break; }
		}
		if (raw < 0) return;

		target = Inventory.SelectedIndex;
		if (Inventory_SelectedBlock != BLOCK_AIR) {
			for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
				if (Inventory.Table[i] == BLOCK_AIR) { target = i; break; }
			}
		}

		oldBlock = Inventory.Table[target];
		oldCount = Inventory.Counts[target];
		Inventory.Table[target] = Inventory.Table[raw];
		Inventory.Counts[target] = Inventory.Counts[raw];
		Inventory.Table[raw] = oldBlock;
		Inventory.Counts[raw] = oldCount;
		Inventory_SetSelectedIndex(target);
		return;
	}

	/* Vanilla classic client doesn't let you select these blocks */
	if (Game_PureClassic) {
		if (block == BLOCK_GRASS)       block = BLOCK_DIRT;
		if (block == BLOCK_DOUBLE_SLAB) block = BLOCK_SLAB;
	}

	/* Try to replace same block */
	for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
		if (Inventory_Get(i) != block) continue;
		Inventory_SetSelectedIndex(i); return;
	}

	if (AutoRotate_Enabled) {
		/* Try to replace existing autorotate variant */
		for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
			if (AutoRotate_BlocksShareGroup(Inventory_Get(i), block)) {
				Inventory_SetSelectedIndex(i);
				Inventory_SetSelectedBlock(block);
				return;
			}
		}
	}

	/* Is the currently selected block an empty slot? */
	if (Inventory_SelectedBlock == BLOCK_AIR) {
		Inventory_SetSelectedBlock(block); return;
	}

	/* Try to replace empty slots */
	for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
		if (Inventory_Get(i) != BLOCK_AIR) continue;
		Inventory_Set(i, block);
		Inventory_SetSelectedIndex(i); return;
	}

	/* Finally, replace the currently selected block */
	Inventory_SetSelectedBlock(block);
}

cc_bool Inventory_Contains(BlockID block) {
	int i;
	int slots = Game_SurvivalMode ? INVENTORY_SURVIVAL_SLOTS : INVENTORY_BLOCKS_PER_HOTBAR;
	for (i = 0; i < slots; i++) {
		if (Inventory.Table[i] == block && (!Game_SurvivalMode || Inventory.Counts[i] > 0)) return true;
	}
	return false;
}

int Inventory_Count(BlockID block) {
	int i, count = 0;
	int slots = Game_SurvivalMode ? INVENTORY_SURVIVAL_SLOTS : INVENTORY_HOTBARS * INVENTORY_BLOCKS_PER_HOTBAR;
	for (i = 0; i < slots; i++) {
		if (Inventory.Table[i] != block) continue;
		count += Game_SurvivalMode ? Inventory.Counts[i] : 1;
	}
	return count;
}

cc_bool Inventory_TryAddCount(BlockID block, int count) {
	int i, amount, space;
	int capacity = 0;
	if (block == BLOCK_AIR || count <= 0) return true;

	if (Game_SurvivalMode) {
		for (i = 0; i < INVENTORY_SURVIVAL_SLOTS; i++) {
			if (Inventory.Table[i] == block) {
				capacity += INVENTORY_MAX_STACK - Inventory.Counts[i];
			} else if (Inventory.Table[i] == BLOCK_AIR) {
				capacity += INVENTORY_MAX_STACK;
			}
			if (capacity >= count) break;
		}
		if (capacity < count) return false;

		for (i = 0; i < INVENTORY_SURVIVAL_SLOTS; i++) {
			if (Inventory.Table[i] != block || Inventory.Counts[i] >= INVENTORY_MAX_STACK) continue;
			space  = INVENTORY_MAX_STACK - Inventory.Counts[i];
			amount = min(space, count);
			Inventory.Counts[i] += (cc_uint8)amount;
			count -= amount;
			Event_RaiseVoid(&UserEvents.HeldBlockChanged);
			if (!count) return true;
		}

		for (i = 0; i < INVENTORY_SURVIVAL_SLOTS; i++) {
			if (Inventory.Table[i] != BLOCK_AIR) continue;
			amount = min(INVENTORY_MAX_STACK, count);
			Inventory_SetRawSlot(i, block, amount);
			if (Inventory_SelectedBlock == BLOCK_AIR && i < INVENTORY_BLOCKS_PER_HOTBAR) Inventory_SetSelectedIndex(i);
			Event_RaiseVoid(&UserEvents.HeldBlockChanged);
			count -= amount;
			if (!count) return true;
		}
		return false;
	}

	for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
		if (Inventory_Get(i) != BLOCK_AIR) continue;
		Inventory_Set(i, block);
		if (Inventory_SelectedBlock == BLOCK_AIR) Inventory_SetSelectedIndex(i);
		Event_RaiseVoid(&UserEvents.HeldBlockChanged);
		return true;
	}
	return false;
}

cc_bool Inventory_TryAdd(BlockID block) {
	return Inventory_TryAddCount(block, 1);
}

void Inventory_ConsumeSelected(void) {
	if (Inventory_SelectedBlock == BLOCK_AIR) return;
	if (Game_SurvivalMode && Inventory_SelectedCount > 1) {
		Inventory.Counts[Inventory.Offset + Inventory.SelectedIndex]--;
	} else {
		Inventory_Set(Inventory.SelectedIndex, BLOCK_AIR);
	}
	Event_RaiseVoid(&UserEvents.HeldBlockChanged);
}

static int Inventory_CraftingNonEmpty(void) {
	int i, count = 0;
	for (i = 0; i < INVENTORY_CRAFTING_GRID; i++) {
		if (Inventory.Craft[i] != BLOCK_AIR && Inventory.CraftCounts[i]) count++;
	}
	return count;
}

static int Inventory_CraftingSlots(void) {
	int width = Inventory.CraftingWidth ? Inventory.CraftingWidth : 2;
	return width * width;
}

void Inventory_SetCraftingGridWidth(int width) {
	int i;
	if (width != 3) width = 2;
	Inventory.CraftingWidth = (cc_uint8)width;

	/* Switching from table crafting back to inventory crafting must not leave
	   invisible ingredients sitting in slots 4..8. Return them first. */
	if (width == 2) {
		for (i = 4; i < INVENTORY_CRAFTING_GRID; i++) {
			if (Inventory.Craft[i] != BLOCK_AIR && Inventory.CraftCounts[i]) {
				Inventory_TryAddCount(Inventory.Craft[i], Inventory.CraftCounts[i]);
			}
			Inventory.Craft[i] = BLOCK_AIR;
			Inventory.CraftCounts[i] = 0;
		}
	}
	Inventory_UpdateCrafting();
}

static cc_bool Inventory_CraftSlotEmpty(int slot) {
	return Inventory.Craft[slot] == BLOCK_AIR || !Inventory.CraftCounts[slot];
}

static cc_bool Inventory_CraftSlotHas(int slot, BlockID block) {
	return Inventory.Craft[slot] == block && Inventory.CraftCounts[slot];
}

static cc_bool Inventory_CraftOnlyUsesSlots(int slots) {
	int i;
	for (i = slots; i < INVENTORY_CRAFTING_GRID; i++) {
		if (!Inventory_CraftSlotEmpty(i)) return false;
	}
	return true;
}

static cc_bool Inventory_CraftExact2(BlockID a, BlockID b, BlockID c, BlockID d) {
	return Inventory_CraftOnlyUsesSlots(4)
		&& (a == BLOCK_AIR ? Inventory_CraftSlotEmpty(0) : Inventory_CraftSlotHas(0, a))
		&& (b == BLOCK_AIR ? Inventory_CraftSlotEmpty(1) : Inventory_CraftSlotHas(1, b))
		&& (c == BLOCK_AIR ? Inventory_CraftSlotEmpty(2) : Inventory_CraftSlotHas(2, c))
		&& (d == BLOCK_AIR ? Inventory_CraftSlotEmpty(3) : Inventory_CraftSlotHas(3, d));
}

static cc_bool Inventory_CraftExact3(BlockID a, BlockID b, BlockID c, BlockID d, BlockID e, BlockID f, BlockID g, BlockID h, BlockID i) {
	return Inventory.CraftingWidth == 3
		&& (a == BLOCK_AIR ? Inventory_CraftSlotEmpty(0) : Inventory_CraftSlotHas(0, a))
		&& (b == BLOCK_AIR ? Inventory_CraftSlotEmpty(1) : Inventory_CraftSlotHas(1, b))
		&& (c == BLOCK_AIR ? Inventory_CraftSlotEmpty(2) : Inventory_CraftSlotHas(2, c))
		&& (d == BLOCK_AIR ? Inventory_CraftSlotEmpty(3) : Inventory_CraftSlotHas(3, d))
		&& (e == BLOCK_AIR ? Inventory_CraftSlotEmpty(4) : Inventory_CraftSlotHas(4, e))
		&& (f == BLOCK_AIR ? Inventory_CraftSlotEmpty(5) : Inventory_CraftSlotHas(5, f))
		&& (g == BLOCK_AIR ? Inventory_CraftSlotEmpty(6) : Inventory_CraftSlotHas(6, g))
		&& (h == BLOCK_AIR ? Inventory_CraftSlotEmpty(7) : Inventory_CraftSlotHas(7, h))
		&& (i == BLOCK_AIR ? Inventory_CraftSlotEmpty(8) : Inventory_CraftSlotHas(8, i));
}

static cc_bool Inventory_CraftSingle(BlockID block) {
	int i, slots = Inventory_CraftingSlots();
	for (i = 0; i < slots; i++) {
		if (Inventory_CraftSlotHas(i, block) && Inventory_CraftingNonEmpty() == 1) return true;
	}
	return false;
}

static cc_bool Inventory_CraftVerticalPair(BlockID block) {
	if (Inventory.CraftingWidth == 2) {
		return Inventory_CraftExact2(block, BLOCK_AIR, block, BLOCK_AIR)
			|| Inventory_CraftExact2(BLOCK_AIR, block, BLOCK_AIR, block);
	}
	return Inventory_CraftExact3(block, BLOCK_AIR, BLOCK_AIR, block, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR)
		|| Inventory_CraftExact3(BLOCK_AIR, block, BLOCK_AIR, BLOCK_AIR, block, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR)
		|| Inventory_CraftExact3(BLOCK_AIR, BLOCK_AIR, block, BLOCK_AIR, BLOCK_AIR, block, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR)
		|| Inventory_CraftExact3(BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, block, BLOCK_AIR, BLOCK_AIR, block, BLOCK_AIR, BLOCK_AIR)
		|| Inventory_CraftExact3(BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, block, BLOCK_AIR, BLOCK_AIR, block, BLOCK_AIR)
		|| Inventory_CraftExact3(BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, block, BLOCK_AIR, BLOCK_AIR, block);
}

static cc_bool Inventory_Craft2x2Square(BlockID block) {
	if (Inventory.CraftingWidth == 2) {
		return Inventory_CraftExact2(block, block, block, block);
	}
	return Inventory_CraftExact3(block, block, BLOCK_AIR, block, block, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR)
		|| Inventory_CraftExact3(BLOCK_AIR, block, block, BLOCK_AIR, block, block, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR)
		|| Inventory_CraftExact3(BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, block, block, BLOCK_AIR, block, block, BLOCK_AIR)
		|| Inventory_CraftExact3(BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, block, block, BLOCK_AIR, block, block);
}

static cc_bool Inventory_CraftSword(BlockID material) {
	return Inventory.CraftingWidth == 3 && (
		Inventory_CraftExact3(material, BLOCK_AIR, BLOCK_AIR, material, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR, BLOCK_AIR) ||
		Inventory_CraftExact3(BLOCK_AIR, material, BLOCK_AIR, BLOCK_AIR, material, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR) ||
		Inventory_CraftExact3(BLOCK_AIR, BLOCK_AIR, material, BLOCK_AIR, BLOCK_AIR, material, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK));
}

static cc_bool Inventory_CraftPickaxe(BlockID material) {
	return Inventory_CraftExact3(material, material, material, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR);
}

static cc_bool Inventory_CraftShovel(BlockID material) {
	return Inventory.CraftingWidth == 3 && (
		Inventory_CraftExact3(material, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR, BLOCK_AIR) ||
		Inventory_CraftExact3(BLOCK_AIR, material, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR) ||
		Inventory_CraftExact3(BLOCK_AIR, BLOCK_AIR, material, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK));
}

static cc_bool Inventory_CraftAxe(BlockID material) {
	return Inventory.CraftingWidth == 3 && (
		Inventory_CraftExact3(material, material, BLOCK_AIR, material, SURVIVAL_ITEM_STICK, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR) ||
		Inventory_CraftExact3(BLOCK_AIR, material, material, BLOCK_AIR, SURVIVAL_ITEM_STICK, material, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR));
}

static cc_bool Inventory_CraftHoe(BlockID material) {
	return Inventory.CraftingWidth == 3 && (
		Inventory_CraftExact3(material, material, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR) ||
		Inventory_CraftExact3(BLOCK_AIR, material, material, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR, BLOCK_AIR, SURVIVAL_ITEM_STICK, BLOCK_AIR));
}

static void Inventory_SetCraftResult(BlockID result, int count) {
	Inventory.CraftResult = result;
	Inventory.CraftResultCount = (cc_uint8)count;
}

void Inventory_UpdateCrafting(void) {
	Inventory.CraftResult = BLOCK_AIR;
	Inventory.CraftResultCount = 0;
	if (!Inventory.CraftingWidth) Inventory.CraftingWidth = 2;

	/* 1 log -> 4 planks */
	if (Inventory_CraftSingle(BLOCK_LOG)) {
		Inventory_SetCraftResult(BLOCK_WOOD, 4);
		return;
	}

	/* 2 planks vertically -> 4 sticks */
	if (Inventory_CraftVerticalPair(BLOCK_WOOD)) {
		Inventory_SetCraftResult(SURVIVAL_ITEM_STICK, 4);
		return;
	}

	/* 4 planks in a square -> crafting table */
	if (Inventory_Craft2x2Square(BLOCK_WOOD)) {
		Inventory_SetCraftResult(BLOCK_CRAFTING_TABLE, 1);
		return;
	}

	/* Real 3x3 tools. These only work from the crafting table UI. */
	if (Inventory_CraftPickaxe(BLOCK_WOOD))   { Inventory_SetCraftResult(SURVIVAL_ITEM_WOOD_PICKAXE, 1); return; }
	if (Inventory_CraftPickaxe(BLOCK_COBBLE)) { Inventory_SetCraftResult(SURVIVAL_ITEM_STONE_PICKAXE, 1); return; }
	if (Inventory_CraftPickaxe(BLOCK_IRON))   { Inventory_SetCraftResult(SURVIVAL_ITEM_IRON_PICKAXE, 1); return; }
	if (Inventory_CraftPickaxe(BLOCK_GOLD))   { Inventory_SetCraftResult(SURVIVAL_ITEM_GOLD_PICKAXE, 1); return; }
	if (Inventory_CraftPickaxe(BLOCK_CYAN))   { Inventory_SetCraftResult(SURVIVAL_ITEM_DIAMOND_PICKAXE, 1); return; }

	if (Inventory_CraftSword(BLOCK_WOOD))     { Inventory_SetCraftResult(SURVIVAL_ITEM_WOOD_SWORD, 1); return; }
	if (Inventory_CraftSword(BLOCK_COBBLE))   { Inventory_SetCraftResult(SURVIVAL_ITEM_STONE_SWORD, 1); return; }
	if (Inventory_CraftSword(BLOCK_IRON))     { Inventory_SetCraftResult(SURVIVAL_ITEM_IRON_SWORD, 1); return; }
	if (Inventory_CraftSword(BLOCK_GOLD))     { Inventory_SetCraftResult(SURVIVAL_ITEM_GOLD_SWORD, 1); return; }
	if (Inventory_CraftSword(BLOCK_CYAN))     { Inventory_SetCraftResult(SURVIVAL_ITEM_DIAMOND_SWORD, 1); return; }

	if (Inventory_CraftAxe(BLOCK_WOOD))       { Inventory_SetCraftResult(SURVIVAL_ITEM_WOOD_AXE, 1); return; }
	if (Inventory_CraftAxe(BLOCK_COBBLE))     { Inventory_SetCraftResult(SURVIVAL_ITEM_STONE_AXE, 1); return; }
	if (Inventory_CraftAxe(BLOCK_IRON))       { Inventory_SetCraftResult(SURVIVAL_ITEM_IRON_AXE, 1); return; }
	if (Inventory_CraftAxe(BLOCK_GOLD))       { Inventory_SetCraftResult(SURVIVAL_ITEM_GOLD_AXE, 1); return; }
	if (Inventory_CraftAxe(BLOCK_CYAN))       { Inventory_SetCraftResult(SURVIVAL_ITEM_DIAMOND_AXE, 1); return; }

	if (Inventory_CraftShovel(BLOCK_WOOD))    { Inventory_SetCraftResult(SURVIVAL_ITEM_WOOD_SHOVEL, 1); return; }
	if (Inventory_CraftShovel(BLOCK_COBBLE))  { Inventory_SetCraftResult(SURVIVAL_ITEM_STONE_SHOVEL, 1); return; }
	if (Inventory_CraftShovel(BLOCK_IRON))    { Inventory_SetCraftResult(SURVIVAL_ITEM_IRON_SHOVEL, 1); return; }
	if (Inventory_CraftShovel(BLOCK_GOLD))    { Inventory_SetCraftResult(SURVIVAL_ITEM_GOLD_SHOVEL, 1); return; }
	if (Inventory_CraftShovel(BLOCK_CYAN))    { Inventory_SetCraftResult(SURVIVAL_ITEM_DIAMOND_SHOVEL, 1); return; }

	if (Inventory_CraftHoe(BLOCK_WOOD))       { Inventory_SetCraftResult(SURVIVAL_ITEM_WOOD_HOE, 1); return; }
	if (Inventory_CraftHoe(BLOCK_COBBLE))     { Inventory_SetCraftResult(SURVIVAL_ITEM_STONE_HOE, 1); return; }
	if (Inventory_CraftHoe(BLOCK_IRON))       { Inventory_SetCraftResult(SURVIVAL_ITEM_IRON_HOE, 1); return; }
	if (Inventory_CraftHoe(BLOCK_GOLD))       { Inventory_SetCraftResult(SURVIVAL_ITEM_GOLD_HOE, 1); return; }
	if (Inventory_CraftHoe(BLOCK_CYAN))       { Inventory_SetCraftResult(SURVIVAL_ITEM_DIAMOND_HOE, 1); return; }
}

cc_bool Inventory_CraftOutput(void) {
	int i;
	if (!Inventory.CraftResult || !Inventory.CraftResultCount) return false;
	if (!Inventory_TryAddCount(Inventory.CraftResult, Inventory.CraftResultCount)) return false;

	for (i = 0; i < INVENTORY_CRAFTING_GRID; i++) {
		if (!Inventory.CraftCounts[i]) continue;
		Inventory.CraftCounts[i]--;
		if (!Inventory.CraftCounts[i]) Inventory.Craft[i] = BLOCK_AIR;
	}
	Inventory_UpdateCrafting();
	return true;
}

void Inventory_ReturnCrafting(void) {
	int i;
	for (i = 0; i < INVENTORY_CRAFTING_GRID; i++) {
		if (Inventory.Craft[i] != BLOCK_AIR && Inventory.CraftCounts[i]) {
			if (!Inventory_TryAddCount(Inventory.Craft[i], Inventory.CraftCounts[i])) continue;
		}
		Inventory.Craft[i] = BLOCK_AIR;
		Inventory.CraftCounts[i] = 0;
	}
	Inventory_UpdateCrafting();
}

/* Returns default block that should go in the given inventory slot */
static BlockID DefaultMapping(int slot) {
	if (Game_SurvivalMode) return BLOCK_AIR;

	if (Game_ClassicMode) {
		if (slot < Game_Version.InventorySize) return Game_Version.Inventory[slot];
	} else if (slot < Game_Version.MaxCoreBlock) {
		return (BlockID)(slot + 1);
	}
	return BLOCK_AIR;
}

void Inventory_ResetMapping(void) {
	int slot;
	for (slot = 0; slot < Array_Elems(Inventory.Map); slot++) {
		Inventory.Map[slot] = DefaultMapping(slot);
	}

	if (Game_SurvivalMode) return;
	Inventory.Map[200] = SURVIVAL_ITEM_WOOD_PICKAXE;
	Inventory.Map[201] = SURVIVAL_ITEM_STONE_PICKAXE;
	Inventory.Map[202] = SURVIVAL_ITEM_IRON_PICKAXE;
	Inventory.Map[203] = SURVIVAL_ITEM_GOLD_PICKAXE;
	Inventory.Map[204] = SURVIVAL_ITEM_DIAMOND_PICKAXE;
}

void Inventory_AddDefault(BlockID block) {
	int slot;
	if (block > BLOCK_MAX_CPE) {
		Inventory.Map[block - 1] = block; return;
	}
	
	for (slot = 0; slot < BLOCK_MAX_CPE; slot++) {
		if (DefaultMapping(slot) != block) continue;
		Inventory.Map[slot] = block;
		return;
	}
}

void Inventory_Remove(BlockID block) {
	int slot;
	for (slot = 0; slot < Array_Elems(Inventory.Map); slot++) {
		if (Inventory.Map[slot] == block) Inventory.Map[slot] = BLOCK_AIR;
	}
}


/*########################################################################################################################*
*--------------------------------------------------Inventory component----------------------------------------------------*
*#########################################################################################################################*/
static void OnReset(void) {
	Inventory_ResetMapping();
	Inventory.CanChangeSelected = true;
	if (Game_SurvivalMode) {
		Inventory.Offset = 0;
		if (Inventory.SelectedIndex < 0 || Inventory.SelectedIndex >= INVENTORY_BLOCKS_PER_HOTBAR) {
			Inventory.SelectedIndex = 0;
		}
	}
}

static void OnInit(void) {
	int i;
	BlockID* inv = Inventory.Table;
	OnReset();
	Inventory.Offset = 0;
	if (Inventory.SelectedIndex < 0 || Inventory.SelectedIndex >= INVENTORY_BLOCKS_PER_HOTBAR) {
		Inventory.SelectedIndex = 0;
	}
	Inventory.BlocksPerRow = Game_Version.BlocksPerRow;
	
	for (i = 0; i < Array_Elems(Inventory.Table); i++) {
		inv[i] = BLOCK_AIR;
		Inventory.Counts[i] = 0;
	}
	for (i = 0; i < INVENTORY_CRAFTING_GRID; i++) {
		Inventory.Craft[i] = BLOCK_AIR;
		Inventory.CraftCounts[i] = 0;
	}
	Inventory_UpdateCrafting();
	if (Game_SurvivalMode) return;

	for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
		inv[i] = Game_Version.Hotbar[i];
	}
}

struct IGameComponent Inventory_Component = {
	OnInit,  /* Init  */
	NULL,    /* Free  */
	OnReset, /* Reset */
};
