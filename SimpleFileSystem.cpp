#include "SimpleFileSystem.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <sstream>

SimpleFileSystem::SimpleFileSystem()
    : disk_(nullptr)
      , superblock_(nullptr)
      , inode_table_(nullptr)
      , data_blocks_(nullptr)
      , mounted_(false)
      , image_path_("") {
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        file_table_[i] = FileHandle();
    }
}

SimpleFileSystem::~SimpleFileSystem() {
    if (mounted_) {
        unmount();
    }
}

bool SimpleFileSystem::mount(const std::string &image_path) {
    if (mounted_) {
        std::cout << "Already mounted. Unmounting first.\n";
        unmount();
    }

    std::ifstream file(image_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Failed to open disk image: " << image_path << "\n";
        return false;
    }

    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    size_t expected_size = sizeof(struct partition);

    if (static_cast<size_t>(file_size) < expected_size) {
        std::cerr << "[ERROR] Disk image is too small!\n";
        file.close();
        return false;
    }

    disk_ = new(std::nothrow) struct partition;
    if (!disk_) {
        std::cerr << "[ERROR] Failed to allocate memory!\n";
        file.close();
        return false;
    }

    if (!file.read(reinterpret_cast<char *>(disk_), sizeof(struct partition))) {
        std::cerr << "[ERROR] Failed to read disk image!\n";
        delete disk_;
        disk_ = nullptr;
        file.close();
        return false;
    }

    file.close();

    superblock_ = &disk_->s;
    inode_table_ = disk_->inode_table;
    data_blocks_ = disk_->data_blocks;


    if (superblock_->partition_type != SIMPLE_PARTITION) {
        std::cerr << "[ERROR] Invalid partition type! Expected 0x"
                << std::hex << SIMPLE_PARTITION
                << ", got 0x" << superblock_->partition_type << std::dec << "\n";
        delete disk_;
        disk_ = nullptr;
        return false;
    }

    std::cout << "[MOUNT] Partition type verified: 0x"
            << std::hex << superblock_->partition_type << std::dec << "\n";

    mounted_ = true;
    image_path_ = image_path;

    std::cout << "\n[MOUNT] ✓ File system mounted successfully!\n\n";

    print_superblock_info();

    std::cout << "\n";
    list_root_directory();

    return true;
}

void SimpleFileSystem::unmount() {
    if (!mounted_) {
        std::cout << "[UNMOUNT] Not mounted.\n";
        return;
    }

    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (file_table_[i].in_use) {
            std::cout << "Closing open file: fd=" << i << "\n";
            file_table_[i] = FileHandle();
        }
    }

    if (disk_) {
        delete disk_;
        disk_ = nullptr;
    }

    superblock_ = nullptr;
    inode_table_ = nullptr;
    data_blocks_ = nullptr;
    mounted_ = false;
    image_path_ = "";

    std::cout << "File system unmounted.\n";
}

