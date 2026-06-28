/* jni_fake.h -- fake JNI environment for libsotn.so's SDL 2.0.8 Android backend.
 * MIT license; see LICENSE. */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

void jni_init(void);

// class singletons handed to the three nativeSetupJNI entry points
void *jni_activity_class(void);
void *jni_audio_class(void);
void *jni_controller_class(void);

void *jni_new_string(const char *s);

// main.c installs a callback to register the virtual gamepad when SDL first
// calls pollInputDevices() (i.e. after SDL_Init)
extern void (*jni_poll_input_devices_cb)(void);

void sdl_audio_shutdown(void);

#endif
