# Makefile — PSP HLE Runtime для Dune HD Max
#
# Тулчейн лежит в соседнем проекте. Собирается динамически: на плеере нужен
# ld-musl-mipsel.so.1, а musl в статике не умеет dlopen.

TC=../Dune\ Max/toolchain/bootlin-mips32el-musl
GCC=$(TC)/bin/mipsel-buildroot-linux-musl-gcc
OBJDUMP=$(TC)/bin/mipsel-buildroot-linux-musl-objdump

# -Iinclude обязателен во всех правилах: без него не находятся psp_launcher.h.
#
# -Wl,-z,max-page-size=0x4000 обязателен: страницы памяти на плеере 16 КиБ,
# и бинарник с выравниванием 4 КиБ сегфолтится при запуске. Проверено
# sysconf(_SC_PAGESIZE) на живом устройстве.
CFLAGS=-O2 -Wall -Wextra -march=mips32r2 -mabi=32 -Iinclude
NM=$(TC)/bin/mipsel-buildroot-linux-musl-nm
RE=$(TC)/bin/mipsel-buildroot-linux-musl-readelf
LDFLAGS=-lpthread -ldl -Wl,-z,max-page-size=0x4000

# Рантайм. guest_load.c — проверенный загрузчик гостя, он заменил прежний
# elf_loader.c, который не умел ни релокации PRX, ни подмену стабов.
SRC=src/main.c \
    src/guest_load.c \
    src/psp_iso.c \
    src/psp_decrypt.c \
    src/syscall_hle.c \
    src/gfx_translator.c src/gfx_command.c src/ge_walk.c \
    src/psp_launcher.c

# libkirk нужен и инструменту, и рантайму: автозапуск розничной игры обязан
# расшифровывать образ сам, иначе psp-hle на файле ~PSP просто отказывается.
KIRK_SRC=third_party/libkirk/kirk_engine.c \
          third_party/libkirk/amctrl.c third_party/libkirk/bn.c \
          third_party/libkirk/ec.c third_party/libkirk/SHA1.c \
          third_party/libkirk/AES.c

TOOL_SRC=tools/cso_extract.c tools/iso_extract_eboot.c \
         tools/pspdecrypt.c tools/mem_probe.c
TEST_SRC=tests/guest_load_test.c tests/guest_enter_test.c \
          tests/guest_trap_trace.c tests/ptrace_probe.c \
          tests/trap_probe.c tests/gfx_command_test.c \
          tests/ge_walk_test.c

PLAYER=192.168.1.119
SHARE=//$(PLAYER)/DuneHDD_636782ed_d922_4d3c_ba41_02a68fe15053
SMB=smbclient $(SHARE) -N -m NT1 --option=client\ min\ protocol=NT1
KEY=../Dune\ Max/tmp/dune_key

BIN=psp-hle cso_extract iso_extract_eboot pspdecrypt mem_probe
TESTBIN=guest_load_test guest_enter_test guest_trap_trace ptrace_probe \
        trap_probe gfx_command_test uctx_layout ge_walk_test

all: $(BIN)

# ---------------------------------------------------------------------------
# Рантайм
# ---------------------------------------------------------------------------

psp-hle: $(SRC) $(KIRK_SRC)
	$(GCC) $(CFLAGS) -Ithird_party/libkirk -o $@ $(SRC) $(KIRK_SRC) $(LDFLAGS)

# ---------------------------------------------------------------------------
# Утилиты: каждая собирается со своим набором, потому что у них разные
# зависимости (pspdecrypt требует libkirk).
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Нативный плагин Dune: рантайм живёт ВНУТРИ `shell`.
#
# Требования к сборке неочевидны и каждое уже стоило рестарта интерфейса:
#
#   -nostdlib        на плеере glibc 2.8, а тулчейн musl; при линковке с
#                    библиотекой компоновщик не создаёт `.rela.dyn`/`.rela.plt`,
#                    GOT остаётся незаполненным, и `shell` падает
#   -fno-builtin     мы ОПРЕДЕЛЯЕМ `memset`; без этого GCC разворачивает её
#                    тело в вызов встроенной, линковщик резолвит вызов в нас же,
#                    и получается `bal` в собственное начало — бесконечная
#                    рекурсия и переполнение стека
#   -fvisibility=hidden   свои `malloc`/`free`/`memcpy` не должны попасть в
#                    глобальную область и перекрыть символы glibc у `shell`.
#                    Но точку входа это тоже скрывает, поэтому у неё стоит
#                    `visibility("default")` явно
#   -fno-stack-protector  `__stack_chk_fail` на плеере нечем заменить
# ---------------------------------------------------------------------------

