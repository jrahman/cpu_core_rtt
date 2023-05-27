#include <cstdint>
#include <atomic>
#include <thread>
#include <stdio.h>
#include <chrono>
#include <pthread.h>
#include <sched.h>
#include <iostream>

#include "Util.h"

#ifdef __APPLE__

#include <mach/thread_policy.h>
#include <mach/task_info.h>
#include <mach/thread_act.h>

typedef struct cpu_set
{
    uint32_t count;
} cpu_set_t;

static inline void
CPU_ZERO(cpu_set_t *cs) { cs->count = 0; }

static inline void
CPU_SET(int num, cpu_set_t *cs) { cs->count |= (1 << num); }

static inline int
CPU_ISSET(int num, cpu_set_t *cs) { return (cs->count & (1 << num)); }

int pthread_setaffinity_np(pthread_t thread, size_t cpu_size,
                           cpu_set_t *cpu_set)
{
    thread_port_t mach_thread = pthread_mach_thread_np(thread);
    int core = 0;

    for (; core < 8 * cpu_size; core++)
    {
        if (CPU_ISSET(core, cpu_set))
            break;
    }
    thread_affinity_policy_data_t policy = {core};
    thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY,
                      (thread_policy_t)&policy, 1);
    return 0;
}
#endif

std::atomic_bool keepRunning = true;

// TODO 128 byte cache lines for M1
constexpr auto kCacheLineSize = 64;

struct
{
    std::atomic<uint64_t> clientToServerChannel{0};
    uint8_t padding1[kCacheLineSize * 8];

    std::atomic<uint64_t> serverToClientChannel{0};
    uint8_t padding2[kCacheLineSize * 8];

    struct
    {
        int64_t offset;
        uint64_t rtt;
    } results[64];

    uint16_t idx;
} data;

void client(uint16_t core)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);

    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr);
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);

    uint64_t localTickReqSend, remoteTickReqReceive, remoteTickRespSend, localTickRespReceive, resp = 0;

    while (keepRunning)
    {
        localTickReqSend = rdtsc();
        data.clientToServerChannel.store(localTickReqSend);

        uint64_t originalResp = resp;
        while ((resp = data.serverToClientChannel.load(std::memory_order_seq_cst)) == originalResp)
        {
            mm_pause();
        }

        localTickRespReceive = rdtsc();

        auto upperBits = (localTickRespReceive & 0xffffffff00000000);
        remoteTickRespSend = ((resp & 0xffffffff00000000) >> 32) | upperBits;
        remoteTickReqReceive = (resp & 0x00000000ffffffff) | upperBits;

        auto offset = (((int64_t)remoteTickReqReceive - (int64_t)localTickReqSend) + ((int64_t)remoteTickRespSend - (int64_t)localTickRespReceive)) / 2;
        auto rtt = (localTickRespReceive - localTickReqSend) - (remoteTickRespSend - remoteTickReqReceive);

        data.results[data.idx++ % 64] = typeof(data.results[0]){.offset = offset, .rtt = rtt};
    }
}

void server(uint16_t core)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);

    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr);
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);

    uint64_t localTickReqReceived, remoteTickReqSend = 0, remoteTickRespReceived, localTickRespSend;

    while (keepRunning)
    {
        auto originalTick = remoteTickReqSend;
        while ((remoteTickReqSend = data.clientToServerChannel.load()) == originalTick)
        {
            mm_pause();
        }

        localTickReqReceived = rdtsc();

        data.serverToClientChannel.store(((uint64_t)((uint32_t)(localTickRespSend = rdtsc())) << 32) | (uint64_t)(uint32_t)localTickReqReceived);
    }
}

int main(int argc, char **argv)
{

    int cores = atoi(argv[1]);

    for (uint16_t idx = 0; idx < cores; idx++)
    {
        for (uint16_t jdx = 0; jdx < cores; jdx++)
        {
            if (idx == jdx)
            {
                continue;
            }

            keepRunning = true;

            std::cerr << "Starting threads for " << idx << ", " << jdx << std::endl;

            std::thread serverThread{[&]()
                                     {
                                         server(idx);
                                     }};
            std::thread clientThread{[&]()
                                     {
                                         client(jdx);
                                     }};

            std::this_thread::sleep_for(std::chrono::milliseconds{1000});

            std::cerr << "Cleaning up threads..." << std::endl;

            keepRunning = false;

            std::this_thread::sleep_for(std::chrono::milliseconds{25});

            pthread_cancel(serverThread.native_handle());
            pthread_cancel(clientThread.native_handle());

            std::cerr << "Joining threads..." << std::endl;

            if (serverThread.joinable())
            {
                serverThread.join();
            }

            if (clientThread.joinable())
            {
                clientThread.join();
            }

            for (uint16_t i = 0; i < 64; i++)
            {
                std::cout << idx << "," << jdx << "," << data.results[i].rtt << "," << data.results[i].offset << std::endl;
            }
        }
    }

    return 0;
}