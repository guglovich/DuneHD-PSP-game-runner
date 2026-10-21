/*
 * Протокол команд GFX плеера — константы и сборка команды.
 *
 * Источник: Projects/Dune Max/docs/gfx-command-protocol.md, разбор
 * gfx_lib.c из поставки shell (сборка r11 от 19.09.2019). Здесь только то,
 * что нужно коду; сам разбор там же.
 *
 * Транспорт — буфер в DRAM плюс RUASetProperty. Регистры GFX из
 * userspace не пишутся и /dev/mum0 не открывается, поэтому этот путь
 * безопасен: опасность в том, какую поверхность мы отдаём DSP, а не в
 * способе отправки.
 */

#ifndef GFX_PROTOCOL_H
#define GFX_PROTOCOL_H

#include <stdint.h>

typedef uint32_t u32;
typedef uint8_t  u8;

/* Границы протокола. Проверка кода в SEND_GFX_COMMAND: idx = code - 0x10C4,
 * допустимо idx < 0x198, иначе ошибка "OTHER PROPERTY". */
#define GFX_CODE_MIN      0x10C4u
#define GFX_SLOT_COUNT    0x198u   /* 408 слотов */
#define GFX_CODE_MAX      (GFX_CODE_MIN + GFX_SLOT_COUNT - 1u)

/* Геометрия команды. sizeof(Command) = 56 байт, слот в буфере выровнен до
 * 64, максимум 32 команды в батче. */
#define GFX_COMMAND_SIZE  56u
#define GFX_SLOT_SIZE     64u
#define GFX_MAX_BATCH     32u

/* Границы размера команды, измерены на DSP-стороне (em8xxx.ko).
 *
 * Проверка `sltiu v0, v0, 89`: при размере 89 и больше возвращается 7.
 * При размере 88 флаг обнуляется — то есть 88 байт это последний
 * допустимый размер. */
#define GFX_DSP_CMD_SIZE_MAX  88u
#define GFX_DSP_CMD_SIZE_LIMIT 89u

/* Транспорт только 32-битный: байтовой записи нет вообще. Отсюда же
 * подтверждается правило обнуления буфера args — если бы DSP умел читать
 * байты, мусор в старших байтах слова Surface не имел бы значения. */
#define GFX_TRANSPORT_IS_32BIT_ONLY 1

/* Коды возврата SEND_GFX_COMMAND. 7 — backpressure, буфер не влезает и
 * отправку надо повторить; 6 — ошибка; прочее уходит в MY_fatal_error_f. */
#define GFX_RET_OK        0
#define GFX_RET_ERROR     6
#define GFX_RET_RETRY     7

/* Реализованные коды. Имена читаются прямо из заглушек обработчиков, каждая
 * содержит addiu $fp, $2, <смещение> и указывает на строку в пуле
 * base+0x7200. Остальные 357 слотов указывают на заглушку DRAMSize, то есть
 * не реализованы.
 *
 * Имена в обёртках MY_GFX_* — маркетинговые и протоколу не обязаны
 * совпадать: MY_GFX_CopyRectangle это 0x10DC MoveRectangle,
 * MY_GFX_CopyAndScaleRectangle — 0x10DF MoveAndScaleRectangle. */
enum {
    GFX_DRAM_SIZE              = 0x10C4,
    GFX_OPEN                   = 0x10C5,
    GFX_CLOSE                  = 0x10C6,
    GFX_COMMAND_BUFFER         = 0x10C7,
    GFX_CORRECTION             = 0x10C8,
    GFX_COLOR_FORMAT           = 0x10C9,
    GFX_ALPHA_FORMAT           = 0x10CA,
    GFX_SURFACE                = 0x10CB,
    GFX_BCS                    = 0x10CC,
    GFX_FILL_RECTANGLE         = 0x10CD,
    GFX_BLEND_RECTANGLES       = 0x10CE,
    GFX_BLEND_GRADIENT         = 0x10CF,
    GFX_RASTER_BLEND_GRADIENT  = 0x10D0,
    GFX_RASTER_BLEND_RECT      = 0x10D1,
    GFX_RASTER_GRADIENT        = 0x10D2,
    GFX_RASTER_RECTANGLES      = 0x10D3,
    GFX_RASTER_RECTANGLES_XY   = 0x10D4,
    GFX_REPLACE_GRADIENT       = 0x10D5,
    GFX_FILL_GRADIENT          = 0x10D6,
    GFX_SINGLE_COLOR_BLEND     = 0x10D7,
    GFX_BLEND_AND_SCALE        = 0x10D8,
    GFX_MOVE_RECTANGLE         = 0x10DC,
    GFX_REPLACE_RECTANGLE      = 0x10DD,
    GFX_REPLACE_AND_SCALE      = 0x10DE,
    GFX_MOVE_AND_SCALE         = 0x10DF,
    GFX_DISPLAY_PICTURE        = 0x10E0,
    GFX_WAIT_FOR_PICTURE       = 0x10E1,
    GFX_LINEAR_GRADIENT_SURF   = 0x10E2,
    GFX_RADIAL_GRADIENT_SURF   = 0x10E3,
    GFX_PALETTE_1BPP           = 0x10E4,
    GFX_PALETTE_2BPP           = 0x10E5,
    GFX_PALETTE_4BPP           = 0x10E6,
    GFX_PALETTE_8BPP           = 0x10E7,
    GFX_KEY_COLOR              = 0x10E8,
    GFX_FIELD_TYPE             = 0x10E9,
    GFX_NONLINEAR_SCALE        = 0x10EA,
    GFX_GLYPH_MASK             = 0x10EB,
    GFX_GLYPH_SCALE_MATRIX     = 0x10EC,
    GFX_LPF_THRESHOLDS         = 0x10ED,
    GFX_GLYPH_OUTPUT_SIZE      = 0x10EE,
    GFX_FLUSH_COMMAND_QUEUE    = 0x10EF,
    GFX_COMMAND_QUEUE_EMPTY    = 0x10F0,
    GFX_PICTURE_BUFFER_ADDR    = 0x10F1,
    GFX_COMMAND_QUEUE          = 0x10F2,
    GFX_ENABLE_ALPHA_FADING    = 0x10F3,
    GFX_HOLD_COMMAND_QUEUE     = 0x10F4,
    GFX_RELEASE_COMMAND_QUEUE  = 0x10F5,
    GFX_ALPHA_PALETTE          = 0x10F6,
    GFX_ADAPTATIVE_ENABLE      = 0x1203
};

