#include <stdio.h>
#include <string.h>

#include "fs.h"

FILE *disk;
struct super_block sb;
struct inode inode_table[224];

int fs_mount(const char *disk_image)
{
    disk = fopen(disk_image, "rb");
    if (!disk) {
        perror("fopen");
        return -1;
    }

    fread(&sb, sizeof(struct super_block), 1, disk);

    if (sb.partition_type != SIMPLE_PARTITION) {
        printf("Invalid filesystem\n");
        return -1;
    }

    fseek(disk, sb.first_inode * BLOCK_SIZE, SEEK_SET);
    fread(inode_table, sizeof(struct inode), 224, disk);

    return 0;
}

void fs_ls_root(void)
{
    struct inode *root_inode = &inode_table[0];

    printf("Root inode size: %u\n", root_inode->size);
    printf("Root inode blocks: ");
    for (int i = 0; i < 6; i++)
        printf("%u ", root_inode->blocks[i]);
    printf("\n");

    printf("Root directory contents:\n");

    int block_num = root_inode->blocks[0];
    if (block_num == 0)
        return;

    unsigned char buffer[BLOCK_SIZE];
    fseek(disk, block_num * BLOCK_SIZE, SEEK_SET);
    fread(buffer, BLOCK_SIZE, 1, disk);

    printf("\nRaw directory block dump (first %u bytes):\n", root_inode->size);
    for (int i = 0; i < root_inode->size; i++) {
        printf("%02x ", buffer[i]);
    }
    printf("\n");

    int offset = 0;
    while (offset + 16 <= root_inode->size) {

        unsigned int inode;
        unsigned int dir_length;
        unsigned int name_len;
        unsigned int file_type;

        memcpy(&inode,      buffer + offset,      4);
        memcpy(&dir_length, buffer + offset + 4,  4);
        memcpy(&name_len,   buffer + offset + 8,  4);
        memcpy(&file_type,  buffer + offset + 12, 4);

        /* garde-fous CRITIQUES */
        if (inode == 0)
            break;

        if (dir_length < 16)
            break;

        if (offset + dir_length > root_inode->size)
            break;

        if (name_len > dir_length - 16)
            break;

        char name[256];
        memcpy(name, buffer + offset + 16, name_len);
        name[name_len] = '\0';

        printf(" - %s (inode %u)\n", name, inode);

        offset += dir_length;
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2 || !strcmp(argv[1], "help")) {
        printf("Usage: %s disk.img\n", argv[0]);
        return 0;
    }

    if (fs_mount(argv[1]) < 0)
        return 1;

    printf("Volume name: %s\n", sb.volume_name);

    fs_ls_root();

    return 0;
}
