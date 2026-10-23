/*
 * Guest address space: one 4 GiB reservation, committed page by page as the
 * guest maps memory. Guest address A lives at g_mem + A.
 *
 * The dynarmic page table (absolute offsets) holds g_mem for every mapped page
 * and NULL otherwise, so the JIT touches mapped memory directly and faults on
 * the rest. PROT_NONE pages count as unmapped (guard pages fault, as they must).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define GPAGE 4096u
#define GPAGES (1u << 20)

#define GPROT_READ  1
#define GPROT_WRITE 2
#define GPROT_EXEC  4
#define GMAPPED     0x80   /* in g_prot: page is committed */

extern uint8_t *g_mem;
extern uint8_t **g_page_table;
extern uint8_t g_prot[GPAGES];

int mem_init(void);

static inline void *G(uint32_t a) { return g_mem + a; }
static inline uint32_t GA(const void *p) { return (uint32_t)((const uint8_t *)p - g_mem); }

/* Commit [addr, addr+len) and set protection. Fixed address; replaces what was there. */
int mem_map_fixed(uint32_t addr, uint32_t len, int prot);
/* Finds room for len bytes (top-down below hint_top) and maps it; 0 on failure. */
uint32_t mem_map_anywhere(uint32_t len, int prot);
void mem_unmap(uint32_t addr, uint32_t len);
int mem_protect(uint32_t addr, uint32_t len, int prot);
/* True when every page of [addr, addr+len) is mapped and readable. */
int mem_ok(uint32_t addr, uint32_t len);

/* Tell every core that code in this range may have changed. */
void mem_code_changed(uint32_t addr, uint32_t len);
