/*
 * cpu_bench.c - deterministic integer CPU benchmark for the SM-A032F.
 *
 * One source, two builds:
 *   -DDEFAULT_MODE='"single"'  -> single-core, pinned to one big core by default
 *   -DDEFAULT_MODE='"all"'     -> one thread per online core
 *
 * The workload is a fully serial 32-bit integer chain (6 multiplies per
 * iteration). It cannot be vectorized or evaluated in closed form, so the
 * measured time is real CPU execution time.
 *
 * Build (on the phone, Termux):
 *   clang -O2 -pthread -o cpu_bench_single cpu_bench.c -DDEFAULT_MODE='"single"'
 *   clang -O2 -pthread -o cpu_bench_all    cpu_bench.c -DDEFAULT_MODE='"all"'
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
#include <sys/resource.h>

#ifndef DEFAULT_MODE
#define DEFAULT_MODE "single"
#endif

#define MAX_THREADS 64
#define MAX_CPUS 8
#define MAX_OPP 16

#define TEMP_PATH "/sys/class/thermal/thermal_zone1/temp"
#define VDD_PATH  "/sys/class/regulator/regulator.1/microvolts"

struct thread_arg {
    int tid;
    int cpu;                 /* -1 = do not pin */
    uint64_t iters;          /* requested iterations (fixed-work mode) */
    uint64_t done;           /* iterations actually completed */
    uint64_t checksum;
    double wall_s;
    double cpu_s;
    pthread_barrier_t *barrier;
};

static int g_mode_all;
static int g_nthreads = 1;
static int g_pin_cpu = -1;
static int g_no_pin;
static uint64_t g_iters = 100000000ULL;
static double g_duration = 0.0;
static int g_little_max_khz = 1600000;   /* policy0 */
static int g_big_max_khz = 2000000;      /* policy4 */

static double now_s(clockid_t id)
{
    struct timespec ts;
    if (clock_gettime(id, &ts) != 0)
        return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static uint32_t burn(uint32_t seed, uint64_t iters)
{
    uint32_t a = seed | 1u;
    uint32_t b = seed ^ 0x9e3779b9u;
    uint32_t c = seed + 0x85ebca6bu;

    for (uint64_t i = 0; i < iters; i++) {
        a = mix32(a + (uint32_t)i);
        b = mix32(b + a);
        c = mix32(c + b);
        a ^= c;
    }
    return a ^ b ^ c;
}

static int pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

/* ---- frequency residency and thermal instrumentation ---- */

struct freq_stat {
    int ncpu;
    int nfreq[MAX_CPUS];
    unsigned int freq[MAX_CPUS][MAX_OPP];
    unsigned long long ticks[MAX_CPUS][MAX_OPP];
};

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

static int write_text(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, value, strlen(value));
    close(fd);
    return n == (ssize_t)strlen(value) ? 0 : -1;
}

static void boost_clusters(void)
{
    char little[32], big[32];

    snprintf(little, sizeof(little), "%d", g_little_max_khz);
    snprintf(big, sizeof(big), "%d", g_big_max_khz);

    write_text("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor",
               "performance");
    write_text("/sys/devices/system/cpu/cpufreq/policy4/scaling_governor",
               "performance");
    write_text("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq",
               little);
    write_text("/sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq",
               big);
}

static void restore_governors(void)
{
    write_text("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor",
               "schedutil");
    write_text("/sys/devices/system/cpu/cpufreq/policy4/scaling_governor",
               "schedutil");
}

static long read_long(const char *path)
{
    char buf[64];
    if (read_text(path, buf, sizeof(buf)) != 0)
        return -1;
    return atol(buf);
}

static int read_procstat(unsigned long long *v)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[512];

    if (!f)
        return -1;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    memset(v, 0, 8 * sizeof(v[0]));
    int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    return n >= 4 ? 0 : -1;
}

