// lasrando room-facts <romfs>: what the room readers (rooms_game.cpp) read from the game's files, as JSON (the shape
// tools/rando/room_logic.py / dungeon_gen.py would give it), to compare with the Python's on the same files.
#include <stdio.h>

#include "outputs.h"
#include "rooms.h"

namespace rando {
namespace solve {

static Value ints(const std::vector<int> &v) {
    Value l = Value::new_list();
    for (int i : v) l.list().push_back((long long)i);
    return l;
}
static Value strs(const std::vector<std::string> &v) {
    Value l = Value::new_list();
    for (const std::string &s : v) l.list().push_back(s);
    return l;
}

int room_facts(const std::string &romfs) {
    using namespace rooms;
    GameFiles game = game_files_dir(romfs);
    Value out = Value::new_dict(), areas = Value::new_dict(), ports = Value::new_dict(), holes = Value::new_dict();
    for (int hook = 0; hook < 2; hook++) {
        GameScope scope(game, hook != 0);
        const Data &D = data();
        Value a = Value::new_dict();
        for (const Room &r : D.rooms) {
            const Areas &A = r.areas[hook];
            Value x = Value::new_dict(), moves = Value::new_list();
            x.set("tiles", ints(A.tiles)), x.set("n", (long long)A.n);
            for (const Move &m : A.moves) {
                Value al = Value::new_list();
                for (const Atoms &t : m.alts) al.list().push_back(strs(t));
                moves.list().push_back(Value(List{Value((long long)m.a), Value((long long)m.b), al}));
            }
            x.set("moves", moves);
            a.set(r.name, x);
        }
        areas.set(hook ? "hook" : "plain", a);
        ports.set(hook ? "hook" : "plain", D.ports[hook]);
        holes.set(hook ? "hook" : "plain", D.hole_spots[hook]);
        if (hook) continue;
        Value rooms_v = Value::new_dict();
        for (const Room &r : D.rooms) {
            Value x = Value::new_dict(), locks = Value::new_list(), land = Value::new_list(), sh = Value::new_dict();
            for (const auto &l : r.locks) locks.list().push_back(Value(List{Value(std::string(1, SIDES[l.first])), l.second}));
            x.set("locks", locks);
            Value bl = Value::new_list();
            for (const Value &b : r.blocks) bl.list().push_back(b);
            x.set("blocks", bl);
            x.set("throw", r.throw_ok ? strs(r.throw_req) : Value());
            for (int s = 0; s < 4; s++) sh.set(std::string(1, SIDES[s]), strs(r.shutter[s]));
            x.set("shutter", sh);
            x.set("ntex", (long long)r.tex.size());
            for (int s = 0; s < 4; s++)
                for (const Open &o : r.op[s])
                    if (!o.slot.wide())
                        for (int m : {0, 1}) {
                            auto it = r.land.find({s, o.slot.v, m});
                            if (it == r.land.end()) continue;
                            land.list().push_back(Value(List{Value(std::string(1, SIDES[s])), Value((long long)o.slot.v),
                                                             Value((long long)m),
                                                             Value(List{Value(it->second.depth), Value(it->second.height),
                                                                        Value(it->second.lateral)})}));
                        }
            x.set("land", land);
            rooms_v.set(r.name, x);
        }
        out.set("rooms", rooms_v);
        Value levels = Value::new_dict();
        for (const std::string &lv : LEVEL_ORDER) {
            Value x = Value::new_dict();
            x.set("offgrid", strs(D.offgrid.at(lv)));
            x.set("noffgrid_tex", (long long)D.offgrid_tex_all.at(lv).size());
            x.set("nbase_tex", (long long)D.base_tex.at(lv).size());
            x.set("cells", strs(std::vector<std::string>(D.level_cells.at(lv).begin(), D.level_cells.at(lv).end())));
            Value st = Value::new_list(), hl = Value::new_list();
            for (const auto &p : D.stairs.at(lv)) st.list().push_back(Value(List{Value(p.first), Value(p.second)}));
            for (const auto &p : D.holes.at(lv)) hl.list().push_back(Value(List{Value(p.first), Value(p.second)}));
            x.set("stairs", st), x.set("holes", hl);
            levels.set(lv, x);
        }
        out.set("levels", levels);
        Value inward = Value::new_list();
        for (const auto &e : D.inward)
            inward.list().push_back(Value(List{Value(std::get<0>(e.first)), Value(std::string(1, SIDES[std::get<1>(e.first)])),
                                               Value((long long)std::get<2>(e.first)), Value(e.second)}));
        out.set("inward", inward);
        Value anims = Value::new_dict(), shifts = Value::new_dict();
        for (const auto &e : D.map_anims) anims.set(e.first, (long long)e.second);
        for (const auto &e : D.map_shifts) shifts.set(std::to_string(e.first), Value(List{Value(e.second.first), Value(e.second.second)}));
        out.set("map_anims", anims), out.set("map_shifts", shifts);
    }
    out.set("areas", areas), out.set("ports", ports), out.set("holes", holes);
    printf("%s\n", py_json(out).c_str());
    return 0;
}

}  // namespace solve
}  // namespace rando
