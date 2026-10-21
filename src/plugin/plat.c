/*
 * Слой системных вызовов для нативного плагина.
 *
 * Зачем он нужен, измерено, а не предположено.
 *
 * Первая попытка была не писать его вовсе: `.so` загружается внутрь `shell`,
 * а это процесс glibc 2.8, и ссылки на `malloc`/`mmap`/`snprintf` должны
 * разрешиться в уже загруженную libc. В обычном процессе плеера так и
 * вышло — все пять вызовов отработали. Но внутри `shell` процесс упал.
 *
 * Причина видна в самой библиотеке: при линковке с `-nostdlib` компоновщик
 * создаёт секцию `.MIPS.stubs` и GOT, но **не создаёт ни `.rela.dyn`, ни
 * `.rela.plt`** — то есть заполнить GOT нечем. Проверено: `readelf -r`
 * отвечает «There are no relocations in this file» при том, что пять
 * символов помечены UND. Как это ужилось в обычном процессе — не
 * выяснено, и догадываться мы не будем.
 *
 * Практический вывод: полагаться на то, как `shell` разбирает наши
 * неразрешённые ссылки, нельзя. Слой ниже делает библиотеку
 * самодостаточной — `NEEDED` пуст, неразрешённых символов нет, и поведение
 * загрузчика перестаёт быть нашим условием.
 *
 * ПОДПИСИ. Здесь подключены те же заголовки, что и у загрузчика гостя, и
 * реализации объявлены с их же прототипами. Это не формальность: `size_t` на
 * этой цели — `unsigned int`, а не `unsigned long`, и первая версия слоя
 * брала `unsigned long`, из-за чего компилятор ругался на несовпадение со
 * встроенными объявлениями `memcpy`/`strlen`. Объявить со своих слов значило
 * получить две несовместимые сущности с одним именем.
 *
 * Область видимости. Файл собирается с `-fvisibility=hidden`, поэтому эти
 * определения не попадают в `.dynsym`. Без этого наши `malloc`, `free` и
 * `memcpy` оказались бы в глобальной области и могли перекрыть одноимённые
 * символы glibc для самого `shell` и его 79 библиотек. Линковщик разрешает
 * наши ссылки локально, динамический — ничего не трогает.
 *
 * Чего здесь нет и почему. Аллокатор ступенчатый, без `free`: загрузчик
 * гостя выделяет таблицы только при загрузке и освобождает их при выгрузке,
 * а повторных циклов загрузки в одном процессе не бывает.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "plat.h"

/* --- системные вызовы MIPS o32 -----------------------------------------
 *
 * Номера сверены на живом плеере программой, собранной этим же тулчейном:
 * read 4003, write 4004, open 4005, close 4006, lseek 4019, mmap 4090,
 * munmap 4091, mprotect 4125, getpid 4020, cacheflush 4147.
 *
 * Их нельзя брать из заголовков хоста: значения `O_*` и `MAP_ANONYMOUS` на
 * MIPS отличаются от x86. Ошибка здесь даёт не «файл не создан», а честный
 * ENOENT, уводящий в сторону.
 */

#define SYS_read       4003
#define SYS_write      4004
#define SYS_open       4005
#define SYS_close      4006
#define SYS_lseek      4019
#define SYS_mmap       4090
#define SYS_munmap     4091
#define SYS_mprotect   4125
#define SYS_getpid     4020
#define SYS_cacheflush 4147          /* __NR_Linux + 147 */
#define SYS_exit       4001          /* завершает ПОТОК, не процесс */
#define SYS_nanosleep  4166          /* __NR_Linux + 166 */
#define SYS_rt_sigaction 4194        /* __NR_Linux + 194 */

#define BCACHE         3              /* ICACHE|DCACHE */

/* Сырой вызов: номер в `$v0`, аргументы в `$a0..$a3`, признак ошибки ядро
 * возвращает в `$a3`. Без проверки `$a3` отрицательный код неотличим от
 * успеха.
 *
 * `$a3` — оператор ЧТЕНИЕ-ЗАПИСЬ (`"+r"`), а не только выход: в него
 * кладутся и четвёртый аргумент, и признак ошибки. С `"=r"` компилятор
 * считает регистр нечитаемым на входе. И `$13` — это тот же `$a3`,
 * поэтому его нельзя перечислять в списке порченных регистров: он и так
 * операнд. Обе ошибки стоили рестарта `shell`. */
