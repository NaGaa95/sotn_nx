/* libc_shim.c -- bionic<->newlib libc wrappers for libsotn.so. Converting
 * wrappers where the ABIs differ; matches are forwarded from imports.c.
 * MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "libc_shim.h"

// fortify (_chk): ignore the object-size argument
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memcpy(dst, src, n);
}
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memmove(dst, src, n);
}
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen; return strcpy(dst, src);
}
size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen; return strlen(s);
}
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va);
}
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsprintf(s, fmt, va);
}
ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) {
  (void)buflen; return read(fd, buf, count);
}

static int gettid_fake(void) {
  u64 id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&id, CUR_THREAD_HANDLE)) && id)
    return (int)(id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178

long syscall_fake(long number, ...) {
  if (number == ARM64_SYS_GETTID) return gettid_fake();
  errno = ENOSYS;
  return -1;
}

void sincos_fake(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

// bionic clockids (REALTIME=0, MONOTONIC=1, ...) differ from newlib's, so the
// engine's clock_gettime(0) was rejected as EINVAL -> uncaught std::system_error.
// Translate the id; fall back to the libnx tick so it never fails. bionic and
// newlib timespec match on arm64 LP64.
int clock_gettime_fake(int clk, void *ts_) {
  struct timespec *ts = ts_;
  if (!ts) { errno = EFAULT; return -1; }
  clockid_t real = (clk == 0 || clk == 5) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
  if (clock_gettime(real, ts) == 0)
    return 0;
  uint64_t ns = armTicksToNs(armGetSystemTick());
  ts->tv_sec = (int64_t)(ns / 1000000000ull);
  ts->tv_nsec = (int64_t)(ns % 1000000000ull);
  return 0;
}

void android_set_abort_message_fake(const char *msg) { (void)msg; }

size_t __ctype_get_mb_cur_max_fake(void) { return 1; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    case BIONIC_SC_PHYS_PAGES: return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default: return -1;
  }
}

// signals: the engine installs a crash handler we never trigger; stub them
int sigaction_fake(int sig, const void *act, void *oldact) {
  (void)sig; (void)act; (void)oldact; return 0;
}
void *signal_fake(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
int sigaddset_fake(void *set, int sig) { (void)set; (void)sig; return 0; }
int sigemptyset_fake(void *set) { (void)set; return 0; }
int pthread_sigmask_fake(int how, const void *set, void *oldset) {
  (void)how; (void)set; (void)oldset; return 0;
}

// struct stat conversion (bionic aarch64 layout)
struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, __pad1;
  int64_t st_size;
  int32_t st_blksize, __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim, st_mtim, st_ctim;
  uint32_t __unused4, __unused5;
};

int stat_fake(const char *path, void *st) {
  struct stat in;
  if (stat(path, &in) != 0) return -1;
  struct bionic_stat *out = st;
  memset(out, 0, sizeof(*out));
  out->st_dev = in.st_dev; out->st_ino = in.st_ino;
  out->st_mode = in.st_mode; out->st_nlink = in.st_nlink;
  out->st_uid = in.st_uid; out->st_gid = in.st_gid;
  out->st_rdev = in.st_rdev; out->st_size = in.st_size;
  out->st_blksize = in.st_blksize; out->st_blocks = in.st_blocks;
  out->st_atim.tv_sec = in.st_atime;
  out->st_mtim.tv_sec = in.st_mtime;
  out->st_ctim.tv_sec = in.st_ctime;
  return 0;
}

// locale: ignore the locale argument, use the C versions
void *newlocale_fake(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base; return (void *)1;
}
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

long double strtold_l_fake(const char *s, char **end, void *loc) {
  (void)loc; return strtold(s, end);
}
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc; return strtoll(s, end, base);
}
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc; return strtoull(s, end, base);
}

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

// stdio over the fake bionic __sF: libc++/SDL bind std streams to &__sF[N];
// these absorb accesses to those fake FILEs and forward everything else.
uint8_t fake_sF[3][0x100];

static int is_fake_file(const void *f) {
  const uint8_t *p = f, *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (!f || !ptr) return 0;
  if (is_fake_file(f)) return n;
  return fwrite(ptr, size, n, f);
}

// NULL-safe: the engine fread()s into a malloc'd buffer without checking the
// FILE* or the buffer, so guard against a write-to-0x0 Data Abort.
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (!f || !ptr || is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
  if (!f) return -1;
  if (is_fake_file(f)) return c;
  return fputc(c, f);
}
int fflush_fake(FILE *f) {
  if (!f || is_fake_file(f)) return 0;
  return fflush(f);
}
int fclose_fake(FILE *f) {
  if (!f || is_fake_file(f)) return 0;
  return fclose(f);
}
int ferror_fake(FILE *f) {
  if (!f || is_fake_file(f)) return 0;
  return ferror(f);
}
int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f)) return 0;
  va_list va; va_start(va, fmt);
  int ret = vfprintf(f, fmt, va);
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) return 0;
  return vfprintf(f, fmt, va);
}
int fseek_fake(FILE *f, long off, int whence) {
  if (!f || is_fake_file(f)) return -1;
  return fseek(f, off, whence);
}
int putchar_fake(int c) { return c; }
int puts_fake(const char *s) { (void)s; return 0; }

// Some data lives in a region/language subfolder (pspbin/eu/, pack/jp/,
// sound/xa/en/) the engine sometimes omits; retry with each inserted.
static FILE *fopen_region_fallback(const char *path, const char *mode) {
  const char *slash = strrchr(path, '/');
  if (!slash || slash == path) return NULL;
  static const char *regions[] = { "eu", "us", "jp", "en" };
  char cand[640];
  const int dirlen = (int)(slash - path);
  for (unsigned i = 0; i < sizeof(regions) / sizeof(*regions); i++) {
    snprintf(cand, sizeof(cand), "%.*s/%s/%s", dirlen, path, regions[i], slash + 1);
    FILE *f = fopen(cand, mode);
    if (f) return f;
  }
  return NULL;
}

// large stream buffer: the engine issues many small reads/seeks against the
// game archives and fsdev round trips dominate otherwise.
FILE *fopen_fake(const char *path, const char *mode) {
  if (!path) return NULL;
  FILE *f = fopen(path, mode);
  if (!f && strchr(mode, 'r'))
    f = fopen_region_fallback(path, mode);
  if (f && strchr(mode, 'r'))
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
  return f;
}

// ANativeWindow -> NWindow: hand SDL the real Switch window so mesa's
// eglCreateWindowSurface lands on it.
void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env; (void)surface;
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  return win;
}
void ANativeWindow_release_fake(void *win) { (void)win; }
int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
  (void)format;
  if (w > 0 && h > 0) nwindowSetDimensions((NWindow *)win, w, h);
  return 0;
}

// POSIX semaphores via pointer indirection (bionic sem_t is 16 bytes on LP64,
// so a heap FakeSem* fits in the caller's storage)
typedef struct { Semaphore sem; } FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  if (!fs) return -1;
  semaphoreInit(&fs->sem, value);
  *s = fs;
  return 0;
}
int sem_destroy_fake(void **s) {
  if (s && *s) { free(*s); *s = NULL; }
  return 0;
}
int sem_post_fake(void **s) {
  if (s && *s) semaphoreSignal(&((FakeSem *)*s)->sem);
  return 0;
}
int sem_wait_fake(void **s) {
  if (s && *s) semaphoreWait(&((FakeSem *)*s)->sem);
  return 0;
}
int sem_trywait_fake(void **s) {
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem)) return 0;
  errno = EAGAIN;
  return -1;
}
int sem_getvalue_fake(void **s, int *val) {
  *val = (s && *s) ? (int)((FakeSem *)*s)->sem.count : 0;
  return 0;
}
