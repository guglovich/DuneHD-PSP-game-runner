#!/usr/bin/env python3
"""Разбор таблицы импортов PSP ELF (EBOOT.BIN).

Отвечает на главный вопрос разведки: чем выражены импорты — текстовыми
именами или числами NID.

Раскладка взята из фактического разбора расшифрованного EBOOT.BIN и
совпадает с описанием PSPRecomp (psprecomp/elf32.cpp):

  .rodata.sceModuleInfo  52 байта: attributes, version, name[28],
                         gp, ent_top, ent_end, stub_top, stub_end
  между stub_top и stub_end — цепочка записей по 20 байт:
      +0x00  указатель на имя библиотеки
      +0x04  (не используется)
      +0x08  длина записи в словах
      +0x0A  число импортов (u16)
      +0x0C  указатель на таблицу NID
      +0x10  указатель на таблицу стабов (по 8 байт на импорт)

Адреса в этих полях — виртуальные, а не файловые. Перевод берётся из
программного заголовка LOAD: file = vaddr + (p_offset - p_vaddr).

Замечание о проверке: скрипт обязан падать, а не молча выдавать пустоту,
если адрес вне файла или модуль не найден. Раньше ошибка перевода дала
чтение мусора вместо ошибки.
"""

import argparse
import struct
import sys
from pathlib import Path

PT_LOAD = 1
SHT_NOBITS = 8


class ElfError(Exception):
    pass


