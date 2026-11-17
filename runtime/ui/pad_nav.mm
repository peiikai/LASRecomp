// Controller buttons into our ImGui menus: see pad_nav.h.
#include "pad_nav.h"
#include "i18n.h"
#include "imgui_internal.h"  // (ImGuiKey_Gamepad_BEGIN / END)
#include <atomic>
#include "input_bind.h"
#include "../settings.h"

static std::atomic<SDL_JoystickID> g_last_pad{0};

// Menu action -> the ImGui key the menus read (BIND_UI_* order)
static const ImGuiKey k_act_key[BIND_UI_COUNT] = {ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
                                                   ImGuiKey_GamepadFaceUp, ImGuiKey_GamepadStart};

ImGuiKey ui_pad_key(SDL_JoystickID which, int button, bool down) {
    static ImGuiKey held[PADB_HOST_COUNT];  // (the layout can change while a button is held: its release)
    if (button < 0 || button >= PADB_HOST_COUNT) return ImGuiKey_None;
    if (!down) { ImGuiKey k = held[button]; held[button] = ImGuiKey_None; if (k != ImGuiKey_None) return k; }
    if (down) g_last_pad = which;
    ImGuiKey k = ImGuiKey_None;
    int a = bind_ui_action(button);  // (LAS Menu, or the Confirm button setting)
    if (a >= 0) k = k_act_key[a - BIND_UI_CONFIRM];
    else switch (button) {  // tabs and moving: always these (custom LAS Menu: nothing else)
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: k = ImGuiKey_GamepadL1; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: k = ImGuiKey_GamepadR1; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: k = ImGuiKey_GamepadDpadUp; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: k = ImGuiKey_GamepadDpadDown; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: k = ImGuiKey_GamepadDpadLeft; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: k = ImGuiKey_GamepadDpadRight; break;
    case SDL_GAMEPAD_BUTTON_WEST: if (!g_bind_ui_custom) k = ImGuiKey_GamepadFaceLeft; break;
    case SDL_GAMEPAD_BUTTON_BACK: if (!g_bind_ui_custom) k = ImGuiKey_GamepadBack; break;
    default: break;
    }
    if (down) held[button] = k;
    return k;
}

bool ui_pad_trigger(SDL_JoystickID which, int axis, int value, ImGuiKey *key, bool *down) {
    static bool on[2];
    if (axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return false;
    int t = axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    bool now = on[t] ? value > 8000 : value > 16000;  // (as the game's: runtime/hle/input.c)
    if (now == on[t]) return false;
    on[t] = now;
    *key = ui_pad_key(which, t ? PADB_RTRIGGER : PADB_LTRIGGER, now);
    *down = now;
    return *key != ImGuiKey_None;
}

void ui_pad_release_all(ImGuiIO &io) {
    io.ClearEventsQueue();  // (presses / releases not applied yet: from before)
    io.ClearInputKeys();
    for (int k = ImGuiKey_Gamepad_BEGIN; k < ImGuiKey_Gamepad_END; k++) io.KeysData[k - ImGuiKey_NamedKey_BEGIN].AnalogValue = 0;
}

int ui_pad_family(void) { return input_menu_family(g_last_pad.load()) % 3; }
const char *ui_pad_action_label(int act) {
    int f = ui_pad_family();
    return L(bind_pad_label(bind_ui_host(act), f));
}
const char *ui_pad_confirm_label(void) { return ui_pad_action_label(BIND_UI_CONFIRM); }
const char *ui_pad_back_label(void) { return ui_pad_action_label(BIND_UI_BACK); }
const char *ui_pad_start_label(void) { return ui_pad_action_label(BIND_UI_OK); }
