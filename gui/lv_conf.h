/**
 * @file lv_conf.h
 * LVGL v9.2.2 configuration for Smart Screen (SDL2 + FreeType on Ubuntu).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/*── Color ────────────────────────────────────────────────────────────*/
#define LV_COLOR_DEPTH  32

/*── Memory ────────────────────────────────────────────────────────────*/
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB
#define LV_MEM_SIZE             (2 * 1024 * 1024U)

/*── OS ────────────────────────────────────────────────────────────────*/
#define LV_USE_OS   LV_OS_NONE

/*── Display: SDL ──────────────────────────────────────────────────────*/
#define LV_USE_SDL          1
#if LV_USE_SDL
    #define LV_SDL_INCLUDE_PATH    <SDL2/SDL.h>
    #define LV_SDL_RENDER_MODE     LV_DISPLAY_RENDER_MODE_DIRECT
    #define LV_SDL_BUF_COUNT       2
    #define LV_SDL_FULLSCREEN      0
    #define LV_SDL_DIRECT_EXIT     0
#endif

/*── Fonts ─────────────────────────────────────────────────────────────*/
#define LV_FONT_DEFAULT             &lv_font_montserrat_14

/* Enable specific Latin font sizes */
#define LV_FONT_MONTSERRAT_16       1
#define LV_FONT_MONTSERRAT_18       1
#define LV_FONT_MONTSERRAT_20       1
#define LV_FONT_MONTSERRAT_24       1
#define LV_FONT_MONTSERRAT_28       1
#define LV_FONT_MONTSERRAT_36       1
#define LV_FONT_SIMSUN_16_CJK       1

/* FreeType for TTF/OTF font rendering (Chinese support) */
#define LV_USE_FREETYPE             1
#if LV_USE_FREETYPE
    #define LV_FREETYPE_CACHE_SIZE       (64 * 1024)
    #define LV_FREETYPE_CACHE_FT_GLYPHS  128
    #define LV_FREETYPE_INCLUDE          <ft2build.h>
#endif

/*── Widgets ───────────────────────────────────────────────────────────*/
#define LV_USE_BUTTON       1
#define LV_USE_LABEL        1
#define LV_USE_IMAGE        1
#define LV_USE_SWITCH       1
#define LV_USE_SLIDER       1
#define LV_USE_TABVIEW      1
#define LV_USE_ARC          1
#define LV_USE_BAR          1
#define LV_USE_CHECKBOX     1
#define LV_USE_FLEX         1
#define LV_USE_GRID         1
#define LV_USE_ANIM         1

/*── Filesystem (for loading images) ───────────────────────────────────*/
#define LV_USE_FS_STDIO         1
#define LV_FS_STDIO_LETTER      'A'
#define LV_FS_STDIO_PATH        ""
#define LV_FS_STDIO_CACHE_SIZE   4096

/*── Image decoders ────────────────────────────────────────────────────*/
#define LV_USE_BMP              1
#define LV_USE_LODEPNG          1

/*── Extras ────────────────────────────────────────────────────────────*/
#define LV_USE_SNAPSHOT     0
#define LV_USE_LOG          0

#endif /* LV_CONF_H */
