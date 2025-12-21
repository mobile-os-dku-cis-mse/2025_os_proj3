# Simple File System Makefile

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -O2

# 소스 파일
SOURCES = main.cpp SimpleFileSystem.cpp
HEADERS = fs.h SimpleFileSystem.h

# 출력 파일
TARGET = simple_fs

# 기본 타겟
all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES)

# 정리
clean:
	rm -f $(TARGET)

# 실행
run: $(TARGET)
	./$(TARGET) disk.img

# 디버그 모드 컴파일
debug: CXXFLAGS += -DDEBUG -g -O0
debug: clean $(TARGET)

# 구조체 크기 확인
check-sizes:
	@echo "Checking structure sizes..."
	$(CXX) $(CXXFLAGS) -c -o /dev/null fs.h 2>&1 || echo "Structure size check passed"

.PHONY: all clean run debug check-sizes