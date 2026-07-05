/* main.c -- Castlevania SOTN Switch wrapper entry point.
 * MIT license; see LICENSE. */

#define SDL_MAIN_HANDLED

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module game_mod;

// reserve a slice for the .so loader; the rest is the newlib heap where the
// engine's malloc lands. Requires full-RAM mode (forwarder).
#define SO_HEAP_RESERVE (32 * 1024 * 1024)

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_reserve = SO_HEAP_RESERVE;
  if (so_reserve > size / 2)
    so_reserve = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t fake_heap_size = size - so_reserve;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", SO_NAME);
  char assets[300];
  snprintf(assets, sizeof(assets), "%s/assets", config.data_root);
  if (stat(assets, &st) < 0)
    fatal_error("Could not find game data at\n%s.\n(see README).", assets);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
}

// ---------------------------------------------------------------------------
// SDL 2.0.8 Android entry points (exported by libsotn.so)
// ---------------------------------------------------------------------------

static int  (*e_SDL_main)(int argc, char **argv);
static int  (*e_JNI_OnLoad)(void *vm, void *reserved);
static void (*e_Activity_setupJNI)(void *env, void *cls);
static void (*e_Audio_setupJNI)(void *env, void *cls);
static void (*e_Controller_setupJNI)(void *env, void *cls);
static void (*e_onNativeResize)(void *env, void *cls, int w, int h, int format, float rate);
static void (*e_onNativeTouch)(void *env, void *cls, int dev, int finger, int action, float x, float y, float p);
static void (*e_nativePause)(void *env, void *cls);
static void (*e_nativeResume)(void *env, void *cls);
static int  (*e_addJoystick)(void *env, void *cls, int dev, void *name, void *desc, int is_accel, int nbuttons, int naxes, int nhats, int nballs);
static int  (*e_onPadDown)(void *env, void *cls, int dev, int keycode);
static int  (*e_onPadUp)(void *env, void *cls, int dev, int keycode);
static int  (*e_onJoy)(void *env, void *cls, int dev, int axis, float value);

#define J "Java_org_libsdl_app_"

static void resolve_entry_points(void) {
  e_SDL_main           = (void *)so_find_addr_rx(&game_mod, "SDL_main");
  e_JNI_OnLoad         = (void *)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
  e_Activity_setupJNI  = (void *)so_find_addr_rx(&game_mod, J "SDLActivity_nativeSetupJNI");
  e_Audio_setupJNI     = (void *)so_try_find_addr_rx(&game_mod, J "SDLAudioManager_nativeSetupJNI");
  e_Controller_setupJNI= (void *)so_try_find_addr_rx(&game_mod, J "SDLControllerManager_nativeSetupJNI");
  e_onNativeResize     = (void *)so_find_addr_rx(&game_mod, J "SDLActivity_onNativeResize");
  e_onNativeTouch      = (void *)so_try_find_addr_rx(&game_mod, J "SDLActivity_onNativeTouch");
  e_nativePause        = (void *)so_try_find_addr_rx(&game_mod, J "SDLActivity_nativePause");
  e_nativeResume       = (void *)so_try_find_addr_rx(&game_mod, J "SDLActivity_nativeResume");
  e_addJoystick        = (void *)so_try_find_addr_rx(&game_mod, J "SDLControllerManager_nativeAddJoystick");
  e_onPadDown          = (void *)so_try_find_addr_rx(&game_mod, J "SDLControllerManager_onNativePadDown");
  e_onPadUp            = (void *)so_try_find_addr_rx(&game_mod, J "SDLControllerManager_onNativePadUp");
  e_onJoy              = (void *)so_try_find_addr_rx(&game_mod, J "SDLControllerManager_onNativeJoy");
}

// ---------------------------------------------------------------------------
// virtual gamepad
// ---------------------------------------------------------------------------

#define VJOY_ID 1

#define AKEYCODE_DPAD_UP    19
#define AKEYCODE_DPAD_DOWN  20
#define AKEYCODE_DPAD_LEFT  21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_BUTTON_A   96
#define AKEYCODE_BUTTON_B   97
#define AKEYCODE_BUTTON_X   99
#define AKEYCODE_BUTTON_Y   100
#define AKEYCODE_BUTTON_L1  102
#define AKEYCODE_BUTTON_R1  103
#define AKEYCODE_BUTTON_THUMBL 106
#define AKEYCODE_BUTTON_THUMBR 107
#define AKEYCODE_BUTTON_START  108
#define AKEYCODE_BUTTON_SELECT 109

