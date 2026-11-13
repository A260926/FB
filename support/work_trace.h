#pragma once
#include <cstddef>
#include <cstdint>
// Production default may define this macro as a no-op when this file is not force-included.
void task_tail_work(std::uint64_t quantum,const char* kind,std::size_t units);
#define FFTCONVOLVER_TAIL_WORK(quantum,kind,units) task_tail_work(quantum,kind,units)
namespace task {
struct WorkEvent { std::uint64_t quantum; const char* kind; std::size_t units; };
// Fixed capacity recorder. Clear between independent streams, outside process.
void clear_work();
std::size_t work_count();
WorkEvent work_event(std::size_t index);
}
