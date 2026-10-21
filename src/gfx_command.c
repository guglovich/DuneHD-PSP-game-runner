/*
 * Сборка и отправка команд GFX — по протоколу из gfx_lib.c.
 *
 * Здесь ровно то, что подтверждено разбором: отсечение прямоугольника на
 * CPU и команда FillRectangle из пяти слов. Ничего не выводится «на глаз»:
 * каждое поле команды проверяется тестом tests/gfx_command_test.c.
 *
 * Транспорт безопасен: буфер в DRAM плюс RUASetProperty, регистры GFX из
 * userspace не пишутся, /dev/mum0 не открывается.
 */

#include "gfx_protocol.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Собственная полезная часть команды обязана влезать в границу DSP.
 * `GFX_COMMAND_SIZE` = 56 при пределе 88, запас есть; проверка ставится на
 * этапе сборки, чтобы при изменении одной из констант нельзя было получить
 * команду, которую DSP отвергнет с кодом 7. */
_Static_assert(GFX_COMMAND_SIZE <= GFX_DSP_CMD_SIZE_MAX,
               "полезная часть команды не влезает в границу DSP 88 байт");

/* ------------------------------------------------------------------------- */
/* Имена кодов                                                                */
/* ------------------------------------------------------------------------- */

static const struct {
    u32 code;
    const char *name;
} code_names[] = {
    { GFX_DRAM_SIZE,             "DRAMSize" },
    { GFX_OPEN,                  "Open" },
    { GFX_CLOSE,                 "Close" },
    { GFX_COMMAND_BUFFER,        "CommandBuffer" },
    { GFX_CORRECTION,            "Correction" },
    { GFX_COLOR_FORMAT,          "ColorFormat" },
    { GFX_ALPHA_FORMAT,          "AlphaFormat" },
    { GFX_SURFACE,               "Surface" },
    { GFX_BCS,                   "BCS" },
    { GFX_FILL_RECTANGLE,        "FillRectangle" },
    { GFX_BLEND_RECTANGLES,      "BlendRectangles" },
    { GFX_BLEND_GRADIENT,        "BlendGradient" },
    { GFX_RASTER_BLEND_GRADIENT, "RasterBlendGradient" },
    { GFX_RASTER_BLEND_RECT,     "RasterBlendRectangles" },
    { GFX_RASTER_GRADIENT,       "RasterGradient" },
    { GFX_RASTER_RECTANGLES,     "RasterRectangles" },
    { GFX_RASTER_RECTANGLES_XY,  "RasterRectanglesXY" },
    { GFX_REPLACE_GRADIENT,      "ReplaceGradient" },
    { GFX_FILL_GRADIENT,         "FillGradient" },
    { GFX_SINGLE_COLOR_BLEND,    "SingleColorBlendRectangles" },
    { GFX_BLEND_AND_SCALE,       "BlendAndScaleRectangles" },
    { GFX_MOVE_RECTANGLE,        "MoveRectangle" },
    { GFX_REPLACE_RECTANGLE,     "ReplaceRectangle" },
    { GFX_REPLACE_AND_SCALE,     "ReplaceAndScaleRectangle" },
    { GFX_MOVE_AND_SCALE,        "MoveAndScaleRectangle" },
    { GFX_DISPLAY_PICTURE,       "DisplayPicture" },
    { GFX_WAIT_FOR_PICTURE,      "WaitForPicture" },
    { GFX_LINEAR_GRADIENT_SURF,  "LinearGradientSurface" },
    { GFX_RADIAL_GRADIENT_SURF,  "RadialGradientSurface" },
    { GFX_PALETTE_1BPP,          "Palette_1BPP" },
    { GFX_PALETTE_2BPP,          "Palette_2BPP" },
    { GFX_PALETTE_4BPP,          "Palette_4BPP" },
    { GFX_PALETTE_8BPP,          "Palette_8BPP" },
    { GFX_KEY_COLOR,             "KeyColor" },
    { GFX_FIELD_TYPE,            "FieldType" },
    { GFX_NONLINEAR_SCALE,       "NonlinearScale" },
    { GFX_GLYPH_MASK,            "GlyphMask" },
    { GFX_GLYPH_SCALE_MATRIX,    "GlyphScaleMatrix" },
    { GFX_LPF_THRESHOLDS,        "LPFThresholds" },
    { GFX_GLYPH_OUTPUT_SIZE,     "GlyphOutputSize" },
    { GFX_FLUSH_COMMAND_QUEUE,   "FlushCommandQueue" },
    { GFX_COMMAND_QUEUE_EMPTY,   "CommandQueueEmpty" },
    { GFX_PICTURE_BUFFER_ADDR,   "PictureBufferAddress" },
    { GFX_COMMAND_QUEUE,         "CommandQueue" },
    { GFX_ENABLE_ALPHA_FADING,   "EnableAlphaFading" },
    { GFX_HOLD_COMMAND_QUEUE,    "HoldCommandQueue" },
    { GFX_RELEASE_COMMAND_QUEUE, "ReleaseCommandQueue" },
    { GFX_ALPHA_PALETTE,         "AlphaPalette" },
    { GFX_ADAPTATIVE_ENABLE,     "AdaptativeEnable" }
};

