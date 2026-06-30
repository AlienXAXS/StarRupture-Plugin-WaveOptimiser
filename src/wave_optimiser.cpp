#include "wave_optimiser.h"
#include "mass_entity_set.h"
#include "wave_optimiser_signatures.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <Windows.h>

namespace WaveOptimiser
{
    std::mutex g_statsMutex;

    // FMassCommandBuffer::AddTag<FCrMassTemperatureUpdatingTag>(this, Entity).
    typedef void(__fastcall* AddTagUpdatingTag_t)(void* CmdBuf, FMassEntityHandle Entity);
    AddTagUpdatingTag_t g_addTagUpdatingTag = nullptr;

    // FMassEntityManager::GetFragmentDataPtr<FCrMassTemperatureFragment>(this, Entity) -> void*
    // Returns nullptr if entity doesn't have the fragment.
    typedef void*(__fastcall* GetFragmentDataPtr_t)(void* EntityMgr, FMassEntityHandle Entity);
    GetFragmentDataPtr_t g_getFragmentDataPtr = nullptr;

    // FCrMassTemperatureFragment::CurrentHeat sits at offset 0x0.
    // Set high enough to guarantee "too hot" on the first temperature processor tick.
    constexpr float kInjectHeat = 9999.0f;

    // Returns the currently open FMassCommandBuffer* from a live FMassEntityManager*.
    // Layout confirmed from struct dump:
    //   +0x510  DeferredCommandBuffers  TStaticArray<TSharedPtr<FMassCommandBuffer,1>,2,8>
    //   +0x530  OpenedCommandBufferIndex  uint8
    // Each TSharedPtr is 16 bytes; Object* is the first field.
    static void* GetCommandBuffer(void* entityMgr)
    {
        if (!entityMgr) return nullptr;
        // Validate the entityMgr pointer covers the fields we need before reading.
        if (IsBadReadPtr(entityMgr, 0x540)) return nullptr;
        const uint8_t idx = *(uint8_t*)((char*)entityMgr + 0x530);
        if (idx > 1) return nullptr;
        void* ptr = *(void**)((char*)entityMgr + 0x510 + (size_t)idx * 16);
        // Validate the returned command buffer pointer before callers touch its fields.
        if (!ptr || IsBadReadPtr(ptr, 0x60)) return nullptr;
        return ptr;
    }

    namespace
    {
        enum class WaveOp : uint8_t { Add, Remove };

        struct QueuedEntity
        {
            FMassEntityHandle handle;
            uint8_t waveType; // only meaningful when op == Add
            void* subsystem;
            WaveOp op;
        };

        typedef void(__fastcall* AddEntitiesToWave_t)(void* thisPtr, const MassEntitySet::ScriptSet* entitiesToAdd, uint8_t waveType);
        typedef void(__fastcall* RemoveEntitiesFromWave_t)(void* thisPtr, const MassEntitySet::ScriptSet* entitiesToRemove);

        AddEntitiesToWave_t       g_original = nullptr;
        RemoveEntitiesFromWave_t  g_originalRemove = nullptr;
        HookHandle                g_hook = nullptr;
        HookHandle                g_hookRemove = nullptr;
        // g_statsMutex and g_trackedHandles live in the enclosing named namespace
        // so IsEntityValidDiag (defined later in the same namespace) can reach them.
        std::deque<QueuedEntity> g_queue;

        // g_statsMutex guards g_queue, g_trackedHandles, and the counters below.
        // The hook and OnTick() run on the game thread; GetStats() is polled from
        // the render thread by the debug panel. g_statsMutex is defined in the
        // enclosing named namespace so IsEntityValidDiag can also reach it.

        // Lifetime/live counters backing GetStats() - see header for field meanings.
        size_t   g_currentBurstTotal = 0;
        uint64_t g_totalCaptured = 0;
        uint64_t g_totalProcessed = 0;
        uint64_t g_totalBatchesReplayed = 0;
        uint8_t  g_lastWaveType = 0;

