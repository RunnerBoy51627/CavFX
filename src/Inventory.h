#ifndef CC_INVENTORY_H
#define CC_INVENTORY_H
#include "Core.h"
#include "BlockID.h"
CC_BEGIN_HEADER


/* Manages inventory hotbar, and ordering of blocks in the inventory menu.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
struct IGameComponent;
extern struct IGameComponent Inventory_Component;

/* Number of blocks in each hotbar */
#define INVENTORY_BLOCKS_PER_HOTBAR 9
/* Number of hotbars that can be selected between */
#define INVENTORY_HOTBARS 9
#define INVENTORY_SURVIVAL_SLOTS 36
#define INVENTORY_CRAFTING_GRID 4
#define HOTBAR_MAX_INDEX (INVENTORY_BLOCKS_PER_HOTBAR - 1)
#define INVENTORY_MAX_STACK 64

CC_VAR extern struct _InventoryData {
	/* Stores the currently bound blocks for all hotbars. */
	BlockID Table[INVENTORY_HOTBARS * INVENTORY_BLOCKS_PER_HOTBAR];
	/* Stack sizes for survival mode slots. 0 means empty/non-survival palette entry. */
	cc_uint8 Counts[INVENTORY_HOTBARS * INVENTORY_BLOCKS_PER_HOTBAR];
	BlockID Craft[INVENTORY_CRAFTING_GRID];
	cc_uint8 CraftCounts[INVENTORY_CRAFTING_GRID];
	BlockID CraftResult;
	cc_uint8 CraftResultCount;
	/* Mapping of indices in inventory menu to block IDs. */
	BlockID Map[BLOCK_COUNT];
	/* Currently selected index within a hotbar. */
	int SelectedIndex;
	/* Currently selected hotbar. */
	int Offset;
	/* Whether the user is allowed to change selected/held block. */
	cc_bool CanChangeSelected;
	/* Number of blocks in each row in inventory menu. */
	cc_uint8 BlocksPerRow;
} Inventory;

/* Gets the block at the nth index in the current hotbar. */
#define Inventory_Get(idx) (Inventory.Table[Inventory.Offset + (idx)])
/* Sets the block at the nth index in the current hotbar. */
#define Inventory_Set(idx, block) Inventory_SetSlot(idx, block, (block) == BLOCK_AIR ? 0 : 1)
/* Gets the stack count at the nth index in the current hotbar. */
#define Inventory_GetCount(idx) (Inventory.Counts[Inventory.Offset + (idx)])
/* Gets the currently selected block. */
#define Inventory_SelectedBlock Inventory_Get(Inventory.SelectedIndex)
#define Inventory_SelectedCount Inventory_GetCount(Inventory.SelectedIndex)

/* Checks if the user can change their selected/held block. */
/* NOTE: Shows a message in chat if they are unable to. */
cc_bool Inventory_CheckChangeSelected(void);
/* Attempts to set the currently selected index in a hotbar. */
void Inventory_SetSelectedIndex(int index);
/* Sets the block and count for the selected hotbar slot. */
void Inventory_SetSlot(int index, BlockID block, int count);
void Inventory_SetRawSlot(int raw, BlockID block, int count);
cc_bool Inventory_CanAddToRawSlot(int raw, BlockID block);
int Inventory_AddToRawSlot(int raw, BlockID block, int count);
/* Attempts to set the currently active hotbar. */
void Inventory_SetHotbarIndex(int index);
void Inventory_SwitchHotbar(void);
/* Attempts to set the block for the selected index in the current hotbar. */
/* NOTE: If another slot is already this block, the selected index is instead changed. */
void Inventory_SetSelectedBlock(BlockID block);
/* Attempts to set the selected block in a user-friendly manner. */
/* e.g. this method tries to replace empty slots before other blocks */
void Inventory_PickBlock(BlockID block);
/* Whether any hotbar slot contains the given block. */
cc_bool Inventory_Contains(BlockID block);
/* Counts how many of the given block are in hotbar/inventory slots. */
int Inventory_Count(BlockID block);
/* Attempts to add the given block to the first empty hotbar slot. */
cc_bool Inventory_TryAdd(BlockID block);
cc_bool Inventory_TryAddCount(BlockID block, int count);
/* Removes the block in the currently selected hotbar slot. */
void Inventory_ConsumeSelected(void);
void Inventory_UpdateCrafting(void);
cc_bool Inventory_CraftOutput(void);
void Inventory_ReturnCrafting(void);
/* Sets all slots to contain their default associated block. */
/* NOTE: The order of default blocks may not be in order of ID. */
void Inventory_ResetMapping(void);

/* Inserts the given block at its default slot in the inventory. */
/* NOTE: Replaces (doesn't move) the block that was at that slot before. */
void Inventory_AddDefault(BlockID block);
/* Removes any slots with the given block from the inventory. */
void Inventory_Remove(BlockID block);

CC_END_HEADER
#endif
