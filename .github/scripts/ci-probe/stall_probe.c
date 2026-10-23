// [TEST PR] Stall probe for the coverage runner VM.
//   stall_probe spin <cpu> <threshold_ms>   pin to <cpu>, spin, report gaps between
//                                           consecutive clock reads above threshold
//   stall_probe sleep <ms> <threshold_ms>   nanosleep(<ms>) in a loop, report wake-ups
//                                           late by more than threshold
// Each gap line carries the thread's voluntary/involuntary context switch deltas:
// a spin gap with nivcsw+=0 means the thread stayed on its CPU from the guest's
// point of view, i.e. the vCPU itself was not running (hypervisor), while
// nivcsw>0 is ordinary in-guest preemption. Counters are read every
// iteration so the deltas cover exactly the reported gap.
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int
main(int argc, char** argv)
{
    if (argc < 4)
        return 2;
    int const spin = strcmp(argv[1], "spin") == 0;
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
        if (!spin)
        {
            struct timespec r = {arg / 1000, (arg % 1000) * 1000000L};
            nanosleep(&r, NULL);
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
                "%s %s gap=%.0fms cpu=%d nvcsw+=%ld nivcsw+=%ld\n",
                b,
                argv[1],
                gap,
                sched_getcpu(),
                dv,
                di);
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
                "%s %s alive cpu=%d gaps=%ld gaps_without_switch=%ld maxgap=%.0fms\n",
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
