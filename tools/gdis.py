#!/usr/bin/env python3
"""Дизассемблирование кода гостя по РАНТАЙМ-адресу, локально.

Зачем локально, а не выгрузкой с плеера. Устройство доступно не всегда, а
выгрузка `psp-hle dump` стоит ssh-вызов. При этом для кода картина та же:
гость слинкован не-PIC, обращений через `$gp` в исполняемых секциях ноль,
и смещение в файле равно `адрес - guest_base`.

    tools/gdis.py 0x08A9C958 0x40          # от адреса, 0x40 байт
    tools/gdis.py 0x08A9C958 --until 0x08A9C9C4

Осторожно с байтовым порядком при сборке бинарника для objdump: `od -t x4` на
little-endian хосте печатает слово в ХОСТОВОМ порядке. Слова здесь
перекодируются в порядок памяти явно, иначе каждая инструкция выходит
перевёрнутой и вывод выглядит осмысленным мусором.
"""
import struct
import subprocess
import sys
from pathlib import Path

GUEST_BASE = 0x08804000
# Именно тот модуль, что грузится на плеере: md5 27e8339e78430eaee2932806d870196b,
# размер 0x482204. НЕ путать с `tmp/real/EBOOT.dec.BIN` — он другого размера
# (0x55a7e8) и по тем же адресам даёт данные вместо кода.
IMAGE = Path(__file__).resolve().parent.parent / "tmp/real/guest.elf"
OBJDUMP = ("/home/jinseoyeon/Projects/Dune Max/toolchain/bootlin-mips32el-musl/"
           "bin/mipsel-buildroot-linux-musl-objdump")


def segments():
    """Сегменты LOAD как (p_vaddr, p_offset, p_filesz)."""
    f = IMAGE.read_bytes()
    phoff, = struct.unpack_from("<I", f, 28)
    phentsize, phnum = struct.unpack_from("<HH", f, 42)
    out = []
    for i in range(phnum):
        o = phoff + i * phentsize
        p_type, p_offset, p_vaddr = struct.unpack_from("<3I", f, o)
        p_filesz, = struct.unpack_from("<I", f, o + 16)
        if p_type == 1:
            out.append((p_vaddr, p_offset, p_filesz))
    if not out:
        raise SystemExit("в образе нет сегментов LOAD")
    return out


def to_offset(addr, segs):
    """Рантайм-адрес -> смещение в файле ПО ЗАГОЛОВКАМ СЕГМЕНТОВ.

    Тождество `смещение = адрес - guest_base` НЕВЕРНО: у этого модуля
    `p_offset = p_vaddr + 0x80`, и с тождеством инструмент показывал по
    запрошенному адресу код, который в реальности лежит на 0x80 байт ниже —
    то есть вывод выглядел осмысленным и был чужим.
    """
    v = addr - GUEST_BASE
    for p_vaddr, p_offset, p_filesz in segs:
        if p_vaddr <= v < p_vaddr + p_filesz:
            return p_offset + (v - p_vaddr)
    raise SystemExit(f"адрес {addr:#x} вне LOAD-сегментов")


def words(addr, length):
    """Слова гостя по рантайм-адресу, в порядке байтов ПАМЯТИ."""
    off = to_offset(addr, segments())
    raw = IMAGE.read_bytes()[off:off + length]
    if len(raw) < length:
        raise SystemExit(f"в образе нет {length} байт по {addr:#x}")
    return struct.unpack(f"<{length // 4}I", raw)


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    addr = int(args[0], 0)
    length = 0x40
    if "--until" in args:
        length = int(args[args.index("--until") + 1], 0) - addr
    elif len(args) > 1:
        length = int(args[1], 0)

    blob = b"".join(w.to_bytes(4, "little") for w in words(addr, length))
    tmp = Path(__file__).resolve().parent.parent / "tmp/gdis.bin"
    tmp.write_bytes(blob)
    out = subprocess.run(
        [OBJDUMP, "-D", "-b", "binary", "-m", "mips:isa32r2", "-EL",
         f"--adjust-vma={addr:#x}", str(tmp)],
        capture_output=True, text=True, check=True).stdout
    print("\n".join(l for l in out.splitlines() if l.strip().startswith("8")
                    or l.strip().startswith("a")))


if __name__ == "__main__":
    main()