        // Detour for UCrMassEnviroWaveSubsystem::AddEntitiesToWave. Copies the
        // (potentially huge) incoming TSet's entity handles into our own queue
        // and returns immediately instead of running the original synchronous
        // per-entity loop - that loop is what freezes the game on a large save.
        // OnTick() replays the captured handles through the real original
        // function in small batches across many ticks.
        void __fastcall Detour_AddEntitiesToWave(void* thisPtr, const MassEntitySet::ScriptSet* entitiesToAdd, uint8_t waveType)
        {
            if (!entitiesToAdd)
                return;

            const int numBits = entitiesToAdd->Elements.AllocationFlags.NumBits;
            const uint32_t* bits = entitiesToAdd->Elements.AllocationFlags.SecondaryData
                ? entitiesToAdd->Elements.AllocationFlags.SecondaryData
                : entitiesToAdd->Elements.AllocationFlags.InlineWords;
            const MassEntitySet::TSetElement* elements = entitiesToAdd->Elements.Data.Data;

            std::lock_guard<std::mutex> lock(g_statsMutex);

            // A fresh capture burst starts whenever the queue was empty -
            // used purely to size the debug panel's progress bar.
            if (g_queue.empty())
                g_currentBurstTotal = 0;

            int captured = 0;
            for (int i = 0; i < numBits; ++i)
            {
                if (bits[i / 32] & (1u << (i % 32)))
                {
                    g_queue.push_back({ elements[i].Value, waveType, thisPtr, WaveOp::Add });
                    LOG_TRACE("WaveOptimiser: captured Add entity Index=%d SerialNumber=%d (wave type %u)",
                        elements[i].Value.Index, elements[i].Value.SerialNumber, static_cast<unsigned>(waveType));
                    ++captured;
                }
            }

            g_currentBurstTotal += static_cast<size_t>(captured);
            g_totalCaptured += static_cast<uint64_t>(captured);
            g_lastWaveType = waveType;

            // Per-call summary - infrequent (once per wave trigger), not hot-path.
            LOG_DEBUG("WaveOptimiser: deferred %d Add entities (wave type %u) - queue depth now %zu",
                captured, static_cast<unsigned>(waveType), g_queue.size());

            // Deliberately not calling g_original here - see header comment.
        }

        // Detour for UCrMassEnviroWaveSubsystem::RemoveEntitiesFromWave. Must be
        // hooked alongside AddEntitiesToWave and pushed into the *same* ordered
        // queue: RemoveEntitiesFromWave mutates this->EntitiesInWave (a set
        // difference) and removes the wave-stage tag, which is the same state
        // AddEntitiesToWave establishes. If Remove ran synchronously while
        // matching Adds were still sitting in the queue (e.g. the wave reverses
        // direction mid-drain), entities would get tagged/added to EntitiesInWave
        // *after* they should already have been removed - corrupting wave-stage
        // membership and the heat ramp that depends on it.
        void __fastcall Detour_RemoveEntitiesFromWave(void* thisPtr, const MassEntitySet::ScriptSet* entitiesToRemove)
        {
            if (!entitiesToRemove)
                return;

            const int numBits = entitiesToRemove->Elements.AllocationFlags.NumBits;
            const uint32_t* bits = entitiesToRemove->Elements.AllocationFlags.SecondaryData
                ? entitiesToRemove->Elements.AllocationFlags.SecondaryData
                : entitiesToRemove->Elements.AllocationFlags.InlineWords;
            const MassEntitySet::TSetElement* elements = entitiesToRemove->Elements.Data.Data;

            std::lock_guard<std::mutex> lock(g_statsMutex);

            if (g_queue.empty())
                g_currentBurstTotal = 0;

            int captured = 0;
            for (int i = 0; i < numBits; ++i)
            {
                if (bits[i / 32] & (1u << (i % 32)))
                {
                    g_queue.push_back({ elements[i].Value, 0, thisPtr, WaveOp::Remove });
                    LOG_TRACE("WaveOptimiser: captured Remove entity Index=%d SerialNumber=%d",
                        elements[i].Value.Index, elements[i].Value.SerialNumber);
                    ++captured;
                }
            }

            g_currentBurstTotal += static_cast<size_t>(captured);
            g_totalCaptured += static_cast<uint64_t>(captured);

            LOG_DEBUG("WaveOptimiser: deferred %d Remove entities - queue depth now %zu",
                captured, g_queue.size());

            // Deliberately not calling g_originalRemove here - see header comment.
        }
    } // namespace

