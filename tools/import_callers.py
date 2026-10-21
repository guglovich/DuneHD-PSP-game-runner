#!/usr/bin/env python3
"""Кто из импортов действительно вызывается гостевым кодом.

Зачем. Импортов в модуле 147, но реализовывать все 147 сразу не нужно: то,
что игра не вызывает, можно оставить заглушкой. Ещё важнее обратное — по
списку видно минимальный набор, без которого гостевой код не стартует, и
порядок: первые вызовы из точки входа определяют, что придётся подменять
раньше всего.

Как ищет. Гость вызывает импорт не напрямую, а через стаб: инструкция `jal`
ведёт в `.sceStub.text`, где каждому импорту отведён слот по 8 байт. Номер
слота внутри библиотеки известен из таблицы импортов, значит адрес стаба
однозначно сопоставляется с NID, а NID — с именем из справочника SDK.

`jal` раскладывается как `PC[31:28] | (imm26 << 2)`, поэтому в модуле с базой 0
цель считается от нуля.

Запуск:
    python3 tools/import_callers.py tmp/real/EBOOT.dec.BIN
    python3 tools/import_callers.py EBOOT.dec.BIN --names --min-calls 2
"""

import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from elf_imports import Elf, ElfError, parse_module_info, parse_stub_table  # noqa: E402

# Кодировка MIPS-инструкций.
# Опкод лежит в битах 31..26. У `j` это 000010, у `jal` — 000011.
# Нас интересует только `jal`: вызов функции. Прыжок `j` в слот задержки
# стаба — это не вызов гостя наружу, он внутри самой конструкции.
OP_JAL = 0x0C000000
MASK_OP = 0xFC000000
SH_IMM = 2
IMM_MASK = 0x03FFFFFF


def load_nid_names(path: Path):
    """Справочник NID -> (библиотека, имя). Строки-комментарии пропускаются."""
    names = {}
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            nid, lib, name = parts[0], parts[1], parts[2]
            # Имя в справочнике может быть помечено как ненастоящее.
            if len(parts) >= 5 and parts[4] == "1":
                name = f"{name} (?)"
            names[nid.upper()] = (lib, name)
    return names


def build_stub_index(libs):
    """Адрес стаба -> сведения об импорте.

    Адреса берутся из самой таблицы импортов, поэтому сопоставление точное,
    а не эвристическое.
    """
    index = {}
    for lib in libs:
        for i, nid in enumerate(lib["nids"]):
            addr = lib["stubs"][i]
            index[addr] = {
                "nid": nid,
                "library": lib["library"],
                "slot": i,
                "stub": addr,
            }
    return index


def scan_calls(elf: Elf, stub_lo: int, stub_hi: int):
    """Подсчёт вызовов в диапазон стабов по всем исполняемым секциям.

    Считаются только `jal` с целью внутри диапазона стабов: прочие переходы
    к обычному коду интереса не представляют, а `jalr` через регистр
    разобрать статически нельзя, и таких вызовов в модуле нет.
    """
    counts = Counter()
    total_jal = 0

    for sec in elf.sections:
        if not (sec["flags"] & 0x4):     # SHF_EXECINSTR
            continue
        # Секция адресуется по файловому смещению, а не по виртуальному
        # адресу: у этого образа отображение начинается со смещения 0x54,
        # поэтому по vaddr читать нельзя.
        start = sec["offset"]
        data = elf.d[start:start + sec["size"]]

        for off in range(0, len(data) - 3, 4):
            (word,) = struct.unpack_from("<I", data, off)
            if (word & MASK_OP) != OP_JAL:
                continue
            total_jal += 1
            target = (word & IMM_MASK) << SH_IMM
            if stub_lo <= target < stub_hi:
                counts[target] += 1

    return counts, total_jal


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", type=Path, help="расшифрованный EBOOT.BIN")
    ap.add_argument("--nids", type=Path, default=Path("data/psp_nids.tsv"),
                    help="справочник NID (по умолчанию data/psp_nids.tsv)")
    ap.add_argument("--names", action="store_true",
                    help="показывать имена из справочника")
    ap.add_argument("--min-calls", type=int, default=1,
                    help="не показывать импорты с меньшим числом вызовов")
    ap.add_argument("--stub-dump", action="store_true",
                    help="вывести все стабы по возрастанию адреса, с именами "
                         "и числом вызовов; по этому спику читается дизасм "
                         "точки входа")
    args = ap.parse_args()

    try:
        elf = Elf(args.elf)
        info = parse_module_info(elf)
        libs = parse_stub_table(elf, info)
    except (OSError, ElfError) as e:
        print(f"ошибка: {e}", file=sys.stderr)
        return 1

    index = build_stub_index(libs)
    if not index:
        print("таблица импортов пуста", file=sys.stderr)
        return 1

    stub_lo = min(index)
    stub_hi = max(index) + 8
    counts, total_jal = scan_calls(elf, stub_lo, stub_hi)

    names = load_nid_names(args.nids) if args.names else {}

    total_imports = sum(len(lib["nids"]) for lib in libs)
    called = set(counts)
    print(f"модуль            : {info['name']}")
    print(f"импортов          : {total_imports} в {len(libs)} библиотеках")
    print(f"стабы             : 0x{stub_lo:08X} .. 0x{stub_hi:08X}")
    print(f"инструкций jal    : {total_jal}")
    print(f"вызовов в стабы   : {sum(counts.values())}")
    print(f"вызвано импортов  : {len(called)} из {total_imports} "
          f"({100.0 * len(called) / total_imports:.1f}%)")
    print(f"ни разу не вызвано: {total_imports - len(called)}")
    print()

    if args.stub_dump:
        print('  стаб  слот  вызовов  NID  библиотека / имя')
        print("-" * 100)
        for stub in sorted(index):
            info_s = index[stub]
            lib, name = names.get(info_s["nid"].upper(), ("", ""))
            print(f'  0x{stub:08X}  {info_s["slot"]:4d}  {counts.get(stub, 0):7d}  '
                  f'{info_s["nid"]}  {lib or "?"}/{name or "имя не найдено"}')
        return 0

    rows = []
    for stub, n in counts.items():
        info_s = index.get(stub)
        if info_s is None:
            continue
        lib, name = names.get(info_s["nid"].upper(), ("", ""))
        rows.append((n, info_s, lib, name))

    rows.sort(key=lambda r: (-r[0], r[1]["nid"]))

    print(f'{"вызовов":>7}  {"стаб":>10}  {"NID":>10}  '
          f'{"библиотека":22s} имя')
    print("-" * 100)
    shown = 0
    for n, info_s, lib, name in rows:
        if n < args.min_calls:
            continue
        shown += 1
        print(f'{n:7d}  0x{info_s["stub"]:08X}  {info_s["nid"]:>10s}  '
              f'{lib or "?":22s} {name or "(имя не найдено)"}')

    if args.min_calls > 1:
        print()
        print(f"показано импортов с >= {args.min_calls} вызовами: {shown}")
    if shown == 0:
        print("(ничего не показано — снизьте --min-calls)")

    return 0


if __name__ == "__main__":
    sys.exit(main())