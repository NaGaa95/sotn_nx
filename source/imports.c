/* imports.c -- resolves libsotn.so's dynamic imports against newlib, host
 * zlib, mesa GLES, and our shims. MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <locale.h>
#include <setjmp.h>
#include <sys/time.h>
#include <zlib.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "egl_shim.h"
#include "jni_fake.h"
#include "gl_compat.h" // GL prototypes (GLES2 core + GLES1 externs)

// real libc/gcc symbols whose addresses we forward verbatim
extern uintptr_t __cxa_atexit;
extern uintptr_t __stack_chk_fail;

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
#if DEBUG_LOG
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("%s: %s\n", tag, string);
#else
  (void)tag; (void)fmt;
#endif
  return 0;
}

int __android_log_write(int prio, const char *tag, const char *text) {
  (void)prio;
  debugPrintf("%s: %s\n", tag, text);
  return 0;
}

// pthread: bionic's opaque types are zero-inited inline, so lazily back them
// with heap newlib objects stashed in the caller's first pointer slot.

// bionic static recursive/errorcheck mutex init markers (type bits << 14)
#define BIONIC_RECURSIVE_MARK 0x8000
#define BIONIC_ERRCHECK_MARK  0x4000

// newlib and bionic disagree on a handful of errno values; pthread functions
// return these by value, and the engine's libc++ compares them against the
// bionic constants it was built with. Most match; the ones that bite are
// ETIMEDOUT (newlib 116 -> bionic 110, e.g. condition_variable::wait_for) and
// EDEADLK (newlib 45 -> bionic 35).
static int to_bionic_errno(int e) {
  switch (e) {
    case 116: return 110; // ETIMEDOUT
    case 45:  return 35;  // EDEADLK
    default:  return e;
  }
}

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (mutexattr && *mutexattr == 1);
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

static int ensure_mutex(pthread_mutex_t **uid) {
  if (!*uid) return pthread_mutex_init_fake(uid, NULL);
  const uintptr_t v = (uintptr_t)*uid;
  if (v == BIONIC_RECURSIVE_MARK || v == BIONIC_ERRCHECK_MARK) {
    int attr = 1;
    return pthread_mutex_init_fake(uid, &attr);
  }
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x10000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}
int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int r = ensure_mutex(uid); if (r) return r;
  return pthread_mutex_lock(*uid);
}
int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int r = ensure_mutex(uid); if (r) return r;
  return pthread_mutex_trylock(*uid);
}
int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int r = ensure_mutex(uid); if (r) return r;
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}
int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_broadcast(*cnd);
}
int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_signal(*cnd);
}
int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd && (uintptr_t)*cnd > 0x10000) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  if (ensure_mutex(mtx)) return -1;
  return pthread_cond_wait(*cnd, *mtx);
}
int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  if (ensure_mutex(mtx)) return -1;
  return to_bionic_errno(pthread_cond_timedwait(*cnd, *mtx, t));
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }

// new engine threads need tpidr_el0 pointing at a stack-guard block first
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  return ts.entry(ts.arg);
}

int pthread_create_fake(pthread_t *thread, const void *attr, void *entry, void *arg) {
  (void)attr;
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  return pthread_create(thread, NULL, thread_trampoline, ts);
}

// ---------------------------------------------------------------------------
// small misc shims
// ---------------------------------------------------------------------------

static int sched_yield_fake(void) { svcSleepThread(0); return 0; }
static int sched_get_priority_max_fake(int policy) { (void)policy; return 1; }
static int sched_get_priority_min_fake(int policy) { (void)policy; return 0; }
static int ret0_stub(void) { return 0; }

static int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

// POSIX-2008 byte-bounded conversions over mbrtowc/wcrtomb (UTF-8 correct)
static size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, mbstate_t *ps) {
  static mbstate_t local;
  if (!ps) ps = &local;
  const char *s = *src;
  size_t bytes = nms, count = 0;
  while (bytes > 0 && (!dst || count < len)) {
    wchar_t wc;
    size_t r = mbrtowc(dst ? &wc : NULL, s, bytes, ps);
    if (r == (size_t)-1) { errno = EILSEQ; return (size_t)-1; }
    if (r == (size_t)-2) break;            // incomplete multibyte char
    if (r == 0) { if (dst) dst[count] = 0; *src = NULL; return count; }
    if (dst) dst[count] = wc;
    count++; s += r; bytes -= r;
  }
  *src = s;
  return count;
}

static size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, mbstate_t *ps) {
  static mbstate_t local;
  if (!ps) ps = &local;
  const wchar_t *s = *src;
  size_t count = 0;
  char tmp[MB_LEN_MAX];
  for (size_t i = 0; i < nwc; i++) {
    size_t r = wcrtomb(tmp, s[i], ps);
    if (r == (size_t)-1) { errno = EILSEQ; return (size_t)-1; }
    if (dst) {
      if (count + r > len) break;
      memcpy(dst + count, tmp, r);
    }
    count += r;
    if (s[i] == 0) { *src = NULL; return count - 1; } // exclude NUL from count
  }
  *src = s + nwc;
  return count;
}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

static const DynLibFunction dynlib_functions[] = {
  // --- bionic runtime / stdio backing ---
  { "__sF", (uintptr_t)&fake_sF },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "abort", (uintptr_t)&abort },
  { "exit", (uintptr_t)&exit },
  { "_exit", (uintptr_t)&_exit },

  // --- fortify (_chk) wrappers ---
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },

  // --- bionic misc ---
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_write", (uintptr_t)&__android_log_write },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "getenv", (uintptr_t)&getenv },
  { "setenv", (uintptr_t)&setenv },
  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },

  // --- dynamic loader (routed to the EGL/GLES bridge) ---
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },

  // --- Android NDK window (SDL Android video) ---
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release_fake },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_fake },

  // --- signals (stubbed) ---
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "signal", (uintptr_t)&signal_fake },
  { "sigaddset", (uintptr_t)&sigaddset_fake },
  { "sigemptyset", (uintptr_t)&sigemptyset_fake },
  { "pthread_sigmask", (uintptr_t)&pthread_sigmask_fake },

  // --- setjmp/longjmp (must bind to the real newlib symbols) ---
  { "setjmp", (uintptr_t)&setjmp },
  { "longjmp", (uintptr_t)&longjmp },

  // --- memory ---
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },

  // --- mem/str ---
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "memmem", (uintptr_t)&memmem },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strlen", (uintptr_t)&strlen },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strcoll", (uintptr_t)&strcoll },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strftime", (uintptr_t)&strftime },
  { "strlcat", (uintptr_t)&strlcat },
  { "strlcpy", (uintptr_t)&strlcpy },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "strtok", (uintptr_t)&strtok },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "atoi", (uintptr_t)&atoi },
  { "atof", (uintptr_t)&atof },
  { "toupper", (uintptr_t)&toupper },
  { "tolower", (uintptr_t)&tolower },
  { "qsort", (uintptr_t)&qsort },

  // --- ctype ---
  { "islower", (uintptr_t)&islower },
  { "isupper", (uintptr_t)&isupper },
  { "isspace", (uintptr_t)&isspace },
  { "isxdigit", (uintptr_t)&isxdigit },

  // --- wide char / wctype ---
  { "btowc", (uintptr_t)&btowc },
  { "wctob", (uintptr_t)&wctob },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "iswalpha", (uintptr_t)&iswalpha },
  { "iswblank", (uintptr_t)&iswblank },
  { "iswcntrl", (uintptr_t)&iswcntrl },
  { "iswdigit", (uintptr_t)&iswdigit },
  { "iswlower", (uintptr_t)&iswlower },
  { "iswprint", (uintptr_t)&iswprint },
  { "iswpunct", (uintptr_t)&iswpunct },
  { "iswspace", (uintptr_t)&iswspace },
  { "iswupper", (uintptr_t)&iswupper },
  { "iswxdigit", (uintptr_t)&iswxdigit },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },

  // --- locale ---
  { "setlocale", (uintptr_t)&setlocale },
  { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },

  // --- printf / scanf family ---
  { "printf", (uintptr_t)&debugPrintf },
  { "putchar", (uintptr_t)&putchar_fake },
  { "puts", (uintptr_t)&puts_fake },
  { "snprintf", (uintptr_t)&snprintf },
  { "swprintf", (uintptr_t)&swprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "vsscanf", (uintptr_t)&vsscanf },

  // --- stdio over fake __sF + buffered fopen ---
  { "fopen", (uintptr_t)&fopen_fake },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "ftell", (uintptr_t)&ftell },
  { "ftello", (uintptr_t)&ftello },
  { "fflush", (uintptr_t)&fflush_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fgets", (uintptr_t)&fgets },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "remove", (uintptr_t)&remove },
  { "lseek", (uintptr_t)&lseek },
  { "stat", (uintptr_t)&stat_fake },

  // --- math ---
  { "acos", (uintptr_t)&acos }, { "acosf", (uintptr_t)&acosf },
  { "asin", (uintptr_t)&asin }, { "asinf", (uintptr_t)&asinf },
  { "atan", (uintptr_t)&atan }, { "atanf", (uintptr_t)&atanf },
  { "atan2", (uintptr_t)&atan2 }, { "atan2f", (uintptr_t)&atan2f },
  { "cos", (uintptr_t)&cos }, { "cosf", (uintptr_t)&cosf },
  { "sin", (uintptr_t)&sin }, { "sinf", (uintptr_t)&sinf },
  { "sincos", (uintptr_t)&sincos_fake },
  { "tan", (uintptr_t)&tan }, { "tanf", (uintptr_t)&tanf },
  { "exp", (uintptr_t)&exp },
  { "log", (uintptr_t)&log }, { "logf", (uintptr_t)&logf },
  { "log10", (uintptr_t)&log10 }, { "log10f", (uintptr_t)&log10f },
  { "pow", (uintptr_t)&pow }, { "powf", (uintptr_t)&powf },
  { "fmod", (uintptr_t)&fmod }, { "fmodf", (uintptr_t)&fmodf },
  { "frexp", (uintptr_t)&frexp }, { "ldexp", (uintptr_t)&ldexp },
  { "modf", (uintptr_t)&modf },
  { "scalbn", (uintptr_t)&scalbn }, { "scalbnf", (uintptr_t)&scalbnf },

  // --- time ---
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "usleep", (uintptr_t)&usleep },

  // --- pthread ---
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_detach", (uintptr_t)&pthread_detach },
  { "pthread_self", (uintptr_t)&pthread_self },
  { "pthread_equal", (uintptr_t)&pthread_equal },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize },
  { "pthread_getschedparam", (uintptr_t)&ret0_stub },
  { "pthread_setschedparam", (uintptr_t)&ret0_stub },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },

  // --- semaphores (libc_shim, pointer-indirected) ---
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },

  // --- scheduling ---
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },
  { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min_fake },

  // --- zlib (host -lz) ---
  { "adler32", (uintptr_t)&adler32 },
  { "crc32", (uintptr_t)&crc32 },
  { "deflate", (uintptr_t)&deflate },
  { "deflateEnd", (uintptr_t)&deflateEnd },
  { "deflateInit2_", (uintptr_t)&deflateInit2_ },
  { "deflateReset", (uintptr_t)&deflateReset },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "inflateReset", (uintptr_t)&inflateReset },
  { "inflateReset2", (uintptr_t)&inflateReset2 },

  // --- GLES: common / GLES2 core (mesa) ---
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glViewport", (uintptr_t)&glViewport },
  { "glScissor", (uintptr_t)&glScissor },
  { "glEnable", (uintptr_t)&glEnable },
  { "glDisable", (uintptr_t)&glDisable },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
  { "glBlendEquation", (uintptr_t)&glBlendEquation },
  { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
  { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glFinish", (uintptr_t)&glFinish },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glBufferSubData", (uintptr_t)&glBufferSubData },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glShaderBinary", (uintptr_t)&glShaderBinary },
  { "glCompileShader", (uintptr_t)&glCompileShader },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glDetachShader", (uintptr_t)&glDetachShader },
  { "glLinkProgram", (uintptr_t)&glLinkProgram },
  { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform4f", (uintptr_t)&glUniform4f },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },

  // --- GLES: texture tracking + crop rect (gl_compat) ---
  { "glBindTexture", (uintptr_t)&glBindTexture_compat },
  { "glActiveTexture", (uintptr_t)&glActiveTexture_compat },
  { "glTexImage2D", (uintptr_t)&glTexImage2D_compat },
  { "glTexParameteriv", (uintptr_t)&glTexParameteriv_compat },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures_compat },

  // --- GLES1 fixed-function (mesa libGLESv1_CM) ---
  { "glColor4f", (uintptr_t)&glColor4f },
  { "glVertexPointer", (uintptr_t)&glVertexPointer },
  { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
  { "glEnableClientState", (uintptr_t)&glEnableClientState },
  { "glDisableClientState", (uintptr_t)&glDisableClientState },
  { "glMatrixMode", (uintptr_t)&glMatrixMode },
  { "glLoadIdentity", (uintptr_t)&glLoadIdentity },
  { "glOrthof", (uintptr_t)&glOrthof },
  { "glPushMatrix", (uintptr_t)&glPushMatrix },
  { "glPopMatrix", (uintptr_t)&glPopMatrix },
  { "glTranslatef", (uintptr_t)&glTranslatef },
  { "glRotatef", (uintptr_t)&glRotatef },
  { "glTexEnvf", (uintptr_t)&glTexEnvf },

  // --- GLES1 OES extensions -> compat / core ---
  { "glDrawTexfOES", (uintptr_t)&glDrawTexfOES_compat },
  { "glBindFramebufferOES", (uintptr_t)&glBindFramebuffer },
  { "glDeleteFramebuffersOES", (uintptr_t)&glDeleteFramebuffers },
  { "glGenFramebuffersOES", (uintptr_t)&glGenFramebuffers },
  { "glCheckFramebufferStatusOES", (uintptr_t)&glCheckFramebufferStatus },
  { "glFramebufferTexture2DOES", (uintptr_t)&glFramebufferTexture2D },
  { "glBlendEquationOES", (uintptr_t)&glBlendEquation },
  { "glBlendEquationSeparateOES", (uintptr_t)&glBlendEquationSeparate },
  { "glBlendFuncSeparateOES", (uintptr_t)&glBlendFuncSeparate },
};

static const size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void sotn_resolve_imports(so_module *mod) {
  so_relocate(mod);
  so_resolve(mod, (DynLibFunction *)dynlib_functions, (int)dynlib_numfunctions, 1);
}
