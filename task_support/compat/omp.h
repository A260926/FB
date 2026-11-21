#ifndef MINPT_SERIAL_OMP_H
#define MINPT_SERIAL_OMP_H
inline int omp_get_max_threads(){return 1;}
inline int omp_get_thread_num(){return 0;}
#endif
