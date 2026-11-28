#include "../runtime/achievements.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
using namespace achievements;
using Save = std::array<uint8_t, SaveSize>;
static Save fresh() { Save s{}; s[0]=14; s[8]=1; s[0x5388]='L'; return s; }
static void put(Save &s,unsigned off,uint64_t n,unsigned bytes=8) {
    for(unsigned i=0;i<bytes;++i)s[off+i]=(n>>(8*i))&255;
}
static bool has(const std::vector<size_t>& got,const char *id) {
    for(auto i:got)if(!strcmp(catalog()[i].id,id))return true;
    return false;
}
int main(int argc,char **argv) {
    assert(argc==2); std::string path=argv[1];
    std::set<std::string> ids;
    for(const auto &r:catalog()) { assert(ids.insert(r.id).second); assert(r.bytes<=8); assert(r.offset+r.bytes<=SaveSize); }
    assert(catalog().size()==80);
    Tracker tr; assert(tr.open(path)); Save s=fresh();
    assert(tr.sample(s.data(),s.size()).empty());
    put(s,0x5548,31);auto got=tr.sample(s.data(),s.size());
    assert(got.size()==2 && has(got,"LA-017") && has(got,"LA-018"));
    assert(tr.sample(s.data(),s.size()).empty());
    // Returning to an earlier save cannot earn the same unlock again.
    put(s,0x5548,0);tr.sample(s.data(),s.size());put(s,0x5548,31);
    assert(tr.sample(s.data(),s.size()).empty());
    Tracker restarted;assert(restarted.open(path));
    Save blank=fresh();restarted.sample(blank.data(),blank.size());
    assert(restarted.sample(s.data(),s.size()).empty());
    // Opening a completed file establishes a baseline, including when the same UUID is reloaded.
    put(s,0x5548,(1ULL<<50)-1);tr.baseline();
    assert(tr.sample(s.data(),s.size()).empty());
    put(s,0x5548,0);tr.sample(s.data(),s.size());put(s,0x5548,(1ULL<<50)-1);
    assert(tr.sample(s.data(),s.size()).empty());
    s[8]=2;put(s,0x5548,0);assert(tr.sample(s.data(),s.size()).empty());
    put(s,0x5548,1023);got=tr.sample(s.data(),s.size());assert(got.size()==1 && has(got,"LA-019"));
    // Unused bits and health refills are not collectibles.
    put(s,0x5548,1023|(1ULL<<63));put(s,0x5550,1ULL<<63);put(s,0x5544,80,2);
    assert(tr.sample(s.data(),s.size()).empty());
    put(s,0x5550,15);got=tr.sample(s.data(),s.size());assert(has(got,"LA-001")&&has(got,"LA-002"));
    put(s,0x5558,1,2);got=tr.sample(s.data(),s.size());assert(has(got,"LA-010"));
    // A fairy in a bottle isn't a new bottle; ownership bits are the source.
    s[0x5580]=7;assert(tr.sample(s.data(),s.size()).empty());
    put(s,0x5568,0xe000,4);got=tr.sample(s.data(),s.size());assert(got.size()==3&&has(got,"LA-072"));
    // Snapshot truncation invalidates tracking safely.
    assert(tr.sample(s.data(),100).empty());
    assert(tr.sample(s.data(),s.size()).empty());
    // Failed writes do not announce or mark an unlock; an invalid history isn't overwritten.
    Tracker bad;assert(!bad.open(path+"/missing/log"));
    bad.sample(blank.data(),blank.size());assert(bad.sample(s.data(),s.size()).empty());
    Tracker writefail;assert(writefail.open(path+"-missing-dir/log"));
    blank[8]=s[8];
    writefail.sample(blank.data(),blank.size());assert(writefail.sample(s.data(),s.size()).empty());
    assert(!writefail.error().empty());
    // A torn append doesn't swallow a later, complete record.
    {std::ofstream f(path,std::ios::app);f<<"LA-020 123";}
    Tracker torn;assert(torn.open(path));assert(!torn.unlocked(19));
    torn.sample(blank.data(),blank.size());put(blank,0x5568,1,4);
    assert(has(torn.sample(blank.data(),blank.size()),"LA-052"));
    Tracker reread;assert(reread.open(path));assert(reread.unlocked(51));
    // Every catalog entry must be reachable from a fresh live baseline.
    for (size_t i=0; i<catalog().size(); ++i) {
        const Rule &r=catalog()[i];
        Tracker single; assert(single.open(path+"-rule-"+std::to_string(i)));
        Save probe=fresh(); single.sample(probe.data(),probe.size());
        uint64_t value=0;
        if(r.mode==Rule::Hearts) { put(probe,0x5558,0x1ff,2);put(probe,0x5550,0xffffffff); }
        else {
            if(r.mode==Rule::Bits)value=r.mask;
            else if(r.mode==Rule::Equal)value=r.target;
            else { unsigned left=r.target;for(unsigned b=0;b<64 && left;++b)if(r.mask&(1ULL<<b)){value|=1ULL<<b;--left;}assert(left==0); }
            put(probe,r.offset,value,r.bytes);
        }
        assert(has(single.sample(probe.data(),probe.size()),r.id));
    }
    // Either fishing flag awards First Catch. The adjacent Rapids flag is a separate rule.
    Tracker activities; assert(activities.open(path + "-activities"));
    Save event_save = fresh();
    activities.sample(event_save.data(), event_save.size());
    event_save[0x5dee] = 0x20;
    got = activities.sample(event_save.data(), event_save.size());
    assert(got.size() == 1 && has(got, "LA-075"));
    event_save[0x5dee] |= 0x10;
    assert(activities.sample(event_save.data(), event_save.size()).empty());
    event_save[0x5de8] = 0x40;
    got = activities.sample(event_save.data(), event_save.size());
    assert(got.size() == 1 && has(got, "LA-079"));
    {   // One-time import: the first save opened is credited at once (no popups); other saves never import; reset
        // arms it again.
        std::string ip=path+".import"; remove(ip.c_str());
        Tracker im; assert(im.open(ip) && !im.imported());
        Save a=fresh(); put(a,0x5548,31);  // 5 shells
        assert(im.sample(a.data(),a.size()).empty());
        assert(im.imported() && im.unlocked(16) && im.unlocked(17) && !im.unlocked(18));
        Save b=fresh(); b[8]=2; put(b,0x5548,0x7fff);  // another playthrough with 15 shells: not imported
        assert(im.sample(b.data(),b.size()).empty() && !im.unlocked(18));
        Tracker again; assert(again.open(ip) && again.imported() && again.unlocked(17) && !again.unlocked(18));
        assert(again.reset() && !again.imported() && !again.unlocked(17));
        assert(again.sample(b.data(),b.size()).empty() && again.unlocked(18) && again.unlocked(19));
        Tracker last; assert(last.open(ip) && last.imported() && last.unlocked(19));
        // Reset while a save is open: held until the next load, then that save is imported.
        assert(last.sample(b.data(),b.size()).empty());
        last.hold_import(true); assert(last.reset());
        last.baseline(); assert(last.sample(b.data(),b.size()).empty() && !last.imported() && !last.unlocked(18));
        last.hold_import(false); last.baseline();
        assert(last.sample(b.data(),b.size()).empty() && last.imported() && last.unlocked(18));
        remove(ip.c_str());
    }
    puts("Achievement tests passed: import, reset, thresholds, deduplication, reloads, persistence, masks and I/O errors.");
}
