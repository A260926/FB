/*  Relacy Race Detector
 *  Copyright (c) 2008-2013, Dmitry S. Vyukov
 *  All rights reserved.
 *  This software is provided AS-IS with no warranty, either express or implied.
 *  This software is distributed under a license and may not be copied,
 *  modified or distributed except as expressly authorized under the
 *  terms of the license contained in the file LICENSE in this distribution.
 */

#ifndef RL_LOCK_ORDER_HPP
#define RL_LOCK_ORDER_HPP
#ifdef _MSC_VER
#   pragma once
#endif

// Optional lock-order inversion checking.
//
// The detector is intentionally self contained: all of its state lives in
// rl::lock_order_tracker which is owned by rl::context.  Per-thread held lock
// sets are indexed by the simulated thread id, so they never follow the host
// stack of a switching fiber.  A lock lifecycle (from initialization to
// destruction) is represented by a unique node id; destroying a lock retires
// every edge incident to its node, and every simulation iteration starts with
// an empty graph.
//
// All tracker containers use rl::lock_order_raw_allocator which calls the
// system malloc/free directly.  Relacy replaces the global operator new while
// a context is active and routes it through its simulated memory manager
// (with leak checking at every iteration); detector bookkeeping must stay
// invisible to that layer.  Source locations point at compiler string
// literals, so no owned strings are needed and diagnostics never dangle after
// a lock is destroyed.

#include "base.hpp"

#include <map>
#include <memory>
#include <set>
#include <vector>


namespace rl
{


template <class T>
struct lock_order_raw_allocator
{
    typedef T               value_type;
    typedef T*              pointer;
    typedef T const*        const_pointer;
    typedef T&              reference;
    typedef T const&        const_reference;
    typedef std::size_t     size_type;
    typedef std::ptrdiff_t  difference_type;

    template <class U>
    struct rebind
    {
        typedef lock_order_raw_allocator<U> other;
    };

    lock_order_raw_allocator() noexcept
    {
    }

    lock_order_raw_allocator(lock_order_raw_allocator const&) noexcept
    {
    }

    template <class U>
    lock_order_raw_allocator(lock_order_raw_allocator<U> const&) noexcept
    {
    }

    T* allocate(std::size_t n)
    {
        void* p = (::malloc)(n * sizeof(T));
        if (0 == p)
            throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t) noexcept
    {
        (::free)(p);
    }

    template <class U>
    bool operator==(lock_order_raw_allocator<U> const&) const noexcept
    {
        return true;
    }

    template <class U>
    bool operator!=(lock_order_raw_allocator<U> const&) const noexcept
    {
        return false;
    }
};


// Origin of a recorded dependency edge.  "normal" is an ordinary blocking
// exclusive acquisition; "wait_reacquire" is the re-acquisition performed by
// a condition variable after wait() released the mutex.
enum lock_order_edge_origin_e
{
    lock_order_edge_normal,
    lock_order_edge_wait_reacquire,
};


// Source location of an acquisition.  The char const* members reference
// __FILE__ / __FUNCTION__ compiler literals with static storage duration, so
// copying this struct around (and keeping it after lock destruction) is safe.
struct lock_order_source_site
{
    char const*                 func_;
    char const*                 file_;
    unsigned                    line_;

    lock_order_source_site()
        : func_("")
        , file_("")
        , line_(0)
    {
    }

    explicit lock_order_source_site(debug_info_param info)
        : func_(info.func_ ? info.func_ : "")
        , file_(info.file_ ? info.file_ : "")
        , line_(info.line_)
    {
    }

    void print(std::ostream& s) const
    {
        s << (file_[0] ? file_ : "<unknown>") << ':' << line_;
        if (func_[0])
            s << " (" << func_ << ')';
    }
};


struct lock_order_edge
{
    unsigned                    from_node_;
    unsigned                    to_node_;
    thread_id_t                 thread_id_;
    lock_order_source_site      from_site_;
    lock_order_source_site      to_site_;
    lock_order_edge_origin_e    origin_;

