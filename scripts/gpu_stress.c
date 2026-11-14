/*
 * gpu_stress.c - PowerVR/GLES2 offscreen GPU stress + frequency sampler
 * for the SM-A032F (Unisoc SC9863A, PowerVR Rogue GE8322).
 *
 * Creates an EGL pbuffer + GLES2 context, then draws a full-screen quad
 * with a heavy fragment shader (iterative sin/cos/length loop) for a
 * fixed duration. A sampler thread records the GPU devfreq frequency,
 * GPU/CPU temperature, regulator voltages, CPU frequency and DDR
 * frequency every 100 ms.
 *
 * Read-only with respect to the phone: it only reads sysfs/procfs and
 * renders to its own pbuffer. It never writes devfreq/regulator sysfs.
 *
 * Build (on the phone, Termux):
 *   clang -O2 -pthread -o gpu_stress gpu_stress.c -ldl
 *
 * Run:
 *   ./gpu_stress -d 15 -s 1024 -i 64
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------- EGL / GLES2 minimal definitions ---------- */

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef unsigned int EGLBoolean;
typedef int EGLint;

#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_TRUE 1

#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_TRIANGLE_STRIP 0x0005
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

typedef EGLDisplay (*PFN_eglGetDisplay)(void *);
typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLSurface (*PFN_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*PFN_eglTerminate)(EGLDisplay);
typedef const char *(*PFN_glGetString)(unsigned int);
typedef unsigned int (*PFN_glCreateShader)(unsigned int);
typedef void (*PFN_glShaderSource)(unsigned int, int, const char *const *, const int *);
typedef void (*PFN_glCompileShader)(unsigned int);
typedef void (*PFN_glGetShaderiv)(unsigned int, unsigned int, int *);
typedef void (*PFN_glGetShaderInfoLog)(unsigned int, int, int *, char *);
typedef unsigned int (*PFN_glCreateProgram)(void);
typedef void (*PFN_glAttachShader)(unsigned int, unsigned int);
typedef void (*PFN_glLinkProgram)(unsigned int);
typedef void (*PFN_glGetProgramiv)(unsigned int, unsigned int, int *);
typedef void (*PFN_glGetProgramInfoLog)(unsigned int, int, int *, char *);
typedef void (*PFN_glUseProgram)(unsigned int);
typedef int (*PFN_glGetUniformLocation)(unsigned int, const char *);
typedef int (*PFN_glGetAttribLocation)(unsigned int, const char *);
typedef void (*PFN_glUniform1f)(int, float);
typedef void (*PFN_glGenBuffers)(int, unsigned int *);
typedef void (*PFN_glBindBuffer)(unsigned int, unsigned int);
typedef void (*PFN_glBufferData)(unsigned int, long, const void *, unsigned int);
typedef void (*PFN_glEnableVertexAttribArray)(unsigned int);
typedef void (*PFN_glVertexAttribPointer)(unsigned int, int, unsigned int, unsigned char, int, const void *);
typedef void (*PFN_glDrawArrays)(unsigned int, int, int);
typedef void (*PFN_glFinish)(void);
typedef void (*PFN_glViewport)(int, int, int, int);
typedef void (*PFN_glClearColor)(float, float, float, float);
typedef void (*PFN_glClear)(unsigned int);
typedef unsigned int (*PFN_glGetError)(void);

