/*
 * Гейт релокации HI16/LO16 на НАСТОЯЩЕМ образе гостя и на НАСТОЯЩЕЙ функции.
 *
 * Зачем отдельно от `guest_load_test.c`. Тот проверяет, что указатели после
 * релокации попадают внутрь образа. Порог слишком мягкий: указатель,
 * уехавший на 64 КиБ, ВСЁ РАВНО попадает внутрь образа и гейт зелёный.
 * Именно так и было: 5908 указателей из 24571 пары HI16/LO16 были смещены
 * на 64 КиБ вниз, гейты зелёные, а гость на первом же конструкторе `.cplinit`
 * читал слово `0xFFFFFFFF` и разыменовал его. Прогон на плеере обрывался
 * на `pc=0x08AD0C14`, и до входа в гостя дело не доходило ни разу.
 *
 * ЧТО ПРОВЕРЯЕТСЯ. Инвариант пары, а не «правдоподобность адреса»:
 *
 *     hi<<16 + sext16(lo)  ==  A + S
 *
 * где `A` — адрес из файла (адденд пары HI16/LO16), `S` — адрес сегмента
 * в загруженном образе. Это определение релокации MIPS, и оно не допускает
 * «почти»: каждая из 24571 пары обязана дать точное равенство.
 *
 * ПАРИРОВАНИЕ повторяет загрузчик дословно: HI16 берёт СЛЕДУЮЩУЮ запись
 * LO16 с тем же `identity`, и адрес сегмента берётся у ПАРЫ, а не у HI16.
 * Иначе гейт проверял бы пары, которых в загрузчике нет.
 *
 * ВТОРОЙ ГЕЙТ — на способность теста ловить сам дефект. Прежняя формула
 * считается здесь же и обязана дать ровно 5908 расхождений. Если даст
 * ноль — значит разбор образа сломан, тест ничего не доказывает, и он
 * падает. Это проверка «на намеренно испорченной копии», встроенная в сам
 * тест: без неё зелёный результат означал бы только «автор посчитал так же».
 *
 * Числа 120719 и 24571 сверяются с отчётом плеера: поле `reloc_total` там
 * 0x1D78F = 120719, `reloc_failed` = 0. Совпадение означает, что образ и
 * пары разобраны так же, как в загрузчике на устройстве.
 *
 * Запуск: ./reloc_hi_test [путь к guest.elf]
 */

#include "guest_reloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELOC_TOTAL_DEVICE   120719u   /* 0x1D78F из отчёта плеера   */
#define RELOC_HI_DEVICE      24571u    /* замер по файлу             */
#define RELOC_BAD_OLD_DEVICE 5908u     /* сколько ломала прежняя формула */

#define R_MIPS_32   2u
#define R_MIPS_26   4u
#define R_MIPS_HI16 5u
#define R_MIPS_LO16 6u

#define PT_LOAD     1u

typedef struct {
    u32 vaddr;
    u32 offset;
    u32 filesz;
} Seg;

static unsigned char *g_file;
static size_t         g_len;

static int g_fail;

static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "ok  " : "СБОЙ", what);
    if (!ok)
        g_fail++;
}

static u32 rd32(const void *p)
{
    const unsigned char *b = (const unsigned char *)p;

    return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16)
         | ((u32)b[3] << 24);
}

static s16 rd16s(const void *p)
{
    const unsigned char *b = (const unsigned char *)p;

    return (s16)(u16)((u32)b[0] | ((u32)b[1] << 8));
}

/* Знаковое расширение младших 16 бит СЛОВА. Отдельная функция, а не
 * `rd16s(&word)`: чтение через адрес переменной зависит от порядка байтов
 * ХОСТА, а на этой ошибке проект уже ловил перевёрнутые инструкции. */
static s32 sext_lo(u32 word)
{
    return (s32)(s16)(u16)(word & 0xFFFFu);
}

/* Прежняя формула — НАМЕРЕННО ОСТАВЛЕННАЯ здесь и больше нигде.
 *
 * Нужна только чтобы тест доказвал, что он вообще ловит дефект. Считает
 * `hi` как `(hi16_адденда + S) >> 16`, то есть опускает младшую половину
 * адреса. Её не надо «чинить»: она должна ровно воспроизводить ошибку,
 * ради которой написан гейт. */
