#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>

#include "fs.h"

#define PATH_MAX 4096
#define FD_MAX   32

#define CACHE_MEM 30
#define DIRECT_BLKS 6

// ===== 전역 파티션=====
static struct partition g_part;

// 현재 디렉토리 inode 번호
static unsigned int cnt_ino;

// ===== fd table =====
struct file {
    int used;
    struct inode *ino;
    unsigned int offset;
    int flags;
};
static struct file fdtab[FD_MAX];

// 캐시 요소
struct cache_ent {
    int valid;
    unsigned int dir_ino;
    char name[256];
    int ino; // >=0 inode 번호, -1 없음을 캐시(negative cache)
};
static struct cache_ent cache[CACHE_MEM];

struct shared_cache {
    struct cache_ent ent[CACHE_MEM];
    unsigned int rr;
};

static struct shared_cache *g_cache = NULL;


// ===== util =====
static void die(const char *msg){ perror(msg); exit(1); }

static inline uint64_t nsec_now(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline double ns_to_ms(uint64_t ns){ return (double)ns / 1e6; }

static int is_space(char c){
    return c==' ' || c=='\t' || c=='\r' || c=='\n';
}

// ===== mount =====
static void mount_root(const char *imgpath) {
    int fd = open(imgpath, O_RDONLY);
    if (fd < 0) die("open disk.img");

    ssize_t got = read(fd, &g_part, sizeof(g_part));
    if (got < (ssize_t)sizeof(struct super_block)) die("read disk.img");
    close(fd);

    if (g_part.s.partition_type != SIMPLE_PARTITION) {
        fprintf(stderr, "invalid partition_type: 0x%x\n", g_part.s.partition_type);
        exit(1);
    }
    if (g_part.s.block_size != BLOCK_SIZE) {
        fprintf(stderr, "unexpected block_size: %u\n", g_part.s.block_size);
        exit(1);
    }

    char vol[25];
    memcpy(vol, g_part.s.volume_name, 24);
    vol[24] = '\0';
    printf("Mounted FS: volume=\"%s\" block_size=%u first_inode=%u first_data_block=%u\n",
           vol, g_part.s.block_size, g_part.s.first_inode, g_part.s.first_data_block);
}

// ===== fs helpers =====
static struct inode *get_inode(unsigned int ino) {
    if (ino >= g_part.s.num_inodes) return NULL;
    return &g_part.inode_table[ino];
}

static unsigned char *get_block(unsigned short blk) {
    if (blk >= g_part.s.num_blocks) return NULL;
    return g_part.data_blocks[blk].d;
}

// ===== cache =====
static int cache_lookup(unsigned int dir_ino, const char *name, int *out_ino){
    for (int i = 0; i < CACHE_MEM; i++){
        struct cache_ent *e = &g_cache->ent[i];
        if (!e->valid) continue;
        if (e->dir_ino != dir_ino) continue;
        if (strncmp(e->name, name, sizeof(e->name)) == 0){
            *out_ino = e->ino;
            return 1;
        }
    }
    return 0;
}

static void cache_insert(unsigned int dir_ino, const char *name, int ino){
    for (int i = 0; i < CACHE_MEM; i++){
        struct cache_ent *e = &g_cache->ent[i];
        if (!e->valid) continue;
        if (e->dir_ino != dir_ino) continue;
        if (strncmp(e->name, name, sizeof(e->name)) == 0){
            e->ino = ino;
            return;
        }
    }

    for (int i = 0; i < CACHE_MEM; i++){
        struct cache_ent *e = &g_cache->ent[i];
        if (!e->valid){
            e->valid = 1;
            e->dir_ino = dir_ino;
            snprintf(e->name, sizeof(e->name), "%s", name);
            e->ino = ino;
            return;
        }
    }

    unsigned int idx = g_cache->rr % CACHE_MEM;
    g_cache->rr = (g_cache->rr + 1) % CACHE_MEM;

    struct cache_ent *e = &g_cache->ent[idx];
    e->valid = 1;
    e->dir_ino = dir_ino;
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->ino = ino;
}

static void cache_clear(void){
    memset(g_cache, 0, sizeof(*g_cache));
}

// ===== directory scan/find =====
static int find_inode(const char *name) {
    int cached;
    if (cache_lookup(cnt_ino, name, &cached)) {
        return cached; // -1도 그대로 반환
    }

    struct inode *dir = get_inode(cnt_ino);
    if (!dir) return -1;

    unsigned int dir_size = dir->size;
    unsigned int blk_size = g_part.s.block_size;
    unsigned int offset = 0;

    while (offset < dir_size) {
        unsigned int logical_blk  = offset / blk_size;
        unsigned int off_in_block = offset % blk_size;
        if (logical_blk >= DIRECT_BLKS) break;

        unsigned short phys_blk = dir->blocks[logical_blk];
        unsigned char *blk = get_block(phys_blk);
        if (!blk) break;

        struct dentry *d = (struct dentry *)(blk + off_in_block);
        if (d->dir_length == 0) break;

        if (d->inode != INVALID_INODE && d->name_len > 0 && d->name_len < sizeof(d->name)) {
            char namebuf[257];
            memcpy(namebuf, d->name, d->name_len);
            namebuf[d->name_len] = '\0';

            cache_insert(cnt_ino, namebuf, (int)d->inode);

            if (strcmp(namebuf, name) == 0) {
                return (int)d->inode;
            }
        }

        offset += d->dir_length;
    }

    cache_insert(cnt_ino, name, -1);
    return -1;
}

static void list_dir(void) {
    struct inode *dir = get_inode(cnt_ino);
    if (!dir) {
        fprintf(stderr, "dir inode not found (ino=%u)\n", cnt_ino);
        return;
    }

    printf("\n=== directory (inode %u) ===\n", cnt_ino);

    unsigned int dir_size = dir->size;
    unsigned int blk_size = g_part.s.block_size;
    unsigned int offset = 0;

    while (offset < dir_size) {
        unsigned int logical_blk  = offset / blk_size;
        unsigned int off_in_block = offset % blk_size;
        if (logical_blk >= DIRECT_BLKS) break;

        unsigned short phys_blk = dir->blocks[logical_blk];
        unsigned char *blk = get_block(phys_blk);
        if (!blk) break;

        struct dentry *d = (struct dentry *)(blk + off_in_block);
        if (d->dir_length == 0) break;
        if (d->inode == INVALID_INODE) { offset += d->dir_length; continue; }

        if (d->name_len > 0 && d->name_len < sizeof(d->name)) {

            // 캐시 저장
            char namebuf[257];
            memcpy(namebuf, d->name, d->name_len);
            namebuf[d->name_len] = '\0';

            cache_insert(cnt_ino, namebuf, (int)d->inode);

            // 파일 메타데이터 출력
            struct inode *fi = get_inode(d->inode);
            char type = '?';
            unsigned int fsize = 0;
            if (fi) {
                fsize = fi->size;
                if (d->file_type == DENTRY_TYPE_DIR_FILE) type = 'd';
                else if (d->file_type == DENTRY_TYPE_REG_FILE) type = '-';
            }

            printf("%c inode=%3u size=%6u name=%s\n",
                   type, d->inode, fsize, namebuf);
        }

        offset += d->dir_length;
    }
}

// ===== open/read/write/close =====
static int can_read(const struct inode *ino){
    return (ino->mode & (INODE_MODE_AC_USER_R|INODE_MODE_AC_GRP_R|INODE_MODE_AC_OTHER_R)) != 0;
}
static int can_write(const struct inode *ino){
    return (ino->mode & (INODE_MODE_AC_USER_W|INODE_MODE_AC_GRP_W|INODE_MODE_AC_OTHER_W)) != 0;
}

static int my_open(const char *path, int flags) {
    const char *name = path;
    if (path[0] == '/') name = path + 1;

    int ino_num = find_inode(name);
    if (ino_num < 0) return -1;

    struct inode *ino = get_inode((unsigned)ino_num);
    if (!ino) return -1;

    int acc = flags & O_ACCMODE;
    if (acc == O_RDONLY) {
        if (!can_read(ino)) return -1;
    } else if (acc == O_WRONLY) {
        if (!can_write(ino)) return -1;
    } else if (acc == O_RDWR) {
        if (!can_read(ino) || !can_write(ino)) return -1;
    }

    for (int i = 0; i < FD_MAX; i++){
        if (!fdtab[i].used){
            fdtab[i].used = 1;
            fdtab[i].ino = ino;
            fdtab[i].offset = 0;
            fdtab[i].flags = flags;
            return i;
        }
    }
    return -1;
}

static ssize_t my_read(int fd, void *buf, size_t count) {
    if (fd < 0 || fd >= FD_MAX || !fdtab[fd].used) return -1;

    struct file *f = &fdtab[fd];
    unsigned int size = f->ino->size;
    unsigned int blk_size = g_part.s.block_size;

    if (f->offset >= size) return 0;
    if (count > size - f->offset) count = size - f->offset;

    unsigned char *out = (unsigned char*)buf;
    size_t remain = count;

    while (remain > 0) {
        unsigned int logical_blk  = f->offset / blk_size;
        unsigned int off_in_block = f->offset % blk_size;
        if (logical_blk >= DIRECT_BLKS) break;

        unsigned short phys_blk = f->ino->blocks[logical_blk];
        unsigned char *blk = get_block(phys_blk);
        if (!blk) break;

        size_t can = blk_size - off_in_block;
        if (can > remain) can = remain;

        memcpy(out, blk + off_in_block, can);
        out += can;
        f->offset += (unsigned int)can;
        remain -= can;
    }
    return (ssize_t)(count - remain);
}

static ssize_t my_write(int fd, const void *buf, size_t count){
    if (fd < 0 || fd >= FD_MAX || !fdtab[fd].used) return -1;

    struct file *f = &fdtab[fd];
    unsigned int blk_size = g_part.s.block_size;

    int acc = f->flags & O_ACCMODE;
    if (!(acc == O_WRONLY || acc == O_RDWR)) return -1;

    unsigned int max_size = DIRECT_BLKS * blk_size;
    if (f->offset >= max_size) return 0;
    if (count > max_size - f->offset) count = max_size - f->offset;

    const unsigned char *in = (const unsigned char*)buf;
    size_t remain = count;

    while (remain > 0) {
        unsigned int logical_blk  = f->offset / blk_size;
        unsigned int off_in_block = f->offset % blk_size;
        if (logical_blk >= DIRECT_BLKS) break;

        unsigned short phys_blk = f->ino->blocks[logical_blk];
        unsigned char *blk = get_block(phys_blk);
        if (!blk) break;

        size_t can = blk_size - off_in_block;
        if (can > remain) can = remain;

        memcpy(blk + off_in_block, in, can);
        in += can;
        f->offset += (unsigned int)can;
        remain -= can;
    }

    // size 갱신(메모리만)
    if (f->offset > f->ino->size) f->ino->size = f->offset;

    return (ssize_t)(count - remain);
}

static int my_close(int fd){
    if (fd < 0 || fd >= FD_MAX || !fdtab[fd].used) return -1;
    fdtab[fd].used = 0;
    fdtab[fd].ino = NULL;
    fdtab[fd].offset = 0;
    fdtab[fd].flags = 0;
    return 0;
}

// ===== timed cat =====
static uint64_t cat_file_timed(const char *path, int quiet) {
    uint64_t t0 = nsec_now();

    int fd = my_open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "cat: open fail (%s)\n", path);
        return nsec_now() - t0;
    }

    struct inode *ino = fdtab[fd].ino;
    if (!ino || !(ino->mode & INODE_MODE_REG_FILE)) {
        fprintf(stderr, "cat: not a regular file (%s)\n", path);
        my_close(fd);
        return nsec_now() - t0;
    }

    char buf[1024];
    ssize_t n;
    while ((n = my_read(fd, buf, sizeof(buf))) > 0) {
        if (!quiet) fwrite(buf, 1, (size_t)n, stdout);
    }

    if (n < 0) perror("my_read");

    my_close(fd);

    return nsec_now() - t0;
}

