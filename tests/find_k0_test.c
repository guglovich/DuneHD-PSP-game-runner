/*
 * Самопроверка инструмента `tools/find_k0.py`.
 *
 * Зачем. Инструмент ищет инструкции, пишущие в `$k0`/`$k1`. Первая его версия
 * нашла НОЛЬ таких инструкций во всём коде гостя (2,9 МБ), и это выглядело
 * как результат. Настоящая проверка показала: **ноль невозможен** — компилятор
 * использует `$k0` как рабочий регистр постоянно, и ноль вхождений означает
 * брак инструмента, а не свойство гостя.
 *
 * Затем инструмент проверяли на `psp-hle` — и это тоже было негодно: к тому
 * моменту подмена `$k0` в `guest_enter` была откачена, то есть проверять было
 * нечего, и инструмент «проходил» на заведомо годном входе.
 *
 * Здесь каждая форма записи перечислена явно, и ожидаемые адреса известны заранее.
 */

#include <stdint.h>

/* Каждая функция пишет в $k0 или $k1 РОВНО ОДНИМ способом. Если инструмент
 * пропустит хоть одну, он негоден. */

uint32_t write_addu(uint32_t a, uint32_t b)
{
    __asm__ volatile("addu $26,%0,%1" : : "r"(a), "r"(b));
    return 26;
}

uint32_t write_or(uint32_t a)
{
    __asm__ volatile("or $26,%0,$0" : : "r"(a));
    return 26;
}

uint32_t write_addiu(uint32_t a)
{
    __asm__ volatile("addiu $26,%0,4" : : "r"(a));
    return 26;
}

uint32_t write_lui(void)
{
    __asm__ volatile("lui $26,0x1234");
    return 26;
}

uint32_t write_lw(uint32_t a)
{
    __asm__ volatile("lw $26,0(%0)" : : "r"(a));
    return 26;
}

/* Первая версия инструмента эти три формы пропускала: `sll rd,rs,0`,
 * `movz` и `movn` пишут `rd`, но их не было в списке. */

uint32_t write_sll(uint32_t a)
{
    __asm__ volatile("sll $26,%0,0" : : "r"(a));
    return 26;
}

uint32_t write_movz(uint32_t a, uint32_t c)
{
    __asm__ volatile("movz $26,%0,%1" : : "r"(a), "r"(c));
    return 26;
}

uint32_t write_movn(uint32_t a, uint32_t c)
{
    __asm__ volatile("movn $26,%0,%1" : : "r"(a), "r"(c));
    return 26;
}

/* --- формы, добавленные 2026-10-10 после разбора противоречия ----------- */
/* В `find_k0.py` эти опкоды не обрабатывались вовсе: SPECIAL2 (`0x1C`) стоял
 * в `NO_DEST` с ошибочным комментарием «regimm», SPECIAL3 (`0x1D`) не был
 * упомянут, а `ll` (`0x30`) тоже был зачислен в `NO_DEST`, хотя пишет `rt`.
 * Инструмент проходил самопроверку и при этом молчал на трёх формах. */

uint32_t write_mul(uint32_t a, uint32_t b)
{
    /* SPECIAL2, funct 0x02 — доступен на MIPS32r2. */
    __asm__ volatile(".set push\n\t.set mips32r2\n\tmul $26,%0,%1\n\t.set pop"
                     : : "r"(a), "r"(b));
    return 26;
}

uint32_t write_clz(uint32_t a)
{
    /* SPECIAL2, funct 0x20. */
    __asm__ volatile(".set push\n\t.set mips32r2\n\tclz $26,%0\n\t.set pop"
                     : : "r"(a));
    return 26;
}

uint32_t write_ext(uint32_t a)
{
    /* SPECIAL3, funct 0x00 — назначение в `rt`. */
    __asm__ volatile(".set push\n\t.set mips32r2\n\text $26,%0,4,8\n\t.set pop"
                     : : "r"(a));
    return 26;
}

uint32_t write_ins(uint32_t a)
{
    /* SPECIAL3, funct 0x04 — назначение в `rt`. */
    __asm__ volatile(".set push\n\t.set mips32r2\n\tins $26,%0,4,8\n\t.set pop"
                     : : "r"(a));
    return 26;
}

/* Контроль: писать НЕ должен. Инструмент, считающий такие слова
 * вхождениями, даёт ложные срабатывания. */

uint32_t no_write_sll_zero(uint32_t a)
{
    /* `sll $0,...` пишет в $zero, а не в $k0. */
    __asm__ volatile("sll $0,%0,2" : : "r"(a));
    return 26;
}

uint32_t no_write_pref(uint32_t a)
{
    /* `pref` — подсказка процессору, ничего не пишет, но номер регистра
     * в ней есть. Первая версия инструмента считала это записью. */
    __asm__ volatile("pref 0,0(%0)" : : "r"(a));
    return 26;
}

uint32_t no_write_sw(uint32_t a, uint32_t v)
{
    /* Запись в ПАМЯТЬ по $k0, а не в сам регистр. */
    __asm__ volatile("sw %1,0(%0)" : : "r"(a), "r"(v) : "memory");
    return 26;
}

uint32_t no_write_madd(uint32_t a, uint32_t b)
{
    /* SPECIAL2 `madd` пишет в `hi`/`lo`, а НЕ в регистр назначения. Если бы
     * инструмент считал его записью, появилось бы ложное срабатывание. */
    __asm__ volatile(".set push\n\t.set mips32r2\n\tmadd %0,%1\n\t.set pop"
                     : : "r"(a), "r"(b));
    return 26;
}

int main(void)
{
    volatile uint32_t x = 1;

    return (int)(write_addu(x, x) + write_or(x) + write_addiu(x) + write_lui()
                 + write_lw((uintptr_t)&x) + write_sll(x) + write_movz(x, x)
                 + write_movn(x, x)
                 + write_mul(x, x) + write_clz(x) + write_ext(x) + write_ins(x)
                 + no_write_sll_zero(x)
                 + no_write_pref((uintptr_t)&x) + no_write_sw((uintptr_t)&x, x)
                 + no_write_madd(x, x));
}
