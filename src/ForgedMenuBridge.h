#ifndef FORGED_MENU_BRIDGE_H
#define FORGED_MENU_BRIDGE_H

/*
    Final compatible Forged menu bridge.

    Supports BOTH:
      New system:
        ForgedCore/forged.disabled exists = OFF

      Old system:
        ForgedCore/forged.enabled exists = ON

    Disable Forged:
      - creates forged.disabled
      - deletes forged.enabled everywhere

    Enable Forged:
      - deletes forged.disabled everywhere
      - writes forged.enabled for older menu code compatibility
*/

typedef enum ForgedMenuState {
    FORGED_MENU_STATE_INSTALLER_MISSING = 0,
    FORGED_MENU_STATE_CAN_INSTALL       = 1,
    FORGED_MENU_STATE_ENABLED           = 2,
    FORGED_MENU_STATE_DISABLED          = 3
} ForgedMenuState;

ForgedMenuState ForgedMenu_GetState(void);

int ForgedMenu_CoreExists(void);
int ForgedMenu_InstallerExists(void);
int ForgedMenu_IsEnabled(void);
int ForgedMenu_ButtonEnabled(void);

const char* ForgedMenu_GetButtonText(void);
const char* ForgedMenu_GetStatusText(void);

int ForgedMenu_OnButtonClick(void);

/* Public direct controls, in case your Menus.c calls these directly */
int ForgedMenu_EnableForged(void);
int ForgedMenu_DisableForged(void);

int ForgedMenu_ShouldShowInstallPopup(void);
int ForgedMenu_ConfirmInstall(void);
int ForgedMenu_CancelInstall(void);

const char* ForgedMenu_GetPopupTitle(void);
const char* ForgedMenu_GetPopupBody(void);

#endif
