
CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Iinclude
RELEASE  := -O2
DEBUG    := -O0 -g -fsanitize=address,undefined
 
SRC := src/bump_allocator.cpp src/free_list_allocator.cpp
 
.PHONY: all test bench asan clean
 
all: test bench
 
# Build and run the test suite
test:
	$(CXX) $(CXXFLAGS) $(RELEASE) $(SRC) tests/test_allocators.cpp -o build_tests
	./build_tests
 
# Build and run the benchmarks (always -O2, otherwise the numbers are lies)
bench:
	$(CXX) $(CXXFLAGS) $(RELEASE) $(SRC) benchmarks/benchmark.cpp -o build_bench
	./build_bench
 
# Run the tests under AddressSanitizer + UBSan.
# For an allocator this is NOT optional - it catches overlapping blocks,
# buffer overruns, and misaligned reads that normal tests miss.
asan:
	$(CXX) $(CXXFLAGS) $(DEBUG) $(SRC) tests/test_allocators.cpp -o build_asan
	./build_asan
 
clean:
	rm -f build_tests build_bench build_asan
