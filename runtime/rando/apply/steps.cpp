// The ported steps, in the generator's order (a full build runs them so; lasrando check-steps runs each on its own).
#include <string.h>

#include "steps.h"

namespace rando {

const std::vector<BuildStep> &build_steps() {
    static const std::vector<BuildStep> all = [] {
        std::vector<BuildStep> v;
        for (auto group : {steps_las_general, steps_las_items, steps_las_world, steps_ours_items, steps_ours_world})
            for (const BuildStep &s : group()) v.push_back(s);
        return v;
    }();
    return all;
}

}  // namespace rando
