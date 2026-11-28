#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace achievements {
constexpr size_t SaveSize = 0x5ee0;
enum class Badge { Heart, Shell, Figurine, Music, Sword, Shield, Trade, Upgrade, Bottle,
                   Dungeon, Fish, Prize, Rapids, Moon, Hidden };
struct Rule {
    const char *id, *title, *description;
    unsigned offset, bytes;
    uint64_t mask;
    unsigned target;
    enum Mode { Count, Bits, Equal, Hearts } mode;
    Badge badge;
};
const std::vector<Rule> &catalog();
unsigned progress(const Rule &rule, const uint8_t *save);
class Tracker {
public:
    bool open(const std::string &path);
    void baseline();
    std::vector<size_t> sample(const uint8_t *save, size_t size);
    bool unlocked(size_t index) const;
    bool reset();  // forget every unlock; the next save opened is imported again
    bool imported() const { return imported_; }
    void hold_import(bool hold) { hold_import_ = hold; }  // (after a reset: wait for the next save load)
    const std::string &error() const { return error_; }
private:
    std::string path_, error_;
    std::vector<bool> earned_, seen_;
    std::array<uint8_t, 16> uuid_{};
    bool ready_ = false, writable_ = false, imported_ = false, hold_import_ = false;
    bool import_save(const uint8_t *save);
    bool record(size_t index);
};
}
