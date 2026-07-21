# Type-theoretic C++ SDK for RCP (header-only + examples/tests).
CXX     ?= g++
CXXFLAGS?= -std=c++23 -O2 -Wall -Wextra -Iinclude

.PHONY: all test clean
all: test_types example_server example_client

test_types: tests/test_types.cpp $(wildcard include/rcp/*.hpp) include/rcp.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

example_server: examples/example_server.cpp $(wildcard include/rcp/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@

example_client: examples/example_client.cpp $(wildcard include/rcp/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@

test: test_types example_server example_client
	./test_types
	./example_client ./example_server

clean:
	rm -f test_types example_server example_client
