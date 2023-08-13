#include <cstdint>
#include <atomic>
#include <thread>
#include <stdio.h>
#include <chrono>
#include <pthread.h>
#include <sched.h>
#include <iostream>
#include <immintrin.h>

#ifdef TOPOLOGY
#include <cpuid.h>
#endif

std::atomic_bool keepRunning = true;

#define rdtsc __builtin_ia32_rdtsc

#ifdef PAUSE_NOP

#define pause() \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop"); \
    asm("nop");

#else
#define pause() _mm_pause()
#endif

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
            pause();
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
            pause();
        }

        localTickReqReceived = rdtsc();

        data.serverToClientChannel.store(((uint64_t)((uint32_t)(localTickRespSend = rdtsc())) << 32) | (uint64_t)(uint32_t)localTickReqReceived);
    }
}

int main(int argc, char **argv)
{

    int cores = atoi(argv[1]);

// Enumerate topology information at startup for later analysis
#ifdef TOPOLOGY
    for (uint16_t idx = 0; idx < cores; idx++)
    {

        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(idx, &set);

        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        sched_yield();

        unsigned int eax, ebx, ecx, edx;
        uint16_t type[5];
        uint16_t siblings[5];
        uint16_t level = 0;
        for (level = 0; level < 5; level++)
        {
            ecx = level;
            __get_cpuid_count(0x1F /* v2 extended topology leaf */, level, &eax, &ebx, &ecx, &edx);

            std::cout << " eax: " << eax << ", ebx: " << ebx << ", ecx: " << ecx << ", edx: " << edx << std::endl;
            siblings[level] = ebx;
            type[level] = (ecx >> 8) & 0xff;
            if (type[level] == 0)
            {
                break;
            }
        }
        std::cout << "CPU: " << idx;
        for (uint16_t idx = 0; idx <= level; idx++)
        {
            std::cout << ", level, " << level << ", type: " << type[idx] << ", siblings: " << siblings[idx];
        }
        std::cout << std::endl;
    }
#endif

    return 0;

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

            std::this_thread::sleep_for(std::chrono::milliseconds{250});

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
