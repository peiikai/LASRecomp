// Per-instruction replacements applied by the generator (port of tools/patches.py): guest address -> function
// returning C code, plus hook sites (the instruction stays; runtime/hle/hooks.c decides at run time).
//
// Every patch checks the original instruction word, so a wrong address in a version table fails loudly.
// The sites come from the selected version's address table (runtime/game_addresses/patches_<key>.h).
#include "gen_internal.hpp"

namespace lasgen {

thread_local const AddressTable *PATCH_ADDRESSES = nullptr;

namespace {

uint64_t A(const char *name) {
    if (!PATCH_ADDRESSES) throw GenError("patches: no address table selected");
    auto it = PATCH_ADDRESSES->find(name);
    if (it == PATCH_ADDRESSES->end()) throw GenError(std::string("AttributeError: ") + name);
    return it->second;
}

std::string at(const std::string &code) { return addr_text(*PATCH_ADDRESSES, code); }

// f'{v:#x}'
std::string ax(uint64_t v) { return "0x" + hexs(v); }

void check(bool ok, uint64_t a, uint32_t w) {
    if (!ok) throw GenError("AssertionError: patch " + ax(a) + ": " + ax(w));
}

// Per-site data tables, filled by build_patches (patches.py module-level dicts; later keys win, as in a dict)
struct Tables {
    std::map<uint64_t, int> aspect_sites;
    std::map<uint64_t, std::pair<uint32_t, int>> fovy_sites;
    struct Sky { uint32_t want; int off; const char *sign; };
    std::map<uint64_t, Sky> skysea_box_sites;
    std::map<uint64_t, int> warp_request_sites;
    std::vector<uint64_t> equip_y_read_sites;
    std::map<uint64_t, std::pair<int, int>> walk_speed_sites;
    struct Tunic { uint32_t word, mask; const char *what; };
    std::map<uint64_t, Tunic> tunic_clear_sites;
    struct NoHandler { uint32_t want; std::string code; const char *what; };
    std::map<uint64_t, NoHandler> no_handler_sites;
};
thread_local Tables T;

// Widescreen: the game's camera code loads a 16:9 aspect-ratio literal as the default projection aspect. These loads
// are redirected to rt_aspect_site(), which returns the aspect for the current output shape (or 16:9) per site, so
// only the 3D cameras widen.
std::string aspect_load(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xffc00000) == 0xbd400000 && ((w >> 10) & 0xfff) * 4 == 0xba4, a, w);
    int rt = w & 31;
    return cat("{ extern float rt_aspect_site(int); float f = rt_aspect_site(", T.aspect_sites.at(a), "); ",
               "c->v[", rt, "] = 0; memcpy(&c->v[", rt, "], &f, 4); }  /* ", ax(a), ": ldr s", rt, ", =16/9 */");
}

// Off-screen culling: the scene's visibility pass loads the camera's aspect into s9; we load the runtime aspect
// instead (site 6).
std::string cull_aspect(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xbd457369, a, w);  // ldr s9, [x27, #0x570]
    return cat("{ extern float rt_cull_aspect(float); float o; memcpy(&o, (const void *)(uintptr_t)(c->x[27] + 0x570), 4); ",
               "float f = rt_cull_aspect(o); c->v[9] = 0; memcpy(&c->v[9], &f, 4); }",
               "  /* ", ax(a), ": ldr s9, [x27, #0x570] (culling aspect) */");
}

// Vertical field of view (camera field +0x564): taller-than-16:9 outputs keep the designed horizontal view and widen
// the vertical one ("Vert+").
std::string fovy_load(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    auto [want, rn] = T.fovy_sites.at(a);
    check(w == want, a, w);
    int rt = w & 31;
    return cat("{ extern float rt_fovy(float); float o; ",
               "memcpy(&o, (const void *)(uintptr_t)(c->x[", rn, "] + 0x564), 4); float f = rt_fovy(o); ",
               "c->v[", rt, "] = 0; memcpy(&c->v[", rt, "], &f, 4); }  /* ", ax(a), ": ldr s", rt, ", [x", rn,
               ", #0x564] (fovy) */");
}

// Sky and sea beyond the overworld's north edge: each piece is drawn only while Link stands inside its box; with a
// wider view the next piece is needed sooner. The box's across bounds are widened by the extra view
// (runtime/hle/widescreen.c rt_skysea_margin).
std::string skysea_box(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    auto s = T.skysea_box_sites.at(a);
    check(w == s.want, a, w);  // ldr qN, [x23, #off]
    int rt = w & 31;
    return cat("{ extern float rt_skysea_margin(void); float b[4]; ",
               "memcpy(b, (const void *)(uintptr_t)(c->x[23] + ", ax(s.off), "), 16); b[0] += ", s.sign,
               "f * rt_skysea_margin(); ", "memcpy(&c->v[", rt, "], b, 16); }  /* ", ax(a), ": ldr q", rt, ", [x23, #",
               ax(s.off), "] (sky / sea piece box) */");
}

// Indoor detection: the `ldr s1, =16/9` in the indoor shadow setup keeps loading 16:9 and tells the runtime an
// interior is being rendered.
std::string indoor(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xffc00000) == 0xbd400000 && ((w >> 10) & 0xfff) * 4 == 0xba4, a, w);
    int rt = w & 31;
    return cat("{ extern float rt_indoor_aspect(void); float f = rt_indoor_aspect(); ", "c->v[", rt, "] = 0; memcpy(&c->v[",
               rt, "], &f, 4); }  /* ", ax(a), ": ldr s", rt, ", =16/9 (indoor shadow) */");
}