static PFN_eglGetDisplay p_eglGetDisplay;
static PFN_eglInitialize p_eglInitialize;
static PFN_eglChooseConfig p_eglChooseConfig;
static PFN_eglCreatePbufferSurface p_eglCreatePbufferSurface;
static PFN_eglCreateContext p_eglCreateContext;
static PFN_eglMakeCurrent p_eglMakeCurrent;
static PFN_eglTerminate p_eglTerminate;
static PFN_glGetString p_glGetString;
static PFN_glCreateShader p_glCreateShader;
static PFN_glShaderSource p_glShaderSource;
static PFN_glCompileShader p_glCompileShader;
static PFN_glGetShaderiv p_glGetShaderiv;
static PFN_glGetShaderInfoLog p_glGetShaderInfoLog;
static PFN_glCreateProgram p_glCreateProgram;
static PFN_glAttachShader p_glAttachShader;
static PFN_glLinkProgram p_glLinkProgram;
static PFN_glGetProgramiv p_glGetProgramiv;
static PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
static PFN_glUseProgram p_glUseProgram;
static PFN_glGetUniformLocation p_glGetUniformLocation;
static PFN_glGetAttribLocation p_glGetAttribLocation;
static PFN_glUniform1f p_glUniform1f;
static PFN_glGenBuffers p_glGenBuffers;
static PFN_glBindBuffer p_glBindBuffer;
static PFN_glBufferData p_glBufferData;
static PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;
static PFN_glVertexAttribPointer p_glVertexAttribPointer;
static PFN_glDrawArrays p_glDrawArrays;
static PFN_glFinish p_glFinish;
static PFN_glViewport p_glViewport;
static PFN_glClearColor p_glClearColor;
static PFN_glClear p_glClear;
static PFN_glGetError p_glGetError;

/* ---------- configuration ---------- */

#define GPU_FREQ "/sys/class/devfreq/60000000.gpu/cur_freq"
#define GPU_GOV  "/sys/class/devfreq/60000000.gpu/governor"
#define GPU_MIN  "/sys/class/devfreq/60000000.gpu/min_freq"
#define GPU_MAX  "/sys/class/devfreq/60000000.gpu/max_freq"
#define GPU_AVAIL "/sys/class/devfreq/60000000.gpu/available_frequencies"
#define DDR_FREQ "/sys/class/devfreq/scene-frequency/sprd-governor/ddrinfo_cur_freq"
#define CPU0_FREQ "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"
#define CPU4_FREQ "/sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq"
#define TEMP_CPU "/sys/class/thermal/thermal_zone1/temp"
#define TEMP_GPU "/sys/class/thermal/thermal_zone2/temp"

#define MAX_SAMPLES 8192
#define MAX_REGS 32

struct sample {
    double t;
    long gpu;
    long ddr;
    long cpu0;
    long cpu4;
    long temp_cpu;
    long temp_gpu;
    long vcore;
    long vmem;
};

static volatile int g_stop;
static struct sample g_samples[MAX_SAMPLES];
static int g_nsamples;
static double g_t0;

static char g_vcore_path[128];
static char g_vmem_path[128];

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, size - 1);
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
    v = strtol(buf, &end, 10);
    if (end == buf)
        return -1;
    return v;
}

static void find_regulator(const char *name, char *out, size_t outlen)
{
    int i;

    out[0] = '\0';
    for (i = 0; i < MAX_REGS; i++) {
        char dir[128], npath[160], vpath[160], nbuf[64];

        snprintf(dir, sizeof(dir), "/sys/class/regulator/regulator.%d", i);
        snprintf(npath, sizeof(npath), "%s/name", dir);
        if (read_text(npath, nbuf, sizeof(nbuf)) != 0)
            continue;
        nbuf[strcspn(nbuf, "\n")] = '\0';
        if (strcmp(nbuf, name) == 0) {
            snprintf(vpath, sizeof(vpath), "%s/microvolts", dir);
            snprintf(out, outlen, "%s", vpath);
            return;
        }
    }
}

static long read_reg(const char *path)
{
    if (!path[0])
        return -1;
    return read_long(path);
}

