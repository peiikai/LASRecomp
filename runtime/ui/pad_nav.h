// Controller buttons into our ImGui menus (the in-game overlay runtime/ui/menu.mm, the start screen launcher.mm).
// The menus read ImGuiKey_GamepadFaceDown as "confirm" and ImGuiKey_GamepadFaceRight as "back" (as ImGui's own
// navigation does), FaceUp and Start; L1 / R1 (tabs) and the D-pad stay put. The player's LAS Menu buttons (Controls >
// Configure inputs; runtime/input_bind.h BIND_UI_*) pick the physical button for each when set. Otherwise the Controls
// tab's "Confirm button" (g_settings.face_by_label) does, the game's A and B too. Right (default): the right button
// confirms and the bottom one goes back, as on a Switch. Bottom: the other way round, as on Xbox / PlayStation. Same on
// every pad. The Button icons setting only picks the words the hints use (input_menu_family in runtime/hle/input.c),
// never which button does what. The player's game bindings never apply here.
#pragma once
#include <SDL3/SDL.h>
#include "imgui.h"

// The ImGui key for a press / release of `button` (an SDL_GamepadButton, or PADB_LTRIGGER / PADB_RTRIGGER) on
// controller `which` (a release gets the key its press got)
ImGuiKey ui_pad_key(SDL_JoystickID which, int button, bool down);
// A trigger's axis motion: true when it was pressed / let go as a button that drives a menu action (*key, *down)
bool ui_pad_trigger(SDL_JoystickID which, int axis, int value, ImGuiKey *key, bool *down);
// The family of the controller last used in a menu (0 Nintendo, 1 Xbox, 2 PlayStation): the hints' words
int ui_pad_family(void);
// The confirm / back / start buttons' names in that family's words (e.g. PlayStation, game layout: "Circle" / "Cross" /
// "Options"; as labeled: "Cross" / "Circle" / "Options")
const char *ui_pad_confirm_label(void);
const char *ui_pad_back_label(void);
const char *ui_pad_start_label(void);
const char *ui_pad_action_label(int act);  // any menu action's button (act: BIND_UI_*)
// Every key and controller input let go, in ImGui's eyes: its releases can go missing (the menu closed, a rebind took
// them) and a key it still thinks is held repeats - the cursor keeps scrolling until that button is pressed again
void ui_pad_release_all(ImGuiIO &io);