static void start_file_1_to_30(void){
    uint64_t total0 = nsec_now();

    for (int i = 1; i <= 30; i++){
        char name[256];
        snprintf(name, sizeof(name), "file_%d", i);

        uint64_t dt = cat_file_timed(name, 1);
        printf("cat %s : %.3f ms\n", name, ns_to_ms(dt));

    }

    uint64_t total = nsec_now() - total0;
    printf("total(10 files) : %.3f ms\n", ns_to_ms(total));
}

// start: 현재 디렉토리에서 "일반 파일" 중 랜덤 10개 cat
static void start_random_10(void){
    srand((unsigned)(time(NULL) ^ getpid()));

    int idx[100];
    for (int i = 0; i < 100; i++) idx[i] = i + 1;

    // Fisher-Yates: 앞쪽 10개만 뽑히도록 부분 셔플
    for (int i = 0; i < 10; i++) {
        int j = i + (rand() % (100 - i));
        int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
    }

    uint64_t total0 = nsec_now();

    for (int k = 0; k < 10; k++) {
        char name[256];
        snprintf(name, sizeof(name), "file_%d", idx[k]);

        uint64_t dt = cat_file_timed(name, 0);
        printf("cat %s : %.3f ms\n\n", name, ns_to_ms(dt));
    }

    uint64_t total = nsec_now() - total0;
    printf("total(10 files from 1..100) : %.3f ms\n", ns_to_ms(total));

}