    lock_order_edge()
        : from_node_(0)
        , to_node_(0)
        , thread_id_(0)
        , origin_(lock_order_edge_normal)
    {
    }
};


class lock_order_tracker
{
public:
    lock_order_tracker()
        : enabled_(false)
        , output_(0)
        , next_node_id_(1)
        , inversion_detected_(false)
    {
    }

    void configure(bool enabled, std::ostream* output)
    {
        enabled_ = enabled;
        output_ = output;
    }

    bool enabled() const
    {
        return enabled_;
    }

    bool inversion_detected() const
    {
        return inversion_detected_;
    }

    // Number of live lock lifecycle nodes (introspection for tests).
    std::size_t node_count() const
    {
        return nodes_.size();
    }

    // Number of distinct topology dependency edges (introspection for tests).
    std::size_t topology_edge_count() const
    {
        std::size_t count = 0;
        for (adjacency_t::const_iterator it = adjacency_.begin(); it != adjacency_.end(); ++it)
            count += it->second.size();
        return count;
    }

    // Called by the context at the beginning of every simulation iteration.
    // No graph state may survive between iterations.
    void on_iteration_begin()
    {
        nodes_.clear();
        key_to_node_.clear();
        adjacency_.clear();
        observations_.clear();
        held_.clear();
        wait_reacquire_pending_.clear();
        next_node_id_ = 1;
        inversion_detected_ = false;
    }

    // Lock lifecycle: a new independent lifecycle starts at initialization.
    void on_mutex_initialized(void const* key, debug_info_param info)
    {
        if (false == enabled_)
            return;
        if (0 == key)
            return;
        if (key_to_node_.end() != key_to_node_.find(key))
            return; // defensive: double registration keeps the first node

        unsigned const id = next_node_id_++;
        node_t node;
        node.id_ = id;
        node.key_ = key;
        node.init_site_ = lock_order_source_site(info);
        nodes_[id] = node;
        key_to_node_[key] = id;
    }

    // Destruction ends the lifecycle: the node and all incident edges retire.
    void on_mutex_destroyed(void const* key)
    {
        if (false == enabled_)
            return;
        if (0 == key)
            return;

        key_map_t::iterator kit = key_to_node_.find(key);
        if (kit == key_to_node_.end())
            return;

        unsigned const id = kit->second;

        // outgoing edges
        adjacency_.erase(id);
        observations_.erase(id);

        // incoming edges
        for (adjacency_t::iterator ait = adjacency_.begin(); ait != adjacency_.end(); ++ait)
        {
            edge_list& edges = ait->second;
            edges.erase(std::remove_if(edges.begin(), edges.end(), edge_points_to(id)),
                        edges.end());
        }
        for (observation_map_t::iterator oit = observations_.begin();
             oit != observations_.end(); ++oit)
        {
            oit->second.erase(id);
        }

        // stale held-set entries (normally empty for legal destruction)
        for (held_map_t::iterator hit = held_.begin(); hit != held_.end(); ++hit)
        {
            held_list& locks = hit->second;
            locks.erase(std::remove_if(locks.begin(), locks.end(), held_is(id)),
                        locks.end());
        }

        nodes_.erase(id);
        key_to_node_.erase(kit);
    }

    // A successful try_lock enters the held set but must not produce any
    // incoming dependency edges, regardless of whether it succeeded or
    // failed (failures never reach this hook).
    void on_try_lock_acquired(void const* key,
                             thread_id_t tid,
                             debug_info_param info)
    {
        if (false == enabled_ || inversion_detected_)
            return;

        unsigned id = 0;
        if (false == resolve_node(key, id))
            return;

        add_held(tid, id, info);
    }

