// [TEST PR] Stall probe for the coverage runner VM.
//   stall_probe spin <cpu> <threshold_ms>   pin to <cpu>, spin, report gaps
//   between
//                                           consecutive clock reads above
//                                           threshold
//   stall_probe sleep <ms> <threshold_ms>   nanosleep(<ms>) in a loop, report
//   wake-ups
//                                           late by more than threshold
//   stall_probe exit <cpu> <threshold_ms>   pin, execute cpuid (an
//   unconditional VM
//                                           exit) once per ms; a gap whose
//                                           work= equals the gap is a vCPU
//                                           stuck in the hypervisor
//   stall_probe fault <cpu> <threshold_ms>  pin, map+touch+unmap 2 MB of fresh
//   pages
//                                           every 5 ms; work= equal to the gap
//                                           means the stall is in the
//                                           page-fault/EPT path
// Each gap line carries the thread's voluntary/involuntary context switch
// deltas: a spin gap with nivcsw+=0 means the thread stayed on its CPU from the
// guest's point of view, i.e. the vCPU itself was not running (hypervisor),
// while nivcsw>0 is ordinary in-guest preemption. Counters are read every
// iteration so the deltas cover exactly the reported gap.
#define _GNU_SOURCE
#include <cpuid.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void
utc(char* buf, size_t n)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    size_t l = strftime(buf, n, "%FT%T", &tm);
    snprintf(buf + l, n - l, ".%03ldZ", ts.tv_nsec / 1000000);
}

enum mode { SPIN, SLEEP, EXIT, FAULT, BIG };

// One unit of work for the exit/fault modes; returns its duration in ms.
// For fault mode the three phases are timed separately so a slow unit can
// be attributed to mmap (guest VMA work), the touches (guest page fault +
// EPT fault in the host) or munmap (TLB shootdown / PV flush).
static double g_map, g_touch, g_unmap;

static double
work(enum mode m)
{
    double const t0 = now_ms();
    if (m == EXIT)
    {
        unsigned a, b, c, d;
        __cpuid(0, a, b, c, d);
    }
    else if (m == FAULT || m == BIG)
    {
        size_t const n = m == BIG ? 64u << 20 : 2u << 20;
        char* p = mmap(
            NULL,
            n,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0);
        double const t1 = now_ms();
        if (p != MAP_FAILED)
        {
            for (size_t i = 0; i < n; i += 4096)
                p[i] = 1;
        }
        double const t2 = now_ms();
        if (p != MAP_FAILED)
            munmap(p, n);
        double const t3 = now_ms();
        g_map = t1 - t0;
        g_touch = t2 - t1;
        g_unmap = t3 - t2;
    }
    return now_ms() - t0;
}

int
main(int argc, char** argv)
{
    if (argc < 4)
        return 2;
    enum mode const m = strcmp(argv[1], "spin") == 0 ? SPIN
        : strcmp(argv[1], "sleep") == 0              ? SLEEP
        : strcmp(argv[1], "exit") == 0               ? EXIT
        : strcmp(argv[1], "fault") == 0              ? FAULT
                                                     : BIG;
    int const spin = m != SLEEP;
    double const pace = m == EXIT ? 1 : m == FAULT ? 5 : m == BIG ? 1000 : 0;
    int const arg = atoi(argv[2]);
    double const thr = atof(argv[3]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (spin)
    {
        cpu_set_t s;
        CPU_ZERO(&s);
        CPU_SET(arg, &s);
        if (sched_setaffinity(0, sizeof s, &s) != 0)
            perror("sched_setaffinity");
    }
    struct rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    long pv = ru.ru_nvcsw, pi = ru.ru_nivcsw;
    double prev = now_ms(), last_report = prev, maxgap = 0;
    long gaps = 0, gaps_no_switch = 0;
    char b[48];
    for (;;)
    {
        double w = 0;
        if (!spin)
        {
            struct timespec r = {arg / 1000, (arg % 1000) * 1000000L};
            nanosleep(&r, NULL);
        }
        else if (pace > 0)
        {
            w = work(m);
            while (now_ms() - prev < pace)
                ;
        }
        double const t = now_ms();
        double const gap = t - prev - (spin ? 0 : arg);
        // Track the switch counters continuously so a gap's delta covers
        // only that gap (a syscall per iteration is fine for a probe).
        getrusage(RUSAGE_THREAD, &ru);
        long const dv = ru.ru_nvcsw - pv, di = ru.ru_nivcsw - pi;
        pv = ru.ru_nvcsw;
        pi = ru.ru_nivcsw;
        if (gap > thr)
        {
            utc(b, sizeof b);
            printf(
                "%s %s gap=%.0fms cpu=%d nvcsw+=%ld nivcsw+=%ld work=%.0fms "
                "map/touch/unmap=%.0f/%.0f/%.0f\n",
                b,
                argv[1],
                gap,
                sched_getcpu(),
                dv,
                di,
                w,
                g_map,
                g_touch,
                g_unmap);
            ++gaps;
            if (spin && di == 0)
                ++gaps_no_switch;
            if (gap > maxgap)
                maxgap = gap;
        }
        if (t - last_report > 60000)
        {
            utc(b, sizeof b);
            printf(
                "%s %s alive cpu=%d gaps=%ld gaps_without_switch=%ld "
                "maxgap=%.0fms\n",
                b,
                argv[1],
                sched_getcpu(),
                gaps,
                gaps_no_switch,
                maxgap);
            last_report = t;
        }
        prev = t;
    }
}
