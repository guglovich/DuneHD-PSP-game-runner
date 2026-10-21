#!/usr/bin/env python3
# Проверка трамплина `guest_call_ctor` по РАЗОБРАННОЙ СОБРАННОЙ библиотеке.
#
# См. подробное обоснование в tools/check_ctor_trampoline.sh. Коротко:
# прыжок к конструктору обязан быть `jalr`, иначе `$ra` не записывается,
# конструктор возвращается мимо блока восстановления, и после каждого
# конструктора `$sp` висит на 48 байт ниже, а `$s0–$s8`, `$gp`, `$ra`
# не восстановлены.
#
# Требования к файлу:
#   * ровно один `jalr` — это и есть переход к конструктору;
#   * его слот задержки идёт сразу за ним, а следующая команда — первая
#     команда блока восстановления `lw s0,0(sp)`. Это доказывает, что
#     `$ra`, записанный `jalr`, указывает ИМЕННО на восстановление;
#   * единственный `jr` во всей функции — это `jr ra` эпилога. Любой
#     `jr t9` означает возврат мимо восстановления.
#
# Коды выхода: 0 — годно, 1 — провал, 80 — нет `jalr`, 81 — не разобрано.

import re
import sys

RESTORE_FIRST = ("lw", "s0,0(sp)")

def fail(msg):
    print("ПРОВАЛ: " + msg)
    return 1

def main(path):
    insts = []
    pat = re.compile(r'^\s+([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)$')
    for line in open(path):
        m = pat.match(line.rstrip("\n"))
        if not m:
            continue
        fields = m.group(3).split("\t")
        mn = fields[0].strip()
        operands = "\t".join(fields[1:]).strip()
        insts.append((int(m.group(1), 16), int(m.group(2), 16), mn, operands))

    if not insts:
        print("ПРОВАЛ: в разборе нет ни одной инструкции")
        return 81

    jalr = [i for i, x in enumerate(insts) if x[2] == "jalr"]
    if not jalr:
        print("ПРОВАЛ: в guest_call_ctor нет ни одного `jalr` — переход к "
              "конструктору не записывает `$ra`, блок восстановления "
              "недостижим")
        return 80
    if len(jalr) > 1:
        return fail("jalr больше одного (%d) — неоднозначно, какой из них "
                    "ведущий в конструктор" % len(jalr))
    jx = jalr[0]

    if jx + 2 >= len(insts):
        return fail("после jalr не хватает команд, чтобы увидеть блок "
                    "восстановления")

    # Слот задержки jalr — любая команда; следующая обязана быть началом
    # восстановления. Иначе `$ra` ведёт не туда.
    mn, ops = insts[jx + 2][2], insts[jx + 2][3]
    if (mn, ops) != RESTORE_FIRST:
        return fail("за слотом задержки jalr идёт `%s %s`, а ожидается "
                    "`%s %s` (начало блока восстановления). `$ra` после "
                    "jalr ведёт мимо восстановления"
                    % (mn, ops, RESTORE_FIRST[0], RESTORE_FIRST[1]))

    # Единственный jr во всей функции — `jr ra` эпилога.
    bare = [(insts[i][0], insts[i][3]) for i in range(len(insts))
            if insts[i][2] == "jr"]
    bad = [b for b in bare if b[1] != "ra"]
    if bad:
        return fail("в функции есть `jr` не на `$ra`: " +
                    ", ".join("0x%06x jr %s" % b for b in bad) +
                    " — такой прыжок не записывает `$ra` и возвращает "
                    "конструктор мимо восстановления")
    if len(bare) != 1:
        return fail("ожидался ровно один `jr ra` эпилога, найдено %d"
                    % len(bare))

    # Эпилог обязан возвращать `$sp` и сам возвращаться вызывающему.
    tail = insts[-2:]
    if tail[0][2] != "jr" or tail[0][3] != "ra":
        return fail("функция не заканчивается `jr ra` — эпилога нет")

    addr, word, _, _ = insts[jx]
    print("найден jalr 0x%06x (слово 0x%08x), ведёт на восстановление "
          "0x%06x; единственный jr — `jr ra` эпилога"
          % (addr, word, insts[jx + 2][0]))
    return 0

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: _ctor_trampoline_check.py <disassembly>")
        sys.exit(81)
    sys.exit(main(sys.argv[1]))
