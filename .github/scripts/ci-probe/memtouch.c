// [TEST PR] Host-swap / page-residency probe.
//   memtouch <MB> <idle_sec>
// Allocates and touches <MB> of memory, then repeatedly sleeps <idle_sec> and
// re-touches every page while timing each touch. Guest-resident memory costs
// well under a microsecond per page; pages the host has swapped out or
// reclaimed cost milliseconds each (a host page-in per page). A re-touch of a
// 512 MB buffer that takes seconds, with many pages over 1 ms, means the host
// is paging this VM's memory.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
    if (argc < 3)
        return 2;
    size_t const mb = (size_t)atoi(argv[1]);
    int const idle = atoi(argv[2]);
    size_t const len = mb << 20, page = 4096;
    setvbuf(stdout, NULL, _IOLBF, 0);
    volatile unsigned char* buf = mmap(
        NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }
    memset((void*)buf, 1, len);
    char b[48];
    for (;;)
    {
        sleep(idle);
        double const t0 = now_ms();
        double max = 0, sum_slow = 0;
        long slow = 0;
        for (size_t off = 0; off < len; off += page)
        {
            double const a = now_ms();
            buf[off] += 1;
            double const d = now_ms() - a;
            if (d > 1.0)
            {
                ++slow;
                sum_slow += d;
            }
            if (d > max)
                max = d;
        }
        utc(b, sizeof b);
        printf(
            "%s memtouch mb=%zu idle=%ds total=%.0fms pages_over_1ms=%ld "
            "slow_sum=%.0fms max_page=%.1fms\n",
            b,
            mb,
            idle,
            now_ms() - t0,
            slow,
            sum_slow,
            max);
    }
}
