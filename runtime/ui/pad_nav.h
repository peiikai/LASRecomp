// Controller buttons into our ImGui menus (the in-game overlay runtime/ui/menu.mm, the start screen launcher.mm).
// The menus read ImGuiKey_GamepadFaceDown as "confirm" and ImGuiKey_GamepadFaceRight as "back" (as ImGui's own
// navigation does). Which physical button each one is: the Controls tab's "Confirm button" (g_settings.face_by_label),
// the game's A and B too. Right (default): the right button confirms and the bottom one goes back, as on a Switch.
// Bottom: the other way round, as on Xbox / PlayStation. Same on every pad. The Button icons setting only picks the
// words the hints use (input_menu_family in runtime/hle/input.c). The player's game bindings never apply here.
#pragma once
#include <SDL3/SDL.h>
#include "imgui.h"

// The ImGui key for a press / release of `button` on controller `which` (a release gets the key its press got)
ImGuiKey ui_pad_key(SDL_JoystickID which, Uint8 button, bool down);
// The family of the controller last used in a menu (0 Nintendo, 1 Xbox, 2 PlayStation): the hints' words
int ui_pad_family(void);
// The confirm / back / start buttons' names in that family's words (e.g. PlayStation, game layout: "Circle" / "Cross" /
// "Options"; as labeled: "Cross" / "Circle" / "Options")
const char *ui_pad_confirm_label(void);
const char *ui_pad_back_label(void);
const char *ui_pad_start_label(void);
// Every key and controller input let go, in ImGui's eyes: its releases can go missing (the menu closed, a rebind took
// them) and a key it still thinks is held repeats - the cursor keeps scrolling until that button is pressed again
void ui_pad_release_all(ImGuiIO &io);
