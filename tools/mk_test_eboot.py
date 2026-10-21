#!/usr/bin/env python3
"""Генератор тестового PSP EBOOT.BIN.

Собирает минимальный, но структурно корректный ELF32 MIPS little-endian
с той формой, которую ждёт загрузчик:

  * ELF-заголовок (type = ET_EXEC)
  * PT_LOAD-сегмент с .text
  * секция .rodata.sceModuleInfo (52 байта) с gp/ent_top/ent_end/stub_top/stub_end
  * пустая область импортов между stub_top и stub_end
  * запись в $v0 при старте — чтобы было видно, что точка входа достигнута

Проверяется тем же загрузчиком, что и настоящая игра, поэтому полезно
как регрессионный тест: если сломается разбор ELF/PRX или адресация
сегментов, генератор даст тот же класс ошибки, что и настоящий EBOOT.BIN.
"""

import argparse
import struct
import sys
from pathlib import Path

# MIPS32 Opcodes, которые реально кодируются ниже.
# lui $v0, 0x1234 -> оригинальная инструкция 0x3C021234
# lui $v0, 0x5678 -> 0x3C025678
# ori $v0, $v0, 0x9ABC -> 0x34429ABC
# lui $a0, 0x0000 -> 0x3C040000
LUI_V0_LO = 0x3C021234
LUI_V0_HI = 0x3C025678
ORI_V0 = 0x34429ABC
LUI_A0 = 0x3C040000

# 'b .' = branch always to self: инструкция паузы, чтобы точка входа не ушла
# за пределы обработанного кода и её можно было отличить от мусора.
SELF_BRANCH = 0x1000FFFF

BASE_ADDRESS = 0x08804000  # стандартная база загрузки пользовательского модуля PSP
MODULE_INFO_ADDRESS_OFFSET = 0x100
MODULE_INFO_SIZE = 52


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def build_module_info(name, gp, ent_top, ent_end, stub_top, stub_end):
    """52-байтовая структура PspModuleInfo как её читает загрузчик."""
    raw_name = name.encode("ascii")[:28]
    raw_name = raw_name + b"\x00" * (28 - len(raw_name))
    return (
        struct.pack("<H", 0)          # attributes
        + bytes([1, 0])              # version 1.0
        + raw_name                   # name[28]
        + struct.pack("<IIIII", gp, ent_top, ent_end, stub_top, stub_end)
    )


def build_entry_code():
    """Точка входа: пишем константу-метку в $v0, кладём адрес в $a0, петля."""
    return [
        LUI_V0_LO,
        LUI_V0_HI,
        ORI_V0,
        LUI_A0,
        SELF_BRANCH,
    ]


