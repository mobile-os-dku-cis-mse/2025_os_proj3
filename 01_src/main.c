// main.c

#define _POSIX_C_SOURCE 200809L // clock_gettime과 timespec을 사용하기 위한 POSIX C SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> 
#include <unistd.h>
#include "fs.h"

// ―――――――――――――――――――――――――――――― Global variables(전역 변수) ――――――――――――――――――――――――――――――
int is_mounted = 0;               // File system이 mount되어 있는 지 나타내는 flag
char mounted_disk_name[256] = ""; // Mount된 disk img file의 이름을 저장하는 변수

int g_buffer_cache_enabled = 0;   // Buffer cache의 사용 여부를 나타내는 전역 변수
int g_dentry_cache_enabled = 0;   // Directory entry cache의 사용 여부를 나타내는 전역 변수
int buffer_head = 0;              // Buffer cache에서 cache가 가득 찬 경우, First Input First Output(FIFO) 방식으로 가장 오래된 entry를 덮어쓰기 위한 pointer
long long g_cache_hits = 0;       // Cache Hit 횟수를 저장하는 전역 변수
long long g_cache_misses = 0;     // Cache Miss 횟수를 저장하는 전역 변수

// ―――――――――――――――――――――――――――――― Structures(구조체) ――――――――――――――――――――――――――――――
struct partition disk;              // Memory에 로드된 전체 디스크 이미지

struct dentry_cache_entry {
    char name[MAX_FILE_NAME_LEN];   // File의 이름
    int inode_num;                  // File의 i-node 번호
    int valid;                      // Cache entry의 사용 가능 여부
};
struct dentry_cache_entry dentry_cache[DENTRY_HASH_SIZE];

struct buffer_cache_entry {
    unsigned int block_num;         // Disk의 물리적 block 번호
    unsigned char data[BLOCK_SIZE]; // Block의 data
    int valid;                      // Cache entry의 사용 가능 여부
};
struct buffer_cache_entry buffer_cache[BUFFER_CACHE_SIZE];

// ―――――――――――――――――――――――――――――― Cache function(cache 관련 함수) ――――――――――――――――――――――――――――――
// Directory entry cache와 buffer cache와 cache hit과 miss 횟수를 초기화하는 함수
void init_caches(void) {
    memset(dentry_cache, 0, sizeof(dentry_cache));
    memset(buffer_cache, 0, sizeof(buffer_cache));
    g_dentry_cache_enabled = 0;
    g_buffer_cache_enabled = 0;
    g_cache_hits = 0;
    g_cache_misses = 0;
}

// Directory entry cache에서 indexing을 하기 위한 문자열 hash 함수
unsigned int hash_func(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) hash = ((hash << 5) + hash) + c;
    return hash % DENTRY_HASH_SIZE;
}

// File의 이름을 통해 i-node 번호 cache를 조회하는 함수
int lookup_dentry_cache(const char *name) {
    if (!g_dentry_cache_enabled) return -1;
    unsigned int idx = hash_func(name);
    if (dentry_cache[idx].valid && strcmp(dentry_cache[idx].name, name) == 0) {
        g_cache_hits++; 
        return dentry_cache[idx].inode_num;
    }
    g_cache_misses++; 
    return -1;
}

// 새 항목을 directory entry cache에 추가하는 함수
void insert_dentry_cache(const char *name, int inode_num) {
    if (!g_dentry_cache_enabled) return;
    unsigned int idx = hash_func(name);
    strncpy(dentry_cache[idx].name, name, MAX_FILE_NAME_LEN - 1);
    dentry_cache[idx].inode_num = inode_num;
    dentry_cache[idx].valid = 1;
}

// 물리적 block 번호를 memory의 data block pointer로 변환하는 함수
unsigned char* get_block_ptr(unsigned int physical_block_num) {
    // 물리적 block 번호가 범위를 초과한 경우, NULL 반환
    if (physical_block_num < disk.s.first_data_block || physical_block_num >= disk.s.num_blocks) {
        return NULL;
    }
    return disk.data_blocks[physical_block_num - disk.s.first_data_block].d;
}