PLUGIN_SRC=src/plugin/dune_plugin.c src/plugin/plat.c \
           src/plugin/guest_run.c src/plugin/gfx_out.c \
           src/guest_load.c src/ge_walk.c
PLUGIN_CFLAGS=-O2 -Wall -Wextra -march=mips32r2 -mabi=32 -Iinclude \
              -nostdlib -fno-builtin -fno-stack-protector \
              -fPIC -fvisibility=hidden

# `-Wl,--no-undefined` — не украшение, а запрет выпуска сломанной библиотеки.
#
# По умолчанию `-shared` НЕ требует разрешения символов: сборка с удалённым
# определением функции даёт готовый `.so`, который падает только на
# `dlopen(..., RTLD_NOW)` уже внутри плеера. Именно так была потеряна
# `guest_run.c`: 19 определений, `dlopen` молча отверг библиотеку, а
# `dune_run_native_code` не вызывался НИКОГДА — при том что все наблюдения
# выглядели как «гость падает». Гейт ниже это ловил, но гейт можно забыть
# прогнать, а линкер останавливает сборку всегда.
native_code.so: $(PLUGIN_SRC) src/plugin/plat.h src/plugin/guest_run.h \
                src/plugin/gfx_out.h include/guest_load.h
	$(GCC) $(PLUGIN_CFLAGS) -shared -Wl,--no-undefined -o $@ $(PLUGIN_SRC)

# Гейт самодостаточности библиотеки.
#
# Проверяет три вещи, каждая из которых уже стоила рестарта интерфейса:
# неразрешённых символов нет, `NEEDED` пуст, экспорт `dune_run_native_code`
# на месте. Третье — не паранойя: `-fvisibility=hidden` скрывает и точку
# входа, и `shell` тогда молча отвергает библиотеку.
#
# Первая проверка уже срабатывала: после `-fno-builtin` остался `U memcmp`.
check-plugin: native_code.so
	@sh tools/check_uctx.sh
	@sh tools/check_k0arg.sh
	@sh tools/check_faultfilter.sh
	@sh tools/check_reloc.sh
	@OBJDUMP=$(OBJDUMP) sh tools/check_ctor_trampoline.sh
	@echo "=== неразрешённые символы (должно быть пусто) ==="
	@$(NM) -u native_code.so; \
	  n=$$($(NM) -u native_code.so | wc -l); \
	  if [ "$$n" != "0" ]; then \
	    echo "ПРОВАЛ: неразрешённых символов $$n, библиотека не самодостаточна"; \
	    exit 1; \
	  fi
	@echo "=== NEEDED (должно быть пусто) ==="
	@$(RE) -dW native_code.so | grep -i needed && \
	  { echo "ПРОВАЛ: есть NEEDED, на плеере musl, а не glibc"; exit 1; } || true
	@echo "=== экспорт точки входа ==="
	@$(NM) -D native_code.so | grep -q ' T dune_run_native_code' || \
	  { echo "ПРОВАЛ: экспорт скрыт -fvisibility=hidden, shell отвергнет .so"; exit 1; }
	@echo "=== гейт пройден ==="

cso_extract: tools/cso_extract.c
	$(GCC) $(CFLAGS) -o $@ $<

iso_extract_eboot: tools/iso_extract_eboot.c src/psp_iso.c
	$(GCC) $(CFLAGS) -o $@ $^

pspdecrypt: tools/pspdecrypt.c src/psp_decrypt.c $(KIRK_SRC)
	$(GCC) $(CFLAGS) -Ithird_party/libkirk -o $@ $^

mem_probe: tools/mem_probe.c
	$(GCC) $(CFLAGS) -o $@ $<

# ---------------------------------------------------------------------------
# Проверки
# ---------------------------------------------------------------------------

tests: $(TESTBIN)

guest_load_test: tests/guest_load_test.c src/guest_load.c
	$(GCC) $(CFLAGS) -o $@ $^

# Ходячий по GE-спискам — чистый C, собирается и на хосте.
ge_walk_test: tests/ge_walk_test.c src/ge_walk.c
	$(GCC) $(CFLAGS) -o $@ $^

