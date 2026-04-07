// benchmark.cpp - 对比 zrqueue 与 rigtorp::SPSCQueue 性能
// 支持 Windows (MSVC/MinGW) 和 Linux (GCC/Clang)

#include "zrqueue.h"
#include "rigtorp/SPSCQueue.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <atomic>

// ========== 跨平台线程绑定 (CPU Affinity) ==========
#if defined(_WIN32)
    #include <windows.h>
    static void pinThread(int cpu) {
        if (cpu < 0) {
            return;
        }
        // 将线程绑定到指定逻辑处理器 (仅支持前 64 个核心)
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
        // 其他平台：不做绑定
    }
#endif

constexpr size_t QUEUE_SIZE = 10000000;   // 队列容量
constexpr int64_t ITERS = 10000000;       // 消息数量

int main(int argc, char* argv[]) {
    int cpu1 = -1;
    int cpu2 = -1;

    if (argc >= 3) {
        cpu1 = std::atoi(argv[1]);
        cpu2 = std::atoi(argv[2]);
    }

    const size_t queueSize = 10000000;
    const int64_t iters = 10000000;

    //{
    //    zrqueue::SPSCStaticQueue<int, 128> q;
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

    std::cout << "\nzrqueue::SPSCQueue:" << std::endl;
    {
        zrqueue::SPSCQueue<int> q(queueSize);
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
        zrqueue::SPSCQueue<int> q1(queueSize), q2(queueSize);
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