long plat_syscall(long n, long a0, long a1, long a2, long a3)
{
	register long r4 __asm__("$4") = a0;
	register long r5 __asm__("$5") = a1;
	register long r6 __asm__("$6") = a2;
	register long r7 __asm__("$7") = a3;
	register long r2 __asm__("$2");

	__asm__ volatile (
		"addu $2, $0, %2\n\t"
		"syscall\n\t"
		: "=&r"(r2), "+r"(r7)
		: "ir"(n), "0"(r2), "r"(r4), "r"(r5), "r"(r6)
		: "memory", "$1", "$3", "$8", "$9", "$10", "$11", "$12",
		  "$14", "$15", "$24", "$25", "hi", "lo");

	return (r7 && r2 > 0) ? -r2 : r2;
}

/* Пять и шесть аргументов идут на стеке, в `16($sp)` и `20($sp)`. Это не
 * догадка, а снято дизассемблированием musl:
 *
 *     addiu $sp, $sp, -32
 *     sw    $t0, 16($sp)
 *     sw    $t1, 20($sp)
 *     li    $v0, 4210
 *     syscall
 *
 * Первая версия клала их в `$t0`/`$t1`, ядро читало мусор и отвечало
 * EINVAL, а мы записали это как «адрес занят» — то есть из отказа вывели
 * неверную причину. */
long plat_syscall6(long n, long a0, long a1, long a2, long a3,
                   long a4, long a5)
{
	register long r4 __asm__("$4") = a0;
	register long r5 __asm__("$5") = a1;
	register long r6 __asm__("$6") = a2;
	register long r7 __asm__("$7") = a3;
	register long r2 __asm__("$2");

	__asm__ volatile (
		"addiu $sp, $sp, -32\n\t"
		"sw %7, 16($sp)\n\t"
		"sw %8, 20($sp)\n\t"
		"addu $2, $0, %2\n\t"
		"syscall\n\t"
		"addiu $sp, $sp, 32\n\t"
		: "=&r"(r2), "+r"(r7)
		: "ir"(n), "0"(r2), "r"(r4), "r"(r5), "r"(r6), "r"(a4), "r"(a5)
		: "memory", "$1", "$3", "$8", "$9", "$10", "$11", "$12",
		  "$14", "$15", "$24", "$25", "hi", "lo");

	return (r7 && r2 > 0) ? -r2 : r2;
}

/* --- функции с именами, которые ожидает загрузчик гостя ---------------- */

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	return (void *)plat_syscall6(SYS_mmap, (long)addr, (long)len, prot,
	                             flags, fd, (long)off);
}

int munmap(void *addr, size_t len)
{
	return plat_syscall(SYS_munmap, (long)addr, (long)len, 0, 0) < 0 ? -1 : 0;
}

int mprotect(void *addr, size_t len, int prot)
{
	return plat_syscall(SYS_mprotect, (long)addr, (long)len, prot, 0) < 0
	       ? -1 : 0;
}

/* `open`/`close`/`ftruncate`/`unlink` нужны загрузчику гостя: зеркало
 * без кэша делается через ОБЩИЙ файл в tmpfs под двумя `MAP_SHARED`
 * отображениями, а `guest_load.c` собирается и в плагин (без libc),
 * и в инструменты с настоящей libc — та же схема, что у `mmap` выше.
 *
 * Имена и подписи — из заголовков musl, чтобы обе сборки видели одно
 * и то же. `open` вариадический: третий аргумент (режим файла) читается
 * только при `O_CREAT`, как в libc.
 *
 * Значения `O_*` на MIPS не как на x86, поэтому в ВЫЗЫВАЮЩЕМ коде флаги
 * собираются числами, а не по именам (см. `guest_load.c`), и здесь тоже:
 * проверять надо значения тулчейна, а не привычные с x86. Номера
 * `ftruncate`=4093 и `unlink`=4010 взяты из заголовка тулчейна
 * (`bits/syscall.h`), а не по памяти. */
int open(const char *path, int flags, ...)
{
	int mode = 0;

	if (flags & 0x0100 /* O_CREAT на MIPS */) {
		__builtin_va_list ap;

		__builtin_va_start(ap, flags);
		mode = __builtin_va_arg(ap, int);   /* mode_t promote до int */
		__builtin_va_end(ap);
	}
	return (int)plat_syscall(SYS_open, (long)path, flags, (long)mode, 0);
}

