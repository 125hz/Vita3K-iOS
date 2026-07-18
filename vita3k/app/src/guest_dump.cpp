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

// Diagnostic snapshot of the guest runtime: every guest thread's PC/LR/status,
// every kernel sync primitive with waiters, display/vblank progress, audio and
// NGS scheduler state. Frontends call this from a watchdog when a title stops
// making display progress. Thread collection retries the central kernel lock
// for one second before using an independent snapshot; other contended state
// is read with try_lock and skipped when unavailable.

#include <app/functions.h>

#include <audio/state.h>
#include <config/state.h>
#include <cpu/functions.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <ngs/scheduler.h>
#include <ngs/state.h>
#include <ngs/system.h>
#include <nids/functions.h>
#include <renderer/state.h>
#include <util/log.h>

#include <fmt/format.h>

#include <chrono>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace app {

static const char *thread_status_str(ThreadStatus status) {
    switch (status) {
    case ThreadStatus::run: return "run";
    case ThreadStatus::dormant: return "dormant";
    case ThreadStatus::suspend: return "suspend";
    case ThreadStatus::wait: return "wait";
    }
    return "unknown";
}

static void dump_display_state(EmuEnvState &emuenv) {
    DisplayState &display = emuenv.display;
    LOG_INFO("Display: vblank_count={} last_setframe_vblank_count={} guest_frame_count={} predicting={} current_sync_object=0x{:X}",
        display.vblank_count.load(), display.last_setframe_vblank_count.load(), emuenv.frame_count,
        display.predicting.load(), display.current_sync_object.load());

    {
        std::unique_lock<std::mutex> info_lock(display.display_info_mutex, std::try_to_lock);
        if (info_lock.owns_lock()) {
            LOG_INFO("Display: sce_frame base=0x{:X} pitch={} size={}x{}; next_rendered_frame base=0x{:X} size={}x{}",
                display.sce_frame.base.address(), display.sce_frame.pitch,
                display.sce_frame.image_size.x, display.sce_frame.image_size.y,
                display.next_rendered_frame.base.address(),
                display.next_rendered_frame.image_size.x, display.next_rendered_frame.image_size.y);
        } else {
            LOG_INFO("Display: display_info_mutex busy, frame info skipped");
        }
    }

    {
        std::unique_lock<std::mutex> lock(display.mutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (const auto &wait_info : display.vblank_wait_infos) {
                if (wait_info.target_thread)
                    LOG_INFO("Display: thread {} '{}' waits for vblank target_vcount={}",
                        wait_info.target_thread->id, wait_info.target_thread->name, wait_info.target_vcount);
            }
        } else {
            LOG_INFO("Display: display.mutex busy, vblank wait list skipped");
        }
    }

    if (emuenv.renderer)
        LOG_INFO("Renderer: should_display={} host_frames_presented={} batches_processed={} pipelines_compiled={}",
            emuenv.renderer->should_display, emuenv.renderer->host_frames_presented.load(),
            emuenv.renderer->batches_processed.load(std::memory_order_relaxed),
            emuenv.renderer->shaders_count_compiled);
    LOG_INFO("Memory: access_violation_traps={}", emuenv.mem.access_violations_handled.load(std::memory_order_relaxed));
}

