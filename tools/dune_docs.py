#!/usr/bin/env python3
"""Зеркало документации Dune HD и поиск по нему.

Зачем. Внешней базы и MCP у проекта нет, а официальная документация Dune
разложена по ~40 страницам. Страница `search.cgi` работает, но отдаёт только
имена файлов и счётчики совпадений — без фрагментов текста (проверено:
`q=plugins` вернул 12 имён файлов и пустые `<pre>`). Пользоваться таким
поиском приходится вслепую.

Решение — скачать дерево один раз и искать локально. У документации есть
плоский текстовый дубль: `sdk/doc/html/<файл>.html` соответствует
`sdk/doc/txt/<файл>.txt`. Текстовая версия предпочтительнее: в ней заголовки
разделов оформлены линиями `===`, а HTML-теги и entity-ссылки не мешают.

Использование:
    dune_docs.py sync     # скачать/обновить корпус
    dune_docs.py list     # что скачано, что не удалось
    dune_docs.py grep ТЕКСТ  # поиск по корпусу (обычный grep -i по всем .txt)

Ограничение, о котором надо помнить: дерево НЕ содержит нативного C API
(`RUALock`, `librua`, `mum0`, `em8xxx0` — 0 совпадений, проверено поиском).
Всё про вывод кадра разбирается по бинарникам плеера.
"""

import os
import re
import subprocess
import sys

BASE_HTML = "https://files.dune-hd.com/sdk/doc/html/"
BASE_TXT = "https://files.dune-hd.com/sdk/doc/txt/"
CORPUS = os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "dune-sdk-doc")

HREF_RE = re.compile(r"href='([A-Za-z0-9_.+-]+\.html)'")


def fetch(url, dest):
    """Скачать url в dest. True при успехе, False при отказе."""
    if os.path.exists(dest):
        return True
    tmp = dest + ".part"
    rc = subprocess.call(["curl", "-sSf", "-L", "--max-time", "60",
                          "-o", tmp, url])
    if rc == 0 and os.path.getsize(tmp) > 0:
        os.replace(tmp, dest)
        return True
    if os.path.exists(tmp):
        os.unlink(tmp)
    return False


def page_names():
    """Список базовых имён страниц из оглавления index.html."""
    idx = os.path.join(CORPUS, "_index.html")
    if not fetch(BASE_HTML + "index.html", idx):
        raise SystemExit("не скачалось оглавление %s" % BASE_HTML)
    text = open(idx, encoding="utf-8", errors="replace").read()
    names = []
    for m in HREF_RE.finditer(text):
        n = m.group(1)
        if n not in names:
            names.append(n)
    return names


def do_sync():
    os.makedirs(CORPUS, exist_ok=True)
    names = page_names()
    ok, bad = [], []
    for n in names:
        stem = n[:-5]  # снять .html
        dest = os.path.join(CORPUS, stem + ".txt")
        if fetch(BASE_TXT + stem + ".txt", dest):
            ok.append(stem)
        else:
            bad.append(stem)
    print("скачано %d из %d страниц" % (len(ok), len(names)))
    if bad:
        print("нет текстовой версии (остаётся HTML):")
        for s in bad:
            print("  %s  ->  %s%s" % (s, BASE_HTML, s + ".html"))
    return 0


def do_list():
    for n in sorted(os.listdir(CORPUS)):
        p = os.path.join(CORPUS, n)
        if os.path.isfile(p):
            print("%9d  %s" % (os.path.getsize(p), n))
    return 0


def do_grep(terms):
    if not terms:
        raise SystemExit("нечего искать")
    pat = " ".join(terms)
    return subprocess.call(["grep", "-rin", "--", pat, CORPUS])


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    cmd = argv[1]
    if cmd == "sync":
        return do_sync()
    if cmd == "list":
        return do_list()
    if cmd == "grep":
        return do_grep(argv[2:])
    raise SystemExit("неизвестная команда: %s" % cmd)


if __name__ == "__main__":
    sys.exit(main(sys.argv))