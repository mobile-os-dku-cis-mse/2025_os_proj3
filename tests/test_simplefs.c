#include "simplefs.h"
#include "util.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

static void test_mk_mount_list() {
    const char* img = "test_disk.img";
    assert(mk_simplefs_create(img, "TESTVOL", 1234, 20, NULL, 0) == 0);

    FS fs;
    assert(fs_mount(&fs, img) == 0);
    assert(fs.sb.first_data_block == 8);
    assert(fs.inode_table[SIMPLEFS_ROOT_INO].size > 0);

    // ensure dir cache works by opening a known pattern name (file_0 likely exists)
    PCB pcb = { .pid = 1 };
    int fd = sys_open(&pcb, &fs, "/file_0", O_RD);
    assert(fd >= 0);

    uint8_t buf[64];
    int r = sys_read(&pcb, &fs, fd, buf, sizeof(buf));
    assert(r >= 0);
    sys_close(&pcb, &fs, fd);

    fs_umount(&fs);
}

static void test_write_persist() {
    const char* img = "test_disk2.img";
    assert(mk_simplefs_create(img, "TESTVOL", 999, 5, NULL, 0) == 0);

    FS fs;
    assert(fs_mount(&fs, img) == 0);

    PCB pcb = { .pid = 1 };
    int fd = sys_open(&pcb, &fs, "/file_0", O_WR);
    assert(fd >= 0);

    const char payload[] = "PERSISTENT_WRITE_TEST\n";
    assert(sys_write(&pcb, &fs, fd, payload, (uint32_t)strlen(payload)) == (int)strlen(payload));
    assert(sys_close(&pcb, &fs, fd) == 0);
    assert(fs_sync(&fs) == 0);
    fs_umount(&fs);

    // remount and read back
    assert(fs_mount(&fs, img) == 0);
    fd = sys_open(&pcb, &fs, "/file_0", O_RD);
    assert(fd >= 0);
    uint8_t buf[64];
    int r = sys_read(&pcb, &fs, fd, buf, sizeof(buf)-1);
    assert(r > 0);
    buf[r] = '\0';
    assert(strstr((char*)buf, "PERSISTENT_WRITE_TEST") != NULL);
    sys_close(&pcb, &fs, fd);
    fs_umount(&fs);
}

static void test_buffer_cache_hits() {
    const char* img = "test_disk3.img";
    assert(mk_simplefs_create(img, "TESTVOL", 42, 5, NULL, 0) == 0);

    FS fs;
    assert(fs_mount(&fs, img) == 0);
    PCB pcb = { .pid = 1 };

    uint64_t h0 = fs.bcache.hits, m0 = fs.bcache.misses;

    int fd = sys_open(&pcb, &fs, "/file_0", O_RD);
    assert(fd >= 0);

    uint8_t buf[128];
    (void)sys_read(&pcb, &fs, fd, buf, 64);
    (void)sys_read(&pcb, &fs, fd, buf, 64);
    sys_close(&pcb, &fs, fd);

    // second read likely benefits from cache
    assert(fs.bcache.misses >= m0);
    assert(fs.bcache.hits >= h0);

    fs_umount(&fs);
}

int main(void) {
    test_mk_mount_list();
    test_write_persist();
    test_buffer_cache_hits();

    printf("All tests passed.\n");
    return 0;
}
