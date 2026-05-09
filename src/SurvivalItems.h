#ifndef CC_SURVIVALITEMS_H
#define CC_SURVIVALITEMS_H
#include "Core.h"
#include "BlockID.h"
CC_BEGIN_HEADER

/* CavFX survival item IDs.
   These are fake BlockIDs above normal block range so inventory/widgets can carry them. */
enum SurvivalItemID {
	SURVIVAL_ITEM_NONE = BLOCK_AIR,

	/* Pickaxes - existing IDs kept for save compatibility */
	SURVIVAL_ITEM_WOOD_PICKAXE = 200,
	SURVIVAL_ITEM_STONE_PICKAXE,
	SURVIVAL_ITEM_IRON_PICKAXE,
	SURVIVAL_ITEM_GOLD_PICKAXE,
	SURVIVAL_ITEM_DIAMOND_PICKAXE,

	/* Weapons */
	SURVIVAL_ITEM_WOOD_SWORD,
	SURVIVAL_ITEM_STONE_SWORD,
	SURVIVAL_ITEM_IRON_SWORD,
	SURVIVAL_ITEM_GOLD_SWORD,
	SURVIVAL_ITEM_DIAMOND_SWORD,

	/* Axes */
	SURVIVAL_ITEM_WOOD_AXE,
	SURVIVAL_ITEM_STONE_AXE,
	SURVIVAL_ITEM_IRON_AXE,
	SURVIVAL_ITEM_GOLD_AXE,
	SURVIVAL_ITEM_DIAMOND_AXE,

	/* Shovels */
	SURVIVAL_ITEM_WOOD_SHOVEL,
	SURVIVAL_ITEM_STONE_SHOVEL,
	SURVIVAL_ITEM_IRON_SHOVEL,
	SURVIVAL_ITEM_GOLD_SHOVEL,
	SURVIVAL_ITEM_DIAMOND_SHOVEL,

	/* Hoes */
	SURVIVAL_ITEM_WOOD_HOE,
	SURVIVAL_ITEM_STONE_HOE,
	SURVIVAL_ITEM_IRON_HOE,
	SURVIVAL_ITEM_GOLD_HOE,
	SURVIVAL_ITEM_DIAMOND_HOE,

	SURVIVAL_ITEM_STICK,
	SURVIVAL_ITEM_COAL,
	SURVIVAL_ITEM_IRON_INGOT,
	SURVIVAL_ITEM_GOLD_INGOT,
	SURVIVAL_ITEM_DIAMOND,
	SURVIVAL_ITEM_RAW_PORKCHOP
};

#define SURVIVAL_ITEM_FIRST SURVIVAL_ITEM_WOOD_PICKAXE
#define SURVIVAL_ITEM_LAST  SURVIVAL_ITEM_RAW_PORKCHOP

static CC_INLINE cc_bool SurvivalItem_IsItem(BlockID item) {
	return item >= SURVIVAL_ITEM_FIRST && item <= SURVIVAL_ITEM_LAST;
}

static CC_INLINE cc_bool SurvivalItem_IsWeapon(BlockID item) {
	return item >= SURVIVAL_ITEM_WOOD_SWORD && item <= SURVIVAL_ITEM_DIAMOND_SWORD;
}

static CC_INLINE cc_bool SurvivalItem_IsTool(BlockID item) {
	return item >= SURVIVAL_ITEM_WOOD_PICKAXE && item <= SURVIVAL_ITEM_DIAMOND_HOE;
}

static CC_INLINE int SurvivalItem_AttackDamage(BlockID item) {
	switch (item) {
	case SURVIVAL_ITEM_WOOD_SWORD:    return 4;
	case SURVIVAL_ITEM_STONE_SWORD:   return 5;
	case SURVIVAL_ITEM_IRON_SWORD:    return 6;
	case SURVIVAL_ITEM_GOLD_SWORD:    return 4;
	case SURVIVAL_ITEM_DIAMOND_SWORD: return 7;

	case SURVIVAL_ITEM_WOOD_AXE:      return 3;
	case SURVIVAL_ITEM_STONE_AXE:     return 4;
	case SURVIVAL_ITEM_IRON_AXE:      return 5;
	case SURVIVAL_ITEM_GOLD_AXE:      return 3;
	case SURVIVAL_ITEM_DIAMOND_AXE:   return 6;
	default: return 1;
	}
}

static CC_INLINE float SurvivalItem_Knockback(BlockID item) {
	switch (item) {
	case SURVIVAL_ITEM_WOOD_SWORD:
	case SURVIVAL_ITEM_STONE_SWORD:
	case SURVIVAL_ITEM_IRON_SWORD:
	case SURVIVAL_ITEM_GOLD_SWORD:
	case SURVIVAL_ITEM_DIAMOND_SWORD:
		return 0.42f;
	case SURVIVAL_ITEM_WOOD_AXE:
	case SURVIVAL_ITEM_STONE_AXE:
	case SURVIVAL_ITEM_IRON_AXE:
	case SURVIVAL_ITEM_GOLD_AXE:
	case SURVIVAL_ITEM_DIAMOND_AXE:
		return 0.55f;
	default: return 0.35f;
	}
}