// keycode_to_SDL maps Android keycodes to SDL_CONTROLLER_BUTTON_* (A=0..dpright=14);
// SDL 2.0.8 auto-generates the gamecontroller mapping from these counts.
#define VJOY_NBUTTONS 15
#define VJOY_NAXES    6

static volatile int s_joy_registered = 0;

// registered from SDL's pollInputDevices() callback so it lands after SDL_Init.
static void register_vjoy(void) {
  if (s_joy_registered || !e_addJoystick) return;
  s_joy_registered = 1;
  void *name = jni_new_string("Switch Controller");
  void *desc = jni_new_string("switch_pro_controller");
  e_addJoystick(fake_env, jni_controller_class(), VJOY_ID, name, desc,
                0, VJOY_NBUTTONS, VJOY_NAXES, 0, 0);
}

// ---------------------------------------------------------------------------
// input feeding (main thread)
// ---------------------------------------------------------------------------

static PadState pad;

// entries 0/1 are A/B; swap_ab swaps their keycodes at startup
static struct { u64 sw; int key; } s_btnmap[] = {
  { HidNpadButton_A,      AKEYCODE_BUTTON_A },   // Nintendo A (east)  -> confirm
  { HidNpadButton_B,      AKEYCODE_BUTTON_B },   // Nintendo B (south) -> cancel
  { HidNpadButton_Y,      AKEYCODE_BUTTON_X },
  { HidNpadButton_X,      AKEYCODE_BUTTON_Y },
  { HidNpadButton_L,      AKEYCODE_BUTTON_L1 },
  { HidNpadButton_R,      AKEYCODE_BUTTON_R1 },
  { HidNpadButton_Plus,   AKEYCODE_BUTTON_START },
  { HidNpadButton_Minus,  AKEYCODE_BUTTON_SELECT },
  { HidNpadButton_StickL, AKEYCODE_BUTTON_THUMBL },
  { HidNpadButton_StickR, AKEYCODE_BUTTON_THUMBR },
  { HidNpadButton_Up,     AKEYCODE_DPAD_UP },
  { HidNpadButton_Down,   AKEYCODE_DPAD_DOWN },
  { HidNpadButton_Left,   AKEYCODE_DPAD_LEFT },
  { HidNpadButton_Right,  AKEYCODE_DPAD_RIGHT },
};

static u64 s_prev_buttons = 0;
static int s_prev_axis[6] = {0};
static int s_touching = 0;
static float s_last_tx = 0, s_last_ty = 0;

static void send_axis(int idx, int raw) {
  if (raw == s_prev_axis[idx]) return;
  s_prev_axis[idx] = raw;
  if (e_onJoy) e_onJoy(fake_env, jni_controller_class(), VJOY_ID, idx, raw / 32767.0f);
}

