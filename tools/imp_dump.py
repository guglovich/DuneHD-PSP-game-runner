#!/usr/bin/env python3
# Дампер таблицы импортов гостевого ELF — в том же порядке, в каком её
# строит загрузчик, чтобы сквозной индекс совпал с полем `idx=` отчёта.
#
# Зачем: отчёт плеера печатает `idx=`, `nid=` и `stub=`, но НЕ имя
# библиотеки. Чтобы понять, какой подсистемы PSP не хватает, NID надо
# привязать к библиотеке. Делать это по номеру NID «на глаз» нельзя:
# в отчёте гость крутится на `8F2DF740`, и без имени библиотеки остаётся
# только догадка.
#
# Порядок берётся из guest_load.c: каталог импортов лежит между
# `stub_top` и `stub_end`, записи по 20 байт:
#     +0   libname   (указатель)
#     +8   длина в словах, младший байт
#     +10  count     (u16) — число импортов
#     +12  nid_table (указатель)
#     +16  stub_table(указатель)
# Библиотеки идут подряд, и сквозной индекс — это сумма count всех
# предыдущих библиотек плюс номер внутри своей.
#
# Указатели в ФАЙЛЕ не релокованы и содержат vaddr первого сегмента.
# Для этого гостя смещение vaddr -> файловое простое и равно 0x80
# (сверено по `.text` vaddr 0 -> off 0x80 и `.sceStub.text.*`
# vaddr 0x302068 -> off 0x3020e8). Оно же проверяется на разрешении имени.

import struct
import sys

VADDR_TO_FILE = 0x80

def main(path, stub_top, stub_end):
    blob = open(path, "rb").read()

    def rd(v):
        off = v + VADDR_TO_FILE
        if off < 0 or off + 4 > len(blob):
            return None
        return off

    print("%-6s %-6s %-30s %-10s %s" %
          ("gidx", "NIDX", "библиотека", "NID", "stub"))
    gidx = 0
    cursor = stub_top
    while cursor + 20 <= stub_end:
        off = rd(cursor)
        if off is None:
            print("каталог вышел за файл: 0x%08X" % cursor)
            return 1
        libname, _pad, _len_lo, _len_hi, count, nid_table, stub_table = \
            struct.unpack("<IIBBHII", blob[off:off + 20])

        noff = rd(libname)
        name = "?"
        if noff is not None:
            name = blob[noff:noff + 32].split(b"\0")[0].decode(
                "ascii", "replace")

        noff2 = rd(nid_table)
        print("%-6s %-6s %-30s %-10s %08X   %08X" %
              ("", "", name, "", nid_table, stub_table))
        if noff2 is not None:
            for i in range(count):
                nid = struct.unpack("<I", blob[noff2 + 4 * i:
                                               noff2 + 4 * i + 4])[0]
                print("%-6d %-6d %-30s %08X   %08X" %
                      (gidx, i, name, nid, stub_table + 8 * i))
                gidx += 1
        cursor += 20
    print("\nвсего библиотек пройдено, импортов: %d" % gidx)
    return 0

if __name__ == "__main__":
    a = sys.argv
    sys.exit(main(a[1] if len(a) > 1 else "tmp/real/guest.elf",
                  int(a[2], 16) if len(a) > 2 else 0x3028EC,
                  int(a[3], 16) if len(a) > 3 else 0x302B30))
