#include <esp_app_format.h>

// extern "C" directly on definition (not inside a block) gives true external
// C linkage — overriding the arduino-lib-builder copy in libapp_update.a.
// -Wl,--allow-multiple-definition in platformio.ini lets the linker accept
// both definitions and keep the first one found (this .o before the archive).
extern "C" const __attribute__((section(".rodata_desc")))
__attribute__((aligned(16)))
esp_app_desc_t esp_app_desc = {
    /* magic_word     */ ESP_APP_DESC_MAGIC_WORD,
    /* secure_version */ 0,
    /* reserv1        */ {0, 0},
    /* version        */ APP_VERSION,
    /* project_name   */ "sms_forwarding by keroming",
    /* time           */ APP_BUILD_TIME,
    /* date           */ APP_BUILD_DATE,
    /* idf_ver        */ IDF_VER,
    /* app_elf_sha256 */ {0},
    /* reserv2        */ {0},
};
