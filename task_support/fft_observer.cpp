#include <atomic>
#include <cstddef>
static std::atomic<size_t> calls{0};
extern "C" void rb_task_fft_inverse(){calls.fetch_add(1,std::memory_order_relaxed);}
extern "C" size_t rb_task_fft_inverse_count(){return calls.load(std::memory_order_relaxed);}
