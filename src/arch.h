#ifndef VERBA_ARCH_H
#define VERBA_ARCH_H

/*
 * Architecture map: how to pull a syscall number, its arguments, and its return
 * value out of a stopped tracee's registers. This is the single place that
 * knows the ABI; porting to aarch64 means adding a branch here (its registers
 * live in struct user_pt_regs, args in regs[0..5], number in regs[8]).
 */

#include <sys/types.h>
#include <sys/user.h>

#if defined(__x86_64__)

typedef struct user_regs_struct verba_regs;

static inline long arch_syscall_nr(const verba_regs *r)
{
	return (long)r->orig_rax;
}

static inline long arch_syscall_ret(const verba_regs *r)
{
	return (long)r->rax;
}

static inline unsigned long arch_syscall_arg(const verba_regs *r, int i)
{
	switch (i) {
	case 0:
		return r->rdi;
	case 1:
		return r->rsi;
	case 2:
		return r->rdx;
	case 3:
		return r->r10;
	case 4:
		return r->r8;
	case 5:
		return r->r9;
	default:
		return 0;
	}
}

#else
#error "verba currently supports x86-64 only (see TODO.md section 1.1)"
#endif

#endif /* VERBA_ARCH_H */
