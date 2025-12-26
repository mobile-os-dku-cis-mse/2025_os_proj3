#include "simplefs.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static void demo_random_reads(FS* fs, PCB* pcb) {
    size_t nfiles = 0;
    char** names = collect_root_filenames(fs, &nfiles);
    if (!names || nfiles == 0) {
        printf("No files in root.\n");
        return;
    }

    srand((unsigned)time(NULL));
    for (int t = 0; t < 10; t++) {
        const char* fname = names[rand() % nfiles];
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "/%s", fname);

        printf("[pid=%d] open(%s)\n", pcb->pid, path);
        int fd = sys_open(pcb, fs, path, O_RD);
        if (fd < 0) { printf("  -> open failed\n"); continue; }

        uint8_t buf[2049];
        int r = sys_read(pcb, fs, fd, buf, 2048);
        if (r < 0) { printf("  -> read failed\n"); }
        else {
            buf[r] = '\0';
            printf("  -> read %d bytes\n", r);
            printf("----- content (truncated) -----\n");
            fwrite(buf, 1, (size_t)r, stdout);
            printf("\n-------------------------------\n");
        }
        sys_close(pcb, fs, fd);
    }

    free_filenames(names, nfiles);
}

static void demo_one_write(FS* fs, PCB* pcb) {
    // Pick first filename and overwrite it (write-from-beginning)
    size_t nfiles = 0;
    char** names = collect_root_filenames(fs, &nfiles);
    if (!names || nfiles == 0) return;

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "/%s", names[0]);

    int fd = sys_open(pcb, fs, path, O_WR);
    if (fd < 0) { printf("write demo: open failed (need O_WR)\n"); goto out; }

    const char msg[] = "HELLO FROM write() EXTRA CREDIT!\n";
    int w = sys_write(pcb, fs, fd, msg, (uint32_t)strlen(msg));
    printf("write demo: wrote %d bytes to %s\n", w, path);

    sys_close(pcb, fs, fd);
    fs_sync(fs);

    // read back
    fd = sys_open(pcb, fs, path, O_RD);
    uint8_t buf[128];
    int r = sys_read(pcb, fs, fd, buf, sizeof(buf)-1);
    buf[(r > 0) ? r : 0] = '\0';
    printf("write demo: read-back:\n%s\n", buf);
    sys_close(pcb, fs, fd);

out:
    free_filenames(names, nfiles);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s disk.img\n", argv[0]);
        return 2;
    }

    FS fs;
    if (fs_mount(&fs, argv[1]) != 0) die("fs_mount failed");

    printf("=== Boot: mount rootfs ===\n");
    fs_print_super(&fs);
    fs_print_root_ls(&fs);

    PCB child;
    memset(&child, 0, sizeof(child));
    child.pid = 1;

    demo_random_reads(&fs, &child);
    demo_one_write(&fs, &child);

    printf("Buffer cache stats: hits=%llu misses=%llu writebacks=%llu evictions=%llu\n",
           (unsigned long long)fs.bcache.hits,
           (unsigned long long)fs.bcache.misses,
           (unsigned long long)fs.bcache.writebacks,
           (unsigned long long)fs.bcache.evictions);

    fs_umount(&fs);
    return 0;
}