// Warp support: the level-jump request object registers itself in its parent while a level loads; record it so
// runtime/hle/warp.c can fill the request itself.
std::string warp_request(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    int rt = T.warp_request_sites.at(a);
    check(w == (0xf900e100u | (uint32_t)rt), a, w);  // str xRT, [x8, #0x1c0]
    return cat("{ extern void rt_warp_request_obj(uint64_t); ", "*(uint64_t *)(uintptr_t)(c->x[8] + 0x1c0) = c->x[", rt,
               "]; rt_warp_request_obj(c->x[", rt, "]); }", "  /* ", ax(a), ": str x", rt,
               ", [x8, #0x1c0] (warp: level-jump request) */");
}

// Generated dungeons (runtime/hle/chamber.c): every level jump goes through the request processor; its prologue
// reports the request so jumps into Tail Cave can be redirected.
std::string level_request(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10203ff, a, w);  // sub sp, sp, #0x80
    return cat("{ extern void rt_level_request(Cpu *); rt_level_request(c); c->sp -= 0x80; }", "  /* ", ax(a),
               ": sub sp, sp, #0x80 (level-jump request) */");
}

// Chamber staircase exit prompt: in a generated dungeon the dialog is skipped and 0 stored, so the staircase leaves
// directly (runtime/hle/chamber.c).
std::string adv_exit_dialog(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x94025d28 || (w >> 26) == 0x25, a, w);  // bl
    return at(cat("{ extern int rt_adv_exit_choice(void); extern void f_@ADV_EXIT_DIALOG_TARGET@(Cpu *); ",
                  "if (!rt_adv_exit_choice()) { c->x[30] = 0x@ADV_EXIT_DIALOG_RETURN@ull; f_@ADV_EXIT_DIALOG_TARGET@(c); } }",
                  "  /* ", ax(a), ": bl 0xf05280 (chamber exit prompt) */"));
}

// Difficulty (runtime/ui/difficulty.mm): Link's damage is `damage << hero` at these `lsl Wd, Wn, Wm` sites, then
// subtracted from health. Each is redirected to rt_damage(damage, shift) so the difficulty multiplier / one-hit KO
// applies.
std::string damage_lsl(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xffe0fc00) == 0x1ac02000, a, w);  // lslv (32-bit)
    int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    return cat("{ extern int32_t rt_damage(int32_t, int32_t); c->x[", rd, "] = (uint32_t)rt_damage((int32_t)c->x[", rn,
               "], ", "(int32_t)(c->x[", rm, "] & 31)); }  /* ", ax(a), ": lsl w", rd, ", w", rn, ", w", rm, " (damage) */");
}

// Fixed damage (-2 quarter hearts, -4 in Hero Mode: `csel w20, w10, w9, eq` with w10=-4, w9=-2).
std::string damage_fixed(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xffe00c00) == 0x1a800000 && (w & 31) == 20, a, w);  // csel w20, ...
    return cat("{ extern int32_t rt_damage(int32_t, int32_t); extern int rt_hero_mode(void); ",
               "c->x[20] = (uint32_t)(-rt_damage(2, rt_hero_mode())); }", "  /* ", ax(a), ": csel w20, -4, -2 (fixed damage) */");
}

// Items page description: in randomizer seeds the explanation is hidden (it overlaps the key / trade-item rows of
// the inventory overlay).
std::string item_explanation(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xf9407bf6, a, w);  // ldr x22, [sp, #0xf0]
    return cat("{ extern int rt_hide_item_explanation(void); ",
               "c->x[22] = rt_hide_item_explanation() ? 0 : LD64(c->sp + 0xf0); }", "  /* ", ax(a),
               ": ldr x22, [sp, #0xf0] (item explanation message) */");
}

// ... and the item NAME: in randomizer seeds it is skipped too.
std::string item_name(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x94035896, a, w);  // bl 0x7000f8c8b0
    return at(cat("c->x[30] = 0x@ITEM_NAME_RETURN@ull; { extern int rt_hide_item_explanation(void); ",
                  "if (!rt_hide_item_explanation()) f_@ITEM_NAME_TARGET@(c); }", "  /* ", ax(a),
                  ": bl 0xf8c8b0 (set the item name pane) */"));
}

// Keysanity (runtime/hle/keysanity.c): with an explicit dungeon index the hook updates that dungeon's save record.
std::string add_item(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10143ff, a, w);  // sub sp, sp, #0x50
    return cat("{ extern int rt_add_item(Cpu *); if (rt_add_item(c)) return; c->sp -= 0x50; }", "  /* ", ax(a),
               ": sub sp, sp, #0x50 (AddItem: keysanity) */");
}

// Trendsanity (runtime/hle/shopsanity.c): a randomized prize row sets its check's flags.
std::string prize_add(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x2a1403e2, a, w);  // mov w2, w20
    return cat("{ c->x[2] = (uint32_t)c->x[20]; extern void rt_add_prize(Cpu *); rt_add_prize(c); }", "  /* ", ax(a),
               ": mov w2, w20 (AddPrize index; trendsanity) */");
}

// Inventory overlay: runs once per frame only while the pause menu's Items page is shown.
std::string items_page(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10243ff, a, w);  // sub sp, sp, #0x90
    return cat("{ extern void rt_items_page_tick(void); rt_items_page_tick(); c->sp -= 0x90; }", "  /* ", ax(a),
               ": sub sp, sp, #0x90 (items page shown) */");
}

// Touch controls: the pause menu's update, once per frame on every page.
std::string pause_menu(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10383ff, a, w);  // sub sp, sp, #0xe0
    return cat("{ extern void rt_pause_menu_tick(void); rt_pause_menu_tick(); c->sp -= 0xe0; }", "  /* ", ax(a),
               ": sub sp, sp, #0xe0 (pause menu shown) */");
}

