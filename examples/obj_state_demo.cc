#include "tests/obj-fuzz/state_model.h"

#include <cstdio>
#include <string>
#include <vector>

// Demonstrates the state-aware generation / check / reduce pipeline.  Usage:
//   obj_state_demo OUTPUT_DIR
//
// Generates a fresh case, checks it through the worker protocol, reduces it,
// re-checks the reduced case and copies case.obj/case.mtl and
// reduced/case.obj/reduced/case.mtl into OUTPUT_DIR.  Prints
// OBJ_STATE_DEMO_OK on success.

int main(int argc, char **argv) {
  // check() re-executes this same binary as the worker process.
  if (argc >= 2 && std::string(argv[1]) == "worker") {
    return obj_state_main(argc, argv);
  }
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s OUTPUT_DIR\n", argv[0]);
    return 2;
  }
  const std::string out_dir(argv[1]);
  const std::string case_dir = out_dir + "/case_work";
  const std::string reduced_dir = out_dir + "/reduced_work";

  std::string message;
  if (!objstate::ensure_directory(out_dir)) {
    std::fprintf(stderr, "demo: cannot create %s\n", out_dir.c_str());
    return 1;
  }
  if (!objstate::obj_state_generate_dir(20261124ULL, case_dir, &message)) {
    std::fprintf(stderr, "demo: generate failed: %s\n", message.c_str());
    return 1;
  }
  if (!objstate::obj_state_check_dir(case_dir, argv[0], true, &message)) {
    std::fprintf(stderr, "demo: check failed: %s\n", message.c_str());
    return 1;
  }
  if (!objstate::obj_state_reduce_dir(case_dir, reduced_dir, "F0", 64, &message)) {
    std::fprintf(stderr, "demo: reduce failed: %s\n", message.c_str());
    return 1;
  }
  if (!objstate::obj_state_check_dir(reduced_dir, argv[0], true, &message)) {
    std::fprintf(stderr, "demo: reduced check failed: %s\n", message.c_str());
    return 1;
  }

  if (!objstate::ensure_directory(out_dir + "/reduced")) {
    std::fprintf(stderr, "demo: cannot create reduced directory\n");
    return 1;
  }
  if (!objstate::copy_file_bytes(case_dir + "/case.obj", out_dir + "/case.obj") ||
      !objstate::copy_file_bytes(case_dir + "/case.mtl", out_dir + "/case.mtl") ||
      !objstate::copy_file_bytes(reduced_dir + "/case.obj",
                                 out_dir + "/reduced/case.obj") ||
      !objstate::copy_file_bytes(reduced_dir + "/case.mtl",
                                 out_dir + "/reduced/case.mtl") ||
      !objstate::copy_file_bytes(reduced_dir + "/reduction.log",
                                 out_dir + "/reduced/reduction.log")) {
    std::fprintf(stderr, "demo: cannot copy outputs\n");
    return 1;
  }

  std::printf("OBJ_STATE_DEMO_OK\n");
  return 0;
}
