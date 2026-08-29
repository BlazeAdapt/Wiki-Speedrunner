EXEEXT := .exe
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Iinclude
LDLIBS = -lz

BIN = build_pages build_redirects build_linktargets build_pagelinks build_csr search
OUTDIR = bin

.PHONY: all clean
all: $(addprefix $(OUTDIR)/, $(BIN))

$(OUTDIR):
	mkdir -p $(OUTDIR)

$(OUTDIR)/%: src/%.cpp include/*.hpp | $(OUTDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf $(OUTDIR)
