/*
 * Проверка вычисления `$gp` по прологу MIPS-функции.
 *
 * Зачем. В рантайме `$gp` подставляется в трамплин абсолютным числом, и
 * ошибка в знаковом расширении непосредственного операнда `addiu` стоила
 * ровно 0x10000: GOT-слот читался не оттуда, и падение выглядело как «гость
 * свалился внутри нашего кода». Проверка нужна, потому что пролог собранного
 * бинарника может иметь `lo` меньше 0x8000 — тогда ошибка расширения на нём
 * не видна вовсе, и гейт был бы слеп ровно к тому дефекту, ради которого
 * написан. Поэтому знаковое расширение проверяется на синтетическом `lo`
 * больше 0x7FFF отдельно от пролога.
 *
 * Что проверяется:
 *   1. распознаётся ли пролог `lui $gp / addiu $gp / addu $gp,$gp,$t9`;
 *   2. совпадает ли вычисленный `_gp` с символом `_gp` из таблицы символов;
 *   3. расширяется ли `lo` знаком при любом значении.
 *
 * Использование: gpcheck <файл> <смещение_функции> <ожидаемый_gp>
 * Код возврата 0 — всё верно, иначе 1 (проверка не пройдена) или 2 (ошибка ввода).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* Дельта, которую прибавляет пролог к `$t9`: `lui $gp,hi ; addiu $gp,gp,lo`.
 * `addiu` расширяет непосредственный операнд знаком — это и проверяется. */
static uint32_t gp_delta(const uint32_t *p)
{
    uint32_t hi, lo;

    if ((p[0] & 0xFFFF0000u) != 0x3C1C0000u)
        return 0;
    if ((p[1] & 0xFFFF0000u) != 0x279C0000u)
        return 0;
    if (p[2] != 0x0399E021u)
        return 0;
    hi = p[0] & 0xFFFFu;
    lo = p[1] & 0xFFFFu;
    return (hi << 16) + (uint32_t)(int32_t)(int16_t)(uint16_t)lo;
}

static uint32_t sign_extend_lo(uint32_t lo)
{
    return (uint32_t)(int32_t)(int16_t)(uint16_t)lo;
}

/* Знаковое расширение проверяется напрямую по ожидаемому значению, а НЕ
 * сравнением с «как сделано без знака»: такой способ сравнения проходит и на
 * сломанном коде — именно так первый вариант гейта и был пойман. */
static int check_sign_extension(uint32_t hi, uint32_t lo)
{
    uint32_t syn[3];
    uint32_t delta, expect;

    syn[0] = 0x3C1C0000u | hi;
    syn[1] = 0x279C0000u | lo;
    syn[2] = 0x0399E021u;

    delta = gp_delta(syn);
    expect = (hi << 16) + sign_extend_lo(lo);
    if (delta == expect) {
        printf("  lo=0x%04X: дельта 0x%05X — верно\n", lo, delta);
        return 0;
    }
    printf("  lo=0x%04X: дельта 0x%05X, ожидалось 0x%05X — РАСХОЖДЕНИЕ\n",
           lo, delta, expect);
    return 1;
}

int main(int argc, char **argv)
{
    FILE *f;
    unsigned off;
    uint32_t want, p[3], delta, gp;
    int rc = 0;

    if (argc != 4) {
        fprintf(stderr, "Использование: gpcheck <файл> <смещение> <ожидаемый_gp>\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    off = (unsigned)strtoul(argv[2], NULL, 0);
    want = (uint32_t)strtoul(argv[3], NULL, 0);
    if (fseek(f, off, SEEK_SET) != 0 || fread(p, 4, 3, f) != 3) {
        fprintf(stderr, "Не прочитан пролог по смещению 0x%X\n", off);
        fclose(f);
        return 2;
    }
    fclose(f);

    printf("пролог по файлу 0x%05X: %08X %08X %08X\n", off, p[0], p[1], p[2]);
    delta = gp_delta(p);
    if (delta == 0) {
        printf("  пролог НЕ распознан\n");
        return 1;
    }
    gp = off + delta;
    printf("  дельта = 0x%05X, _gp = 0x%05X, ожидалось 0x%05X -> %s\n", delta,
           gp, want, gp == want ? "совпало" : "РАСХОЖДЕНИЕ");
    if (gp != want)
        rc = 1;

    printf("знаковое расширение addiu:\n");
    rc |= check_sign_extension(0x0002u, 0x9000u);   /* обязан дать -0x7000 */
    rc |= check_sign_extension(0x0002u, 0x4CFCu);   /* без знака не изменилось бы */

    return rc;
}