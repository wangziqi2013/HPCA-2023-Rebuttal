
CXX=g++
CXXFLAGS=-fno-strict-aliasing -Wall -Wextra -g -std=c++17 -fno-exceptions

.phony: all lib clean

all: lib

lib: malloc_2d_lib

malloc_2d_lib: malloc_2d.h malloc_2d.cpp 
	$(CXX) malloc_2d.cpp -shared -o libmalloc_2d.so $(CXXFLAGS) -fPIC -O3 -DNDEBUG -DMALLOC_2D_LIB

malloc_2d.o: malloc_2d.h malloc_2d.cpp
	$(CXX) -c malloc_2d.cpp -o malloc_2d.o $(CXXFLAGS)

clean:
	rm -f *.o
	rm -f *.so
