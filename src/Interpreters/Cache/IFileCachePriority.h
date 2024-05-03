#pragma once

#include <memory>
#include <Core/Types.h>
#include <Common/Exception.h>
#include <Interpreters/Cache/FileSegmentInfo.h>
#include <Interpreters/Cache/Guards.h>
#include <Interpreters/Cache/IFileCachePriority.h>
#include <Interpreters/Cache/FileCache_fwd_internal.h>
#include <Interpreters/Cache/UserInfo.h>
#include <magic_enum.hpp>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

struct FileCacheReserveStat;
class EvictionCandidates;

class IFileCachePriority : private boost::noncopyable
{
public:
    using Key = FileCacheKey;
    using QueueEntryType = FileCacheQueueEntryType;
    using UserInfo = FileCacheUserInfo;
    using UserID = UserInfo::UserID;

    struct Entry
    {
        Entry(const Key & key_, size_t offset_, size_t size_, KeyMetadataPtr key_metadata_);
        Entry(const Entry & other);

        std::string toString() const { return fmt::format("{}:{}:{}", key, offset, size); }

        const Key key;
        const size_t offset;
        const KeyMetadataPtr key_metadata;

        std::atomic<size_t> size;
        size_t hits = 0;

        struct StateHolder;
        using StateHolderPtr = std::shared_ptr<StateHolder>;

        const StateHolderPtr state_holder;

        enum class State {
            None, /// Queue entry is not created yet.
            Created, /// Queue entry created.
            Evicting, /// Queue entry is in process of eviction.
            Evicted, /// Queue entry is evicted.
        };
        struct StateHolder
        {
            State getState(const CachePriorityGuard::Lock &) const { return state; }
            State getState(const LockedKey &) const { return state; }
            /// This does not look good to have getState with two options for locks,
            /// but still it is valid as we do setState always under both of them.
            /// (Well, not always - only always for setting it to True,
            /// but for False we have lower guarantees and allow a logical race,
            /// physical race is not possible because the value is atomic).
            /// We can avoid this ambiguity for getState by introducing
            /// a separate lock `EntryGuard::Lock`, it will make this part of code more coherent,
            /// but it will introduce one more mutex while it is avoidable.
            /// Introducing one more mutex just for coherency does not win the trade-off (isn't it?).

            void setEvictingState(const LockedKey &, const CachePriorityGuard::Lock &)
            {
                setStateImpl(State::Evicting);
            }

            void setEvictedState(const LockedKey &, const CachePriorityGuard::Lock &)
            {
                setStateImpl(State::Evicting);
            }

            void resetEvictingState()
            {
                if (state != State::Evicting)
                {
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                                    "Expected state `Evicting`, got: {}",
                                    magic_enum::enum_name(state.load()));
                }
                setStateImpl(State::Created);
            }

        private:
            std::atomic<State> state = State::None;

            void setStateImpl(State state_)
            {
                auto prev = state.exchange(state_, std::memory_order_relaxed);
                chassert(prev != state_);
                UNUSED(prev);
            }
        };
    };
    using EntryPtr = std::shared_ptr<Entry>;

    class Iterator
    {
    public:
        virtual ~Iterator() = default;

        virtual EntryPtr getEntry() const = 0;

        virtual size_t increasePriority(const CachePriorityGuard::Lock &) = 0;

        /// Note: IncrementSize unlike decrementSize requires a cache lock, because
        /// it requires more consistency guarantees for eviction.

        virtual void incrementSize(size_t size, const CachePriorityGuard::Lock &) = 0;

        virtual void decrementSize(size_t size) = 0;

        virtual void remove(const CachePriorityGuard::Lock &) = 0;

        virtual void invalidate() = 0;

        virtual QueueEntryType getType() const = 0;
    };
    using IteratorPtr = std::shared_ptr<Iterator>;

    virtual ~IFileCachePriority() = default;

    size_t getElementsLimit(const CachePriorityGuard::Lock &) const { return max_elements; }

    size_t getSizeLimit(const CachePriorityGuard::Lock &) const { return max_size; }

    virtual size_t getSize(const CachePriorityGuard::Lock &) const = 0;

    virtual size_t getSizeApprox() const = 0;

    virtual size_t getElementsCount(const CachePriorityGuard::Lock &) const = 0;

    virtual size_t getElementsCountApprox() const = 0;

    virtual std::string getStateInfoForLog(const CachePriorityGuard::Lock &) const = 0;

    virtual void check(const CachePriorityGuard::Lock &) const;

    /// Throws exception if there is not enough size to fit it.
    virtual IteratorPtr add( /// NOLINT
        KeyMetadataPtr key_metadata,
        size_t offset,
        size_t size,
        const UserInfo & user,
        const CachePriorityGuard::Lock &,
        bool best_effort = false) = 0;

    /// `reservee` is the entry for which are reserving now.
    /// It does not exist, if it is the first space reservation attempt
    /// for the corresponding file segment.
    virtual bool canFit( /// NOLINT
        size_t size,
        size_t elements,
        const CachePriorityGuard::Lock &,
        IteratorPtr reservee = nullptr,
        bool best_effort = false) const = 0;

    virtual void shuffle(const CachePriorityGuard::Lock &) = 0;

    struct IPriorityDump
    {
        virtual ~IPriorityDump() = default;
    };
    using PriorityDumpPtr = std::shared_ptr<IPriorityDump>;

    virtual PriorityDumpPtr dump(const CachePriorityGuard::Lock &) = 0;

    virtual bool collectCandidatesForEviction(
        size_t size,
        size_t elements,
        FileCacheReserveStat & stat,
        EvictionCandidates & res,
        IteratorPtr reservee,
        const UserID & user_id,
        const CachePriorityGuard::Lock &) = 0;

    /// Collect eviction `candidates_num` candidates for eviction.
    virtual bool collectCandidatesForEviction(
        size_t desired_size,
        size_t desired_elements_count,
        size_t max_candidates_to_evict,
        FileCacheReserveStat & stat,
        EvictionCandidates & candidates,
        const CachePriorityGuard::Lock &) = 0;

    virtual bool modifySizeLimits(
        size_t max_size_,
        size_t max_elements_,
        double size_ratio_,
        const CachePriorityGuard::Lock &) = 0;

    /// A space holder implementation, which allows to take hold of
    /// some space in cache given that this space was freed.
    /// Takes hold of the space in constructor and releases it in destructor.
    struct HoldSpace : private boost::noncopyable
    {
        HoldSpace(
            size_t size_,
            size_t elements_,
            IFileCachePriority & priority_,
            const CachePriorityGuard::Lock & lock)
            : size(size_), elements(elements_), priority(priority_)
        {
            priority.holdImpl(size, elements, lock);
        }

        void release()
        {
            if (released)
                return;
            released = true;
            priority.releaseImpl(size, elements);
        }

        ~HoldSpace()
        {
            if (!released)
                release();
        }

    private:
        const size_t size;
        const size_t elements;
        IFileCachePriority & priority;
        bool released = false;
    };
    using HoldSpacePtr = std::unique_ptr<HoldSpace>;

protected:
    IFileCachePriority(size_t max_size_, size_t max_elements_);

    virtual void holdImpl(size_t /* size */, size_t /* elements */, const CachePriorityGuard::Lock &) {}

    virtual void releaseImpl(size_t /* size */, size_t /* elements */) {}

    std::atomic<size_t> max_size = 0;
    std::atomic<size_t> max_elements = 0;
};

}
