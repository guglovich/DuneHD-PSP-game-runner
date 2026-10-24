/* Нативный плагин Dune HD: доказательство, что нативный код исполняется
 * внутри `shell`.
 *
 * Зачем. Задача проекта — исполнить гостя PSP. Способы, которые мы уже
 * исключили, требуют постороннего процесса, а он лишён доступа к Sigma.
 * Плагин же грузит ХОСТ, то есть `shell`, и потому ограничения на
 * посторонние процессы к нему не относятся. Это подтверждено практикой:
 * плагин DOOM сообщества загрузился, получил управление и упал уже внутри
 * собственной проверки.
 *
 * Механизм (измерено на плеере, `shell.log`):
 *
 *     plugin_run_native_code(/persistfs/plugins/doom, dummy)
 *     [native_plugins_handler] open plugin .../native_code.so
 *     [native_plugins_handler] plugin opened
 *     [native_plugins_handler] call dummy
 *     [native_plugins_handler] run
 *
 * То есть `shell` ищет экспорт `dune_run_native_code` и вызывает его с
 * указателем на структуру задания. Разобранная в `.so` от DOOM обработка
 * выглядит так:
 *
 *     beqz a0, default          # нет задания
 *     lw   v1,4(a0); beq v1,1   # поле +4 должно быть 1
 *     lw   v0,8(a0); bltz v0     # поле +8 >= 0
 *     lw   v0,0(a0); slti v0,36  # поле +0 >= 36
 *     sw   a0, global            # запомнить задание
 *     lw   t9,32(a0)             # поле +32: УКАЗАТЕЛЬ НА ФУНКЦИЮ
 *     jalr t9                    # вызвать её
 *
 * То есть `main` не нужен: хост сам указывает, что запускать.
 *
 * Про зависимости. Библиотека собрана БЕЗ внешних ссылок, на сырых
 * системных вызовах. Причина: наш тулчейн musl, а на плеере glibc 2.8.
 * Прежние попытки внедрения через `ld.so.preload` были отвергнуты именно
 * потому, что линковка с musl тянула несуществующий здесь `libc.so` и
 * загрузчик отбрасывал preload-библиотеку целиком.
 */

typedef unsigned int   u32;
typedef int            s32;

#include <string.h>
#include <sys/mman.h>

#include "plat.h"
#include "guest_load.h"
#include "guest_run.h"
#include "gfx_out.h"

/* Причина, по которой `put` молча перестал писать.
 *
 * На прогоне 2026-10-13 между `gmark_worker=0x47554554` и `maps0800=` в отчёте
 * нет НИ `iow_count=`, НИ его значения — то есть пропал весь блок из 21 байта,
 * тогда как записи вокруг него дошли. Дизассемблирование СОБРАННОЙ библиотеки
 * показало, что `PUT("iow_count=")` и следующий `put_hex` в коде есть и
 * получают правильные длины (`s1 = 10` и `11`), то есть исходник и бинарник
 * исправны, а пропало уже на устройстве.
 *
 * Единственная причина, которой `put` может молча не записать, — `plat_write`
 * вернул `<= 0`. Она и измеряется: сколько раз, с какой длиной и с каким
 * результатом. Ничего не предполагается. */
static volatile unsigned put_fail_cnt;
static volatile unsigned put_fail_len;
static volatile int      put_fail_rc;

/* Пишет целиком, без libc: `write` может вернуть меньше, чем просили. */
static void put(int fd, const char *s, unsigned len)
{
	while (len != 0) {
		long n = plat_write(fd, s, len);

		if (n <= 0) {
			put_fail_cnt++;
			put_fail_len = len;
			put_fail_rc = (int)n;
			return;
		}
		s += n;
		len -= (unsigned)n;
	}
}

/* Печать без libc: только шестнадцатеричные числа.
 *
 * Буфер на 11 байт: `0x`, восемь цифр и перевод строки. Размер 9 при
 * фактической записи 11 давал переполнение стека, на которое указал
 * компилятор (`-Wstringop-overflow`).
 */
/* Номера системных вызовов для чтения `/proc/self/maps`.
 *
 * На MIPS o32 это `__NR_Linux + 3` и `+ 6`, то есть 4003 и 4006. Плагин
 * собран с `-nostdlib`, подключать `<asm/unistd.h>` здесь нечем. */
#define PROBE_SYS_read  4003
#define PROBE_SYS_close 4006

/* Разбор одной шестнадцатеричной цифры. Свой, а не `strtoul`: библиотека
 * собрана с `-nostdlib`, и обращение к libc создало бы неразрешённый символ,
 * из-за которого `shell` отверг бы весь плагин при `dlopen`. */
static u32 hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return (u32)(c - '0');
	if (c >= 'a' && c <= 'f')
		return (u32)(c - 'a') + 10u;
	if (c >= 'A' && c <= 'F')
		return (u32)(c - 'A') + 10u;
	return 0;
}

static void put_hex(int fd, u32 v)
{
	static const char digits[] = "0123456789ABCDEF";
	char buf[11];
	int i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 8; i++)
		buf[2 + i] = digits[(v >> ((7 - i) * 4)) & 0xF];
	buf[10] = '\n';
	put(fd, buf, 11);
}

/* Индекс элемента списка: ровно две цифры и БЕЗ перевода строки.
 *
 * `put_hex` дописывает `'\n'`, поэтому индекс печатался на своей строке, а
 * после литерала `"]=0x"` ещё и `put_hex` давал второе `0x` — строка
 * разъезжалась на три и разбирать её пришлось глазами. */
static void put_idx(int fd, u32 v)
{
	static const char d[] = "0123456789ABCDEF";
	char b[2];

	b[0] = d[(v >> 4) & 0xF];
	b[1] = d[v & 0xF];
	put(fd, b, 2);
}

#define MARKER "/tmp/run/psp_plugin.txt"

/* Сколько узлов списка atexit печатать в отчёте. */
#define ATEXIT_REPORT_MAX 64

/* Печать строки литерала без ручного счёта длины.
 *
 * Раньше длина писалась числом, и это четыре раза дало обрезанный вывод —
 * дважды потому, что число просто не совпадало, и дважды потому, что строки
 * с русским текстом многобайтные: `--- гость ---` это 15 байт, а не 13
 * символов. `sizeof` считает байты и ошибаться не может. */
#define PUT(fd, s) put((fd), (s), sizeof(s) - 1)

/* База и размер образа гостя.
 *
 * `GUEST_BASE` = `0x08804000` — тот адрес, который зашит в абсолютные
 * ссылки гостя, поэтому отображать его можно только туда.
 *
 * `GUEST_SPAN` — 4.83 МБ, измерено по PT_LOAD образа: `p_memsz` = 4 833 664
 * при базе `0x00000000`. Плюс запас на пул трамплинов и на округление до
 * страницы в 16 КиБ. */
