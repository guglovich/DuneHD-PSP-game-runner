#!/usr/bin/env python3
"""Сделать копию PSP PRX, пригодную для дизасма обычными инструментами.

Зачем. Розничный `EBOOT.BIN` — это ELF, который ни objdump, ни другие
бинутис-инструменты читать не хотят: `file format not recognized`, а при
`--section=.text` — «section not found». При этом `file` и `readelf` образ
видят нормально. Причина не в формате, а в двух конкретных полях.

Правка 1 — `e_type`. У PRX он равен `0xFFA0`, тогда как для дизасма должно
быть `ET_EXEC` (2). Значение остальных полей при этом ничего не меняет:
смещения секций, адреса и содержимое те же, поэтому дизасм показывает ровно
тот код, что лежит в гостевом образе.

Правка 2 — `sh_info` секции `SHT_SYMTAB`. Символы в розничном образе вырезаны
(`.strtab` размером 1 байт, `.symtab` — одна запись), но линкер оставил в
`sh_info` значение 22640, тогда как символов всего один и `shnum` равен 28.
Именно из-за этого BFD отказывается читать файл: сначала был виноват `.symtab`,
что видно по перебору `e_shnum` — 1, 2, 3 принимаются, 4 и выше отвергаются.
Кладём `sh_info`, равное числу символов, — минимальная правка, которая ничего
не теряет: читать из вырезанной таблицы всё равно нечего.

Ничего в оригинале не меняется, правится только копия.

Побочный плюс: адреса в выводе совпадают с виртуальными адресами модуля
(база 0), а не с файловыми смещениями. В гостевой памяти к ним прибавляется
0x08804000, но для анализа удобнее base-0.

Запуск:
    python3 tools/prx_to_objdump.py IN.BIN OUT.BIN
"""

import struct
import sys

PRX_E_TYPE = 0xFFA0
ET_EXEC = 2
SHT_SYMTAB = 2

OFF_E_TYPE = 16
OFF_E_MACHINE = 18
OFF_E_SHOFF = 32
OFF_E_SHENTSIZE = 46
OFF_E_SHNUM = 48
EM_MIPS = 8

SH_SIZE = 20
SH_ENTSIZE = 36
SH_INFO = 28
SHDR_SIZE = 40


def fix_symtab_info(data: bytearray) -> int:
    """Ограничить sh_info таблиц символов числом символов.

    Возвращает число исправленных секций.
    """
    shoff = struct.unpack_from("<I", data, OFF_E_SHOFF)[0]
    shentsize = struct.unpack_from("<H", data, OFF_E_SHENTSIZE)[0]
    shnum = struct.unpack_from("<H", data, OFF_E_SHNUM)[0]

    fixed = 0
    for i in range(shnum):
        h = shoff + i * shentsize
        if h + SHDR_SIZE > len(data):
            break
        sh_type = struct.unpack_from("<I", data, h + 4)[0]
        if sh_type != SHT_SYMTAB:
            continue
        sh_size = struct.unpack_from("<I", data, h + SH_SIZE)[0]
        entsize = struct.unpack_from("<I", data, h + SH_ENTSIZE)[0]
        sh_info = struct.unpack_from("<I", data, h + SH_INFO)[0]
        if entsize == 0:
            continue
        nsyms = sh_size // entsize
        if sh_info > nsyms:
            struct.pack_into("<I", data, h + SH_INFO, nsyms)
            fixed += 1
            print(f"  .symtab (секция {i}): sh_info {sh_info} -> {nsyms} "
                  f"(символов {nsyms} при shnum {shnum})", file=sys.stderr)
    return fixed


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    with open(sys.argv[1], "rb") as f:
        data = bytearray(f.read())

    if len(data) < 52 or data[:4] != b"\x7fELF":
        print("вход не начинается с магии ELF", file=sys.stderr)
        return 1

    e_type = struct.unpack_from("<H", data, OFF_E_TYPE)[0]
    e_machine = struct.unpack_from("<H", data, OFF_E_MACHINE)[0]

    if e_machine != EM_MIPS:
        print(f"e_machine = {e_machine}, ожидался MIPS (8)", file=sys.stderr)
        return 1

    struct.pack_into("<H", data, OFF_E_TYPE, ET_EXEC)
    print(f"  e_type 0x{e_type:04X} -> 0x{ET_EXEC:04X}", file=sys.stderr)

    fix_symtab_info(data)

    with open(sys.argv[2], "wb") as f:
        f.write(data)

    print(f"{sys.argv[1]} -> {sys.argv[2]} ({len(data)} байт)")
    return 0


if __name__ == "__main__":
    sys.exit(main())