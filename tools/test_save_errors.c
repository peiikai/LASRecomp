// Save I/O failures must reach the caller and must not bypass the working copy.
#include "test_host_fs.h"

int main(int argc, char **argv) {
    assert(argc == 2);
#ifdef _WIN32
    _set_fmode(_O_BINARY);
#endif
    HostFs fs = {.journaled=true}; strlcpy(fs.root, argv[1], sizeof fs.root);
    Service mount = {.state=&fs};
    char path[1200], pending[1250], other[1200];
    snprintf(path, sizeof path, "%s/slot.bin", argv[1]);
    snprintf(pending, sizeof pending, "%s.pending", path);
    put_file(path, 0x11, 32);

    // Failed staging (a blocked path, full journal or missing source) never opens the committed file for writing.
    assert(!mkdir(pending, 0755));
    assert(open_file(&mount, "/slot.bin", 2).result && first_byte(path) == 0x11);
    assert(!rmdir(pending)); fs.npending = PENDING_MAX;
    assert(open_file(&mount, "/slot.bin", 2).result && first_byte(path) == 0x11);
    fs.npending = 0;
    assert(open_file(&mount, "/missing.bin", 2).result);
    snprintf(other, sizeof other, "%s/missing.bin.pending", argv[1]);
    assert(access(other, F_OK));

    IpcResp opened = open_file(&mount, "/slot.bin", 2); assert(!opened.result);
    Service *handle = opened.objects[0];
    const char bytes[] = "a complete save write";
    write_limit = 3; write_interrupts = 2;
    assert(!write_file(handle, bytes, sizeof bytes, 0));
    assert(written == sizeof bytes && !write_interrupts && first_byte(path) == 0x11);
    char actual[sizeof bytes];
    assert(pread(((HostFile *)handle->state)->fd, actual, sizeof actual, 0) == sizeof actual);
    assert(!memcmp(actual, bytes, sizeof bytes));

    // Short writes followed by ENOSPC, zero writes and flush failures all report errors.
    written = 0; write_fail_after = 4;
    assert(write_file(handle, bytes, sizeof bytes, 0) && first_byte(path) == 0x11);
    write_fail_after = -1; write_zero = 1;
    assert(write_file(handle, bytes, sizeof bytes, 0)); write_zero = 0;
    sync_error = 1;
    assert(write_file(handle, bytes, sizeof bytes, 1));
    IpcReq flush = {.cmd=2}; IpcResp response = {0};
    file(handle, &flush, &response); assert(response.result);
    sync_error = 0; write_limit = 0;
    assert(!write_file(handle, bytes, sizeof bytes, 0)); close_file(handle);

    // A durability failure leaves the old file in place and the staged file available for retry.
    sync_error = 1;
    assert(commit_mount(&mount) && fs.npending == 1 && first_byte(path) == 0x11);
    assert(!access(pending, F_OK)); sync_error = 0;
    assert(!commit_mount(&mount) && !fs.npending && first_byte(path) == 'a');

    // One failed replacement does not lose either its retry or another file's successful commit.
    opened = open_file(&mount, "/slot.bin", 2); assert(!opened.result);
    assert(!write_file(opened.objects[0], "b", 1, 0)); close_file(opened.objects[0]);
    snprintf(other, sizeof other, "%s/other.bin", argv[1]); put_file(other, 'x', 32);
    opened = open_file(&mount, "/other.bin", 2); assert(!opened.result);
    assert(!write_file(opened.objects[0], "y", 1, 0)); close_file(opened.objects[0]);
    assert(!unlink(path) && !mkdir(path, 0755));
    assert(commit_mount(&mount) && fs.npending == 1 && first_byte(other) == 'y');
    assert(!rmdir(path)); put_file(path, 'a', 32);
    assert(!commit_mount(&mount) && !fs.npending && first_byte(path) == 'b');
#ifdef _WIN32
    opened = open_file(&mount, "/slot.bin", 2); assert(!opened.result);
    assert(!write_file(opened.objects[0], "c", 1, 0)); close_file(opened.objects[0]);
    HANDLE locked = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(commit_mount(&mount) && fs.npending == 1 && first_byte(path) == 'b');
    CloseHandle(locked);
    assert(!commit_mount(&mount) && first_byte(path) == 'c');
#endif
    // Read errors and invalid write buffers must not look like successful I/O.
    HostFile bad = {.fd=-1}; Service bad_file = {.state=&bad};
    uint64_t args[3] = {0, 0, sizeof actual};
    IpcReq read = {.cmd=0, .data=(const uint8_t *)args, .nb=1};
    read.b[0] = (IpcBuf){(uintptr_t)actual, sizeof actual}; response = (IpcResp){0};
    file(&bad_file, &read, &response); assert(response.result);
    IpcReq write = {.cmd=1, .data=(const uint8_t *)args, .na=1};
    write.a[0] = (IpcBuf){(uintptr_t)actual, sizeof actual-1}; response = (IpcResp){0};
    file(&bad_file, &write, &response); assert(response.result);

    // Non-journaled mounts retain their ordinary in-place write behavior.
    fs.journaled = false; opened = open_file(&mount, "/slot.bin", 2); assert(!opened.result);
    assert(!write_file(opened.objects[0], "d", 1, 0)); close_file(opened.objects[0]);
    assert(first_byte(path) == 'd' && !fs.npending && !commit_mount(&mount));
    puts("Save errors: staging, short writes, interruption, flush, publication/retry and SD behavior passed");
}
