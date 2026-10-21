#!/usr/bin/env python3
"""Сборка справочника NID из исходников PSP SDK.

Откуда берётся. В библиотеках PSP функция определяется не текстовым
именем, а числом NID — это 4-байтовый идентификатор, который загрузчик
использует для резолва импортов. Из-за этого по самому EBOOT.BIN нельзя
понять, какая функция вызывается: видны только NID.

Исходники PSP SDK содержат и NID, и имя рядом:

    IMPORT_FUNC "sceDisplay",0x0E20F177,sceDisplaySetMode

Из 105 файлов `.S` в `src/` получается 3115 объявлений, из них 2661
уникальный NID. Этого хватает, чтобы назвать почти любой импорт.

Скрипт скачивает файлы напрямую с GitHub и собирает таблицу. Ничего не
записывает на диск, кроме указанного выходного файла.

Замечание: некоторые NID в SDK намеренно безымянные — имя в макросе
совпадает с NID, например `sceMpeg_11CAB459`. Такие строки отмечены
признаком `unnamed`, и по ним нельзя понять назначение.
"""

import argparse
import re
import sys
import urllib.request
import urllib.error

API_TREE = "https://api.github.com/repos/pspdev/pspsdk/git/trees/master?recursive=1"
RAW_BASE = "https://raw.githubusercontent.com/pspdev/pspsdk/master/"

# IMPORT_FUNC "библиотека",0xNID,имя  —  и вариант без отступа
RE_IMPORT = re.compile(
    r'IMPORT_FUNC\s+"([^"]+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*([A-Za-z_][\w]*)')
# IMPORT_START "библиотека",0xNNNN — база импортов библиотеки
RE_IMPORT_START = re.compile(r'IMPORT_START\s+"([^"]+)"\s*,\s*(0x[0-9A-Fa-f]+)')


def fetch(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "psp-hle-tools"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("latin1")


def list_asm_files():
    """Список .S файлов в src/ через дерево репозитория."""
    import json
    tree = json.loads(fetch(API_TREE))
    if tree.get("truncated"):
        print("ВНИМАНИЕ: дерево репозитория обрезано, список может быть неполным",
              file=sys.stderr)
    return [e["path"] for e in tree.get("tree", [])
            if e["path"].startswith("src/") and e["path"].endswith(".S")]


def collect(paths, verbose=False):
    rows = []
    fails = []
    bases = {}
    for p in paths:
        try:
            text = fetch(RAW_BASE + p)
        except (urllib.error.URLError, OSError) as e:
            fails.append((p, str(e)))
            continue
        for m in RE_IMPORT_START.finditer(text):
            bases[m.group(1)] = m.group(2)
        for m in RE_IMPORT.finditer(text):
            nid = m.group(2).upper().replace("0X", "0x")
            name = m.group(3)
            # Имя вида sceMpeg_11CAB459 — в SDK это «безымянный» экспорт,
            # назначение из него не следует.
            unnamed = name.endswith("_" + nid[2:].upper())
            rows.append({
                "nid": nid, "lib": m.group(1), "name": name,
                "file": p, "unnamed": unnamed,
            })
    if verbose:
        print(f"скачано файлов: {len(paths) - len(fails)} из {len(paths)}",
              file=sys.stderr)
        for p, e in fails:
            print(f"  не удалось {p}: {e}", file=sys.stderr)
    return rows, bases, fails


def main():
    ap = argparse.ArgumentParser(description="Собрать справочник NID из PSP SDK")
    ap.add_argument("-o", "--output", required=True,
                    help="выходной TSV: NID, библиотека, имя, файл, unnamed")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    paths = list_asm_files()
    if not paths:
        print("не удалось получить список файлов", file=sys.stderr)
        return 1

    rows, bases, fails = collect(paths, args.verbose)

    rows.sort(key=lambda r: r["nid"])
    with open(args.output, "w", encoding="utf-8") as f:
        f.write("# Справочник NID, собран из исходников PSP SDK (pspdev/pspsdk).\n")
        f.write("# Формат: NID\\tбиблиотека\\tимя\\tисходник\\tunnamed\n")
        for r in rows:
            f.write(f"{r['nid']}\t{r['lib']}\t{r['name']}\t{r['file']}"
                    f"\t{'1' if r['unnamed'] else '0'}\n")

    uniq = {r["nid"] for r in rows}
    unnamed = [r for r in rows if r["unnamed"]]
    print(f"файлов обработано   : {len(paths) - len(fails)} из {len(paths)}")
    print(f"объявлений IMPORT_FUNC: {len(rows)}")
    print(f"уникальных NID       : {len(uniq)}")
    print(f"безымянных (unnamed) : {len(unnamed)}")
    print(f"библиотек            : {len({r['lib'] for r in rows})}")
    print(f"записано             : {args.output}")
    if fails:
        print(f"не удалось скачать   : {len(fails)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())