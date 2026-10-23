/*
 * dynarmic binding. See cpu.h; everything else in the host is C.
 *
 * Memory: the JIT reads and writes through the shared page table directly; it
 * only calls back here for pages the table marks absent (a fault - recorded,
 * then the core halts with MemoryAbort at the exact faulting instruction, since
 * check_halt_on_memory_access is on) and for exclusive accesses, which go
 * through the global monitor.
 *
 * CP15: TPIDRURO (c13,c0,3) is the thread pointer glibc and bionic read with
 * mrc; TPIDRURW (c13,c0,2) is plain storage; the CP15 barriers are no-ops.
 */
#include "cpu.h"

#include <intrin.h>
#include <cstring>
#include <memory>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

using namespace Dynarmic;

namespace {

uint8_t **g_page_table;
std::unique_ptr<ExclusiveMonitor> g_monitor;

inline uint8_t *host_ptr(uint32_t a)
{
    uint8_t *base = g_page_table[a >> 12];
    return base ? base + a : nullptr;
}

class CP15 final : public A32::Coprocessor {
public:
    uint32_t tpidruro = 0, tpidrurw = 0;

    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg, A32::CoprocReg,
                                                     unsigned) override { return std::nullopt; }

    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                               unsigned opc2) override
    {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C7)   /* DMB/DSB/ISB, cache maintenance */
            return Callback{&noop, std::nullopt};
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 2)
            return &tpidrurw;
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                              unsigned opc2) override
    {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0) {
            if (opc2 == 3)
                return &tpidruro;
            if (opc2 == 2)
                return &tpidrurw;
        }
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<uint8_t>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<uint8_t>) override { return std::nullopt; }

private:
    static uint64_t noop(void *, uint32_t, uint32_t) { return 0; }
};

} // namespace

struct cpu final : public A32::UserCallbacks {
    void *opaque;
    std::shared_ptr<CP15> cp15 = std::make_shared<CP15>();
    std::unique_ptr<A32::Jit> jit;
    cpu_fault_t fault{};

    void abort(int kind, uint32_t addr)
    {
        if (fault.kind == CPU_FAULT_NONE)
            fault = {kind, addr};
        jit->HaltExecution(HaltReason::MemoryAbort);
    }

    template <typename T> T read(uint32_t a)
    {
        uint8_t *p = host_ptr(a);
        if (!p || !host_ptr(a + sizeof(T) - 1)) {
            abort(CPU_FAULT_READ, a);
            return 0;
        }
        T v;
        std::memcpy(&v, p, sizeof(T));
        return v;
    }
    template <typename T> void write(uint32_t a, T v)
    {
        uint8_t *p = host_ptr(a);
        if (!p || !host_ptr(a + sizeof(T) - 1)) {
            abort(CPU_FAULT_WRITE, a);
            return;
        }
        std::memcpy(p, &v, sizeof(T));
    }

    std::optional<uint32_t> MemoryReadCode(A32::VAddr a) override
    {
        uint8_t *p = host_ptr(a);
        if (!p)
            return std::nullopt;
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    uint8_t MemoryRead8(A32::VAddr a) override { return read<uint8_t>(a); }
    uint16_t MemoryRead16(A32::VAddr a) override { return read<uint16_t>(a); }
    uint32_t MemoryRead32(A32::VAddr a) override { return read<uint32_t>(a); }
    uint64_t MemoryRead64(A32::VAddr a) override { return read<uint64_t>(a); }
    void MemoryWrite8(A32::VAddr a, uint8_t v) override { write(a, v); }
    void MemoryWrite16(A32::VAddr a, uint16_t v) override { write(a, v); }
    void MemoryWrite32(A32::VAddr a, uint32_t v) override { write(a, v); }
    void MemoryWrite64(A32::VAddr a, uint64_t v) override { write(a, v); }

    bool MemoryWriteExclusive8(A32::VAddr a, uint8_t v, uint8_t expected) override
    {
        uint8_t *p = host_ptr(a);
        if (!p) { abort(CPU_FAULT_WRITE, a); return false; }
        return (uint8_t)_InterlockedCompareExchange8((volatile char *)p, (char)v, (char)expected) == expected;
    }
    bool MemoryWriteExclusive16(A32::VAddr a, uint16_t v, uint16_t expected) override
    {
        uint8_t *p = host_ptr(a);
        if (!p) { abort(CPU_FAULT_WRITE, a); return false; }
        return (uint16_t)_InterlockedCompareExchange16((volatile short *)p, (short)v, (short)expected) == expected;
    }
    bool MemoryWriteExclusive32(A32::VAddr a, uint32_t v, uint32_t expected) override
    {
        uint8_t *p = host_ptr(a);
        if (!p) { abort(CPU_FAULT_WRITE, a); return false; }
        return (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)v, (long)expected) == expected;
    }
    bool MemoryWriteExclusive64(A32::VAddr a, uint64_t v, uint64_t expected) override
    {
        uint8_t *p = host_ptr(a);
        if (!p) { abort(CPU_FAULT_WRITE, a); return false; }
        return (uint64_t)_InterlockedCompareExchange64((volatile long long *)p, (long long)v, (long long)expected) == expected;
    }