#define GUEST_BASE  0x08804000u
#define GUEST_SPAN  0x00500000u

/* Путь к образу гостя на HDD.
 *
 * Образ лежит файлом, а не встроен в `.so`: он 5.3 МБ, и вшивать его в
 * библиотеку незачем — HDD уже смонтирован, а установщик его и так
 * раскладывает рядом. */
#define GUEST_IMAGE "/D/psp_plugin_probe/guest.elf"

/* Читает очередную строку файла, не длиннее `cap-1`.
 *
 * Возвращает длину строки (без перевода строки) либо 0 в конце файла.
 * Собственная реализация, потому что libc здесь нет, а `fgets` не
 * определён. Признак конца строки и `'\0'` — разные байты, поэтому
 * `'\r'` тоже отбрасывается: карта памяти на этом плеере пишется
 * с переводом строки CR LF в некоторых сборках ядра. */
static long plat_read_line(int fd, char *buf, size_t cap)
{
	size_t n = 0;
	long got;

	for (;;) {
		char c;
		long one = plat_syscall(4003 /*read*/, fd, (long)&c, 1, 0);

		if (one != 1)
			break;
		if (c == '\n')
			break;
		if (c == '\r')
			continue;
		if (n + 1 < cap)
			buf[n++] = c;
	}
	buf[n] = 0;
	got = (long)n;
	return got;
}

/* Разбирает `начало-конец` из строки карты памяти.
 *
 * Возвращает 1, если оба числа разобраны, и 0 иначе. Числа шестнадцатеричные
 * и разной длины, поэтому берём по символу до `-`, а не по фиксированному
 * смещению. */
static int parse_hex_range(const char *line, unsigned long *a, unsigned long *b)
{
	unsigned long v1 = 0, v2 = 0;
	int seen = 0, which = 0, digits = 0;

	for (; *line != 0; line++) {
		char c = *line;
		int d;

		if (c == '-') {
			if (digits == 0)
				return 0;
			which = 1;
			digits = 0;
			seen++;
			continue;
		}
		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			break;

		digits++;
		if (which == 0)
			v1 = v1 * 16 + (unsigned long)d;
		else
			v2 = v2 * 16 + (unsigned long)d;
	}
	if (seen != 1 || digits == 0)
		return 0;
	*a = v1;
	*b = v2;
	return 1;
}

/* Свободен ли диапазон в КАРТЕ СОБСТВЕННОГО ПРОЦЕССА.
 *
 * Зачем проверять, а не полагаться на `mmap`. Ядро на этом плеере подсказку
 * адреса отбрасывает: в отдельном процессе просили `0x08804000`, вернули
 * `0x2BEE0000`, — при том что `/proc/self/maps` показывал адрес свободным.
 * Обратная проверка тоже недостаточна: сказать «адрес занят» по карте
 * недостаточно, сказать «свободен» — необходимо, потому что дальше
 * `MAP_FIXED` подменяет отображение безусловно.
 *
 * `MAP_FIXED_NOREPLACE` на ядре 2.6.22 отсутствует — флаг появится заметно
 * позже, — то есть защиты от подмены на стороне ядра нет. Единственная
 * остающаяся защита: прочитать свою же карту памяти перед тем, как просить
 * фиксированный адрес.
 *
 * Читается `/proc/self/maps` — обычный файл, ни одного устройства Sigma. */
static int range_is_free(unsigned long start, unsigned long end)
{
	char line[256];
	int fd, free_range = 1, seen_any = 0;


	fd = plat_open_read("/proc/self/maps");
	if (fd < 0)
		return 0;                    /* не смогли проверить — не идём */

	while (plat_read_line(fd, line, sizeof(line)) > 0) {
		unsigned long v1 = 0, v2 = 0;

		seen_any = 1;
		/* Строка вида `2aaa8000-2aac8000 r-xp ...`. Разбираем только
		 * начало: остальное для решения не нужно. */
		if (parse_hex_range(line, &v1, &v2) != 0)
			continue;
		if (v1 < end && start < v2)
			free_range = 0;
	}
	plat_syscall(4006 /*close*/, fd, 0, 0, 0);

	/* Если не прочитали НИ ОДНОЙ строки, проверку считать неуспешной:
	 * пустой результат здесь не доказательство свободы. */
	return free_range && seen_any;
}

/* Загрузка гостя и отчёт о ней.
 *
 * Шаг сознательно ограничен: загрузка идёт, а гостевой код НЕ запускается.
 * Задача — проверить, что загрузчик работает внутри `shell` на реальном
 * образе, до того как начнёт исполняться чужой машинный код в процессе
 * интерфейса. Ошибка здесь стоит рестарта `shell`; ошибка в гостевом коде —
 * тем более.
 *
 * Отчёт снимает то, что должно быть проверено измерением:
 *   - свободен ли адрес по собственной карте памяти;
 *   - совпал ли адрес отображения с запрошенным;
 *   - число сегментов и их `p_memsz`;
 *   - `gp`, `ent_top`, `ent_end` из `.rodata.sceModuleInfo`;
 *   - сколько импортов найдено и сколько библиотек.
 *
 * Ожидаемые значения сняты с образа заранее: 1 сегмент, `p_memsz` = 4 833 664,
 * 64 библиотеки. Расхождение означает не «гость сломан», а что наша копия
 * образа на устройстве не та, что разобрана здесь. */
/* Отчёт о вызовах импортов, сделанных гостем.
 *
 * Печатаются ИНДЕКС, NID и адрес возврата. Индекс — ключ ко всему: по нему
 * восстанавливаются и NID, и номер трамплина, и адрес стаба в госте. По NID
 * обратного перехода нет — одна и та же функция встречается в нескольких
 * библиотеках, и по её названию адрес в госте не находится. Имена функций в
 * коде гостя отсутствуют, таблица имён лежит в `data/psp_nids.tsv`.
 *
 * Показаны ВСЕ сохранённые записи (ёмкость LOG_MAX): прогоны 36–37
 * показали, что диагностически важные вызовы оказываются и в середине
 * кольца (120 неподменённых NID прогона 37 были невидимы при печати
 * «первые 24 + хвост 32»). При переполнении кольца записи сверх LOG_MAX
 * не хранятся — счётчик `log_overflow` в отчёте показывает потерю. */
