#pragma once

#include <cstdint>
#include <cstring>

// Hand-rolled mirror of the engine's TSet<FMassEntityHandle, DefaultKeyFuncs<...>,
// FDefaultSetAllocator> memory layout (confirmed against the decompiled
// AddEntitiesToWave loop, which only ever reads Elements.AllocationFlags and
// Elements.Data - the hash table fields are never touched). Building a batch
// this way lets us hand the *real* original function a small TSet instead of
// the huge one the game tried to pass, reusing 100% of its own per-entity
// logic (IsEntityValid, AddTagsInternal command, SignalEntity, EntitiesInWave
// append) untouched.
//
// Layout mirrors:
//   TSparseArray<TSetElement<T>>::Data           -> TArray<T> {Data, Num, Max}
//   TSparseArray<TSetElement<T>>::AllocationFlags -> TBitArray<FDefaultBitArrayAllocator>
//   TSparseArray<TSetElement<T>>::FirstFreeIndex / NumFreeIndices
//   TSet::Hash / HashSize (unused by the loop - left zeroed)
//
// Capacity is capped at 128 entries so the bit array never needs to spill out
// of its 4-DWORD inline allocation (TInlineAllocator<4> == 128 bits) - no
// heap allocation is required to build a batch.
struct FMassEntityHandle
{
    int32_t Index;
    int32_t SerialNumber;
};

namespace MassEntitySet
{
    constexpr int kMaxBatchCapacity = 128;

    struct TSetElement
    {
        FMassEntityHandle Value;
        int32_t HashNextId; // unused by AddEntitiesToWave's loop
        int32_t HashIndex;  // unused by AddEntitiesToWave's loop
    };

    // Mirrors TArray<T>: {T* Data; int32 Num; int32 Max;}
    struct ScriptArray
    {
        TSetElement* Data;
        int32_t Num;
        int32_t Max;
    };

    // Mirrors TBitArray<FDefaultBitArrayAllocator> with its 4-DWORD inline
    // allocation: {uint32 InlineWords[4]; uint32* SecondaryData; int32 NumBits; int32 MaxBits;}
    struct ScriptBitArray
    {
        uint32_t InlineWords[4];
        uint32_t* SecondaryData; // always null here -> engine code falls back to InlineWords, exactly like the decompiled fallback
        int32_t NumBits;
        int32_t MaxBits;
    };

    // Mirrors TSparseArray<TSetElement<T>>
    struct ScriptSparseArray
    {
        ScriptArray Data;
        ScriptBitArray AllocationFlags;
        int32_t FirstFreeIndex;
        int32_t NumFreeIndices;
    };

    // Mirrors TInlineAllocator<1>::ForElementType<int32> + int32 HashSize for the
    // hash table tail of TSet. Never read by AddEntitiesToWave - zeroed only so
    // the memory is well-defined.
    struct HashTail
    {
        uint32_t InlineWord;
        uint32_t _pad;
        uint32_t* SecondaryData;
        int32_t HashSize;
    };

    // Exact byte-for-byte mirror of TSet<FMassEntityHandle, ..., FDefaultSetAllocator>.
    struct ScriptSet
    {
        ScriptSparseArray Elements;
        HashTail Hash;
    };

    // Fixed-capacity batch builder. Construct, call Reset(), Add() up to
    // kMaxBatchCapacity handles, then pass AsTSet() to the original engine
    // function as if it were a real TSet<FMassEntityHandle>*.
    class Batch
    {
    public:
        void Reset()
        {
            m_count = 0;
        }

        bool Add(FMassEntityHandle handle)
        {
            if (m_count >= kMaxBatchCapacity)
                return false;

            m_elements[m_count].Value = handle;
            m_elements[m_count].HashNextId = 0;
            m_elements[m_count].HashIndex = 0;
            ++m_count;
            return true;
        }

        int Count() const { return m_count; }
        bool IsFull() const { return m_count >= kMaxBatchCapacity; }
        FMassEntityHandle GetEntity(int index) const { return m_elements[index].Value; }

        // Builds the live ScriptSet view over the current batch contents.
        // Valid only until the next Reset()/Add() call - build, call the
        // original function immediately, then discard.
        const ScriptSet* AsScriptSet()
        {
            std::memset(&m_set, 0, sizeof(m_set));

            m_set.Elements.Data.Data = m_elements;
            m_set.Elements.Data.Num = m_count;
            m_set.Elements.Data.Max = m_count;

            // All m_count bits set contiguously from bit 0 - matches a
            // freshly-packed, hole-free batch.
            for (int i = 0; i < m_count; ++i)
                m_set.Elements.AllocationFlags.InlineWords[i / 32] |= (1u << (i % 32));

            m_set.Elements.AllocationFlags.SecondaryData = nullptr;
            m_set.Elements.AllocationFlags.NumBits = m_count;
            m_set.Elements.AllocationFlags.MaxBits = kMaxBatchCapacity;

            m_set.Elements.FirstFreeIndex = -1;
            m_set.Elements.NumFreeIndices = 0;

            return &m_set;
        }

    private:
        TSetElement m_elements[kMaxBatchCapacity];
        ScriptSet m_set;
        int m_count = 0;
    };
}
