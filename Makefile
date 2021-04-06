CXX := g++
CPP_FLAG := -std=c++17 -g

all: st_huge_pg

st_huge_pg: st_huge_pg.o XDMA_udrv.o
	$(CXX) -o $@ $^ $(CPP_FLAG)

test: test.o XDMA_udrv.o
	$(CXX) -o $@ $^ $(CPP_FLAG)

%.o: %.cpp
	$(CXX) -c $< $(CPP_FLAG)

.PHONY: clean
clean:
	rm -f *.o st_huge_pg