static void report_imports(int fd)
{
	u32 n = guest_run_calls();
	u32 limit = guest_run_log_max();
	u32 i;

	PUT(fd, "\n--- гость пошёл ---\n");
	PUT(fd, "guest_pid=");
	put_hex(fd, (u32)guest_run_pid());
	PUT(fd, "hle_imports=");
	put_hex(fd, guest_run_hle_count());
	PUT(fd, "calls=");
	put_hex(fd, n);
	PUT(fd, "unknown_nid=");
	put_hex(fd, guest_run_unknown());
	/* ИМЯ поля важно: это значение `$k0`, записанное САМИМ трамплином перед
	 * возвратом в гостя, а не индекс импорта и не число вызовов. Прежнее
	 * имя `tramp_done=` читалось как счётчик, и в отчёте получилось видимое
	 * противоречие («4 вызова, а тут 84»), которого не было.
	 * Заменять его на индекс нельзя было смысла: индекс и так печатается
	 * обработчиком построчно. */
	PUT(fd, "tramp_k0=");
	put_hex(fd, guest_run_tramp());
	/* Метки входа в гостя, записанные ассемблером в точке прыжка.
	 *
	 * Метки РАЗДЕЛЬНЫЕ по потокам, и это вынужденная мера, а не украшение.
	 * Общая на оба потока переменная дала ложный ответ на прошлом прогоне:
	 * её успевал устанавливать ГЛАВНЫЙ поток — он действительно входит в
	 * гостя и делает четыре вызова импортов, — а сбой при этом
	 * происходил в РАБОЧЕМ. Метка стояла, и это читалось как «гость
	 * введён», то есть тест не отвечал на свой вопрос.
	 *
	 * Хранятся в отдельных глобальных переменных, а не на стеке потока:
	 * гость выделяет кадр на 1008 байт и затирает весь стек до вершины, а
	 * блок контекста по смещению `+0xf8` перезаписывает собственная заливка
	 * `0xffffffff`. Обе попытки поставить метку на стек дали чужое значение. */
	/* Базовая отметка ДО первого из подозреваемых полей: она обязана быть
	 * нулём на любом здоровом прогоне, иначе `put` уже отказывал до них. */
	PUT(fd, "putfail0=");
	put_hex(fd, put_fail_cnt);
	PUT(fd, "gmark_main=");
	put_hex(fd, guest_run_mark_main());
	PUT(fd, "gmark_worker=");
	put_hex(fd, guest_run_mark_worker());
	PUT(fd, "iow_count=");
	put_hex(fd, guest_run_iow_count());
	PUT(fd, "putfail=");
	put_hex(fd, put_fail_cnt);
	PUT(fd, "putflen=");
	put_hex(fd, put_fail_len);
	PUT(fd, "putferr=");
	put_hex(fd, (u32)put_fail_rc);

	/* Список atexit гостя: сколько объектов зарегистрировано и где.
	 *
	 * Показывает, что именно регистрируется дважды, без единой правки
	 * гостевого кода и без ловушек — чистое чтение нашего отображения.
	 * Разбор: `docs/guest-atexit-abort.md`. */
	{
		static u32 nodes[ATEXIT_REPORT_MAX];
		u32 hi = 0, lo = 0;
		u32 why = guest_run_atexit_raw(&hi, &lo);
		u32 head = guest_run_atexit_head();
		u32 i, n = guest_run_atexit_nodes(nodes, ATEXIT_REPORT_MAX);

		/* Причина обязана идти ПЕРВОЙ: без неё все варианты «адрес не
		 * получен» дают один и тот же ноль и отчёт не отвечает, почему. */
		PUT(fd, "  atexit_why=");
		put_hex(fd, why);
		PUT(fd, "  atexit_hi=");
		put_hex(fd, hi);
		PUT(fd, "  atexit_lo=");
		put_hex(fd, lo);
		PUT(fd, "  atexit_filelo=");
		put_hex(fd, guest_run_atexit_file_lo());
		PUT(fd, "  atexit_head=");
		put_hex(fd, head);
		PUT(fd, "  atexit_n=");
		put_hex(fd, n);
		/* Снимок, снятый в момент печати abort'а: к моменту отчёта
		 * список уже очищен `exit`, и без снимка он всегда пуст. */
		PUT(fd, "  atexit_snapn=");
		put_hex(fd, guest_run_atexit_snap_n());
		PUT(fd, "  atexit_snaphead=");
		put_hex(fd, guest_run_atexit_snap_head());
		/* Узлы снимка, снятого в момент печати abort'а, и СЫРОЕ поле `next`.
	 *
	 * Печатается именно снимок, а не список, прочитанный в отчёте: момент
	 * печати заведомо предшествует `exit`. Поле `next` обязано идти рядом —
	 * без него кольцо в списке и обрыв цепочки выглядят в отчёте одинаково,
	 * и вывод приходится делать по догадке. */
		for (i = 0; i < ATEXIT_REPORT_MAX; i++) {
			u32 a = guest_run_atexit_snap_at(i);
			u32 b;

			if (i >= guest_run_atexit_snap_n())
				break;
			b = guest_run_atexit_snap_next_at(i);
			PUT(fd, "\n  atexit[");
			put_idx(fd, i);
			PUT(fd, "]=");
			put_hex(fd, a);
			PUT(fd, " next=0x");
			put_hex(fd, b);
		}
		PUT(fd, "\n");

		/* Окрестности первого узла списка в рантайме.
		 *
		 * Узел `0x00324ACC` — тот самый, что замыкает кольцо, и рядом
		 * с ним `0x00324AC4`, куда первый конструктор пишет `0xFFFFFFFF`.
		 * В файле вся область нулевая, поэтому судить можно только по
		 * памяти после загрузки: если `0xFFFFFFFF` не дожил, это запись
		 * с нашей стороны, а не поведение гостя. */
		{
			static u32 win[12];
			u32 got = guest_run_peek(0x00324AC0u, 12, win);

			PUT(fd, "  peek324AC0_n=");
			put_hex(fd, got);
			for (i = 0; i < got; i++) {
				PUT(fd, "\n  peek[");
				put_idx(fd, i);
				PUT(fd, "]=");
				put_hex(fd, win[i]);
			}
			PUT(fd, "\n");
		}

		/* Окрестности переменной головы списка atexit.
		 *
		 * Голова живёт по `vaddr 0x003B1AA0` в `.bss`. Кто записал туда
		 * `0x324ACC`, в коде гостя не находится: адрес собирается ровно в
		 * одном месте, и это `ctor[00]`, выполненный один раз. Значит
		 * запись пришла из функции, чьи адреса заданы релокацией и в файле
		 * не читаются, — прежде всего из `sbrk`, который в этом же окне
		 * пишет в свои глобалы.
		 *
		 * Соседние слова — прямой проверяемый признак: если рядом лежит
		 * значение, похожее на адрес блока кучи, значит пишет `sbrk`.
		 * Разбор соседей по именам не годится: символов у гостя нет. */
		{
			static u32 hwin[24];
			u32 got = guest_run_peek(0x003B1A80u, 24, hwin);

			PUT(fd, "  peekhead_n=");
			put_hex(fd, got);
			for (i = 0; i < got; i++) {
				PUT(fd, "\n  hwin[");
				put_idx(fd, i);
				PUT(fd, "]=");
				put_hex(fd, hwin[i]);
			}
			PUT(fd, "\n");
		}

		/* Голова списка atexit перед входом в гостя — по потокам.
		 * Прибор атрибуции: по коду гостя узел `0x324ACC` зарегистрировать
		 * некому (адрес собирается ровно в `ctor[00]`), но он в списке
		 * есть даже при пропущенном `ctor[00]`. */
		PUT(fd, "  head_before_main=0x");
		put_hex(fd, guest_run_head_before_main());
		PUT(fd, "\n  head_before_worker=0x");
		put_hex(fd, guest_run_head_before_worker());
		PUT(fd, "\n  len_before_main=0x");
		put_hex(fd, guest_run_len_before_main());
		PUT(fd, "\n  len_before_worker=0x");
		put_hex(fd, guest_run_len_before_worker());
		PUT(fd, "\n");

		/* Слова ЗАГРУЖЕННОГО образа на месте вызова сравнения.
		 *
		 * Причина: по файлу `lui $s2,0x31` + `addiu $s2,$s2,-0x4ab8` дают
		 * `0x30BB48`, и это указывает в `.rodata`, где лежит строка
		 * `"TogeRadius"`, а не таблица пар указателей с шагом 8, которую
		 * читает код. Значит загруженный `lui` другой — а адрес таблицы
		 * известен только из памяти.
		 *
		 * Первая попытка пекла `0x0BCC00` — это ошибка на 0x570, то есть
		 * соседняя функция, и выглядело как «данные в коде».
		 *
		 * Это ровно тот случай, из-за которого дизасм ФАЙЛА непригоден:
		 * опкод верный, а непосредственный операнд уже переписан релокацией. */
		{
			static u32 cw[72];
			u32 got = guest_run_peek(0x0BD170u, 72, cw);

			PUT(fd, "  cmpwords_n=");
			put_hex(fd, got);
			for (i = 0; i < got; i++) {
				PUT(fd, "\n  cw[");
				put_idx(fd, i);
				PUT(fd, "]=0x");
				put_hex(fd, cw[i]);
			}
			PUT(fd, "\n");
		}

		/* Зеркало без кэша (`0x40000000`) — второй вид той же памяти PSP.
		 *
		 * Прибор к двойному отображению: ожидаю `0x48804000`. Ноль —
		 * tmpfs недоступен или адрес занят; тогда гость сыплет
		 * recoverable-сбоями на `0x48…` и записи теряются. */
		PUT(fd, "  mirror=0x");
		put_hex(fd, guest_run_mirror_base());
		PUT(fd, "\n");

		/* VRAM PSP: `sceGeEdramGetAddr` возвращает его гостю.
		 * Ожидаю `0x04000000`; 0 — VRAM нет, графический путь
		 * гостя разойдётся (измерено 2026-10-15). */
		PUT(fd, "  vram=0x");
		put_hex(fd, guest_run_vram_base());
		PUT(fd, "\n");

		/* Нулевая страница + скретчпад, uncached-вид и кэшируемый.
		 *
		 * Прибор к правке по замеру 90: uncached-вид 0x40000000 не
		 * был отображён, и заливка региона шла 65 537 фолтами до
		 * убийства потока на границе скретчпада. Ожидаю
		 * null_mirror=0x40000000 и scr=0x10000; нули — адреса
		 * заняты, и зона живёт на фолт-эмуляции как раньше. */
		PUT(fd, "  null_mirror=0x");
		put_hex(fd, guest_run_null_mirror());
		PUT(fd, " scr=0x");
		put_hex(fd, guest_run_scr_base());
		PUT(fd, "\n");

		/* Куча VPL: где лежит и как работала за прогон.
		 *
		 * Прибор к подмене семейства `sceKernel*Vpl`: прогон 35
		 * без адреса блока падал в `HeapSystem`. base 0 — область
		 * не отображена (все кандидаты `map_vpl` заняты или tmpfs
		 * недоступна), и `MaxFreeMemSize` отвечала нулём.
		 * used — курсор бампа против vpl_bytes; fail — отказы
		 * выделения, их ненулевость и есть новый блокер. free —
		 * число `FreeVpl`: бамп без освобождения, заметное число
		 * означает, что арена истощается. */
		PUT(fd, "  vpl=0x");
		put_hex(fd, guest_run_vpl_base());
		PUT(fd, " vpl_mirror=0x");
		put_hex(fd, guest_run_vpl_mirror());
		PUT(fd, " vpl_bytes=");
		put_hex(fd, guest_run_vpl_bytes());
		PUT(fd, "\n  vpl_created=");
		put_hex(fd, guest_run_vpl_created());
		PUT(fd, " vpl_allocs=");
		put_hex(fd, guest_run_vpl_allocs());
		PUT(fd, " vpl_free=");
		put_hex(fd, guest_run_vpl_frees());
		PUT(fd, " vpl_fail=");
		put_hex(fd, guest_run_vpl_fails());
		PUT(fd, " vpl_used=");
		put_hex(fd, guest_run_vpl_used());
		PUT(fd, "\n");

		/* Шрифтовая библиотека: приборы к подмене sceLibFont.
		 *
		 * Прогон 36: нулевой хэндл `sceFontNewLib` валил гость в
		 * его шрифтовом коде (сбои по адресам 0x4/0xC — это
		 * maxFonts/alloc из `SceFontLibData`). new — сколько
		 * `NewLib`, open — сколько успешных `Open*`, glyph —
		 * отказы рендера глифов: рендера PGF нет, и заметное
		 * число означает, что текст идёт мимо. ef_created — число
		 * созданных флагов событий: без uid>0 игра не идёт дальше
		 * инициализации (прогон 38). ef_created — число созданных
		 * флагов событий (прогон 39), cb_created — число созданных
		 * обратных вызовов (прогон 39). */
		PUT(fd, "  font_new=");
		put_hex(fd, guest_run_font_libs());
		PUT(fd, " font_open=");
		put_hex(fd, guest_run_font_opens());
		PUT(fd, " font_glyph=");
		put_hex(fd, guest_run_font_glyphs());
		PUT(fd, " ef_created=");
		put_hex(fd, guest_run_ef_created());
		PUT(fd, " cb_created=");
		put_hex(fd, guest_run_cb_created());
		PUT(fd, " cb_fired=");
		put_hex(fd, guest_run_cb_fired());
		PUT(fd, " si_fired=");
		put_hex(fd, guest_run_si_fired());
		PUT(fd, " sema=");
		put_hex(fd, guest_run_sema_created());
		put_hex(fd, guest_run_sema_waited());
		PUT(fd, " sema_stuck=");
		put_hex(fd, guest_run_sema_stuck());
		PUT(fd, "\n");

		/* Внутренние глобали гостя (vaddr образа, расшифрованы
		 * дизассембром): power-флаги очереди задач ptn*.
		 * Прямая видимость вместо догадок по косвенным счётчикам. */
		PUT(fd, " g_suspend=0x");
		put_hex(fd, guest_run_state(0x327FF4u));
		PUT(fd, " g_initdone=0x");
		put_hex(fd, guest_run_state(0x327FF5u));
		/* Замер 94: флаг готовности звука (file-init 7; бэкенду
		 * аллокатора нужен бит 0) и условие VBlank-хендшейка. */
		PUT(fd, " g_sndflag=0x");
		put_hex(fd, guest_run_state(0x3667ACu));
		PUT(fd, " g_sndcond=0x");
		put_hex(fd, guest_run_state(0x3297D8u));
		PUT(fd, " g_qhead=0x");
		put_hex(fd, guest_run_state(0x325C08u));
		PUT(fd, " g_qtail=0x");
		put_hex(fd, guest_run_state(0x325C0Cu));
		PUT(fd, " g_qcnt=0x");
		put_hex(fd, guest_run_state(0x28EE4u));
		PUT(fd, " g_qsema=0x");
		put_hex(fd, guest_run_state(0x28EE8u));
		PUT(fd, "\n");

		/* Кадр гостя: последний `sceDisplaySetFrameBuf`.
		 * top/bw/fmt — как передал гость (top — сырой, зеркало
		 * видно по биту 0x40000000), count — сколько вызовов,
		 * dumps — сколько PPM-снимков записано. */
		PUT(fd, "  fb_top=0x");
		put_hex(fd, guest_run_fb_top());
		PUT(fd, " fb_bw=");
		put_hex(fd, guest_run_fb_bw());
		PUT(fd, " fb_fmt=");
		put_hex(fd, guest_run_fb_fmt());
		PUT(fd, " fb_count=");
		put_hex(fd, guest_run_fb_count());
		PUT(fd, " fb_dumps=");
		put_hex(fd, guest_run_fb_dumps());
		PUT(fd, "\n");

		/* Сколько `ins`/`ext` эмулировано при SIGILL.
		 *
		 * Прибор к эмуляции SPECIAL3 от 2026-10-14: 271 `ins` в
		 * образе, процессор их не выполняет. 0 — SIGILL не было
		 * или окно не нашло; больше нуля — эмуляция сработала. */
		PUT(fd, "  emul_ins=");
		put_hex(fd, guest_run_emul_ins());
		PUT(fd, "\n");

		/* Сколько 64-битных операций MIPS64 переписано в 32-битные.
		 *
		 * Прибор к правке от 2026-10-15: `dadd` в инициализации
		 * кучи поднял SIGILL. Ожидаю 189 (40 dadd + 86 daddu +
		 * 12 dmult + 51 dsrlv-$zero). Ноль — правка не применена. */
		PUT(fd, "  patch_64=");
		put_hex(fd, guest_run_patch_64());
		PUT(fd, "\n");

		/* Сколько `cache`-инструкций заменено на NOP при загрузке.
		 *
		 * Прибор к правке от 2026-10-14: SIGILL на `cache 0x1B`
		 * (vaddr 0x237070) после загрузки модулей. Ожидаю 1; 0 —
		 * правка не применилась, больше 1 — в образе что-то новое. */
		PUT(fd, "  cache_nops=");
		put_hex(fd, guest_run_cache_nops());
		PUT(fd, "\n");

		/* Снимки пути `sceKernelLoadModule` — по слоту, в порядке вызова.
		 *
		 * Прибор прогона на загрузочные аргументы: по коду гостя путь
		 * строится `strcat`-ом на нулевом буфере стека из глобали
		 * `0x327F00 -> "psmf.prx"`. Если в слоте область 2 и читается
		 * `"psmf.prx"` — цепочка `module_start -> StartThread -> разбор
		 * -> LoadModule` сошлась целиком. */
		{
			u32 cnt = guest_run_lm_count();
			u32 s, k;

			PUT(fd, "  lm_count=");
			put_hex(fd, cnt);
			PUT(fd, "\n  lm_next=");
			put_hex(fd, guest_run_lm_slot());
			for (s = 0; s < (u32)GUEST_LM_SNAP_SLOTS; s++) {
				PUT(fd, "\n  lm");
				put_idx(fd, s);
				PUT(fd, "_w=");
				put_hex(fd, guest_run_lm_where(s));
				for (k = 0; k < (u32)GUEST_LM_SNAP_WORDS; k++) {
					PUT(fd, "\n  lm");
					put_idx(fd, s);
					PUT(fd, "r");
					put_idx(fd, k);
					PUT(fd, "]=");
					put_hex(fd, guest_run_lm_word(s, k));
				}
			}
			PUT(fd, "\n");
		}

		/* Голова списка atexit после каждого конструктора `.cplinit`.
		 *
		 * Печатаются только переходы — где голова изменилась или стала
		 * нулём. 161 одинаковая строка читать бесполезно, а смена
		 * головы видна глазом и сразу указывает конструктор. */
		{
			u32 cn = guest_run_ctor_head_n();
			u32 prev = 0xFFFFFFFFu;

			PUT(fd, "  ctor_head_n=");
			put_hex(fd, cn);
			for (i = 0; i < cn && i < 256; i++) {
				u32 h = guest_run_ctor_head_at(i);

				if (h == prev)
					continue;
				prev = h;
				PUT(fd, "\n  ctor[");
				put_idx(fd, i);
				PUT(fd, "].head=0x");
				put_hex(fd, h);
			}
			PUT(fd, "\n");
		}

		/* Длина списка atexit после каждого конструктора.
		 *
		 * Счёт, а не смена головы. Если между двумя нашими конструкторами
		 * длина выросла больше чем на единицу, зарегистрировал кто-то ещё:
		 * чужую регистрацию подряд с нашей снимок головы не отличил бы,
		 * потому что голова после обеих одна и та же. */
		{
			u32 cn = guest_run_ctor_head_n();
			u32 prev = 0xFFFFFFFFu;

			PUT(fd, "  ctor_len_n=");
			put_hex(fd, cn);
			for (i = 0; i < cn && i < 256; i++) {
				u32 l = guest_run_ctor_len_at(i);

				if (l == prev)
					continue;
				prev = l;
				PUT(fd, "\n  ctor[");
				put_idx(fd, i);
				PUT(fd, "].len=0x");
				put_hex(fd, l);
			}
			PUT(fd, "\n");
		}
	}

	/* Что лежит по адресу, который гость зовёт инструкцией `jal 0`.
	 *
	 * В коде гостя по `0x08A9CA00` и `0x08A9CA18` стоит `jal 0`, цель
	 * которого — `0x08000000`: у `jal` берутся старшие 4 бита из `$pc`, а
	 * младшие 26 — из поля, то есть поле ноль даёт ровно `0x08000000`.
	 * Это вне нашего отображения гостя (`0x08804000..0x08B91840`), и
	 * релокации на эти адреса в `.rel.text` нет — проверено.
	 *
	 * Значит вопрос не «что там должно быть», а «отображено ли оно вообще».
	 * Ответ берётся из собственных карт процесса: если `0x08000000` не
	 * отображён, гость обязан был упасть на выборке команды по этому адресу,
	 * и `si_addr` был бы `0x08000000`, а не ноль.
	 *
	 * Разбор hex здесь свой, а не `strtoul`: библиотека собрана с
	 * `-nostdlib`, и обращение к libc создало бы неразрешённый символ,
	 * из-за чего `shell` отверг бы весь плагин при `dlopen`. */
	{
		int mfd = plat_open_read("/proc/self/maps");
		char line[160];
		int found = 0;

		PUT(fd, "\nmaps0800=");
		if (mfd >= 0) {
			for (;;) {
				long r = plat_syscall6(PROBE_SYS_read, mfd,
				                        (long)line,
				                        (long)sizeof(line) - 1,
				                        0, 0, 0);
				unsigned long a = 0, b = 0;
				int i = 0, j = 0, seen = 0;

				if (r <= 0)
					break;
				line[r] = '\0';
				while (line[i] != '\n' && line[i] != '\0' && i < (int)r)
					i++;
				line[i] = '\0';

				while (j < i && line[j] != '-') {
					a = (a << 4) | hex_digit(line[j]);
					j++;
				}
				if (j < i && line[j] == '-') {
					j++;
					seen = 1;
					while (j < i) {
						b = (b << 4) | hex_digit(line[j]);
						j++;
					}
				}
				if (seen && a <= 0x08000000u && 0x08000000u < b) {
					(void)plat_write(fd, line, (size_t)i);
					found = 1;
					break;
				}
				if (i >= (int)r)
					break;
			}
			plat_syscall6(PROBE_SYS_close, mfd, 0, 0, 0, 0, 0);
		}
		if (!found)
			PUT(fd, "НЕ ОТОБРАЖЁН");
	}
	PUT(fd, "log_overflow=");
	put_hex(fd, guest_run_overflow());
	PUT(fd, "finished=");
	put_hex(fd, (u32)guest_run_finished());
	PUT(fd, "limit_hit=");
	put_hex(fd, guest_run_limit_hit());
	PUT(fd, "fault=");
	put_hex(fd, (u32)guest_run_fault());
	if (guest_run_fault()) {
		PUT(fd, "  fault_sig=");
		put_hex(fd, guest_run_fault_sig());
		PUT(fd, "  fault_pc=");
		put_hex(fd, guest_run_fault_pc());
		PUT(fd, "  fault_addr=");
		put_hex(fd, guest_run_fault_addr());
	}
	PUT(fd, "\n");
	PUT(fd, "\n");

	/* Печатаются ВСЕ сохранённые записи кольца, не «первые 24».
	 *
	 * История этой настройки: «первые 24» прятали ХВОСТ (прогон
	 * 2026-10-15, 71 вызов, сбой базы кучи без контекста) — тогда
	 * хвост добавили отдельным блоком. Прогон 37 показал третий
	 * провал того же класса: 120 вызовов из 245 ушли в
	 * неподменённые NID, и ВСЕ они — в невидимой середине кольца.
	 * Ёмкость кольца LOG_MAX (256) больше числа вызовов типичного
	 * прогона, поэтому печатается всё хранимое; при переполнении
	 * записи за LOG_MAX не хранятся вовсе (см. `log_overflow`). */
	if (n > limit)
		n = limit;
	for (i = 0; i < n; i++) {
		PUT(fd, "  #");
		put_hex(fd, i + 1);
		PUT(fd, " nid=");
		put_hex(fd, guest_run_nid(i));
		PUT(fd, " stub=");
		put_hex(fd, guest_run_stub(i));
		PUT(fd, " a0=");
		put_hex(fd, guest_run_arg0(i));
		PUT(fd, " a1=");
		put_hex(fd, guest_run_arg1(i));
		PUT(fd, " a2=");
		put_hex(fd, guest_run_arg2(i));
		PUT(fd, " a3=");
		put_hex(fd, guest_run_arg3(i));
		/* Возврат гостю: без него семантику импорта приходится
		 * выдумывать, а по одному `$a0` она не восстанавливается. */
		PUT(fd, " v0=");
		put_hex(fd, guest_run_ret(i));
		/* Голова списка atexit на момент этого вызова: временная шкала,
		 * по которой видно, между какими вызовами голова сменилась. */
		PUT(fd, " head=0x");
		put_hex(fd, guest_run_call_head(i));
		PUT(fd, "\n");
	}

	/* Последние вызовы: перезаписываемое кольцо из 32.
	 * Показывает, что игра делала В КОНЦЕ прогона, а не в начале.
	 * Индексы идут по возрастанию (slot=0 — самое старое). */
	{
		static const char digits[] = "0123456789ABCDEF";
		char nb[96];
		u32 j;

		PUT(fd, "\n--- последние 32 вызова (кольцо) ---\n");
		for (j = 0; j < 32; j++) {
			u32 cnt_nid = guest_run_recent_nid(j);
			if (cnt_nid == 0)
				continue;
			{
				int ni = 0;
				int nk;

				nb[ni++] = ' '; nb[ni++] = ' ';
				nb[ni++] = 'l'; nb[ni++] = 'a';
				nb[ni++] = 's'; nb[ni++] = 't';
				nb[ni++] = (char)('0' + ((j / 10) % 10));
				nb[ni++] = (char)('0' + (j % 10));
				nb[ni++] = ' '; nb[ni++] = 'n';
				nb[ni++] = 'i'; nb[ni++] = 'd';
				nb[ni++] = '=';
				for (nk = 7; nk >= 0; nk--)
					nb[ni++] = digits[(cnt_nid >> (nk * 4)) & 0xF];
				nb[ni++] = ' '; nb[ni++] = 'r';
				nb[ni++] = 'a'; nb[ni++] = '=';
				{
					u32 ra_v = guest_run_recent_ra(j);
					for (nk = 7; nk >= 0; nk--)
						nb[ni++] = digits[(ra_v >> (nk * 4)) & 0xF];
				}
				nb[ni++] = ' '; nb[ni++] = 'a';
				nb[ni++] = '0'; nb[ni++] = '=';
				{
					u32 a0_v = guest_run_recent_a0(j);
					for (nk = 7; nk >= 0; nk--)
						nb[ni++] = digits[(a0_v >> (nk * 4)) & 0xF];
				}
				nb[ni++] = '\n';
				(void)plat_write(fd, nb, (size_t)ni);
			}
		}
	}

	/* Топ-NID по счётчику вызовов: какие импорсы доминируют
	 * в главном цикле после инициализации. Печатаем ВСЕ ненулевые,
	 * отсортировано не будет — просто raw counts.
	 * С каждой записью — последний ra и a0: ГДЕ вызван и С ЧЕМ. */
	{
		static const char digits[] = "0123456789ABCDEF";
		char nb[128];
		u32 total = guest_run_hle_count();
		u32 j;

		PUT(fd, "\n--- счётчик вызовов по индексу ---\n");
		for (j = 0; j < total && j < 512; j++) {
			u32 cnt = guest_run_nid_count(j);
			if (cnt == 0)
				continue;
			{
				int ni = 0;
				int nk;

				nb[ni++] = ' '; nb[ni++] = ' ';
				nb[ni++] = 'i'; nb[ni++] = 'd'; nb[ni++] = 'x';
				nb[ni++] = '=';
				for (nk = 3; nk >= 0; nk--)
					nb[ni++] = digits[(j >> (nk * 4)) & 0xF];
				nb[ni++] = ' '; nb[ni++] = 'c';
				nb[ni++] = 'n'; nb[ni++] = 't';
				nb[ni++] = '=';
				for (nk = 7; nk >= 0; nk--)
					nb[ni++] = digits[(cnt >> (nk * 4)) & 0xF];
				nb[ni++] = ' '; nb[ni++] = 'r'; nb[ni++] = 'a';
				nb[ni++] = '=';
				{
					u32 rav = guest_run_nid_ra(j);
					for (nk = 7; nk >= 0; nk--)
						nb[ni++] = digits[(rav >> (nk * 4)) & 0xF];
				}
				nb[ni++] = ' '; nb[ni++] = 'a'; nb[ni++] = '0';
				nb[ni++] = '=';
				{
					u32 a0v = guest_run_nid_a0(j);
					for (nk = 7; nk >= 0; nk--)
						nb[ni++] = digits[(a0v >> (nk * 4)) & 0xF];
				}
				nb[ni++] = '\n';
				(void)plat_write(fd, nb, (size_t)ni);
			}
		}
	}
}