int close(int fd)
{
	return plat_syscall(SYS_close, fd, 0, 0, 0) < 0 ? -1 : 0;
}

int ftruncate(int fd, off_t len)
{
	/* 4093 = __NR_ftruncate (тулчейн, MIPS o32). Файлы образа ~5 МиБ,
	 * 64-битный вариант (4212) не нужен. */
	return plat_syscall(4093, fd, (long)len, 0, 0) < 0 ? -1 : 0;
}

int unlink(const char *path)
{
	/* 4010 = __NR_unlink (тулчейн, MIPS o32). */
	return plat_syscall(4010, (long)path, 0, 0, 0) < 0 ? -1 : 0;
}

/* Размер страницы определяется один раз проверкой `mprotect`: значение,
 * которым ядро довольно, и есть размер страницы. На этом плеере 16384
 * (константа `_SC_PAGESIZE` = 30 на MIPS), и задавать его руками нельзя —
 * от него зависят и обязательный `-z max-page-size=0x4000`, и снятие права
 * исполнения с данных гостя. */
static size_t detect_page_size(void)
{
	static const size_t candidates[2] = { 16384, 4096 };
	size_t i;

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		void *p = mmap(0, candidates[i] * 2, PROT_READ | PROT_WRITE,
		               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p == MAP_FAILED)
			continue;
		if (mprotect(p, candidates[i], PROT_READ) == 0) {
			munmap(p, candidates[i] * 2);
			return candidates[i];
		}
		munmap(p, candidates[i] * 2);
	}
	return 4096;
}

long sysconf(int name)
{
	static size_t cached;

	if (name == 30 /* _SC_PAGESIZE на MIPS */) {
		if (cached == 0)
			cached = detect_page_size();
		return (long)cached;
	}
	return plat_syscall(4020, 0, 0, 0, 0) == -22 ? -1 : 0;
}

/* --- вспомогательное для плагина --------------------------------------- */

int plat_getpid(void)
{
	return (int)plat_syscall(SYS_getpid, 0, 0, 0, 0);
}

/* Номер потока (tid) текущего.
 *
 * Нужен, чтобы отличить сбой в главном потоке гостя от сбья в созданном им
 * игровом: регистры у них одинаковые, а ведут себя они по-разному.
 * В обработчике сигнала больше ничего вызвать нельзя, поэтому это сырой
 * `syscall`.
 *
 * Номер ВЗЯТ ИЗ ЗАГОЛОВКА ТУЛЧЕЙНА, а не по памяти: `asm/unistd.h`, ветка
 * `_MIPS_SIM_ABI32` (строки 18..404), `__NR_gettid = __NR_Linux + 222`.
 * Взят по памяти `+266` — такого номера на плеере нет, и вместо tid печатался
 * остаток регистра (`0xFFFFFFEA`). Проверено пробой на устройстве: `SYS 4222`
 * возвращает pid, кандидаты `4266` и `4242` — мусор. */
#define SYS_gettid 4222
int plat_gettid(void)
{
	return (int)plat_syscall(SYS_gettid, 0, 0, 0, 0);
}

int plat_write(int fd, const void *buf, size_t len)
{
	return (int)plat_syscall(SYS_write, fd, (long)buf, (long)len, 0);
}

int plat_read(int fd, void *buf, size_t len)
{
	return (int)plat_syscall(SYS_read, fd, (long)buf, (long)len, 0);
}

int plat_open_read(const char *path)
{
	return (int)plat_syscall6(SYS_open, (long)path, 0, 0, 0, 0, 0);
}

int plat_open_write(const char *path)
{
	/* O_WRONLY|O_CREAT|O_TRUNC = 0x0301 на MIPS: значения `O_*` здесь не
	 * как на x86, где O_CREAT = 0x40. */
	return (int)plat_syscall6(SYS_open, (long)path, 0x0301, 0644, 0, 0, 0);
}

long plat_file_size(int fd)
{
	return plat_syscall(SYS_lseek, fd, 0, 2 /* SEEK_END */, 0);
}

