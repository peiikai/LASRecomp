// clang -O2 tools/test_cloud_scan_darwin.c -framework CoreFoundation -o <test>
// Run with a scratch directory holding one empty regular file.
#include <assert.h>
#include <stdbool.h>
#include <sys/attr.h>
#include <unistd.h>
#include <errno.h>

static bool fail_reads, read_entry, injected;
static int test_getattrlistbulk(int fd, void *attrs, void *buffer, size_t size, uint64_t flags) {
    if (fail_reads && read_entry) {
        injected = true;
        errno = EIO;
        return -1;
    }
    int count = getattrlistbulk(fd, attrs, buffer, size, flags);
    if (count > 0) read_entry = true;
    return count;
}

#define getattrlistbulk test_getattrlistbulk
#include "../runtime/platform/darwin_common.h"
#undef getattrlistbulk

uint64_t g_fault_lr;
bool rt_fault_watch(uint64_t addr, uint64_t pc) { (void)addr; (void)pc; return false; }
void rt_fault_report(const char *what, uint64_t addr, uint64_t pc, uint64_t lr) {
    (void)what; (void)addr; (void)pc; (void)lr;
    abort();
}

int main(int argc, char **argv) {
    assert(argc == 2);
    int policy = getiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD);
    assert(plat_cloud_offloaded(argv[1]) == PLAT_CLOUD_AVAILABLE);
    assert(read_entry);
    fail_reads = true;
    read_entry = false;
    assert(plat_cloud_offloaded(argv[1]) == PLAT_CLOUD_INCOMPLETE);
    assert(injected);
    assert(getiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD) == policy);
    puts("Darwin cloud scan keeps a partial directory read incomplete and restores I/O policy");
}
