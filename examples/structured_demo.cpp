#include "fuzz/structured/driver.h"

#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int remove_tree_cb(const char *path, const struct stat *, int typeflag,
                   struct FTW *) {
    if (typeflag == FTW_DP) return ::rmdir(path);
    return ::unlink(path);
}

void remove_tree(const std::string &path) {
    ::nftw(path.c_str(), remove_tree_cb, 16, FTW_DEPTH | FTW_PHYS);
}

int run_entry(const std::string &argv0, const std::vector<std::string> &args) {
    std::vector<std::string> storage;
    storage.push_back(argv0);
    for (std::size_t i = 0; i < args.size(); ++i) storage.push_back(args[i]);

    std::vector<char *> argv;
    for (std::size_t i = 0; i < storage.size(); ++i)
        argv.push_back(const_cast<char *>(storage[i].c_str()));
    argv.push_back(NULL);
    return cgltf_defense_main(static_cast<int>(storage.size()), argv.data());
}

} // namespace

int main(int argc, char **argv) {
    /* The check command spawns this same executable as: argv[0] worker DIR. */
    if (argc >= 2 && std::strcmp(argv[1], "worker") == 0)
        return cgltf_defense_main(argc, argv);

    if (argc != 2) {
        std::fprintf(stderr, "usage: %s OUTPUT_DIR\n", argv[0]);
        return 2;
    }

    const std::string out = argv[1];
    const std::string reduced = out + "/reduced";

    /* Remove only this entry's own generated products before the demo. */
    std::remove((out + "/case.plan").c_str());
    std::remove((out + "/case.gltf").c_str());
    remove_tree(reduced);

    int rc = run_entry(argv[0], {"generate", "20261125", out});
    if (rc != 0) return rc;

    rc = run_entry(argv[0], {"check", out});
    if (rc != 0) return rc;

    rc = run_entry(argv[0],
                   {"reduce", out, reduced, "--keep", "shared-strided",
                    "--budget", "220"});
    if (rc != 0) return rc;

    rc = run_entry(argv[0], {"check", reduced});
    if (rc != 0) return rc;

    std::printf("STRUCTURED_DEMO_OK\n");
    return 0;
}