# Сборка байтов трамплина импорта для дизасма.
#
# Хостовая. Трамплин — это код, пишущийся в отображение гостя, и на хосте его
# иначе не проверить: на плеере он падает как SIGILL, а адрес инструкции для
# этого сигнала в ucontext недостоверен, то есть разбирать падение нечем.
# Инструмент собирает те же байты, что пишет src/guest_load.c, и отдаёт их
# objdump.
tramp_dump: tests/tramp_dump.c
	$(GCC) $(CFLAGS) -o $@ $<

guest_enter_test: tests/guest_enter_test.c src/guest_load.c
	$(GCC) $(CFLAGS) -o $@ $^

guest_trap_trace: tests/guest_trap_trace.c src/guest_load.c
	$(GCC) $(CFLAGS) -o $@ $^

ptrace_probe: tests/ptrace_probe.c
	$(GCC) $(CFLAGS) -o $@ $<

# Проба механизма ловушки. Запускается на плеере, не на хосте: часть
# выводов (сдвиг PC не восстанавливается, SA_ONSTACK не работает) верна
# только для ядра и musl плеера. На хосте эти свойства другие, и проба
# даст другой ответ — это не повод переносить выводы.
trap_probe: tests/trap_probe.c
	$(GCC) $(CFLAGS) -o $@ $<

# Проверка вычисления $gp по прологу функции.
#
# Хостовая и настоящая: ловит ошибку знакового расширения в `addiu` и
# несовпадение с символом `_gp` в таблице символов. Без неё пропадающая
# на 0x10000 ошибка выглядела бы как «гость падает в нашем коде».
HOSTGP=tmp/gpcheck.host

# Хостовый инструмент, собирающий байты трамплина импорта для дизасма.
HOSTTRAMP=tmp/tramp_dump.host

# Раскладка ucontext. Запускается только на плеере: смещения контекста зависят
# от его ядра и musl, на хосте они другие и проба даст другой ответ.
uctx_layout: tests/uctx_layout.c
	$(GCC) $(CFLAGS) -o $@ $<

# Проверка сборки команд GFX. Хостовая: собирает и сверяет байты команды,
# отправку не проверяет — SEND_GFX_COMMAND есть только внутри shell.
gfx_command_test: tests/gfx_command_test.c src/gfx_command.c
	$(GCC) $(CFLAGS) -o $@ $^ -ldl

# Проверка загрузчика на хосте.
#
# Отдельная сборка нативная: те же исходники, что и для плеера, но обычным
# gcc. Проверка загрузчика — переносимый C, и на хосте она осмысленна.
#
# Гостевые проверки (guest_enter_test, guest_trap_trace) на хосте запускать
# нельзя: гость это MIPS, а хост x86, поэтому они собираются только кросс-
# компилятором и работают лишь на плеере.
HOSTCC?=gcc
HOSTTEST=tmp/guest_load_test.host
HOSTGFX=tmp/gfx_command_test.host

# Адреса `hle_import` и `_gp` берутся из таблицы символов собранного бинарника,
# а не зашиваются в Makefile: они меняются при каждой правке исходников, и
# проверка молча проверяла бы устаревшее число.
gp-syms: psp-hle
	$(OBJDUMP) -t $< | awk '$$2 != "" {printf "%s %s %s\n", $$1, $$3, $$NF}' \
	    > tmp/gp_syms.txt

# Самопроверка инструмента поиска записей в $k0.
#
# Гоняется на КРОСС-СОБРАННОМ MIPS-коде, а не на хосте: инструмент ищет
# инструкции MIPS, и на хосте проверить нечего. Программа перечисляет 8 форм
# записи и 3 контрольные, которые писать не должны.
#
# Без этой проверки инструмент выдаёт правдоподобный «ноль записей в $k0»
# по коду гостя — а ноль означал бы дефект инструмента, а не свойство гостя.
# Именно так и случилось: инструмент был неполон и молчал.
# Проба раскладки ucontext при сбое по выборке команды. Запускается на плеере,
# не на хосте: раскладка и свойства ядра там свои.
uctx_segv: tests/uctx_segv.c
	$(GCC) $(CFLAGS) -static -o $@ $<

find-k0-selftest: tests/find_k0_test.c tools/find_k0.py tools/check_find_k0.sh
	$(GCC) -O1 -march=mips32r2 -mabi=32 -static -o $@ $<
	tools/check_find_k0.sh $@

