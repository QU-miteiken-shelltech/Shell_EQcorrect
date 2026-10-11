# eqcorrect ビルド (Linux / macOS / MSYS2・MinGW)
#   make            : AVX2 版 (既定)
#   make AVX2=0     : AVX2 を使わない版 (古いCPU向け)
#   make clean
CXX      ?= g++
AVX2     ?= 1
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra
ifeq ($(AVX2),1)
CXXFLAGS += -mavx2
endif
TARGET   := eqcorrect
ifeq ($(OS),Windows_NT)
TARGET := eqcorrect.exe
endif

$(TARGET): src/main.cpp src/fft.hpp src/wav.hpp src/req.hpp src/javaser.hpp src/design.hpp src/analysis.hpp src/jsonw.hpp src/platform.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp $(LDFLAGS)

clean:
	rm -f eqcorrect eqcorrect.exe

.PHONY: clean