// Block을 읽는 함수
int read_block_with_cache(unsigned int block_num, void *buf) {
    // Buffer cache를 탐색
    if (g_buffer_cache_enabled) {
        for (int i = 0; i < BUFFER_CACHE_SIZE; i++) {
            // Hit인 경우
            if (buffer_cache[i].valid && buffer_cache[i].block_num == block_num) {
                memcpy(buf, buffer_cache[i].data, BLOCK_SIZE);
                g_cache_hits++; 
                return 0;
            }
        }
    }
    g_cache_misses++; 

    // Miss인 경우 memory에 불러온 disk 구조체에서 block을 읽음
    unsigned char *ptr = get_block_ptr(block_num);
    if (!ptr) return -1;
    memcpy(buf, ptr, BLOCK_SIZE);

    // FIFO 방식으로 cache를 update
    if (g_buffer_cache_enabled) {
        buffer_cache[buffer_head].block_num = block_num;
        memcpy(buffer_cache[buffer_head].data, buf, BLOCK_SIZE);
        buffer_cache[buffer_head].valid = 1;
        buffer_head = (buffer_head + 1) % BUFFER_CACHE_SIZE;
    }
    return 0;
}

// Block을 쓰는 함수
void write_block_with_cache(unsigned int block_num, const void *buf) {
    // Disk를 update
    unsigned char *ptr = get_block_ptr(block_num);
    if (!ptr) return;
    memcpy(ptr, buf, BLOCK_SIZE);

    // Cache가 존재하는 경우 update
    if (g_buffer_cache_enabled) {
        for (int i = 0; i < BUFFER_CACHE_SIZE; i++) {
            if (buffer_cache[i].valid && buffer_cache[i].block_num == block_num) {
                memcpy(buffer_cache[i].data, buf, BLOCK_SIZE);
                return;
            }
        }

        // Cache가 존재하지 않는 경우 FIFO 방식으로 cache를 update
        buffer_cache[buffer_head].block_num = block_num;
        memcpy(buffer_cache[buffer_head].data, buf, BLOCK_SIZE);
        buffer_cache[buffer_head].valid = 1;
        buffer_head = (buffer_head + 1) % BUFFER_CACHE_SIZE;
    }
}

// Cache의 사용 여부와 hit와 miss의 횟수를 출력하는 함수
void fs_cache_stat(void) {
    printf("―――――――――― Cache Status ――――――――――\n");
    printf("ㆍDentry Cache: %s\n", g_dentry_cache_enabled ? "ON" : "OFF");
    printf("ㆍBuffer Cache: %s\n", g_buffer_cache_enabled ? "ON" : "OFF");
    printf("ㆍTotal Cache Hits: %lld\n", g_cache_hits);
    printf("ㆍTotal Cache Misses: %lld\n", g_cache_misses);
    printf("――――――――――――――――――――――――――――――――――\n");
}

// ―――――――――――――――――――――――――――――― File system function(file system 관련 함수) ――――――――――――――――――――――――――――――
// Bitmap을 사용하지 않고, 새로운 data block을 순차적으로 할당하는 함수
int allocate_data_block(void) {
    static int next_free = 1; 
    if (next_free < MAX_BLOCKS) {
         int phys = disk.s.first_data_block + next_free;
         next_free++;
         return phys;
    }
    return 0;
}

// Memory의 disk 구조체의 data를 disk file에 저장하는 함수
void sync_disk(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (fp) {
        fwrite(&disk, 1, sizeof(struct partition), fp);
        fclose(fp);
        printf("ㆍDisk Synced\n");
    } else {
        perror("ㆍDisk Sync failed\n");
    }
}

