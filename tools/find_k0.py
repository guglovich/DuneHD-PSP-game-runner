#!/usr/bin/env python3
"""Инструкции, пишущие в $k0 и $k1, строго по всему коду гостя.

Зачем. На плеере поток падает на `sw $s5,4($k0)` при `$k0 = -4`
(`si_addr` = 0). Кто наполняет `$k0`, из отчёта не видно: в кадре сигнала
этого регистра нет, а поиск по двум функциям вокруг сбоя ничего не нашёл.

Почему по ФАЙЛУ, а не по загруженному образу. Релокации HI16/LO16 меняют
только непосредственный операнд; поля rs/rt/rd не трогают. Значит НОМЕР
РЕГИСТРА в файле тот же, что в памяти. Адреса по файлу не годятся — для них
нужен `psp-hle dump`.

Почему только `.text`. По всему файлу вхождений 715, и почти все — данные:
`and $26,$0,$24` кодом не является. На MIPS `objdump` код от данных не
отличает (это записано в `Projects/AGENTS.md` как ловушка, стоившая неверных
чисел покрытия), поэтому границы берутся из таблицы секций ELF.

Про первую версию этого инструмента. Она определяла «пишет ли» по вхождению
в фиксированный список: SPECIAL с `rd` в (addu, or, and) плюс addiu/lui/lw/lb.
Этого **недостаточно**: `move` компилятор можетemit как `sll rd,rs,0`,
`movz`/`movn` тоже пишут `rd`, и все они попадали мимо. Такая неполнота даёт
ложный вывод «записей нет». Здесь назначение определяется по opcode.
"""

import struct
import sys

K0 = 26
K1 = 27

# Операции, у которых НЕТ регистра назначения.
#
# ВАЖНО: `0x1C` — это SPECIAL2, а НЕ regimm (regimm — `0x01`). Раньше оно стояло
# здесь с комментарием «regimm», из-за чего ВСЕ инструкции SPECIAL2, включая
# пишущие в `rd` (`clz`, `clo`, `mul`), молча пропускались. То же с `0x30`
# (`ll`): load linked РЕАЛЬНО пишет `rt`, и в `NO_DEST` ему не место.
NO_DEST = {0x04, 0x05, 0x06, 0x07,   # beq, bne, blez, bgtz
           0x14, 0x15, 0x16, 0x17,   # beql, bnel, blezl, bgtzl
           0x01, 0x02, 0x03,         # regimm: bltz, bgez, tge, ...
           0x2C, 0x2D,               # lwc1, ldc1 — пишут в FPR
           0x38}                     # sc — пишет rt только по результату, не считаем

# SPECIAL2 (op = 0x1C), MIPS32r2: функции, пишущие в `rd`.
SPECIAL2_RD = {0x02,                  # mul
              0x20, 0x21}            # clz, clo

# SPECIAL3 (op = 0x1F), MIPS32r2.
#
# ВАЖНО: это `0x1F`, а НЕ `0x1D`. `0x1D` — это COP2, сопроцессор, которого на
# Sigma нет. Ошибка была ровно в этом: первая правка объявила SPECIAL3 как
# `0x1D`, и `ext`/`ins` в отчёт не попали, хотя в бинарнике присутствовали.
SPECIAL3_RD = {0x02, 0x20}            # wsbh, bshfl
SPECIAL3_RT = {0x00,                  # ext
              0x04,                  # ins
              0x3B}                  # rdhwr

# Функции SPECIAL, пишущие в rd.
SPECIAL_RD = {0x00,  # sll
              0x02, 0x03,            # srl, sra
              0x04,                  # sllv
              0x06, 0x07,            # srlv, srav
              0x0A, 0x0B,            # movz, movn
              0x0C,                  # syscall (записи нет, но rd не пишет)
              0x10,                  # mfhi
              0x12,                  # mflo
              0x18, 0x19,            # mult, multu (в rd есть через hi/lo)
              0x21, 0x23, 0x24, 0x25, 0x26, 0x27,   # addu, subu, and, or, xor, nor
              0x2A, 0x2B}            # slt, sltu

# Функции SPECIAL, пишущие только в hi/lo.
SPECIAL_HILO = {0x08, 0x09, 0x0D, 0x0E, 0x11, 0x13,
                0x1A, 0x1B, 0x1E, 0x1F}

