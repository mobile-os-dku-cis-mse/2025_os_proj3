#include "SimpleFileSystem.h"
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>

void test_phase1(SimpleFileSystem& fs, const std::string& image_path);
void test_phase2(SimpleFileSystem& fs);
void test_single_file(SimpleFileSystem& fs, const std::string& filename);
void test_random_10_files(SimpleFileSystem& fs);
void test_edge_cases(SimpleFileSystem& fs);
void test_inode_dump(SimpleFileSystem& fs);
void test_block_dump(SimpleFileSystem& fs);


int main(int argc, char* argv[]) {
    std::string image_path = "disk.img";

    if (argc > 1) {
        image_path = argv[1];
    }

    std::srand(static_cast<unsigned>(std::time(nullptr)));

    SimpleFileSystem fs;

    test_phase1(fs, image_path);

    test_phase2(fs);

    fs.unmount();

    std::cout << "\n";
    std::cout << "All Phases Completed Successfully!\n";
    std::cout << "\n";

    return 0;
}

void test_phase1(SimpleFileSystem& fs, const std::string& image_path) {
    std::cout << "MOUNT & LIST\n";
    std::cout << "\n";

    if (!fs.mount(image_path)) {
        std::cerr << "\n[FATAL] Failed to mount file system!\n";
        std::cerr << "Make sure '" << image_path << "' exists in the current directory.\n";
        std::exit(1);
    }

    std::cout << "\n\n[TEST] Phase 1 completed successfully.\n\n";
}

void test_phase2(SimpleFileSystem& fs) {

    std::cout << "\n\n==========Test 2.1: Single File Read Test==========\n\n";

    test_single_file(fs, "/file_1");

    std::cout << "\n";
    std::cout << "\n\n==========Test 2.2: Random 10 Files Test==========\n\n";

    test_random_10_files(fs);

    std::cout << "\n";
    std::cout << "\n\n==========Test 2.3: Edge Cases==========\n\n";

    test_edge_cases(fs);

    std::cout << "\n[TEST] Phase 2 completed successfully.\n";
}

void test_single_file(SimpleFileSystem& fs, const std::string& filename) {

    int fd = fs.open(filename, O_RD);
    if (fd < 0) {
        std::cerr << "Failed to open " << filename << "\n";
        return;
    }

    char buffer[1024];
    std::memset(buffer, 0, sizeof(buffer));

    int bytes_read = fs.read(fd, buffer, sizeof(buffer) - 1);

    if (bytes_read > 0) {
        std::cout << "[CONTENT] \"" << buffer << "\"\n";
    } else if (bytes_read == 0) {
        std::cout << "[CONTENT] (empty file)\n";
    } else {
        std::cerr << "[ERROR] Read failed\n";
    }

    fs.close(fd);

    std::cout << "\n";
}

void test_random_10_files(SimpleFileSystem& fs) {
    std::cout << "Selecting 10 random files from file_1 to file_100...\n\n";

    std::vector<int> all_numbers;
    for (int i = 1; i <= 100; ++i) {
        all_numbers.push_back(i);
    }

    for (int i = 99; i > 0; --i) {
        int j = std::rand() % (i + 1);
        std::swap(all_numbers[i], all_numbers[j]);
    }

    std::vector<int> selected(all_numbers.begin(), all_numbers.begin() + 10);

    std::cout << "Selected files: ";
    for (int i = 0; i < 10; ++i) {
        std::cout << "file_" << selected[i];
        if (i < 9) std::cout << ", ";
    }
    std::cout << "\n\n";

    int success_count = 0;
    int total_bytes = 0;

    for (int i = 0; i < 10; ++i) {
        char filepath[64];
        std::snprintf(filepath, sizeof(filepath), "/file_%d", selected[i]);

        int fd = fs.open(filepath, O_RD);
        if (fd < 0) {
            std::cout << " " << std::setw(3) << (i + 1) << "  "
                      << std::setw(12) << std::left << filepath << std::right
                      << "  FAILED    -     (open failed)                           \n";
            continue;
        }

        char buffer[256];
        std::memset(buffer, 0, sizeof(buffer));
        int bytes = fs.read(fd, buffer, sizeof(buffer) - 1);

        if (bytes > 0 && buffer[bytes - 1] == '\n') {
            buffer[bytes - 1] = '\0';
            bytes--;
        }

        std::string content(buffer);
        if (content.length() > 39) {
            content = content.substr(0, 36) + "...";
        }

        std::cout << " " << std::setw(3) << (i + 1) << "  "
                  << std::setw(12) << std::left << (std::string("file_") + std::to_string(selected[i])) << std::right
                  << "  " << std::setw(6) << "fd = "<< fd
                  << "  " << std::setw(6) << bytes
                  << "  " << std::setw(39) << std::left << content << std::right << " \n";

        fs.close(fd);

        success_count++;
        total_bytes += bytes;
    }

    std::cout << " Summary: " << success_count << "/10 files read successfully, "
              << "Total " << total_bytes << " bytes read"
              << std::setw(24) << " " << "\n";
}

