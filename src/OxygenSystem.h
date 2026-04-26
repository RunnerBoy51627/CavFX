#ifndef CAVFX_OXYGEN_SYSTEM_H
#define CAVFX_OXYGEN_SYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OxygenState {
    float oxygen;
    float maxOxygen;
    int isUnderwater;
    float drownTimer;
    int justTookDrownDamage;
} OxygenState;

void Oxygen_Init(OxygenState *oxy);
void Oxygen_SetUnderwater(OxygenState *oxy, int underwater);

// Returns how much damage should be applied this frame.
int Oxygen_Update(OxygenState *oxy, float dt);

// Useful for drawing Minecraft-style bubble HUD.
int Oxygen_GetBubbles(const OxygenState *oxy, int maxBubbles);
float Oxygen_GetRatio(const OxygenState *oxy);

#ifdef __cplusplus
}
#endif

#endif
