BUILD_DIR ?= build
BUILD_TYPE ?= Debug
CMAKE ?= cmake
CTEST ?= ctest

.PHONY: all build test test-integration test-concurrency test-recovery test-sanitizers test-baseline clean

all: build

build:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	$(CMAKE) --build "$(BUILD_DIR)" --parallel

test: build
	$(CTEST) --test-dir "$(BUILD_DIR)" --output-on-failure

test-integration: build
	$(CTEST) --test-dir "$(BUILD_DIR)" -L integration --output-on-failure

test-concurrency: build
	$(CTEST) --test-dir "$(BUILD_DIR)" -L concurrency --output-on-failure

test-recovery: build
	$(CTEST) --test-dir "$(BUILD_DIR)" -L recovery --output-on-failure

test-sanitizers:
	$(CMAKE) -S . -B "$(BUILD_DIR)/asan" -DFASTDB_SANITIZER=address -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build "$(BUILD_DIR)/asan" --parallel
	$(CTEST) --test-dir "$(BUILD_DIR)/asan" --output-on-failure
	$(CMAKE) -S . -B "$(BUILD_DIR)/tsan" -DFASTDB_SANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build "$(BUILD_DIR)/tsan" --parallel
	$(CTEST) --test-dir "$(BUILD_DIR)/tsan" -L concurrency --output-on-failure

test-baseline:
	bash scripts/audit-baseline.sh

clean:
	$(CMAKE) --build "$(BUILD_DIR)" --target clean