# Операции с одним регистром назначения в rt.
# 0x2F (`pref`) здесь нет НАМЕРЕННО: это подсказка процессору, она ничего
# не пишет, но номер регистра в ней есть. С ним поиск выдавал единственное
# ложное вхождение (`pref $k1,0($s2)`) и на нём строился вывод о записи.
OP_RT = {0x08, 0x09, 0x0A, 0x0B, 0x0D, 0x0F, 0x0C, 0x0E,
         0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B,
         0x30}                  # ll — load linked, пишет rt


def dests(w):
    """Регистры, в которые инструкция пишет."""
    op = w >> 26
    fn = w & 63
    rt = (w >> 16) & 31
    rd = (w >> 11) & 31
    out = set()
    if op == 0:
        if fn in SPECIAL_RD:
            out.add(rd)
        elif fn not in SPECIAL_HILO:
            pass
        return out
    if op == 0x1C:
        if fn in SPECIAL2_RD:
            out.add(rd)
        return out
    if op == 0x1F:
        if fn in SPECIAL3_RD:
            out.add(rd)
        if fn in SPECIAL3_RT:
            out.add(rt)
        return out
    if op in NO_DEST:
        return out
    if op in OP_RT:
        out.add(rt)
    return out


def decode(w):
    op = w >> 26
    rs = (w >> 21) & 31
    rt = (w >> 16) & 31
    rd = (w >> 11) & 31
    fn = w & 63
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm >= 0x8000 else imm
    if op == 0:
        names = {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x08: "jr", 0x09: "jalr",
                 0x10: "mfhi", 0x12: "mflo", 0x18: "mult", 0x21: "addu",
                 0x23: "subu", 0x24: "and", 0x25: "or", 0x26: "xor",
                 0x27: "nor", 0x2A: "slt", 0x2B: "sltu", 0x0A: "movz",
                 0x0B: "movn"}
        return "%s rd=$%d rs=$%d rt=$%d" % (names.get(fn, "special%02x" % fn),
                                            rd, rs, rt)
    names = {0x09: "addiu", 0x0F: "lui", 0x23: "lw", 0x2B: "sw", 0x20: "lb",
             0x24: "lbu", 0x0D: "ori", 0x08: "addi", 0x25: "lhu", 0x29: "sh"}
    if op == 0x1C:
        s2 = {0x02: "mul", 0x20: "clz", 0x21: "clo", 0x00: "madd",
              0x01: "maddu", 0x02 + 0x100: "msub"}
        return "%s rd=$%d rs=$%d rt=$%d" % (s2.get(fn, "special2_%02x" % fn),
                                            rd, rs, rt)
    if op == 0x1F:
        s3 = {0x00: "ext", 0x04: "ins", 0x02: "wsbh", 0x20: "bshfl",
              0x3B: "rdhwr"}
        return "%s rt=$%d rs=$%d imm=%d" % (s3.get(fn, "special3_%02x" % fn),
                                            rt, rs, simm)
    return "%s rt=$%d rs=$%d imm=%d" % (names.get(op, "op%02x" % op), rt, rs,
                                         simm)


def main(path):
    sys.path.insert(0, __file__.rsplit("/", 1)[0])
    from code_sections import sections

    data = open(path, "rb").read()
    # Отбор по ФЛАГУ SHF_EXECINSTR, а не по имени секции. По имени
    # `.sceStub.text` (стабы импортов, флаги 0x6) не проходила, а это
    # исполняемый код.
    spans = [(off, off + size) for n, t, fl, a, off, size in sections(data)
             if (fl & 0x4) and size > 0]
    if not spans:
        raise SystemExit("секций кода не найдено")
    print("секций кода: %d" % len(spans))

    hits = []
    for off in range(0, len(data) - 3, 4):
        if not any(a <= off < b for a, b in spans):
            continue
        w = struct.unpack_from("<I", data, off)[0]
        d = dests(w)
        if K0 in d or K1 in d:
            hits.append((off, w, decode(w), sorted(d & {K0, K1})))

    print("вхождений записи в $k0/$k1: %d" % len(hits))
    for off, w, txt, which in hits[:80]:
        reg = "$k0" if K0 in which else "$k1"
        print("  0x%08X: %08X  %-28s -> %s" % (off, w, txt, reg))
    if len(hits) > 80:
        print("  ... ещё %d" % (len(hits) - 80))


if __name__ == "__main__":
    main(sys.argv[1])