# Дифференциальный тест арифметики GE: сравнивает наш порт с оригиналом
# PPSSPP на 2,6 миллионах случайных входов плюс побайтовую сверку таблиц.
#
# Отдельной целью, а не частью `check-plugin`, потому что собирается на хосте
# с libm: в плагине `-nostdlib`, и вызовы `floorf`/`ldexpf` дали бы неразрешённые
# символы. Поэтому в `ge_math.c` вместо libm — целочисленные приёмы.
ge-math-test: src/ge_math.c include/ge_math.h tests/ge_math_test.c \
              tests/ge_math_tables_ref.h
	$(HOSTCC) -O2 -Wall -Wextra -Iinclude -Itests -o tmp/ge_math_test \
	    tests/ge_math_test.c src/ge_math.c -lm
	tools/check_gemath.sh

check: tests find-k0-selftest ge-math-test
	$(HOSTCC) -O2 -Wall -Wextra -o $(HOSTGP) tools/gpcheck.c
	$(HOSTCC) -O2 -Wall -Wextra -Iinclude -o $(HOSTTEST) \
	    tests/guest_load_test.c src/guest_load.c
	$(HOSTTEST) tmp/real/EBOOT.dec.BIN
	$(MAKE) -s gp-syms
	$(HOSTGP) psp-hle 0x$$(awk '/ hle_import$$/{print $$1; exit}' tmp/gp_syms.txt) 0x$$(awk '/ _gp$$/{print $$1; exit}' tmp/gp_syms.txt)
	$(HOSTCC) -O2 -Wall -Wextra -Iinclude -o $(HOSTTRAMP) \
	    tests/tramp_dump.c src/guest_load.c
	$(HOSTTRAMP) tmp/tramp.bin > /dev/null
	tools/check_tramp.sh $(OBJDUMP) tmp/tramp.bin
	$(HOSTCC) -O2 -Wall -Wextra -Iinclude -o $(HOSTGFX) \
	    tests/gfx_command_test.c src/gfx_command.c -ldl
	$(HOSTGFX)

# ---------------------------------------------------------------------------
# Доставка на плеер
#
# scp на плеере НЕ работает: нет sftp-сервера. Файлы передаются через Samba
# (SMB1/NT1, шара /D) либо через ssh с перенаправлением, что нужно для путей
# с пробелами.
# ---------------------------------------------------------------------------

upload: $(BIN)
	$(SMB) -c 'cd psp_launcher/bin; put psp-hle psp-hle'
	$(SMB) -c 'cd psp_launcher/bin; put cso_extract cso_extract'
	$(SMB) -c 'cd psp_launcher/bin; put iso_extract_eboot iso_extract_eboot'
	$(SMB) -c 'cd psp_launcher/bin; put pspdecrypt pspdecrypt'
	$(SMB) -c 'cd psp_launcher/bin; put mem_probe mem_probe'

upload-tests: tests
	$(SMB) -c 'cd psp_launcher/bin; put guest_load_test guest_load_test'
	$(SMB) -c 'cd psp_launcher/bin; put guest_enter_test guest_enter_test'
	$(SMB) -c 'cd psp_launcher/bin; put guest_trap_trace guest_trap_trace'
	$(SMB) -c 'cd psp_launcher/bin; put ptrace_probe ptrace_probe'
	$(SMB) -c 'cd psp_launcher/bin; put uctx_layout uctx_layout'
	$(SMB) -c 'cd psp_launcher/bin; put uctx_segv uctx_segv'

# Гость на плеере. Шаги пронумерованы и с проверкой живости: при падении
# непонятно, на каком шаге сломалось.
run: upload upload-tests
	@echo "ШАГ 1: ping до"; ping -c 2 -W 2 $(PLAYER) | grep packets | tail -1
	@echo "ШАГ 2: контроль загрузчика"
	ssh -i "$(KEY)" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	    -o LogLevel=ERROR -o ConnectTimeout=10 root@$(PLAYER) \
	    'cd /D/psp_launcher/bin && ./ld-musl-mipsel.so.1 ./guest_load_test /D/psp_launcher/games/EBOOT.dec.BIN'
	@echo "ШАГ 3: вход в гостевой код"
	ssh -i "$(KEY)" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	    -o LogLevel=ERROR -o ConnectTimeout=10 root@$(PLAYER) \
	    'cd /D/psp_launcher/bin && ./ld-musl-mipsel.so.1 ./guest_trap_trace /D/psp_launcher/games/EBOOT.dec.BIN'
	@echo "ШАГ 4: ping после"; ping -c 2 -W 2 $(PLAYER) | grep packets | tail -1

clean:
	rm -f $(BIN) $(TESTBIN) guest_load_test

.PHONY: all tests check upload upload-tests run clean