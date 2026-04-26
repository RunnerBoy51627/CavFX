// CavFX Oxygen System
// Drop this into your survival/gameplay code and call the functions from your update loop.

#include "OxygenSystem.h"

#ifndef CAVFX_MAX_OXYGEN
#define CAVFX_MAX_OXYGEN 100.0f
#endif

#ifndef CAVFX_OXYGEN_DRAIN_PER_SEC
#define CAVFX_OXYGEN_DRAIN_PER_SEC 7.0f
#endif

#ifndef CAVFX_OXYGEN_REGEN_PER_SEC
#define CAVFX_OXYGEN_REGEN_PER_SEC 18.0f
#endif

#ifndef CAVFX_DROWN_TICK_TIME
#define CAVFX_DROWN_TICK_TIME 1.0f
#endif

#ifndef CAVFX_DROWN_DAMAGE
#define CAVFX_DROWN_DAMAGE 2
#endif

static float cavfx_clampf(float v, float min, float max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

void Oxygen_Init(OxygenState *oxy)
{
    if (!oxy) return;

    oxy->oxygen = CAVFX_MAX_OXYGEN;
    oxy->maxOxygen = CAVFX_MAX_OXYGEN;
    oxy->isUnderwater = 0;
    oxy->drownTimer = 0.0f;
    oxy->justTookDrownDamage = 0;
}

void Oxygen_SetUnderwater(OxygenState *oxy, int underwater)
{
    if (!oxy) return;

    oxy->isUnderwater = underwater ? 1 : 0;

    if (!oxy->isUnderwater) {
        oxy->drownTimer = 0.0f;
        oxy->justTookDrownDamage = 0;
    }
}

int Oxygen_Update(OxygenState *oxy, float dt)
{
    int damageToApply = 0;

    if (!oxy) return 0;
    if (dt < 0.0f) dt = 0.0f;

    oxy->justTookDrownDamage = 0;

    if (oxy->isUnderwater) {
        oxy->oxygen -= CAVFX_OXYGEN_DRAIN_PER_SEC * dt;
        oxy->oxygen = cavfx_clampf(oxy->oxygen, 0.0f, oxy->maxOxygen);

        if (oxy->oxygen <= 0.0f) {
            oxy->drownTimer += dt;

            while (oxy->drownTimer >= CAVFX_DROWN_TICK_TIME) {
                oxy->drownTimer -= CAVFX_DROWN_TICK_TIME;
                damageToApply += CAVFX_DROWN_DAMAGE;
                oxy->justTookDrownDamage = 1;
            }
        } else {
            oxy->drownTimer = 0.0f;
        }
    } else {
        oxy->oxygen += CAVFX_OXYGEN_REGEN_PER_SEC * dt;
        oxy->oxygen = cavfx_clampf(oxy->oxygen, 0.0f, oxy->maxOxygen);
        oxy->drownTimer = 0.0f;
    }

    return damageToApply;
}

int Oxygen_GetBubbles(const OxygenState *oxy, int maxBubbles)
{
    float ratio;

    if (!oxy || maxBubbles <= 0 || oxy->maxOxygen <= 0.0f) return 0;

    ratio = oxy->oxygen / oxy->maxOxygen;
    ratio = cavfx_clampf(ratio, 0.0f, 1.0f);

    return (int)((ratio * (float)maxBubbles) + 0.999f);
}

float Oxygen_GetRatio(const OxygenState *oxy)
{
    if (!oxy || oxy->maxOxygen <= 0.0f) return 0.0f;
    return cavfx_clampf(oxy->oxygen / oxy->maxOxygen, 0.0f, 1.0f);
}
