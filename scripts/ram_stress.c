/*
 * ram_stress.c - DDR/LPDDR bandwidth stress + DDR DFS frequency sampler
 * for the SM-A032F (Unisoc SC9863A / SharkL3).
 *
 * It allocates one buffer per thread, faults it in, then runs three
 * phases: write (memset), read (64-bit sum), copy (memcpy half-to-half).
 * While that runs, a sampler thread reads the real DDR frequency from
 * the sprd scene-governor sysfs, plus AP/CP votes, CPU frequency,
 * thermal zone 1, and MemAvailable.
 *
 * Build (on the phone, Termux):
 *   clang -O3 -pthread -o ram_stress ram_stress.c
 *
 * Run (as root, negative nice):
 *   su -c 'nice -n -20 ./ram_stress'
 *
 * Options:
 *   -t N   threads (default 8)
 *   -s N   MiB per thread (default 24 -> 192 MiB total)
 *   -d N   seconds per phase (default 6)
 *   -b N   baseline seconds before phases (default 2)
 *   -P     do not pin threads to CPUs
 *
 * Read-only with respect to the phone: it only reads sysfs/procfs and
 * writes to its own anonymous memory. It never writes DDR sysfs, never
 * forces a DDR frequency, and never touches storage.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_THREADS   16
#define MAX_SAMPLES   4096
#define MIB           (1024ULL * 1024ULL)
#define NPHASES       3

#define DDR_GOV       "/sys/class/devfreq/scene-frequency/sprd-governor"
#define DDR_CUR       DDR_GOV "/ddrinfo_cur_freq"
#define DDR_AP        DDR_GOV "/ddrinfo_ap_freq"
#define DDR_CP        DDR_GOV "/ddrinfo_cp_freq"
#define DDR_FORCE     DDR_GOV "/scaling_force_ddr_freq"
#define DDR_MIN       "/sys/class/devfreq/scene-frequency/min_freq"
#define DDR_MAX       "/sys/class/devfreq/scene-frequency/max_freq"
#define DDR_GOVERNOR  "/sys/class/devfreq/scene-frequency/governor"
#define CPU0_CUR      "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"
#define CPU4_CUR      "/sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq"
#define TEMP_PATH     "/sys/class/thermal/thermal_zone1/temp"
#define MEMINFO       "/proc/meminfo"

static const char *phase_name[NPHASES] = { "write", "read", "copy" };

struct thr_arg {
    int id;
    int cpu;                    /* -1 = unpinned */
    unsigned char *buf;
    size_t len;
    uint64_t bytes[NPHASES];
    uint64_t checksum;
};

struct sample {
    double t;
    long ddr;
    long ap;
    long cp;
    long cpu0;
    long cpu4;
    long temp;                  /* millidegrees C */
    long memavail_kb;
};

static pthread_barrier_t g_barrier;
static volatile double g_phase_end;
static double g_phase_start[NPHASES];
static double g_phase_elapsed[NPHASES];
static volatile int g_abort;
static volatile int g_go;
static char g_abort_reason[160];
static int g_nthreads;
static double g_phase_sec;
static double g_t0;

static volatile int g_sampler_stop;
static struct sample g_samples[MAX_SAMPLES];
static int g_nsamples;

static double now_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static long read_long(const char *path)
{
    char buf[128];
    char *end = NULL;
    long v;

    if (read_text(path, buf, sizeof(buf)) != 0)
        return -1;
    errno = 0;
    v = strtol(buf, &end, 10);
    if (end == buf)
        return -1;
    return v;
}

static long mem_available_kb(void)
{
    FILE *f = fopen(MEMINFO, "r");
    char line[256];
    long v = -1;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %ld kB", &v) == 1)
            break;
    }
    fclose(f);
    return v;
}

static void set_abort(const char *fmt, long a, long b)
{
    if (g_abort)
        return;
    snprintf(g_abort_reason, sizeof(g_abort_reason), fmt, a, b);
    g_abort = 1;
}