    void InterpreterFallback(A32::VAddr pc, size_t) override { abort(CPU_FAULT_UNDEF, pc); }
    void CallSVC(uint32_t imm) override { host_svc(opaque, imm); }
    void ExceptionRaised(A32::VAddr pc, A32::Exception e) override
    {
        switch (e) {
        case A32::Exception::NoExecuteFault:
            abort(CPU_FAULT_EXEC, pc);
            break;
        case A32::Exception::UndefinedInstruction:
        case A32::Exception::UnpredictableInstruction:
        case A32::Exception::DecodeError:
            abort(CPU_FAULT_UNDEF, pc);
            break;
        case A32::Exception::Breakpoint:
            abort(CPU_FAULT_BKPT, pc);
            break;
        default:   /* hints: SEV, WFE, YIELD, PLD ... */
            break;
        }
    }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return ~0ull; }
};

extern "C" {

void cpu_global_init(uint8_t **page_table, int max_cores)
{
    g_page_table = page_table;
    g_monitor = std::make_unique<ExclusiveMonitor>((size_t)max_cores);
}

cpu_t *cpu_create(int core_id, void *opaque)
{
    cpu_t *c = new cpu();
    c->opaque = opaque;
    A32::UserConfig conf{};
    conf.callbacks = c;
    conf.processor_id = (size_t)core_id;
    conf.global_monitor = g_monitor.get();
    conf.page_table = reinterpret_cast<std::array<uint8_t *, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> *>(g_page_table);
    conf.absolute_offset_page_table = true;
    conf.check_halt_on_memory_access = true;
    conf.enable_cycle_counting = false;
    conf.always_little_endian = true;
    conf.arch_version = A32::ArchVersion::v7;
    conf.define_unpredictable_behaviour = true;
    conf.coprocessors[15] = c->cp15;
    conf.code_cache_size = 128u << 20;
    c->jit = std::make_unique<A32::Jit>(conf);
    return c;
}

void cpu_destroy(cpu_t *c) { delete c; }

uint32_t cpu_run(cpu_t *c)
{
    c->fault = {};
    HaltReason hr = c->jit->Run();
    uint32_t bits = static_cast<uint32_t>(hr);
    c->jit->ClearHalt(hr);
    return bits;
}

void cpu_halt(cpu_t *c) { c->jit->HaltExecution(HaltReason::UserDefined1); }
void cpu_invalidate(cpu_t *c, uint32_t addr, uint32_t len) { c->jit->InvalidateCacheRange(addr, len); }
void cpu_clear_exclusive(cpu_t *c) { c->jit->ClearExclusiveState(); }
uint32_t *cpu_regs(cpu_t *c) { return c->jit->Regs().data(); }
uint32_t *cpu_ext_regs(cpu_t *c) { return c->jit->ExtRegs().data(); }
uint32_t cpu_cpsr(cpu_t *c) { return c->jit->Cpsr(); }
void cpu_set_cpsr(cpu_t *c, uint32_t v) { c->jit->SetCpsr(v); }
uint32_t cpu_fpscr(cpu_t *c) { return c->jit->Fpscr(); }
void cpu_set_fpscr(cpu_t *c, uint32_t v) { c->jit->SetFpscr(v); }
uint32_t *cpu_tpidruro(cpu_t *c) { return &c->cp15->tpidruro; }
cpu_fault_t cpu_last_fault(cpu_t *c) { return c->fault; }
void *cpu_opaque(cpu_t *c) { return c->opaque; }

} // extern "C"