// File system을 생성하는 함수
void fs_mkfs(const char *filename) {
    memset(&disk, 0, sizeof(struct partition));
    disk.s.partition_type = SIMPLE_PARTITION;
    disk.s.block_size = BLOCK_SIZE;
    disk.s.inode_size = sizeof(struct inode);
    disk.s.first_inode = 1; 
    disk.s.num_inodes = MAX_INODES;
    disk.s.num_inode_blocks = 7;
    disk.s.num_blocks = MAX_BLOCKS + 8;
    disk.s.first_data_block = 8;
    disk.s.num_free_blocks = MAX_BLOCKS;
    disk.s.num_free_inodes = MAX_INODES;
    snprintf(disk.s.volume_name, 24, "SimpleFS_Volume");
    int root_idx = 2; 
    disk.inode_table[root_idx].mode = INODE_MODE_DIR_FILE | 0x777;
    disk.inode_table[root_idx].size = 128;
    disk.inode_table[root_idx].date = time(NULL);
    disk.inode_table[root_idx].blocks[0] = disk.s.first_data_block;
    disk.s.num_free_blocks--;
    disk.s.num_free_inodes--; 
    unsigned char *root_block = disk.data_blocks[0].d; 
    struct dentry *d1 = (struct dentry *)root_block;
    d1->inode = root_idx + 1;
    strcpy((char*)d1->name, ".");
    d1->name_len = 1;
    d1->file_type = DENTRY_TYPE_DIR_FILE;
    d1->dir_length = 64;
    struct dentry *d2 = (struct dentry *)(root_block + 64);
    d2->inode = root_idx + 1;
    strcpy((char*)d2->name, "..");
    d2->name_len = 2;
    d2->file_type = DENTRY_TYPE_DIR_FILE;
    d2->dir_length = 64;
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("ㆍFile System make failed\n");
        return;
    }
    fwrite(&disk, 1, sizeof(struct partition), fp);
    fclose(fp);
    printf("ㆍFile System maked(%s)\n", filename);
}

// Disk image를 불러오는 함수
void fs_mount(const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("ㆍFile system mount failed\n");
        return;
    }
    size_t read_cnt = fread(&disk, 1, sizeof(struct partition), fp);
    if (read_cnt < sizeof(struct super_block)) {
        printf("ㆍImage is invalid\n");
        fclose(fp);
        return;
    }
    if (disk.s.partition_type != SIMPLE_PARTITION) {
        printf("ㆍMagic number is bad\n");
        fclose(fp);
        return;
    }
    is_mounted = 1;
    init_caches();
    printf("ㆍFile system Mounted(%s, Root Inode Index: 2)\n", filename);
    fclose(fp);
}

// ls 함수
void fs_ls(void) {
    if (!is_mounted) return;
    struct inode *root = &disk.inode_table[2]; 
    printf("Name\t\tInode\tType\n");
    printf("――――――――――――――――――――――――――――\n");
    for (int i = 0; i < 6; i++) {
        if (root->blocks[i] == 0) continue;
        unsigned char buf[BLOCK_SIZE];
        if (read_block_with_cache(root->blocks[i], buf) < 0) continue;
        int offset = 0;
        while (offset < BLOCK_SIZE) {
            struct dentry *entry = (struct dentry *)(buf + offset);
            if (entry->inode == 0) break;
            if (entry->dir_length == 0 || offset + entry->dir_length > BLOCK_SIZE) break;
            printf("%-10s\t%d\t%s\n", 
                   entry->name, entry->inode, 
                   (entry->file_type == DENTRY_TYPE_DIR_FILE) ? "DIR" : "FILE");
            offset += entry->dir_length;
        }
    }
}

// cat 함수
void fs_cat(const char *filename) {
    if (!is_mounted) return;
    
    // 시간 측정 시작
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    struct inode *root = &disk.inode_table[2];
    int inode_num = -1;

    // Directory entry cache 사용
    inode_num = lookup_dentry_cache(filename);
    if (inode_num == -1) {
        for (int i = 0; i < 6; i++) {
            if (root->blocks[i] == 0) continue;
            unsigned char buf[BLOCK_SIZE];
            read_block_with_cache(root->blocks[i], buf);
            int offset = 0;
            while (offset < BLOCK_SIZE) {
                struct dentry *d = (struct dentry *)(buf + offset);
                if (d->inode == 0) break;
                if (strcmp((char*)d->name, filename) == 0) {
                    inode_num = d->inode;
                    insert_dentry_cache(filename, inode_num);
                    break;
                }
                offset += d->dir_length;
            }
            if (inode_num != -1) break;
        }
    }

    if (inode_num == -1) {
        printf("ㆍFile is not exist\n");

        // Error가 발생한 경우에서 시간 측정 종료
        clock_gettime(CLOCK_MONOTONIC, &end);
        long long elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
        printf("ㆍcat time: %lld ns\n", elapsed_ns);
        return;
    }

    // File의 data를 읽음
    struct inode *file_inode = &disk.inode_table[inode_num - 1];
    if (file_inode->mode & INODE_MODE_DIR_FILE) {
        printf("ㆍIt is directory\n");
        goto end_cat_perf;
    }
    int remaining = file_inode->size;
    printf("―――――――――― Data in the file(%d bytes) ――――――――――\n", remaining);
    for (int i = 0; i < 6; i++) {
        if (remaining <= 0) break;
        if (file_inode->blocks[i] == 0) break;

        unsigned char buf[BLOCK_SIZE];
        read_block_with_cache(file_inode->blocks[i], buf);
        
        int to_read = (remaining > BLOCK_SIZE) ? BLOCK_SIZE : remaining;
        for(int j=0; j<to_read; j++) putchar(buf[j]);
        remaining -= to_read;
    }
    printf("\n"); 
    printf("―――――――――――――――――――――――――――――――――――――――――――――――\n");

end_cat_perf:
    // 시간 측정 종료
    clock_gettime(CLOCK_MONOTONIC, &end);
    long long elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
    printf("ㆍcat time: %lld ns\n", elapsed_ns);
}