static void *sampler_thread(void *arg)
{
    (void)arg;
    int printed = 0;

    while (!g_sampler_stop) {
        struct sample s;
        struct timespec ts;

        s.t = now_s() - g_t0;
        s.ddr = read_long(DDR_CUR);
        s.ap = read_long(DDR_AP);
        s.cp = read_long(DDR_CP);
        s.cpu0 = read_long(CPU0_CUR);
        s.cpu4 = read_long(CPU4_CUR);
        s.temp = read_long(TEMP_PATH);
        s.memavail_kb = mem_available_kb();

        if (g_nsamples < MAX_SAMPLES)
            g_samples[g_nsamples++] = s;

        if (s.memavail_kb > 0 && s.memavail_kb < 100 * 1024)
            set_abort("low MemAvailable: %ld kB", s.memavail_kb, 0);
        if (s.temp > 85000)
            set_abort("thermal limit: %ld millidegC", s.temp, 0);

        if (++printed >= 5) {
            printed = 0;
            printf("t=%6.2f ddr=%4ld ap=%4ld cp=%4ld "
                   "cpu0=%7ld cpu4=%7ld temp=%5.1f mem=%ldMB\n",
                   s.t, s.ddr, s.ap, s.cp, s.cpu0, s.cpu4,
                   s.temp / 1000.0,
                   s.memavail_kb > 0 ? s.memavail_kb / 1024 : -1);
            fflush(stdout);
        }

        ts.tv_sec = 0;
        ts.tv_nsec = 100 * 1000 * 1000;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static inline void do_phase(struct thr_arg *a, int p)
{
    uint64_t *q = (uint64_t *)a->buf;
    size_t n = a->len / sizeof(uint64_t);

    if (p == 0) {
        /* streaming write: every line is written */
        memset(a->buf, 0xA5, a->len);
        a->bytes[p] += a->len;
    } else if (p == 1) {
        /* streaming read with independent accumulators */
        uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        size_t i = 0;

        for (; i + 4 <= n; i += 4) {
            s0 += q[i + 0];
            s1 += q[i + 1];
            s2 += q[i + 2];
            s3 += q[i + 3];
        }
        for (; i < n; i++)
            s0 += q[i];

        a->checksum += s0 ^ s1 ^ s2 ^ s3;
        a->bytes[p] += a->len;
    } else {
        /* copy between the two halves: len/2 read + len/2 write */
        size_t half = a->len / 2;
        unsigned char *src = (a->id & 1) ? a->buf + half : a->buf;
        unsigned char *dst = (a->id & 1) ? a->buf : a->buf + half;

        memcpy(dst, src, half);
        a->bytes[p] += a->len;
    }
}

static void *worker_thread(void *arg)
{
    struct thr_arg *a = (struct thr_arg *)arg;
    int p;

    if (a->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(a->cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0)
            fprintf(stderr, "thread %d: pin to cpu%d failed: %s\n",
                    a->id, a->cpu, strerror(errno));
    }

    while (!g_go)
        sched_yield();

    for (p = 0; p < NPHASES; p++) {
        while (!g_abort) {
            double t = now_s();

            if (t >= g_phase_end)
                break;
            do_phase(a, p);
        }

        pthread_barrier_wait(&g_barrier);

        if (a->id == 0) {
            double t = now_s();

            g_phase_elapsed[p] = t - g_phase_start[p];
            printf("PHASE %-5s start=%6.2f end=%6.2f elapsed=%6.2f s\n",
                   phase_name[p], g_phase_start[p] - g_t0,
                   t - g_t0, g_phase_elapsed[p]);
            fflush(stdout);
            if (p + 1 < NPHASES && !g_abort) {
                g_phase_start[p + 1] = t;
                g_phase_end = t + g_phase_sec;
            }
        }

        pthread_barrier_wait(&g_barrier);
        if (g_abort)
            break;
    }
    return NULL;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-t threads] [-s MiB/thread] [-d sec/phase] "
            "[-b baseline_sec] [-P]\n", argv0);
}

