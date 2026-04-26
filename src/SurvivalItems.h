#ifndef CC_SURVIVALITEMS_H
#define CC_SURVIVALITEMS_H
#include "Core.h"
#include "BlockID.h"
CC_BEGIN_HEADER

enum SurvivalItemID {
	SURVIVAL_ITEM_NONE         = BLOCK_AIR,
	SURVIVAL_ITEM_WOOD_PICKAXE = 200,
	SURVIVAL_ITEM_STONE_PICKAXE,
	SURVIVAL_ITEM_IRON_PICKAXE,
	SURVIVAL_ITEM_GOLD_PICKAXE,
	SURVIVAL_ITEM_DIAMOND_PICKAXE
};

static CC_INLINE cc_bool SurvivalItem_IsItem(BlockID item) {
	return item >= SURVIVAL_ITEM_WOOD_PICKAXE && item <= SURVIVAL_ITEM_DIAMOND_PICKAXE;
}

static CC_INLINE TextureLoc SurvivalItem_TextureLoc(BlockID item) {
	switch (item) {
	case SURVIVAL_ITEM_WOOD_PICKAXE:    return 96;
	case SURVIVAL_ITEM_STONE_PICKAXE:   return 97;
	case SURVIVAL_ITEM_IRON_PICKAXE:    return 98;
	case SURVIVAL_ITEM_DIAMOND_PICKAXE: return 99;
	case SURVIVAL_ITEM_GOLD_PICKAXE:    return 100;
	default: return 0;
	}
}

static CC_INLINE const char* SurvivalItem_Name(BlockID item) {
	switch (item) {
	case SURVIVAL_ITEM_WOOD_PICKAXE:    return "Wooden Pickaxe";
	case SURVIVAL_ITEM_STONE_PICKAXE:   return "Stone Pickaxe";
	case SURVIVAL_ITEM_IRON_PICKAXE:    return "Iron Pickaxe";
	case SURVIVAL_ITEM_GOLD_PICKAXE:    return "Golden Pickaxe";
	case SURVIVAL_ITEM_DIAMOND_PICKAXE: return "Diamond Pickaxe";
	default: return "";
	}
}

CC_END_HEADER
#endif
