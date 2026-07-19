// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "cpu/common.h"
#include <cpu/impl/dynarmic_cpu.h>
#include <cpu/state.h>
#include <util/log.h>

#include <mem/ptr.h>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#if defined(VITA3K_PLATFORM_IOS)
#include <oaknut/code_block.hpp>
#endif

#include <bit>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#if defined(VITA3K_PLATFORM_IOS)
namespace {

void log_ios_jit_region_pool_event(oaknut::JitRegionPoolEvent event, std::size_t size, std::size_t available) {
    switch (event) {
    case oaknut::JitRegionPoolEvent::Take:
        LOG_INFO("iOS JIT region pool take: size={} bytes available={}", size, available);
        break;
    case oaknut::JitRegionPoolEvent::Return:
        LOG_INFO("iOS JIT region pool return: size={} bytes available={}", size, available);
        break;
    case oaknut::JitRegionPoolEvent::Miss:
        LOG_INFO("iOS JIT region pool miss: size={} bytes available={}", size, available);
        break;
    case oaknut::JitRegionPoolEvent::Prewarm:
        LOG_INFO("iOS JIT region pool prewarm: size={} bytes available={}", size, available);
        break;
    case oaknut::JitRegionPoolEvent::SessionLost:
        LOG_CRITICAL("StikDebug JIT session lost - cannot prepare a new JIT region; reopen StikDebug and re-enable JIT");
        if (auto logger = spdlog::default_logger())
            logger->flush();
        break;
    }
}

} // namespace

std::size_t prewarm_ios_jit_code_cache_pool(std::size_t target_count, std::size_t cache_size) {
    oaknut::set_jit_region_pool_log_callback(&log_ios_jit_region_pool_event);
    LOG_INFO("iOS JIT region pool prewarm starting: target={} size={} bytes ({} MiB each)",
        target_count, cache_size, cache_size / (1024 * 1024));
    const std::size_t available = oaknut::prewarm_jit_region_pool(cache_size, target_count);
    LOG_INFO("iOS JIT region pool prewarm complete: target={} available={} size={} bytes",
        target_count, available, cache_size);
    return available;
}
#endif

class ArmDynarmicCP15 : public Dynarmic::A32::Coprocessor {
    uint32_t tpidruro;
    uint32_t sctlr;
    uint32_t dacr;

public:
    using CoprocReg = Dynarmic::A32::CoprocReg;

    explicit ArmDynarmicCP15()
        : tpidruro(0)
        , sctlr(0)
        , dacr(0) {
    }

    ~ArmDynarmicCP15() override = default;

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, CoprocReg CRd,
        CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        return std::nullopt;
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn,
        CoprocReg CRm, unsigned opc2) override {
        // MCR p15, 0, Rt, c13, c0, 3 — write TPIDRURO
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            return &tpidruro;
        }

        // MCR p15, 0, Rt, c1, c0, 0 — write SCTLR
        if (!two && CRn == CoprocReg::C1 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &sctlr;
        }

        // MCR p15, 0, Rt, c3, c0, 0 — write DACR
        if (!two && CRn == CoprocReg::C3 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &dacr;
        }

        LOG_WARN("Unhandled CP15 MCR: two={} opc1={} CRn={} CRm={} opc2={}", two, opc1, (int)CRn, (int)CRm, opc2);
        return CallbackOrAccessOneWord{};
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return CallbackOrAccessTwoWords{};
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        // MRC p15, 0, Rt, c13, c0, 3 — read TPIDRURO (thread-local storage)
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            return &tpidruro;
        }

        // MRC p15, 0, Rt, c1, c0, 0 — read SCTLR
        if (!two && CRn == CoprocReg::C1 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &sctlr;
        }

        // MRC p15, 0, Rt, c3, c0, 0 — read DACR
        if (!two && CRn == CoprocReg::C3 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &dacr;
        }

        LOG_WARN("Unhandled CP15 MRC: two={} opc1={} CRn={} CRm={} opc2={}", two, opc1, (int)CRn, (int)CRm, opc2);
        return CallbackOrAccessOneWord{};
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return CallbackOrAccessTwoWords{};
    }

    std::optional<Callback> CompileLoadWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return std::nullopt;
    }

    std::optional<Callback> CompileStoreWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return std::nullopt;
    }

    void set_tpidruro(uint32_t tpidruro) {
        this->tpidruro = tpidruro;
    }

    uint32_t get_tpidruro() const {
        return tpidruro;
    }
};

