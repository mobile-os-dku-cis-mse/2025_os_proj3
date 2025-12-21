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

int SimpleFileSystem::open(const std::string &pathname, int mode) {
    if (!mounted_) {
        std::cerr << "[ERROR] File system not mounted.\n";
        return -1;
    }

    int inode_num = path_to_inode(pathname);
    if (inode_num < 0) {
        std::cerr << "[OPEN] File not found: " << pathname << "\n";
        return -1;
    }

    struct inode *file_inode = &inode_table_[inode_num];

    if (!(file_inode->mode & INODE_MODE_REG_FILE)) {
        std::cerr << "[OPEN] Not a regular file: " << pathname << "\n";
        return -1;
    }

    if ((mode & O_WR) && file_inode->locked) {
        std::cerr << "[OPEN] File is locked for writing: " << pathname << "\n";
        return -1;
    }

    int fd = find_free_fd();
    if (fd < 0) {
        std::cerr << "[OPEN] Too many open files (max=" << MAX_OPEN_FILES << ")\n";
        return -1;
    }

    file_table_[fd].inode_ptr = file_inode;
    file_table_[fd].inode_num = static_cast<uint32_t>(inode_num);
    file_table_[fd].offset = 0;
    file_table_[fd].mode = mode;
    file_table_[fd].in_use = true;

    if (mode & O_WR) {
        file_inode->locked = 1;
    }

    std::cout << "[OPEN] Success: fd=" << fd
            << ", path=\"" << pathname << "\""
            << ", inode=" << inode_num
            << ", size=" << file_inode->size << " bytes"
            << ", mode=" << (mode == O_RD ? "O_RD" : mode == O_WR ? "O_WR" : "O_RDWR")
            << "\n";

    return fd;
}

int SimpleFileSystem::read(int fd, char *buffer, uint32_t size) {
    if (!mounted_) {
        std::cerr << "[ERROR] File system not mounted.\n";
        return -1;
    }

    if (fd < 0 || fd >= MAX_OPEN_FILES) {
        std::cerr << "[READ] Invalid file descriptor: " << fd << "\n";
        return -1;
    }

    if (!file_table_[fd].in_use) {
        std::cerr << "[READ] File descriptor not in use: " << fd << "\n";
        return -1;
    }

    if (!(file_table_[fd].mode & O_RD)) {
        std::cerr << "[READ] File not opened for reading: fd=" << fd << "\n";
        return -1;
    }

    FileHandle &file = file_table_[fd];
    struct inode *inode = file.inode_ptr;

    uint32_t file_size = inode->size;
    uint32_t current_offset = file.offset;

    if (current_offset >= file_size) {
        return 0;
    }

    uint32_t remaining = file_size - current_offset;
    uint32_t to_read = (size < remaining) ? size : remaining;

    uint32_t bytes_read = 0;

    while (bytes_read < to_read) {
        uint32_t logical_block = current_offset / BLOCK_SIZE;
        uint32_t block_offset = current_offset % BLOCK_SIZE;

        int physical_block = get_physical_block(inode, logical_block);
        if (physical_block < 0) {
            std::cerr << "[READ] Block mapping error at logical block "
                    << logical_block << "\n";
            break;
        }

        uint32_t bytes_remaining_in_block = BLOCK_SIZE - block_offset;
        uint32_t bytes_still_needed = to_read - bytes_read;
        uint32_t bytes_to_copy = (bytes_still_needed < bytes_remaining_in_block)
                                     ? bytes_still_needed
                                     : bytes_remaining_in_block;

        std::memcpy(buffer + bytes_read,
                    data_blocks_[physical_block].d + block_offset,
                    bytes_to_copy);

        bytes_read += bytes_to_copy;
        current_offset += bytes_to_copy;
    }

    file.offset = current_offset;

    std::cout << "[READ] fd=" << fd
            << ", requested=" << size << " bytes"
            << ", read=" << bytes_read << " bytes"
            << ", new_offset=" << file.offset
            << "\n";

    return static_cast<int>(bytes_read);
}