static void load_guest_report(int fd)
{
	GuestModule m;
	GuestModuleInfo info;
	GuestImportTable t;
	unsigned char *image;
	long size;
	int gfd;
	int i;

	memset(&m, 0, sizeof(m));
	memset(&info, 0, sizeof(info));
	memset(&t, 0, sizeof(t));

	PUT(fd, "\n--- гость ---\n");

	gfd = plat_open_read(GUEST_IMAGE);
	if (gfd < 0) {
		PUT(fd, "open image FAILED: ");
		put(fd, GUEST_IMAGE, sizeof(GUEST_IMAGE) - 1);
		PUT(fd, "\n");
		return;
	}
	size = plat_file_size(gfd);
	PUT(fd, "image_size=");
	put_hex(fd, (u32)size);

	if (size <= 0 || (unsigned long)size > 0x01000000u) {
		PUT(fd, "image_size UNREASONABLE\n");
		plat_syscall(4006 /*close*/, gfd, 0, 0, 0);
		return;
	}

	image = plat_mmap_file(0, (size_t)size, PROT_READ, MAP_PRIVATE, gfd);
	plat_syscall(4006 /*close*/, gfd, 0, 0, 0);
	if (image == 0 || image == (unsigned char *)-1L) {
		PUT(fd, "image mmap FAILED\n");
		return;
	}
	PUT(fd, "image_magic=");
	put_hex(fd, ((u32 *)image)[0]);
	PUT(fd, "\n");

	/* Проверка свободы адреса идёт ПОСЛЕ того, как образ уже на месте:
	 * она ничего не меняет, а на отчёте видна сразу. */
	if (range_is_free(GUEST_BASE, GUEST_BASE + GUEST_SPAN)) {
		PUT(fd, "addr_free=yes\n");
		m.map_flags = MAP_FIXED;
	} else {
		PUT(fd, "addr_free=NO, без MAP_FIXED\n");
	}

	if (!guest_load(&m, image, (unsigned long)size, GUEST_BASE)) {
		PUT(fd, "guest_load FAILED: ");
		put(fd, m.error, strlen(m.error));
		PUT(fd, "\n");
		return;
	}

	PUT(fd, "guest_base=");
	put_hex(fd, m.base);
	PUT(fd, "span=");
	put_hex(fd, m.span);
	PUT(fd, "segments=");
	put_hex(fd, m.seg_count);
	for (i = 0; i < (int)m.seg_count; i++) {
		PUT(fd, "  seg_vaddr=");
		put_hex(fd, m.segments[i].vaddr);
		PUT(fd, " memsz=");
		put_hex(fd, m.segments[i].memsz);
		PUT(fd, " runtime=");
		put_hex(fd, m.segments[i].runtime);
		PUT(fd, "\n");
	}
	PUT(fd, "map_size=");
	put_hex(fd, (u32)m.map_size);
	PUT(fd, "tramp_base=");
	put_hex(fd, m.tramp_base);
	PUT(fd, "data_off=");
	put_hex(fd, m.data_off);
	PUT(fd, "data_protected=");
	put_hex(fd, (u32)m.data_protected);
	PUT(fd, "entry=");
	put_hex(fd, m.entry);
	PUT(fd, "\n");

	/* Релокации — главный предмет проверки. Тип секции у PSP PRX равен
	 * 0x700000A0, а не обычному SHT_REL (9); при значении 9 загрузчик
	 * находит ноль релокаций, не падает и не ругается — все внутренние
	 * указатели остаются неверными, и это обнаружится только при первом
	 * же вызове гостевого кода. */
	PUT(fd, "reloc_total=");
	put_hex(fd, m.reloc_total);
	PUT(fd, "applied=");
	put_hex(fd, m.reloc_applied);
	PUT(fd, "unsupported=");
	put_hex(fd, m.reloc_unsupported);
	PUT(fd, "failed=");
	put_hex(fd, m.reloc_failed);
	PUT(fd, "bad_segment=");
	put_hex(fd, m.reloc_bad_segment);
	PUT(fd, "\n");

	if (!guest_read_module_info(&m, &info)) {
		PUT(fd, "module_info FAILED\n");
		return;
	}
	PUT(fd, "info_name=");
	put(fd, info.name, strlen(info.name));
	PUT(fd, "\ngp=");
	put_hex(fd, info.gp);
	PUT(fd, "ent_top=");
	put_hex(fd, info.ent_top);
	PUT(fd, "ent_end=");
	put_hex(fd, info.ent_end);
	PUT(fd, "stub_top=");
	put_hex(fd, info.stub_top);
	PUT(fd, "stub_end=");
	put_hex(fd, info.stub_end);
	PUT(fd, "\n");

	if (!guest_scan_imports(&m, &info, &t)) {
		PUT(fd, "scan_imports FAILED\n");
		return;
	}
	PUT(fd, "libs=");
	put_hex(fd, t.count);
	PUT(fd, "imports=");
	put_hex(fd, t.total_imports);
	PUT(fd, "\n");

	/* Запуск гостя в отдельном потоке.
	 *
	 * Поток, а не поток интерфейса: гость после старта уходит в цикл и на
	 * потоке интерфейса заморозил бы картинку. Ждём ограниченное время —
	 * не для красоты, а чтобы отчёт снимался после работы гостя, а не сразу
	 * после запуска. */
	{
		int rc;

		rc = guest_run_start(&m, &t, fd);
		PUT(fd, "run_start=");
		put_hex(fd, (u32)rc);
		PUT(fd, "\n");
		if (rc != 0) {
			if (rc == -4)
				PUT(fd, "выполнение гостя выключено: нет защиты от сбоя\n");
			else
				PUT(fd, "запуск не состоялся\n");
			return;
		}
		guest_run_wait(guest_run_target(), 600000);  /* до полного прогона */
		report_imports(fd);
	}
}

