// Standalone runner for the partial-specialization tests. Kept separate from
// the fixture-based glsl_optimizer_tests runner so the CTest entry point can
// invoke it without a tests directory.
int run_specialization_tests(const char* testsDir);

int main(int /*argc*/, char** /*argv*/)
{
   return run_specialization_tests("");
}
