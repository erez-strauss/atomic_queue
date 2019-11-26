/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; tab-width: 4 -*- */

// Copyright (c) 2019 Maxim Egorushkin. MIT License. See the full licence in file LICENSE.

#include "atomic_queue.h"
#include "atomic_queue_mutex.h"
#include "barrier.h"
#include "cpu_base_frequency.h"
#include "huge_pages.h"
#include "moodycamel.h"

#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <tbb/concurrent_queue.h>
#include <tbb/spin_mutex.h>

#include "mpmc_queue.h" // <es/lockfree/mpmc_queue.h>

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

using std::uint64_t;

using namespace ::atomic_queue;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace {

template<class T>
using Type = std::common_type<T>; // Similar to boost::type<>.

using sum_t = long long;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

double const TSC_TO_SECONDS = 1e-9 / cpu_base_frequency();

template<class T>
inline double to_seconds(T tsc) {
    return tsc * TSC_TO_SECONDS;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class Queue>
struct ESLockfreeMPMCQueueAdapter : Queue {
    using T = typename Queue::value_type;

    void push(T element) {
        while(!this->Queue::push(element))
            spin_loop_pause();
    }

    T pop() {
        T element;
        while(!this->Queue::pop(element))
            spin_loop_pause();
        return element;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class Queue>
struct BoostSpScAdapter : Queue {
    using T = typename Queue::value_type;

    void push(T element) {
        while(!this->Queue::push(element))
            spin_loop_pause();
    }

    T pop() {
        T element;
        while(!this->Queue::pop(element))
            spin_loop_pause();
        return element;
    }
};

template<class Queue>
struct BoostQueueAdapter : BoostSpScAdapter<Queue> {
    using T = typename Queue::value_type;

    void push(T element) {
        while(!this->Queue::bounded_push(element))
            spin_loop_pause();
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class Queue, size_t Capacity>
struct TbbAdapter : RetryDecorator<Queue> {
    TbbAdapter() {
        this->set_capacity(Capacity);
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class Queue, size_t Capacity>
struct CapacityToConstructor : Queue {
    CapacityToConstructor()
        : Queue(Capacity) {}
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

using Allocator = HugePageAllocator<unsigned>;
using BoostAllocator = boost::lockfree::allocator<Allocator>;

void check_huge_pages_leaks(char const* name, HugePages& hp) {
    if(!hp.empty()) {
        std::fprintf(stderr, "%s: %zu bytes of HugePages memory leaked.\n", name, hp.used());
        hp.reset();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// According to my benchmarking, it looks like the best performance is achieved with the following parameters:
// * For SPSC: SPSC=true,  MINIMIZE_CONTENTION=false, MAXIMIZE_THROUGHPUT=false.
// * For MPMC: SPSC=false, MINIMIZE_CONTENTION=true,  MAXIMIZE_THROUGHPUT=true.
// However, I am not sure that conflating these 3 parameters into 1 would be the right thing for every scenario.
template<unsigned SIZE, bool SPSC, bool MINIMIZE_CONTENTION, bool MAXIMIZE_THROUGHPUT>
struct QueueTypes {
    using T = unsigned;

    // For atomic elements only.
    using AtomicQueue =                                Type<RetryDecorator<atomic_queue::AtomicQueue<T, SIZE, T{}, MINIMIZE_CONTENTION, MAXIMIZE_THROUGHPUT, false, SPSC>>>;
    using OptimistAtomicQueue =                                       Type<atomic_queue::AtomicQueue<T, SIZE, T{}, MINIMIZE_CONTENTION, MAXIMIZE_THROUGHPUT, false, SPSC>>;
    using AtomicQueueB =        Type<RetryDecorator<CapacityToConstructor<atomic_queue::AtomicQueueB<T, Allocator, T{}, MAXIMIZE_THROUGHPUT, false, SPSC>, SIZE>>>;
    using OptimistAtomicQueueB =               Type<CapacityToConstructor<atomic_queue::AtomicQueueB<T, Allocator, T{}, MAXIMIZE_THROUGHPUT, false, SPSC>, SIZE>>;

    // For non-atomic elements.
    using AtomicQueue2 =                         Type<RetryDecorator<atomic_queue::AtomicQueue2<T, SIZE, MINIMIZE_CONTENTION, MAXIMIZE_THROUGHPUT, false, SPSC>>>;
    using OptimistAtomicQueue2 =                                Type<atomic_queue::AtomicQueue2<T, SIZE, MINIMIZE_CONTENTION, MAXIMIZE_THROUGHPUT, false, SPSC>>;
    using AtomicQueueB2 = Type<RetryDecorator<CapacityToConstructor<atomic_queue::AtomicQueueB2<T, Allocator, MAXIMIZE_THROUGHPUT, false, SPSC>, SIZE>>>;
    using OptimistAtomicQueueB2 =        Type<CapacityToConstructor<atomic_queue::AtomicQueueB2<T, Allocator, MAXIMIZE_THROUGHPUT, false, SPSC>, SIZE>>;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class Queue>
void throughput_producer(unsigned N, Queue* queue, std::atomic<uint64_t>* t0, Barrier* barrier) {
    barrier->wait();

    // The first producer saves the start time.
    uint64_t expected = 0;
    t0->compare_exchange_strong(expected, __builtin_ia32_rdtsc(), std::memory_order_acq_rel, std::memory_order_relaxed);

    for(unsigned n = 1, stop = N + 1; n <= stop; ++n)
        queue->push(n);
}

template<class Queue>
void throughput_consumer_impl(unsigned N, Queue* queue, sum_t* consumer_sum, std::atomic<unsigned>* last_consumer, uint64_t* t1) {
    unsigned const stop = N + 1;
    sum_t sum = 0;

    for(;;) {
        unsigned n = queue->pop();
        if(n == stop)
            break;
        sum += n;
    }

    // The last consumer saves the end time.
    auto t = __builtin_ia32_rdtsc();
    if(1 == last_consumer->fetch_sub(1, std::memory_order_acq_rel))
        *t1 = t;

    *consumer_sum = sum;
}

template<class Queue>
void throughput_consumer(unsigned N, Queue* queue, sum_t* consumer_sum, std::atomic<unsigned>* last_consumer, uint64_t* t1, Barrier* barrier) {
    barrier->wait();
    throughput_consumer_impl(N, queue, consumer_sum, last_consumer, t1);
}

template<class Queue>
uint64_t benchmark_throughput(HugePages& hp, std::vector<unsigned> const& hw_thread_ids, unsigned N, unsigned thread_count, bool alternative_placement, sum_t* consumer_sums) {
    set_thread_affinity(hw_thread_ids[thread_count * 2 - 1]); // Use this thread for the last consumer.
    unsigned cpu_idx = 0;

    auto queue = hp.create_unique_ptr<Queue>();
    std::atomic<uint64_t> t0{0};
    uint64_t t1 = 0;
    std::atomic<unsigned> last_consumer{thread_count};

    Barrier barrier;
    std::vector<std::thread> threads(thread_count * 2 - 1);
    if(alternative_placement) {
        for(unsigned i = 0; i < thread_count; ++i) {
            set_default_thread_affinity(hw_thread_ids[cpu_idx++]);
            threads[i] = std::thread(throughput_producer<Queue>, N, queue.get(), &t0, &barrier);
            if(i != thread_count - 1) { // This thread is the last consumer.
                set_default_thread_affinity(hw_thread_ids[cpu_idx++]);
                threads[thread_count + i] = std::thread(throughput_consumer<Queue>, N, queue.get(), consumer_sums + i, &last_consumer, &t1, &barrier);
            }
        }
    } else {
        for(unsigned i = 0; i < thread_count; ++i) {
            set_default_thread_affinity(hw_thread_ids[cpu_idx++]);
            threads[i] = std::thread(throughput_producer<Queue>, N, queue.get(), &t0, &barrier);
        }
        for(unsigned i = 0; i < thread_count - 1; ++i) { // This thread is the last consumer.
            set_default_thread_affinity(hw_thread_ids[cpu_idx++]);
            threads[thread_count + i] = std::thread(throughput_consumer<Queue>, N, queue.get(), consumer_sums + i, &last_consumer, &t1, &barrier);
        }
    }

    barrier.release(thread_count * 2 - 1);
    throughput_consumer_impl(N, queue.get(), consumer_sums + (thread_count - 1), &last_consumer, &t1); // Use this thread for the last consumer.

    for(auto& t : threads)
        t.join();

    reset_thread_affinity();

    return t1 - t0.load(std::memory_order_relaxed);
}

template<class Queue>
void run_throughput_benchmark(char const* name, HugePages& hp, std::vector<unsigned> const& hw_thread_ids, unsigned M, unsigned thread_count_min,
                              unsigned thread_count_max) {
    int constexpr RUNS = 3;
    std::vector<sum_t> consumer_sums(thread_count_max);

    for(unsigned threads = thread_count_min; threads <= thread_count_max; ++threads) {
        unsigned const N = M / threads;
        for(bool alternative_placement : {false, true}) {

            sum_t const expected_sum = (N + 1) / 2. * N;
            double const expected_sum_inv = 1. / expected_sum;

            uint64_t min_time = std::numeric_limits<uint64_t>::max();
            for(unsigned run = RUNS; run--;) {
                uint64_t time = benchmark_throughput<Queue>(hp, hw_thread_ids, N, threads, alternative_placement, consumer_sums.data());
                min_time = std::min(min_time, time);

                check_huge_pages_leaks(name, hp);

                // Calculate the checksum.
                sum_t total_sum = 0;
                for(unsigned i = 0; i < threads; ++i) {
                    auto consumer_sum = consumer_sums[i];
                    total_sum += consumer_sum;

                    // Verify that no consumer was starved.
                    auto consumer_sum_frac = consumer_sum * expected_sum_inv;
                    if(consumer_sum_frac < .1)
                        std::fprintf(stderr, "%s: producers: %u: consumer %u received too few messages: %.2lf%% of expected.\n", name, threads, i,
                                     consumer_sum_frac);
                }

                // Verify that all messages were received exactly once: no duplicates, no omissions.
                if(auto diff = total_sum - expected_sum * threads)
                    std::fprintf(stderr, "%s: wrong checksum error: producers: %u, expected_sum: %'lld, diff: %'lld.\n", name, threads, expected_sum * threads,
                                 diff);
            }

            double min_seconds = to_seconds(min_time);
            unsigned msg_per_sec = N * threads / min_seconds;
            std::printf("%32s,%2u,%c: %'11u msg/sec\n", name, threads, alternative_placement ? 'i' : 's', msg_per_sec);
        }
    }
}

template<class Queue>
void run_throughput_mpmc_benchmark(char const* name, HugePages& hp, std::vector<unsigned> const& hw_thread_ids, Type<Queue>, unsigned thread_count_min = 1) {
    unsigned const thread_count_max = hw_thread_ids.size() / 2;
    run_throughput_benchmark<Queue>(name, hp, hw_thread_ids, 1000000, thread_count_min, thread_count_max);
}

template<class... Args>
void run_throughput_spsc_benchmark(char const* name, HugePages& hp, std::vector<unsigned> const& hw_thread_ids,
                                   Type<BoostSpScAdapter<boost::lockfree::spsc_queue<Args...>>>) {
    using Queue = BoostSpScAdapter<boost::lockfree::spsc_queue<Args...>>;
    run_throughput_benchmark<Queue>(name, hp, hw_thread_ids, 1000000, 1, 1); // spsc_queue can only handle 1 producer and 1 consumer.
}

template<class Queue>
void run_throughput_spsc_benchmark(char const* name, HugePages& hp, std::vector<unsigned> const& hw_thread_ids, Type<Queue>) {
    run_throughput_benchmark<Queue>(name, hp, hw_thread_ids, 1000000, 1, 1); // Special case for 1 producer and 1 consumer.
}

void run_throughput_benchmarks(HugePages& hp, std::vector<CpuTopologyInfo> const& cpu_topology) {
    auto hw_thread_ids = hw_thread_id(cpu_topology); // Sorted by hw_thread_id: avoid HT, same socket.

    std::printf("---- Running throughput benchmarks (higher is better) ----\n");

    int constexpr SIZE = 65536;

    run_throughput_spsc_benchmark("boost::lockfree::spsc_queue", hp, hw_thread_ids,
                                  Type<BoostSpScAdapter<boost::lockfree::spsc_queue<unsigned, boost::lockfree::capacity<SIZE>>>>{});
    run_throughput_mpmc_benchmark("boost::lockfree::queue", hp, hw_thread_ids,
                                  Type<BoostQueueAdapter<boost::lockfree::queue<unsigned, BoostAllocator, boost::lockfree::capacity<SIZE - 2>>>>{});

    run_throughput_spsc_benchmark("es::lockfree::mpmc_queue<f,f>", hp, hw_thread_ids,
                                  Type<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, false, false>>>{});
    run_throughput_mpmc_benchmark("es::lockfree::mpmc_queue<f,f>", hp, hw_thread_ids,
                                  Type<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, false, false>>>{});

    run_throughput_spsc_benchmark("es::lockfree::mpmc_queue<f,t>", hp, hw_thread_ids,
                                  Type<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, false, true>>>{});
    run_throughput_mpmc_benchmark("es::lockfree::mpmc_queue<f,t>", hp, hw_thread_ids,
                                  Type<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, false, true>>>{});

    run_throughput_spsc_benchmark("es::lockfree::mpmc_queue<t,f>", hp, hw_thread_ids,
                                  Type<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, true, false>>>{});
    run_throughput_mpmc_benchmark("es::lockfree::mpmc_queue<t,f>", hp, hw_thread_ids,
                                  Type<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, true, false>>>{});

    run_throughput_spsc_benchmark("es::lockfree::mpmc_queue<t,t>", hp, hw_thread_ids,
                                  Type<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, true, true>>>{});
    run_throughput_mpmc_benchmark("es::lockfree::mpmc_queue<t,t>", hp, hw_thread_ids,
                                  Type<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, true, true>>>{});

    run_throughput_mpmc_benchmark("pthread_spinlock", hp, hw_thread_ids, Type<RetryDecorator<AtomicQueueSpinlock<unsigned, SIZE>>>{});
    // run_throughput_mpmc_benchmark("FairSpinlock", hp, hw_thread_ids, Type<RetryDecorator<AtomicQueueMutex<unsigned, SIZE, FairSpinlock>>>{});
    // run_throughput_mpmc_benchmark("UnfairSpinlock", hp, hw_thread_ids, Type<RetryDecorator<AtomicQueueMutex<unsigned, SIZE, UnfairSpinlock>>>{});

    run_throughput_spsc_benchmark("moodycamel::ReaderWriterQueue", hp, hw_thread_ids, Type<MoodyCamelReaderWriterQueue<unsigned, SIZE>>{});
    run_throughput_mpmc_benchmark("moodycamel::ConcurrentQueue", hp, hw_thread_ids, Type<MoodyCamelQueue<unsigned, SIZE>>{});

    run_throughput_mpmc_benchmark("tbb::spin_mutex", hp, hw_thread_ids, Type<RetryDecorator<AtomicQueueMutex<unsigned, SIZE, tbb::spin_mutex>>>{});
    run_throughput_mpmc_benchmark("tbb::speculative_spin_mutex", hp, hw_thread_ids,
                                  Type<RetryDecorator<AtomicQueueMutex<unsigned, SIZE, tbb::speculative_spin_mutex>>>{});
    run_throughput_mpmc_benchmark("tbb::concurrent_bounded_queue", hp, hw_thread_ids, Type<TbbAdapter<tbb::concurrent_bounded_queue<unsigned>, SIZE>>{});

    using SPSC = QueueTypes<SIZE, true, false, false>;
    using MPMC = QueueTypes<SIZE, false, true, true>; // Enable MAXIMIZE_THROUGHPUT for 2 or more producers/consumers.

    run_throughput_spsc_benchmark("AtomicQueue", hp, hw_thread_ids, SPSC::AtomicQueue{});
    run_throughput_mpmc_benchmark("AtomicQueue", hp, hw_thread_ids, MPMC::AtomicQueue{}, 2);

    run_throughput_spsc_benchmark("AtomicQueueB", hp, hw_thread_ids, SPSC::AtomicQueueB{});
    run_throughput_mpmc_benchmark("AtomicQueueB", hp, hw_thread_ids, MPMC::AtomicQueueB{}, 2);

    run_throughput_spsc_benchmark("OptimistAtomicQueue", hp, hw_thread_ids, SPSC::OptimistAtomicQueue{});
    run_throughput_mpmc_benchmark("OptimistAtomicQueue", hp, hw_thread_ids, MPMC::OptimistAtomicQueue{}, 2);

    run_throughput_spsc_benchmark("OptimistAtomicQueueB", hp, hw_thread_ids, SPSC::OptimistAtomicQueueB{});
    run_throughput_mpmc_benchmark("OptimistAtomicQueueB", hp, hw_thread_ids, MPMC::OptimistAtomicQueueB{}, 2);

    run_throughput_spsc_benchmark("AtomicQueue2", hp, hw_thread_ids, SPSC::AtomicQueue2{});
    run_throughput_mpmc_benchmark("AtomicQueue2", hp, hw_thread_ids, MPMC::AtomicQueue2{}, 2);

    run_throughput_spsc_benchmark("AtomicQueueB2", hp, hw_thread_ids, SPSC::AtomicQueueB2{});
    run_throughput_mpmc_benchmark("AtomicQueueB2", hp, hw_thread_ids, MPMC::AtomicQueueB2{}, 2);

    run_throughput_spsc_benchmark("OptimistAtomicQueue2", hp, hw_thread_ids, SPSC::OptimistAtomicQueue2{});
    run_throughput_mpmc_benchmark("OptimistAtomicQueue2", hp, hw_thread_ids, MPMC::OptimistAtomicQueue2{}, 2);

    run_throughput_spsc_benchmark("OptimistAtomicQueueB2", hp, hw_thread_ids, SPSC::OptimistAtomicQueueB2{});
    run_throughput_mpmc_benchmark("OptimistAtomicQueueB2", hp, hw_thread_ids, MPMC::OptimistAtomicQueueB2{}, 2);

    // run_throughput_mpmc_benchmark<RetryDecorator<AtomicQueueSpinlockHle<unsigned, SIZE>>>("SpinlockHle");

    std::printf("\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class Queue, bool Sender>
void ping_pong_thread_impl(Queue* q1, Queue* q2, unsigned N, uint64_t* time) {
    uint64_t t0 = __builtin_ia32_rdtsc();
    for(unsigned i = 1, j = 0; j < N; ++i) {
        if(Sender) {
            q1->push(i);
            j = q2->pop();
        } else {
            j = q1->pop();
            q2->push(i);
        }
    }
    uint64_t t1 = __builtin_ia32_rdtsc();
    *time = t1 - t0;
}

template<class Queue>
inline void ping_pong_thread_receiver(Barrier* barrier, Queue* q1, Queue* q2, unsigned N, uint64_t* time) {
    barrier->wait();
    ping_pong_thread_impl<Queue, false>(q1, q2, N, time);
}

template<class Queue>
inline void ping_pong_thread_sender(Barrier* barrier, Queue* q1, Queue* q2, unsigned N, uint64_t* time) {
    barrier->release(1);
    ping_pong_thread_impl<Queue, true>(q1, q2, N, time);
}

template<class Queue>
inline std::array<uint64_t, 2> ping_pong_benchmark(unsigned N, HugePages& hp, unsigned const (&cpus)[2]) {
    set_thread_affinity(cpus[0]); // This thread is the sender.
    auto q1 = hp.create_unique_ptr<Queue>();
    auto q2 = hp.create_unique_ptr<Queue>();
    Barrier barrier;
    std::array<uint64_t, 2> times;
    set_default_thread_affinity(cpus[1]);
    std::thread receiver(ping_pong_thread_receiver<Queue>, &barrier, q1.get(), q2.get(), N, &times[0]);
    ping_pong_thread_sender<Queue>(&barrier, q1.get(), q2.get(), N, &times[1]);
    receiver.join();
    return times;
}

template<class Queue>
void run_ping_pong_benchmark(char const* name, HugePages& hp, std::vector<unsigned> const& hw_thread_ids) {
    int constexpr N = 100000;
    int constexpr RUNS = 10;

    unsigned const cpus[2] = {hw_thread_ids[0], hw_thread_ids[1]};

    // select the best of RUNS runs.
    std::array<uint64_t, 2> best_times = {std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max()};
    for(unsigned run = RUNS; run--;) {
        auto times = ping_pong_benchmark<Queue>(N, hp, cpus);
        if(best_times[0] + best_times[1] > times[0] + times[1])
            best_times = times;

        check_huge_pages_leaks(name, hp);
    }

    auto avg_time = to_seconds((best_times[0] + best_times[1]) / 2);
    auto round_trip_time = avg_time / N;
    std::printf("%32s: %.9f sec/round-trip\n", name, round_trip_time);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void run_ping_pong_benchmarks(HugePages& hp, std::vector<CpuTopologyInfo> const& cpu_topology) {
    auto hw_thread_ids = hw_thread_id(cpu_topology); // Sorted by hw_thread_id: avoid HT, same socket.

    std::printf("---- Running ping-pong benchmarks (lower is better) ----\n");

    // This benchmarks doesn't require queue capacity greater than 1, however, capacity of 1 elides
    // some instructions completely because of (x % 1) is always 0. Use something greater than 1 to
    // preclude aggressive optimizations.
    constexpr unsigned SIZE = 8;

    run_ping_pong_benchmark<BoostSpScAdapter<boost::lockfree::spsc_queue<unsigned, boost::lockfree::capacity<SIZE>>>>("boost::lockfree::spsc_queue", hp,
                                                                                                                      hw_thread_ids);
    run_ping_pong_benchmark<BoostQueueAdapter<boost::lockfree::queue<unsigned, BoostAllocator, boost::lockfree::capacity<SIZE>>>>("boost::lockfree::queue", hp,
                                                                                                                                  hw_thread_ids);
    run_ping_pong_benchmark<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, false, false>>>("es::lockfree::mpmc_queue<f,f>", hp,   hw_thread_ids);
    run_ping_pong_benchmark<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, false, true >>>("es::lockfree::mpmc_queue<f,t>", hp,   hw_thread_ids);
    run_ping_pong_benchmark<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, true , false>>>("es::lockfree::mpmc_queue<t,f>", hp,   hw_thread_ids);
    run_ping_pong_benchmark<ESLockfreeMPMCQueueAdapter<es::lockfree::mpmc_queue<unsigned, SIZE, uint32_t, true , true >>>("es::lockfree::mpmc_queue<t,t>", hp,   hw_thread_ids);

    run_ping_pong_benchmark<RetryDecorator<AtomicQueueSpinlock<unsigned, SIZE>>>("pthread_spinlock", hp, hw_thread_ids);
    // run_ping_pong_benchmark<RetryDecorator<AtomicQueueMutex<unsigned, SIZE, FairSpinlock>>>("FairSpinlock", hp, hw_thread_ids);
    // run_ping_pong_benchmark<RetryDecorator<AtomicQueueMutex<unsigned, SIZE, UnfairSpinlock>>>("UnfairSpinlock", hp, hw_thread_ids);

    run_ping_pong_benchmark<MoodyCamelReaderWriterQueue<unsigned, SIZE>>("moodycamel::ReaderWriterQueue", hp, hw_thread_ids);
    run_ping_pong_benchmark<MoodyCamelQueue<unsigned, SIZE>>("moodycamel::ConcurrentQueue", hp, hw_thread_ids);

    run_ping_pong_benchmark<RetryDecorator<AtomicQueueMutex<unsigned, SIZE, tbb::spin_mutex>>>("tbb::spin_mutex", hp, hw_thread_ids);
    run_ping_pong_benchmark<RetryDecorator<AtomicQueueMutex<unsigned, SIZE, tbb::speculative_spin_mutex>>>("tbb::speculative_spin_mutex", hp, hw_thread_ids);
    run_ping_pong_benchmark<TbbAdapter<tbb::concurrent_bounded_queue<unsigned>, SIZE>>("tbb::concurrent_bounded_queue", hp, hw_thread_ids);

    // Use MAXIMIZE_THROUGHPUT=false for better latency.
    using SPSC = QueueTypes<SIZE, true, false, false>;

    run_ping_pong_benchmark<SPSC::AtomicQueue::type>("AtomicQueue", hp, hw_thread_ids);
    run_ping_pong_benchmark<SPSC::AtomicQueueB::type>("AtomicQueueB", hp, hw_thread_ids);
    run_ping_pong_benchmark<SPSC::OptimistAtomicQueue::type>("OptimistAtomicQueue", hp, hw_thread_ids);
    run_ping_pong_benchmark<SPSC::OptimistAtomicQueueB::type>("OptimistAtomicQueueB", hp, hw_thread_ids);
    run_ping_pong_benchmark<SPSC::AtomicQueue2::type>("AtomicQueue2", hp, hw_thread_ids);
    run_ping_pong_benchmark<SPSC::AtomicQueueB2::type>("AtomicQueueB2", hp, hw_thread_ids);
    run_ping_pong_benchmark<SPSC::OptimistAtomicQueue2::type>("OptimistAtomicQueue2", hp, hw_thread_ids);
    run_ping_pong_benchmark<SPSC::OptimistAtomicQueueB2::type>("OptimistAtomicQueueB2", hp, hw_thread_ids);

    // run_ping_pong_benchmark<RetryDecorator<AtomicQueueSpinlockHle<unsigned, SIZE>>>("SpinlockHle");

    std::printf("\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void advise_hugeadm_1GB() {
    std::fprintf(stderr, "Warning: Failed to allocate 1GB huge pages. Run \"sudo hugeadm --pool-pages-min 1GB:1 --pool-pages-max 1GB:1\".\n");
}

void advise_hugeadm_2MB() {
    std::fprintf(stderr, "Warning: Failed to allocate 2MB huge pages. Run \"sudo hugeadm --pool-pages-min 2MB:16 --pool-pages-max 2MB:16\".\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int main() {
    std::setlocale(LC_NUMERIC, "");

    auto cpu_topology = get_cpu_topology_info();
    if(cpu_topology.size() < 2)
        throw std::runtime_error("A CPU with at least 2 hardware threads is required.");

    HugePages::warn_no_1GB_pages = advise_hugeadm_1GB;
    HugePages::warn_no_2MB_pages = advise_hugeadm_2MB;
    size_t constexpr MB = 1024 * 1024;
    HugePages hp(HugePages::PAGE_1GB, 32 * MB); // Try allocating a 1GB huge page to minimize TLB misses.
    HugePageAllocatorBase::hp = &hp;

    run_throughput_benchmarks(hp, cpu_topology);
    run_ping_pong_benchmarks(hp, cpu_topology);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
