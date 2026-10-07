# Makefile for the BHTTP/1 project.
#
#   make          build both programs into ./bin
#   make clean    delete them again
#
# Works with g++ on Linux, macOS, and on Windows through MSYS2 / MinGW.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Isrc

# Windows needs the Winsock library; Unix needs pthreads.
ifeq ($(OS),Windows_NT)
  LDLIBS = -lws2_32
  EXT    = .exe
else
  LDLIBS = -pthread
  EXT    =
endif

BIN    = bin
COMMON = src/frame.cpp

all: $(BIN)/bserve$(EXT) $(BIN)/bcurl$(EXT)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/bserve$(EXT): src/bserve.cpp $(COMMON) src/frame.h src/net.h | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ src/bserve.cpp $(COMMON) $(LDLIBS)

$(BIN)/bcurl$(EXT): src/bcurl.cpp $(COMMON) src/frame.h src/net.h | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ src/bcurl.cpp $(COMMON) $(LDLIBS)

clean:
	rm -rf $(BIN)

.PHONY: all clean
