#include "achievements.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace achievements {
static uint64_t read_le(const uint8_t *p, unsigned bytes) {
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
unsigned progress(const Rule &r, const uint8_t *save) {
    if (r.mode == Rule::Hearts)
        return 3 + __builtin_popcountll(read_le(save + 0x5558, 2) & 0x1ff) +
                   __builtin_popcountll(read_le(save + 0x5550, 8) & 0xffffffffULL) / 4;
    uint64_t value = read_le(save + r.offset, r.bytes) & r.mask;
    if (r.mode == Rule::Equal) return value == r.target ? 1 : 0;
    if (r.mode == Rule::Bits) return value == r.mask ? 1 : 0;
    return __builtin_popcountll(value);
}
static bool satisfied(const Rule &r, const uint8_t *s) {
    return progress(r, s) >= ((r.mode == Rule::Equal || r.mode == Rule::Bits) ? 1 : r.target);
}
const std::vector<Rule> &catalog() {
    static const std::vector<Rule> rules = {
#include "achievement_rules.inc"
    };
    return rules;
}
void Tracker::baseline() { ready_ = false; }
bool Tracker::open(const std::string &path) {
    path_ = path; error_.clear(); writable_ = false; ready_ = false; imported_ = false;
    earned_.assign(catalog().size(), false); seen_ = earned_;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f && errno != ENOENT) { error_ = "Cannot read achievement history"; return false; }
    if (f) {
        char line[256], id[32], extra;
        long long stamp;
        while (fgets(line, sizeof line, f)) {
            // A partial final append is not an unlock. Each new record starts with a newline.
            if (!strchr(line, '\n')) continue;
            if (sscanf(line, "%31s %lld %c", id, &stamp, &extra) != 2 || stamp <= 0) continue;
            if (!strcmp(id, "IMPORTED")) { imported_ = true; continue; }
            for (size_t i = 0; i < catalog().size(); ++i)
                if (!strcmp(id, catalog()[i].id)) earned_[i] = true;
        }
        bool ok = !ferror(f);
        if (fclose(f)) ok = false;
        if (!ok) { error_ = "Cannot read achievement history"; return false; }
    }
    writable_ = true;
    return true;
}
// The first save opened with no import yet: what it already meets is earned at once, without popups. Then a marker,
// so other saves never import again (until reset()).
bool Tracker::import_save(const uint8_t *save) {
    std::string lines;
    long long now = (long long)time(nullptr);
    for (size_t i = 0; i < catalog().size(); ++i)
        if (!earned_[i] && satisfied(catalog()[i], save)) lines += "\n" + std::string(catalog()[i].id) + " " + std::to_string(now) + "\n";
    lines += "\nIMPORTED " + std::to_string(now) + "\n";
    FILE *f = fopen(path_.c_str(), "ab");
    if (!f) { error_ = "Cannot save achievement unlock"; return false; }
    bool ok = fwrite(lines.data(), 1, lines.size(), f) == lines.size();
    if (fflush(f)) ok = false;
    if (fclose(f)) ok = false;
    if (!ok) { error_ = "Cannot save achievement unlock"; return false; }
    for (size_t i = 0; i < catalog().size(); ++i)
        if (satisfied(catalog()[i], save)) earned_[i] = true;
    imported_ = true;
    error_.clear();
    return true;
}
bool Tracker::reset() {
    if (!writable_) return false;
    FILE *f = fopen(path_.c_str(), "wb");
    if (!f || fclose(f)) { error_ = "Cannot save achievement unlock"; return false; }
    earned_.assign(catalog().size(), false);
    seen_ = earned_;
    imported_ = false;
    ready_ = false;
    error_.clear();
    return true;
}
bool Tracker::unlocked(size_t i) const { return i < earned_.size() && earned_[i]; }
bool Tracker::record(size_t i) {
    FILE *f = fopen(path_.c_str(), "ab");
    if (!f) { error_ = "Cannot save achievement unlock"; return false; }
    bool ok = fprintf(f, "\n%s %lld\n", catalog()[i].id, (long long)time(nullptr)) > 0;
    if (fflush(f)) ok = false;
    if (fclose(f)) ok = false;
    if (!ok) { error_ = "Cannot save achievement unlock"; return false; }
    earned_[i] = true;
    error_.clear();
    return true;
}
std::vector<size_t> Tracker::sample(const uint8_t *save, size_t size) {
    std::vector<size_t> out;
    if (!writable_ || !save || size < SaveSize || read_le(save, 4) != 14 || !read_le(save + 0x5388, 2)) {
        baseline(); return out;
    }
    if (!ready_ || memcmp(uuid_.data(), save + 8, 16)) {
        memcpy(uuid_.data(), save + 8, 16);
        if (!imported_ && !hold_import_) import_save(save);
        for (size_t i = 0; i < catalog().size(); ++i) seen_[i] = satisfied(catalog()[i], save);
        ready_ = true;
        return out;
    }
    for (size_t i = 0; i < catalog().size(); ++i) {
        bool met = satisfied(catalog()[i], save);
        if (!earned_[i] && !seen_[i] && met) {
            if (record(i)) { out.push_back(i); seen_[i] = true; }
            // Keep it eligible after an I/O error; the adapter limits polling to 10 Hz.
        } else if (met) seen_[i] = true;
    }
    return out;
}
}
