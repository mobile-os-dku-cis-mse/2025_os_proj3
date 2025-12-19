#ifndef SIMPLE_FILE_SYSTEM_H
#define SIMPLE_FILE_SYSTEM_H

#include "fs.h"
#include <string>
#include <cstdint>

#define O_RD    0x01
#define O_WR    0x02
#define O_RDWR  0x03

struct FileHandle {
    struct inode* inode_ptr;
    uint32_t inode_num;
    uint32_t offset;
    uint32_t mode;
    bool in_use;

    FileHandle() : inode_ptr(nullptr), inode_num(0),
                   offset(0), mode(0), in_use(false) {}
};

class SimpleFileSystem {
public:
    static constexpr int MAX_OPEN_FILES = 256;

private:
    struct partition* disk_;
    struct super_block* superblock_;
    struct inode* inode_table_;
    struct blocks* data_blocks_;

    FileHandle file_table_[MAX_OPEN_FILES];

    bool mounted_;
    std::string image_path_;

public:
    SimpleFileSystem();
    ~SimpleFileSystem();

    SimpleFileSystem(const SimpleFileSystem&) = delete;
    SimpleFileSystem& operator=(const SimpleFileSystem&) = delete;

    bool mount(const std::string& image_path);
    void unmount();
    bool is_mounted() const { return mounted_; }
    void print_superblock_info() const;
    void list_directory(uint32_t inode_num) const;
    void list_root_directory() const;

    int open(const std::string& pathname, int mode);
    int read(int fd, char* buffer, uint32_t size);
    int close(int fd);

private:
    int path_to_inode(const std::string& path) const;
    int find_entry_in_directory(uint32_t dir_inode_num,
                                 const std::string& name) const;
    int get_physical_block(const struct inode* inode_ptr,
                           uint32_t logical_block) const;
    int find_free_fd();
    char get_file_type_char(uint32_t mode) const;
    std::string get_permission_string(uint32_t mode) const;
};

#endif