// The Items page's cursor move: a tap on a cell asks for that cell, through the game's own move.
std::string items_nav(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xb9400116, a, w);  // ldr w22, [x8]
    return cat("{ extern uint32_t rt_items_nav_pick(uint32_t); c->x[22] = rt_items_nav_pick(LD32(c->x[8])); }", "  /* ",
               ax(a), ": ldr w22, [x8] (items page: the next cell) */");
}

// The file select's cursor: a tap on a file puts the cursor there right after the list helper.
std::string file_select_nav(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x370003c0, a, w);  // tbnz w0, #0, <moved>
    uint64_t t = a + ((w >> 5) & 0x3FFF) * 4;
    return cat("{ extern int rt_file_nav_pick(uint64_t cursor, int moved); ",
               "if (rt_file_nav_pick(c->x[19], (int)(c->x[0] & 1))) goto L_", hexs(t), "; }", "  /* ", ax(a),
               ": tbnz w0, #0 (file select: the cursor moved) */");
}

// Check tracker: runs once per frame only while the pause menu's Map page is shown.
std::string map_page(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10203ff, a, w);  // sub sp, sp, #0x80
    return cat("{ extern void rt_map_page_tick(Cpu *); rt_map_page_tick(c); c->sp -= 0x80; }", "  /* ", ax(a),
               ": sub sp, sp, #0x80 (map page shown) */");
}

// HUD hearts refresh (runtime/hle/hud_hearts.c): reruns the HUD's own set-up when a menu changed Link's hearts.
std::string hud_hearts(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd102c3ff, a, w);  // sub sp, sp, #0xb0
    return cat("{ extern void rt_hud_hearts_tick(Cpu *); rt_hud_hearts_tick(c); c->sp -= 0xb0; }", "  /* ", ax(a),
               ": sub sp, sp, #0xb0 (HUD update: hearts refresh) */");
}

// ... the Map page's epilogue: the check tracker hides the game's own cursor and button bar there.
std::string map_page_end(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x910203ff, a, w);  // add sp, sp, #0x80
    return cat("{ extern void rt_map_page_end(Cpu *); rt_map_page_end(c); c->sp += 0x80; }", "  /* ", ax(a),
               ": add sp, sp, #0x80 (map page updated) */");
}

// Chaos map letters: runs once per frame while the DUNGEON map page is shown.
std::string dgn_map(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10243ff, a, w);  // sub sp, sp, #0x90
    return cat("{ extern void rt_dgnmap_tick(Cpu *); rt_dgnmap_tick(c); c->sp -= 0x90; }", "  /* ", ax(a),
               ": sub sp, sp, #0x90 (dungeon map page shown) */");
}

std::string dgn_map_end(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x910243ff, a, w);  // add sp, sp, #0x90
    return cat("{ extern void rt_dgnmap_end(Cpu *); rt_dgnmap_end(c); c->sp += 0x90; }", "  /* ", ax(a),
               ": add sp, sp, #0x90 (dungeon map page updated) */");
}

// Debug (LARECOMP_LOG_RESNAME=1): logs resource names looked up by name.
std::string res_name(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10503ff, a, w);  // sub sp, sp, #0x140
    return cat("{ extern void rt_log_resname(uint64_t); rt_log_resname(c->x[2]); c->sp -= 0x140; }", "  /* ", ax(a),
               ": sub sp, sp, #0x140 (resource-by-name debug log) */");
}

// Frame pacing (runtime/hle/framepace.c): the frame timer gets the number of vsyncs that really passed.
std::string frame_steps(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xb8696908, a, w);  // ldr w8, [x8, x9]
    return cat("{ extern uint32_t rt_frame_steps(uint32_t interval); ",
               "c->x[8] = rt_frame_steps(LD32(c->x[8] + c->x[9])); }", "  /* ", ax(a),
               ": ldr w8, [x8, x9] (swap interval -> vsyncs elapsed: runtime/hle/framepace.c) */");
}

// The game's own dynamic resolution, off: resolution is ours to choose (runtime/nvn/output_scale.c).
std::string game_dynres(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x395922e8, a, w);  // ldrb w8, [x23, #0x648]
    return cat("{ extern int rt_game_dynres(void); ", "c->x[8] = rt_game_dynres() ? LD8(c->x[23] + 0x648) : 0; }", "  /* ",
               ax(a), ": ldrb w8, [x23, #0x648] (the game's dynamic resolution: off) */");
}

std::string game_dynres_init(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xb86d698c, a, w);  // ldr w12, [x12, x13] (x13 = 0x6e4c)
    return cat("{ extern int rt_game_dynres(void); ", "c->x[12] = rt_game_dynres() ? LD32(c->x[12] + c->x[13]) : 0; }",
               "  /* ", ax(a), ": ldr w12, [x12, x13] (the game's starting resolution level: full size) */");
}

// Escape Bottle (runtime/hle/escape.c): the third bottle's "use it now?" predicate; in seeds the runtime warps
// instead and the answer is no.
std::string bottle3_use(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x1a9f17e0, a, w);  // cset w0, eq
    return cat("{ extern int rt_bottle3_use(void); c->x[0] = COND_NE(c) ? 0 : 1; if (c->x[0] && rt_bottle3_use()) c->x[0] = 0; }",
               "  /* ", ax(a), ": cset w0, eq (third bottle used: Escape Bottle) */");
}

