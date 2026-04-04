// zrqueue Benchmark Test
// Compile: g++ -O3 -std=c++17 -march=native -pthread benchmark_zrqueue.cpp -o benchmark_zrqueue

#include "zrqueue.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <stdio.h>
#include <stdlib.h>

#if defined(__linux__)
    #ifndef _GNU_SOURCE
    #define _GNU_SOURCE
    #endif
    #include <pthread.h>
    #include <sched.h>
#elif defined(_WIN32)
    #include <windows.h>
#else
    #warning "Unsupported platform for thread pinning."
#endif

void pinThread(int cpu) {
if (cpu < 0) {
    return;
}

#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        perror("pthread_setaffinity_np failed");
        exit(1);
    }

#elif defined(_WIN32)
    // 注意：SetThreadAffinityMask 使用位掩码，通常只能处理前 64 个逻辑核心
    if (cpu >= (sizeof(DWORD_PTR) * 8)) {
        fprintf(stderr, "CPU index out of range for SetThreadAffinityMask\n");
        exit(1);
    }

    DWORD_PTR mask = (DWORD_PTR)1 << cpu;
    HANDLE thread = GetCurrentThread();
    if (SetThreadAffinityMask(thread, mask) == 0) {
        fprintf(stderr, "SetThreadAffinityMask failed, error code: %lu\n", GetLastError());
        exit(1);
    }
#else
    // 兜底逻辑：不支持的平台不做任何处理
    fprintf(stderr, "Thread pinning is not supported on this platform.\n");
#endif
}

int main(int argc, char* argv[]) {
    (void)argc, (void)argv;

    using namespace zrqueue;

    int cpu1 = -1;
    int cpu2 = -1;

    if (argc == 3) {
        cpu1 = std::atoi(argv[1]);
        cpu2 = std::atoi(argv[2]);
    }

    const size_t queueSize = 10000000;
    const int64_t iters = 10000000;

    std::cout << "zrqueue::SPSCQueue:" << std::endl;

    {
        SPSCQueue<int> q(queueSize);
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
        SPSCQueue<int> q1(queueSize), q2(queueSize);
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

    return 0;
}