static void *sampler_thread(void *arg)
{
    (void)arg;
    while (!g_stop) {
        struct sample s;
        struct timespec ts = { 0, 100 * 1000 * 1000 };

        s.t = now_s() - g_t0;
        s.gpu = read_long(GPU_FREQ);
        s.ddr = read_long(DDR_FREQ);
        s.cpu0 = read_long(CPU0_FREQ);
        s.cpu4 = read_long(CPU4_FREQ);
        s.temp_cpu = read_long(TEMP_CPU);
        s.temp_gpu = read_long(TEMP_GPU);
        s.vcore = read_reg(g_vcore_path);
        s.vmem = read_reg(g_vmem_path);

        if (g_nsamples < MAX_SAMPLES)
            g_samples[g_nsamples++] = s;

        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void *sym(void *lib, const char *name)
{
    void *p = dlsym(lib, name);
    if (!p)
        fprintf(stderr, "dlsym %s failed: %s\n", name, dlerror());
    return p;
}

static unsigned int compile_shader(unsigned int type, const char *src)
{
    unsigned int sh = p_glCreateShader(type);
    int ok = 0, loglen = 0;
    char *log;

    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    p_glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &loglen);
    if (!ok) {
        log = calloc(1, (size_t)(loglen > 1 ? loglen : 1));
        p_glGetShaderInfoLog(sh, loglen, NULL, log);
        fprintf(stderr, "shader compile failed: %s\n", log ? log : "?");
        free(log);
        return 0;
    }
    return sh;
}

static unsigned int make_program(int iterations)
{
    static const char *vs =
        "attribute vec2 aPos;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "  vUV = aPos * 0.5 + 0.5;\n"
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
    char fs[2048];
    unsigned int vsh, fsh, prog;
    int ok = 0, loglen = 0;
    char *log;

    snprintf(fs, sizeof(fs),
        "precision highp float;\n"
        "varying vec2 vUV;\n"
        "uniform float uSeed;\n"
        "void main() {\n"
        "  vec2 p = vUV * 2.0 - 1.0;\n"
        "  float acc = 0.0;\n"
        "  for (int i = 0; i < %d; i++) {\n"
        "    float fi = float(i);\n"
        "    p.x += sin(p.y * fi + uSeed);\n"
        "    p.y += cos(p.x * fi - uSeed);\n"
        "    acc += length(p);\n"
        "  }\n"
        "  gl_FragColor = vec4(fract(acc * 0.001), p * 0.5 + 0.5, 1.0);\n"
        "}\n", iterations);

    vsh = compile_shader(GL_VERTEX_SHADER, vs);
    fsh = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!vsh || !fsh)
        return 0;

    prog = p_glCreateProgram();
    p_glAttachShader(prog, vsh);
    p_glAttachShader(prog, fsh);
    p_glLinkProgram(prog);
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    p_glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &loglen);
    if (!ok) {
        log = calloc(1, (size_t)(loglen > 1 ? loglen : 1));
        p_glGetProgramInfoLog(prog, loglen, NULL, log);
        fprintf(stderr, "program link failed: %s\n", log ? log : "?");
        free(log);
        return 0;
    }
    return prog;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-d seconds] [-s size] [-i iterations] [-b baseline]\n", argv0);
}

