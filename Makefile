# RadarMASH top-level build.
#   make test    build and run the unit tests
#   make sim     build the desktop simulator
#   make run     build and start the simulator at http://localhost:8360
#   make check   everything CI runs: tests, then a simulator smoke test
CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra
# Memory and undefined-behaviour checking is on by default on Linux (and so in CI). It is off on macOS, where a
# sanitized binary can hang at start-up with Apple's toolchain; override with `make test SANITIZE=-fsanitize=...`.
ifeq ($(shell uname -s),Darwin)
SANITIZE ?=
else
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
endif
WERROR ?=

CORE := $(wildcard core/*.cpp)
HEADERS := $(wildcard core/*.h) $(wildcard tests/*.h)

build/tests: $(wildcard tests/*.cpp) $(CORE) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(WERROR) $(SANITIZE) $(wildcard tests/*.cpp) $(CORE) -o $@

test: build/tests
	./build/tests

sim:
	$(MAKE) -C sim CXX="$(CXX)"

run:
	$(MAKE) -C sim run

# One frame must come out as a 24-bit 360x360 BMP: 54-byte header + 360*360*3 bytes.
smoke: sim
	@mkdir -p build
	sim/radarmash-sim --shot build/smoke.bmp --t 9000 --select 0
	test "$$(wc -c < build/smoke.bmp | tr -d ' ')" -eq 388854

check: test smoke

clean:
	rm -rf build
	$(MAKE) -C sim clean

.PHONY: test sim run smoke check clean
