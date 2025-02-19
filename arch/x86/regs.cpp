#include <arch.h>
#include <cpu.h>
#include <printf.h>
#include "arch.h"

reg_t &arch_reg(int ind, reg_t *r) {
#ifdef __x86_64__
  auto *tf = (struct x86_64regs *)r;
  switch (ind) {
    case REG_PC:
      return tf->rip;

    case REG_SP:
      return tf->rsp;

    case REG_BP:
      return tf->rbp;

    case REG_ARG0:
      return tf->rdi;
    case REG_ARG1:
      return tf->rsi;
  }
#endif

  panic("regs: invalid ind %d\n", ind);
  while (1) {
  }
}

unsigned arch_trapframe_size() {
#ifdef __x86_64__
  // there are 22 registers stored in the x86_64 trapframe
  return sizeof(reg_t) * 22;
#endif

  panic("trapframe_size: unimplemneted\n");
  return 0;
}

void arch_initialize_trapframe(bool userspace, reg_t *r) {
  if (userspace) {
    r[18 /* CS */] = (SEG_UCODE << 3) | DPL_USER;
    r[21 /* DS */] = (SEG_UDATA << 3) | DPL_USER;
    r[19 /* FL */] = FL_IF;
  } else {
    r[18 /* CS */] = (SEG_UCODE << 3) | DPL_KERN;
    r[21 /* DS */] = (SEG_UDATA << 3) | DPL_KERN;
    r[19 /* FL */] = FL_IF | readeflags();
  }
}
