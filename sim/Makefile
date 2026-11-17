CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread

radarmash-sim: *.cpp *.h ../core/*.cpp ../core/*.h third_party/stb_image.h admin_html.h
	$(CXX) $(CXXFLAGS) *.cpp ../core/*.cpp -o $@

# the settings page, shared with the firmware
admin_html.h: ../web/admin.html ../tools/embed.py
	python3 ../tools/embed.py ../web/admin.html $@ ADMIN_HTML

# e.g. make run ARGS="--live --home 51.5,-0.1"
run: radarmash-sim
	./radarmash-sim $(ARGS)

clean:
	rm -f radarmash-sim admin_html.h

.PHONY: run clean
