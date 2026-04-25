#include "Inventory.h"
#include "Funcs.h"
#include "Game.h"
#include "Block.h"
#include "Event.h"
#include "Chat.h"
#include "Protocol.h"

struct _InventoryData Inventory;

cc_bool Inventory_CheckChangeSelected(void) {
	if (!Inventory.CanChangeSelected) {
		Chat_AddRaw("&cThe server has forbidden you from changing your held block.");
		return false;
	}
	return true;
}

void Inventory_SetSelectedIndex(int index) {
	if (!Inventory_CheckChangeSelected()) return;
	Inventory.SelectedIndex = index;
	Event_RaiseVoid(&UserEvents.HeldBlockChanged);
}

void Inventory_SetSlot(int index, BlockID block, int count) {
	Inventory_SetRawSlot(Inventory.Offset + index, block, count);
}

void Inventory_SetRawSlot(int raw, BlockID block, int count) {
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

void Inventory_UpdateCrafting(void) {
	int i, nonEmpty = Inventory_CraftingNonEmpty();
	Inventory.CraftResult = BLOCK_AIR;
	Inventory.CraftResultCount = 0;

	if (nonEmpty != 1) return;
	for (i = 0; i < INVENTORY_CRAFTING_GRID; i++) {
		if (Inventory.Craft[i] != BLOCK_LOG || !Inventory.CraftCounts[i]) continue;
		Inventory.CraftResult = BLOCK_WOOD;
		Inventory.CraftResultCount = 4;
		return;
	}
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
}

static void OnInit(void) {
	int i;
	BlockID* inv = Inventory.Table;
	OnReset();
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
