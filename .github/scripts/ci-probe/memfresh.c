// [TEST PR] Host first-touch latency probe.
//   memfresh <MB> <interval_sec>
// Every <interval_sec>: mmap <MB> of fresh anonymous memory, touch each page
// while timing it, munmap. Fresh guest pages that the host has never backed
// need a host-side allocation (EPT fault); on a host under memory pressure or
// doing hugepage compaction that costs milliseconds to seconds per page
// instead of about a microsecond.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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
    int const interval = atoi(argv[2]);
    size_t const len = mb << 20, page = 4096;
    setvbuf(stdout, NULL, _IOLBF, 0);
    char b[48];
    for (;;)
    {
        volatile unsigned char* buf = mmap(
            NULL,
            len,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0);
        if (buf == MAP_FAILED)
        {
            perror("mmap");
            return 1;
        }
        double const t0 = now_ms();
        double max = 0, sum_slow = 0;
        long slow = 0;
        for (size_t off = 0; off < len; off += page)
        {
            double const a = now_ms();
            buf[off] = 1;
            double const d = now_ms() - a;
            if (d > 1.0)
            {
                ++slow;
                sum_slow += d;
            }
            if (d > max)
                max = d;
        }
        double const total = now_ms() - t0;
        munmap((void*)buf, len);
        utc(b, sizeof b);
        printf(
            "%s memfresh mb=%zu total=%.0fms us_per_page=%.2f pages_over_1ms=%ld "
            "slow_sum=%.0fms max_page=%.1fms\n",
            b,
            mb,
            total,
            total * 1000.0 / (double)(len / page),
            slow,
            sum_slow,
            max);
        sleep(interval);
    }
}