/* Имя кода для журнала; для нереализованных слотов — "unimplemented". */
const char *gfx_code_name(u32 code);

/* Прямоугольник в пикселях. */
typedef struct {
    int x, y, w, h;
} GfxRect;

/* ПРАВИЛО, ОБЯЗАТЕЛЬНОЕ ДЛЯ ВСЕХ СБОРОК.
 *
 * SEND_GFX_COMMAND копирует args как есть, ровно size байт. Некоторые поля
 * уже u32, а некоторые — одиночный байт: в MY_GFX_Surface (0x10CB) это
 * args[2], который пишется инструкцией sb, то есть определён только младший
 * байт слова. Три старших байта остаются тем, что лежало в буфере.
 *
 * Отсюда: буфер args обнуляется целиком перед каждой сборкой, иначе в
 * команду уходит мусор из стека. Ниже это делает memset в каждой сборке,
 * и гейт tests/gfx_command_test.c проверяет, что хвост за полезной частью
 * нулевой.
 *
 * Затронуты команды с байтовыми полями: 0x10CB (+8), 0x10CE (+32),
 * 0x10D7 (+28), 0x10D8 (+40). У FillRectangle (0x10CD) байтовых полей нет,
 * но правило действует и для неё — обнуление стоит один вызов memset. */
#define GFX_ARGS_MUST_BE_ZEROED 1

/* Собранная команда: полезная часть 56 байт, слот в буфере — 64. */
typedef struct {
    u32 code;
    u32 size;                      /* размер полезной части, не слота */
    u8  bytes[GFX_COMMAND_SIZE];
} GfxCommand;

/* Отсечение прямоугольника по клипу, целиком на CPU.
 *
 * Возвращает 1, если прямоугольник остался непустым, и 0 если его нечем
 * рисовать. При успехе r изменяется на пересечение.
 *
 * Отсечение делается на CPU потому, что так делает сам shell: в
 * MY_GUI_paint_rect_no_blend пересечение считается через min/max на
 * movn, а clip-прямоугольники GFX не используются вовсе.
 */
int gfx_clip_rect(GfxRect *r, const GfxRect *clip);

/* Сборка команды FillRectangle (0x10CD).
 *
 * Раскладка восстановлена по эталонному вызову в MY_GUI_paint_rect_no_blend
 * и совпадает с разбором обёртки: пять слов, { x, y, ширина, высота, цвет }.
 * Размер 20 байт.
 *
 * Цвет — 32 бита RGBA, альфа в старшем байте. Непрозрачная заливка это
 * color | 0xFF000000, то есть в shell `lui $3,0xff00; or color,$10,$3`.
 *
 * Возвращает размер полезной части в байтах, либо 0 если прямоугольник
 * отсечён весь и отправлять нечего.
 */
u32 gfx_build_fill_rect(GfxCommand *cmd, const GfxRect *r, u32 rgba);

