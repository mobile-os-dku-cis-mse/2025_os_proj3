// File system header(fs.h)

#ifndef _FS_H
#define _FS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ―――――――――――――――――――――――――――――― Constants(상수) ――――――――――――――――――――――――――――――
#define BLOCK_SIZE           512    // Block의 크기
#define MAX_BLOCKS           4088   // Block의 최대 개수, metadata block 8개를 제외한 4088개
#define SIMPLE_PARTITION     0x1234 // Super block에 기록될 partition type
#define MAX_INODES           1024   // i-node의 최대 개수
#define INODE_MODE_DIR_FILE  0x4000 // Directory임을 나타내는 i-node의 mode bit
#define INODE_MODE_REG_FILE  0x8000 // 일반 file임을 나타내는 i-node의 mode bit
#define NUM_DIRECT_BLOCKS    6      // Block pointer의 개수
#define MAX_FILE_NAME_LEN    60     // File 이름의 최대 길이

// Buffer cache & directory entry
#define BUFFER_CACHE_SIZE    10     // Buffer cache의 크기
#define DENTRY_HASH_SIZE     10     // Directory entry hash의 크기
#define DENTRY_TYPE_DIR_FILE 1      // Directory임을 나타냄
#define DENTRY_TYPE_REG_FILE 2      // 일반 file임을 나타냄

// ―――――――――――――――――――――――――――――― Global variables(전역 변수) ――――――――――――――――――――――――――――――
// main.c에서 중복 정의를 피하기 위해서 extern을 사용
extern int g_buffer_cache_enabled; // Buffer cache의 사용 여부를 나타내는 전역 변수
extern int g_dentry_cache_enabled; // Directory entry cache의 사용 여부를 나타내는 전역 변수

// ―――――――――――――――――――――――――――――― Structures(구조체) ――――――――――――――――――――――――――――――
// Data block
struct data_block {
    unsigned char d[BLOCK_SIZE];
};

// Super block
struct super_block {
    unsigned short partition_type;               // SIMPLE_PARTITION을 저장함
    unsigned short block_size;                   // BLOCK_SIZE를 저장함
    unsigned short inode_size;                   // i-node 구조체의 크기 128을 저장함
    unsigned int first_inode;                    // 첫 번째 i-node의 번호 1을 저장함
    unsigned int num_inodes;                     // MAX_INODES를 저장함
    unsigned int num_inode_blocks;               // i-node block의 개수 7을 저장함
    unsigned int num_blocks;                     // 전체 block의 개수 4096을 저장함
    unsigned int first_data_block;               // 첫 번째 data block의 번호 8을 저장함
    unsigned int num_free_blocks;                // 남은 block의 개수를 저장함
    unsigned int num_free_inodes;                // 남은 i-node의 개수를 저장함
    char volume_name[24];                        // Volume의 이름을 저장함
    unsigned char reserved[466];                 // 예약 공간
};

// i-node
struct inode {
    unsigned short mode;                         // Directory인 지 file인 지와 권한을 저장함
    unsigned short link_count;                   // Hard link의 개수를 저장함
    unsigned int uid;                            // 소유자의 사용자 ID를 저장함
    unsigned int gid;                            // 소유자의 group ID를 저장함
    unsigned int size;                           // File의 크기를 저장함
    time_t date;                                 // File의 마지막 수정 시간을 저장함
    unsigned int blocks[NUM_DIRECT_BLOCKS];      // File data가 저장된 disk block의 번호를 가리키는 6개의 pointer 배열
    unsigned char reserved[56];                  // 예약 공간
};

// Partition
struct partition {
    struct super_block s;                        // Super block은 1block을 사용하며 논리적으로 0번 block에 해당됨
    unsigned char bitmap_inodes[BLOCK_SIZE];     // i-nodes bitmap은 1block을 사용하며 논리적으로 1번 block에 해당됨
    unsigned char bitmap_blocks[6 * BLOCK_SIZE]; // Data block bitmap은 6block을 사용하며 논리적으로 2~7번 block에 해당됨
    struct inode inode_table[MAX_INODES];        // i-node table은 7block을 사용하며 논리적으로 8번 block에 해당됨
    struct data_block data_blocks[MAX_BLOCKS];   // Data block은 4088block을 사용하며 논리적으로 9~4096번 block에 해당됨
};

// Directory entry
struct dentry {
    unsigned int inode;                          // i-node의 번호를 저장함
    unsigned short name_len;                     // 이름의 길이를 저장함
    unsigned char file_type;                     // Directory인 지 file인 지를 저장함
    unsigned char dir_length;                    // Entry의 전체 길이를 저장함
    char name[MAX_FILE_NAME_LEN];                // 이름을 저장함
};

#endif