static CC_INLINE TextureLoc SurvivalItem_TextureLoc(BlockID item) {
	switch (item) {
	/* Items.png texture IDs, 16x16 atlas.
	   IMPORTANT: these are texture indices, NOT SurvivalItemID values. */
	case SURVIVAL_ITEM_WOOD_SWORD:      return 64;
	case SURVIVAL_ITEM_STONE_SWORD:     return 65;
	case SURVIVAL_ITEM_IRON_SWORD:      return 66;
	case SURVIVAL_ITEM_DIAMOND_SWORD:   return 67;
	case SURVIVAL_ITEM_GOLD_SWORD:      return 68;

	case SURVIVAL_ITEM_WOOD_SHOVEL:     return 80;
	case SURVIVAL_ITEM_STONE_SHOVEL:    return 81;
	case SURVIVAL_ITEM_IRON_SHOVEL:     return 82;
	case SURVIVAL_ITEM_DIAMOND_SHOVEL:  return 83;
	case SURVIVAL_ITEM_GOLD_SHOVEL:     return 84;

	case SURVIVAL_ITEM_WOOD_PICKAXE:    return 96;
	case SURVIVAL_ITEM_STONE_PICKAXE:   return 97;
	case SURVIVAL_ITEM_IRON_PICKAXE:    return 98;
	case SURVIVAL_ITEM_DIAMOND_PICKAXE: return 99;
	case SURVIVAL_ITEM_GOLD_PICKAXE:    return 100;

	case SURVIVAL_ITEM_WOOD_AXE:        return 112;
	case SURVIVAL_ITEM_STONE_AXE:       return 113;
	case SURVIVAL_ITEM_IRON_AXE:        return 114;
	case SURVIVAL_ITEM_DIAMOND_AXE:     return 115;
	case SURVIVAL_ITEM_GOLD_AXE:        return 116;

	case SURVIVAL_ITEM_WOOD_HOE:        return 128;
	case SURVIVAL_ITEM_STONE_HOE:       return 129;
	case SURVIVAL_ITEM_IRON_HOE:        return 130;
	case SURVIVAL_ITEM_DIAMOND_HOE:     return 131;
	case SURVIVAL_ITEM_GOLD_HOE:        return 132;

	case SURVIVAL_ITEM_STICK:           return 53;
	case SURVIVAL_ITEM_COAL:            return 38;
	case SURVIVAL_ITEM_IRON_INGOT:      return 23;
	case SURVIVAL_ITEM_GOLD_INGOT:      return 39;
	case SURVIVAL_ITEM_DIAMOND:         return 55;
	case SURVIVAL_ITEM_RAW_PORKCHOP:    return 87;
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

	case SURVIVAL_ITEM_WOOD_SWORD:      return "Wooden Sword";
	case SURVIVAL_ITEM_STONE_SWORD:     return "Stone Sword";
	case SURVIVAL_ITEM_IRON_SWORD:      return "Iron Sword";
	case SURVIVAL_ITEM_GOLD_SWORD:      return "Golden Sword";
	case SURVIVAL_ITEM_DIAMOND_SWORD:   return "Diamond Sword";

	case SURVIVAL_ITEM_WOOD_AXE:        return "Wooden Axe";
	case SURVIVAL_ITEM_STONE_AXE:       return "Stone Axe";
	case SURVIVAL_ITEM_IRON_AXE:        return "Iron Axe";
	case SURVIVAL_ITEM_GOLD_AXE:        return "Golden Axe";
	case SURVIVAL_ITEM_DIAMOND_AXE:     return "Diamond Axe";

	case SURVIVAL_ITEM_WOOD_SHOVEL:     return "Wooden Shovel";
	case SURVIVAL_ITEM_STONE_SHOVEL:    return "Stone Shovel";
	case SURVIVAL_ITEM_IRON_SHOVEL:     return "Iron Shovel";
	case SURVIVAL_ITEM_GOLD_SHOVEL:     return "Golden Shovel";
	case SURVIVAL_ITEM_DIAMOND_SHOVEL:  return "Diamond Shovel";

	case SURVIVAL_ITEM_WOOD_HOE:        return "Wooden Hoe";
	case SURVIVAL_ITEM_STONE_HOE:       return "Stone Hoe";
	case SURVIVAL_ITEM_IRON_HOE:        return "Iron Hoe";
	case SURVIVAL_ITEM_GOLD_HOE:        return "Golden Hoe";
	case SURVIVAL_ITEM_DIAMOND_HOE:     return "Diamond Hoe";

	case SURVIVAL_ITEM_STICK:           return "Stick";
	case SURVIVAL_ITEM_COAL:            return "Coal";
	case SURVIVAL_ITEM_IRON_INGOT:      return "Iron Ingot";
	case SURVIVAL_ITEM_GOLD_INGOT:      return "Gold Ingot";
	case SURVIVAL_ITEM_DIAMOND:         return "Diamond";
	case SURVIVAL_ITEM_RAW_PORKCHOP:    return "Raw Porkchop";
	default: return "";
	}
}

CC_END_HEADER

/* CavFX food helpers */
static CC_INLINE cc_bool SurvivalItem_IsFood(BlockID item) {
	switch (item) {
	case SURVIVAL_ITEM_RAW_PORKCHOP:
		return true;
	default:
		return false;
	}
}

static CC_INLINE int SurvivalItem_FoodHeal(BlockID item) {
	switch (item) {
	case SURVIVAL_ITEM_RAW_PORKCHOP: return 3; /* 1.5 hearts */
	default: return 0;
	}
}

#endif
