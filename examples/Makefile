CXXFLAGS := -std=gnu++11 -I../task_support/vendor

all: sections symbols segments relocations

sections: sections.cc ../elf_parser.cpp ../elf_parser.hpp meta.hh
	g++ -o sections sections.cc ../elf_parser.cpp $(CXXFLAGS)

symbols: symbols.cc ../elf_parser.cpp ../elf_parser.hpp meta.hh
	g++ -o symbols symbols.cc ../elf_parser.cpp $(CXXFLAGS)

segments: segments.cc ../elf_parser.cpp ../elf_parser.hpp meta.hh
	g++ -o segments segments.cc ../elf_parser.cpp $(CXXFLAGS)

relocations: relocations.cc ../elf_parser.cpp ../elf_parser.hpp meta.hh
	g++ -o relocations relocations.cc ../elf_parser.cpp $(CXXFLAGS)

clean:
	rm -f sections symbols segments relocations