int plat_thread_alive(int tid)
{
	char path[48];
	const char *pre = "/proc/self/task/";
	unsigned long v = (unsigned long)tid;
	int i = 0, fd, k;

	/* Путь собираем вручную: тянуть `snprintf` ради десяти цифр незачем, а
	 * слой форматирования тянет за собой состояние. */
	while (pre[i] != 0) {
		path[i] = pre[i];
		i++;
	}
	/* Старшие цифры идут первыми, поэтому счётчик разрядов равен старшей
	 * ненулевой позиции; для нуля разряд один. */
	{
		unsigned long d = 1;

		while (v / d >= 10)
			d *= 10;
		for (k = 0; k < 12; k++) {
			path[i++] = (char)('0' + (int)((v / d) % 10));
			if (d == 1)
				break;
			d /= 10;
		}
	}
	path[i] = 0;

	/* Каталог потока существует ровно пока поток жив; `open` каталога на
	 * этом ядре достаточно, `stat` не нужен. */
	fd = plat_open_read(path);
	if (fd < 0)
		return 0;
	(void)plat_syscall(SYS_close, fd, 0, 0, 0);
	return 1;
}

void plat_flush_icache(void *addr, size_t len)
{
	plat_syscall(SYS_cacheflush, (long)addr, (long)len, BCACHE, 0);
}

void *plat_mmap_file(void *addr, size_t len, int prot, int flags, int fd)
{
	return mmap(addr, len, prot, flags, fd, 0);
}

/* --- потоки ------------------------------------------------------------
 *
 * Соглашение `clone` на MIPS o32 снято дизассемблированием musl, а не взято
 * из опыта другой платформы. Три вещи здесь неочевидны:
 *
 *   - ФЛАГИ идут в `$a0`, а не в `$a2`, как на x86;
 *   - `fn` и `arg` ядру не передаются ВОВСЕ — ребёнок продолжает
 *     исполняться внутри `clone` и достаёт их со своего стека;
 *   - `$sp` ребёнка подставляет ядро и равен `(stack & -8) - 16`.
 *
 * Отсюда же: лишний `addiu $sp,$sp,-16` в коде ребёнка уводил чтение `fn` на
 * 16 байт ниже, и тело потока не выполнялось ни разу — проба объявляла
 * нерабочим работающий механизм.
 *
 * Сырой `exit` (4001) завершает ТОЛЬКО поток, `exit_group` (4246) — весь
 * процесс. Проверено на плеере. */
/* `fn` и `arg` нового потока.
 *
 * Кладутся в глобалы, а не только на стек потока, потому что ветка ребёнка
 * исполняется ПОСЛЕ того, как компилятор выставил свой кадр: `$sp` уже сдвинут
 * на размер кадра, и чтение `0($sp)` берёт не `fn`, а мусор. Прыжок по такому
 * адресу и давал SIGSEGV.
 *
 * Глобалы разделяются между потоками (`CLONE_VM`), поэтому значение видно
 * ребёнку. Ограничение: потоки создаются последовательно, не одновременно.
 */
/* `fn` и `arg` нового потока.
 *
 * Всё делается ассемблером, и это не спор вкуса. Два предыдущих варианта
 * ломались одинаково незаметно:
 *
 *   - чтение `fn` из `0($sp)` прямо в ассемблерной ветке ребёнка работало в
 *     отдельной пробе и падало здесь: компилятор вставлял пролог кадра и
 *     сдвигал `$sp`, и `fn` читался не оттуда;
 *   - хранение `fn` в глобале с вызовом из C давало `jalr` по адресу 0 —
 *     замерено на плеере: сбой ровно на `jalr t9`, гость не выполнил ни
 *     одной инструкции.
 *
 * Причина одна: обе схемы опираются на то, что НЕ переживает переход в новый
 * поток. Надёжно только стек, который подставляет ядро, и он доступен как
 * `$29` без смещений, если у функции нет кадра. Поэтому функция написана
 * целиком на ассемблере: кадра нет, и в ветке ребёнка `$29` равен ровно
 * тому, что подставило ядро.
 *
 * Соглашение взято у musl, см. выше. Ветка ребёнка не возвращается. */
long plat_clone(void (*fn)(void *), void *arg, unsigned long stack_top,
                unsigned long flags);