/* Точка входа, которую зовёт хост.
 *
 * Шаг сознательно ограничен: гость загружается, но его код НЕ запускается.
 * Проверяем, что загрузчик работает внутри `shell` на живом образе, прежде
 * чем разрешить исполняться чужому машинному коду в процессе интерфейса.
 *
 * Раньше здесь стояла отдельная проба `mmap` с проверкой каждой страницы.
 * Она своё дело сделала: адрес `0x08804000` отдаётся и все 320 страниц
 * отвечают, это измерено и на обычном процессе, и здесь. Теперь она бы
 * СТОЛКНУЛАСЬ с загрузчиком — оба хотят один и тот же адрес, и второй
 * отказал бы с «адрес занят». Замер перенесён внутрь загрузчика.
 *
 * Пишем всё в файл-метку: на экране наш код ничего не рисует, поэтому
 * «нет реакции на кнопку» — это ожидаемое поведение, а не отказ.
 */
/* Видимость по умолчанию обязательна и не выбирается по вкусу: весь файл
 * собирается с `-fvisibility=hidden`, чтобы `malloc`, `mmap` и прочие не
 * попали в глобальную область и не перекрыли символы glibc для самого
 * `shell`. Но под этим же флагом скрывается и точка входа, которую хост
 * ищет по имени, — а загрузчик молча отверг бы библиотеку. */