static void update_input(void) {
  padUpdate(&pad);
  const u64 cur = padGetButtons(&pad);

  if (s_joy_registered) {
    for (unsigned i = 0; i < sizeof(s_btnmap) / sizeof(*s_btnmap); i++) {
      const u64 m = s_btnmap[i].sw;
      if ((cur & m) && !(s_prev_buttons & m)) { if (e_onPadDown) e_onPadDown(fake_env, jni_controller_class(), VJOY_ID, s_btnmap[i].key); }
      else if (!(cur & m) && (s_prev_buttons & m)) { if (e_onPadUp) e_onPadUp(fake_env, jni_controller_class(), VJOY_ID, s_btnmap[i].key); }
    }

    // left stick drives movement; the d-pad drives it too (full deflection)
    HidAnalogStickState l = padGetStickPos(&pad, 0);
    HidAnalogStickState r = padGetStickPos(&pad, 1);
    int lx = l.x, ly = -l.y; // SDL: up is negative
    if (cur & HidNpadButton_Left)  lx = -32767;
    if (cur & HidNpadButton_Right) lx =  32767;
    if (cur & HidNpadButton_Up)    ly = -32767;
    if (cur & HidNpadButton_Down)  ly =  32767;
    send_axis(0, lx);
    send_axis(1, ly);
    send_axis(2, r.x);
    send_axis(3, -r.y);
    send_axis(4, (cur & HidNpadButton_ZL) ? 32767 : 0);
    send_axis(5, (cur & HidNpadButton_ZR) ? 32767 : 0);
  }
  s_prev_buttons = cur;

  if (e_onNativeTouch) {
    HidTouchScreenState ts = {0};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
      float x = ts.touches[0].x / 1280.0f;
      float y = ts.touches[0].y / 720.0f;
      e_onNativeTouch(fake_env, jni_activity_class(), 0, 0, s_touching ? 2 : 0, x, y, 1.0f);
      s_touching = 1; s_last_tx = x; s_last_ty = y;
    } else if (s_touching) {
      e_onNativeTouch(fake_env, jni_activity_class(), 0, 0, 1, s_last_tx, s_last_ty, 1.0f);
      s_touching = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// game thread
// ---------------------------------------------------------------------------

static Thread s_game_thread;
static volatile int s_game_running = 1;

static void game_thread_fn(void *arg) {
  (void)arg;
  tls_setup_guard(); // engine reads its stack canary from tpidr_el0+0x28
  char arg0[] = "sotn";
  char *argv[] = { arg0, NULL };
  e_SDL_main(1, argv);
  s_game_running = 0;
}

int main(void) {
  cpu_boost(1);

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  if (config.swap_ab) { // PlayStation layout: confirm/jump on B (south)
    s_btnmap[0].key = AKEYCODE_BUTTON_B;
    s_btnmap[1].key = AKEYCODE_BUTTON_A;
  }

  check_syscalls();
  check_data();
  mkdir(config.save_root, 0777);
  char snap[300]; // the engine saves to <data_root>/snapshots/
  snprintf(snap, sizeof(snap), "%s/snapshots", config.data_root);
  mkdir(snap, 0777);
  set_screen_size(config.screen_width, config.screen_height);

  setenv("SDL_ACCELEROMETER_AS_JOYSTICK", "0", 1);

  SDL_SetMainReady();
  SDL_Init(SDL_INIT_AUDIO); // host SDL2: audio output only

  if (so_load(&game_mod, SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  sotn_resolve_imports(&game_mod);

  // resolve exports before so_finalize maps the code and locks load_base out
  resolve_entry_points();
  if (!e_SDL_main || !e_Activity_setupJNI || !e_onNativeResize)
    fatal_error("Could not resolve SDL entry points in libsotn.so.");

  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  tls_setup_guard();
  so_execute_init_array(&game_mod);
  so_free_temp(&game_mod);

  jni_init();
  jni_poll_input_devices_cb = register_vjoy;

  if (e_JNI_OnLoad) e_JNI_OnLoad(fake_vm, NULL);
  e_Activity_setupJNI(fake_env, jni_activity_class());
  if (e_Audio_setupJNI) e_Audio_setupJNI(fake_env, jni_audio_class());
  if (e_Controller_setupJNI) e_Controller_setupJNI(fake_env, jni_controller_class());

  // publish the resolution before the engine runs SDL_Init(VIDEO); format 1 = RGBA_8888
  e_onNativeResize(fake_env, jni_activity_class(), screen_width, screen_height, 1, 60.0f);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  if (R_FAILED(threadCreate(&s_game_thread, game_thread_fn, NULL, NULL, 8 * 1024 * 1024, 0x2C, -2)))
    fatal_error("Could not create the game thread.");
  threadStart(&s_game_thread);

  int boot_frames = 0;
  int focused = 1;
  while (appletMainLoop() && s_game_running) {
    AppletFocusState fs = appletGetFocusState();
    int now_focused = (fs == AppletFocusState_InFocus);
    if (now_focused != focused) {
      focused = now_focused;
      if (focused) { if (e_nativeResume) e_nativeResume(fake_env, jni_activity_class()); }
      else         { if (e_nativePause)  e_nativePause(fake_env, jni_activity_class()); }
    }

    update_input();

    if (++boot_frames == 600) cpu_boost(0);
    if (boot_frames == 1250 && !s_joy_registered) register_vjoy(); // fallback if pollInputDevices never fires

    svcSleepThread(4 * 1000 * 1000);
  }

  // Exit through libnx (see __appExit in util.c); the engine's threads are still
  // running, so skip the graceful SDL/thread teardown.
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