__asm__(
	"    .set noreorder\n"
	"    .set noat\n"
	"    .text\n"
	"    .globl plat_clone\n"
	"    .type plat_clone, @function\n"
	"plat_clone:\n"
	"    li      $1, -8\n"
	"    and     $5, $5, $1\n"      /* стек выравнивается по 8 */
	"    addiu   $5, $5, -16\n"     /* место под fn и arg */
	"    sw      $4, 0($5)\n"
	"    sw      $6, 4($5)\n"
	"    move    $4, $7\n"          /* ФЛАГИ идут в $a0, не в $a2 */
	"    li      $2, 4120\n"
	"    syscall\n"
	"    bnez    $2, 1f\n"
	"    nop\n"
	/* Мы новый поток: `$29` подставлен ядром и указывает ровно на fn. */
	"    lw      $25, 0($29)\n"
	"    lw      $4, 4($29)\n"
	"    jalr    $25\n"
	"    nop\n"
	"    li      $4, 0\n"
	"    li      $2, 4001\n"        /* завершает только поток */
	"    syscall\n"
	"1:  jr      $31\n"
	"    nop\n"
	"    .set at\n"
	"    .set reorder\n"
	"    .size plat_clone, .-plat_clone\n");

/* Завершает только текущий поток.
 *
 * Сырой `exit`, а не libc: libc-овский прогнал бы обработчики выхода и
 * сбросил буферы `stdio` всего процесса, а процесс — это `shell`. */
void plat_thread_exit(void)
{
	plat_syscall(SYS_exit, 0, 0, 0, 0);
	for (;;)
		;
}

/* --- сигналы ------------------------------------------------------------
 *
 * Всё ниже измерено перебором на живом плеере (`tmp/plugin/sigprobe.c`),
 * а не выведено по аналогии с другой архитектурой. Это существенно: обе
 * ошибки выглядят безобидно, и одна из них — как полная бездейтельность.
 *
 * Обработчик из сбоя не выходит, он завершает поток, поэтому `restorer`
 * ядру нужен непустым, но не исполняется. Код оставлен осмысленным, чтобы
 * при случайном возврате поведение было хоть каким-то предсказуемым. */
__asm__(
	"    .set noreorder\n"
	"    .text\n"
	"    .globl plat_signal_restorer\n"
	"plat_signal_restorer:\n"
	"    or    $2, $4, $zero\n"   /* movv: номер вызова в $a0 */
	"    li    $2, 4193\n"         /* __NR_rt_sigreturn на MIPS o32 */
	"    syscall\n"
	"    .set reorder\n");

extern void plat_signal_restorer(void);

/* `struct sigaction` в том виде, в каком её ждёт MIPS o32.
 *
 * Порядок полей: `sa_flags`, обработчик, `sa_restorer`, маска. Маска — 16
 * байт из-за `_NSIG` = 128. */
struct plat_ksigaction {
	unsigned long flags;
	void *handler;
	void *restorer;
	unsigned long mask[4];
};

int plat_signal_raw(int sig, void *handler, unsigned long flags)
{
	struct plat_ksigaction act;

	memset(&act, 0, sizeof(act));
	act.flags = flags;
	act.handler = handler;
	act.restorer = plat_signal_restorer;
	return plat_syscall(SYS_rt_sigaction, sig, (long)&act, 0,
	                   (long)sizeof(act.mask));
}

int plat_signal_reset(int sig)
{
	return plat_signal_raw(sig, (void *)1 /* SIG_DFL */, 0);
}

/* Пауза в миллисекундах.
 *
 * Сырым вызовом, а не `nanosleep` из libc: тот разворачивается в
 * `__nanosleep_time64`, то есть в ещё один неразрешённый символ — гейт
 * самодостаточности его ловит. Значение `tv_nsec` обязано быть меньше
 * секунды: ядро отвергает `>= 10^9`. */
void plat_sleep_ms(unsigned long ms)
{
	struct timespec ts;

	ts.tv_sec = (long)(ms / 1000u);
	ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
	plat_syscall(SYS_nanosleep, (long)&ts, 0, 0, 0);
}

/* --- память ------------------------------------------------------------ */

void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	size_t i;

	for (i = 0; i < n; i++)
		d[i] = (unsigned char)c;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	size_t i;

	for (i = 0; i < n; i++)
		d[i] = s[i];
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	size_t i;

	if (d == s || n == 0)
		return dst;
	if (d < s) {
		for (i = 0; i < n; i++)
			d[i] = s[i];
	} else {
		for (i = n; i > 0; i--)
			d[i - 1] = s[i - 1];
	}
	return dst;
}

size_t strlen(const char *s)
{
	size_t n = 0;

	while (s[n] != 0)
		n++;
	return n;
}

int strncmp(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i])
			return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
		if (a[i] == 0)
			return 0;
	}
	return 0;
}

