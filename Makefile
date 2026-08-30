# Developer facade for the CMake-based STGS project.
# CMake remains the source of truth; this file only exposes memorable showcase commands.

CMAKE ?= cmake
DOXYGEN ?= doxygen
BROWSER ?= firefox

BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= $(shell nproc 2>/dev/null || printf '2')
PORT ?= 9000
HOST ?= 127.0.0.1
PORTS ?= 9000:9010
DECODER_THREADS ?= 4
COUNT ?= 100
RATE ?= 50
OUTPUT ?= telemetry.csv

DOXYFILE ?= Doxyfile
DOXYGEN_OUTPUT ?= docs/doxygen

.PHONY: all configure build debug release test run-ground-tcp run-ground-udp \
        run-sim-tcp run-sim-udp port-check message-demo signal-demo benchmark \
        asan-test tsan-test doxigen clean distclean help

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DSTGS_WARNINGS_AS_ERRORS=ON

build: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)

release:
	$(MAKE) BUILD_TYPE=Release build

debug:
	$(MAKE) BUILD_TYPE=Debug build

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

run-ground-tcp: build
	./$(BUILD_DIR)/stgs_ground_station --tcp --port $(PORT) --decoder-threads $(DECODER_THREADS) --output $(OUTPUT)

run-ground-udp: build
	./$(BUILD_DIR)/stgs_ground_station --udp --port $(PORT) --decoder-threads $(DECODER_THREADS) --output $(OUTPUT)

run-sim-tcp: build
	./$(BUILD_DIR)/stgs_satellite_simulator --tcp --host $(HOST) --port $(PORT) --count $(COUNT) --rate $(RATE)

run-sim-udp: build
	./$(BUILD_DIR)/stgs_satellite_simulator --udp --host $(HOST) --port $(PORT) --count $(COUNT) --rate $(RATE)

port-check: build
	./$(BUILD_DIR)/stgs_port_check --host $(HOST) --ports $(PORTS)

message-demo: build
	./$(BUILD_DIR)/stgs_satellite_simulator --tcp --host $(HOST) --discover-ports $(PORTS) --message "Bonjour depuis STGS" --count 1

signal-demo: build
	./$(BUILD_DIR)/stgs_satellite_simulator --tcp --host $(HOST) --discover-ports $(PORTS) --signal --signal-frequency 5 --sample-rate 200 --signal-samples 256 --signal-amplitude 1.0 --noise-stddev 0.35 --count 5 --rate 2

benchmark: build
	./$(BUILD_DIR)/stgs_benchmark_decode --frames 200000 --payload-size 256 --decoder-threads $(DECODER_THREADS)

asan-test:
	$(CMAKE) -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSTGS_ENABLE_SANITIZERS=ON -DSTGS_WARNINGS_AS_ERRORS=ON
	$(CMAKE) --build build-asan --parallel $(JOBS)
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir build-asan --output-on-failure

tsan-test:
	$(CMAKE) -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSTGS_ENABLE_THREAD_SANITIZER=ON -DSTGS_WARNINGS_AS_ERRORS=ON
	$(CMAKE) --build build-tsan --parallel $(JOBS)
	TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan --output-on-failure

doxigen:
	@command -v $(DOXYGEN) >/dev/null 2>&1 || { \
		printf '%s\n' 'Erreur : Doxygen est introuvable. Installe-le avec : sudo apt install doxygen graphviz'; \
		exit 1; \
	}
	@command -v $(BROWSER) >/dev/null 2>&1 || { \
		printf '%s\n' 'Erreur : Mozilla Firefox est introuvable.'; \
		exit 1; \
	}
	@if [ ! -f "$(DOXYFILE)" ]; then \
		printf '%s\n' 'Création de $(DOXYFILE)...'; \
		$(DOXYGEN) -g "$(DOXYFILE)" >/dev/null; \
		sed -i \
			-e 's|^PROJECT_NAME[[:space:]]*=.*|PROJECT_NAME           = "STGS - Space Telemetry Ground Station"|' \
			-e 's|^OUTPUT_DIRECTORY[[:space:]]*=.*|OUTPUT_DIRECTORY       = $(DOXYGEN_OUTPUT)|' \
			-e 's|^RECURSIVE[[:space:]]*=.*|RECURSIVE              = YES|' \
			-e 's|^EXTRACT_ALL[[:space:]]*=.*|EXTRACT_ALL            = YES|' \
			-e 's|^GENERATE_HTML[[:space:]]*=.*|GENERATE_HTML          = YES|' \
			-e 's|^GENERATE_LATEX[[:space:]]*=.*|GENERATE_LATEX         = NO|' \
			-e 's|^HAVE_DOT[[:space:]]*=.*|HAVE_DOT               = YES|' \
			-e 's|^CALL_GRAPH[[:space:]]*=.*|CALL_GRAPH             = YES|' \
			-e 's|^CALLER_GRAPH[[:space:]]*=.*|CALLER_GRAPH           = YES|' \
			-e 's|^SOURCE_BROWSER[[:space:]]*=.*|SOURCE_BROWSER         = YES|' \
			-e 's|^INLINE_SOURCES[[:space:]]*=.*|INLINE_SOURCES         = YES|' \
			-e 's|^INPUT[[:space:]]*=.*|INPUT                  = .|' \
			-e 's|^EXCLUDE[[:space:]]*=.*|EXCLUDE                = build build-asan build-tsan $(DOXYGEN_OUTPUT) .git|' \
			"$(DOXYFILE)"; \
	fi
	@printf '%s\n' 'Génération de la documentation Doxygen...'
	@$(DOXYGEN) "$(DOXYFILE)"
	@test -f "$(DOXYGEN_OUTPUT)/html/index.html" || { \
		printf '%s\n' 'Erreur : index.html Doxygen introuvable.'; \
		exit 1; \
	}
	@printf '%s\n' 'Ouverture de la documentation dans Mozilla Firefox...'
	@$(BROWSER) --new-window "file://$(CURDIR)/$(DOXYGEN_OUTPUT)/html/index.html" >/dev/null 2>&1 &

clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean 2>/dev/null || true

distclean:
	rm -rf build build-* docs/doxygen telemetry.csv telemetry.json *.stgf

help:
	@printf '%s\n' \
	  'make build          - configure and build Release/BUILD_TYPE' \
	  'make test           - build then run CTest' \
	  'make port-check     - probe loopback TCP ports (PORTS=9000:9010)' \
	  'make message-demo   - send one typed message to the first open STGS port' \
	  'make signal-demo    - send a noisy sine signal to the first open STGS port' \
	  'make asan-test      - run tests with ASan + UBSan' \
	  'make tsan-test      - run tests with ThreadSanitizer' \
	  'make benchmark      - run decode benchmark' \
	  'make doxigen        - generate Doxygen HTML docs and open them in Firefox' \
	  'make distclean      - remove generated build/output files'
