// LAS Recomp's item steps: shared helpers (ours_items_lib.h).
#include "ours_items_lib.h"

#include <stdexcept>

namespace rando {
namespace ours {

std::vector<std::pair<std::string, int>> alloc_flags(GSheet &sheet, const std::vector<std::string> &names, int base,
                                                      int limit) {
    int nxt = base;
    for (const GStruct &v : sheet.values) {
        int u = las::at(v, "index").i;
        if (base <= u && u < limit && u + 1 > nxt) nxt = u + 1;
    }
    std::vector<std::pair<std::string, int>> out;
    for (const std::string &n : names) {
        if (nxt >= limit) throw std::runtime_error("out of GlobalFlags indices");
        sheet.values.push_back({{"symbol", las::gs(n)}, {"index", las::gi(nxt)}});
        out.push_back({n, nxt});
        nxt++;
    }
    return out;
}

void retarget(EvflFlowchart &fc, uint16_t from, uint16_t to, const std::set<uint16_t> &skip) {
    for (size_t i = 0; i < fc.events.size(); i++) {
        if (skip.count((uint16_t)i)) continue;
        EvflEvent &e = fc.events[i];
        switch (e.type) {
        case EvflEvent::Action: case EvflEvent::SubFlow: case EvflEvent::Join:
            if (e.next == from) e.next = to;
            break;
        case EvflEvent::Switch:
            for (auto &c : e.cases)
                if (c.second == from) c.second = to;
            break;
        case EvflEvent::Fork:
            for (uint16_t &f : e.forks)
                if (f == from) f = to;
            break;
        }
    }
    for (EvflEntryPoint &ep : fc.entry_points)
        if (ep.main_event == from) ep.main_event = to;
}

uint16_t event_index(EvflFlowchart &fc, const std::string &name) {
    int i = fc.find_event(name);
    if (i < 0) throw std::runtime_error("no event " + name + " in " + fc.name);
    return (uint16_t)i;
}

}  // namespace ours
}  // namespace rando
