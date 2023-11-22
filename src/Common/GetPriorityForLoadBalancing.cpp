#include <Common/Exception.h>
#include <Common/GetPriorityForLoadBalancing.h>
#include <Common/Priority.h>
#include <Core/SettingsEnums.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

std::function<Priority(size_t index)> GetPriorityForLoadBalancing::getPriorityFunc(LoadBalancing load_balance, size_t offset, size_t pool_size) const
{
    std::function<Priority(size_t index)> get_priority;
    switch (load_balance)
    {
        case LoadBalancing::NEAREST_HOSTNAME:
            if (hostname_prefix_distance.empty())
                throw Exception(ErrorCodes::LOGICAL_ERROR, "It's a bug: hostname_prefix_distance is not initialized");
            get_priority = [this](size_t i) { return Priority{static_cast<Int64>(hostname_prefix_distance[i])}; };
            break;
        case LoadBalancing::HOSTNAME_LEVENSHTEIN_DISTANCE:
            if (hostname_levenshtein_distance.empty())
                throw Exception(ErrorCodes::LOGICAL_ERROR, "It's a bug: hostname_levenshtein_distance is not initialized");
            get_priority = [this](size_t i) { return Priority{static_cast<Int64>(hostname_levenshtein_distance[i])}; };
            break;
        case LoadBalancing::IN_ORDER:
            get_priority = [](size_t i) { return Priority{static_cast<Int64>(i)}; };
            break;
        case LoadBalancing::RANDOM:
            break;
        case LoadBalancing::FIRST_OR_RANDOM:
            get_priority = [offset](size_t i) { return i != offset ? Priority{1} : Priority{0}; };
            break;
        case LoadBalancing::ROUND_ROBIN:
            if (last_used >= pool_size)
                last_used = 0;
            ++last_used;
            /* Consider pool_size equals to 5
             * last_used = 1 -> get_priority: 0 1 2 3 4
             * last_used = 2 -> get_priority: 4 0 1 2 3
             * last_used = 3 -> get_priority: 4 3 0 1 2
             * ...
             * */
            get_priority = [this, pool_size](size_t i)
            {
                ++i; // To make `i` indexing start with 1 instead of 0 as `last_used` does
                return Priority{static_cast<Int64>(i < last_used ? pool_size - i : i - last_used)};
            };
            break;
        case LoadBalancing::KEEPER_LOCAL_AVAILABILITY_ZONE:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "KEEPER_LOCAL_AVAILABILITY_ZONE has its own logic for determining priority");
    }
    return get_priority;
}

}
