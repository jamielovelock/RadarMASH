CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

radarmash-sim: main.cpp ../core/*.cpp ../core/*.h
	$(CXX) $(CXXFLAGS) main.cpp ../core/*.cpp -o $@

run: radarmash-sim
	./radarmash-sim

clean:
	rm -f radarmash-sim

.PHONY: run clean
