CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread

radarmash-sim: *.cpp *.h ../core/*.cpp ../core/*.h third_party/stb_image.h
	$(CXX) $(CXXFLAGS) *.cpp ../core/*.cpp -o $@

# e.g. make run ARGS="--live --home 51.5,-0.1"
run: radarmash-sim
	./radarmash-sim $(ARGS)

clean:
	rm -f radarmash-sim

.PHONY: run clean