    // A successful blocking exclusive acquisition.  Dependencies from every
    // other lock still held by the simulated thread are recorded.  Returns
    // true when the new dependency closes a lock-order cycle; when
    // emit_diagnostic is true the diagnostic is written to the configured
    // output stream before returning.
    bool on_blocking_lock_acquired(void const* key,
                                   thread_id_t tid,
                                   debug_info_param info,
                                   bool emit_diagnostic = true)
    {
        if (false == enabled_ || inversion_detected_)
            return inversion_detected_;

        unsigned id = 0;
        if (false == resolve_node(key, id))
            return false;

        lock_order_edge_origin_e origin = consume_wait_reacquire(tid);

        held_list current;
        held_map_t::iterator hit = held_.find(tid);
        if (hit != held_.end())
            current = hit->second;

        for (held_list::const_iterator h = current.begin(); h != current.end(); ++h)
        {
            if (h->node_id_ == id)
                continue; // recursive acquisition never creates a self loop

            lock_order_edge candidate;
            candidate.from_node_ = h->node_id_;
            candidate.to_node_ = id;
            candidate.thread_id_ = tid;
            candidate.from_site_ = h->acquire_site_;
            candidate.to_site_ = lock_order_source_site(info);
            candidate.origin_ = origin;

            edge_list path;
            if (closes_cycle(candidate, path))
            {
                inversion_detected_ = true;
                if (emit_diagnostic)
                    emit_report(candidate, path);
                return true;
            }

            record_observation(candidate);
        }

        add_held(tid, id, info);
        return false;
    }

    // Final (outermost) unlock removes the lock from the simulated thread's
    // held set.  Removal is keyed by lock id so non-LIFO legal release works.
    void on_lock_released(void const* key, thread_id_t tid)
    {
        if (false == enabled_)
            return;

        unsigned id = 0;
        if (false == resolve_node(key, id))
            return;

        held_map_t::iterator hit = held_.find(tid);
        if (hit == held_.end())
            return;

        held_list& locks = hit->second;
        locks.erase(std::remove_if(locks.begin(), locks.end(), held_is(id)),
                    locks.end());
    }

    // Marks the next blocking acquisition performed by the simulated thread
    // as a condition-variable wait re-acquisition.
    void set_wait_reacquire_pending(thread_id_t tid, bool value)
    {
        if (false == enabled_)
            return;
        wait_reacquire_pending_[tid] = value;
    }

private:
    struct node_t
    {
        unsigned                    id_;
        void const*                 key_;
        lock_order_source_site      init_site_;
    };

    struct held_lock_t
    {
        unsigned                    node_id_;
        lock_order_source_site      acquire_site_;
    };

    struct stored_edge_t
    {
        lock_order_edge             edge_;
        bool                        seen_normal_;
        bool                        seen_wait_reacquire_;

        stored_edge_t()
            : seen_normal_(false)
            , seen_wait_reacquire_(false)
        {
        }
    };

    typedef lock_order_raw_allocator<lock_order_edge> edge_allocator_t;
    typedef std::vector<lock_order_edge, edge_allocator_t> edge_list;

    typedef std::map<unsigned, edge_list, std::less<unsigned>,
                     lock_order_raw_allocator<std::pair<unsigned const, edge_list> > >
        adjacency_t;

    typedef lock_order_raw_allocator<held_lock_t> held_allocator_t;
    typedef std::vector<held_lock_t, held_allocator_t> held_list;

    typedef std::map<thread_id_t, held_list, std::less<thread_id_t>,
                     lock_order_raw_allocator<std::pair<thread_id_t const, held_list> > >
        held_map_t;

    typedef std::map<void const*, unsigned, std::less<void const*>,
                     lock_order_raw_allocator<std::pair<void const* const, unsigned> > >
        key_map_t;

    typedef std::map<unsigned, node_t, std::less<unsigned>,
                     lock_order_raw_allocator<std::pair<unsigned const, node_t> > >
        node_map_t;

    typedef std::map<unsigned, stored_edge_t, std::less<unsigned>,
                     lock_order_raw_allocator<std::pair<unsigned const, stored_edge_t> > >
        observation_inner_t;

    typedef std::map<unsigned, observation_inner_t, std::less<unsigned>,
                     lock_order_raw_allocator<std::pair<unsigned const, observation_inner_t> > >
        observation_map_t;

    typedef std::map<thread_id_t, bool, std::less<thread_id_t>,
                     lock_order_raw_allocator<std::pair<thread_id_t const, bool> > >
        reacquire_map_t;

    struct edge_points_to
    {
        unsigned id_;
        explicit edge_points_to(unsigned id) : id_(id) {}
        bool operator()(lock_order_edge const& e) const { return e.to_node_ == id_; }
    };

