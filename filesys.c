#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fs.h"

#define INODE_TABLE_ENTRY_COUNT 224
#define CACHE_SIZE 5

FILE *disk;
struct super_block sb;
struct inode *inode_table;
long data_region_start;

struct file_desc {
    int valid;
    int inode_index;
    int offset;
};

struct cache_entry{
    int valid;
    int physical_block_num;
    int last_access_time;
    char data[BLOCK_SIZE];
};

struct file_desc open_file_table[32];
struct cache_entry buffer_cache[CACHE_SIZE];
int timer = 0;

void mount(const char *disk_name);
int open(const char *filename);
int read(int fd, void *buf, int size);
void close(int fd);

int main(){
    printf("Mounting disk image. \n");
    mount("disk.img");
    int dir_fd = open(".");
    close(dir_fd);

    srand((unsigned int)time(NULL));
    printf("\n Starting Random File Access (10 Files).\n");
    printf("======================================================\n");

    int success_count = 0;
    int try_count = 0;
    int request_count = 10;
    while(success_count < request_count){
        try_count++;
        int file_num = (rand() % 100) + 1;
        char filename[20];
        sprintf(filename, "file_%d", file_num);

        int fd = open(filename);

        if(fd == -1) continue;
        char buf[101] = {0,};
        int n = read(fd, buf, 100);

        if (n > 0 && buf[n - 1] == '\n'){
            buf[n - 1] = '\0';
        }

        printf("\n[%d/%d Success] File Operation Log:\n", success_count + 1,request_count);
        printf("  -> [Open]  Filename: \"%s\", FD: %d\n", filename, fd);
        printf("  -> [Read]  %d bytes read\n", n);
        printf("  -> [Data]  \"%s\"\n", buf);
        close(fd);
        printf("  -> [CLOSE] FD : %d\n\n",fd);
        success_count++;
    }
    char *filename = "file_1";
    int fd = open(filename);
    char buf[4096] = {0,};
    int n = read(fd, buf, 4095);
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
    printf("\n  -> [Open]  Filename: \"%s\", FD: %d\n", filename, fd);
    printf("  -> [Read]  %d bytes read\n", n);
    printf("  -> [Data]  \"%s\"\n", buf);
    close(fd);
    printf("  -> [CLOSE] FD : %d\n", fd);
    printf("======================================================\n");
    printf("Simulation Finished. (Total attempts: %d)\n", try_count);
    return 0;
}

void mount(const char *disk_name){
    disk = fopen(disk_name, "rb");
    if(disk == NULL){
        perror("Cannot open disk image");
        exit(1);
    }
    fseek(disk, 0, SEEK_SET); // 슈퍼블록 읽기
    fread(&sb, sizeof(struct super_block), 1, disk);
    if(sb.partition_type != SIMPLE_PARTITION){
        printf("Invalid partition type.");
    }
    printf(">> Superblock loaded: Block Size: %d, Inode Count: %d\n", sb.block_size,sb.num_inodes);
    int table_size = sb.num_inodes * sizeof(struct inode); // 저장공간 절약을 위한 동적할당
    inode_table = (struct inode *)malloc(table_size);
    if(inode_table == NULL){
        perror("Memory allocation failed");
        exit(1);
    }

    long fixed_table_size = INODE_TABLE_ENTRY_COUNT * sizeof(struct inode); // 아이노드 테이블은 224개 고정
    data_region_start = sizeof(struct super_block) + fixed_table_size;
    long inode_table_start = sizeof(struct super_block); 
    fseek(disk, inode_table_start, SEEK_SET);
    fread(inode_table, sizeof(struct inode), sb.num_inodes, disk);
    int valid_files = 0;

    for (int i = 0; i < sb.num_inodes; i++){ // 0번부터 끝까지 모든 아이노드를 검사
        if (inode_table[i].mode != 0 && inode_table[i].size > 0) valid_files++;
    }
    printf("--------------------------------------------\n");
    printf(">> Total %d valid files found.\n", valid_files);
    for(int i = 0; i<CACHE_SIZE; i++){
        buffer_cache[i].valid = 0;
    }
    printf(">> Buffer Cache Initialized (%d slots).\n", CACHE_SIZE);
}

int open(const char *filename){
    struct inode *root = &inode_table[2]; // 검색을 위한 루트 아이노드
    int found_inode = -1;

    if (strcmp(filename, ".") == 0){
        found_inode = 2; // 루트 아이노드 번호
    }else{
        char buf[BLOCK_SIZE];
        int total_processed_size = 0;

        for (int i = 0; i < 6; i++) {
            if (total_processed_size >= root->size) break;
            if (found_inode != -1) break;

            long pos = data_region_start + (long)root->blocks[i] * BLOCK_SIZE;
            fseek(disk, pos, SEEK_SET);
            fread(buf, 1, BLOCK_SIZE, disk);

            int offset = 0;
            while (offset < BLOCK_SIZE && total_processed_size < root->size){
                struct dentry *de = (struct dentry *)(buf + offset);
                if (de->dir_length == 0) break;
                if (de->inode != 0 && strcmp((char *)de->name, filename) == 0){
                    found_inode = de->inode;
                    break;
                }
                offset += de->dir_length;
                total_processed_size += de->dir_length;
            }
        }
    }
    if (found_inode == -1) return -1; 
    int fd = -1;
    for (int i = 0; i < 32; i++){
        if (open_file_table[i].valid == 0){
            fd = i;
            break;
        }
    }
    if (fd == -1) return -1;

    open_file_table[fd].valid = 1;
    open_file_table[fd].inode_index = found_inode;
    open_file_table[fd].offset = 0;

    struct inode *target_inode = &inode_table[found_inode];

    if (target_inode->mode & INODE_MODE_DIR_FILE) {
        printf("\n[Directory Listing for '%s' (Inode %d)]\n", filename, found_inode);
        printf(" Name\t\t\tInode\tType\n");
        printf("--------------------------------------------\n");

        char dir_buf[BLOCK_SIZE];
        int dir_processed = 0;

        for (int i = 0; i < 6; i++) {
            if (dir_processed >= target_inode->size)break;

            long pos = data_region_start + (long)target_inode->blocks[i] * BLOCK_SIZE;
            fseek(disk, pos, SEEK_SET);
            fread(dir_buf, 1, BLOCK_SIZE, disk);

            int offset = 0;
            while (offset < BLOCK_SIZE && dir_processed < target_inode->size){
                struct dentry *de = (struct dentry *)(dir_buf + offset);
                if (de->dir_length == 0) break;
                if (de->inode != 0){
                    char *Filetype;
                    Filetype = (de->file_type == 0) ? "DIR" : "FILE";
                    struct inode *entry_inode = &inode_table[de->inode];
                    printf(" %-20s\t%d\t%s\t%d bytes\n", de->name, de->inode, Filetype, entry_inode->size);
                }
                offset += de->dir_length;
                dir_processed += de->dir_length;
            }
        }
        printf("--------------------------------------------\n");
    }
    return fd;
}