// File을 생성하거나 덮어쓰는 함수
void fs_write_file(const char *filename, const char *content) {
    if (!is_mounted) return;
    
    // 시간 측정 시작
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    struct inode *root = &disk.inode_table[2];
    int inode_idx = -1;

    // Directory entry cache 사용
    int existing_inode = lookup_dentry_cache(filename);
    if (existing_inode != -1) {
        inode_idx = existing_inode - 1; 
    } else {
        for(int i=0; i<6; i++) {
             if(root->blocks[i]==0) continue;
             unsigned char buf[BLOCK_SIZE];
             read_block_with_cache(root->blocks[i], buf);
             int offset=0;
             while(offset < BLOCK_SIZE) {
                 struct dentry *d = (struct dentry*)(buf+offset);
                 if(d->inode == 0) break;
                 if(strcmp((char*)d->name, filename)==0) {
                     existing_inode = d->inode;
                     insert_dentry_cache(filename, existing_inode);
                     break;
                 }
                 offset += d->dir_length;
             }
             if(existing_inode != -1) break;
        }
        if (existing_inode != -1) {
            inode_idx = existing_inode - 1;
        }
    }

    if (existing_inode == -1) {
        // 새 File을 생성
        for (int i = 0; i < MAX_INODES; i++) {
            if (disk.inode_table[i].mode == 0) {
                inode_idx = i;
                break;
            }
        }
        if (inode_idx == -1) { printf("ㆍi-node is not free\n"); goto end_write_perf; }
        disk.inode_table[inode_idx].mode = INODE_MODE_REG_FILE | 0x777;
        disk.inode_table[inode_idx].date = time(NULL);
        disk.inode_table[inode_idx].size = 0;
        int added = 0;
        for (int i = 0; i < 6; i++) {
            if (root->blocks[i] == 0) continue;
            unsigned char buf[BLOCK_SIZE];
            read_block_with_cache(root->blocks[i], buf);
            int offset = 0;
            while (offset < BLOCK_SIZE) {
                struct dentry *d = (struct dentry *)(buf + offset);
                if (d->inode == 0) {
                    d->inode = inode_idx + 1;
                    strncpy((char*)d->name, filename, MAX_FILE_NAME_LEN-1);
                    d->name_len = strlen(filename);
                    d->file_type = DENTRY_TYPE_REG_FILE;
                    d->dir_length = 64;
                    write_block_with_cache(root->blocks[i], buf);
                    added = 1;
                    insert_dentry_cache(filename, inode_idx + 1);
                    break;
                }
                offset += d->dir_length;
                if (offset >= BLOCK_SIZE) break;
            }
            if (added) break;
        }
        if (!added) { printf("ㆍRoot directory is full\n"); goto end_write_perf; }
        printf("ㆍFile created(%s, i-node %d)\n", filename, inode_idx + 1);
    }

    // Data를 씀
    struct inode *file_inode = &disk.inode_table[inode_idx];
    size_t len = strlen(content);
    int required_blocks = (len + BLOCK_SIZE - 1) / BLOCK_SIZE;
    int written = 0;
    for (int i = 0; i < required_blocks && i < 6; i++) {
        if (file_inode->blocks[i] == 0) {
            file_inode->blocks[i] = allocate_data_block();
            if (file_inode->blocks[i] == 0) {
                printf("ㆍWriting stopped because can not allocate data block\n");
                break;
            }
        }
        unsigned char buf[BLOCK_SIZE];
        memset(buf, 0, BLOCK_SIZE);
        int chunk = (len - written > BLOCK_SIZE) ? BLOCK_SIZE : (len - written);
        memcpy(buf, content + written, chunk);
        write_block_with_cache(file_inode->blocks[i], buf);
        written += chunk;
    }
    file_inode->size = written;
    printf("ㆍ%d bytes is written in %s\n", written, filename);

end_write_perf:
    // 시간 측정 종료
    clock_gettime(CLOCK_MONOTONIC, &end);
    long long elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
    printf("ㆍwrite time: %lld ns\n", elapsed_ns);
}