class Elf:
    def __init__(self, path):
        self.path = path
        self.d = Path(path).read_bytes()
        self._parse_headers()
        self._build_sections()

    def _parse_headers(self):
        d = self.d
        if d[:4] != b"\x7fELF":
            raise ElfError("не ELF (нет магии 7f 45 4c 46)")
        if d[4] != 1:
            raise ElfError("ожидался ELF32")
        if d[5] != 1:
            raise ElfError("ожидался little-endian")

        (self.e_type, self.e_machine, _ver, self.e_entry,
         self.e_phoff, self.e_shoff, self.e_flags,
         _ehsize, self.e_phentsize, self.e_phnum,
         self.e_shentsize, self.e_shnum, self.e_shstrndx) = struct.unpack_from(
            "<HHIIIIIHHHHHH", d, 16)

        self.segments = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            (p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz,
             p_flags, p_align) = struct.unpack_from("<IIIIIIII", d, off)
            self.segments.append({
                "type": p_type, "offset": p_offset, "vaddr": p_vaddr,
                "filesz": p_filesz, "memsz": p_memsz, "flags": p_flags,
            })

        loads = [s for s in self.segments if s["type"] == PT_LOAD]
        if not loads:
            raise ElfError("нет сегмента PT_LOAD")
        # Перевод адресов: file = vaddr + delta, delta = p_offset - p_vaddr.
        self.delta = loads[0]["offset"] - loads[0]["vaddr"]

    def _build_sections(self):
        d = self.d
        raw = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            raw.append(struct.unpack_from("<IIIIIIIIII", d, off))

        str_off = raw[self.e_shstrndx][4]
        self.sections = []
        for s in raw:
            name_off = str_off + s[0]
            end = d.index(b"\x00", name_off)
            self.sections.append({
                "name": d[name_off:end].decode("latin1"),
                "type": s[1], "flags": s[2], "vaddr": s[3],
                "offset": s[4], "size": s[5],
            })

    def section(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        return None

    def fo(self, vaddr):
        """Виртуальный адрес -> смещение в файле, с проверкой границ.

        Проверка обязательна: без неё неверный адрес даёт чтение мусора,
        который выглядит как осмысленные данные.
        """
        off = vaddr + self.delta
        if off < 0 or off >= len(self.d):
            raise ElfError(
                f"адрес 0x{vaddr:08X} вне файла (смещение 0x{off:X}, "
                f"размер {len(self.d)})")
        return off

    def u8(self, vaddr):
        return self.d[self.fo(vaddr)]

    def u16(self, vaddr):
        return struct.unpack_from("<H", self.d, self.fo(vaddr))[0]

    def u32(self, vaddr):
        return struct.unpack_from("<I", self.d, self.fo(vaddr))[0]

    def s32(self, vaddr):
        return struct.unpack_from("<i", self.d, self.fo(vaddr))[0]

    def cstr(self, vaddr, maxlen=256):
        off = self.fo(vaddr)
        end = self.d.find(b"\x00", off, off + maxlen)
        if end < 0:
            raise ElfError(f"строка по 0x{vaddr:08X} не terminирована")
        return self.d[off:end].decode("latin1")


def parse_module_info(elf):
    sec = elf.section(".rodata.sceModuleInfo")
    if sec is None:
        raise ElfError("нет секции .rodata.sceModuleInfo — это не PSP-модуль")
    base = sec["vaddr"]
    info = {
        "attributes": elf.u16(base),
        "version": (elf.u8(base + 2), elf.u8(base + 3)),
        "name": elf.cstr(base + 4, 28),
        "gp": elf.u32(base + 32),
        "ent_top": elf.u32(base + 36),
        "ent_end": elf.u32(base + 40),
        "stub_top": elf.u32(base + 44),
        "stub_end": elf.u32(base + 48),
    }
    return info


def parse_stub_table(elf, info):
    """Обход цепочки записей между stub_top и stub_end."""
    cursor = info["stub_top"]
    end = info["stub_end"]
    if end < cursor:
        raise ElfError("stub_end меньше stub_top")

    libs = []
    guard = 0
    while cursor < end:
        guard += 1
        if guard > 1024:
            raise ElfError("слишком много записей — вероятно, зацикливание")
        libname = elf.u32(cursor)
        length_words = elf.u8(cursor + 8)
        count = elf.u16(cursor + 10)
        nid_table = elf.u32(cursor + 12)
        stub_table = elf.u32(cursor + 16)

        library = elf.cstr(libname, 128)
        # NID возвращается строкой в едином формате "0xXXXXXXXX":
        # так его хранит справочник из build_nid_table.py, и сопоставление
        # не зависит от того, десятичное ли число получено или шестнадцатеричное.
        nids = [f"0x{elf.u32(nid_table + 4 * i):08X}" for i in range(count)]
        stubs = [stub_table + 8 * i for i in range(count)]

        libs.append({
            "library": library, "count": count, "nids": nids,
            "stub_table": stub_table, "stubs": stubs,
            "record": cursor, "length_words": length_words,
        })

        step = max(20, length_words * 4)
        if step % 4:
            step = (step + 3) & ~3
        cursor += step

    return libs


def main():
    ap = argparse.ArgumentParser(description="Разбор импортов PSP ELF")
    ap.add_argument("elf", type=Path, help="расшифрованный EBOOT.BIN")
    ap.add_argument("--nids", action="store_true",
                    help="вывести все NID (по умолчанию только сводка)")
    args = ap.parse_args()

    try:
        elf = Elf(args.elf)
    except ElfError as e:
        print(f"ОШИБКА: {e}", file=sys.stderr)
        return 1

    print(f"файл      : {args.elf}")
    print(f"тип ELF   : 0x{elf.e_type:04X}"
          f"{'  (PRX)' if elf.e_type == 0xFFA0 else ''}")
    print(f"машина    : {elf.e_machine} (8 = MIPS)")
    print(f"entry     : 0x{elf.e_entry:08X}")
    print(f"перевод   : file = vaddr {elf.delta:+d}")
    print()

    try:
        info = parse_module_info(elf)
    except ElfError as e:
        print(f"ОШИБКА: {e}", file=sys.stderr)
        return 1

    print("=== модуль ===")
    print(f"  имя        : {info['name']}")
    print(f"  версия     : {info['version'][0]}.{info['version'][1]}")
    print(f"  gp         : 0x{info['gp']:08X}")
    print(f"  ent        : 0x{info['ent_top']:08X} .. 0x{info['ent_end']:08X}")
    print(f"  stub       : 0x{info['stub_top']:08X} .. 0x{info['stub_end']:08X}")
    print()

    try:
        libs = parse_stub_table(elf, info)
    except ElfError as e:
        print(f"ОШИБКА при разборе стабов: {e}", file=sys.stderr)
        return 1

    total = sum(l["count"] for l in libs)
    print(f"=== импорты: {total} из {len(libs)} библиотек ===")
    print()
    for lib in libs:
        print(f"  {lib['library']:<24} {lib['count']:>4} функций"
              f"   nid_table=0x{lib['stub_table']:08X}")
    print()

    if total == 0:
        print("Импортов нет — модуль самодостаточный.")
        return 0

    all_nids = [n for lib in libs for n in lib["nids"]]
    uniq = sorted(set(all_nids))
    print(f"уникальных NID: {len(uniq)} из {total}")
    print()

    if args.nids:
        for lib in libs:
            print(f"--- {lib['library']} ---")
            for i, (nid, stub) in enumerate(zip(lib["nids"], lib["stubs"])):
                print(f"  {i:>4}  NID={nid}  stub=0x{stub:08X}")
            print()
    else:
        print("первые 20 NID:")
        for nid in all_nids[:20]:
            print(f"  {nid}")
        if len(all_nids) > 20:
            print(f"  ... ещё {len(all_nids) - 20}")
        print()
        print("(полный список — с --nids)")
    return 0


if __name__ == "__main__":
    sys.exit(main())