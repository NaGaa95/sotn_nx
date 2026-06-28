/* config.h -- Castlevania SOTN Switch wrapper configuration.
 * MIT license; see LICENSE. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#define SO_NAME "libsotn.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "sotn_debug.log"

// '/'-absolute paths (so SDL treats them as files, not assets); resolve against
// the default sdmc device. DATA_ROOT/assets/... holds the extracted OBB tree,
// SAVE_ROOT holds saves/config. Overridable from config.txt.
#define DEFAULT_DATA_ROOT "/switch/sotn"
#define DEFAULT_SAVE_ROOT "/switch/sotn/save"

#define DEBUG_LOG 0

extern int screen_width;
extern int screen_height;

// getCurrentLanguage() index: 0 Japanese 1 English 2 French 3 Spanish 4 German 5 Italian
#define LANG_ENGLISH 1

typedef struct {
  int screen_width;   // -1 = auto
  int screen_height;
  int language;
  int swap_ab;        // 0 = A confirms/jumps (Nintendo, default); 1 = B does (PlayStation)
  char data_root[256];
  char save_root[256];
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
