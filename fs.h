#ifndef FS_H
#define FS_H

#include <cstdint>

#define SIMPLE_PARTITION        0x1111
#define BLOCK_SIZE              1024
#define INVALID_INODE           0

#define INODE_MODE_REG_FILE     0x10000
#define INODE_MODE_DIR_FILE     0x20000
#define INODE_MODE_DEV_FILE     0x40000

#define INODE_MODE_AC_ALL       0x777
#define INODE_MODE_AC_USER_R    0x001
#define INODE_MODE_AC_USER_W    0x002
#define INODE_MODE_AC_USER_X    0x004

#define INODE_MODE_AC_OTHER_R   0x010
#define INODE_MODE_AC_OTHER_W   0x020
#define INODE_MODE_AC_OTHER_X   0x040

#define INODE_MODE_AC_GRP_R     0x100
#define INODE_MODE_AC_GRP_W     0x200
#define INODE_MODE_AC_GRP_X     0x400

#define DENTRY_TYPE_REG_FILE    0x1
#define DENTRY_TYPE_DIR_FILE    0x2

#pragma pack(push, 1)

struct super_block {
    uint32_t partition_type;    // 파티션 타입 식별자               (0x1111)
    uint32_t block_size;        // 블록 크기                      (0x400 == 1024)
    uint32_t inode_size;        // inode 구조체 크기              (0x20 == 32)
    uint32_t first_inode;       // 첫 번째 inode 번호             (0x2 == 2)

    uint32_t num_inodes;        // 전체 inode 개수                (0xe0 == 224)
    uint32_t num_inode_blocks;  // inode 테이블 블록 수            (0x7 == 7)
    uint32_t num_free_inodes;   // 사용 가능한 inode 수            (0x79 == 121)

    uint32_t num_blocks;        // 전체 데이터 블록 수              (0xFF8 == 4088)
    uint32_t num_free_blocks;   // 사용 가능한 데이터 블록 수         (0xFF8 == 4088)
    uint32_t first_data_block;  // 첫 번째 데이터 블록 번호          (0x8 == 8)

    char volume_name[24];       // 볼륨 이름
    uint8_t padding[960];       // 1024 - 64(이름이 24 나머지 40) = 960 bytes 패딩
};

// 괄호는 첫 번째 inode의 값입니다.
struct inode {
    uint32_t mode;              // 파일 타입 + 접근 권한 (0x02:0777)
    uint32_t locked;            // 쓰기 잠금 상태       (0x0)
    uint32_t date;              // 생성/수정 시간       (0x0)
    uint32_t size;              // 파일 크기           (0x0cc0 = 3264)
    int32_t indirect_block;     // 간접 블록 번호       (0x0)
    uint16_t blocks[6];         // 직접 블록 포인터      (00, 01, 02, 03, 00, 00)
};

struct blocks {
    uint8_t d[BLOCK_SIZE];
};

struct partition {
    struct super_block s;               // Superblock (1 block)
    struct inode inode_table[224];      // Inode 테이블 (7 blocks)
    struct blocks data_blocks[4088];    // 데이터 블록 (4088 blocks)
};

struct dentry {
    uint32_t inode;             // 파일의 inode 번호     (0x1 == 1)
    uint32_t dir_length;        // 이 엔트리의 길이       (0x20 == 32)
    uint32_t name_len;          // 파일 이름의 실제 길이   (0x2 == 2)
    uint32_t file_type;         // 파일 타입 (1=일반 2=디렉토리)
    char name[16];              // 파일 이름            (최대 16 bytes)
};

#pragma pack(pop)

#endif // FS_H