#define CODE_NAME_COUNT (sizeof(code_names) / sizeof(code_names[0]))

const char *gfx_code_name(u32 code)
{
    size_t i;

    for (i = 0; i < CODE_NAME_COUNT; i++)
        if (code_names[i].code == code)
            return code_names[i].name;

    /* Слот за пределами диапазона отвергается проверкой кода в самом
     * SEND_GFX_COMMAND с ошибкой "OTHER PROPERTY", а слот внутри диапазона
     * без обработчика указывает на заглушку DRAMSize. */
    if (code < GFX_CODE_MIN || code > GFX_CODE_MAX)
        return "other-property";
    return "unimplemented";
}

/* ------------------------------------------------------------------------- */
/* Отсечение                                                                  */
/* ------------------------------------------------------------------------- */

int gfx_clip_rect(GfxRect *r, const GfxRect *clip)
{
    int x0, y0, x1, y1;

    if (r == NULL || clip == NULL)
        return 0;

    /* Вырожденный прямоугольник рисовать нечем. shell проверяет то же
     * перед вычислением пересечения. */
    if (r->w <= 0 || r->h <= 0)
        return 0;

    /* Пересечение полуинтервалов [x, x+w) и [clip->x, clip->x+clip->w). */
    x0 = (r->x > clip->x) ? r->x : clip->x;
    y0 = (r->y > clip->y) ? r->y : clip->y;

    x1 = r->x + r->w;
    if (clip->x + clip->w < x1)
        x1 = clip->x + clip->w;
    y1 = r->y + r->h;
    if (clip->y + clip->h < y1)
        y1 = clip->y + clip->h;

    if (x1 <= x0 || y1 <= y0)
        return 0;

    r->x = x0;
    r->y = y0;
    r->w = x1 - x0;
    r->h = y1 - y0;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Сборка команды                                                             */
/* ------------------------------------------------------------------------- */

/* Раскладка FillRectangle — пять слов, 20 байт:
 *   +0  x
 *   +4  y
 *   +8  ширина
 *   +12 высота
 *   +16 цвет
 * Размер совпадает с тем, что собирает разбор обёртки MY_GFX_FillRectangle. */
#define FILL_RECT_WORDS 5u
#define FILL_RECT_SIZE  (FILL_RECT_WORDS * 4u)

static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

u32 gfx_build_fill_rect(GfxCommand *cmd, const GfxRect *r, u32 rgba)
{
    u8 *p;

    if (cmd == NULL || r == NULL)
        return 0;
    if (r->w <= 0 || r->h <= 0)
        return 0;

    memset(cmd->bytes, 0, sizeof(cmd->bytes));

    p = cmd->bytes;
    put_u32(p +  0, (u32)r->x);
    put_u32(p +  4, (u32)r->y);
    put_u32(p +  8, (u32)r->w);
    put_u32(p + 12, (u32)r->h);
    /* Альфа в старшем байте: непрозрачная заливка. В shell это
     * `lui $3,0xff00; or color,$10,$3`. */
    put_u32(p + 16, rgba | 0xFF000000u);

    cmd->code = GFX_FILL_RECTANGLE;
    cmd->size = FILL_RECT_SIZE;
    return cmd->size;
}

/* ------------------------------------------------------------------------- */
/* Surface                                                                    */
/* ------------------------------------------------------------------------- */

/* Кэш состояния в userspace. Вендор делает то же: повтор с той же парой
 * (a, b) не отправляется вовсе. Смысл не в экономии, а в том, что команда
 * меняет состояние движка и обязана идти перед рисованием, как
 * glBindFramebuffer. Отбрасывать повтор без сверки нельзя: тогда рисование
 * уйдёт не на ту поверхность. */
static u32 g_surf_engine;
static u32 g_surf_a;
static u32 g_surf_b;
static int g_surf_valid;

#define SURFACE_WORDS 6u
#define SURFACE_SIZE  (SURFACE_WORDS * 4u)

u32 gfx_build_surface(GfxCommand *cmd, u32 engine, u32 a, u32 b)
{
    u8 *p;

    if (cmd == NULL)
        return 0;

    /* Состояние не изменилось — команда вендором не отправляется. */
    if (g_surf_valid && g_surf_engine == engine && g_surf_a == a &&
        g_surf_b == b)
        return 0;

    g_surf_engine = engine;
    g_surf_a = a;
    g_surf_b = b;
    g_surf_valid = 1;

    /* Обнуление обязательно: args[2] пишется инструкцией sb, то есть
     * определён только младший байт слова, а остальные остаются мусором из
     * стека, если буфер не был чистым. */
    memset(cmd->bytes, 0, sizeof(cmd->bytes));

    p = cmd->bytes;
    put_u32(p +  0, engine);
    put_u32(p +  4, b);
    /* p[8] — байтовое поле, остаётся нулём после memset. Ставить отдельное
     * значение нечего: у вендора там тоже ноль. */
    put_u32(p + 12, a);
    /* p[16], p[20] — нули. */

    cmd->code = GFX_SURFACE;
    cmd->size = SURFACE_SIZE;
    return cmd->size;
}

/* ------------------------------------------------------------------------- */
/* Отправка                                                                   */
/* ------------------------------------------------------------------------- */

typedef int (*gfx_send_fn)(u32 code, void *args, u32 size);

/* Адрес SEND_GFX_COMMAND. Ищется один раз: функция экспортирована бинарником
 * shell, и из нашей библиотеки, загруженной в shell, она видна в глобальной
 * области. Вне shell символа нет — тогда отправка честно сообщает об
 * ошибке, а не падает. */
static gfx_send_fn g_send;

static gfx_send_fn resolve_send(void)
{
    if (g_send != NULL)
        return g_send;

    /* Преобразование указателя, полученного от dlsym, в тип функции:
     * POSIX требует именно так, и на практике это работает на всех
     * платформах, где dlsym возвращает адрес функции. */
    *(void **)(&g_send) = dlsym(RTLD_DEFAULT, "SEND_GFX_COMMAND");
    return g_send;
}

/* ------------------------------------------------------------------------- */
/* Батч                                                                      */
/* ------------------------------------------------------------------------- */

/* Растёт батч так же, как в исходнике: старт 128 слотов, при запросе
 * 129-го сразу 8192, дальше удвоение.
 *
 * В приведённом разборе путь первого выделения выглядит как
 * `capacity = 128; if (...) { need = 0x2000; ... } realloc(buffer, need << 6)`.
 * То есть в ветке, где условие не выполнено, `need` не присваивается, и
 * `realloc` получает мусор из стека. Либо в разборе потерялась строка
 * `need = 128`, либо это настоящая ошибка вендора. Здесь принят единственный
 * осмысленный вариант: `need` следует за `capacity`, то есть первый буфер —
 * 128 слотов, 8192 байт. Это же зафиксировано гейтом. */
static int batch_reserve(GfxBatch *b, u32 need_slots)
{
    u8 *p;
    u32 want = need_slots;

    if (b->capacity >= want)
        return 1;

    if (b->capacity < GFX_BATCH_INITIAL_SLOTS) {
        want = GFX_BATCH_INITIAL_SLOTS;
    } else {
        want = GFX_BATCH_MIN_SLOTS;
        while (want < need_slots)
            want <<= 1;
    }

    p = (u8 *)realloc(b->slots, (size_t)want * GFX_SLOT_SIZE);
    if (p == NULL)
        return 0;

    b->slots = p;
    b->capacity = want;
    return 1;
}

void gfx_batch_begin(GfxBatch *b)
{
    if (b == NULL)
        return;
    b->slots = NULL;
    b->count = 0;
    b->capacity = 0;
}

int gfx_batch_add(GfxBatch *b, const GfxCommand *cmd)
{
    u8 *slot;

    if (b == NULL || cmd == NULL)
        return 0;
    if (cmd->size > GFX_COMMAND_SIZE)
        return 0;
    if (cmd->size == 0)
        return 0;

    if (!batch_reserve(b, b->count + 1))
        return 0;

    slot = b->slots + (size_t)b->count * GFX_SLOT_SIZE;
    memset(slot, 0, GFX_SLOT_SIZE);
    memcpy(slot, cmd->bytes, cmd->size);
    b->count++;
    return 1;
}

int gfx_batch_submit(const GfxBatch *b, unsigned max_retries)
{
    unsigned tries;
    int rc;

    if (b == NULL || b->slots == NULL || b->count == 0)
        return GFX_RET_ERROR;

    /* Размер уходит в байтах, а не в командах: слот 64 байта, и это
     * исключение среди всех остальных команд. */
    for (tries = 0; tries <= max_retries; tries++) {
        rc = gfx_send_raw(GFX_COMMAND_BUFFER, (void *)b->slots,
                          b->count * GFX_SLOT_SIZE);
        if (rc != GFX_RET_RETRY)
            return rc;
    }
    return GFX_RET_RETRY;
}

void gfx_batch_end(GfxBatch *b)
{
    if (b == NULL)
        return;
    free(b->slots);
    b->slots = NULL;
    b->count = 0;
    b->capacity = 0;
}

u32 gfx_batch_count(const GfxBatch *b)
{
    return (b == NULL) ? 0u : b->count;
}

u32 gfx_batch_capacity(const GfxBatch *b)
{
    return (b == NULL) ? 0u : b->capacity;
}

u32 gfx_batch_byte_size(const GfxBatch *b)
{
    if (b == NULL)
        return 0;
    return b->count * GFX_SLOT_SIZE;
}

const u8 *gfx_batch_slot(const GfxBatch *b, u32 index)
{
    if (b == NULL || b->slots == NULL || index >= b->count)
        return NULL;
    return b->slots + (size_t)index * GFX_SLOT_SIZE;
}

/* ------------------------------------------------------------------------- */
/* Отправка                                                                   */
/* ------------------------------------------------------------------------- */

int gfx_send_raw(u32 code, void *args, u32 size)
{
    gfx_send_fn send;

    if (code < GFX_CODE_MIN || code > GFX_CODE_MAX)
        return GFX_RET_ERROR;
    if (size > GFX_BATCH_MAX_BYTES)
        return GFX_RET_ERROR;

    send = resolve_send();
    if (send == NULL)
        return GFX_RET_ERROR;

    return send(code, args, size);
}

int gfx_send_command(const GfxCommand *cmd)
{
    if (cmd == NULL)
        return GFX_RET_ERROR;
    if (cmd->size > GFX_COMMAND_SIZE)
        return GFX_RET_ERROR;

    /* Граница 88/89 с DSP-стороны. Здесь она применяется, потому что размер
     * здесь — это размер одной команды.
     *
     * Для 0x10C7 она намеренно НЕ применяется: там размер — это байты всего
     * батча, и проверка 88 запретила бы батчить вовсе, что противоречит
     * пределу в 32 команды. Это открытый вопрос, а не решённое место. */
    if (cmd->size > GFX_DSP_CMD_SIZE_MAX)
        return GFX_RET_ERROR;

    return gfx_send_raw(cmd->code, (void *)cmd->bytes, cmd->size);
}