CMAKE ?= cmake
BUILD_DIR ?= build
BUILD_TYPE ?= Release
CMAKE_FLAGS ?=

.PHONY: all build test clean help run-ground run-sim benchmark

all: build

help:
	@echo "Available targets:"
	@echo "  make build        Configure and build the project in $(BUILD_DIR)"
	@echo "  make test         Build and run the CTest suite"
	@echo "  make run-ground   Run the ground station help text"
	@echo "  make run-sim      Run the simulator help text"
	@echo "  make benchmark    Build and run the decode benchmark helper"
	@echo "  make clean        Remove the build directory"
	@echo ""
	@echo "Useful variables:"
	@echo "  BUILD_TYPE=Debug|Release"
	@echo "  BUILD_DIR=custom_build_dir"
	@echo "  CMAKE_FLAGS='-DSTGS_BUILD_TESTS=OFF'"

build:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_FLAGS)
	$(CMAKE) --build $(BUILD_DIR) --parallel $$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

run-ground: build
	./$(BUILD_DIR)/stgs_ground_station --help

run-sim: build
	./$(BUILD_DIR)/stgs_satellite_simulator --help

benchmark: build
	./$(BUILD_DIR)/stgs_benchmark_decode --help

clean:
	rm -rf $(BUILD_DIR)
