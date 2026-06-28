/* egl_shim.c -- dlopen/dlsym bridge to mesa EGL/GLES. SDL's Android backend
 * dlopen()s libEGL.so and resolves egl* by name; we answer with mesa.
 * MIT license; see LICENSE. */

#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "egl_shim.h"

#define FAKE_DL_HANDLE ((void *)0xE61D1B)

typedef void (*generic_func)(void);
typedef struct { const char *name; generic_func fn; } EglEntry;

#define E(sym) { #sym, (generic_func)sym }

// core EGL entry points (eglGetProcAddress only returns extension/GL functions)
static const EglEntry egl_table[] = {
  E(eglGetError),
  E(eglGetDisplay),
  E(eglInitialize),
  E(eglTerminate),
  E(eglQueryString),
  E(eglGetConfigs),
  E(eglChooseConfig),
  E(eglGetConfigAttrib),
  E(eglCreateWindowSurface),
  E(eglCreatePbufferSurface),
  E(eglCreatePixmapSurface),
  E(eglDestroySurface),
  E(eglQuerySurface),
  E(eglBindAPI),
  E(eglQueryAPI),
  E(eglWaitClient),
  E(eglReleaseThread),
  E(eglCreatePbufferFromClientBuffer),
  E(eglSurfaceAttrib),
  E(eglBindTexImage),
  E(eglReleaseTexImage),
  E(eglSwapInterval),
  E(eglCreateContext),
  E(eglDestroyContext),
  E(eglMakeCurrent),
  E(eglGetCurrentContext),
  E(eglGetCurrentSurface),
  E(eglGetCurrentDisplay),
  E(eglQueryContext),
  E(eglWaitGL),
  E(eglWaitNative),
  E(eglSwapBuffers),
  E(eglCopyBuffers),
  E(eglGetProcAddress),
};

void *dlopen_fake(const char *filename, int flag) {
  (void)filename; (void)flag;
  return FAKE_DL_HANDLE; // any GL/EGL library (or NULL) maps to the bridge
}

void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol)
    return NULL;
  for (unsigned i = 0; i < sizeof(egl_table) / sizeof(*egl_table); i++) {
    if (strcmp(symbol, egl_table[i].name) == 0)
      return (void *)egl_table[i].fn;
  }
  return (void *)eglGetProcAddress(symbol); // GL entry points + EGL extensions
}

int dlclose_fake(void *handle) { (void)handle; return 0; }

char *dlerror_fake(void) { return NULL; }