int main(int argc, char **argv)
{
    int nthreads = 8;
    size_t mib = 24;
    double phase_sec = 6.0;
    double baseline_sec = 2.0;
    int pin = 1;
    int opt;
    int i;
    size_t total;
    long mem_before, mem_after;
    struct thr_arg *args;
    pthread_t workers[MAX_THREADS];
    pthread_t sampler;
    int started = 0;

    while ((opt = getopt(argc, argv, "t:s:d:b:P")) != -1) {
        switch (opt) {
        case 't':
            nthreads = atoi(optarg);
            break;
        case 's':
            mib = (size_t)strtoul(optarg, NULL, 10);
            break;
        case 'd':
            phase_sec = atof(optarg);
            break;
        case 'b':
            baseline_sec = atof(optarg);
            break;
        case 'P':
            pin = 0;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (nthreads < 1 || nthreads > MAX_THREADS) {
        fprintf(stderr, "threads must be 1..%d\n", MAX_THREADS);
        return 1;
    }
    if (mib < 1 || mib > 512) {
        fprintf(stderr, "MiB/thread must be 1..512\n");
        return 1;
    }
    if (phase_sec < 1.0 || phase_sec > 120.0) {
        fprintf(stderr, "phase seconds must be 1..120\n");
        return 1;
    }
    if (baseline_sec < 0.0 || baseline_sec > 60.0) {
        fprintf(stderr, "baseline seconds must be 0..60\n");
        return 1;
    }

    total = (size_t)nthreads * mib * MIB;
    mem_before = mem_available_kb();

    printf("RAM STRESS: %d threads x %zu MiB = %llu MiB total\n",
           nthreads, mib, (unsigned long long)(total / MIB));
    printf("mem before: %ld MB available\n",
           mem_before > 0 ? mem_before / 1024 : -1);
    {
        char gov[64] = "?";

        read_text(DDR_GOVERNOR, gov, sizeof(gov));
        printf("ddr max=%ld min=%ld governor=%s\n",
               read_long(DDR_MAX), read_long(DDR_MIN), gov);
    }
    printf("ddr before: cur=%ld ap=%ld cp=%ld force=%ld\n",
           read_long(DDR_CUR), read_long(DDR_AP), read_long(DDR_CP),
           read_long(DDR_FORCE));
    fflush(stdout);

    if (mem_before > 0 && mem_before < 300 * 1024) {
        fprintf(stderr, "refusing: only %ld MB available\n",
                mem_before / 1024);
        return 2;
    }

    args = calloc((size_t)nthreads, sizeof(*args));
    if (!args) {
        perror("calloc");
        return 3;
    }

    for (i = 0; i < nthreads; i++) {
        size_t len = mib * MIB;

        args[i].id = i;
        args[i].cpu = pin ? (i % 8) : -1;
        args[i].len = len;
        args[i].buf = mmap(NULL, len, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (args[i].buf == MAP_FAILED) {
            fprintf(stderr, "mmap %zu MiB for thread %d failed: %s\n",
                    mib, i, strerror(errno));
            goto out_free;
        }
        /* fault every page in before timing starts */
        memset(args[i].buf, 0, len);
    }

    mem_after = mem_available_kb();
    printf("mem after fault-in: %ld MB available\n",
           mem_after > 0 ? mem_after / 1024 : -1);
    if (mem_after > 0 && mem_after < 150 * 1024) {
        fprintf(stderr, "refusing: only %ld MB left after fault-in\n",
                mem_after / 1024);
        goto out_free;
    }

    g_nthreads = nthreads;
    g_phase_sec = phase_sec;
    g_t0 = now_s();
    g_sampler_stop = 0;
    g_nsamples = 0;
    g_abort = 0;
    g_abort_reason[0] = '\0';

    if (pthread_create(&sampler, NULL, sampler_thread, NULL) != 0) {
        perror("pthread_create sampler");
        goto out_free;
    }

    if (baseline_sec > 0.0) {
        struct timespec ts;
        ts.tv_sec = (time_t)baseline_sec;
        ts.tv_nsec = (long)((baseline_sec - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }

    g_phase_start[0] = now_s();
    g_phase_end = g_phase_start[0] + g_phase_sec;

    for (i = 0; i < nthreads; i++) {
        if (pthread_create(&workers[i], NULL, worker_thread, &args[i]) != 0) {
            perror("pthread_create worker");
            g_abort = 1;
            break;
        }
        started++;
    }

    if (started > 0) {
        pthread_barrier_init(&g_barrier, NULL, (unsigned)started);
        g_go = 1;
        for (i = 0; i < started; i++)
            pthread_join(workers[i], NULL);

        {
            struct timespec ts = { 0, 300 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    } else {
        g_abort = 1;
        snprintf(g_abort_reason, sizeof(g_abort_reason),
                 "no worker threads started");
    }

    g_sampler_stop = 1;
    pthread_join(sampler, NULL);

    printf("\n===== SUMMARY =====\n");
    if (g_abort)
        printf("ABORTED: %s\n", g_abort_reason);

    for (i = 0; i < nthreads; i++) {
        uint64_t sum = 0;
        int p;

        for (p = 0; p < NPHASES; p++)
            sum += args[i].bytes[p];
        if (sum == 0)
            printf("thread %d on cpu%d: no work\n",
                   args[i].id, args[i].cpu);
    }

    for (int p = 0; p < NPHASES; p++) {
        uint64_t sum = 0;
        double bw;

        for (i = 0; i < nthreads; i++)
            sum += args[i].bytes[p];
        bw = g_phase_elapsed[p] > 0.0
                 ? (double)sum / g_phase_elapsed[p] / 1e9
                 : 0.0;
        printf("phase %-5s: %6.2f s  %8.2f GB traffic  "
               "%6.2f GB/s\n",
               phase_name[p], g_phase_elapsed[p],
               (double)sum / 1e9, bw);
    }

    {
        long ddr_min = 1000000, ddr_max = -1;
        double ddr_sum = 0.0;
        int ddr_n = 0;
        long ap_max = -1, cp_max = -1;
        long cpu0_max = -1, cpu4_max = -1;
        long temp_min = 1000000, temp_max = -1;
        long mem_min = -1;
        int hist[2048];
        int h;

        memset(hist, 0, sizeof(hist));
        for (i = 0; i < g_nsamples; i++) {
            struct sample *s = &g_samples[i];

            if (s->ddr >= 0) {
                if (s->ddr < ddr_min) ddr_min = s->ddr;
                if (s->ddr > ddr_max) ddr_max = s->ddr;
                ddr_sum += (double)s->ddr;
                ddr_n++;
                if (s->ddr >= 0 && s->ddr < (long)(sizeof(hist) / sizeof(hist[0])))
                    hist[s->ddr]++;
            }
            if (s->ap > ap_max) ap_max = s->ap;
            if (s->cp > cp_max) cp_max = s->cp;
            if (s->cpu0 > cpu0_max) cpu0_max = s->cpu0;
            if (s->cpu4 > cpu4_max) cpu4_max = s->cpu4;
            if (s->temp >= 0) {
                if (s->temp < temp_min) temp_min = s->temp;
                if (s->temp > temp_max) temp_max = s->temp;
            }
            if (s->memavail_kb > 0 &&
                (mem_min < 0 || s->memavail_kb < mem_min))
                mem_min = s->memavail_kb;
        }

        printf("ddr samples=%d min=%ld max=%ld avg=%.1f MHz\n",
               ddr_n, ddr_min, ddr_max,
               ddr_n > 0 ? ddr_sum / ddr_n : 0.0);
        printf("ddr histogram (MHz:count):");
        for (h = 0; h < (int)(sizeof(hist) / sizeof(hist[0])); h++) {
            if (hist[h])
                printf(" %d:%d", h, hist[h]);
        }
        printf("\n");
        printf("ap max=%ld cp max=%ld\n", ap_max, cp_max);
        printf("cpu0 max=%ld cpu4 max=%ld kHz\n", cpu0_max, cpu4_max);
        printf("temp min=%.1f max=%.1f end=%.1f C\n",
               temp_min / 1000.0, temp_max / 1000.0,
               g_nsamples > 0 ? g_samples[g_nsamples - 1].temp / 1000.0 : -1.0);
        printf("mem min=%ld MB\n", mem_min > 0 ? mem_min / 1024 : -1);
        printf("ddr after: cur=%ld ap=%ld cp=%ld force=%ld\n",
               read_long(DDR_CUR), read_long(DDR_AP), read_long(DDR_CP),
               read_long(DDR_FORCE));
    }

    fflush(stdout);

out_free:
    for (i = 0; i < nthreads; i++) {
        if (args[i].buf && args[i].buf != MAP_FAILED)
            munmap(args[i].buf, args[i].len);
    }
    free(args);
    return 0;
}
