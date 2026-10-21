#!/usr/bin/env python3
"""Сколько в коде гостя обращений через $gp (регистр 28).

Зачем. На плеере в отчёте о сбое потока `$fp` = `0x2AAC86AC`, `$a2` =
`0x2AAC8000`, `$t0` = `0x2AACC0D0` — это адреса **нашего** образа, попавшие в
регистры гостя. Почему они там: трамплин выставляет гостю `$gp` = `_gp` нашего
бинарника, и если гость обращается к своим данным через `$gp`, он читает наш
GOT.

Прежняя запись в `Projects/AGENTS.md` утверждает «ноль обращений через `$gp` в
исполняемых секциях». Она проверяется здесь заново: та же логика уже дала ложный
«ноль» по `$k0` (инструмент определял назначение регистра по списку опкодов и
пропускал `move` через `sll`).

Определяется по ПОЛЕЮ БАЗЫ, а не по мнемонике: у `lw`/`sw`/`lb`/`sb` и всех
аналогов это биты 25..21. Наличие `lui`/`addu ...,$t9` рядом для вычисления
`$gp` не считается обращением через `$gp`.

Ограничение: только секции с флагом `SHF_EXECINSTR`. На MIPS `objdump` код от
данных не отличает, и по всему файлу таких мест гораздо больше.
"""

import struct
import sys

GP = 28

# Операции, у которых биты 25..21 — это база памяти.
LOAD_STORE = {
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu",
    0x26: "lwr", 0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
    0x2F: "cache", 0x30: "ll", 0x38: "sc",
}

# REGIMM: условные переходы, база = rs.
REGIMM_BRANCH = {0x00, 0x01, 0x10, 0x11}


def main(path):
    sys.path.insert(0, __file__.rsplit("/", 1)[0])
    from code_sections import sections

    data = open(path, "rb").read()
    spans = [(off, off + size) for n, t, fl, a, off, size in sections(data)
             if (fl & 0x4) and size > 0]
    if not spans:
        raise SystemExit("секций кода не найдено")

    total = 0
    hits = []
    for off in range(0, len(data) - 3, 4):
        if not any(a <= off < b for a, b in spans):
            continue
        w = struct.unpack_from("<I", data, off)[0]
        op = w >> 26
        rs = (w >> 21) & 31
        if op in LOAD_STORE and rs == GP:
            hits.append((off, w, LOAD_STORE[op]))
            total += 1
        elif op == 0x01 and rs == GP:
            hits.append((off, w, "regimm/branch"))
            total += 1

    print("инструкций в коде: %d" % sum((b - a) // 4 for a, b in spans))
    print("обращений через $gp (база памяти = 28): %d" % total)
    for off, w, name in hits[:40]:
        print("  0x%08X: %08X  %s" % (off, w, name))
    if len(hits) > 40:
        print("  ... ещё %d" % (len(hits) - 40))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
