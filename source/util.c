/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#if DEBUG_LOG

static int s_nxlinkSock = -1;
static FILE *s_log = NULL; // persistent log handle (fast; fflush per line)

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

// sdmc is mounted by the time userAppInit runs, so open the log once here
// instead of reopening it per line (the engine logs thousands of lines).
void userAppInit(void) {
  initNxLink();
  s_log = fopen(LOG_NAME, "w");
}

void userAppExit(void) {
  if (s_log) { fclose(s_log); s_log = NULL; }
  deinitNxLink();
}

#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;

  if (s_log) {
    va_start(list, text);
    vfprintf(s_log, text, list);
    va_end(list);
    fflush(s_log); // flush each line so a crash still leaves a complete log
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);
#endif
  return 0;
}

// Shared TLS block for the engine stack-protector guard at tpidr_el0 + 0x28.
static uint8_t s_tls_block[0x1000] __attribute__((aligned(16)));

void tls_setup_guard(void) {
  *(uint64_t *)(s_tls_block + 0x28) = 0x0123456789ABCDEFull;
  armSetTlsRw(s_tls_block);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }

// Override libnx's exit teardown to skip the SD unmount: the engine's threads
// are still reading the card, so commit saves and leave it mounted.
void __appExit(void) {
  fsdevCommitDevice("sdmc");
  timeExit();
  hidExit();
  appletExit();
  smExit();
}
