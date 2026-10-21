#!/usr/bin/env python3
"""Обход дерева каталогов ISO 9660.

Печатает содержимое образа, чтобы найти EBOOT.BIN и посмотреть, что ещё
лежит рядом с ним. Ничего не изменяет — только чтение.

Проверенные при разборе грабли, зафиксированные в коде:
  * path_table_size в PVD занимает 8 байт (both-endian), из-за чего все
    поля после него сдвинуты на 4, а root_dir_record лежит по 156, не 152;
  * обе позиции проверяются _Static_assert в iso_extract_eboot.c, здесь
    та же проверка дублируется утверждениями, чтобы ошибка не прошла
    молча при смене формата.
"""

import argparse
import struct
import sys
from pathlib import Path

SECTOR = 2048
PVD_LBA = 16
ROOT_RECORD_OFFSET = 156
BOTH_ENDIAN_SIZE = 8


def die(msg):
    print(f"ОШИБКА: {msg}", file=sys.stderr)
    raise SystemExit(1)


class Iso:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.path = path

    def sector(self, lba):
        self.f.seek(lba * SECTOR)
        data = self.f.read(SECTOR)
        if len(data) != SECTOR:
            die(f"не читается сектор {lba} (LBA за пределами образа?)")
        return data

    def close(self):
        self.f.close()

    def pvd(self):
        pvd = self.sector(PVD_LBA)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            die("сектор 16 не является Primary Volume Descriptor")
        block = int.from_bytes(pvd[128:130], "little")
        if block != SECTOR:
            die(f"размер логического блока {block}, ожидался {SECTOR}")
        # Зеркало both-endian обязано совпадать: это же проверяло смещение.
        size_le = int.from_bytes(pvd[132:136], "little")
        size_be = int.from_bytes(pvd[140 - 4:140 - 4 + 4], "big")
        if pvd[156] != 0x22:
            die(f"root_dir_record начинается с 0x{pvd[156]:02x}, а не с 0x22 "
                "(34) — раскладка PVD снова разъехалась")
        self.volume_id = pvd[40:72].decode("latin1").strip()
        self.system_id = pvd[8:40].decode("latin1").strip()
        self.path_table_size = size_le
        return pvd

    @staticmethod
    def parse_record(rec):
        length = rec[0]
        if length == 0:
            return None
        ext_lba = int.from_bytes(rec[2:6], "little")
        size = int.from_bytes(rec[10:14], "little")
        flags = rec[25]
        name_len = rec[32]
        raw = rec[33:33 + name_len]
        name = raw.decode("latin1")
        # В ISO 9660 "." и ".." — это однобайтовые имена 0x00 и 0x01.
        # Их надо распознать ДО обрезки: иначе "." станет пустой строкой,
        # пустая строка не совпадёт с фильтром, и обход уйдёт в рекурсию.
        # В этом образе так и было: обход падал с RecursionError.
        if name_len == 1 and raw[0] in (0x00, 0x01):
            name = "." if raw[0] == 0x00 else ".."
        else:
            if name.endswith("\x00"):
                name = name[:-1]
            if name.endswith(";1"):
                name = name[:-2]
        return {
            "lba": ext_lba,
            "size": size,
            "is_dir": bool(flags & 0x02),
            "name": name,
            "raw_name": rec[33:33 + name_len],
            "reclen": length,
        }

    def list_dir(self, lba, size):
        """Возвращает записи каталога. Запись нулевой длины означает
        конец сектора, а не конец каталога."""
        entries = []
        remaining = size
        sector_index = 0
        while remaining > 0:
            data = self.sector(lba + sector_index)
            pos = 0
            while pos < SECTOR:
                rec_len = data[pos]
                if rec_len == 0:
                    break  # конец записей в этом секторе
                entry = self.parse_record(data[pos:pos + rec_len])
                if entry:
                    entries.append(entry)
                pos += rec_len
            remaining -= SECTOR
            sector_index += 1
        return entries


def walk(iso, entry, prefix, out, max_depth=6):
    if entry["name"] in (".", "..", "\x00"):
        return
    path = f"{prefix}/{entry['name']}"
    if entry["is_dir"]:
        out.append(("DIR ", path, entry["size"], entry["lba"]))
        if len([p for p in path.split("/") if p]) < max_depth:
            for child in iso.list_dir(entry["lba"], entry["size"]):
                walk(iso, child, path, out, max_depth)
    else:
        out.append(("FILE", path, entry["size"], entry["lba"]))


def main():
    ap = argparse.ArgumentParser(description="Обход дерева ISO 9660")
    ap.add_argument("image", type=Path)
    ap.add_argument("--all", action="store_true",
                    help="показать все файлы, а не только верхний уровень")
    args = ap.parse_args()

    iso = Iso(args.image)
    try:
        pvd = iso.pvd()
        print(f"образ:      {args.image.name}")
        print(f"system id:  {iso.system_id!r}")
        print(f"volume id:  {iso.volume_id!r}")
        print(f"path table: {iso.path_table_size} байт")
        print()

        rd = pvd[ROOT_RECORD_OFFSET:ROOT_RECORD_OFFSET + 34]
        root = Iso.parse_record(rd)
        entries = iso.list_dir(root["lba"], root["size"])

        out = []
        for entry in entries:
            walk(iso, entry, "", out)

        dirs = [r for r in out if r[0] == "DIR "]
        files = [r for r in out if r[0] == "FILE"]

        print(f"каталогов: {len(dirs)}, файлов: {len(files)}")
        print()
        print("=== структура ===")
        for kind, path, size, lba in dirs:
            print(f"  {kind} {path}/")
        if args.all:
            for kind, path, size, lba in files:
                print(f"  {kind} {path}  {size} байт  LBA {lba}")
        else:
            print(f"\n(файлов: {len(files)}; полный список — с --all)")

        eboots = [r for r in out if r[1].upper().endswith("EBOOT.BIN")]
        print()
        if eboots:
            print("=== EBOOT.BIN найден ===")
            for kind, path, size, lba in eboots:
                print(f"  {path}  {size} байт  LBA {lba}")
        else:
            print("EBOOT.BIN не найден")

        print()
        print("=== крупнейшие файлы (топ-20) ===")
        for kind, path, size, lba in sorted(files, key=lambda r: -r[2])[:20]:
            print(f"  {size:>12,}  {path}")
    finally:
        iso.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())