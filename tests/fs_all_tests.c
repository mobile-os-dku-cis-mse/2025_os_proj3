#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../include/fs.h"

int main() {


    const char *disk = "tests/test.img";
    char cwd[1024];
    char buf[1024];
    int res;

    // ...existing code...

    if (getcwd(cwd, sizeof(cwd))) {
        printf("[DEBUG] Current working directory: %s\n", cwd);
    }

    printf("[TEST] Mounting disk...\n");
    res = fs_mount(disk);
    printf("fs_mount: %d\n", res);


    // --- Test: Remove non-empty directory ---
    printf("[TEST] Creating directory 'baz'...\n");
    res = fs_makedir("baz");
    printf("fs_makedir: %d\n", res);

    printf("[TEST] Creating file 'baz/bazfile' in 'baz'...\n");
    res = fs_createfiel("baz/bazfile", 0);
    printf("fs_createfiel: %d\n", res);

    printf("[TEST] Attempting to remove non-empty directory 'baz' (should warn)...\n");
    res = fs_removedir("baz");
    printf("fs_removedir: %d\n", res);

    printf("[TEST] Deleting file 'bazfile'...\n");
    res = fs_delete("bazfile");
    printf("fs_delete: %d\n", res);

    printf("[TEST] Removing now-empty directory 'baz'...\n");
    res = fs_removedir("baz");
    printf("fs_removedir: %d\n", res);

    printf("[TEST] Creating file 'foo'...\n");
    res = fs_createfiel("foo", 0);
    printf("fs_createfiel: %d\n", res);

    printf("[TEST] Opening file 'foo'...\n");
    int fd = fs_open("foo", 0);
    printf("fs_open: %d\n", fd);

    printf("[TEST] Writing to 'foo'...\n");
    strcpy(buf, "Hello, world!");
    res = fs_write(fd, buf, strlen(buf));
    printf("fs_write: %d\n", res);

    printf("[TEST] Closing 'foo'...\n");
    res = fs_close(fd);
    printf("fs_close: %d\n", res);

    printf("[TEST] Re-opening 'foo' for reading...\n");
    fd = fs_open("foo", 0);
    printf("fs_open: %d\n", fd);

    printf("[TEST] Reading from 'foo'...\n");
    memset(buf, 0, sizeof(buf));
    res = fs_read(fd, buf, sizeof(buf)-1);
    printf("fs_read: %d, content: '%s'\n", res, buf);

    printf("[TEST] Closing 'foo'...\n");
    res = fs_close(fd);
    printf("fs_close: %d\n", res);

    printf("[TEST] Creating directory 'bar'...\n");
    res = fs_makedir("bar");
    printf("fs_makedir: %d\n", res);

    printf("[TEST] Deleting file 'foo'...\n");
    res = fs_delete("foo");
    printf("fs_delete: %d\n", res);

    printf("[TEST] Removing directory 'bar'...\n");
    res = fs_removedir("bar");
    printf("fs_removedir: %d\n", res);

    printf("[TEST] Unmounting disk...\n");
    res = fs_unmount();
    printf("fs_unmount: %d\n", res);

    return 0;
}