    bool Init(IPluginSelf* self)
    {
        IPluginScanner* scanner = self->scanner;

        // Resolve GetFragmentDataPtr from a call-site xref: read rel32 at xrefAddr+1.
        uintptr_t fragXRef = scanner->FindPatternInMainModule(WaveOptimiserSignatures::GetFragmentDataPtr_XRef);
        if (fragXRef)
        {
            const int32_t rel = *reinterpret_cast<const int32_t*>(fragXRef + 1);
            g_getFragmentDataPtr = reinterpret_cast<GetFragmentDataPtr_t>(fragXRef + 5 + rel);
            LOG_INFO("WaveOptimiser: GetFragmentDataPtr resolved at 0x%llX",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_getFragmentDataPtr)));
        }
        else
        {
            LOG_WARN("WaveOptimiser: GetFragmentDataPtr xref not found - heat injection disabled");
        }

        uintptr_t addTagXRef = scanner->FindPatternInMainModule(WaveOptimiserSignatures::AddTagUpdatingTag_XRef);
        if (addTagXRef)
        {
            const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            LOG_INFO("WaveOptimiser: AddTagUpdatingTag_XRef callsite at module+0x%llX (VA=0x%llX)",
                static_cast<unsigned long long>(addTagXRef - moduleBase),
                static_cast<unsigned long long>(addTagXRef));

            // Decode the rel32 call target from the E8 instruction at the callsite.
            const int32_t rel2 = *reinterpret_cast<const int32_t*>(addTagXRef + 1);
            g_addTagUpdatingTag = reinterpret_cast<AddTagUpdatingTag_t>(addTagXRef + 5 + rel2);
            LOG_INFO("WaveOptimiser: AddTagUpdatingTag resolved at module+0x%llX (VA=0x%llX)",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_addTagUpdatingTag) - moduleBase),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_addTagUpdatingTag)));
        }
        else
        {
            LOG_WARN("WaveOptimiser: AddTagUpdatingTag xref not found - UpdatingTag repair disabled");
        }

        LOG_DEBUG("WaveOptimiser: scanning for AddEntitiesToWave...");

        uintptr_t addAddr = scanner->FindPatternInMainModule(WaveOptimiserSignatures::AddEntitiesToWave);
        if (!addAddr)
        {
            LOG_WARN("WaveOptimiser: AddEntitiesToWave pattern not found - wave batching not installed");
            return false;
        }

        LOG_DEBUG("WaveOptimiser: scanning for RemoveEntitiesFromWave...");

        uintptr_t removeAddr = scanner->FindPatternInMainModule(WaveOptimiserSignatures::RemoveEntitiesFromWave);
        if (!removeAddr)
        {
            LOG_WARN("WaveOptimiser: RemoveEntitiesFromWave pattern not found - wave batching not installed "
                "(refusing to hook Add alone, that would desync EntitiesInWave ordering)");
            return false;
        }

        g_hook = self->hooks->Hooks->Install(
            addAddr,
            reinterpret_cast<void*>(&Detour_AddEntitiesToWave),
            reinterpret_cast<void**>(&g_original));

        if (!g_hook)
        {
            LOG_WARN("WaveOptimiser: AddEntitiesToWave hook installation failed");
            return false;
        }

        g_hookRemove = self->hooks->Hooks->Install(
            removeAddr,
            reinterpret_cast<void*>(&Detour_RemoveEntitiesFromWave),
            reinterpret_cast<void**>(&g_originalRemove));

        if (!g_hookRemove)
        {
            LOG_WARN("WaveOptimiser: RemoveEntitiesFromWave hook installation failed");
            self->hooks->Hooks->Remove(g_hook);
            g_hook = nullptr;
            g_original = nullptr;
            return false;
        }

        LOG_INFO("WaveOptimiser: AddEntitiesToWave hooked at 0x%llX, RemoveEntitiesFromWave hooked at 0x%llX",
            static_cast<unsigned long long>(addAddr), static_cast<unsigned long long>(removeAddr));
        return true;
    }

    void Shutdown(IPluginSelf* self)
    {
        if (g_hook && self->hooks->Hooks)
        {
            self->hooks->Hooks->Remove(g_hook);
            g_hook = nullptr;
            g_original = nullptr;
        }

        if (g_hookRemove && self->hooks->Hooks)
        {
            self->hooks->Hooks->Remove(g_hookRemove);
            g_hookRemove = nullptr;
            g_originalRemove = nullptr;
        }

        LOG_DEBUG("WaveOptimiser: hooks removed");

        std::lock_guard<std::mutex> lock(g_statsMutex);
        g_queue.clear();
        g_currentBurstTotal = 0;
    }

    void OnTick(float /*deltaSeconds*/)
    {
        if (!g_originalRemove || g_queue.empty())
            return;

        const int maxPerTick = WaveOptimiserConfig::Config::GetMaxEntitiesPerTick();
        int processed = 0;

        MassEntitySet::Batch batch;

        std::unique_lock<std::mutex> lock(g_statsMutex);

        while (processed < maxPerTick && !g_queue.empty())
        {
            const QueuedEntity first = g_queue.front();
            void* subsystem = first.subsystem;
            uint8_t waveType = first.waveType;
            WaveOp op = first.op;

            batch.Reset();

            // Group consecutive entries sharing the same subsystem instance,
            // op (Add/Remove) and wave type into one batch - stop at a
            // mismatched op/subsystem/waveType, batch capacity, or per-tick
            // budget. Never merging across an op boundary is what preserves
            // the original Add/Remove call order on replay.
            while (!g_queue.empty()
                && processed < maxPerTick
                && !batch.IsFull()
                && g_queue.front().subsystem == subsystem
                && g_queue.front().op == op
                && (op == WaveOp::Remove || g_queue.front().waveType == waveType))
            {
                batch.Add(g_queue.front().handle);
                g_queue.pop_front();
                ++processed;
            }

            if (batch.Count() > 0)
            {
                // Hot path - fires every tick while a large backlog drains.
                LOG_TRACE("WaveOptimiser: replaying %s batch of %d entities (wave type %u, queue depth now %zu)",
                    op == WaveOp::Add ? "Add" : "Remove",
                    batch.Count(), static_cast<unsigned>(waveType), g_queue.size());

                // Drop the lock while calling into engine code - GetStats() is a
                // quick poll from the render thread and shouldn't be blocked for
                // the duration of the (potentially non-trivial) original call.
                lock.unlock();
                if (op == WaveOp::Add)
                {
                    // Bypass the original AddEntitiesToWave entirely. Instead:
                    //  1. Write CurrentHeat directly to each entity's temperature
                    //     fragment — guarantees "too hot" regardless of tag state.
                    //  2. Add FCrMassTemperatureUpdatingTag so the temperature
                    //     processor picks up the entity next tick, detects the
                    //     non-zero heat, and fires TemperatureChanged_16 naturally.
                    void* entityMgr = IsEntityValidDiag::GetEntityMgr();
                    void* cmdBuf    = GetCommandBuffer(entityMgr);

                    const bool cmdBufReady = cmdBuf
                        && !*(bool*)    ((char*)cmdBuf + 0x54)   // bIsFlushing
                        &&  (*(uint32_t*)((char*)cmdBuf + 0x58)) == GetCurrentThreadId(); // OwnerThreadId

                    for (int e = 0; e < batch.Count(); ++e)
                    {
                        const FMassEntityHandle entity = batch.GetEntity(e);

                        if (g_getFragmentDataPtr && entityMgr)
                        {
                            void* frag = g_getFragmentDataPtr(entityMgr, entity);
                            if (frag)
                                *(float*)frag = kInjectHeat; // CurrentHeat @ offset 0x0
                        }

                        if (g_addTagUpdatingTag && cmdBufReady)
                        {
                            // Validate call target is in executable memory before first use.
                            static bool s_addTagValidated = false;
                            static bool s_addTagBad       = false;
                            if (!s_addTagValidated)
                            {
                                s_addTagValidated = true;
                                MEMORY_BASIC_INFORMATION mbi{};
                                if (VirtualQuery(reinterpret_cast<void*>(g_addTagUpdatingTag), &mbi, sizeof(mbi)) == 0
                                    || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                                {
                                    s_addTagBad = true;
                                    LOG_WARN("WaveOptimiser: AddTagUpdatingTag resolved to non-executable memory 0x%llX - tag add disabled",
                                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_addTagUpdatingTag)));
                                }
                                else
                                {
                                    LOG_INFO("WaveOptimiser: AddTagUpdatingTag at 0x%llX validated executable",
                                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_addTagUpdatingTag)));
                                }
                            }
                            if (!s_addTagBad)
                                g_addTagUpdatingTag(cmdBuf, entity);
                        }
                    }
                }
                else
                {
                    // Remove: original still handles EntitiesInWave bookkeeping.
                    g_originalRemove(subsystem, batch.AsScriptSet());
                }
                lock.lock();

                g_totalProcessed += static_cast<uint64_t>(batch.Count());
                ++g_totalBatchesReplayed;
            }
        }

        if (g_queue.empty())
        {
            g_currentBurstTotal = 0;
            LOG_DEBUG("WaveOptimiser: queue drained");
        }
    }

    Stats GetStats()
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);

        Stats stats{};
        stats.hookInstalled = g_hook != nullptr;
        stats.queueDepth = g_queue.size();
        stats.currentBurstTotal = g_currentBurstTotal;
        stats.totalCaptured = g_totalCaptured;
        stats.totalProcessed = g_totalProcessed;
        stats.totalBatchesReplayed = g_totalBatchesReplayed;
        stats.lastWaveType = g_lastWaveType;
        return stats;
    }
} // namespace WaveOptimiser

