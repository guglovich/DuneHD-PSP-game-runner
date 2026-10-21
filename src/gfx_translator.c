/*
 * GFX Translator — трансляция графических вызовов PSP в DirectFB
 *
 * Вызовы графического процессора PSP (команды sceGu* или сырые Display Lists)
 * перехватываются и транслируются в вызовы DirectFB API, отсылая кадровый буфер
 * напрямую в Video Scaler.
 *
 * PSP GPU (Graphics Engine) использует display lists — списки команд,
 * которые выполняются асинхронно. Мы перехватываем эти команды и транслируем
 * их в вызовы DirectFB.
 */

#include "psp_hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

/* ============================================================================
 * Состояние GFX
 * ==========================================================================*/

typedef struct {
    void *addr;          /* Адрес кадрового буфера */
    u32 width;           /* Ширина */
    u32 height;          /* Высота */
    u32 pixel_format;    /* Формат пикселя */
    u32 stride;          /* Шаг строки в байтах */
    bool dirty;          /* Флаг необходимости обновления */
} GfxFramebuffer;

static GfxFramebuffer g_fb = {0};
static pthread_mutex_t g_gfx_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Таблица форматов пикселя */
static const struct {
    u32 psp_format;
    u32 bpp;
    const char *name;
} pixel_formats[] = {
    {PSP_DISPLAY_PIXEL_FORMAT_565,  2, "RGB565"},
    {PSP_DISPLAY_PIXEL_FORMAT_5551,  2, "RGBA5551"},
    {PSP_DISPLAY_PIXEL_FORMAT_4444,  2, "RGBA4444"},
    {PSP_DISPLAY_PIXEL_FORMAT_8888,  4, "RGBA8888"},
};

#define PIXEL_FORMAT_COUNT (sizeof(pixel_formats) / sizeof(pixel_formats[0]))

/* ============================================================================
 * Внутренние функции
 * ==========================================================================*/

/* Получение информации о формате пикселя */
static const char *get_format_name(u32 format)
{
    u32 i;
    for (i = 0; i < PIXEL_FORMAT_COUNT; i++) {
        if (pixel_formats[i].psp_format == format) {
            return pixel_formats[i].name;
        }
    }
    return "Unknown";
}

static u32 get_format_bpp(u32 format)
{
    u32 i;
    for (i = 0; i < PIXEL_FORMAT_COUNT; i++) {
        if (pixel_formats[i].psp_format == format) {
            return pixel_formats[i].bpp;
        }
    }
    return 4;  /* Default to 32-bit */
}

/* Конвертация пикселя из PSP формата в RGBA8888 */
static u32 convert_pixel(u32 pixel, u32 format)
{
    switch (format) {
    case PSP_DISPLAY_PIXEL_FORMAT_565:
        {
            u8 r = (pixel >> 11) & 0x1F;
            u8 g = (pixel >> 5) & 0x3F;
            u8 b = pixel & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            return (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    case PSP_DISPLAY_PIXEL_FORMAT_5551:
        {
            u8 r = (pixel >> 11) & 0x1F;
            u8 g = (pixel >> 6) & 0x1F;
            u8 b = (pixel >> 1) & 0x1F;
            u8 a = (pixel & 1) ? 0xFF : 0x00;
            r = (r << 3) | (r >> 2);
            g = (g << 3) | (g >> 2);
            b = (b << 3) | (b >> 2);
            return (a << 24) | (r << 16) | (g << 8) | b;
        }
    case PSP_DISPLAY_PIXEL_FORMAT_4444:
        {
            u8 r = (pixel >> 12) & 0xF;
            u8 g = (pixel >> 8) & 0xF;
            u8 b = (pixel >> 4) & 0xF;
            u8 a = pixel & 0xF;
            r = (r << 4) | r;
            g = (g << 4) | g;
            b = (b << 4) | b;
            a = (a << 4) | a;
            return (a << 24) | (r << 16) | (g << 8) | b;
        }
    case PSP_DISPLAY_PIXEL_FORMAT_8888:
    default:
        return pixel;
    }
}

/* ============================================================================
 * Инициализация GFX
 * ==========================================================================*/

void psp_gfx_init(void)
{
    printf("[GFX] Инициализация GFX Translator...\n");
    printf("[GFX] Поддерживаемые форматы:\n");
    {
        u32 i;
        for (i = 0; i < PIXEL_FORMAT_COUNT; i++) {
            printf("[GFX]   %s (%u bpp)\n",
                   pixel_formats[i].name, pixel_formats[i].bpp);
        }
    }
    printf("[GFX] GFX Translator инициализирован\n");
}

/* ============================================================================
 * Управление дисплеем
 * ==========================================================================*/

void psp_gfx_set_mode(u32 width, u32 height, u32 format)
{
    pthread_mutex_lock(&g_gfx_mutex);

    g_fb.width = width;
    g_fb.height = height;
    g_fb.pixel_format = format;
    g_fb.stride = width * get_format_bpp(format);
    g_fb.dirty = true;

    printf("[GFX] sceGuSetMode: %ux%u, %s\n",
           width, height, get_format_name(format));

    pthread_mutex_unlock(&g_gfx_mutex);
}

void psp_gfx_set_framebuf(void *addr, u32 width, u32 height, u32 format)
{
    pthread_mutex_lock(&g_gfx_mutex);

    g_fb.addr = addr;
    g_fb.width = width;
    g_fb.height = height;
    g_fb.pixel_format = format;
    g_fb.stride = width * get_format_bpp(format);
    g_fb.dirty = true;

    printf("[GFX] sceGuSetFrameBuf: addr=%p, %ux%u, %s\n",
           addr, width, height, get_format_name(format));

    pthread_mutex_unlock(&g_gfx_mutex);
}

void psp_gfx_wait_vsync(void)
{
    /* В реальности нужно ждать vsync дисплея */
    usleep(16666);  /* ~60 Hz */
}

/* ============================================================================
 * Отрисовка
 * ==========================================================================*/

void psp_gfx_draw_arrays(u32 primitive, u32 count, void *vertices, void *texcoords)
{
    pthread_mutex_lock(&g_gfx_mutex);

    if (!g_fb.addr) {
        pthread_mutex_unlock(&g_gfx_mutex);
        return;
    }

    /* Помечаем буфер как изменённый */
    g_fb.dirty = true;

    /* В реальной реализации здесь нужно:
     * 1. Разобрать вершины и текстурные координаты
     * 2. Выполнить растеризацию примитивов
     * 3. Записать результат в кадровый буфер
     *
     * Для сейчас просто логируем
     */

    printf("[GFX] sceGuDrawArrays: primitive=%u, count=%u\n", primitive, count);

    pthread_mutex_unlock(&g_gfx_mutex);
}

/* ============================================================================
 * Очистка
 * ==========================================================================*/

void psp_gfx_shutdown(void)
{
    pthread_mutex_destroy(&g_gfx_mutex);
    printf("[GFX] GFX Translator остановлен\n");
}
