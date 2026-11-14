#include "elf++.hh"
#include "dwarf++.hh"
#include "line_index.hh"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string>

using namespace std;

void
usage(const char *cmd)
{
        fprintf(stderr, "usage: %s elf-file pc\n", cmd);
        exit(2);
}

int
main(int argc, char **argv)
{
        if (argc != 3)
                usage(argv[0]);

        dwarf::taddr pc;
        try {
                pc = stoll(argv[2], nullptr, 0);
        } catch (invalid_argument &e) {
                usage(argv[0]);
        } catch (out_of_range &e) {
                usage(argv[0]);
        }

        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
                fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
                return 1;
        }

        elf::elf ef(elf::create_mmap_loader(fd));
        dwarf::dwarf dw(dwarf::elf::create_loader(ef));

        for (auto cu : dw.compilation_units()) {
                auto &lt = cu.get_line_table();
                if (!lt.valid())
                        continue;

                // Build the index once.  It owns everything it
                // needs, so queries no longer touch the line table
                // or the DWARF data.
                dwarf::line_index index(lt);

                // Which source intervals cover this operation?
                for (auto &iv : index.find(pc)) {
                        printf("[%#" PRIx64 ", %#" PRIx64 ") %s:%u\n",
                               iv.begin_address, iv.end_address,
                               iv.file.c_str(), iv.line);

                        // Where else can we break on that line?
                        for (auto &bp : index.find_line(iv.file, iv.line))
                                printf("  break at %#" PRIx64
                                       " (sequence %u row %u)\n",
                                       bp.begin_address, bp.sequence, bp.row);
                }
        }

        return 0;
}
