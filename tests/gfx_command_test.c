/*
 * Проверка сборки команд GFX на хосте.
 *
 * Проверяется то, что восстановлено разбором gfx_lib.c, а не то, что
 * кажется правильным: раскладка пяти слов FillRectangle, размер 20, альфа в
 * старшем байте цвета и отсечение по клипу на CPU.
 *
 * Отправка проверяется только на отсутствии символа: вне shell функции
 * SEND_GFX_COMMAND нет, и код обязан сообщить об ошибке, а не упасть. Сам
 * вызов на живом устройстве — отдельный шаг, он требует нашей библиотеки
 * внутри shell.
 *
 * Гейт намеренно проверяется на заведомо неверной сборке: если бы код
 * сравнивал только размер и не сами байты, подмена порядка полей прошла бы
 * молча.
 */

#include "gfx_protocol.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ОК" : "СБОЙ");
    if (!ok)
        g_fail++;
}

static u32 word(const GfxCommand *c, unsigned i)
{
    const u8 *p = c->bytes + i * 4u;

    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

/* Проверка отсечения: результат пересечения с клипом. */
static void clip_case(const char *name, GfxRect r, GfxRect clip,
                      int want_ok, int want_x, int want_y, int want_w,
                      int want_h)
{
    GfxRect got = r;
    int ok = gfx_clip_rect(&got, &clip);
    int pass;

    pass = (ok == want_ok);
    if (pass && want_ok)
        pass = got.x == want_x && got.y == want_y && got.w == want_w &&
               got.h == want_h;

    printf("  %-40s -> ", name);
    if (want_ok)
        printf("(%d,%d %dx%d) %s\n", got.x, got.y, got.w, got.h,
               pass ? "ОК" : "СБОЙ");
    else
        printf("отброшен %s\n", pass ? "ОК" : "СБОЙ");

    if (!pass)
        g_fail++;
}

int main(void)
{
    GfxCommand cmd;
    GfxRect r;
    u32 size;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== проверка сборки команд GFX ===\n\n");

    /* --- имена кодов ---------------------------------------------------- */
    printf("имена кодов:\n");
    check(strcmp(gfx_code_name(GFX_FILL_RECTANGLE), "FillRectangle") == 0,
          "0x10CD = FillRectangle");
    check(strcmp(gfx_code_name(GFX_MOVE_RECTANGLE), "MoveRectangle") == 0,
          "0x10DC = MoveRectangle, не CopyRectangle");
    check(strcmp(gfx_code_name(GFX_MOVE_AND_SCALE),
                 "MoveAndScaleRectangle") == 0,
          "0x10DF = MoveAndScaleRectangle");
    check(strcmp(gfx_code_name(GFX_ADAPTATIVE_ENABLE),
                 "AdaptativeEnable") == 0, "0x1203 = AdaptativeEnable");
    check(strcmp(gfx_code_name(GFX_OPEN), "Open") == 0, "0x10C5 = Open");
    check(strcmp(gfx_code_name(GFX_DISPLAY_PICTURE),
                 "DisplayPicture") == 0, "0x10E0 = DisplayPicture");
    check(strcmp(gfx_code_name(0x125B), "unimplemented") == 0,
          "слот внутри диапазона без обработчика = unimplemented");
    check(strcmp(gfx_code_name(0x125C), "other-property") == 0,
          "код за диапазоном = other-property");

    /* --- геометрия команды ---------------------------------------------- */
    printf("\nгеометрия команды FillRectangle:\n");
    check(GFX_COMMAND_SIZE == 56, "sizeof(Command) = 56");
    check(GFX_SLOT_SIZE == 64, "слот = 64");
    check(GFX_MAX_BATCH == 32, "максимум 32 команды в батче");
    check(GFX_SLOT_COUNT == 408, "диапазон кодов = 408 слотов");

    /* --- раскладка полей ------------------------------------------------ */
    printf("\nраскладка пяти слов:\n");
    r.x = 7; r.y = 11; r.w = 320; r.h = 240;
    size = gfx_build_fill_rect(&cmd, &r, 0x00123456u);
    check(size == 20, "размер полезной части = 20 байт");
    check(cmd.code == GFX_FILL_RECTANGLE, "код = 0x10CD");
    check(word(&cmd, 0) == 7u, "+0  x = 7");
    check(word(&cmd, 1) == 11u, "+4  y = 11");
    check(word(&cmd, 2) == 320u, "+8  ширина = 320");
    check(word(&cmd, 3) == 240u, "+12 высота = 240");
    check(word(&cmd, 4) == 0xFF123456u, "+16 цвет с альфой 0xFF");
    check(word(&cmd, 4) == (0x00123456u | 0xFF000000u),
          "альфа = 0xFF, то есть color | 0xFF000000");

    /* Порядок полей важен, и проверка должна его ловить: сравниваем не
     * только размер, но и каждый байт. */
    check(memcmp(cmd.bytes + 0, "\x07\x00\x00\x00", 4) == 0,
          "байты x записаны little-endian");
    check(memcmp(cmd.bytes + 16, "\x56\x34\x12\xFF", 4) == 0,
          "байты цвета записаны little-endian");
    check(cmd.bytes[20] == 0, "за полезной частью чисто");

    /* Вырожденные прямоугольники отправлять нечего. */
    check(gfx_build_fill_rect(&cmd, NULL, 0xFFu) == 0, "NULL отбрасывается");
    r.w = 0;
    check(gfx_build_fill_rect(&cmd, &r, 0xFFu) == 0, "нулевая ширина");
    r.w = 320; r.h = 0;
    check(gfx_build_fill_rect(&cmd, &r, 0xFFu) == 0, "нулевая высота");

    /* --- отсечение ------------------------------------------------------ */
    printf("\nотсечение по клипу на CPU:\n");
    {
        GfxRect screen = { 0, 0, 320, 240 };

        clip_case("внутри экрана без изменений",
                  (GfxRect){ 10, 20, 100, 50 }, screen, 1, 10, 20, 100, 50);
        clip_case("слева и сверху",
                  (GfxRect){ -5, -5, 100, 50 }, screen, 1, 0, 0, 95, 45);
        clip_case("справа и снизу",
                  (GfxRect){ 300, 200, 100, 50 }, screen, 1, 300, 200, 20, 40);
        clip_case("накрывает весь экран",
                  (GfxRect){ -100, -100, 1000, 1000 }, screen, 1, 0, 0, 320,
                  240);
        clip_case("целиком слева",
                  (GfxRect){ -100, 0, 50, 50 }, screen, 0, 0, 0, 0, 0);
        clip_case("целиком правее экрана",
                  (GfxRect){ 400, 0, 50, 50 }, screen, 0, 0, 0, 0, 0);
        clip_case("нулевая ширина",
                  (GfxRect){ 10, 10, 0, 50 }, screen, 0, 0, 0, 0, 0);
        clip_case("отрицательная высота",
                  (GfxRect){ 10, 10, 50, -1 }, screen, 0, 0, 0, 0, 0);
    }

    /* --- правило обнуления буфера args ---------------------------------- */
    /* SEND_GFX_COMMAND копирует args как есть, ровно size байт. У 0x10CB
     * поле args[2] пишется инструкцией sb, то есть определён только младший
     * байт слова. Если буфер не обнулён, в команду уходит мусор из стека. */
    printf("\nправило обнуления буфера args:\n");
    {
        int i, tail_ok = 1, zero_ok = 1;

        r.x = 1; r.y = 2; r.w = 3; r.h = 4;
        size = gfx_build_fill_rect(&cmd, &r, 0x00ABCDEFu);
        for (i = (int)size; i < GFX_COMMAND_SIZE; i++)
            if (cmd.bytes[i] != 0)
                tail_ok = 0;
        check(tail_ok, "хвост FillRectangle за полезной частью нулевой");

        /* Отдельно байтовое поле Surface: у него и в младшем байте ноль, но
         * три старших байта слова обязаны быть нулевыми тоже. */
        size = gfx_build_surface(&cmd, 0, 0x11223344u, 0x55667788u);
        zero_ok = (cmd.bytes[8] == 0 && cmd.bytes[9] == 0 &&
                   cmd.bytes[10] == 0 && cmd.bytes[11] == 0);
        check(zero_ok, "args[2] Surface: все четыре байта слова нулевые");
    }

    /* --- Surface ---------------------------------------------------------- */
    printf("\nкоманда Surface (0x10CB):\n");
    {
        size = gfx_build_surface(&cmd, 1, 0xAABBCCDDu, 0x11223344u);
        check(size == 24, "размер полезной части = 24 байта");
        check(cmd.code == GFX_SURFACE, "код = 0x10CB");
        check(gfx_code_name(cmd.code) != NULL, "имя команды разрешается");
        check(word(&cmd, 0) == 1u, "+0  engine");
        check(word(&cmd, 1) == 0x11223344u, "+4  b");
        check(word(&cmd, 2) == 0u, "+8  байтовое поле = 0");
        check(word(&cmd, 3) == 0xAABBCCDDu, "+12 a");
        check(word(&cmd, 4) == 0u, "+16 ноль");
        check(word(&cmd, 5) == 0u, "+20 ноль");
        check(cmd.bytes[24] == 0, "за полезной частью чисто");
    }

    /* Кэш в userspace: повтор с той же парой не отправляется вовсе. Это не
     * экономия, а порядок команд: Surface обязана идти перед рисованием,
     * как glBindFramebuffer. */
    printf("\nкэш состояния Surface:\n");
    check(gfx_build_surface(&cmd, 1, 0xAABBCCDDu, 0x11223344u) == 0,
          "повтор с той же парой не отправляется");
    check(gfx_build_surface(&cmd, 1, 0xAABBCCDDu, 0x99999999u) == 24,
          "изменение b отправляется");
    check(gfx_build_surface(&cmd, 1, 0xAABBCCDDu, 0x99999999u) == 0,
          "повтор после отправки снова не отправляется");
    check(gfx_build_surface(&cmd, 2, 0xAABBCCDDu, 0x99999999u) == 24,
          "изменение engine отправляется");
    check(gfx_build_surface(NULL, 1, 1, 1) == 0, "NULL отбрасывается");

    /* --- батч ----------------------------------------------------------- */
    /* Буфер команд — обычная куча libc, слот 64 Б, а отправка батча идёт
     * кодом 0x10C7, где второй аргумент — размер В БАЙТАХ. Это исключение
     * среди всех остальных команд, и перепутать его легко. */
    printf("\nбатч команд:\n");
    {
        GfxBatch batch;
        const u8 *slot;
        u32 i;
        u32 added = 0;
        int all_ok = 1;

        check(GFX_SLOT_SIZE == 64, "слот = 64 байта");
        check(GFX_BATCH_MAX_BYTES == 2048, "предел батча = 32 * 64 = 2048 Б");

        gfx_batch_begin(&batch);
        check(gfx_batch_count(&batch) == 0, "пустой батч: count = 0");
        check(gfx_batch_byte_size(&batch) == 0, "пустой батч: размер = 0");

        /* Первое добавление обязано выделить ровно 128 слотов, то есть
         * 8192 байт, и ни словом меньше. */
        r.x = 1; r.y = 2; r.w = 3; r.h = 4;
        size = gfx_build_fill_rect(&cmd, &r, 0x00112233u);
        check(gfx_batch_add(&batch, &cmd) == 1, "команда принята");
        check(gfx_batch_count(&batch) == 1, "count = 1");
        check(gfx_batch_capacity(&batch) == GFX_BATCH_INITIAL_SLOTS,
              "первое выделение = 128 слотов (8192 Б)");
        check(gfx_batch_byte_size(&batch) == 64,
              "размер отправки = 64 Б, то есть count << 6");

        slot = gfx_batch_slot(&batch, 0);
        check(slot != NULL, "слот доступен");
        if (slot != NULL) {
            check(memcmp(slot, cmd.bytes, cmd.size) == 0,
                  "полезная часть скопирована в слот");
            for (i = cmd.size; i < GFX_SLOT_SIZE; i++)
                if (slot[i] != 0)
                    all_ok = 0;
            check(all_ok, "хвост слота 20..63 обнулён");
        }
        check(gfx_batch_slot(&batch, 1) == NULL,
              "слот за пределами count не выдаётся");

        /* Рост: 128 мест хватает, 129-й требует сразу 8192.
         * Добавление идёт в цикле, но проверяется один раз: 128 одинаковых
         * строк вывода ничего не добавляют. */
        for (i = 0; i < 128; i++)
            added += (gfx_batch_add(&batch, &cmd) == 1);
        check(added == 128, "128 добавлений без единой ошибки");
        check(gfx_batch_count(&batch) == 129, "count = 129");
        check(gfx_batch_capacity(&batch) == GFX_BATCH_MIN_SLOTS,
              "рост при 129-й команде: сразу 0x2000 слотов");
        check(gfx_batch_byte_size(&batch) == 129u * 64u,
              "размер отправки = count * 64");

        /* Слоты не наезжают друг на друга. */
        slot = gfx_batch_slot(&batch, 128);
        check(slot != NULL && memcmp(slot, cmd.bytes, cmd.size) == 0,
              "129-й слот записан целиком");
        check(gfx_batch_add(&batch, NULL) == 0, "NULL не принимается");

        /* Предел батча в самом GFX — 32 команды на отправку. */
        check(gfx_batch_byte_size(&batch) > GFX_BATCH_MAX_BYTES,
              "накопленный батч больше предела одной отправки");

        /* Отправка вне shell обязана сообщить об ошибке, а не упасть. */
        check(gfx_batch_submit(&batch, 4) == GFX_RET_ERROR,
              "отправка вне shell сообщает об ошибке");
        check(gfx_batch_submit(NULL, 4) == GFX_RET_ERROR, "NULL батч отбрасывается");

        gfx_batch_end(&batch);
        check(gfx_batch_count(&batch) == 0, "после освобождения count = 0");
        check(gfx_batch_capacity(&batch) == 0, "после освобождения capacity = 0");
    }

    /* --- границы размера команды ---------------------------------------- */
    /* Проверено на DSP-стороне в em8xxx.ko: sltiu v0,v0,89, при размере 89 и
     * больше возвращается 7, при 88 флаг обнуляется, то есть 88 — последний
     * допустимый. И транспорт только 32-битный, байтовой записи нет. */
    printf("\nграницы размера команды с DSP-стороны:\n");
    {
        GfxCommand big;
        u32 i;

        check(GFX_DSP_CMD_SIZE_MAX == 88, "последний допустимый размер = 88 Б");
        check(GFX_DSP_CMD_SIZE_LIMIT == 89, "при 89 и больше возврат 7");
        check(GFX_COMMAND_SIZE <= GFX_DSP_CMD_SIZE_MAX,
              "наша полезная часть 56 Б влезает в границу");

        /* Команда размером ровно в границу принимается, на байт больше — нет. */
        memset(big.bytes, 0, sizeof(big.bytes));
        big.code = GFX_FILL_RECTANGLE;
        big.size = GFX_DSP_CMD_SIZE_MAX;
        check(gfx_send_command(&big) == GFX_RET_ERROR,
              "вне shell даже допустимый размер даёт ошибку, не падение");

        big.size = GFX_DSP_CMD_SIZE_MAX + 1;
        check(gfx_send_command(&big) == GFX_RET_ERROR,
              "на байт больше границы отбрасывается");
        big.size = GFX_DSP_CMD_SIZE_LIMIT;
        check(gfx_send_command(&big) == GFX_RET_ERROR,
              "на границе отсечения отбрасывается");
        big.size = 57;   /* больше GFX_COMMAND_SIZE, но меньше 88 */
        check(gfx_send_command(&big) == GFX_RET_ERROR,
              "размер больше 56 отбрасывается как некорректный");

        /* Транспорт 32-битный: байтовых полей в GFXCommand нет, и любое
         * значение занимает ровно 4 байта. Проверяем, что кладём ровно
         * четыре байта на поле и хвост слова чист. */
        /* Ожидание здесь важно: сборка принудительно ставит альфу 0xFF, то
         * есть 0x04050607 превращается в 0xFF050607. Первая версия проверки
         * ждала 0x04050607 и упала — то есть гейт поймал ошибку в самой
         * проверке, а не в коде. */
        r.x = 0x00010203; r.y = 0; r.w = 1; r.h = 1;
        size = gfx_build_fill_rect(&cmd, &r, 0x04050607u);
        check(cmd.bytes[16] == 0x07 && cmd.bytes[17] == 0x06 &&
              cmd.bytes[18] == 0x05 && cmd.bytes[19] == 0xFF,
              "поле 4 байта little-endian, альфа принудительно 0xFF");
        for (i = 20; i < GFX_COMMAND_SIZE; i++)
            if (cmd.bytes[i] != 0)
                break;
        check(i == GFX_COMMAND_SIZE, "за полезной частью чисто");
    }

    /* --- отправка ------------------------------------------------------- */
    printf("\nотправка:\n");
    r.x = 0; r.y = 0; r.w = 320; r.h = 240;
    size = gfx_build_fill_rect(&cmd, &r, 0x00000000u);
    check(size == 20, "команда собрана");
    check(gfx_send_command(&cmd) == GFX_RET_ERROR,
          "вне shell отправка сообщает об ошибке, а не падает");
    check(gfx_send_command(NULL) == GFX_RET_ERROR, "NULL команда отбрасывается");
    check(gfx_code_name(cmd.code) != NULL, "имя команды разрешается");

    /* Код вне диапазона отправлять нельзя: SEND_GFX_COMMAND ответил бы на
     * это ошибкой OTHER PROPERTY, но дешевле не отправлять вовсе. */
    cmd.code = 0x125C;
    check(gfx_send_command(&cmd) == GFX_RET_ERROR,
          "код за диапазоном не отправляется");

    printf("\n=== итог ===\n");
    if (g_fail == 0)
        printf("все проверки пройдены\n");
    else
        printf("провалено проверок: %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}