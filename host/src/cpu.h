/*
 * One emulated ARMv7-A core per guest thread (dynarmic A32), behind a C interface.
 * Implementation: cpu.cpp - the only C++ in the host, because dynarmic's API is C++.
 *
 * Guest memory is reached through the shared page table (mem.h); accesses to
 * unmapped pages come back to C as faults, SVCs as host_svc().
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cpu cpu_t;

/* Run() exit reasons (dynarmic HaltReason bits). */
enum {
    CPU_HALT_CACHE_INVALIDATION = 0x00000002,
    CPU_HALT_MEMORY_ABORT       = 0x00000004,
    CPU_HALT_USER               = 0x01000000,   /* host wants the thread back: signal, exit, PC change */
};

/* Faults reported by the last Run(). */
enum cpu_fault_kind { CPU_FAULT_NONE, CPU_FAULT_READ, CPU_FAULT_WRITE, CPU_FAULT_EXEC, CPU_FAULT_UNDEF, CPU_FAULT_BKPT };

typedef struct {
    int kind;
    uint32_t addr;   /* data address, or the pc for EXEC/UNDEF/BKPT */
} cpu_fault_t;

/* page_table: 1<<20 entries, each either the guest memory base or NULL (absolute offsets). */
void cpu_global_init(uint8_t **page_table, int max_cores);

cpu_t *cpu_create(int core_id, void *opaque);
void cpu_destroy(cpu_t *c);

/* Runs until halted; returns the CPU_HALT_* bits that stopped it (and clears them). */
uint32_t cpu_run(cpu_t *c);
/* Thread-safe: stop c at the next opportunity with CPU_HALT_USER. */
void cpu_halt(cpu_t *c);
/* Thread-safe: forget translations of [addr, addr+len). */
void cpu_invalidate(cpu_t *c, uint32_t addr, uint32_t len);
void cpu_clear_exclusive(cpu_t *c);

uint32_t *cpu_regs(cpu_t *c);     /* r0..r15 */
uint32_t *cpu_ext_regs(cpu_t *c); /* s0..s63 = d0..d31 */
uint32_t cpu_cpsr(cpu_t *c);
void cpu_set_cpsr(cpu_t *c, uint32_t v);
uint32_t cpu_fpscr(cpu_t *c);
void cpu_set_fpscr(cpu_t *c, uint32_t v);
uint32_t *cpu_tpidruro(cpu_t *c); /* the TLS register glibc and bionic read with mrc p15 */
cpu_fault_t cpu_last_fault(cpu_t *c);
void *cpu_opaque(cpu_t *c);

/* Implemented by the C side. */
void host_svc(void *opaque, uint32_t imm);

#ifdef __cplusplus
}
#endif
