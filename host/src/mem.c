/* See mem.h. */
#include "mem.h"

#include <windows.h>
#include <string.h>

#include "host.h"

uint8_t *g_mem;
uint8_t **g_page_table;
uint8_t g_prot[GPAGES];

static SRWLOCK g_lock = SRWLOCK_INIT;

/* mmap without an address goes top-down from here, below the stack area, as Linux does. */
#define MMAP_TOP 0xbf000000u
#define MMAP_BOTTOM 0x10000000u

int mem_init(void)
{
    /* 4 GiB plus a page of slack, so an access straddling the top of the guest space stays reserved. */
    g_mem = VirtualAlloc(NULL, (SIZE_T)1 << 32 | GPAGE, MEM_RESERVE, PAGE_NOACCESS);
    g_page_table = VirtualAlloc(NULL, sizeof(uint8_t *) * GPAGES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return g_mem && g_page_table ? 0 : -1;
}

static void set_pages(uint32_t first, uint32_t count, int prot)
{
    for (uint32_t i = first; i < first + count; i++) {
        g_prot[i] = (uint8_t)(GMAPPED | prot);
        g_page_table[i] = (prot & (GPROT_READ | GPROT_WRITE | GPROT_EXEC)) ? g_mem : NULL;
    }
}

static int commit(uint32_t addr, uint32_t len)
{
    return VirtualAlloc(g_mem + addr, len, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
}

int mem_map_fixed(uint32_t addr, uint32_t len, int prot)
{
    if (addr & (GPAGE - 1))
        return -1;
    len = (len + GPAGE - 1) & ~(GPAGE - 1);
    if (!len || (uint64_t)addr + len > ((uint64_t)1 << 32))
        return -1;
    AcquireSRWLockExclusive(&g_lock);
    /* MAP_FIXED over an existing mapping replaces it with zeroed memory. */
    for (uint32_t p = addr; p < addr + len; p += GPAGE)
        if (g_prot[p / GPAGE] & GMAPPED)
            memset(g_mem + p, 0, GPAGE);
    int r = commit(addr, len);
    if (r == 0)
        set_pages(addr / GPAGE, len / GPAGE, prot);
    ReleaseSRWLockExclusive(&g_lock);
    if (r == 0 && (prot & GPROT_EXEC))
        mem_code_changed(addr, len);
    return r;
}

uint32_t mem_map_anywhere(uint32_t len, int prot)
{
    len = (len + GPAGE - 1) & ~(GPAGE - 1);
    uint32_t need = len / GPAGE, run = 0, found = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (uint32_t p = MMAP_TOP / GPAGE; p-- > MMAP_BOTTOM / GPAGE;) {
        if (g_prot[p] & GMAPPED) {
            run = 0;
            continue;
        }
        if (++run == need) {
            found = p * GPAGE;
            break;
        }
    }
    if (found) {
        if (commit(found, len) == 0)
            set_pages(found / GPAGE, need, prot);
        else
            found = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

void mem_unmap(uint32_t addr, uint32_t len)
{
    addr &= ~(GPAGE - 1);
    len = (len + GPAGE - 1) & ~(GPAGE - 1);
    if ((uint64_t)addr + len > ((uint64_t)1 << 32))
        len = (uint32_t)(((uint64_t)1 << 32) - addr);
    AcquireSRWLockExclusive(&g_lock);
    int had_code = 0;
    for (uint32_t i = addr / GPAGE; i < (addr + (uint64_t)len) / GPAGE; i++) {
        if (g_prot[i] & GPROT_EXEC)
            had_code = 1;
        if (g_prot[i] & GMAPPED)
            VirtualFree(g_mem + (size_t)i * GPAGE, GPAGE, MEM_DECOMMIT);
        g_prot[i] = 0;
        g_page_table[i] = NULL;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (had_code)
        mem_code_changed(addr, len);
}

int mem_protect(uint32_t addr, uint32_t len, int prot)
{
    addr &= ~(GPAGE - 1);
    len = (len + GPAGE - 1) & ~(GPAGE - 1);
    AcquireSRWLockExclusive(&g_lock);
    for (uint32_t i = addr / GPAGE; i < (addr + (uint64_t)len) / GPAGE; i++) {
        if (!(g_prot[i] & GMAPPED)) {
            ReleaseSRWLockExclusive(&g_lock);
            return -1;
        }
    }
    set_pages(addr / GPAGE, len / GPAGE, prot);
    ReleaseSRWLockExclusive(&g_lock);
    /* A page becoming executable is how JITs (Mono's) publish code. */
    if (prot & GPROT_EXEC)
        mem_code_changed(addr, len);
    return 0;
}

int mem_ok(uint32_t addr, uint32_t len)
{
    if (!len)
        return 1;
    if ((uint64_t)addr + len > ((uint64_t)1 << 32))
        return 0;
    for (uint32_t i = addr / GPAGE; i <= (addr + len - 1) / GPAGE; i++)
        if (!g_page_table[i])
            return 0;
    return 1;
}

void mem_code_changed(uint32_t addr, uint32_t len)
{
    threads_invalidate_code(addr, len);
}