void SimpleFileSystem::print_superblock_info() const {
    if (!mounted_) {
        std::cerr << "[ERROR] File system not mounted.\n";
        return;
    }


    std::cout << " Volume Name        : " << std::left << std::setw(42)
            << superblock_->volume_name << "\n";

    std::cout << " Partition Type     : 0x" << std::hex << std::setfill('0')
            << std::setw(4) << superblock_->partition_type
            << std::dec << std::setfill(' ')
            << "                                    \n";


    std::cout << " Block Size         : " << std::left << std::setw(10)
            << superblock_->block_size << " bytes"
            << std::setw(26) << " " << "\n";

    std::cout << " Total Data Blocks  : " << std::left << std::setw(10)
            << superblock_->num_blocks
            << std::setw(32) << " " << "\n";

    std::cout << " Free Data Blocks   : " << std::left << std::setw(10)
            << superblock_->num_free_blocks
            << std::setw(32) << " " << "\n";

    std::cout << " First Data Block   : " << std::left << std::setw(10)
            << superblock_->first_data_block
            << std::setw(32) << " " << "\n";

    std::cout << " Inode Size         : " << std::left << std::setw(10)
            << superblock_->inode_size << " bytes"
            << std::setw(26) << " " << "\n";

    std::cout << " Total Inodes       : " << std::left << std::setw(10)
            << superblock_->num_inodes
            << std::setw(32) << " " << "\n";

    std::cout << " Free Inodes        : " << std::left << std::setw(10)
            << superblock_->num_free_inodes
            << std::setw(32) << " " << "\n";

    std::cout << " Inode Table Blocks : " << std::left << std::setw(10)
            << superblock_->num_inode_blocks
            << std::setw(32) << " " << "\n";

    std::cout << " First Inode (Root) : " << std::left << std::setw(10)
            << superblock_->first_inode
            << std::setw(32) << " " << "\n";

    uint32_t sb_start = 0;
    uint32_t sb_end = BLOCK_SIZE - 1;
    uint32_t inode_start = BLOCK_SIZE;
    uint32_t inode_end = BLOCK_SIZE + (superblock_->num_inode_blocks * BLOCK_SIZE) - 1;
    uint32_t data_start = superblock_->first_data_block * BLOCK_SIZE;

    std::cout << " Superblock         : offset 0x" << std::hex << std::setfill('0')
            << std::setw(4) << sb_start << " - 0x" << std::setw(4) << sb_end
            << std::dec << std::setfill(' ') << std::setw(24) << " \n";

    std::cout << " Inode Table        : offset 0x" << std::hex << std::setfill('0')
            << std::setw(4) << inode_start << " - 0x" << std::setw(4) << inode_end
            << std::dec << std::setfill(' ') << std::setw(24) << " \n";

    std::cout << " Data Blocks        : offset 0x" << std::hex << std::setfill('0')
            << std::setw(4) << data_start << " - ..."
            << std::dec << std::setfill(' ') << std::setw(29) << " \n";
}

void SimpleFileSystem::list_root_directory() const {
    if (!mounted_) {
        std::cerr << "[ERROR] File system not mounted.\n";
        return;
    }

    list_directory(superblock_->first_inode);
}