static void print_procstat_delta(const unsigned long long *a,
                                 const unsigned long long *b)
{
    unsigned long long d[8];
    unsigned long long total = 0;

    for (int i = 0; i < 8; i++) {
        d[i] = b[i] >= a[i] ? b[i] - a[i] : 0;
        total += d[i];
    }
    if (total == 0)
        return;

    printf("procstat user=%.1f%% nice=%.1f%% sys=%.1f%% idle=%.1f%% "
           "iow=%.1f%% irq=%.1f%% sirq=%.1f%% steal=%.1f%%\n",
           100.0 * d[0] / total, 100.0 * d[1] / total, 100.0 * d[2] / total,
           100.0 * d[3] / total, 100.0 * d[4] / total, 100.0 * d[5] / total,
           100.0 * d[6] / total, 100.0 * d[7] / total);
}

static void read_tis(struct freq_stat *s)
{
    memset(s, 0, sizeof(*s));
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > MAX_CPUS)
        ncpu = MAX_CPUS;
    s->ncpu = ncpu;

    for (int c = 0; c < ncpu; c++) {
        char path[160];
        char buf[4096];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/stats/time_in_state", c);
        if (read_text(path, buf, sizeof(buf)) != 0)
            continue;

        const char *p = buf;
        while (*p && s->nfreq[c] < MAX_OPP) {
            unsigned int f;
            unsigned long long t;
            int used = 0;
            if (sscanf(p, "%u %llu%n", &f, &t, &used) != 2)
                break;
            s->freq[c][s->nfreq[c]] = f;
            s->ticks[c][s->nfreq[c]] = t;
            s->nfreq[c]++;
            p += used;
        }
    }
}

static void print_freq_delta(const struct freq_stat *a, const struct freq_stat *b,
                             const int *active)
{
    double little_sum = 0.0, big_sum = 0.0;
    int little_n = 0, big_n = 0;

    for (int c = 0; c < a->ncpu && c < b->ncpu; c++) {
        unsigned long long delta[MAX_OPP];
        unsigned long long total = 0;
        double weighted = 0.0;

        memset(delta, 0, sizeof(delta));
        for (int i = 0; i < a->nfreq[c]; i++) {
            unsigned long long after = a->ticks[c][i];
            for (int j = 0; j < b->nfreq[c]; j++) {
                if (b->freq[c][j] == a->freq[c][i]) {
                    after = b->ticks[c][j];
                    break;
                }
            }
            if (after > a->ticks[c][i]) {
                delta[i] = after - a->ticks[c][i];
                total += delta[i];
                weighted += (double)a->freq[c][i] * (double)delta[i];
            }
        }

        if (total == 0)
            continue;

        double avg_mhz = weighted / (double)total / 1000.0;
        printf("freq cpu%d avg_mhz=%.1f", c, avg_mhz);

        /* Up to three most-used OPPs. */
        for (int rank = 0; rank < 3; rank++) {
            unsigned long long best = 0;
            int best_i = -1;
            for (int i = 0; i < a->nfreq[c]; i++) {
                if (delta[i] > best) {
                    best = delta[i];
                    best_i = i;
                }
            }
            if (best_i < 0 || best == 0)
                break;
            printf(" %u:%.0f%%", a->freq[c][best_i],
                   100.0 * (double)best / (double)total);
            delta[best_i] = 0;
        }
        printf("\n");

        if (c < 4) {
            if (active[c]) {
                little_sum += avg_mhz;
                little_n++;
            }
        } else {
            if (active[c]) {
                big_sum += avg_mhz;
                big_n++;
            }
        }
    }

    if (little_n)
        printf("freq_cluster little_avg_mhz=%.1f\n", little_sum / little_n);
    if (big_n)
        printf("freq_cluster big_avg_mhz=%.1f\n", big_sum / big_n);
}

