// stb_image (public domain, stb_image.h unchanged) for the apps: JPEG and PNG (with its zlib, which Hackaday also
// uses to inflate the gzipped feed); every buffer in PSRAM. Apps: PRIV_REQUIRES stb, then #include "stb_image.h".
#include "esp_heap_caps.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_ASSERT(x)
#define STBI_MALLOC(n) heap_caps_malloc(n, MALLOC_CAP_SPIRAM)
#define STBI_REALLOC(p, n) heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM)
#define STBI_FREE(p) heap_caps_free(p)
#include "stb_image.h"