// Hint Book (runtime/hle/hintbook.c): the first bottle's predicate.
std::string bottle1_use(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x1a9f17e0, a, w);  // cset w0, eq
    return cat("{ extern int rt_bottle1_use(void); c->x[0] = COND_NE(c) ? 0 : 1; if (c->x[0] && rt_bottle1_use()) c->x[0] = 0; }",
               "  /* ", ax(a), ": cset w0, eq (first bottle used: Hint Book) */");
}

// D-pad items (runtime/hle/dpad_items.c): the d-pad as a third set of item buttons, in the game's own "use it now?"
// checks. The Y test also passes for a newly pressed d-pad direction, and the read then answers the item itself.
std::string equip_y_test(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xFFF8001F) == 0x37180008, a, w);  // tbnz w8, #3, <read>
    int64_t disp = (int64_t)(((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000;
    uint64_t t = a + (uint64_t)(disp * 4);
    return cat("{ extern int rt_equip_y_test(uint32_t pressed); if (rt_equip_y_test((uint32_t)c->x[8])) goto L_", hexs(t),
               "; }", "  /* ", ax(a), ": tbnz w8, #3 (Y newly pressed, or a d-pad direction) */");
}

const uint32_t EQUIP_Y_READ_SLOTS[12] = {0xf, 0xe, 0xd, 0x6, 0x5, 0x4, 0x3, 0x2, 0x7, 0xc, 0x8, 0x9};  // (each check's item)

std::string equip_y_read(Module &m, uint64_t a) {
    uint64_t off = a - m.base;
    uint32_t w = m.image_word(a);
    check((w & 0xFFFFFFE0) == 0x39428900 && (w & 31) < 30, a, w);  // ldrb wN, [x8, #0xa2]
    size_t idx = 0;
    while (idx < T.equip_y_read_sites.size() && T.equip_y_read_sites[idx] != a) idx++;
    if (idx >= T.equip_y_read_sites.size() || idx >= 12) throw GenError("ValueError: equip_y_read site");
    uint32_t slot = EQUIP_Y_READ_SLOTS[idx];
    if (off + 8 <= m.nso.image.size()) {
        uint32_t n = m.nso.u32(off + 4);
        check(n == (0x7100001Fu | slot << 10 | (w & 31) << 5), a, n);  // cmp wN, #<slot>
    }
    return cat("{ extern uint32_t rt_equip_y_read(uint64_t ps, int slot); c->x[", (int)(w & 31),
               "] = rt_equip_y_read(c->x[8], ", slot, "); }", "  /* ", ax(a), ": ldrb w", (int)(w & 31),
               ", [x8, #0xa2] (item on Y; item ", ax(slot), " on a d-pad direction) */");
}

// Boss souls (runtime/hle/souls.c) and enemy mode Random (runtime/hle/enemy_random.c): the actor factory's create
// function lookup; a null fn is its "no such actor" path.
std::string boss_spawn(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xf9400118, a, w);  // ldr x24, [x8]
    if (a - 4 >= m.base) {  // (the address table test gives this one word only)
        w = m.image_word(a - 4);
        check(w == 0x9b086ee8, a, w);  // madd x8, x23, x8, x27
    }
    return cat("{ extern uint64_t rt_actor_create_fn(uint64_t fn, uint32_t type, uint64_t info); ",
               "extern void rt_enemy_spawn(uint64_t info); rt_enemy_spawn(c->x[2]); ",
               "c->x[23] = LD16(c->x[2] + 8); c->x[8] = c->x[27] + c->x[23] * 0x58ull; ",
               "c->x[24] = rt_actor_create_fn(LD64(c->x[8]), (uint32_t)c->x[23], c->x[2]); }", "  /* ", ax(a),
               ": ldr x24, [x8] (actor create fn: boss souls, enemy mode Random) */");
}

// Hookshot anything (runtime/hle/hookshot.c): the attribute word of the surface the hookshot's ray hit.
std::string hookshot_attr(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xb9403bf7, a, w);  // ldr w23, [sp, #0x38]
    return cat("{ extern uint32_t rt_hookshot_attr(Cpu *, uint32_t); ", "c->x[23] = rt_hookshot_attr(c, LD32(c->sp + 0x38)); }",
               "  /* ", ax(a), ": ldr w23, [sp, #0x38] (hookshot hit attribute) */");
}

// Hookshot 8-way aim (runtime/hle/hookshot.c): the runtime can round the firing direction to one of 8 first.
std::string hookshot_dir(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x3d80be60, a, w);  // str q0, [x19, #0x2f0]
    return cat("{ extern void rt_hookshot_dir(Cpu *); rt_hookshot_dir(c); memcpy((void *)(uintptr_t)(c->x[19] + 0x2f0), &c->v[0], 16); }",
               "  /* ", ax(a), ": str q0, [x19, #0x2f0] (hookshot firing direction) */");
}

// 360-degree movement (runtime/hle/qol.c): the runtime returns the stick's own angle instead of the 8-way snap.
std::string snap_dir(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10103ff, a, w);  // sub sp, sp, #0x40
    return cat("{ extern int rt_snap_dir(Cpu *); if (rt_snap_dir(c)) return; c->sp -= 0x40; }", "  /* ", ax(a),
               ": sub sp, sp, #0x40 (8-way stick snap: 360-degree movement) */");
}

// Walk speed (runtime/hle/qol.c): rt_walk_speed multiplies the walk speed multiplier in s0 by the setting. Jump
// distances stay the game's own.
std::string walk_speed(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    auto [rn, site] = T.walk_speed_sites.at(a);
    check(w == (0xf94ca408u | (uint32_t)rn << 5), a, w);  // ldr x8, [x19|x20, #0x1948]
    return cat("{ extern float rt_walk_speed(uint64_t, float, int); float m_; memcpy(&m_, &c->v[0], 4); ",
               "m_ = rt_walk_speed(c->x[", rn, "], m_, ", site, "); c->v[0] = 0; memcpy(&c->v[0], &m_, 4); ",
               "c->x[8] = LD64(c->x[", rn, "] + 0x1948); }", "  /* ", ax(a), ": ldr x8, [x", rn,
               ", #0x1948] (walk speed multiplier in s0) */");
}

