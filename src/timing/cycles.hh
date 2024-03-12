#ifndef SRC_CYCLES_HH
#define SRC_CYCLES_HH

namespace timing {

typedef unsigned long long ticks;

static __inline__ ticks cpu_cycles_start () {
#if defined(__aarch64__) || defined(_M_ARM64)
  uint64_t value;
  asm("isb; mrs %0, PMCCNTR_EL0" : "=r"(value));
  return value;
#else
  unsigned cycles_low, cycles_high;
  asm volatile ("CPUID\n\t"
               "RDTSC\n\t"
               "mov %%edx, %0\n\t"
               "mov %%eax, %1\n\t": "=r" (cycles_high), "=r" (cycles_low)::
                                                          "%rax", "%rbx", "%rcx", "%rdx");
  return ((ticks)cycles_high << 32) | cycles_low;
#endif
}


static __inline__ ticks cpu_cycles_stop () {
#if defined(__aarch64__) || defined(_M_ARM64)
  uint64_t value;
  asm("mrs %0, PMCCNTR_EL0" : "=r"(value); isb);
  return value;
#else
  unsigned cycles_low, cycles_high;
  asm volatile("RDTSCP\n\t"
               "mov %%edx, %0\n\t"
               "mov %%eax, %1\n\t"
               "CPUID\n\t": "=r" (cycles_high), "=r" (cycles_low):: "%rax",
                             "%rbx", "%rcx", "%rdx");
  return ((ticks)cycles_high << 32) | cycles_low;
#endif
}

}

#endif  // SRC_CYCLES_HH
