#!/usr/bin/env python3
"""Сопоставление импортов PSP-модуля со справочником NID.

Показывает, какие импорты удалось назвать, а какие остались безымянными,
и выдаёт список NID для подмены. Это основной шаг к перехвату системных
вызовов: безымянный NID подменять нечем, пока не выяснено, что он делает.

Входные данные:
  * расшифрованный EBOOT.BIN — даёт список импортов и адреса стабов;
  * справочник из build_nid_table.py — сопоставляет NID с именем.

Ошибки, а не пустота, обязательны: если справочник не найден или не
разобран, скрипт должен об этом сказать, иначе вывод «0 найдено» будет
принят за «импорты не совпадают».
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elf_imports import Elf, ElfError, parse_module_info, parse_stub_table  # noqa: E402


def load_nid_table(path):
    table = {}
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            if line.startswith("#"):
                continue
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) != 5:
                raise ValueError(
                    f"{path}:{lineno}: ожидалось 5 полей, получено {len(parts)}")
            nid, lib, name, src, unnamed = parts
            try:
                int(nid, 16)
            except ValueError:
                raise ValueError(f"{path}:{lineno}: NID не число: {nid!r}")
            table[nid] = {
                "lib": lib, "name": name, "src": src,
                "unnamed": unnamed == "1",
            }
    if not table:
        raise ValueError(f"{path}: справочник пуст")
    return table


def main():
    ap = argparse.ArgumentParser(description="Сопоставление импортов со справочником NID")
    ap.add_argument("elf", type=Path, help="расшифрованный EBOOT.BIN")
    ap.add_argument("--nids", type=Path, default=Path("data/psp_nids.tsv"))
    ap.add_argument("--only-missing", action="store_true",
                    help="показать только ненайденные импорты")
    args = ap.parse_args()

    try:
        table = load_nid_table(args.nids)
    except (OSError, ValueError) as e:
        print(f"ОШИБКА со справочником: {e}", file=sys.stderr)
        return 1

    try:
        elf = Elf(args.elf)
        info = parse_module_info(elf)
        libs = parse_stub_table(elf, info)
    except ElfError as e:
        print(f"ОШИБКА разбора ELF: {e}", file=sys.stderr)
        return 1

    named = 0
    unnamed_nid = 0          # есть в справочнике, но безымянный экспорт
    not_in_table = 0
    total = 0

    print(f"модуль: {info['name']}")
    print(f"справочник: {args.nids} ({len(table)} NID)")
    print()

    for lib in libs:
        rows = []
        for i, nid in enumerate(lib["nids"]):
            total += 1
            entry = table.get(nid)
            if entry is None:
                not_in_table += 1
                rows.append((i, nid, None))
            elif entry["unnamed"]:
                unnamed_nid += 1
                rows.append((i, nid, entry["name"]))
            else:
                named += 1
                rows.append((i, nid, entry["name"]))

        if args.only_missing:
            rows = [r for r in rows if r[2] is None or table[r[1]]["unnamed"]]
            if not rows:
                continue

        print(f"--- {lib['library']} ({len(lib['nids'])}) ---")
        for i, nid, name in rows:
            stub = lib["stubs"][i]
            if name is None:
                mark = "НЕТ В СПРАВОЧНИКЕ"
            elif table[nid]["unnamed"]:
                mark = f"{name}  [unnamed]"
            else:
                mark = name
            print(f"  {i:>3}  stub=0x{stub:08X}  {nid}  {mark}")
        print()

    print("=== итог ===")
    print(f"всего импортов      : {total}")
    print(f"названо             : {named}")
    print(f"есть, но безымянные : {unnamed_nid}")
    print(f"нет в справочнике   : {not_in_table}")
    if total:
        pct = 100.0 * named / total
        print(f"покрытие именами    : {pct:.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())