// ===== path prompt =====
static void path_cd(char *path, size_t cap, const char *name){
    if (strcmp(name, "/") == 0) { snprintf(path, cap, "/"); return; }

    if (strcmp(name, ".") == 0) return;

    if (strcmp(name, "..") == 0) {
        size_t len = strlen(path);
        if (len <= 1) { snprintf(path, cap, "/"); return; }
        if (path[len-1] == '/') path[len-1] = '\0';
        char *p = strrchr(path, '/');
        if (!p) { snprintf(path, cap, "/"); return; }
        if (p == path) path[1] = '\0';
        else *(p+1) = '\0';
        return;
    }

    size_t len = strlen(path);
    if (len == 0) { snprintf(path, cap, "/"); len = 1; }
    if (path[len-1] != '/') {
        if (len+1 < cap) { path[len] = '/'; path[len+1] = '\0'; len++; }
    }
    if (len + strlen(name) + 1 >= cap) return;
    strncat(path, name, cap - strlen(path) - 1);
    strncat(path, "/", cap - strlen(path) - 1);
}

// write 텍스트(3번째 토큰 이후 나머지) 포인터 찾기
static char* text_after_3tokens(char *line_raw){
    char *p = line_raw;
    for (int k = 0; k < 3; k++){
        while (*p && is_space(*p)) p++;
        while (*p && !is_space(*p)) p++;
    }
    while (*p && is_space(*p)) p++;
    // trailing newline 제거
    char *q = p + strlen(p);
    while (q > p && (q[-1] == '\n' || q[-1] == '\r')) q--;
    *q = '\0';
    return p; // empty string 가능
}

