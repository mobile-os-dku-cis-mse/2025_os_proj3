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