int SimpleFileSystem::close(int fd) {
    if (!mounted_) {
        std::cerr << "[ERROR] File system not mounted.\n";
        return -1;
    }

    if (fd < 0 || fd >= MAX_OPEN_FILES) {
        std::cerr << "[CLOSE] Invalid file descriptor: " << fd << "\n";
        return -1;
    }

    if (!file_table_[fd].in_use) {
        std::cerr << "[CLOSE] File descriptor not in use: " << fd << "\n";
        return -1;
    }

    if (file_table_[fd].mode & O_WR) {
        if (file_table_[fd].inode_ptr) {
            file_table_[fd].inode_ptr->locked = 0;
        }
    }

    uint32_t inode_num = file_table_[fd].inode_num;
    uint32_t final_offset = file_table_[fd].offset;

    file_table_[fd].inode_ptr = nullptr;
    file_table_[fd].inode_num = 0;
    file_table_[fd].offset = 0;
    file_table_[fd].mode = 0;
    file_table_[fd].in_use = false;

    std::cout << "[CLOSE] fd=" << fd
            << ", inode=" << inode_num
            << ", final_offset=" << final_offset
            << "\n";

    return 0;
}

void SimpleFileSystem::dump_inode(uint32_t inode_num) const {
    if (!mounted_) {
        std::cerr << "[ERROR] File system not mounted.\n";
        return;
    }

    if (inode_num >= superblock_->num_inodes) {
        std::cerr << "[ERROR] Invalid inode number: " << inode_num << "\n";
        return;
    }

    const struct inode *i = &inode_table_[inode_num];

    std::cout << "\n=== Inode #" << inode_num << " Dump ===\n";
    std::cout << "  mode          : 0x" << std::hex << std::setfill('0')
            << std::setw(8) << i->mode << std::dec << std::setfill(' ') << "\n";
    std::cout << "  locked        : " << i->locked << "\n";
    std::cout << "  date          : " << i->date << "\n";
    std::cout << "  size          : " << i->size << " bytes\n";
    std::cout << "  indirect_block: " << i->indirect_block << "\n";
    std::cout << "  blocks        : [";
    for (int j = 0; j < 6; ++j) {
        std::cout << i->blocks[j];
        if (j < 5) std::cout << ", ";
    }
    std::cout << "]\n";

    std::cout << "  type          : ";
    if (i->mode & INODE_MODE_DIR_FILE) std::cout << "Directory";
    else if (i->mode & INODE_MODE_REG_FILE) std::cout << "Regular File";
    else if (i->mode & INODE_MODE_DEV_FILE) std::cout << "Device File";
    else std::cout << "Unknown";
    std::cout << "\n";

    std::cout << "  permissions   : " << get_permission_string(i->mode) << "\n";
}

void SimpleFileSystem::dump_block(uint32_t block_num, uint32_t bytes) const {
    if (!mounted_) {
        std::cerr << "[ERROR] File system not mounted.\n";
        return;
    }

    if (block_num >= 4088) {
        std::cerr << "[ERROR] Invalid block number: " << block_num << "\n";
        return;
    }

    std::cout << "\n=== Data Block #" << block_num << " Dump (" << bytes << " bytes) ===\n";

    const uint8_t *data = data_blocks_[block_num].d;

    for (uint32_t i = 0; i < bytes && i < BLOCK_SIZE; ++i) {
        if (i % 16 == 0) {
            std::cout << std::hex << std::setfill('0') << std::setw(4) << i << ": "
                    << std::dec;
        }

        std::cout << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(data[i]) << std::dec << " ";

        if ((i + 1) % 16 == 0) {
            std::cout << " |";
            for (uint32_t j = i - 15; j <= i; ++j) {
                char c = data[j];
                std::cout << (isprint(c) ? c : '.');
            }
            std::cout << "|\n";
        }
    }

    std::cout << std::dec << std::setfill(' ');
}