    struct held_is
    {
        unsigned id_;
        explicit held_is(unsigned id) : id_(id) {}
        bool operator()(held_lock_t const& h) const { return h.node_id_ == id_; }
    };

    bool                            enabled_;
    std::ostream*                   output_;
    unsigned                        next_node_id_;
    bool                            inversion_detected_;

    node_map_t                      nodes_;
    key_map_t                       key_to_node_;
    adjacency_t                     adjacency_;
    observation_map_t               observations_;
    held_map_t                      held_;
    reacquire_map_t                 wait_reacquire_pending_;

    bool resolve_node(void const* key, unsigned& id) const
    {
        if (0 == key)
            return false;
        key_map_t::const_iterator it = key_to_node_.find(key);
        if (it == key_to_node_.end())
            return false;
        id = it->second;
        return true;
    }

    lock_order_edge_origin_e consume_wait_reacquire(thread_id_t tid)
    {
        reacquire_map_t::iterator it = wait_reacquire_pending_.find(tid);
        bool pending = (it != wait_reacquire_pending_.end()) && it->second;
        if (it != wait_reacquire_pending_.end())
            it->second = false;
        return pending ? lock_order_edge_wait_reacquire : lock_order_edge_normal;
    }

    void add_held(thread_id_t tid, unsigned id, debug_info_param info)
    {
        held_list& locks = held_[tid];
        for (typename held_list::iterator it = locks.begin(); it != locks.end(); ++it)
        {
            if (it->node_id_ == id)
                return; // recursive/duplicate: keep outermost acquisition site
        }
        held_lock_t held;
        held.node_id_ = id;
        held.acquire_site_ = lock_order_source_site(info);
        locks.push_back(held);
    }

    // Stores an observation.  Topology keeps a single directed edge per pair,
    // but the stored record remembers both source kinds observed for the pair
    // so diagnostics can distinguish normal vs wait-reacquire dependencies
    // even when the same pair was observed both ways.
    void record_observation(lock_order_edge const& e)
    {
        // topology (used for cycle detection)
        edge_list& edges = adjacency_[e.from_node_];
        bool topology_exists = false;
        for (typename edge_list::const_iterator it = edges.begin(); it != edges.end(); ++it)
        {
            if (it->to_node_ == e.to_node_)
            {
                topology_exists = true;
                break;
            }
        }
        if (false == topology_exists)
            edges.push_back(e);

        // rich observation record
        stored_edge_t& stored = observations_[e.from_node_][e.to_node_];
        if (lock_order_edge_wait_reacquire == e.origin_)
        {
            stored.seen_wait_reacquire_ = true;
            // Prefer the wait-reacquire observation as the displayed edge so
            // the special origin is visible in the cycle diagnostic even when
            // the same pair was first observed by a normal acquisition.
            stored.edge_ = e;
        }
        else
        {
            stored.seen_normal_ = true;
            if (false == stored.seen_wait_reacquire_)
                stored.edge_ = e;
        }
    }

    // DFS from src to dst over the current topology.  On success path contains
    // the edges from src up to (and including) the edge reaching dst.
    bool find_path(unsigned src, unsigned dst, edge_list& path) const
    {
        if (src == dst)
            return true;

        typedef std::set<unsigned, std::less<unsigned>,
                         lock_order_raw_allocator<unsigned> > visited_set_t;
        visited_set_t visited;
        visited.insert(src);

        struct frame
        {
            unsigned                node_;
            std::size_t             next_;
        };
        typedef std::vector<frame, lock_order_raw_allocator<frame> > stack_t;
        stack_t stack;
        frame first;
        first.node_ = src;
        first.next_ = 0;
        stack.push_back(first);

        edge_list edge_path;

        while (false == stack.empty())
        {
            unsigned const node = stack.back().node_;
            std::size_t idx = stack.back().next_;

            adjacency_t::const_iterator ait = adjacency_.find(node);
            if (ait == adjacency_.end() || idx >= ait->second.size())
            {
                stack.pop_back();
                if (false == edge_path.empty())
                    edge_path.pop_back();
                continue;
            }

            lock_order_edge const e = ait->second[idx];
            stack.back().next_ = idx + 1;

            if (e.to_node_ == dst)
            {
                edge_path.push_back(e);
                path = edge_path;
                return true;
            }

            if (visited.insert(e.to_node_).second)
            {
                edge_path.push_back(e);
                frame f;
                f.node_ = e.to_node_;
                f.next_ = 0;
                stack.push_back(f);
            }
        }
        return false;
    }