static u32 relocate_hi_old(u32 op, s32 lo_signed, u32 target)
{
    u32 value = (op & 0xFFFFu) << 16;
    u32 corrected;

    value += (u32)lo_signed;
    value += target;
    corrected = value - (u32)lo_signed;
    return (op & 0xFFFF0000u) | ((corrected >> 16) & 0xFFFFu);
}

static int load(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        printf("  [СБОЙ] не открылся %s\n", path);
        g_fail++;
        return 0;
    }
    fseek(f, 0, SEEK_END);
    g_len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = (unsigned char *)malloc(g_len);
    if (g_file == NULL || fread(g_file, 1, g_len, f) != g_len) {
        printf("  [СБОЙ] не прочитался %s\n", path);
        g_fail++;
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "tmp/real/guest.elf";
    u32 phoff, shoff;
    u16 phentsize, phnum, shentsize, shnum, shstrndx;
    u32 seg_n = 0;
    Seg seg[8];
    u32 i;
    u32 total = 0, n_hi = 0, unpaired = 0;
    u32 bad_now = 0, bad_old = 0;
    u32 bad_pair = 0;
    const unsigned char *shstr;
    u32 first_bad_hi = 0, first_bad_old = 0;

    printf("=== релокация HI16/LO16: инвариант пары, а не похожесть адреса ===\n");

    if (!load(path))
        return 1;

    /* ELF32, little endian. Проверка магии обязательна: без неё далёкий
     * сдвиг читается как заголовок и разбор идёт молча по мусору. */
    if (g_len < 52 || rd32(g_file) != 0x464C457Fu) {
        printf("  [СБОЙ] это не ELF32: %s\n", path);
        return 1;
    }

    phoff    = rd32(g_file + 28);
    shoff    = rd32(g_file + 32);
    phentsize = rd16s(g_file + 42);
    phnum     = rd16s(g_file + 44);
    shentsize = rd16s(g_file + 46);
    shnum     = rd16s(g_file + 48);
    shstrndx  = rd16s(g_file + 50);

    /* Сегменты: номер сегмента в r_info — это порядковый номер PT_LOAD. */
    for (i = 0; i < phnum && seg_n < 8; i++) {
        const unsigned char *p = g_file + (size_t)phoff + (size_t)i * phentsize;

        if (rd32(p) != PT_LOAD)
            continue;
        seg[seg_n].vaddr  = rd32(p + 8);
        seg[seg_n].offset = rd32(p + 4);
        seg[seg_n].filesz = rd32(p + 16);
        seg_n++;
    }
    check(seg_n >= 1, "найдены сегменты PT_LOAD");

    shstr = g_file + rd32(g_file + (size_t)shoff
                          + (size_t)shstrndx * shentsize + 16);

    for (i = 0; i < shnum; i++) {
        const unsigned char *sh = g_file + (size_t)shoff + (size_t)i * shentsize;
        const char *name;
        u32 off, size, entsz;
        u32 k;
        u32 *r_off, *r_info;

        name = (const char *)shstr + rd32(sh);
        if (strncmp(name, ".rel", 4) != 0)
            continue;
        entsz = rd32(sh + 36);
        off   = rd32(sh + 16);
        size  = rd32(sh + 20);
        if (entsz != 8 || off + size > g_len)
            continue;

        r_off  = (u32 *)malloc(sizeof(u32) * (size / 8));
        r_info = (u32 *)malloc(sizeof(u32) * (size / 8));
        if (r_off == NULL || r_info == NULL) {
            printf("  [СБОЙ] нет памяти под %s\n", name);
            g_fail++;
            free(r_off);
            free(r_info);
            continue;
        }
        for (k = 0; k < size / 8; k++) {
            r_off[k]  = rd32(g_file + off + k * 8);
            r_info[k] = rd32(g_file + off + k * 8 + 4);
            total++;
        }

        for (k = 0; k < size / 8; k++) {
            u32 type = r_info[k] & 0xFu;
            u32 patch_seg, tgt_hi, pair_j = (u32)-1;
            u32 patch_rel, patch_addr, orig_hi;
            u32 tgt, orig_lo, lo_signed, lo_word, hi_word, got, want;
            u32 j;

            if (type == R_MIPS_32 || type == R_MIPS_26 || type == R_MIPS_LO16)
                continue;
            if (type != R_MIPS_HI16)
                continue;

            n_hi++;

            /* Пары ищем ровно как в загрузчике: первая после текущей запись
             * LO16 с тем же identity (оно же — младшие 8 бит r_info). */
            for (j = k + 1; j < size / 8; j++) {
                u32 t2 = r_info[j] & 0xFu;

                if (t2 == R_MIPS_HI16)
                    continue;
                if (t2 != R_MIPS_LO16)
                    continue;
                if ((r_info[j] >> 8) != (r_info[k] >> 8))
                    continue;
                pair_j = j;
                break;
            }
            if (pair_j == (u32)-1) {
                unpaired++;
                continue;
            }

            patch_seg = (r_info[k] >> 8) & 0xFFu;
            tgt_hi    = (r_info[pair_j] >> 16) & 0xFFu;   /* сегмент у ПАРЫ */
            if (patch_seg >= seg_n || tgt_hi >= seg_n) {
                bad_pair++;
                continue;
            }

            patch_rel  = r_off[k];
            patch_addr = seg[patch_seg].offset
                       + (patch_rel - seg[patch_seg].vaddr);
            if (patch_addr + 4 > g_len) {
                bad_pair++;
                continue;
            }
            orig_hi = rd32(g_file + patch_addr);

            patch_rel  = r_off[pair_j];
            patch_addr = seg[(r_info[pair_j] >> 8) & 0xFFu].offset
                       + (patch_rel - seg[(r_info[pair_j] >> 8) & 0xFFu].vaddr);
            if (patch_addr + 4 > g_len) {
                bad_pair++;
                continue;
            }
            orig_lo = rd32(g_file + patch_addr);

            /* Цель — адрес сегмента в загруженном образе, как в загрузчике:
             * `m->segments[t].runtime`, то есть PSP_USER_BASE + vaddr. */
            tgt = PSP_USER_BASE + seg[tgt_hi].vaddr;

            lo_signed = (u32)sext_lo(orig_lo);
            lo_word   = guest_relocate_lo(orig_lo, tgt);
            hi_word   = guest_relocate_hi(orig_hi, (s32)lo_signed, tgt);
            got       = guest_pair_value(hi_word, lo_word);

            /* Намеренный адрес: адденд (адрес из файла) плюс цель. */
            want = (((u32)sext_lo(orig_hi)) << 16)
                 + (u32)sext_lo(orig_lo)
                 + tgt;

            if (got != want) {
                bad_now++;
                if (first_bad_hi == 0)
                    first_bad_hi = r_off[k];
            }

            /* Контроль: та же пара, но прежняя формула. Должна расходиться —
             * иначе тест не способен увидеть исходный дефект. */
            hi_word = relocate_hi_old(orig_hi, (s32)lo_signed, tgt);
            if (guest_pair_value(hi_word, lo_word) != want) {
                bad_old++;
                if (first_bad_old == 0)
                    first_bad_old = r_off[k];
            }
        }
        free(r_off);
        free(r_info);
    }

    printf("  разобрано релокаций      : %u   (на плеере reloc_total = %u)\n",
           total, RELOC_TOTAL_DEVICE);
    printf("  из них HI16              : %u\n", n_hi);
    printf("  HI16 без пары            : %u\n", unpaired);
    printf("  несопоставимых адресов   : %u\n", bad_pair);
    printf("  НЫНЯШНЯЯ формула: неверных пар %u", bad_now);
    if (bad_now != 0)
        printf("  (первая на vaddr 0x%08X)", first_bad_hi);
    printf("\n");
    printf("  ПРЕЖНЯЯ  формула: неверных пар %u", bad_old);
    if (bad_old != 0)
        printf("  (первая на vaddr 0x%08X)", first_bad_old);
    printf("\n\n");

    check(total == RELOC_TOTAL_DEVICE,
          "число релокаций совпало с отчётом плеера (120719)");
    check(n_hi == RELOC_HI_DEVICE, "число пар HI16 совпало с замером (24571)");
    check(unpaired == 0, "у каждого HI16 нашлась пара LO16 (reloc_failed = 0)");
    check(bad_pair == 0, "адреса патчей и целей сегментов читаются без срезов");
    check(bad_now == 0,
          "ИНВАРИАНТ: hi<<16 + sext16(lo) == A + S для ВСЕХ пар HI16/LO16");
    /* Контроль способности теста: без него зелёный результат ничего не
     * доказывает — автор мог бы считать так же неверно, как и код. */
    check(bad_old == RELOC_BAD_OLD_DEVICE,
          "контроль: прежняя формула даёт ровно 5908 расхождений");

    printf("=== тест %s ===\n", g_fail == 0 ? "пройден" : "провален");
    return g_fail == 0 ? 0 : 1;
}