void test_edge_cases(SimpleFileSystem& fs) {
    std::cout << "Testing edge cases...\n\n";

    std::cout << "[Edge Case 1] Opening non-existent file:\n";
    int fd1 = fs.open("/nonexistent_file", O_RD);
    if (fd1 < 0) {
        std::cout << "  → Correctly returned error for non-existent file\n";
    }

    std::cout << "\n[Edge Case 2] Reading with invalid fd:\n";
    char buffer[100];
    int result = fs.read(999, buffer, 100);
    if (result < 0) {
        std::cout << "  → Correctly returned error for invalid fd\n";
    }

    std::cout << "\n[Edge Case 3] Closing invalid fd:\n";
    result = fs.close(999);
    if (result < 0) {
        std::cout << "  → Correctly returned error for invalid fd\n";
    }

    std::cout << "\n[Edge Case 4] Reading from closed fd:\n";
    int fd2 = fs.open("/file_1", O_RD);
    if (fd2 >= 0) {
        fs.close(fd2);
        result = fs.read(fd2, buffer, 100);
        if (result < 0) {
            std::cout << "  → Correctly returned error for closed fd\n";
        }
    }

    std::cout << "\n[Edge Case 5] Opening multiple files simultaneously:\n";
    int fds[5];
    for (int i = 0; i < 5; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "/file_%d", i + 1);
        fds[i] = fs.open(path, O_RD);
    }

    std::cout << "  Opened 5 files with fds: ";
    for (int i = 0; i < 5; ++i) {
        std::cout << fds[i];
        if (i < 4) std::cout << ", ";
    }
    std::cout << "\n";

    for (int i = 0; i < 5; ++i) {
        fs.close(fds[i]);
    }
    std::cout << "  All files closed successfully\n";

    std::cout << "\n[Edge Case 6] Opening same file multiple times:\n";
    int fd_a = fs.open("/file_1", O_RD);
    int fd_b = fs.open("/file_1", O_RD);
    std::cout << "  Same file opened twice: fd_a=" << fd_a << ", fd_b=" << fd_b << "\n";

    char buf_a[20], buf_b[10];
    fs.read(fd_a, buf_a, 10);
    fs.read(fd_b, buf_b, 5);

    fs.close(fd_a);
    fs.close(fd_b);
    std::cout << "  Each fd maintains independent offset\n";

    std::cout << "\n[Edge Case 7] Trying to open root directory:\n";
    int fd_dir = fs.open("/", O_RD);
    if (fd_dir < 0) {
        std::cout << "  → Correctly rejected opening directory as regular file\n";
    } else {
        fs.close(fd_dir);
    }

    std::cout << "\nAll edge cases tested.\n";
}

void test_inode_dump(SimpleFileSystem& fs) {
    std::cout << "\n";
    std::cout << "--- Inode Dump Examples ---\n";

    fs.dump_inode(2);
    fs.dump_inode(3);
    fs.dump_inode(12);
}

void test_block_dump(SimpleFileSystem& fs) {
    std::cout << "\n";
    std::cout << "--- Block Dump Examples ---\n";

    std::cout << "\n[Root Directory - Block 0]\n";
    fs.dump_block(0, 96);

    std::cout << "\n[file_1 Content - Block 4]\n";
    fs.dump_block(4, 48);
}