// ---------------------------------------------------------------------------
// DIAGNOSTIC: FMassEntityManager::IsEntityValid hook - filtered to our
// captured entities only. Tells us whether entities are still valid by the
// time g_original calls IsEntityValid during replay.
// ---------------------------------------------------------------------------
namespace IsEntityValidDiag
{
    namespace
    {
        typedef bool(__fastcall* IsEntityValid_t)(void* EntityManager, FMassEntityHandle Entity);

        IsEntityValid_t g_original = nullptr;
        HookHandle      g_hook     = nullptr;

        // Captured from the first IsEntityValid call — all callers share the same
        // FMassEntityManager instance per world so any call gives us the right ptr.
        // Atomic because IsEntityValid is called from parallel Mass processors.
        std::atomic<void*> g_globalEntityMgr{nullptr};

        bool __fastcall Detour(void* EntityManager, FMassEntityHandle Entity)
        {
            if (!g_globalEntityMgr.load(std::memory_order_relaxed))
                g_globalEntityMgr.store(EntityManager, std::memory_order_relaxed);
            return g_original(EntityManager, Entity);
        }
    }

    bool Init(IPluginSelf* self)
    {
        uintptr_t addr = self->scanner->FindPatternInMainModule(
            WaveOptimiserSignatures::IsEntityValid);
        if (!addr)
        {
            LOG_WARN("IsEntityValidDiag: pattern not found");
            return false;
        }
        g_hook = self->hooks->Hooks->Install(
            addr,
            reinterpret_cast<void*>(&Detour),
            reinterpret_cast<void**>(&g_original));
        if (!g_hook)
        {
            LOG_WARN("IsEntityValidDiag: hook install failed");
            return false;
        }
        LOG_INFO("IsEntityValidDiag: hooked at 0x%llX", static_cast<unsigned long long>(addr));
        return true;
    }

