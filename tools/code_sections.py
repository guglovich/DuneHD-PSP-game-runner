#!/usr/bin/env python3
"""Границы секций кода образа гостя.

Зачем. Поиск по всему файлу даёт 715 вхождений записи в `$k0`, и почти все
из них — данные: `and $26,$0,$24` кодом не является. Отсекать по глазому
нельзя, а на MIPS `objdump` код от данных не отличает (это записано в
`Projects/AGENTS.md` как отдельная ловушка, стоившая неверных чисел
покрытия).

Точный способ — взять сами границы `.text*` из таблицы секций ELF.
"""

import struct
import sys

SHN_UNDEF = 0
SHT_NULL = 0


def sections(data):
    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize = struct.unpack_from("<H", data, 0x2E)[0]
    shnum = struct.unpack_from("<H", data, 0x30)[0]
    shstrndx = struct.unpack_from("<H", data, 0x32)[0]

    if shoff == 0 or shnum == 0:
        raise SystemExit("таблицы секций нет — это не ELF")

    raw = []
    for i in range(shnum):
        o = shoff + i * shentsize
        (name, typ, flags, addr, offset, size, link, info, align,
         entsize) = struct.unpack_from("<10I", data, o)
        raw.append((name, typ, flags, addr, offset, size))

    if shstrndx >= len(raw):
        raise SystemExit("нет строковой таблицы секций")
    str_off = raw[shstrndx][4]

    out = []
    for name, typ, flags, addr, offset, size in raw:
        s = b""
        p = str_off + name
        while p < len(data) and data[p] != 0:
            s += bytes([data[p]])
            p += 1
        out.append((s.decode("latin1"), typ, flags, addr, offset, size))
    return out


def main(path):
    data = open(path, "rb").read()
    secs = sections(data)

    print("секций: %d" % len(secs))
    total = 0
    for name, typ, flags, addr, off, size in secs:
        if flags & 0x4:
            total += size
            print("  %-28s vaddr=0x%08X  file=0x%05X..0x%05X  %d Б%s"
                  % (name, addr, off, off + size, size,
                     "  <-- исполняемая" if flags & 0x4 else ""))
    print("всего кода: %d Б из %d Б файла" % (total, len(data)))

    if total == 0:
        raise SystemExit("секций кода не найдено — искать негде")


if __name__ == "__main__":
    main(sys.argv[1])