static void dump_threads(EmuEnvState &emuenv) {
    std::vector<ThreadStatePtr> threads;
    std::vector<SceKernelModulePtr> modules;
    bool used_independent_snapshot = false;
    {
        std::unique_lock<std::mutex> lock(emuenv.kernel.mutex, std::defer_lock);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!lock.try_lock() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!lock.owns_lock()) {
            LOG_ERROR("Guest threads: kernel.mutex held >1s during stall - probable deadlock on kernel.mutex; using independent thread snapshot");
            used_independent_snapshot = true;
        } else {
            threads.reserve(emuenv.kernel.threads.size());
            for (const auto &[id, thread] : emuenv.kernel.threads)
                threads.push_back(thread);
            modules.reserve(emuenv.kernel.loaded_modules.size());
            for (const auto &[id, module] : emuenv.kernel.loaded_modules)
                modules.push_back(module);
        }
    }

    if (used_independent_snapshot) {
        const ThreadStatePtrs snapshot = emuenv.kernel.snapshot_threads_for_diagnostics();
        threads.reserve(snapshot.size());
        for (const auto &[id, thread] : snapshot)
            threads.push_back(thread);
    }

    LOG_INFO("Guest threads: {}", threads.size());
    for (const auto &thread : threads) {
        if (!thread || !thread->cpu)
            continue;

        // For running threads these register reads race with execution; the
        // values are a sample, good enough to identify a stuck loop or wait.
        const uint32_t pc = read_pc(*thread->cpu);
        const uint32_t lr = read_lr(*thread->cpu);
        const uint32_t sp = read_sp(*thread->cpu);
        const auto module_for_address = [&modules](const Address address) -> const SceKernelModuleInfo * {
            for (const auto &module : modules) {
                if (!module)
                    continue;
                for (const auto &segment : module->info.segments) {
                    if (segment.size && segment.vaddr.address() <= address
                        && address <= segment.vaddr.address() + segment.memsz)
                        return &module->info;
                }
            }
            return nullptr;
        };
        const SceKernelModuleInfo *pc_module = module_for_address(pc);
        const SceKernelModuleInfo *lr_module = module_for_address(lr);
        // A PC inside an HLE import stub means the thread is executing (or
        // parked inside) that import right now. The stub layout is
        // svc #0 / mov pc, lr / nid, and the PC sits on the mov during the
        // HLE call, so the NID names the exact API the thread is stuck in.
        const auto import_at_pc = [&emuenv](uint32_t addr) -> std::string {
            addr &= ~1u;
            if ((addr & 3) || !is_valid_addr_range(emuenv.mem, addr, addr + 12))
                return "";
            const uint32_t *words = Ptr<uint32_t>(addr).get(emuenv.mem);
            if (!words)
                return "";
            uint32_t nid = 0;
            if (words[0] == 0xe1a0f00e)
                nid = words[1];
            else if (words[0] == 0xef000000 && words[1] == 0xe1a0f00e)
                nid = words[2];
            else
                return "";
            return fmt::format(" import={}(0x{:08X})", import_name(nid), nid);
        };
        LOG_INFO("Thread {:>4} '{}': status={} priority={} PC=0x{:08X}{}{} LR=0x{:08X}{} SP=0x{:08X} entry=0x{:08X}",
            thread->id, thread->name, thread_status_str(thread->status), thread->priority,
            pc, pc_module ? fmt::format(" ({})", pc_module->module_name) : "", import_at_pc(pc),
            lr, lr_module ? fmt::format(" ({})", lr_module->module_name) : "",
            sp, thread->entry_point);
    }
}

static std::string format_waiters(ThreadDataQueue<WaitingThreadData> *queue, const bool is_eventflag) {
    std::string waiters;
    if (!queue)
        return waiters;
    for (auto it = queue->begin(); it != queue->end(); ++it) {
        const WaitingThreadData data = *it;
        if (!data.thread)
            continue;
        if (is_eventflag)
            fmt::format_to(std::back_inserter(waiters), "[{} '{}' wait=0x{:X} mode=0x{:X}] ",
                data.thread->id, data.thread->name, static_cast<uint32_t>(data.wait), static_cast<uint32_t>(data.flags));
        else
            fmt::format_to(std::back_inserter(waiters), "[{} '{}'] ", data.thread->id, data.thread->name);
    }
    return waiters;
}

template <typename PrimMap>
static void dump_primitives(EmuEnvState &emuenv, const char *kind, PrimMap &prim_map) {
    using PrimPtr = typename PrimMap::mapped_type;

    std::vector<PrimPtr> prims;
    {
        std::unique_lock<std::mutex> lock(emuenv.kernel.mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            LOG_INFO("{}: kernel.mutex busy, primitive list skipped", kind);
            return;
        }
        prims.reserve(prim_map.size());
        for (const auto &[uid, prim] : prim_map)
            prims.push_back(prim);
    }

    for (const auto &prim : prims) {
        if (!prim)
            continue;

        // Never block the dump on a primitive that is being operated on.
        std::unique_lock<std::mutex> prim_lock(prim->mutex, std::try_to_lock);
        if (!prim_lock.owns_lock()) {
            LOG_INFO("{} uid={} '{}': primitive busy, state skipped", kind, prim->uid, prim->name);
            continue;
        }

        std::string extra;
        if constexpr (requires { prim->owner; prim->lock_count; }) {
            if (prim->owner)
                extra = fmt::format(" owner={} '{}' lock_count={}", prim->owner->id, prim->owner->name, prim->lock_count);
        } else if constexpr (requires { prim->val; prim->max; }) {
            extra = fmt::format(" val={} max={}", prim->val, prim->max);
        } else if constexpr (requires { prim->flags; }) {
            extra = fmt::format(" flags=0x{:X}", static_cast<uint32_t>(prim->flags));
        }

        constexpr bool is_eventflag = requires { prim->flags; };

        if constexpr (requires { prim->senders; prim->receivers; }) {
            const std::string senders = format_waiters(prim->senders.get(), false);
            const std::string receivers = format_waiters(prim->receivers.get(), false);
            if (!senders.empty() || !receivers.empty())
                LOG_INFO("{} uid={} '{}'{}: senders: {} receivers: {}", kind, prim->uid, prim->name, extra,
                    senders, receivers);
        } else {
            const std::string waiters = format_waiters(prim->waiting_threads.get(), is_eventflag);
            if (!waiters.empty())
                LOG_INFO("{} uid={} '{}'{}: waiters: {}", kind, prim->uid, prim->name, extra, waiters);
        }
    }
}

