#include <cstdint>
#include <cstdlib>
int task_packet_freed=0;
void packet_free(void *p,uint32_t) { ++task_packet_freed; std::free(p); }