int main(int argc, char **argv)
{
    int duration = 15;
    int size = 1024;
    int iterations = 64;
    int baseline = 2;
    int opt;
    void *egl_lib, *gles_lib;
    EGLDisplay dpy;
    EGLConfig cfg = NULL;
    EGLint ncfg = 0, major = 0, minor = 0;
    EGLSurface surf;
    EGLContext ctx;
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_NONE
    };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLint pb_attr[] = { EGL_WIDTH, 0, EGL_HEIGHT, 0, EGL_NONE };
    unsigned int prog, vbo;
    int loc_seed, loc_pos;
    float verts[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    pthread_t sampler;
    double start, last_print;
    unsigned long frames = 0;
    double t_load_start, t_load_end;
    int i;

    while ((opt = getopt(argc, argv, "d:s:i:b:")) != -1) {
        switch (opt) {
        case 'd': duration = atoi(optarg); break;
        case 's': size = atoi(optarg); break;
        case 'i': iterations = atoi(optarg); break;
        case 'b': baseline = atoi(optarg); break;
        default: usage(argv[0]); return 1;
        }
    }
    if (duration < 1) duration = 1;
    if (size < 64) size = 64;
    if (size > 4096) size = 4096;
    if (iterations < 1) iterations = 1;
    if (iterations > 4096) iterations = 4096;
    if (baseline < 0) baseline = 0;

    pb_attr[1] = size;
    pb_attr[3] = size;

    egl_lib = dlopen("libEGL.so", RTLD_NOW);
    gles_lib = dlopen("libGLESv2.so", RTLD_NOW);
    if (!egl_lib || !gles_lib) {
        fprintf(stderr, "dlopen EGL/GLES failed: %s\n", dlerror());
        return 1;
    }

#define LOAD(fn) p_##fn = (__typeof__(p_##fn))sym(egl_lib, #fn)
    LOAD(eglGetDisplay);
    LOAD(eglInitialize);
    LOAD(eglChooseConfig);
    LOAD(eglCreatePbufferSurface);
    LOAD(eglCreateContext);
    LOAD(eglMakeCurrent);
    LOAD(eglTerminate);
#undef LOAD
#define LOADG(fn) p_##fn = (__typeof__(p_##fn))sym(gles_lib, #fn)
    LOADG(glGetString);
    LOADG(glCreateShader);
    LOADG(glShaderSource);
    LOADG(glCompileShader);
    LOADG(glGetShaderiv);
    LOADG(glGetShaderInfoLog);
    LOADG(glCreateProgram);
    LOADG(glAttachShader);
    LOADG(glLinkProgram);
    LOADG(glGetProgramiv);
    LOADG(glGetProgramInfoLog);
    LOADG(glUseProgram);
    LOADG(glGetUniformLocation);
    LOADG(glGetAttribLocation);
    LOADG(glUniform1f);
    LOADG(glGenBuffers);
    LOADG(glBindBuffer);
    LOADG(glBufferData);
    LOADG(glEnableVertexAttribArray);
    LOADG(glVertexAttribPointer);
    LOADG(glDrawArrays);
    LOADG(glFinish);
    LOADG(glViewport);
    LOADG(glClearColor);
    LOADG(glClear);
    LOADG(glGetError);
#undef LOADG

    dpy = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !p_eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "EGL init failed\n");
        return 1;
    }
    if (!p_eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "eglChooseConfig failed\n");
        return 1;
    }
    surf = p_eglCreatePbufferSurface(dpy, cfg, pb_attr);
    if (surf == EGL_NO_SURFACE) {
        fprintf(stderr, "pbuffer create failed\n");
        return 1;
    }
    ctx = p_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !p_eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "context create/current failed\n");
        return 1;
    }

    printf("GPU: %s | %s\n", p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION));
    printf("EGL %d.%d | pbuffer %dx%d | iterations %d | %d s load\n",
           major, minor, size, size, iterations, duration);
    printf("gpu before: freq=%ld governor=%s min=%ld max=%ld avail=%s\n",
           read_long(GPU_FREQ), ({ static char b[64]; read_text(GPU_GOV, b, sizeof(b)) ? "?" : b; }),
           read_long(GPU_MIN), read_long(GPU_MAX),
           ({ static char b[128]; read_text(GPU_AVAIL, b, sizeof(b)) ? "?" : b; }));

    find_regulator("vddcore", g_vcore_path, sizeof(g_vcore_path));
    find_regulator("vddmem", g_vmem_path, sizeof(g_vmem_path));
    printf("regulators: vddcore=%s vddmem=%s\n",
           g_vcore_path[0] ? g_vcore_path : "?",
           g_vmem_path[0] ? g_vmem_path : "?");

    prog = make_program(iterations);
    if (!prog)
        return 1;
    p_glUseProgram(prog);
    loc_seed = p_glGetUniformLocation(prog, "uSeed");
    loc_pos = p_glGetAttribLocation(prog, "aPos");
    if (loc_pos < 0) {
        fprintf(stderr, "aPos attribute not found\n");
        return 1;
    }

    p_glGenBuffers(1, &vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray((unsigned int)loc_pos);
    p_glVertexAttribPointer((unsigned int)loc_pos, 2, GL_FLOAT, 0, 0, 0);

    g_t0 = now_s();
    g_stop = 0;
    g_nsamples = 0;
    pthread_create(&sampler, NULL, sampler_thread, NULL);

    if (baseline > 0) {
        struct timespec ts = { baseline, 0 };
        printf("baseline %d s...\n", baseline);
        nanosleep(&ts, NULL);
    }

    start = now_s();
    last_print = start;
    t_load_start = start;
    printf("load start\n");
    while (1) {
        double t = now_s();

        if (t - start >= duration)
            break;

        p_glViewport(0, 0, size, size);
        p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT);
        if (loc_seed >= 0)
            p_glUniform1f(loc_seed, (float)(t - start));
        p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        p_glFinish();
        frames++;

        if (t - last_print >= 1.0) {
            printf("t=%5.1f frames=%lu fps=%.1f gpu=%ld temp_gpu=%.1f temp_cpu=%.1f vcore=%ld\n",
                   t - t_load_start, frames, (double)frames / (t - start),
                   read_long(GPU_FREQ), read_long(TEMP_GPU) / 1000.0,
                   read_long(TEMP_CPU) / 1000.0, read_reg(g_vcore_path));
            last_print = t;
        }
    }
    t_load_end = now_s();
    printf("load end: %.1f s, %lu frames, %.2f fps\n",
           t_load_end - t_load_start, frames,
           (double)frames / (t_load_end - t_load_start));

    /* brief cooldown sample */
    {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
    }
    g_stop = 1;
    pthread_join(sampler, NULL);

    {
        long gmin = 1000000000, gmax = -1, gn = 0;
        double gsum = 0.0;
        long tmin = 1000000000, tmax = -1;
        long vmin = 1000000000, vmax = -1;
        long dmin = 1000000000, dmax = -1;

        for (i = 0; i < g_nsamples; i++) {
            struct sample *s = &g_samples[i];
            if (s->gpu >= 0) {
                if (s->gpu < gmin) gmin = s->gpu;
                if (s->gpu > gmax) gmax = s->gpu;
                gsum += s->gpu; gn++;
            }
            if (s->temp_gpu >= 0) {
                if (s->temp_gpu < tmin) tmin = s->temp_gpu;
                if (s->temp_gpu > tmax) tmax = s->temp_gpu;
            }
            if (s->vcore > 0) {
                if (s->vcore < vmin) vmin = s->vcore;
                if (s->vcore > vmax) vmax = s->vcore;
            }
            if (s->ddr >= 0) {
                if (s->ddr < dmin) dmin = s->ddr;
                if (s->ddr > dmax) dmax = s->ddr;
            }
        }

        printf("\n===== SUMMARY =====\n");
        printf("samples=%d\n", g_nsamples);
        printf("gpu freq: min=%ld max=%ld avg=%.1f MHz\n", gmin, gmax,
               gn ? (double)gsum / gn / 1e6 : 0.0);
        printf("gpu temp: min=%.1f max=%.1f C\n", tmin / 1000.0, tmax / 1000.0);
        printf("vddcore: min=%ld max=%ld uV\n", vmin, vmax);
        printf("ddr freq: min=%ld max=%ld MHz\n", dmin, dmax);
        printf("gpu after: freq=%ld governor=%s\n", read_long(GPU_FREQ),
               ({ static char b[64]; read_text(GPU_GOV, b, sizeof(b)) ? "?" : b; }));
    }

    p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p_eglTerminate(dpy);
    return 0;
}
