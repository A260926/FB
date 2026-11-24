// The real sanitizer coverage callbacks. Instrumented modules call these to
// register their guard arrays (once per loaded image) and to report guard
// hits; both forward to the registry in coverage.hpp where module binding,
// per-thread collection scopes, and snapshot support live.

#include <fuzzcover/coverage.hpp>

extern "C" void __sanitizer_cov_trace_pc_guard_init(std::uint32_t* start, std::uint32_t* stop)
{
    fuzzcover::detail::coverage_register_guards(start, stop);
}

extern "C" void __sanitizer_cov_trace_pc_guard(std::uint32_t* guard)
{
    fuzzcover::detail::coverage_guard_hit(guard);
}
