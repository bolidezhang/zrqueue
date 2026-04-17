// benchmark.cpp - Benchmark zrqueue vs rigtorp::SPSCQueue
// Supports Windows (MSVC/MinGW) and Linux (GCC/Clang)

#include "zrqueue.h"
#include "rigtorp/SPSCQueue.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <atomic>

// ========== Cross-Platform Thread Pinning (CPU Affinity) ==========
#if defined(_WIN32)
    #include <windows.h>
    static void pinThread(int cpu) {
        if (cpu < 0) {
            return;
        }
        // Pin thread to the specified logical processor (supports up to the first 64 cores)
        DWORD_PTR mask = (cpu < 64) ? (DWORD_PTR)1 << cpu : (DWORD_PTR)-1;
        SetThreadAffinityMask(GetCurrentThread(), mask);
    }
#elif defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
    static void pinThread(int cpu) {
        if (cpu < 0) {
            return;
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#else
    static void pinThread(int) {
        // Other platforms: No pinning
    }
#endif

int main(int argc, char* argv[]) {
    int cpu1 = -1;
    int cpu2 = -1;

    if (argc >= 3) {
        cpu1 = std::atoi(argv[1]);
        cpu2 = std::atoi(argv[2]);
    }

    constexpr size_t queueSize = 10000000;
    constexpr size_t queueSize2 = zrqueue::normalize_size(10000000);
    constexpr int64_t iters = 10000000;

    //{
    //    zrqueue::SpscInlineQueue<int, 128> q;
    //    zrqueue::SpscRingBuffer<int, 64> q1;
    //}

    std::cout << "\nrigtorp::SPSCQueue:" << std::endl;
    {
        rigtorp::SPSCQueue<int> q(queueSize);
        auto t = std::thread([&] {
            pinThread(cpu1);
            for (int i = 0; i < iters; ++i) {
                while (!q.front())
                    ;
                if (*q.front() != i) {
                    throw std::runtime_error("");
                }
                q.pop();
            }
        });

        pinThread(cpu2);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            q.emplace(i);
        }
        t.join();
        auto stop = std::chrono::steady_clock::now();
        std::cout << iters * 1000000 / std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()
            << " ops/ms" << std::endl;
    }

    {
        rigtorp::SPSCQueue<int> q1(queueSize), q2(queueSize);
        auto t = std::thread([&] {
            pinThread(cpu1);
            for (int i = 0; i < iters; ++i) {
                while (!q1.front())
                    ;
                q2.emplace(*q1.front());
                q1.pop();
            }
        });

        pinThread(cpu2);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            q1.emplace(i);
            while (!q2.front())
                ;
            q2.pop();
        }
        auto stop = std::chrono::steady_clock::now();
        t.join();
        std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() / iters
            << " ns RTT" << std::endl;
    }

    std::cout << "\nzrqueue::SpscQueue:" << std::endl;
    {
        zrqueue::SpscQueue<int> q(queueSize);
        auto t = std::thread([&] {
            pinThread(cpu1);
            for (int i = 0; i < iters; ++i) {
                while (!q.front())
                    ;
                if (*q.front() != i) {
                    throw std::runtime_error("");
                }
                q.pop();
            }
        });

        pinThread(cpu2);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            q.emplace(i);
        }
        t.join();
        auto stop = std::chrono::steady_clock::now();
        std::cout << iters * 1000000 / std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()
            << " ops/ms" << std::endl;
    }
    {
        zrqueue::SpscQueue<int> q1(queueSize), q2(queueSize);
        auto t = std::thread([&] {
            pinThread(cpu1);
            for (int i = 0; i < iters; ++i) {
                while (!q1.front())
                    ;
                q2.emplace(*q1.front());
                q1.pop();
            }
        });

        pinThread(cpu2);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            q1.emplace(i);
            while (!q2.front())
                ;
            q2.pop();
        }
        auto stop = std::chrono::steady_clock::now();
        t.join();
        std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() / iters
            << " ns RTT" << std::endl;
    }

    std::cout << "\nzrqueue::SpscInlineQueue:" << std::endl;
    {
        auto q = new zrqueue::SpscInlineQueue<int, queueSize2>();
        auto t = std::thread([&] {
            pinThread(cpu1);
            for (int i = 0; i < iters; ++i) {
                while (!q->front())
                    ;
                if (*q->front() != i) {
                    throw std::runtime_error("");
                }
                q->pop();
            }
        });

        pinThread(cpu2);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            q->emplace(i);
        }
        t.join();
        auto stop = std::chrono::steady_clock::now();
        delete q;

        std::cout << iters * 1000000 / std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()
            << " ops/ms" << std::endl;
    }
    {
        auto q1 = new zrqueue::SpscInlineQueue<int, queueSize2>();
        auto q2 = new zrqueue::SpscInlineQueue<int, queueSize2>();
        auto t = std::thread([&] {
            pinThread(cpu1);
            for (int i = 0; i < iters; ++i) {
                while (!q1->front())
                    ;
                q2->emplace(*q1->front());
                q1->pop();
            }
        });

        pinThread(cpu2);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            q1->emplace(i);
            while (!q2->front())
                ;
            q2->pop();
        }
        auto stop = std::chrono::steady_clock::now();
        t.join();
        delete q1;
        delete q2;

        std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() / iters
            << " ns RTT" << std::endl;
    }

    return 0;
}