/* `memcmp` обязателен по той же причине, что и остальное.
 *
 * С `-fno-builtin` компилятор перестаёт разворачивать `memcmp` в сравнение
 * и emits обычный вызов. `guest_load.c` проверяет им магию ELF, так что
 * символ нужен. Без нашей реализации он остаётся неразрешённым и
 * разрешается в glibc процесса — то есть библиотека снова перестаёт быть
 * самодостаточной, а это ровно то свойство, ради которого слой написан.
 * Проверено: до добавления `readelf -s` показывал `U memcmp`. */


char *strncpy(char *dst, const char *src, size_t n)
{
	size_t i;

	for (i = 0; i < n && src[i] != 0; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = 0;
	return dst;
}

/* `memcmp` обязателен по той же причине, что и остальное.
 *
 * С `-fno-builtin` компилятор перестаёт разворачивать `memcmp` в сравнение
 * и emits обычный вызов. `guest_load.c` проверяет им магию ELF, так что
 * символ нужен. Без нашей реализации он остаётся неразрешённым и
 * разрешается в glibc процесса — то есть библиотека снова перестаёт быть
 * самодостаточной, а это ровно то свойство, ради которого слой написан.
 * Проверено: до добавления `nm -u` показывал `U memcmp`, и гейт
 * `make check-plugin` это ловит. */
int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;
	size_t i;

	for (i = 0; i < n; i++) {
		if (x[i] != y[i])
			return (int)x[i] - (int)y[i];
	}
	return 0;
}

#define ARENA_BYTES (4u * 1024u * 1024u)

static unsigned char *arena;
static size_t arena_used;

static void *bump(size_t n)
{
	unsigned char *p;

	if (arena == 0) {
		arena = mmap(0, ARENA_BYTES, PROT_READ | PROT_WRITE,
		             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		arena_used = 0;
	}
	if (arena == MAP_FAILED) {
		arena = 0;
		return 0;
	}
	n = (n + 7u) & ~(size_t)7u;
	if (arena_used + n > ARENA_BYTES)
		return 0;
	p = arena + arena_used;
	arena_used += n;
	return p;
}

void *malloc(size_t n)
{
	return bump(n);
}

void *calloc(size_t count, size_t size)
{
	size_t total = count * size;
	void *p;

	if (count != 0 && total / count != size)
		return 0;                    /* переполнение: отказ, а не мусор */
	p = bump(total);
	if (p != 0)
		memset(p, 0, total);
	return p;
}

void free(void *p)
{
	(void)p;                         /* ступенчатый аллокатор, см. выше */
}

char *strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = bump(n);

	if (p != 0)
		memcpy(p, s, n);
	return p;
}

static int g_errno;

int *__errno_location(void)
{
	return &g_errno;
}

/* --- форматирование ----------------------------------------------------
 *
 * `va_arg` обязан совпадать с типом, который передал вызывающий. На o32
 * `int` в переменных аргументах остаётся `int`, поэтому `%u` читается как
 * `unsigned int`, а не `unsigned long`: чтение 8 байт из 4-байтового слота
 * даёт мусор, который выглядит как «форматирование сломалось».
 */

typedef struct {
	char *buf;
	size_t cap;
	size_t len;
} Sink;

static void sink_put(Sink *s, char c)
{
	if (s->len + 1 < s->cap)
		s->buf[s->len] = c;
	s->len++;
}

static void sink_str(Sink *s, const char *v)
{
	if (v == 0)
		v = "(null)";
	while (*v != 0)
		sink_put(s, *v++);
}

static void sink_num(Sink *s, unsigned long v, unsigned base, int upper,
                     unsigned long zeros)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[16];
	unsigned n = 0;

	if (v == 0)
		tmp[n++] = '0';
	while (v != 0 && n < sizeof(tmp)) {
		tmp[n++] = digits[v % base];
		v /= base;
	}
	while (n < zeros) {
		sink_put(s, '0');
		n++;
	}
	while (n > 0)
		sink_put(s, tmp[--n]);
}

