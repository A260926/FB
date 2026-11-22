#define DR_MP3_IMPLEMENTATION
#define DR_MP3_FLOAT_OUTPUT
#include "task_support/codec.h"
unsigned long long task_spectral_passes;
#define DRMP3_STEMS_SPECTRAL_PASS() (++task_spectral_passes)
#include "dr_mp3.h"