    // A candidate u->v closes a cycle iff v can already reach u, or u == v.
    bool closes_cycle(lock_order_edge const& candidate, edge_list& path) const
    {
        if (candidate.from_node_ == candidate.to_node_)
        {
            path.clear();
            return true;
        }
        return find_path(candidate.to_node_, candidate.from_node_, path);
    }

    void print_node_label(std::ostream& out, unsigned id) const
    {
        out << "lock#" << id;
        node_map_t::const_iterator it = nodes_.find(id);
        if (it != nodes_.end() && it->second.key_ != 0)
            out << '@' << it->second.key_; // ostream renders pointers with 0x
    }

    // Enriches a topology hop with its stored observation metadata.
    lock_order_edge const& resolve_report_edge(lock_order_edge const& topology_edge,
                                               bool& mixed_sources) const
    {
        mixed_sources = false;
        observation_map_t::const_iterator i = observations_.find(topology_edge.from_node_);
        if (i == observations_.end())
            return topology_edge;
        observation_inner_t::const_iterator j = i->second.find(topology_edge.to_node_);
        if (j == i->second.end())
            return topology_edge;

        stored_edge_t const& stored = j->second;
        mixed_sources = stored.seen_normal_ && stored.seen_wait_reacquire_;
        if (stored.edge_.from_node_ != 0)
            return stored.edge_;
        return topology_edge;
    }

    void emit_report(lock_order_edge const& closing, edge_list const& path) const
    {
        if (0 == output_)
            return;
        std::ostream& out = *output_;

        edge_list cycle;
        cycle.push_back(closing);
        for (std::size_t i = 0; i != path.size(); ++i)
            cycle.push_back(path[i]);

        out << "LOCK ORDER INVERSION\n";
        out << "potential lock-order cycle detected "
               "(successful blocking exclusive acquisition while other locks held)\n";

        // Closed lifecycle node sequence.
        out << "closed lifecycle node sequence: ";
        if (false == cycle.empty())
        {
            print_node_label(out, cycle[0].from_node_);
            for (std::size_t i = 0; i != cycle.size(); ++i)
            {
                out << " -> ";
                print_node_label(out, cycle[i].to_node_);
            }
        }
        out << '\n';

        out << "cycle edges (" << cycle.size() << "):\n";
        for (std::size_t i = 0; i != cycle.size(); ++i)
        {
            lock_order_edge const& topo = cycle[i];

            // The closing edge carries its own fresh observation; older hops
            // are enriched from the observation store.  The returned
            // reference points either at topo or at stable map storage.
            bool mixed_sources = false;
            lock_order_edge const* shown = &topo;
            if (0 != i)
                shown = &resolve_report_edge(topo, mixed_sources);

            char const* source =
                (lock_order_edge_wait_reacquire == shown->origin_)
                    ? "wait-reacquire"
                    : "normal";

            out << "  edge " << (i + 1)
                << ": simulated thread " << shown->thread_id_
                << " [thread=" << shown->thread_id_
                << " tid=" << shown->thread_id_ << "]\n";
            out << "      held lock   ";
            print_node_label(out, shown->from_node_);
            out << " acquired at ";
            shown->from_site_.print(out);
            out << '\n';
            out << "      target lock ";
            print_node_label(out, shown->to_node_);
            out << " acquired at ";
            shown->to_site_.print(out);
            out << '\n';
            out << "      source: " << source;
            if (mixed_sources)
            {
                out << " (same dependency also observed with source: "
                    << (lock_order_edge_wait_reacquire == shown->origin_
                            ? "normal"
                            : "wait-reacquire")
                    << ')';
            }
            out << '\n';
        }
        out << std::flush;
    }
};


}

#endif
