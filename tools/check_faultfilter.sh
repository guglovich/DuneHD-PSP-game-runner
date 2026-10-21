#!/bin/sh
# Гейт на ПОВЕДЕНИЕ обработчика сбоя: фильтр чужих сигналов и запись PC.
#
# Зачем он, когда есть check_uctx/check_k0arg. Те два проверяют СМЫСЛ
# констант и смещений. Этот проверяет то, чего у гейтов не было вовсе:
# поведение при сигнале от потока, который не внесён в фильтр.
#
# Именно этот дефект дважды ронял плеер, причём оба раза make check-plugin
# был зелёным:
#   1. фильтр чужих сигналов уходил `return` без правки PC — SIGILL
#      повторялся, процесс умирал (`status = 5`), интерфейс гас;
#   2. клон потока-конструктора не был внесён в фильтр, и его сигнал
#      убил весь процесс вместо потока.
# Символы и NEEDED при этом были в порядке, поэтому гейт их проходил.
#
# Проверяется на НАМЕРЕННО ИСПОРЧЕННЫХ копиях. Без этого гейт зелёный
# только потому, что автор сломал модель так же неверно, как в коде.

set -e

die() { echo "ПРОВАЛ: $1"; exit 1; }

T=tests/fault_filter_test.c
[ -f "$T" ] || die "нет $T"

echo "=== фильтр чужих сигналов: поведение ==="

CC=${CC:-gcc}
OUT=tmp/fault_filter_test
mkdir -p tmp

$CC -O2 -Wall -Wextra -o "$OUT" "$T" || die "тест не собрался"
"$OUT" || die "тест провалился на настоящей модели"

# --- три намеренно испорченные копии ------------------------------------
# Каждая ломает ровно одну защищённую линию. Если хоть одна пройдёт —
# гейт ловит не то, ради чего написан.
broken="$OUT.broken.c"

mangle_apply() {
	# $1 - имя, $2 - что ищем, $3 - на что меняем
	python3 -c '
import sys
name, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
src = open("tests/fault_filter_test.c").read()
if old not in src:
    sys.exit(80)
if src.count(old) != 1:
    sys.exit(81)
open("tmp/fault_filter_broken.c", "w").write(src.replace(old, new))
' "$1" "$2" "$3"
	[ $? -eq 0 ] || die "подмена «$1» не применилась к исходнику"
}

# 1. чужая ветка без правки PC — ровно то, что убило плеер
mangle_apply "чужой поток без правки PC" \
'	if (!guest_thread_fault(tid)) {
		if (pc_v >= BASE && pc_v < BASE + SPAN) {
			write_pc(slot, pc_v);
			m_pc_fixed = 1;
		}
		return;
	}' \
'	if (!guest_thread_fault(tid)) {
		return;
	}'
$CC -O2 -Wall -Wextra -o "$OUT.b1" tmp/fault_filter_broken.c 2>/dev/null
if "$OUT.b1" >/dev/null 2>&1; then
	die "испорченная копия «чужой поток без правки PC» пройдена — гейт не ловит убийцу плеера"
fi

# 2. байты PC пишутся не от pc+4 — моя первая версия, ломает перенос 0x10000
mangle_apply "запись PC не от pc+4" \
'	uint32_t np = pc_v + 4;

	slot[0] = (unsigned char)(np & 0xFF);
	slot[1] = (unsigned char)((np >> 8) & 0xFF);
	slot[2] = (unsigned char)((np >> 16) & 0xFF);
	slot[3] = (unsigned char)((np >> 24) & 0xFF);' \
'	slot[0] = (unsigned char)((pc_v + 4) & 0xFF);
	slot[1] = (unsigned char)((pc_v >> 8) & 0xFF);
	slot[2] = (unsigned char)((pc_v >> 16) & 0xFF);
	slot[3] = (unsigned char)((pc_v >> 24) & 0xFF);'
$CC -O2 -Wall -Wextra -o "$OUT.b2" tmp/fault_filter_broken.c 2>/dev/null
if "$OUT.b2" >/dev/null 2>&1; then
	die "испорченная копия «запись PC не от pc+4» пройдена — гейт слеп к переносу 0x10000"
fi

# 3. фильтр считает чужим даже свой поток
mangle_apply "свой поток тоже чужой" \
'	if (tid == 0)
		return 1;
	if (tid == m_guest_pid)
		return 1;' \
'	if (tid == 0)
		return 1;'
$CC -O2 -Wall -Wextra -o "$OUT.b3" tmp/fault_filter_broken.c 2>/dev/null
if "$OUT.b3" >/dev/null 2>&1; then
	die "испорченная копия «свой поток тоже чужой» пройдена — гейт слеп к фильтру"
fi

echo "=== гейт пройден (3 испорченные копии отвергнуты) ==="

# --- проверки НАСТОЯЩЕГО `guest_run.c`, а не модели ----------------------
# Зачем отдельно от теста выше. Тест проверяет СВОЮ модель фильтра. Пока
# модель отдельна, порча настоящего кода проходит гейт зелёным — проверено:
# подмена границы образа на `< g_mod->base + 1u` собирается и гейт молчит.
# Это ровно тот дефект, что ронял плеер. Поэтому настоящий файл проверяется
# отдельно, как в `check_k0arg.sh`: по исходнику, а не по символам.
SRC=src/plugin/guest_run.c
ok=1

if ! grep -qF 'unsigned char *pcp = (unsigned char *)uc + UCONTEXT_PC;' "$SRC"; then
	echo "ПРОВАЛ: в чужой ветке нет правки PC — сигнал повторится и процесс умрёт"
	ok=0
fi

if grep -qE 'base \+ 1u\)' "$SRC"; then
	echo "ПРОВАЛ: граница образа сужена до base + 1u"
	ok=0
fi

if ! grep -qF 'if (!guest_thread_fault((u32)plat_gettid())) {' "$SRC"; then
	echo "ПРОВАЛ: нет фильтра tid потоков гостя"
	ok=0
fi

# 5. Проверка границы разыменования БЕЗ переполнения. Прежний вид
#    `eff + 4 <= mod->base + mod->map_size` сворачивается при
#    `eff >= 0xFFFFFFFC` и пропускает чтение по адресу ~0 прямо ВНУТРИ
#    обработчика. Замер плеера: `FAULT=3` идёт БЕЗ строки i0/i1 (она
#    печатается для каждого сбоя), следом `FAULT=4 sig=000B addr=00000002
#    pc=2D0C8AE8` — это `lwl t1,3(v0)` в `guest_fault_handler`, и после
#    этого конструкторы больше не идут.
#
#    Строки-комментарии отсекаются. Без этого гейт ловит саму цитату
#    прежней формулы, написанную в пояснении к исправлению, и падает на
#    ИСПРАВНОМ коде — так и случилось с первой версией этой проверки.
if grep -vE '^[[:space:]]*(\*|/\*|//)' "$SRC" \
   | grep -qE 'eff \+ 4 <= mod->base'; then
	echo "ПРОВАЛ: граница разыменования сворачивается (eff + 4)"
	ok=0
fi
if ! grep -qF '(u64)eff + 4 <= (u64)mod->base + mod->map_size' "$SRC"; then
	echo "ПРОВАЛ: нет формы границы без переполнения (u64)eff + 4"
	ok=0
fi

if [ "$ok" = "0" ]; then
	echo "ПРОВАЛ: настоящий $SRC расходится с моделью фильтра"
	exit 1
fi
echo "=== настоящий guest_run.c согласован с моделью ==="