// Wind push (runtime/nvn/weather.c rt_wind_push): a push added to Link's frame displacement is stopped by walls and
// ledges like his walking.
std::string wind_push(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x38686a68, a, w);  // ldrb w8, [x19, x8]
    return cat("{ extern void rt_wind_push(Cpu *); rt_wind_push(c); c->x[8] = LD8(c->x[19] + c->x[8]); }", "  /* ", ax(a),
               ": ldrb w8, [x19, x8] (Link move: the wind push) */");
}

// Walk animation rate (walk state): the walk animation's play rate in s8.
std::string walk_anim(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xf94ab668, a, w);  // ldr x8, [x19, #0x1568]
    return cat("{ extern float rt_walk_speed(uint64_t, float, int); float m_; memcpy(&m_, &c->v[8], 4); ",
               "m_ = rt_walk_speed(c->x[19], m_, 100); c->v[8] = 0; memcpy(&c->v[8], &m_, 4); ",
               "c->x[8] = LD64(c->x[19] + 0x1568); }", "  /* ", ax(a), ": ldr x8, [x19, #0x1568] (walk animation rate in s8) */");
}

// The same for the shield walk.
std::string shield_anim(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xf94006c8, a, w);  // ldr x8, [x22, #8]
    return cat("{ extern float rt_walk_speed(uint64_t, float, int); float m_; memcpy(&m_, &c->v[0], 4); ",
               "m_ = rt_walk_speed(c->x[20], m_, 101); c->v[0] = 0; memcpy(&c->v[0], &m_, 4); ",
               "c->x[8] = LD64(c->x[22] + 8); }", "  /* ", ax(a), ": ldr x8, [x22, #8] (shield walk animation rate in s0) */");
}

// No Guardian Acorns / Pieces of Power (randomizer no-buff-drops): mAllowBuffReplace reads as false for such seeds.
std::string buff_replace(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x3940e308, a, w);  // ldrb w8, [x24, #0x38]
    return cat("{ extern int rt_buff_replace(int); c->x[8] = (uint32_t)rt_buff_replace(LD8(c->x[24] + 0x38)); }", "  /* ",
               ax(a), ": ldrb w8, [x24, #0x38] (mAllowBuffReplace: no-buff-drops) */");
}

// Blur removal (runtime/hle/qol.c): qol.c keeps the tilt-shift object and takes the parameter handle out / puts it
// back live.
std::string tilt_init(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10243ff, a, w);  // sub sp, sp, #0x90
    return cat("{ extern void rt_tilt_obj(uint64_t); rt_tilt_obj(c->x[0]); c->sp -= 0x90; }", "  /* ", ax(a),
               ": sub sp, sp, #0x90 (tilt-shift set-up: blur removal) */");
}

// Event parameters: a missing String parameter of an event action throws on the Switch (caught there); recompiled
// code can't unwind. A missing String reads as "" instead.
std::string missing_string(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x321c03e0, a, w);  // mov w0, #0x10 (__cxa_allocate_exception size)
    // "" (the terminator of ".ptcl" right before the message; callers strlen() the result, so not NULL)
    return at(cat("{ ST64(c->sp + 0x18, 0x@EMPTY_STRING@ull); ST32(c->sp + 0x20, 0); goto L_@MISSING_STRING_RETURN@; }",
                  "  /* ", ax(a), ": mov w0, #0x10 (missing String event parameter: return \"\", no throw) */"));
}

// The Float version: a missing Float reads as 0.0 instead (the reader treats time <= 0 as instant).
std::string missing_float(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x321c03e0, a, w);  // mov w0, #0x10 (__cxa_allocate_exception size)
    // s0 = 0.0 (q0 at Cpu+512), then the function's own epilogue
    return cat("{ __builtin_memset((char *)c + 512, 0, 16);", " c->x[29] = LD64(c->sp + 48); c->x[30] = LD64(c->sp + 56);",
               " c->x[20] = LD64(c->sp + 32); c->x[19] = LD64(c->sp + 40); c->sp += 0x40; return; }", "  /* ", ax(a),
               ": mov w0, #0x10 (missing Float event parameter: return 0.0, no throw) */");
}

// Treat Tunics as Upgrades (runtime/hle/qol.c rt_tunic_upgrades): with the seed option the other tunic's clear is
// skipped, so both tunics are kept.
std::string tunic_clear(Module &m, uint64_t a) {
    auto s = T.tunic_clear_sites.at(a);
    uint32_t w = m.image_word(a);
    check(w == s.word, a, w);
    return cat("{ extern int rt_tunic_upgrades(void); ", "if (!rt_tunic_upgrades()) c->x[9] = (uint32_t)((uint32_t)c->x[9] & ",
               ax(s.mask), "u); }", "  /* ", ax(a), ": ", s.what, " - kept with Treat Tunics as Upgrades */");
}

// Underwater checks: for heart-piece actors in seeds made with them the "owned?" answer is "not owned"
// (runtime/hle/qol.c rt_underwater_skip_owned).
std::string underwater_owned(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x9400351e, a, w);  // bl 0x8d1a30 (HasItem)
    return at(cat("{ extern int rt_underwater_skip_owned(uint64_t); if (rt_underwater_skip_owned(c->x[20])) c->x[0] = 0; ",
                  "else { extern void f_@HAS_ITEM@(Cpu *); c->x[30] = 0x@UNDERWATER_OWNED_RETURN@ull; f_@HAS_ITEM@(c); } }",
                  "  /* ", ax(a), ": bl HasItem (sunken heart pieces: their own flag decides) */"));
}