def build_elf(module_name="TESTMOD"):
    entry_offset = 0
    code = build_entry_code()

    text_vaddr = BASE_ADDRESS
    text_file_offset = 0x1000
    text_size = len(code) * 4

    modinfo_vaddr = BASE_ADDRESS + MODULE_INFO_ADDRESS_OFFSET
    # Модуль без импортов: stub_top == stub_end.
    ent_top = BASE_ADDRESS
    ent_end = BASE_ADDRESS + MODULE_INFO_ADDRESS_OFFSET + MODULE_INFO_SIZE
    stub_top = ent_end
    stub_end = ent_end
    gp = ent_end

    ehdr_size = 52
    phdr_size = 32
    phdr_count = 1
    shdr_size = 40
    shdr_count = 5

    # Таблицы заголовков НЕ должны перекрываться с загружаемым кодом.
    # phoff и shoff размещаются после .text, а phoff — до первого байта
    # .text, который уходит в образ при загрузке.
    text_end = text_file_offset + text_size
    phoff = align_up(text_end, 4)
    shoff = align_up(phoff + phdr_count * phdr_size, 4)
    shstrtab_offset = shoff + shdr_count * shdr_size
    shstrtab = b"\x00.text\x00.shstrtab\x00.rodata.sceModuleInfo\x00.rel.dyn\x00"

    def shstrtab_offset_of(name):
        # name уже присутствует в таблице строк
        return shstrtab.index(name.encode())

    modinfo_file_offset = text_file_offset + MODULE_INFO_ADDRESS_OFFSET

    blob = bytearray()

    def put(offset, data):
        need = offset + len(data)
        if len(blob) < need:
            blob.extend(b"\x00" * (need - len(blob)))
        blob[offset:offset + len(data)] = data

    # ELF-заголовок: 16 байт ident отдельно от остальных полей
    ident = b"\x7fELF" + bytes([1, 1, 1, 0, 0]) + b"\x00" * 7
    hdr_rest = struct.pack(
        "<HHIIIIIHHHHHH",
        2, 8, 1,                       # type, machine, version
        text_vaddr + entry_offset,     # entry
        phoff, shoff, 0,               # phoff, shoff, flags
        ehdr_size, phdr_size, phdr_count,
        shdr_size, shdr_count, 3,      # shstrndx
    )
    put(0, ident + hdr_rest)

    # PT_LOAD
    put(phoff, struct.pack(
        "<IIIIIIII",
        1,                              # p_type = PT_LOAD
        text_file_offset,               # p_offset
        text_vaddr,                     # p_vaddr
        text_vaddr,                     # p_paddr
        text_size,                      # p_filesz
        MODULE_INFO_ADDRESS_OFFSET + MODULE_INFO_SIZE,  # p_memsz (с запасом на BSS)
        0x5,                            # p_flags = R+X
        0x1000,                         # p_align
    ))

    # Секционные заголовки: [0]=NULL [1]=.text [2]=.shstrtab [3]=.rodata.sceModuleInfo [4]=.rel.dyn
    put(shoff + 0 * shdr_size, struct.pack("<IIIIIIIIII", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
    put(shoff + 1 * shdr_size, struct.pack(
        "<IIIIIIIIII",
        shstrtab_offset_of(".text"),
        1,            # SHT_PROGBITS
        0x6,          # SHF_ALLOC | SHF_EXECINSTR
        text_vaddr,
        text_file_offset,
        text_size,
        0, 0, 4, 0,
    ))
    put(shoff + 2 * shdr_size, struct.pack(
        "<IIIIIIIIII",
        shstrtab_offset_of(".shstrtab"),
        3,            # SHT_STRTAB
        0,
        0,
        shstrtab_offset,
        len(shstrtab),
        0, 0, 1, 0,
    ))
    put(shoff + 3 * shdr_size, struct.pack(
        "<IIIIIIIIII",
        shstrtab_offset_of(".rodata.sceModuleInfo"),
        1,            # SHT_PROGBITS
        0x2,          # SHF_ALLOC
        modinfo_vaddr,
        modinfo_file_offset,
        MODULE_INFO_SIZE,
        0, 0, 4, 0,
    ))
    put(shoff + 4 * shdr_size, struct.pack(
        "<IIIIIIIIII",
        shstrtab_offset_of(".rel.dyn"),
        9,            # SHT_REL
        0,
        0,
        0,
        0,            # пустая таблица релокаций
        0, 0, 4, 8,
    ))

    put(shstrtab_offset, shstrtab)
    put(text_file_offset, b"".join(struct.pack("<I", insn) for insn in code))
    put(modinfo_file_offset,
        build_module_info(module_name, gp, ent_top, ent_end, stub_top, stub_end))

    # Заголовки не должны наезжать на образ: загрузчик читает PT_LOAD по
    # смещению p_offset, и если phoff попал внутрь кода, он прочитает
    # инструкции вместо заголовков. Раньше это случалось именно так.
    code_end = text_file_offset + text_size
    if phoff < code_end:
        raise AssertionError(
            f"phoff={phoff} попадает в образ .text (до {code_end})")
    if shoff < phoff + phdr_count * phdr_size:
        raise AssertionError(f"shoff={shoff} наезжает на таблицу phdr")

    return bytes(blob)


def main():
    parser = argparse.ArgumentParser(description="Сгенерировать тестовый PSP EBOOT.BIN")
    parser.add_argument("output", type=Path, help="путь для записи EBOOT.BIN")
    parser.add_argument("--name", default="TESTMOD", help="имя модуля в .sceModuleInfo")
    args = parser.parse_args()

    data = build_elf(args.name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)

    print(f"записано {len(data)} байт -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())