CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

radarmash-sim: *.cpp *.h ../core/*.cpp ../core/*.h third_party/stb_image.h
	$(CXX) $(CXXFLAGS) *.cpp ../core/*.cpp -o $@

run: radarmash-sim
	./radarmash-sim

clean:
	rm -f radarmash-sim

.PHONY: run clean
