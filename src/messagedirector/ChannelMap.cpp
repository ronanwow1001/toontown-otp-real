#include "ChannelMap.h"

typedef boost::icl::discrete_interval<channel_t> interval_t;

static void get_closed_bounds(const interval_t &interval, channel_t &lower, channel_t &upper)
{
    lower = interval.lower();
    upper = interval.upper();

    if(!(interval.bounds().bits() & 2)) {
        lower += 1;
    }
    if(!(interval.bounds().bits() & 1)) {
        upper -= 1;
    }
}

ChannelMap::ChannelMap()
{
    // Initialize m_range_susbcriptions with empty range
    auto empty_set = std::unordered_set<ChannelSubscriber*>();
    m_range_subscriptions = boost::icl::interval_map<channel_t, std::unordered_set<ChannelSubscriber*> >();
    m_range_subscriptions += std::make_pair(interval_t::closed(0, CHANNEL_MAX), empty_set);
}

void ChannelMap::subscribe_channel(ChannelSubscriber *p, channel_t c)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);

    if(is_subscribed(p, c)) {
        return;
    }

    p->channels().insert(p->channels().end(), c);
    bool has_subs = (m_channel_subscriptions.find(c) != m_channel_subscriptions.end());

    if(!has_subs) {
        on_add_channel(c);
    }

    m_channel_subscriptions.insert(std::make_pair(c, p));
}

bool ChannelMap::remove_subscriber(ChannelSubscriber *p, channel_t c)
{
    auto sub_cnt = m_channel_subscriptions.count(c);
    if(sub_cnt == 0) {
        return false;
    }

    auto subs = m_channel_subscriptions.equal_range(c);

    for(auto it = subs.first; it != subs.second; ++it) {
        if(it->second == p) {
            m_channel_subscriptions.erase(it);
            --sub_cnt;
            break;
        }
    }

    return (sub_cnt == 0);
}

void ChannelMap::unsubscribe_channel(ChannelSubscriber *p, channel_t c)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);

    if(!is_subscribed(p, c)) {
        return;
    }

    p->channels().erase(c);

    if(remove_subscriber(p, c)) {
        on_remove_channel(c);
    }
}

void ChannelMap::subscribe_range(ChannelSubscriber *p, channel_t lo, channel_t hi)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);

    // Prepare participant and range
    std::unordered_set<ChannelSubscriber *> participant_set;
    participant_set.insert(p);

    interval_t interval = interval_t::closed(lo, hi);

    // Update range mappings
    p->ranges() += interval;
    m_range_subscriptions += std::make_pair(interval, participant_set);

    // Now, check if anything along this interval is *new*:
    auto interval_range = m_range_subscriptions.equal_range(interval);
    for(auto it = interval_range.first; it != interval_range.second; ++it) {
        if(it->second.size() == 1) {
            // There's a segment of the interval that has only one element
            // (our newly added participant!) and thus, we should upstream the
            // range addition.
            on_add_range(lo, hi);
            break;
        }
    }
}

void ChannelMap::unsubscribe_range(ChannelSubscriber *p, channel_t lo, channel_t hi)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);

    // Pre-check: if there are no ranges subscribed anyway, no use doing this:
    if(m_range_subscriptions.empty()) {
        return;
    }

    // Prepare participant set
    std::unordered_set<ChannelSubscriber *> participant_set;
    participant_set.insert(p);

    // Construct the interval we are removing, bounded to m_range_subscriptions.
    channel_t lower = m_range_subscriptions.begin()->first.lower();
    channel_t upper = m_range_subscriptions.rbegin()->first.upper();
    interval_t interval = interval_t::closed(std::max(lo, lower), std::min(hi, upper));

    // Calculate the ranges that will "go silent" as a result of our removal:
    auto silent_ranges = boost::icl::interval_set<channel_t>(interval);
    auto interval_range = m_range_subscriptions.equal_range(interval);
    for(auto it = interval_range.first; it != interval_range.second; ++it) {
        if(!it->second.empty() && !(it->second.size() == 1 && *it->second.begin() == p)) {
            // We aren't the last subscription in this range, don't kill it.
            silent_ranges -= it->first;
        }
    }

    // Update range mappings
    p->ranges() -= interval;
    m_range_subscriptions -= std::make_pair(interval, participant_set);

    // Clobber *channel* subscriptions that fall within the range.
    for(auto it = p->channels().begin(); it != p->channels().end();) {
        auto prev = it++;
        channel_t c = *prev;

        if(lo <= c && c <= hi) {
            // N.B. we do NOT call unsubscribe_channel, because that might send
            // off an on_remove_channel event. Instead, we just manually update:
            remove_subscriber(p, c);
            p->channels().erase(prev);
        }
    }

    // Now, clean up any ranges that are now *empty* and should thus be killed:
    for(auto it = silent_ranges.begin(); it != silent_ranges.end(); ++it) {
        get_closed_bounds(*it, lower, upper);

        // Okay, this part of the interval is dead, better request it be
        // sliced off:
        on_remove_range(lower, upper);
    }
}

void ChannelMap::unsubscribe_all(ChannelSubscriber* p)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);

    // Unsubscribe from indivually subscribed channels
    auto channels = std::unordered_set<channel_t>(p->channels());
    for(auto it = channels.begin(); it != channels.end(); ++it) {
        channel_t channel = *it;
        unsubscribe_channel(p, channel);
    }

    // Unsubscribe from subscribed channel ranges
    auto ranges = boost::icl::interval_set<channel_t>(p->ranges());
    for(auto it = ranges.begin(); it != ranges.end(); ++it) {
        channel_t lower;
        channel_t upper;
        get_closed_bounds(*it, lower, upper);
        unsubscribe_range(p, lower, upper);
    }
}

bool ChannelMap::is_subscribed(ChannelSubscriber *p, channel_t c)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);

    if(p->channels().find(c) != p->channels().end()) {
        return true;
    }

    if(p->ranges().find(c) != p->ranges().end()) {
        return true;
    }

    return false;
}

void ChannelMap::lookup_channels(const std::vector<channel_t> &cl, std::unordered_set<ChannelSubscriber *> &ps)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);

    for(auto it = cl.begin(); it != cl.end(); ++it) {
        auto subs = m_channel_subscriptions.equal_range(*it);
        for(auto it2 = subs.first; it2 != subs.second; ++it2) {
            ps.insert(it2->second);
        }

        auto range = boost::icl::find(m_range_subscriptions, *it);
        if(range != m_range_subscriptions.end()) {
            ps.insert(range->second.begin(), range->second.end());
        }
    }
}