void SimpleFileSystem::list_directory(uint32_t inode_num) const {
    if (!mounted_) {
        std::cerr << "[ERROR] File system not mounted.\n";
        return;
    }

    if (inode_num >= superblock_->num_inodes) {
        std::cerr << "[ERROR] Invalid inode number: " << inode_num << "\n";
        return;
    }

    const struct inode *dir_inode = &inode_table_[inode_num];

    if (!(dir_inode->mode & INODE_MODE_DIR_FILE)) {
        std::cerr << "[ERROR] Inode " << inode_num << " is not a directory.\n";
        return;
    }

    uint32_t dir_size = dir_inode->size;
    uint32_t bytes_read = 0;
    uint32_t entry_count = 0;

    for (int blk_idx = 0; blk_idx < 6 && bytes_read < dir_size; ++blk_idx) {
        uint16_t block_num = dir_inode->blocks[blk_idx];

        if (block_num == 0 && blk_idx > 0) {
            continue;
        }

        const uint8_t *block_data = data_blocks_[block_num].d;

        uint32_t offset = 0;
        while (offset + sizeof(struct dentry) <= BLOCK_SIZE && bytes_read < dir_size) {
            const struct dentry *entry =
                    reinterpret_cast<const struct dentry *>(block_data + offset);

            if (entry->inode != INVALID_INODE) {
                const struct inode *file_inode = &inode_table_[entry->inode];

                char type_char = get_file_type_char(file_inode->mode);
                std::string perm_str = get_permission_string(file_inode->mode);

                size_t name_length = std::min(static_cast<size_t>(entry->name_len),
                                              sizeof(entry->name));
                std::string filename;
                for (size_t i = 0; i < name_length && entry->name[i] != '\0'; ++i) {
                    filename += entry->name[i];
                }

                uint32_t used_blocks = 0;
                for (int i = 0; i < 6; ++i) {
                    if (file_inode->blocks[i] != 0 ||
                        (i == 0 && file_inode->size > 0)) {
                        ++used_blocks;
                    }
                }
                if (file_inode->size > 6 * BLOCK_SIZE &&
                    file_inode->indirect_block != -1) {
                    used_blocks += (file_inode->size - 6 * BLOCK_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;
                }

                std::cout << " " << std::left
                        << std::setw(6) << entry->inode
                        << type_char << perm_str << "  "
                        << std::right << std::setw(7) << file_inode->size << " "
                        << std::setw(4) << used_blocks << "  "
                        << std::left << std::setw(20) << filename
                        << " \n";

                ++entry_count;
            }

            offset += sizeof(struct dentry);
            bytes_read += sizeof(struct dentry);
        }
    }

    std::cout << "Total: " << std::left << std::setw(4) << entry_count
            << " entries, Directory size: " << std::setw(6) << dir_size
            << " bytes" << std::setw(11) << " " << "\n";
}

char SimpleFileSystem::get_file_type_char(uint32_t mode) const {
    if (mode & INODE_MODE_DIR_FILE) return 'd';
    if (mode & INODE_MODE_DEV_FILE) return 'c';
    if (mode & INODE_MODE_REG_FILE) return '-';
    return '?';
}

std::string SimpleFileSystem::get_permission_string(uint32_t mode) const {
    std::string result;

    result += (mode & INODE_MODE_AC_USER_R) ? 'r' : '-';
    result += (mode & INODE_MODE_AC_USER_W) ? 'w' : '-';
    result += (mode & INODE_MODE_AC_USER_X) ? 'x' : '-';

    result += (mode & INODE_MODE_AC_OTHER_R) ? 'r' : '-';
    result += (mode & INODE_MODE_AC_OTHER_W) ? 'w' : '-';
    result += (mode & INODE_MODE_AC_OTHER_X) ? 'x' : '-';

    result += (mode & INODE_MODE_AC_GRP_R) ? 'r' : '-';
    result += (mode & INODE_MODE_AC_GRP_W) ? 'w' : '-';
    result += (mode & INODE_MODE_AC_GRP_X) ? 'x' : '-';

    return result;
}

int SimpleFileSystem::path_to_inode(const std::string &path) const {
    if (!mounted_) return -1;

    if (path == "/") {
        return superblock_->first_inode;
    }

    if (path.empty() || path[0] != '/') {
        return -1;
    }

    uint32_t current_inode = superblock_->first_inode;

    std::string path_copy = path.substr(1);
    std::istringstream iss(path_copy);
    std::string token;

    while (std::getline(iss, token, '/')) {
        if (token.empty()) continue;

        int found_inode = find_entry_in_directory(current_inode, token);

        if (found_inode < 0) {
            return -1;
        }

        current_inode = found_inode;
    }

    return current_inode;
}

int SimpleFileSystem::find_entry_in_directory(uint32_t dir_inode_num,
                                              const std::string &name) const {
    if (!mounted_) return -1;

    const struct inode *dir_inode = &inode_table_[dir_inode_num];

    if (!(dir_inode->mode & INODE_MODE_DIR_FILE)) {
        return -1;
    }

    uint32_t bytes_read = 0;
    uint32_t dir_size = dir_inode->size;

    for (int blk_idx = 0; blk_idx < 6 && bytes_read < dir_size; ++blk_idx) {
        uint16_t block_num = dir_inode->blocks[blk_idx];

        if (block_num == 0 && blk_idx > 0) continue;

        const uint8_t *block_data = data_blocks_[block_num].d;

        uint32_t offset = 0;
        while (offset + sizeof(struct dentry) <= BLOCK_SIZE && bytes_read < dir_size) {
            const struct dentry *entry =
                    reinterpret_cast<const struct dentry *>(block_data + offset);

            if (entry->inode != INVALID_INODE) {
                std::string entry_name(entry->name,
                                       std::min(static_cast<size_t>(entry->name_len), sizeof(entry->name)));

                if (entry_name == name) {
                    return entry->inode;
                }
            }

            offset += sizeof(struct dentry);
            bytes_read += sizeof(struct dentry);
        }
    }

    return -1;
}

int SimpleFileSystem::get_physical_block(const struct inode *inode_ptr,
                                         uint32_t logical_block) const {
    if (logical_block < 6) {
        return inode_ptr->blocks[logical_block];
    }

    if (inode_ptr->indirect_block == -1) {
        return -1;
    }

    uint32_t indirect_index = logical_block - 6;

    const uint16_t *indirect_pointers =
            reinterpret_cast<const uint16_t *>(data_blocks_[inode_ptr->indirect_block].d);

    if (indirect_index >= 512) {
        return -1;
    }

    return indirect_pointers[indirect_index];
}

int SimpleFileSystem::find_free_fd() {
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!file_table_[i].in_use) {
            return i;
        }
    }
    return -1;
}