/* Сборка команды Surface (0x10CB).
 *
 * Единственная из разобранных, у которой два особых свойства.
 *
 * 1. **Байтовое поле.** args[2] — uint8_t, а не uint32_t: пишется sb по
 *    смещению +8, и три старших байта слова остаются мусором из стека.
 *    Здесь поле всегда 0, как и у вендора, но обнуление буфера обязательно
 *    в любом случае.
 *
 * 2. **Кэш в userspace.** Повтор с той же парой (a, b) не отправляется
 *    вовсе. То есть Surface меняет состояние движка и обязан идти первым
 *    перед рисованием — как glBindFramebuffer. Отсюда же: если отправку
 *    отбросить как «уже установлено», рисование пойдёт не на ту
 *    поверхность.
 *
 * Раскладка по разбору обёртки (0x562364), 24 байта:
 *   args[0] = engine
 *   args[1] = b
 *   args[2] = 0, определён только младший байт
 *   args[3] = a
 *   args[4] = 0
 *   args[5] = 0
 *
 * ВНИМАНИЕ, расхождение в исходных данных. docs/gfx-argmap.txt сопоставляет
 * слоты с регистрами как +0<-$7, +4<-$6, +12<-$5, тогда как
 * docs/gfx-command-protocol.md восстанавливает C как args[0]=engine($4).
 * Это разные отображения: по таблице args[0] приходит из четвёртого
 * аргумента, а не из первого. Здесь реализована версия из
 * gfx-command-protocol.md, потому что она даёт осмысленные имена полей.
 * Перед использованием на живом устройстве соответствие слотов и регистров
 * нужно подтвердить одним вызовом.
 *
 * Возвращает размер полезной части (24) либо 0, если состояние не изменилось
 * и отправлять нечего.
 */
u32 gfx_build_surface(GfxCommand *cmd, u32 engine, u32 a, u32 b);

/* Батч команд.
 *
 * Буфер команд — обычная куча libc (`realloc` подтверждён по .plt), слот
 * ровно 64 байта. То есть это наша память, а не память GFX: **MM0 и MM1 для
 * команд не нужны**, буфер уходит в DSP через RUASetProperty, а тот читает
 * DRAM. Следствие, из-за которого снята развилка: кадр можно держать в
 * обычной памяти Linux.
 *
 * Отправка батча — команда 0x10C7:
 *
 *     SEND_GFX_COMMAND(0x10C7, buffer, count << 6)
 *
 * То есть первый аргумент — **адрес буфера**, второй — **размер в байтах**,
 * а не в командах. Для всех прочих команд второй аргумент — размер
 * полезной части, то есть здесь исключение, и это главное, что легко перепутать.
 *
 * Правило роста, как в исходнике: старт 128 слотов; если нужен 129-й, то
 * сразу 8192, дальше удвоение до нужного. */
/* Предел на размер одной отправки: 32 команды по 64 байта — это предел батча
 * в самом GFX, и обойти его нельзя, а ловить переполнение лучше здесь. */
#define GFX_BATCH_MAX_BYTES (GFX_MAX_BATCH * GFX_SLOT_SIZE)

#define GFX_BATCH_INITIAL_SLOTS 128u
#define GFX_BATCH_MIN_SLOTS     0x2000u

typedef struct {
    u8 *slots;      /* буфер, слот GFX_SLOT_SIZE байт */
    u32 count;      /* заполнено слотов */
    u32 capacity;   /* выделено слотов */
} GfxBatch;

/* Подготовить батч. Ошибок не возвращает: при нехватке памяти слоты остаются
 * NULL, и gfx_batch_add() честно вернёт 0. */
void gfx_batch_begin(GfxBatch *b);

/* Добавить команду в батч. Копируется полезная часть, хвост слота
 * обнуляется: слот читается целиком, и мусор в нём ушёл бы в DSP.
 * Возвращает 1, если команда принята, и 0 при нехватке памяти или если
 * полезная часть не помещается в слот. */
int gfx_batch_add(GfxBatch *b, const GfxCommand *cmd);

/* Отправить батч. При коде возврата GFX_RET_RETRY — буфер не влезает,
 * отправка повторяется ограниченное число раз.
 *
 * Ограничение намеренное: вендор повторяет бесконечно, и при зависшем DSP
 * это бесконечный цикл вместо отказа. */
int gfx_batch_submit(const GfxBatch *b, unsigned max_retries);

/* Освободить буфер батча. */
void gfx_batch_end(GfxBatch *b);

/* Наблюдаемое состояние — чтобы проверять сборку на хосте, без подмены
 * транспорта. Символа SEND_GFX_COMMAND вне shell нет, и подменять его
 * указателем значило бы тащить в API то, чего в нём быть не должно. */
u32 gfx_batch_count(const GfxBatch *b);
u32 gfx_batch_capacity(const GfxBatch *b);
u32 gfx_batch_byte_size(const GfxBatch *b);
const u8 *gfx_batch_slot(const GfxBatch *b, u32 index);

/* Отправка команды в GFX.
 *
 * Адрес SEND_GFX_COMMAND берётся из глобальной области при первом обращении:
 * функция экспортирована бинарником shell, поэтому из нашей библиотеки,
 * загруженной в shell, она доступна напрямую.
 *
 * Возвращает код, который вернул SEND_GFX_COMMAND, либо GFX_RET_ERROR, если
 * символ не найден. Значение GFX_RET_RETRY означает backpressure: буфер не
 * влезает и отправку надо повторить.
 */
int gfx_send_command(const GfxCommand *cmd);

/* Отправка с явными кодом, адресом и размером.
 *
 * Нужна для 0x10C7: там второй аргумент — размер в байтах, а не размер
 * полезной части команды, и через gfx_send_command() это выразить нельзя.
 * Размер ограничен GFX_BATCH_MAX_BYTES. */
int gfx_send_raw(u32 code, void *args, u32 size);

#endif /* GFX_PROTOCOL_H */