static void dump_audio_state(EmuEnvState &emuenv) {
    std::unique_lock<std::mutex> lock(emuenv.audio.mutex, std::try_to_lock);
    LOG_INFO("Audio: backend='{}' adapter_present={} out_ports={} output_calls={} device_pulls={}",
        emuenv.audio.audio_backend, emuenv.audio.adapter != nullptr,
        lock.owns_lock() ? std::to_string(emuenv.audio.out_ports.size()) : "(busy)",
        emuenv.audio.output_calls.load(std::memory_order_relaxed),
        emuenv.audio.device_pulls.load(std::memory_order_relaxed));
    if (!lock.owns_lock() || !emuenv.audio.adapter)
        return;
    for (const auto &[port_id, port] : emuenv.audio.out_ports) {
        if (!port)
            continue;
        LOG_INFO("Audio port {}: len={} freq={} mode={} stopping={} rest_samples={}",
            port_id, port->len, port->freq, port->mode, port->stopping.load(),
            emuenv.audio.adapter->get_rest_sample(*port));
    }
}

static void dump_ngs_state(EmuEnvState &emuenv) {
    static std::mutex busy_observations_mutex;
    static std::map<ngs::System *, std::chrono::steady_clock::time_point> busy_observations;
    LOG_INFO("NGS: enabled={} systems={}", emuenv.cfg.current_config.ngs_enable, emuenv.ngs.systems.size());
    for (ngs::System *system : emuenv.ngs.systems) {
        if (!system)
            continue;
        ngs::VoiceScheduler &scheduler = system->voice_scheduler;
        std::unique_lock<std::recursive_mutex> lock(scheduler.mutex, std::try_to_lock);
        if (lock.owns_lock()) {
            {
                const std::lock_guard observation_lock(busy_observations_mutex);
                busy_observations.erase(system);
            }
            LOG_INFO("NGS system: racks={} max_voices={} granularity={} sample_rate={} queued_voices={} pending_ops={} is_updating={}",
                system->racks.size(), system->max_voices, system->granularity, system->sample_rate,
                scheduler.queue.size(), scheduler.operations_pending.size(), scheduler.is_updating.load(std::memory_order_acquire));
        } else {
            const auto now = std::chrono::steady_clock::now();
            std::chrono::milliseconds busy_for{ 0 };
            bool consecutive = false;
            {
                const std::lock_guard observation_lock(busy_observations_mutex);
                const auto [it, inserted] = busy_observations.emplace(system, now);
                consecutive = !inserted;
                busy_for = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
            }
            if (consecutive) {
                LOG_ERROR("NGS system: racks={} max_voices={} granularity={} sample_rate={} (scheduler busy for at least {}ms across consecutive dumps, is_updating={})",
                    system->racks.size(), system->max_voices, system->granularity, system->sample_rate,
                    busy_for.count(), scheduler.is_updating.load(std::memory_order_acquire));
            } else {
                LOG_INFO("NGS system: racks={} max_voices={} granularity={} sample_rate={} (scheduler busy first observed, is_updating={})",
                    system->racks.size(), system->max_voices, system->granularity, system->sample_rate,
                    scheduler.is_updating.load(std::memory_order_acquire));
            }
        }
    }
}

void dump_guest_state(EmuEnvState &emuenv, const char *reason) {
    LOG_INFO("===== Guest state dump begin: {} =====", reason);

    dump_display_state(emuenv);
    dump_threads(emuenv);

    dump_primitives(emuenv, "Mutex", emuenv.kernel.mutexes);
    dump_primitives(emuenv, "LwMutex", emuenv.kernel.lwmutexes);
    dump_primitives(emuenv, "Semaphore", emuenv.kernel.semaphores);
    dump_primitives(emuenv, "EventFlag", emuenv.kernel.eventflags);
    dump_primitives(emuenv, "Condvar", emuenv.kernel.condvars);
    dump_primitives(emuenv, "LwCondvar", emuenv.kernel.lwcondvars);
    dump_primitives(emuenv, "SimpleEvent", emuenv.kernel.simple_events);
    dump_primitives(emuenv, "Timer", emuenv.kernel.timers);
    dump_primitives(emuenv, "RWLock", emuenv.kernel.rwlocks);
    dump_primitives(emuenv, "MsgPipe", emuenv.kernel.msgpipes);

    dump_audio_state(emuenv);
    dump_ngs_state(emuenv);

    LOG_INFO("===== Guest state dump end =====");
}

} // namespace app