// ―――――――――――――――――――――――――――――― main function(main 함수) ――――――――――――――――――――――――――――――
int main(int argc, char **argv) {
    char cmd[256];
    char arg1[256];
    char arg2[256];
    (void)argc;
    (void)argv;
    printf("―――――――――― Simple file system command ――――――――――\n");
    printf("ㆍMake file system: mkfs [file]\n");
    printf("ㆍMount: mount [file]\n");
    printf("ㆍWrite: [file] [data]\n");
    printf("ㆍcat: cat [file]\n");
    printf("ㆍls: ls\n");
    printf("ㆍSet cache: set_cache [dentry|buffer] [on|off]\n");
    printf("ㆍCache stat: cache_stat\n");
    printf("ㆍExit: exit\n");
    while (1) {
        printf("Command> ");
        if (scanf("%s", cmd) == EOF) break;
        if (strcmp(cmd, "exit") == 0) {
            if (is_mounted) {
                printf("ㆍUnmounting and syncing in %s\n", mounted_disk_name);
                sync_disk(mounted_disk_name); 
            }
            break;
        } 
        else if (strcmp(cmd, "mkfs") == 0) {
            if (scanf("%s", arg1) == 1) {
                fs_mkfs(arg1);
            }
        }
        else if (strcmp(cmd, "mount") == 0) {
            if (scanf("%s", arg1) == 1) {
                fs_mount(arg1);
                if (is_mounted) {
                    strncpy(mounted_disk_name, arg1, sizeof(mounted_disk_name) - 1);
                }
            }
        } 
        else if (strcmp(cmd, "ls") == 0) {
            if (!is_mounted) printf("ㆍFile system is not mounted\n");
            else fs_ls();
        } 
        else if (strcmp(cmd, "cat") == 0) {
            if (scanf("%s", arg1) == 1) {
                if (!is_mounted) printf("ㆍFile system is not mounted\n");
                else fs_cat(arg1);
            }
        } 
        else if (strcmp(cmd, "write") == 0) {
            if (scanf("%s", arg1) == 1 && scanf("%s", arg2) == 1) {
                if (!is_mounted) printf("ㆍFile system is not mounted\n");
                else fs_write_file(arg1, arg2);
            }
        }
        else if (strcmp(cmd, "cache_stat") == 0) {
            fs_cache_stat();
        }
        else if (strcmp(cmd, "set_cache") == 0) { 
            if (scanf("%s", arg1) == 1 && scanf("%s", arg2) == 1) {
                int state = (strcmp(arg2, "on") == 0) ? 1 : 0;
                if (strcmp(arg1, "dentry") == 0) {
                    g_dentry_cache_enabled = state;
                    printf("ㆍDentry Cache is %s\n", state ? "ON" : "OFF");
                } else if (strcmp(arg1, "buffer") == 0) {
                    g_buffer_cache_enabled = state;
                    printf("ㆍBuffer Cache is %s\n", state ? "ON" : "OFF");
                } else {
                    printf("ㆍUsage: set_cache [dentry|buffer] [on|off]\n");
                }
            } else {
                printf("ㆍUsage: set_cache [dentry|buffer] [on|off]\n");
            }
        }
        else {
            printf("ㆍUnknown command\n");
            while(getchar() != '\n');
        }
    }
    return 0;
}