int plat_vformat(char *buf, size_t cap, const char *fmt,
                 __builtin_va_list ap)
{
	Sink s;
	const char *f = fmt;

	s.buf = buf;
	s.cap = cap;
	s.len = 0;

	while (*f != 0) {
		unsigned long width = 0;
		unsigned long zeros;
		int zero_pad = 0;
		int lng = 0;

		if (*f != '%') {
			sink_put(&s, *f++);
			continue;
		}
		f++;
		/* Флаг `0` и ширина разбираются РАЗДЕЛЬНО. Первая версия считала
		 * их одним числом, и `%08X` давал ширину 18 вместо 8 — то есть
		 * «0x» печатался с восемнадцатью нулями. Именно это и было видно в
		 * сообщении загрузчика: «адрес 0x00000000000…». */
		if (*f == '0') {
			zero_pad = 1;
			f++;
		}
		while (*f >= '0' && *f <= '9') {
			width = width * 10 + (unsigned long)(*f - '0');
			f++;
		}
		while (*f == 'l') {
			lng = 1;
			f++;
		}
		zeros = zero_pad ? width : 0;

		switch (*f) {
		case 's':
			sink_str(&s, __builtin_va_arg(ap, const char *));
			break;
		case 'c':
			sink_put(&s, (char)__builtin_va_arg(ap, int));
			break;
		case 'd':
		case 'i': {
			long v = lng ? __builtin_va_arg(ap, long)
			             : (long)__builtin_va_arg(ap, int);

			if (v < 0) {
				sink_put(&s, '-');
				sink_num(&s, (unsigned long)(-v), 10, 0, zeros);
			} else {
				sink_num(&s, (unsigned long)v, 10, 0, zeros);
			}
			break;
		}
		case 'u':
			sink_num(&s, lng ? __builtin_va_arg(ap, unsigned long)
			                 : (unsigned long)__builtin_va_arg(ap,
			                                               unsigned int),
			         10, 0, zeros);
			break;
		case 'x':
			sink_num(&s, lng ? __builtin_va_arg(ap, unsigned long)
			                 : (unsigned long)__builtin_va_arg(ap,
			                                               unsigned int),
			         16, 0, zeros);
			break;
		case 'X':
			sink_num(&s, lng ? __builtin_va_arg(ap, unsigned long)
			                 : (unsigned long)__builtin_va_arg(ap,
			                                               unsigned int),
			         16, 1, zeros);
			break;
		case 'p': {
			/* `va_arg` читает ОДИН раз: повторный вызов на том же
			 * аргументе — неопределённое поведение, и второй вернул бы
			 * следующий за ним байт строки формата. */
			void *ptr = __builtin_va_arg(ap, void *);

			sink_put(&s, '0');
			sink_put(&s, 'x');
			sink_num(&s, (unsigned long)ptr, 16, 0, 0);
			break;
		}
		case '%':
			sink_put(&s, '%');
			break;
		default:
			sink_put(&s, '%');
			sink_put(&s, *f);
			break;
		}
		if (*f != 0)
			f++;
	}

	if (cap != 0)
		buf[s.len < cap ? s.len : cap - 1] = 0;
	return (int)(s.len < cap ? s.len : (cap != 0 ? cap - 1 : 0));
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = plat_vformat(buf, cap, fmt, ap);
	__builtin_va_end(ap);
	return n;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, __builtin_va_list ap)
{
	return plat_vformat(buf, cap, fmt, ap);
}

char *strerror(int e)
{
	/* Номера на MIPS другие, чем на x86: 18 — это ENXIO, а не EINVAL.
	 * В `em8xxx.ko` заглушки неподдерживаемых подсистем возвращают именно
	 * 18, и это сбивало с толку при разборе отказов. */
	static char unknown[] = "E?";

	switch (e) {
	case 1:  return (char *)"EPERM";
	case 2:  return (char *)"ENOENT";
	case 9:  return (char *)"EBADF";
	case 12: return (char *)"ENOMEM";
	case 14: return (char *)"EFAULT";
	case 18: return (char *)"ENXIO";
	case 19: return (char *)"ENODEV";
	case 22: return (char *)"EINVAL";
	default: return unknown;
	}
}

/* --- символы, нужные компоновщику --------------------------------------
 *
 * `_flush_cache` — musl-внутреннее имя для сброса кэша команд; `mmap` её
 * вызывает, когда процессор MIPS читает из кэша commands написанный код.
 *
 * `_gp_disp` — смещение от `_gp` до самой переменной; требуется PIC-коду,
 * собранному с `abicalls`. Значение musl использует 0x7ff0.
 */

long _flush_cache(void *addr, int nbytes, int cache);

long _flush_cache(void *addr, int nbytes, int cache)
{
	(void)nbytes;
	(void)cache;
	plat_flush_icache(addr, (size_t)nbytes);
	return 0;
}

long _gp_disp = 0x7ff0;