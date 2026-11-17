CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread

radarmash-sim: *.cpp *.h ../core/*.cpp ../core/*.h third_party/stb_image.h admin_html.h wifi_html.h
	$(CXX) $(CXXFLAGS) *.cpp ../core/*.cpp -o $@

# the settings and Wi-Fi setup pages, shared with the firmware
wifi_html.h: ../web/wifi.html ../tools/embed.py
	python3 ../tools/embed.py ../web/wifi.html $@ WIFI_HTML

admin_html.h: ../web/admin.html ../tools/embed.py
	python3 ../tools/embed.py ../web/admin.html $@ ADMIN_HTML

# e.g. make run ARGS="--live --home 51.5,-0.1"
run: radarmash-sim
	./radarmash-sim $(ARGS)

clean:
	rm -f radarmash-sim admin_html.h wifi_html.h

.PHONY: run clean
