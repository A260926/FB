

CXXFLAGS = -std=gnu++11 -I../task_support/vendor

all: sections symbols segments relocations

sections: sections.cc
	g++ $(CXXFLAGS) -o sections sections.cc ../elf_parser.cpp

symbols: symbols.cc
	g++ $(CXXFLAGS) -o symbols symbols.cc ../elf_parser.cpp

segments: segments.cc
	g++ $(CXXFLAGS) -o segments segments.cc ../elf_parser.cpp

relocations: relocations.cc
	g++ $(CXXFLAGS) -o relocations relocations.cc ../elf_parser.cpp

clean:
	rm -f sections symbols segments relocations