static void *thread_main(void *p)
{
    struct thread_arg *t = (struct thread_arg *)p;

    if (t->cpu >= 0 && !g_no_pin) {
        if (pin_to_cpu(t->cpu) != 0) {
            fprintf(stderr, "thread %d: pin to cpu %d failed: %s\n",
                    t->tid, t->cpu, strerror(errno));
        }
    }

    /* All threads wait here so the timed region starts together. */
    pthread_barrier_wait(t->barrier);

    double w0 = now_s(CLOCK_MONOTONIC);
    double c0 = now_s(CLOCK_THREAD_CPUTIME_ID);
    uint32_t seed = 0x12345678u + (uint32_t)t->tid * 0x9e3779b9u;
    uint32_t sum;

    if (g_duration > 0.0) {
        const uint64_t chunk = 1000000ULL;
        double end = w0 + g_duration;
        uint64_t done = 0;
        sum = seed | 1u;
        while (now_s(CLOCK_MONOTONIC) < end) {
            sum = burn(sum, chunk);
            done += chunk;
        }
        t->done = done;
    } else {
        sum = burn(seed, t->iters);
        t->done = t->iters;
    }

    double c1 = now_s(CLOCK_THREAD_CPUTIME_ID);
    double w1 = now_s(CLOCK_MONOTONIC);

    t->checksum = sum;
    t->wall_s = w1 - w0;
    t->cpu_s = c1 - c0;
    return NULL;
}

