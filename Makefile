CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread

SRC = src/main.cpp
OUT = proxy

all:
	$(CXX) $(CXXFLAGS) $(SRC) -o $(OUT)

clean:
	rm -f $(OUT)