int read(int fd, void *buf, int size){
    if(fd < 0 || fd >= 32 || open_file_table[fd].valid == 0) return -1;
    struct file_desc *desc = &open_file_table[fd]; // valid, inode_index, offset으로 이루어짐
    struct inode *node = &inode_table[desc->inode_index]; // 현재 읽고있는 데이터 가져오기
    char *out_buf = (char *)buf;
    int read_count = 0;

    char indirect_buf[BLOCK_SIZE];
    int current_indirect_loaded = -1; // 현재 버퍼에 로딩된 간접블록 번호

    while(read_count < size){ // size(사용자 요청) == read_count(읽은 양) 이 되는 순간 조건 탈출
        if (desc->offset >= node->size) break;

        int logical_block_index = desc->offset / BLOCK_SIZE; // 현재 오프셋이 몇 번째 블록인지
        int byte_offset_in_block = desc->offset % BLOCK_SIZE; // 블록 내부에서의 오프셋 위치

        int physical_block_num = 0;

        if(logical_block_index < 6){
            physical_block_num = node->blocks[logical_block_index];
        }else{
            if(node->indirect_block == 0) break;
            if(current_indirect_loaded != node->indirect_block){
                long indirect_pos = data_region_start + (long)node->indirect_block * BLOCK_SIZE;
                fseek(disk, indirect_pos, SEEK_SET);
                fread(indirect_buf, 1, BLOCK_SIZE, disk);
                current_indirect_loaded = node->indirect_block;
            }
            int indirect_index = logical_block_index - 6;
            unsigned short *block_map = (unsigned short *)indirect_buf;
            physical_block_num = block_map[indirect_index];
        }
        if(physical_block_num == 0) break;

        timer++; // 시간 흐름
        int cache_index = -1;
        for(int i = 0; i< CACHE_SIZE; i++){
            if(buffer_cache[i].valid && buffer_cache[i].physical_block_num == physical_block_num){
                cache_index = i;
                buffer_cache[i].last_access_time = timer; // 최신 시간으로 변경
                printf("[Cache Hit] Block %d found in slot %d\n",physical_block_num, i);
                break;
            }
        }
        if(cache_index == -1){ // 캐시미스
            printf("[Cache MISS] Loading Block %d...\n", physical_block_num);
            int victim_index = -1;
            int min_time = 100000000; // 큰 수로 설정

            for(int i = 0; i<CACHE_SIZE; i++){
                if(buffer_cache[i].valid == 0){
                    victim_index = i;
                    break;
                }
            }
            if(victim_index == -1){
                for(int i = 0; i<CACHE_SIZE; i++){
                    if(buffer_cache[i].last_access_time < min_time){
                        min_time = buffer_cache[i].last_access_time;
                        victim_index = i;
                    }
                }
            }
            long physical_pos = data_region_start + (long)physical_block_num * BLOCK_SIZE;
            fseek(disk, physical_pos, SEEK_SET);
            fread(buffer_cache[victim_index].data, 1, BLOCK_SIZE, disk);

            // 메타데이터 업데이트
            buffer_cache[victim_index].valid = 1;
            buffer_cache[victim_index].physical_block_num = physical_block_num;
            buffer_cache[victim_index].last_access_time = timer;

            cache_index = victim_index;
        }

        int to_read = BLOCK_SIZE - byte_offset_in_block; // 현재 블록에서 이미 읽은 데이터를 빼주어 현재 블록의 남은 데이터 확인
        int remaining_req = size - read_count; // 요청한만큼 읽어야하므로 요청량 - 읽은 양
        int remaining_file = node->size - desc->offset; // 파일의 남은 데이터를 확인

        // 세 값중 가장 작은 값만큼 읽음
        if(to_read > remaining_req) to_read = remaining_req; 
        if(to_read > remaining_file) to_read = remaining_file;

        // 데이터 시작점 + 블록 번호 * 블록 숫자 -> 현재 주소 + 오프셋 -> 현재까지 읽은 주소
        memcpy(out_buf + read_count, buffer_cache[cache_index].data + byte_offset_in_block, to_read);

        desc->offset += to_read; 
        read_count += to_read;
    }
    return read_count;

}

void close(int fd){
    if (fd >= 0 && fd < 32){
        open_file_table[fd].valid = 0; // 사용 해제
    }
}