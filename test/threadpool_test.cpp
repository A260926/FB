#include <bvh/v2/bvh.h>
#include <bvh/v2/executor.h>
#include <bvh/v2/thread_pool.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    bvh::v2::ThreadPool threadpool;
    std::vector<std::thread> threads;

    size_t num_threads = 20;
    size_t num_tasks_per_thread = 10;
    size_t num_millis = 45;

    auto estimated_time = std::chrono::milliseconds(
        num_tasks_per_thread * num_millis * num_threads / threadpool.get_thread_count());

    std::cout
        << "Total estimated time: "
        << std::chrono::duration_cast<std::chrono::seconds>(estimated_time)
        << std::endl;

    // Creates a simple monitoring thread that waits for `estimated_time` milliseconds, and prints a
    // message every `estimated_time/10` milliseconds.
    threads.emplace_back([&]() {
        auto delta = std::max(std::chrono::milliseconds(1), estimated_time / 10);
        auto elapsed = std::chrono::milliseconds(0);

        while (elapsed < estimated_time) {
            std::cout << "Remaining time: " << estimated_time - elapsed << std::endl;
            std::this_thread::sleep_for(delta);
            elapsed += delta;
        }

        std::cout << "Finished" << std::endl;
    });

    // Submit lots of tasks concurrently.
    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            bvh::v2::ThreadPool::TaskGroup task_group;
            for (size_t j = 0; j < num_tasks_per_thread; ++j) {
                threadpool.push(task_group, [&](size_t) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(num_millis));
                });
            }
            threadpool.wait(task_group);
        });
    }

    std::cout << "Waiting for " << threads.size() << " threads" << std::endl;
    for (auto& thread : threads) {
        if (thread.joinable())
            thread.join();
    }

    threadpool.wait_all();
    std::cout << "Done waiting" << std::endl;
    return 0;
}