class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {
    friend class DynarmicCPU;

    CPUState *parent;
    DynarmicCPU *cpu;

public:
    explicit ArmDynarmicCallback(CPUState &parent, DynarmicCPU &cpu)
        : parent(&parent)
        , cpu(&cpu) {}

    ~ArmDynarmicCallback() override = default;

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr addr) override {
        if (cpu->log_mem)
            LOG_TRACE("Instruction fetch at address 0x{:X}", addr);
        return MemoryRead32(addr);
    }

    static void TraceInstruction(uint64_t self_, uint64_t address, uint64_t is_thumb) {
        ArmDynarmicCallback &self = *reinterpret_cast<ArmDynarmicCallback *>(self_);

        std::string disassembly = [&]() -> std::string {
            if (!address || !Ptr<uint32_t>{ (uint32_t)address }.valid(*self.parent->mem)) {
                return "invalid address";
            }
            return disassemble(*self.parent, address);
        }();
        LOG_TRACE("{} ({}): {} {}", log_hex(self_), self.parent->thread_id, log_hex(address), disassembly);
    }

    void PreCodeTranslationHook(bool is_thumb, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {
        if (cpu->log_code) {
            ir.CallHostFunction(&TraceInstruction, ir.Imm64((uint64_t)this), ir.Imm64(pc), ir.Imm64(is_thumb));
        }
    }

    template <typename T>
    T MemoryRead(Dynarmic::A32::VAddr addr) {
        Ptr<T> ptr{ addr };
        if (!ptr || !ptr.valid(*parent->mem) || ptr.address() < parent->mem->host_page_size) {
            LOG_ERROR("Invalid read of uint{}_t at address: 0x{:x}\n{}", sizeof(T) * 8, addr, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < parent->mem->host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return 0;
        }

        T ret = *ptr.get(*parent->mem);
        if (cpu->log_mem) {
            LOG_TRACE("Read uint{}_t at address: 0x{:x}, val = 0x{:x}", sizeof(T) * 8, addr, ret);
        }
        return ret;
    }

    uint8_t MemoryRead8(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint8_t>(addr);
    }

    uint16_t MemoryRead16(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint16_t>(addr);
    }

    uint32_t MemoryRead32(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint32_t>(addr);
    }

    uint64_t MemoryRead64(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint64_t>(addr);
    }

    template <typename T>
    void MemoryWrite(Dynarmic::A32::VAddr addr, T value) {
        Ptr<T> ptr{ addr };
        if (!ptr || !ptr.valid(*parent->mem) || ptr.address() < parent->mem->host_page_size) {
            LOG_ERROR("Invalid write of uint{}_t at addr: 0x{:x}, val = 0x{:x}\n{}", sizeof(T) * 8, addr, value, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < parent->mem->host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return;
        }

        *ptr.get(*parent->mem) = value;
        if (cpu->log_mem) {
            LOG_TRACE("Write uint{}_t at addr: 0x{:x}, val = 0x{:x}", sizeof(T) * 8, addr, value);
        }
    }

    void MemoryWrite8(Dynarmic::A32::VAddr addr, uint8_t value) override {
        MemoryWrite<uint8_t>(addr, value);
    }

    void MemoryWrite16(Dynarmic::A32::VAddr addr, uint16_t value) override {
        MemoryWrite<uint16_t>(addr, value);
    }

    void MemoryWrite32(Dynarmic::A32::VAddr addr, uint32_t value) override {
        MemoryWrite<uint32_t>(addr, value);
    }

    void MemoryWrite64(Dynarmic::A32::VAddr addr, uint64_t value) override {
        MemoryWrite<uint64_t>(addr, value);
    }

    template <typename T>
    bool MemoryWriteExclusive(Dynarmic::A32::VAddr addr, T value, T expected) {
        Ptr<T> ptr{ addr };
        if (!ptr || !ptr.valid(*parent->mem) || ptr.address() < parent->mem->host_page_size) {
            LOG_ERROR("Invalid exclusive write of uint{}_t at addr: 0x{:x}, val = 0x{:x}, expected = 0x{:x}\n{}", sizeof(T) * 8, addr, value, expected, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < parent->mem->host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return false;
        }

        auto result = Ptr<T>(addr).atomic_compare_and_swap(*parent->mem, value, expected);
        if (cpu->log_mem) {
            LOG_TRACE("Write uint{}_t at addr: 0x{:x}, val = 0x{:x}, expected = 0x{:x}", sizeof(T) * 8, addr, value, expected);
        }
        return result;
    }

    bool MemoryWriteExclusive8(Dynarmic::A32::VAddr addr, uint8_t value, uint8_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive16(Dynarmic::A32::VAddr addr, uint16_t value, uint16_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive32(Dynarmic::A32::VAddr addr, uint32_t value, uint32_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive64(Dynarmic::A32::VAddr addr, uint64_t value, uint64_t expected) override {
        return MemoryWriteExclusive(addr, value, expected); // Ptr<uint64_t>(addr).atomic_compare_and_swap(*parent->mem, value, expected);
    }

    void InterpreterFallback(Dynarmic::A32::VAddr addr, size_t num_insts) override {
        LOG_ERROR("Unimplemented instruction at address {}:\n{}", log_hex(addr), save_context(*parent).description());
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::Breakpoint: {
            cpu->break_ = true;
            cpu->jit->HaltExecution();
            if (cpu->is_thumb_mode())
                cpu->set_pc(pc | 1);
            else
                cpu->set_pc(pc);
            break;
        }
        case Dynarmic::A32::Exception::WaitForInterrupt: {
            cpu->halted = true;
            cpu->jit->HaltExecution();
            break;
        }
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadInstruction:
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForEvent:
            break;
        case Dynarmic::A32::Exception::Yield:
            break;
        case Dynarmic::A32::Exception::UndefinedInstruction:
            LOG_WARN("Undefined instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::UnpredictableInstruction:
            LOG_WARN("Unpredictable instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::DecodeError: {
            LOG_WARN("Decode error at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        }
        default:
            LOG_WARN("Unknown exception {} Raised at pc = 0x{:x}", static_cast<size_t>(exception), pc);
            LOG_TRACE("at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
        }
    }

    void CallSVC(uint32_t svc) override {
        parent->svc_called = true;
        parent->svc = svc;
        cpu->jit->HaltExecution(Dynarmic::HaltReason::UserDefined8);
    }

    void AddTicks(uint64_t ticks) override {}

    uint64_t GetTicksRemaining() override {
        return 1ull << 60;
    }
};

Dynarmic::ExclusiveMonitor DynarmicCPU::shared_monitor(MAX_CORE_COUNT);

std::unique_ptr<Dynarmic::A32::Jit> DynarmicCPU::make_jit() {
    Dynarmic::A32::UserConfig config{};
#if defined(VITA3K_PLATFORM_IOS)
    // Vita3K owns one Dynarmic JIT per guest thread. On iOS 26+, each cache
    // also has a same-sized writable vm_remap alias, so Dynarmic's 128 MiB
    // default scales poorly during middleware worker-thread bursts. Sixteen
    // MiB remains above Dynarmic's documented approximate 8 MiB minimum; the
    // backend clears the cache when it approaches capacity.
    constexpr std::size_t IOS_CODE_CACHE_SIZE = 16 * 1024 * 1024;
    config.code_cache_size = IOS_CODE_CACHE_SIZE;
#endif
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.callbacks = cb.get();
    if (parent->mem->use_page_table) {
        config.page_table = (log_mem || !cpu_opt) ? nullptr : reinterpret_cast<decltype(config.page_table)>(parent->mem->page_table.get());
        config.absolute_offset_page_table = true;
    } else if (!log_mem && cpu_opt) {
        config.fastmem_pointer = std::bit_cast<uintptr_t>(parent->mem->memory.get());
    }
    config.hook_hint_instructions = true;
    config.global_monitor = &shared_monitor;
    config.coprocessors[15] = cp15;
    config.processor_id = core_id;
    config.optimizations = cpu_opt ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
    config.enable_cycle_counting = false;

#if defined(VITA3K_PLATFORM_IOS)
    // StikDebug services Oaknut's BRK #0xf00d while the JIT constructor maps
    // its execution cache. Serialize this short boundary so simultaneous
    // guest-thread creation cannot issue overlapping debugger requests.
    static std::mutex jit_creation_mutex;
    static uint64_t jit_creation_count = 0;
    const std::lock_guard lock(jit_creation_mutex);
    const uint64_t allocation_id = ++jit_creation_count;
    LOG_INFO("iOS Dynarmic JIT cache #{} allocating: thread={} core={} size={} bytes ({} MiB)",
        allocation_id, parent->thread_id, core_id, config.code_cache_size,
        config.code_cache_size / (1024 * 1024));
    auto jit = std::make_unique<Dynarmic::A32::Jit>(config);
    LOG_INFO("iOS Dynarmic JIT cache #{} ready: thread={} core={} size={} bytes ({} MiB)",
        allocation_id, parent->thread_id, core_id, config.code_cache_size,
        config.code_cache_size / (1024 * 1024));
    return jit;
#else
    return std::make_unique<Dynarmic::A32::Jit>(config);
#endif
}

DynarmicCPU::DynarmicCPU(CPUState *state, std::size_t processor_id, bool cpu_opt)
    : parent(state)
    , cb(std::make_unique<ArmDynarmicCallback>(*state, *this))
    , cp15(std::make_shared<ArmDynarmicCP15>())
    , core_id(processor_id)
    , cpu_opt(cpu_opt) {
#if defined(VITA3K_PLATFORM_IOS)
    // JIT code regions come from a fixed pool prepared while the debugger is
    // attached. Defer the allocation until this core first executes so
    // created-but-not-yet-started threads don't hold a region.
    parked_ctx = std::make_unique<CPUContext>();
#else
    jit = make_jit();
#endif
}

DynarmicCPU::~DynarmicCPU() = default;

void DynarmicCPU::ensure_jit() {
    if (jit)
        return;
    jit = make_jit();
    if (parked_ctx) {
        const CPUContext ctx = *parked_ctx;
        parked_ctx.reset();
        load_context(ctx);
    }
}

void DynarmicCPU::release_code_cache() {
    if (!jit)
        return;
    parked_ctx = std::make_unique<CPUContext>(save_context());
    jit.reset();
}

bool DynarmicCPU::ensure_code_cache() {
    try {
        ensure_jit();
    } catch (const std::exception &e) {
        LOG_CRITICAL("Cannot (re)create JIT code cache for thread {}: {}", parent->thread_id, e.what());
        return false;
    }
    return true;
}

int DynarmicCPU::run() {
    try {
        ensure_jit();
    } catch (const std::exception &e) {
        LOG_CRITICAL("Cannot (re)create JIT code cache for thread {}: {}", parent->thread_id, e.what());
        return -1;
    }
    halted = false;
    break_ = false;
    parent->svc_called = false;
    Dynarmic::HaltReason halt_reason;
    do {
        halt_reason = jit->Run();
    } while ((halt_reason == Dynarmic::HaltReason::Step) || (halt_reason == Dynarmic::HaltReason::CacheInvalidation));

    return halted;
}

int DynarmicCPU::step() {
    try {
        ensure_jit();
    } catch (const std::exception &e) {
        LOG_CRITICAL("Cannot (re)create JIT code cache for thread {}: {}", parent->thread_id, e.what());
        return -1;
    }
    parent->svc_called = false;
    jit->Step();
    return 0;
}

bool DynarmicCPU::hit_breakpoint() {
    return break_;
}

void DynarmicCPU::trigger_breakpoint() {
    break_ = true;
    stop();
}

void DynarmicCPU::set_log_code(bool log) {
    if (log_code == log)
        return;

    log_code = log;
    if (jit)
        jit = make_jit();
}

void DynarmicCPU::set_log_mem(bool log) {
    if (log_mem == log)
        return;

    log_mem = log;
    if (jit)
        jit = make_jit();
}

bool DynarmicCPU::get_log_code() {
    return log_code;
}

bool DynarmicCPU::get_log_mem() {
    return log_mem;
}

void DynarmicCPU::stop() {
    if (!jit)
        return;
    jit->HaltExecution();
}

uint32_t DynarmicCPU::get_reg(uint8_t idx) {
    if (!jit)
        return parked_ctx ? parked_ctx->cpu_registers[idx] : 0;
    return jit->Regs()[idx];
}

uint32_t DynarmicCPU::get_sp() {
    return get_reg(13);
}

uint32_t DynarmicCPU::get_pc() {
    return get_reg(15);
}

void DynarmicCPU::set_reg(uint8_t idx, uint32_t val) {
    if (!jit) {
        if (parked_ctx)
            parked_ctx->cpu_registers[idx] = val;
        return;
    }
    jit->Regs()[idx] = val;
}

void DynarmicCPU::set_cpsr(uint32_t val) {
    if (!jit) {
        if (parked_ctx)
            parked_ctx->cpsr = val;
        return;
    }
    jit->SetCpsr(val);
}

uint32_t DynarmicCPU::get_tpidruro() {
    return cp15->get_tpidruro();
}

void DynarmicCPU::set_tpidruro(uint32_t val) {
    cp15->set_tpidruro(val);
}

void DynarmicCPU::set_pc(uint32_t val) {
    if (val & 1) {
        set_cpsr(get_cpsr() | 0x20);
        val = val & 0xFFFFFFFE;
    } else {
        set_cpsr(get_cpsr() & 0xFFFFFFDF);
        val = val & 0xFFFFFFFC;
    }
    set_reg(15, val);
}

void DynarmicCPU::set_lr(uint32_t val) {
    set_reg(14, val);
}

void DynarmicCPU::set_sp(uint32_t val) {
    set_reg(13, val);
}

uint32_t DynarmicCPU::get_cpsr() {
    if (!jit)
        return parked_ctx ? parked_ctx->cpsr : 0;
    return jit->Cpsr();
}

uint32_t DynarmicCPU::get_fpscr() {
    if (!jit)
        return parked_ctx ? parked_ctx->fpscr : 0;
    return jit->Fpscr();
}

void DynarmicCPU::set_fpscr(uint32_t val) {
    if (!jit) {
        if (parked_ctx)
            parked_ctx->fpscr = val;
        return;
    }
    jit->SetFpscr(val);
}

CPUContext DynarmicCPU::save_context() {
    if (!jit)
        return parked_ctx ? *parked_ctx : CPUContext{};

    CPUContext ctx;
    ctx.cpu_registers = jit->Regs();
    static_assert(sizeof(ctx.fpu_registers) == sizeof(jit->ExtRegs()));
    memcpy(ctx.fpu_registers.data(), jit->ExtRegs().data(), sizeof(ctx.fpu_registers));
    ctx.fpscr = jit->Fpscr();
    ctx.cpsr = jit->Cpsr();

    return ctx;
}

void DynarmicCPU::load_context(const CPUContext &ctx) {
    if (!jit) {
        parked_ctx = std::make_unique<CPUContext>(ctx);
        return;
    }
    jit->Regs() = ctx.cpu_registers;
    static_assert(sizeof(ctx.fpu_registers) == sizeof(jit->ExtRegs()));
    memcpy(jit->ExtRegs().data(), ctx.fpu_registers.data(), sizeof(ctx.fpu_registers));
    jit->SetCpsr(ctx.cpsr);
    jit->SetFpscr(ctx.fpscr);
}

uint32_t DynarmicCPU::get_lr() {
    return get_reg(14);
}

float DynarmicCPU::get_float_reg(uint8_t idx) {
    if (!jit)
        return parked_ctx ? parked_ctx->fpu_registers[idx] : 0.0f;
    return std::bit_cast<float>(jit->ExtRegs()[idx]);
}

void DynarmicCPU::set_float_reg(uint8_t idx, float val) {
    if (!jit) {
        if (parked_ctx)
            parked_ctx->fpu_registers[idx] = val;
        return;
    }
    jit->ExtRegs()[idx] = std::bit_cast<uint32_t>(val);
}

bool DynarmicCPU::is_thumb_mode() {
    return get_cpsr() & 0x20;
}

std::size_t DynarmicCPU::processor_id() const {
    return core_id;
}

void DynarmicCPU::invalidate_jit_cache(Address start, size_t length) {
    // A released cache has nothing stale in it.
    if (!jit)
        return;
    jit->InvalidateCacheRange(start, length);
}

void DynarmicCPU::clear_exclusive() {
    shared_monitor.ClearProcessor(core_id);
}
