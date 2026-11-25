// Lock-order inversion regression tests.
//
// This translation unit intentionally has no main(): task_support/driver.c
// supplies main and drives the cases listed in task_cases/task_case_count.
// Cases are split into the "order", "lifecycle" and "condvar" groups.
//
// Every sample uses only legal Relacy-modeled APIs and bounded scheduling;
// none of them rely on real-thread interlocking, sleeps or host timing.

#include "relacy/relacy.hpp"

#include "task_support/check.h"

#include <sstream>
#include <string>


namespace
{

struct sim_result
{
    bool                        simulate_ok;
    rl::test_result_e           test_result;
    std::string                 output;
};

template<class suite_t>
sim_result run_sim(unsigned iterations, bool check_lock_order = true)
{
    rl::test_params params;
    params.check_lock_order = check_lock_order;
    params.iteration_count = iterations;
    std::ostringstream captured;
    params.output_stream = &captured;
    bool const ok = rl::simulate<suite_t>(params);
    sim_result res = { ok, params.test_result, captured.str() };
    return res;
}

bool has(std::string const& text, char const* needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace


// ---------------------------------------------------------------------------
// order group: dependency recording, cycles, recursion, try_lock, timeouts
// ---------------------------------------------------------------------------

namespace
{

struct order_acyclic : rl::test_suite<order_acyclic, 1>
{
    rl::mutex a_;
    rl::mutex b_;

    void thread(unsigned)
    {
        a_.lock($);
        b_.lock($);
        b_.unlock($);
        a_.unlock($);

        // A second, consistently ordered critical section.
        a_.lock($);
        b_.lock($);
        b_.unlock($);
        a_.unlock($);
    }
};

struct order_three_ring : rl::test_suite<order_three_ring, 1>
{
    rl::mutex a_;
    rl::mutex b_;
    rl::mutex c_;

    void thread(unsigned)
    {
        // Separate critical sections accumulate A -> B, B -> C, C -> A.
        a_.lock($); b_.lock($);
        b_.unlock($); a_.unlock($);

        b_.lock($); c_.lock($);
        c_.unlock($); b_.unlock($);

        c_.lock($); a_.lock($); // closes the ring
        a_.unlock($); c_.unlock($);
    }
};

struct order_cross_thread : rl::test_suite<order_cross_thread, 2>
{
    rl::mutex a_;
    rl::mutex b_;
    rl::atomic<int> done_;

    void before()
    {
        done_($) = 0;
    }

    void thread(unsigned index)
    {
        if (index == 0)
        {
            a_.lock($);
            b_.lock($);
            b_.unlock($);
            a_.unlock($);
            done_($) = 1;
        }
        else
        {
            // Runs only after thread 0 fully released its locks, so the
            // reversed order completes without any real blocking cycle.
            while (done_($) == 0)
                rl::yield(1, $);

            b_.lock($);
            a_.lock($);
            a_.unlock($);
            b_.unlock($);
        }
    }
};

struct order_recursive : rl::test_suite<order_recursive, 1>
{
    rl::mutex a_;
    rl::recursive_mutex r_;

    void thread(unsigned)
    {
        a_.lock($);
        r_.lock($);
        r_.lock($); // recursive level must not create a self loop
        r_.unlock($);
        r_.unlock($);
        a_.unlock($);

        r_.lock($);
        r_.lock($);
        r_.unlock($);
        r_.unlock($);
    }
};

struct order_non_lifo_release : rl::test_suite<order_non_lifo_release, 1>
{
    rl::mutex a_;
    rl::mutex b_;
    rl::mutex c_;

    void thread(unsigned)
    {
        a_.lock($);
        b_.lock($);
        c_.lock($);

        b_.unlock($); // middle lock released first (legal, non-LIFO)
        c_.unlock($);
        a_.unlock($);
    }
};

struct order_try_no_edge : rl::test_suite<order_try_no_edge, 1>
{
    rl::mutex a_;
    rl::mutex b_;

    bool try_lock_retry(rl::mutex& m)
    {
        for (int i = 0; i != 4; ++i)
        {
            if (m.try_lock($))
                return true;
        }
        return false;
    }

    void thread(unsigned)
    {
        // Existing dependency B -> A.
        b_.lock($);
        a_.lock($);
        a_.unlock($);
        b_.unlock($);

        // A held, successful try_lock of B must not add A -> B, otherwise
        // the pre-existing B -> A would look like a cycle.
        a_.lock($);
        bool const got = try_lock_retry(b_);
        RL_ASSERT(got);
        if (got)
            b_.unlock($);
        a_.unlock($);
    }
};

struct order_try_membership : rl::test_suite<order_try_membership, 1>
{
    rl::mutex a_;
    rl::mutex b_;

    bool try_lock_retry(rl::mutex& m)
    {
        for (int i = 0; i != 4; ++i)
        {
            if (m.try_lock($))
                return true;
        }
        return false;
    }

    void thread(unsigned)
    {
        // try-acquired A enters the held set and participates in deps.
        RL_ASSERT(try_lock_retry(a_));
        b_.lock($); // records A -> B
        b_.unlock($);
        a_.unlock($);

        b_.lock($);
        a_.lock($); // B -> A closes the cycle
        a_.unlock($);
        b_.unlock($);
    }
};

struct order_try_failure : rl::test_suite<order_try_failure, 2>
{
    rl::mutex x_;
    rl::atomic<int> ready_;
    rl::atomic<int> tried_;

    void before()
    {
        ready_($) = 0;
        tried_($) = 0;
    }

    void thread(unsigned index)
    {
        if (index == 0)
        {
            x_.lock($);
            ready_($) = 1;
            while (tried_($) == 0)
                rl::yield(1, $);
            x_.unlock($);
        }
        else
        {
            while (ready_($) == 0)
                rl::yield(1, $);
            if (x_.try_lock($))
                x_.unlock($);
            tried_($) = 1;
        }
    }
};

struct timeout_observer_t
{
    int                         observed_timeouts;
    int                         unexpected_edge_deltas;
};

static timeout_observer_t g_timeout_observer;

struct order_timed_lock : rl::test_suite<order_timed_lock, 2>
{
    rl::mutex                   held_;
    rl::atomic<int>             ready_;
    rl::rl_pthread_mutex_t      timed_;

    void before()
    {
        ready_($) = 0;
        rl::rl_pthread_mutex_init(&timed_, 0, $);
    }

    void after()
    {
        rl::rl_pthread_mutex_destroy(&timed_, $);
    }

    void thread(unsigned index)
    {
        if (index == 0)
        {
            rl::rl_pthread_mutex_lock(&timed_, $);
            ready_($) = 1;
            for (int i = 0; i != 6; ++i)
                rl::yield(1, $);
            rl::rl_pthread_mutex_unlock(&timed_, $);
        }
        else
        {
            while (ready_($) == 0)
                rl::yield(1, $);

            held_.lock($);
            std::size_t const edges_before =
                rl::ctx().lock_order().topology_edge_count();

            int const rv = rl::rl_pthread_mutex_timedlock(&timed_, 0, $);

            std::size_t const edges_after =
                rl::ctx().lock_order().topology_edge_count();

            if (rv == rl::RL_ETIMEDOUT)
            {
                ++g_timeout_observer.observed_timeouts;
                // A timed-out acquisition must produce no incoming edges.
                if (edges_after != edges_before)
                    ++g_timeout_observer.unexpected_edge_deltas;
            }
            else
            {
                rl::rl_pthread_mutex_unlock(&timed_, $);
            }
            held_.unlock($);
        }
    }
};

} // namespace


static int case_order_acyclic()
{
    sim_result r = run_sim<order_acyclic>(3);
    TASK_CHECK(r.test_result == rl::test_result_success);
    TASK_CHECK(r.simulate_ok);
    TASK_CHECK(!has(r.output, "LOCK ORDER INVERSION"));
    return 0;
}

static int case_order_three_ring()
{
    sim_result r = run_sim<order_three_ring>(1);
    TASK_CHECK(r.test_result == rl::test_result_lock_order_inversion);
    TASK_CHECK(has(r.output, "LOCK ORDER INVERSION"));
    TASK_CHECK(has(r.output, "closed lifecycle node sequence"));
    TASK_CHECK(has(r.output, "source: normal"));
    TASK_CHECK(has(r.output, "simulated thread 0"));
    TASK_CHECK(has(r.output, "lock_order_regression.cpp"));
    TASK_CHECK(std::string(rl::test_result_str(
        rl::test_result_lock_order_inversion)) == "LOCK ORDER INVERSION");
    return 0;
}

static int case_order_cross_thread()
{
    sim_result r = run_sim<order_cross_thread>(1);
    TASK_CHECK(r.test_result == rl::test_result_lock_order_inversion);
    TASK_CHECK(has(r.output, "LOCK ORDER INVERSION"));
    TASK_CHECK(has(r.output, "simulated thread 0"));
    TASK_CHECK(has(r.output, "simulated thread 1"));
    return 0;
}

static int case_order_recursive()
{
    sim_result r = run_sim<order_recursive>(2);
    TASK_CHECK(r.test_result == rl::test_result_success);
    TASK_CHECK(!has(r.output, "LOCK ORDER INVERSION"));
    return 0;
}

static int case_order_non_lifo_release()
{
    sim_result r = run_sim<order_non_lifo_release>(1);
    TASK_CHECK(r.test_result == rl::test_result_success);
    TASK_CHECK(!has(r.output, "LOCK ORDER INVERSION"));
    return 0;
}

static int case_order_try_no_edge()
{
    sim_result r = run_sim<order_try_no_edge>(4);
    TASK_CHECK(r.test_result == rl::test_result_success);
    TASK_CHECK(!has(r.output, "LOCK ORDER INVERSION"));
    return 0;
}

static int case_order_try_membership()
{
    sim_result r = run_sim<order_try_membership>(4);
    TASK_CHECK(r.test_result == rl::test_result_lock_order_inversion);
    TASK_CHECK(has(r.output, "LOCK ORDER INVERSION"));
    return 0;
}

static int case_order_try_failure()
{
    sim_result r = run_sim<order_try_failure>(8);
    TASK_CHECK(r.test_result == rl::test_result_success);
    return 0;
}

static int case_order_timed_timeout()
{
    g_timeout_observer.observed_timeouts = 0;
    g_timeout_observer.unexpected_edge_deltas = 0;

    sim_result r = run_sim<order_timed_lock>(800);
    TASK_CHECK(r.test_result == rl::test_result_success);
    TASK_CHECK(g_timeout_observer.observed_timeouts > 0);
    TASK_CHECK(g_timeout_observer.unexpected_edge_deltas == 0);
    return 0;
}


// ---------------------------------------------------------------------------
// lifecycle group: node lifetimes, address reuse, reset, repeated simulate
// ---------------------------------------------------------------------------

namespace
{

struct lifecycle_address_reuse : rl::test_suite<lifecycle_address_reuse, 1>
{
    rl::mutex                   a_;
    alignas(rl::mutex) unsigned char storage_[sizeof(rl::mutex)];

    void thread(unsigned)
    {
        typedef rl::mutex reused_mutex;
        reused_mutex* first = new (static_cast<void*>(storage_)) reused_mutex();
        a_.lock($);
        first->lock($);       // records a -> first
        first->unlock($);
        a_.unlock($);
        std::destroy_at(first); // lifecycle ends: edge retires

        // Same address, brand new lifecycle.
        reused_mutex* second = new (static_cast<void*>(storage_)) reused_mutex();
        second->lock($);
        a_.lock($);           // only second -> a, must be acyclic
        a_.unlock($);
        second->unlock($);
        std::destroy_at(second);
    }
};

} // namespace


static int case_lifecycle_address_reuse_sample()
{
    sim_result r = run_sim<lifecycle_address_reuse>(2);
    TASK_CHECK(r.test_result == rl::test_result_success);
    TASK_CHECK(!has(r.output, "LOCK ORDER INVERSION"));
    return 0;
}

static int case_lifecycle_repeated_simulate()
{
    // Repeated simulate() calls must not inherit state from each other.
    for (int i = 0; i != 3; ++i)
    {
        sim_result good = run_sim<order_acyclic>(2);
        TASK_CHECK(good.test_result == rl::test_result_success);

        sim_result bad = run_sim<order_three_ring>(1);
        TASK_CHECK(bad.test_result == rl::test_result_lock_order_inversion);
        // Node numbering restarts at lock#1 for every fresh context/iteration.
        TASK_CHECK(has(bad.output, "lock#1"));
    }
    return 0;
}

static int case_lifecycle_tracker_state_machine()
{
    std::ostringstream report;
    rl::lock_order_tracker tracker;
    tracker.configure(true, &report);

    int key_a = 0;
    int key_b = 0;
    int key_c = 0;
    void* const ka = &key_a;
    void* const kb = &key_b;
    void* const kc = &key_c;

    tracker.on_mutex_initialized(ka, $);
    tracker.on_mutex_initialized(kb, $);
    tracker.on_mutex_initialized(kc, $);
    TASK_CHECK(tracker.node_count() == 3);

    // Thread 0: A then B records exactly one edge.
    TASK_CHECK(!tracker.on_blocking_lock_acquired(ka, 0, $));
    TASK_CHECK(!tracker.on_blocking_lock_acquired(kb, 0, $));
    TASK_CHECK(tracker.topology_edge_count() == 1);

    // A recursive-style repeated acquisition of B adds no self edge.
    TASK_CHECK(!tracker.on_blocking_lock_acquired(kb, 0, $));
    TASK_CHECK(tracker.topology_edge_count() == 1);

    tracker.on_lock_released(kb, 0);

    // A still held, acquire C -> one more edge.
    TASK_CHECK(!tracker.on_blocking_lock_acquired(kc, 0, $));
    TASK_CHECK(tracker.topology_edge_count() == 2);
    tracker.on_lock_released(kc, 0);
    tracker.on_lock_released(ka, 0);

    // Destroying B retires its node and incident edge (A -> B).
    tracker.on_mutex_destroyed(kb);
    TASK_CHECK(tracker.node_count() == 2);
    std::size_t edges_after_destroy = tracker.topology_edge_count();
    TASK_CHECK(edges_after_destroy == 1); // only A -> C remains

    // Re-initializing the same key starts a fresh generation/node.
    tracker.on_mutex_initialized(kb, $);
    TASK_CHECK(tracker.node_count() == 3);

    // Build C -> A on thread 7.  Combined with the surviving A -> C edge
    // this closes a two-edge cycle.
    TASK_CHECK(!tracker.on_blocking_lock_acquired(kc, 7, $));
    bool const cycle = tracker.on_blocking_lock_acquired(ka, 7, $);
    TASK_CHECK(cycle);
    TASK_CHECK(tracker.inversion_detected());
    TASK_CHECK(has(report.str(), "LOCK ORDER INVERSION"));
    TASK_CHECK(has(report.str(), "simulated thread 7"));

    // Iteration reset drops the whole graph.
    tracker.on_iteration_begin();
    TASK_CHECK(tracker.node_count() == 0);
    TASK_CHECK(tracker.topology_edge_count() == 0);
    TASK_CHECK(!tracker.inversion_detected());

    // Disabled tracker never reports anything.
    rl::lock_order_tracker off;
    std::ostringstream off_out;
    off.configure(false, &off_out);
    off.on_mutex_initialized(ka, $);
    TASK_CHECK(!off.on_blocking_lock_acquired(ka, 0, $));
    TASK_CHECK(off.node_count() == 0);
    return 0;
}


// ---------------------------------------------------------------------------
// condvar group: wait release / re-acquire source labeling / timed wait
// ---------------------------------------------------------------------------

namespace
{

struct condvar_reacquire_cycle : rl::test_suite<condvar_reacquire_cycle, 2>
{
    rl::mutex                   outer_;
    rl::mutex                   m_;
    rl::condition_variable      cv_;
    rl::atomic<int>             signal_;
    rl::atomic<int>             ready_;
    rl::atomic<int>             done_;

    void before()
    {
        signal_($) = 0;
        ready_($) = 0;
        done_($) = 0;
    }

    void thread(unsigned index)
    {
        if (index == 1)
        {
            outer_.lock($);
            m_.lock($);
            ready_($) = 1;
            while (signal_($) == 0)
                cv_.wait(m_, $); // releases m_, keeps outer_
            m_.unlock($);
            outer_.unlock($);
            done_($) = 1;
        }
        else
        {
            while (ready_($) == 0)
                rl::yield(1, $);

            m_.lock($);
            signal_($) = 1;
            m_.unlock($);
            cv_.notify_all($);

            while (done_($) == 0)
                rl::yield(1, $);

            m_.lock($);
            outer_.lock($); // closes outer -> m cycle, normal source
            outer_.unlock($);
            m_.unlock($);
        }
    }
};

struct condvar_benign : rl::test_suite<condvar_benign, 2>
{
    rl::mutex                   m_;
    rl::condition_variable      cv_;
    rl::var<int>                ready_;

    void before()
    {
        ready_($) = 0;
    }

    void thread(unsigned index)
    {
        m_.lock($);
        if (index == 0)
        {
            ready_($) = 1;
            cv_.notify_all($);
        }
        else
        {
            while (ready_($) == 0)
                cv_.wait(m_, $);
            RL_ASSERT(ready_($) == 1);
        }
        m_.unlock($);
    }
};

struct condvar_wait_for_timeout : rl::test_suite<condvar_wait_for_timeout, 2>
{
    rl::mutex                   m_;
    rl::condition_variable      cv_;
    rl::atomic<int>             ready_;

    void before()
    {
        ready_($) = 0;
    }

    void thread(unsigned index)
    {
        m_.lock($);
        if (index == 0)
        {
            ready_($) = 1;
            cv_.notify_all($);
        }
        else
        {
            while (ready_($) == 0)
                cv_.wait_for(m_, 1, $);
            RL_ASSERT(ready_($) == 1);
        }
        m_.unlock($);
    }
};

} // namespace


static int case_condvar_reacquire_label()
{
    sim_result r = run_sim<condvar_reacquire_cycle>(1);
    TASK_CHECK(r.test_result == rl::test_result_lock_order_inversion);
    TASK_CHECK(has(r.output, "wait-reacquire"));
    TASK_CHECK(has(r.output, "source: normal"));
    TASK_CHECK(has(r.output, "LOCK ORDER INVERSION"));
    return 0;
}

static int case_condvar_benign_checked()
{
    sim_result r = run_sim<condvar_benign>(64);
    TASK_CHECK(r.test_result == rl::test_result_success);
    TASK_CHECK(!has(r.output, "LOCK ORDER INVERSION"));
    return 0;
}

static int case_condvar_wait_for_timeout()
{
    sim_result r = run_sim<condvar_wait_for_timeout>(64);
    TASK_CHECK(r.test_result == rl::test_result_success);
    return 0;
}


// ---------------------------------------------------------------------------
// task case table consumed by task_support/driver.c
// ---------------------------------------------------------------------------

const struct task_case task_cases[] =
{
    { "order",     "acyclic",              case_order_acyclic },
    { "order",     "three_ring",           case_order_three_ring },
    { "order",     "cross_thread",         case_order_cross_thread },
    { "order",     "recursive",            case_order_recursive },
    { "order",     "non_lifo_release",     case_order_non_lifo_release },
    { "order",     "try_no_edge",          case_order_try_no_edge },
    { "order",     "try_membership",       case_order_try_membership },
    { "order",     "try_failure",          case_order_try_failure },
    { "order",     "timed_timeout",        case_order_timed_timeout },

    { "lifecycle", "address_reuse_sample", case_lifecycle_address_reuse_sample },
    { "lifecycle", "repeated_simulate",    case_lifecycle_repeated_simulate },
    { "lifecycle", "tracker_state_machine", case_lifecycle_tracker_state_machine },

    { "condvar",   "reacquire_label",      case_condvar_reacquire_label },
    { "condvar",   "benign_checked",       case_condvar_benign_checked },
    { "condvar",   "wait_for_timeout",     case_condvar_wait_for_timeout },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