// ...and their item models show underwater: in underwater-item seeds every material counts as under_water.
std::string underwater_view(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x1a9f07fb, a, w);  // cset w27, ne (under_water != 0)
    return cat("{ extern int rt_underwater_items(void); c->x[27] = (rt_underwater_items() || COND_NE(c)) ? 1u : 0u; }",
               "  /* ", ax(a), ": cset w27, ne (under_water: forced on for sunken items) */");
}

// ...and picking one up gives only its own item (no extra heart piece).
std::string underwater_pickup(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x9420589f, a, w);  // bl 0x89b960
    return at(cat("{ extern int rt_underwater_skip_owned(uint64_t); if (!rt_underwater_skip_owned(c->x[19])) ",
                  "{ extern void f_@HEART_PIECE_ADD@(Cpu *); c->x[30] = 0x@UNDERWATER_PICKUP_RETURN@ull; f_@HEART_PIECE_ADD@(c); } }",
                  "  /* ", ax(a), ": bl 0x89b960 (adds the heart piece: skipped for sunken items) */"));
}

// Event actions with no handler: recompiled code can't unwind, so the throw goes straight into the catch block,
// without the C++ runtime's begin / end catch (nothing was thrown).
std::string no_handler(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    auto &s = T.no_handler_sites.at(a);
    check(w == s.want, a, w);
    return cat("{ ", s.code, " }  /* ", ax(a), ": ", s.what, " */");
}

// Mirror mode: the in-world action prompt's x is mirrored for that pane (runtime/hle/mirror.c).
std::string prompt_pos(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x2d060102, a, w);  // stp s2, s0, [x8, #0x30]
    return cat("{ extern float rt_prompt_x(uint64_t pane, float x); float px_, py_; memcpy(&px_, &c->v[2], 4); ",
               "memcpy(&py_, &c->v[0], 4); px_ = rt_prompt_x(c->x[8], px_); ",
               "memcpy((void *)(uintptr_t)(c->x[8] + 0x30), &px_, 4); memcpy((void *)(uintptr_t)(c->x[8] + 0x34), &py_, 4); }",
               "  /* ", ax(a), ": stp s2, s0, [x8, #0x30] (action prompt spot: mirror mode) */");
}

// Issue #45: the font's missing-character box glyph is marked without checking for none; no glyph skips the
// marking as the second lookup's check does.
std::string font_box_glyph(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x3940e008, a, w);  // ldrb w8, [x0, #0x38]
    return at("if (c->x[0] == 0) goto L_@FONT_BOX_GLYPH_SKIP@; c->x[8] = LD8(c->x[0] + 0x38);") + "  /* " + ax(a) +
           ": ldrb w8, [x0, #0x38] (font box glyph: none skips the marking) */";
}

// Issue #452: a character the font has no glyph for is drawn and measured as the box glyph, read without checking
// for none. No box glyph reads as an empty one (no width, nothing drawn).
std::string font_fallback(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    const char *as = nullptr, *op = nullptr;
    switch (w) {
        case 0x79409800: as = "ldrh w0, [x0, #0x4c]", op = "c->x[0] = LD16(c->x[0] + 0x4c);"; break;
        case 0xaa0003e9: as = "mov x9, x0", op = "c->x[9] = c->x[0];"; break;
        case 0x79409816: as = "ldrh w22, [x0, #0x4c]", op = "c->x[22] = LD16(c->x[0] + 0x4c);"; break;
        case 0x79409c08: as = "ldrh w8, [x0, #0x4e]", op = "c->x[8] = LD16(c->x[0] + 0x4e);"; break;
    }
    check(as != nullptr, a, w);
    return cat("{ static uint8_t empty_glyph_[0x60] __attribute__((aligned(16))); ",
               "if (c->x[0] == 0) c->x[0] = (uint64_t)(uintptr_t)empty_glyph_; } ", op, "  /* ", ax(a), ": ", as,
               " (font fallback glyph: none reads as an empty one) */");
}

// Issue #19: two delegate forwarders read a missing delegate as "nothing to do" (runtime/hle/delegate_null.c).
std::string delegate_forwarder(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xaa0003e1, a, w);  // mov x1, x0
    int slot = a == A("DELEGATE_FORWARDER_SITES_0") ? 0x28 : 0x30;
    return cat("{ extern int rt_delegate_null(Cpu *, int); if (rt_delegate_null(c, ", ax((uint64_t)slot),
               ")) return; c->x[1] = c->x[0]; }", "  /* ", ax(a), ": mov x1, x0 (delegate forwarder: nothing to forward to) */");
}

}  // namespace