// ===== write (메모리만 수정) =====
static void cmd_write(const char *name, const char *off_s, char *text){
    if (!name || !*name) { fprintf(stderr, "write: missing file\n"); return; }
    if (!off_s || !*off_s) { fprintf(stderr, "write: missing offset\n"); return; }
    if (!text) text = (char*)"";

    int fd = my_open(name, O_WRONLY);
    if (fd < 0) { fprintf(stderr, "write: open fail (%s)\n", name); return; }

    struct inode *ino = fdtab[fd].ino;
    if (!ino || !(ino->mode & INODE_MODE_REG_FILE)) {
        fprintf(stderr, "write: not a regular file (%s)\n", name);
        my_close(fd);
        return;
    }

    long long off = atoll(off_s);
    if (off < 0) off = 0;
    fdtab[fd].offset = (unsigned int)off;

    uint64_t t0 = nsec_now();
    ssize_t n = my_write(fd, text, strlen(text));
    uint64_t t1 = nsec_now();

    my_close(fd);

    if (n < 0) fprintf(stderr, "write: error\n");
    else printf("[write] %s: %zd bytes at %lld (%.3f ms)\n", name, n, off, ns_to_ms(t1 - t0));
}


int main(int argc, char *argv[])
{
    const char *img = "disk.img";
    if (argc >= 2) img = argv[1];

    mount_root(img);
    memset(fdtab, 0, sizeof(fdtab));
    memset(cache, 0, sizeof(cache)); // (미사용) 변수 유지

    g_cache = mmap(NULL, sizeof(*g_cache),
                   PROT_READ|PROT_WRITE,
                   MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (g_cache == MAP_FAILED) die("mmap cache");
    memset(g_cache, 0, sizeof(*g_cache));

    cnt_ino = g_part.s.first_inode;
    list_dir();

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/");

    char *usr = getenv("USER");
    if (!usr) usr = (char*)"user";

    while (1) {
        printf("\n%s:%s > ", usr, path);
        fflush(stdout);

        char line_raw[2048];
        if (!fgets(line_raw, sizeof(line_raw), stdin)) break;

        // 토크나이즈용 복사본
        char line[2048];
        snprintf(line, sizeof(line), "%s", line_raw);

        char *save = NULL;
        char *args[10] = {0};
        int narg = 0;

        args[narg++] = strtok_r(line, " \t\r\n", &save);
        if (!args[0]) continue;
        while (narg < 9 && (args[narg] = strtok_r(NULL, " \t\r\n", &save)) != NULL) narg++;
        args[narg] = NULL;

        const char *cmd = args[0];

        if (!strcmp(cmd, "quit") || !strcmp(cmd, "q")) break;

        if (!strcmp(cmd, "clearcache") || !strcmp(cmd, "c")) { cache_clear(); printf("cache cleared\n"); continue; }

        if (!strcmp(cmd, "cd")) {
            if (!args[1]) { fprintf(stderr, "cd: missing operand\n"); continue; }

            if (!strcmp(args[1], "/")) {
                cnt_ino = g_part.s.first_inode;
                path_cd(path, sizeof(path), "/");
                continue;
            }

            int next = find_inode(args[1]);
            if (next < 0) { fprintf(stderr, "cd: no such dir\n"); continue; }

            struct inode *ni = get_inode((unsigned)next);
            if (!ni || !(ni->mode & INODE_MODE_DIR_FILE)) {
                fprintf(stderr, "cd: not a directory\n");
                continue;
            }

            cnt_ino = (unsigned)next;
            path_cd(path, sizeof(path), args[1]);
            continue;
        }
        
        if (!strcmp(cmd, "write")) { // write file_name offset text...
            if (!args[1] || !args[2]) {
                fprintf(stderr, "usage: write <file> <offset> <text...>\n");
                continue;
            }
            char *text = text_after_3tokens(line_raw);
            cmd_write(args[1], args[2], text); 
            continue;
        }


        // 자식 프로세스 생성
        int pid = fork();
        if(pid == -1){
            perror("fork error");
			return 0;
        } else if(pid > 0){
            // parent
            waitpid(pid, NULL, 0);
        } else{
            // child
            if (!strcmp(cmd, "ls")) {
                list_dir(); 
                _exit(0);
            }
            if (!strcmp(cmd, "cat")) {
                if (!args[1]) { fprintf(stderr, "cat: missing operand\n"); continue; }
                uint64_t dt = cat_file_timed(args[1], 0);
                printf("[cat] %s : %.3f ms\n", args[1], ns_to_ms(dt));
                _exit(0);
            }

            if (!strcmp(cmd, "start") || !strcmp(cmd, "s")) {
                //start_random_10();
                start_file_1_to_30();
                _exit(0);
            }
            
            fprintf(stderr, "unknown command: %s\n", cmd);
            fprintf(stderr, "available: ls, cd, cat, start, write, clearcache, quit\n");
            _exit(0);
        }
    }

    return 0;
}