static int parse_args(int argc, char **argv)
{
    const char *mode = DEFAULT_MODE;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            mode = argv[++i];
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            g_iters = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
            g_duration = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            g_nthreads = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) {
            g_pin_cpu = atoi(argv[++i]);
            g_no_pin = 0;
        } else if (!strcmp(argv[i], "--no-pin")) {
            g_no_pin = 1;
        } else if (!strcmp(argv[i], "--little-max") && i + 1 < argc) {
            g_little_max_khz = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--big-max") && i + 1 < argc) {
            g_big_max_khz = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: %s [--mode single|all] [--iters N] [--duration S]\n"
                   "          [--threads N] [--cpu N] [--no-pin]\n"
                   "          [--little-max KHZ] [--big-max KHZ]\n", argv[0]);
            return 1;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (!strcmp(mode, "all")) {
        g_mode_all = 1;
        if (g_nthreads <= 1)
            g_nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    } else if (!strcmp(mode, "single")) {
        g_mode_all = 0;
        g_nthreads = 1;
        if (g_pin_cpu < 0)
            g_pin_cpu = 4;       /* first big core on the SM-A032F */
    } else {
        fprintf(stderr, "invalid mode '%s' (use single or all)\n", mode);
        return -1;
    }

    if (g_nthreads < 1 || g_nthreads > MAX_THREADS) {
        fprintf(stderr, "threads must be 1..%d\n", MAX_THREADS);
        return -1;
    }
    if (g_iters < 1000 && g_duration <= 0.0) {
        fprintf(stderr, "iters must be >= 1000 (or use --duration S)\n");
        return -1;
    }
    if (g_duration < 0.0) {
        fprintf(stderr, "duration must be > 0\n");
        return -1;
    }
    if (g_little_max_khz <= 0 || g_big_max_khz <= 0) {
        fprintf(stderr, "frequency caps must be positive kHz values\n");
        return -1;
    }

    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (g_pin_cpu >= online) {
        fprintf(stderr, "cpu %d is not online (online=%ld)\n", g_pin_cpu, online);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int rc = parse_args(argc, argv);
    if (rc != 0)
        return rc > 0 ? 0 : 1;

    if (setpriority(PRIO_PROCESS, 0, -20) != 0)
        fprintf(stderr, "note: setpriority(-20) failed: %s\n", strerror(errno));

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, (unsigned)g_nthreads + 1);

    pthread_t threads[MAX_THREADS];
    struct thread_arg args[MAX_THREADS];

    for (int i = 0; i < g_nthreads; i++) {
        args[i].tid = i;
        args[i].cpu = g_no_pin ? -1 : (g_mode_all ? i : g_pin_cpu);
        args[i].iters = g_iters;
        args[i].done = 0;
        args[i].checksum = 0;
        args[i].wall_s = 0;
        args[i].cpu_s = 0;
        args[i].barrier = &barrier;
    }

    int active[MAX_CPUS];
    memset(active, 0, sizeof(active));
    if (g_no_pin) {
        for (int c = 0; c < MAX_CPUS; c++)
            active[c] = 1;
    } else {
        for (int i = 0; i < g_nthreads; i++) {
            if (args[i].cpu >= 0 && args[i].cpu < MAX_CPUS)
                active[args[i].cpu] = 1;
        }
    }

    for (int i = 0; i < g_nthreads; i++) {
        int err = pthread_create(&threads[i], NULL, thread_main, &args[i]);
        if (err != 0) {
            fprintf(stderr, "pthread_create %d: %s\n", i, strerror(err));
            return 1;
        }
    }

    boost_clusters();
    usleep(100000);   /* let the new caps settle before sampling */

    struct freq_stat fs_before, fs_after;
    unsigned long long ps_before[8], ps_after[8];
    long temp0 = read_long(TEMP_PATH);
    long vdd0 = read_long(VDD_PATH);
    read_procstat(ps_before);
    read_tis(&fs_before);

    /* Release the starting gate, then time only the benchmark region. */
    pthread_barrier_wait(&barrier);
    double w0 = now_s(CLOCK_MONOTONIC);
    double p0 = now_s(CLOCK_PROCESS_CPUTIME_ID);

    for (int i = 0; i < g_nthreads; i++)
        pthread_join(threads[i], NULL);

    read_tis(&fs_after);
    read_procstat(ps_after);
    long temp1 = read_long(TEMP_PATH);
    long vdd1 = read_long(VDD_PATH);

    double p1 = now_s(CLOCK_PROCESS_CPUTIME_ID);
    double w1 = now_s(CLOCK_MONOTONIC);

    uint64_t checksum = 0;
    uint64_t total_done = 0;
    for (int i = 0; i < g_nthreads; i++) {
        checksum ^= args[i].checksum << (i & 15);
        total_done += args[i].done;
    }

    double wall = w1 - w0;
    double cpu = p1 - p0;
    double total_iters = (double)total_done;
    double mips = total_iters / wall / 1e6;

    printf("mode=%s threads=%d duration_s=%.3f requested_iters=%llu total_iters=%.0f\n",
           g_mode_all ? "all" : "single", g_nthreads, g_duration,
           (unsigned long long)g_iters, total_iters);
    printf("freq_cap little_max_khz=%d big_max_khz=%d pin_cpu=%d\n",
           g_little_max_khz, g_big_max_khz, g_pin_cpu);
    printf("wall_s=%.6f process_cpu_s=%.6f throughput_mips=%.3f\n",
           wall, cpu, mips);
    printf("checksum=%08llx\n", (unsigned long long)checksum);
    printf("pin_map:");
    for (int i = 0; i < g_nthreads; i++)
        printf(" t%d=cpu%d", i, args[i].cpu);
    printf("\n");

    for (int i = 0; i < g_nthreads; i++) {
        printf("thread%d cpu=%d iters=%llu wall_s=%.6f cpu_s=%.6f mips=%.3f checksum=%08llx\n",
               i, args[i].cpu, (unsigned long long)args[i].done,
               args[i].wall_s, args[i].cpu_s,
               (double)args[i].done / args[i].wall_s / 1e6,
               (unsigned long long)args[i].checksum);
    }

    printf("temp_millic before=%ld after=%ld\n", temp0, temp1);
    printf("vddcpu_uv before=%ld after=%ld\n", vdd0, vdd1);
    print_procstat_delta(ps_before, ps_after);
    print_freq_delta(&fs_before, &fs_after, active);

    printf("RESULT mode=%s threads=%d wall_s=%.6f total_iters=%.0f throughput_mips=%.3f checksum=%08llx\n",
           g_mode_all ? "all" : "single", g_nthreads, wall, total_iters,
           mips, (unsigned long long)checksum);

    pthread_barrier_destroy(&barrier);
    restore_governors();
    return 0;
}