    void Shutdown(IPluginSelf* self)
    {
        if (g_hook && self->hooks->Hooks)
        {
            self->hooks->Hooks->Remove(g_hook);
            g_hook     = nullptr;
            g_original = nullptr;
        }
    }

    void* GetEntityMgr() { return g_globalEntityMgr.load(std::memory_order_relaxed); }
}

// ---------------------------------------------------------------------------
// DIAGNOSTIC: UCrBuildingFunctionLibrary::AddTemperatureToEntity hook
// Log every call so we can see what World/Entity/Value the game uses in the
// working (no-plugin) path. Remove this whole block once understood.
// ---------------------------------------------------------------------------
namespace AddTemperatureDiag
{
    namespace
    {
        // (UWorld*, FMassEntityHandle, float)
        // x64 __fastcall: rcx=World, rdx=EntityHandle (Index=low32, SerialNumber=high32), xmm2=Value
        typedef void(__fastcall* AddTemperatureToEntity_t)(void* World, FMassEntityHandle EntityHandle, float Value);

        AddTemperatureToEntity_t g_original = nullptr;
        HookHandle               g_hook     = nullptr;

        void __fastcall Detour(void* World, FMassEntityHandle EntityHandle, float Value)
        {
            LOG_INFO("AddTemperatureToEntity: World=0x%p  Entity=[%d,%d]  Value=%.3f",
                World, EntityHandle.Index, EntityHandle.SerialNumber, Value);
            g_original(World, EntityHandle, Value);
        }
    }

    bool Init(IPluginSelf* self)
    {
        uintptr_t addr = self->scanner->FindPatternInMainModule(
            WaveOptimiserSignatures::AddTemperatureToEntity);
        if (!addr)
        {
            LOG_WARN("AddTemperatureDiag: pattern not found");
            return false;
        }
        g_hook = self->hooks->Hooks->Install(
            addr,
            reinterpret_cast<void*>(&Detour),
            reinterpret_cast<void**>(&g_original));
        if (!g_hook)
        {
            LOG_WARN("AddTemperatureDiag: hook install failed");
            return false;
        }
        LOG_INFO("AddTemperatureDiag: hooked at 0x%llX", static_cast<unsigned long long>(addr));
        return true;
    }

    void Shutdown(IPluginSelf* self)
    {
        if (g_hook && self->hooks->Hooks)
        {
            self->hooks->Hooks->Remove(g_hook);
            g_hook     = nullptr;
            g_original = nullptr;
        }
    }
}