PatchSet build_patches(const std::string &version_key) {
    PATCH_ADDRESSES = &address_table(version_key);
    T = Tables();
    PatchSet ps;
    auto &P = ps.patches;
    auto set = [&](uint64_t a, PatchFn f) { P[a] = f; };

    T.aspect_sites = {};
    for (int i = 0; i < 6; i++) T.aspect_sites[A(("ASPECT_SITES_" + std::to_string(i)).c_str())] = i;
    for (auto &e : T.aspect_sites) set(e.first, aspect_load);

    set(A("CULL_ASPECT"), cull_aspect);

    T.fovy_sites[A("FOVY_SITES_0")] = {0xbd4566e0, 23};
    T.fovy_sites[A("FOVY_SITES_1")] = {0xbd456768, 27};
    for (auto &e : T.fovy_sites) set(e.first, fovy_load);

    T.skysea_box_sites[A("SKYSEA_BOX_MAX_SITE")] = {0x3dc0bae1, 0x2e0, "1.0"};
    T.skysea_box_sites[A("SKYSEA_BOX_MIN_SITE")] = {0x3dc0b6e2, 0x2d0, "-1.0"};
    for (auto &e : T.skysea_box_sites) set(e.first, skysea_box);

    set(A("INDOOR_SITE"), indoor);

    T.warp_request_sites[A("WARP_REQUEST_SITES_0")] = 20;
    T.warp_request_sites[A("WARP_REQUEST_SITES_1")] = 19;
    for (auto &e : T.warp_request_sites) set(e.first, warp_request);

    set(A("LEVEL_REQUEST_SITE"), level_request);
    set(A("ADV_EXIT_DIALOG_SITE"), adv_exit_dialog);

    for (const char *n : {"DAMAGE_LSL_SITES_0", "DAMAGE_LSL_SITES_1", "DAMAGE_LSL_SITES_2"}) set(A(n), damage_lsl);
    set(A("DAMAGE_FIXED_SITE"), damage_fixed);

    set(A("ITEM_EXPLANATION_SITE"), item_explanation);
    set(A("ITEM_NAME_SITE"), item_name);
    set(A("ADD_ITEM_SITE"), add_item);
    set(A("PRIZE_ADD_SITE"), prize_add);
    set(A("ITEMS_PAGE_SITE"), items_page);
    set(A("PAUSE_MENU_SITE"), pause_menu);
    set(A("ITEMS_NAV_SITE"), items_nav);
    set(A("FILE_SELECT_NAV_SITE"), file_select_nav);
    set(A("MAP_PAGE_SITE"), map_page);
    set(A("HUD_HEARTS_SITE"), hud_hearts);
    set(A("MAP_PAGE_END_SITE"), map_page_end);
    set(A("DGN_MAP_SITE"), dgn_map);
    set(A("DGN_MAP_END_SITE"), dgn_map_end);
    set(A("RES_NAME_SITE"), res_name);
    set(A("FRAME_STEPS_SITE"), frame_steps);
    set(A("GAME_DYNRES_SITE"), game_dynres);
    set(A("GAME_DYNRES_INIT_SITE"), game_dynres_init);
    set(A("BOTTLE3_USE_SITE"), bottle3_use);
    set(A("BOTTLE1_USE_SITE"), bottle1_use);

    std::vector<uint64_t> y_test;
    for (int i = 0; i < 12; i++) y_test.push_back(A(("EQUIP_Y_TEST_SITES_" + std::to_string(i)).c_str()));
    for (int i = 0; i < 12; i++) T.equip_y_read_sites.push_back(A(("EQUIP_Y_READ_SITES_" + std::to_string(i)).c_str()));
    for (uint64_t a : y_test) set(a, equip_y_test);
    for (uint64_t a : T.equip_y_read_sites) set(a, equip_y_read);

    set(A("BOSS_SPAWN_SITE"), boss_spawn);
    set(A("HOOKSHOT_ATTR_SITE"), hookshot_attr);
    set(A("HOOKSHOT_DIR_SITE"), hookshot_dir);
    set(A("SNAP_DIR_SITE"), snap_dir);

    T.walk_speed_sites[A("WALK_SPEED_SITES_0")] = {19, 0};
    T.walk_speed_sites[A("WALK_SPEED_SITES_1")] = {19, 1};
    T.walk_speed_sites[A("WALK_SPEED_SITES_2")] = {20, 2};
    T.walk_speed_sites[A("WALK_SPEED_SITES_3")] = {20, 3};
    for (auto &e : T.walk_speed_sites) set(e.first, walk_speed);

    set(A("WIND_PUSH_SITE"), wind_push);
    set(A("WALK_ANIM_SITE_0"), walk_anim);
    set(A("WALK_ANIM_SITE_1"), shield_anim);
    set(A("BUFF_REPLACE_SITE"), buff_replace);
    set(A("TILT_INIT_SITE"), tilt_init);
    set(A("MISSING_STRING_SITE"), missing_string);
    set(A("MISSING_FLOAT_SITE"), missing_float);

    T.tunic_clear_sites[A("TUNIC_CLEAR_SITES_0")] = {0x12197929, 0xffffffbf, "and w9, w9, #0xffffffbf (blue tunic: red cleared)"};
    T.tunic_clear_sites[A("TUNIC_CLEAR_SITES_1")] = {0x12001929, 0x7f, "and w9, w9, #0x7f (red tunic: blue cleared)"};
    for (auto &e : T.tunic_clear_sites) set(e.first, tunic_clear);

    set(A("UNDERWATER_OWNED_SITE"), underwater_owned);
    set(A("UNDERWATER_VIEW_SITE"), underwater_view);
    set(A("UNDERWATER_PICKUP_SITE"), underwater_pickup);

    T.no_handler_sites[A("NO_HANDLER_SITES_0")] = {0x321d03e0, at("goto L_@NO_HANDLER_CATCH@;"),
                                                   "mov w0, #8 (bad_function_call: to the catch block)"};
    T.no_handler_sites[A("NO_HANDLER_SITES_1")] = {0x940e8868, "", "bl __cxa_begin_catch (skipped)"};
    T.no_handler_sites[A("NO_HANDLER_SITES_2")] = {0x940e887b, "", "bl __cxa_end_catch (skipped)"};
    for (auto &e : T.no_handler_sites) set(e.first, no_handler);

    set(A("PROMPT_POS_SITE"), prompt_pos);
    set(A("FONT_BOX_GLYPH_SITE"), font_box_glyph);
    for (const char *n : {"FONT_FALLBACK_SITES_0", "FONT_FALLBACK_SITES_1", "FONT_FALLBACK_SITES_2", "FONT_FALLBACK_SITES_3"})
        set(A(n), font_fallback);
    for (const char *n : {"DELEGATE_FORWARDER_SITES_0", "DELEGATE_FORWARDER_SITES_1"}) set(A(n), delegate_forwarder);

    // ---- hook sites: the instruction stays as it is; just before it the runtime's rt_hook (runtime/hle/hooks.c) is
    // asked what to do - 0 run it, 1 the hook did it all, 2.. jump to the site's first.. target (each a label in its
    // function). The game code they generate never changes when a hook's behaviour does: only a new site needs the
    // game code rebuilt. New game-code changes go here, as a hook, unless they can't.
    // Power-up music (powerup_music.c), field mini-bosses (miniboss.c), digs (digs.c), Shadow Link, quick pickups,
    // issues #19, #41, #183, #314, #349, #681: see the address tables' names and the runtime files for each.
    auto &H = ps.hooks;
    auto hook = [&](const char *site, uint32_t word, std::vector<const char *> targets) {
        Hook h;
        h.word = word;
        for (const char *t : targets) h.targets.push_back(A(t));
        H[A(site)] = h;
    };
    hook("BUFF_MUSIC_PICKUP_SITE", 0x97ccd77b, {});            // bl main+0xaf560: Guardian Acorn / Piece of Power music
    hook("BUFF_MUSIC_FLAGS_SITES_0", 0xb95b8d15, {});          // ldr w21, [x8, #0x1b8c]: music update, power-up sound flag
    hook("BUFF_MUSIC_FLAGS_SITES_1", 0xb95b8d14, {});          // ldr w20: zone music
    hook("BUFF_MUSIC_FLAGS_SITES_2", 0xb95b8d15, {});          // event music
    hook("BUFF_MUSIC_FLAGS_SITES_3", 0xb95b8d15, {});          // power-up presentation
    hook("MB_SEEN_SITE", 0xf94002a8, {});                      // ldr x8, [x21]: an actor seen - field mini-bosses
    hook("CRASHOUT_SITE", 0xb900b81f, {"CRASHOUT_SKIP"});      // Blaino knock-out, no crash-out on the Field
    hook("GOHMA_HEADING_SITES_0", 0xb915ee68, {});             // Gohma heading at Link on the Field
    hook("GOHMA_HEADING_SITES_1", 0xb915ee68, {});
    hook("GOHMA_ARRIVED_SITE", 0x3dc58260, {"GOHMA_ARRIVED_SKIP"});  // Gohma, no track end on the Field
    hook("MSTALFOS_BODY_SITE", 0x320003e8, {});                // Master Stalfos body collision flag, off on the Field
    hook("MSTALFOS_JUMP_SITE", 0x1e213800, {});                // Master Stalfos jump arc from his takeoff height
    hook("SHADOW_LINK_APPEAR_SITE", 0xf9411809, {"SHADOW_LINK_APPEAR_SKIP"});  // no Appear event on the Field
    hook("SHADOW_LINK_BGM_SITE", 0x97ff5eac, {});              // his chamber music, not outside it
    hook("ACTOR_EVENT_SITE", 0x97fb074a, {"ACTOR_EVENT_SKIP"});  // an actor's event request (quick pickups: none)
    hook("MCHIEF_STORY_SITE", 0x37380148, {"MCHIEF_STORY_SKIP"});  // Moblin Chief, no story check on the Field
    hook("DIG_HOLE_SITE", 0xf94007f6, {});                     // a hole was dug (digs.c)
    hook("HOLE_CAP_SITES_0", 0xf101451f, {});                  // dug-hole limit (digs.c)
    hook("HOLE_CAP_SITES_1", 0xf101411f, {});
    hook("EMPTY_HANDLER_SITE", 0x9400005e, {"EMPTY_HANDLER_CATCH"});  // an empty event handler, as its catch (#41)
    hook("CHOICE_COUNT_SITE", 0xb91cc668, {});                 // a question's answer count (#183)
    hook("FRAME_JOB_KICKOFF_SITES_0", 0x910782d3, {});         // frame job kick-off, previous finish first (#19)
    hook("FRAME_JOB_KICKOFF_SITES_1", 0x910782d3, {});
    hook("FRAME_JOB_KICKOFF_SITES_2", 0x910782d3, {});
    hook("FRAME_JOB_KICKOFF_SITES_3", 0x910782b3, {});
    hook("FRAME_JOB_FINISH_SITE", 0xf9400008, {});             // frame job finish (debug delay)
    hook("OWNER_CALL_NOTIFY_SITE", 0x9100e2a0, {});            // owner-thread call done, the caller is waiting first
    hook("SHAPE_SIM_SITES_0", 0xaa0003f4, {"SHAPE_SIM_SKIP_0"});  // PhysX shape change without its sim object (#314)
    hook("SHAPE_SIM_SITES_1", 0xaa0003e1, {"SHAPE_SIM_SKIP_1"});
    hook("EFFECT_BONE_SITE", 0xf9402ac0, {"EFFECT_BONE_SKIP"});  // a bone-following effect without its bone (#349)
    hook("TEXT_RATE_SITE", 0xb9028408, {});                    // a message pane's text step (#681)
    hook("TEXT_SNAP_SITE", 0x540002cb, {});                    // message text short of the page's end (#681)
    return ps;
}

}  // namespace lasgen