__attribute__((visibility("default")))
void dune_run_native_code(void *task)
{
	int fd;
	u32 *w;
	long pid;

	w = (u32 *)task;
	pid = plat_getpid();

	fd = plat_open_write(MARKER);
	if (fd < 0)
		return;

	PUT(fd, "psp-plugin: RUNS\n");
	PUT(fd, "pid=");      put_hex(fd, (u32)pid);
	PUT(fd, "task=");
	if (task == 0) {
		PUT(fd, "NULL\n");
	} else {
		PUT(fd, "+0="); put_hex(fd, w[0]);
		PUT(fd, "+4="); put_hex(fd, w[1]);
		PUT(fd, "+8="); put_hex(fd, w[2]);
		PUT(fd, "+32="); put_hex(fd, w[8]);
		PUT(fd, "\n");
	}

	load_guest_report(fd);

	/* --- ПЕРВЫЙ ВЫВОД КАДРА НА ЭКРАН --------------------------------
	 *
	 * Порядок обязан быть таким: сперва полный отчёт (данные уже на
	 * диске при любом исходе), затем `enable_directfb_mode` из
	 * таблицы колбэков (тот же вызов, что у рабочего образца
	 * `native_plugin.c`, шаг 1-2), затем показ кадра через
	 * RUA/GFX-конвейер `shell`.
	 *
	 * Таблица колбэков — это и есть `task`: разбор в
	 * `Dune Max/docs/hle-injection.md`, поле +32 — указатель на
	 * `stb_enable_directfb_mode`. В наших прогонах там был
	 * `0x00754F00` — код внутри `shell`.
	 *
	 * Ни одного обращения к устройствам: RUA-токены — не физические
	 * адреса, команда уходит через буфер DRAM. */
	{
		void (**cb)(void) = (void (**)(void))w;

		(void)gfx_out_init();

		/* task == 0 обрабатывается выше (печать NULL), здесь
		 * тогда просто нечего вызывать: таблицы колбэков нет. */
		if (task != 0 && cb[8] != 0) {
			PUT(fd, "dfb_cb=0x");
			put_hex(fd, (u32)(uintptr_t)cb[8]);
			PUT(fd, "\n");
			cb[8]();          /* stb_enable_directfb_mode */
			PUT(fd, "dfb_on\n");
		} else {
			PUT(fd, "dfb_skip\n");
		}
		{
			u32 r = guest_run_show_last_frame();

			PUT(fd, "gfx_show=0x");
			put_hex(fd, r);
			PUT(fd, " step=0x");
			put_hex(fd, gfx_out_step);
			PUT(fd, " shown=0x");
			put_hex(fd, gfx_out_shown);
			PUT(fd, " rc_lock=0x");
			put_hex(fd, gfx_out_rc_lock);
			PUT(fd, " rc_getlock=0x");
			put_hex(fd, gfx_out_rc_getlock);
			PUT(fd, " rc_unlock=0x");
			put_hex(fd, gfx_out_rc_unlock);
			PUT(fd, "\n");
		}
	}

	plat_syscall(4006 /*close*/, fd, 0, 0, 0);
}
