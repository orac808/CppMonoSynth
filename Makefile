CXX = g++
CXXFLAGS = -O2 -Wall -std=c++14
LDFLAGS = -lasound -lm -static-libgcc -static-libstdc++
TARGET = monosynth

all: $(TARGET)

$(TARGET): monosynth.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
