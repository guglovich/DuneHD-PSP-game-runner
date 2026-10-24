/*
 * Запуск гостя внутри `shell`: поток, трамплины, обработчик импортов.
 *
 * Интерфейс намеренно узкий: печать отчёта остаётся в `dune_plugin.c`.
 * Здесь всё, что исполняется после старта потока, и всё, что обязано
 * пережить выгрузку библиотеки.
 *
 * Почему поток. На потоке интерфейса гость ушёл бы в цикл игры и заморозил
 * картинку: обработчик импортов возвращает ноль почти на всё, а гость после
 * старта крутится в ожидании привода — наблюдалось 2,6 млн вызовов
 * `sceUmdGetDriveStat` за 15 секунд.
 *
 * Почему выход гостя — сырой `exit`. Возврат в C невозможен: `$gp`, `$t9`,
 * `$s0..$s7` и `$sp` гость затирает, и чтение после возврата шло бы по
 * адресам, пришедшим из гостя. Сырой `exit` (4001) завершает только поток —
 * проверено на плеере, что это не `exit_group` (4246), так что процесс
 * интерфейса не страдает.
 */

#include <string.h>

#include "plat.h"
#include "guest_run.h"
#include "guest_load.h"
#include "ge_walk.h"
#include "gfx_out.h"

/* Стек потока гостя. Верхушка на всякий случай: кадры гостя глубокие, а
 * переполнение выглядит как SIGSEGV без внятной причины. */
#define GUEST_STACK_BYTES (512u * 1024u)

/* Сколько вызовов импортов запоминаем. */
#define LOG_MAX 256

/* Потоков гостя, которых мы умеем запустить.
 *
 * Слот один не по недосмотру: `sceKernelCreateThread` гость вызывает один раз
 * и с одним адресом входа, а `StartThread` — следом. Второй слот нужен лишь
 * для проверки, что запуск действительно создаёт поток, а не помечает флаг. */
#define GUEST_THREAD_SLOTS 8

/* Стек одного потока гостя.
 *
 * Отдельный от стек�� основного потока: поток гостя живёт дольше него и уходит
 * в игровой цикл, а освободить общий стек нельзя — он занят самим собой. */
#define GUEST_WORKER_STACK_BYTES (256u * 1024u)

/* Потоков импортов — ровно столько, сколько влезает в пул трамплинов.
 *
 * Предел обязан совпадать с ёмкостью пула, а не подбираться: каждый трамплин
 * занимает GUEST_TRAMP_BYTES, и при нехватке места `install_trampolines`
 * возвращает 0, а `guest_run_start` — -2 («не построены трамплины»).
 *
 * Прежнее значение 256 было молчаливым отказом: у LocoRoco 2 импортов 269, так
 * что установка обрывалась на 257-м, и причина была неочевидна — гейт
 * самодостаточности проходил, отчёт обрывался строкой `run_start=0xFFFFFFFE`.
 * Проверять надо фактическое число импортов, а не константу.
 *
 * 380 * 172 = 65 360 ≤ 65 536, то есть пул ровно вмещает столько трамплинов. */
#define HLE_MAX_IMPORTS ((GUEST_TRAMP_SIZE - POOL_TAIL_BYTES) / GUEST_TRAMP_BYTES)

/* ВЫПОЛНЯТЬ ЛИ ГОСТЯ.
 *
 * 0 — запрещено, и это не лень, а отсутствие защиты. `SIGSEGV` в потоке гостя
 * убивает весь процесс вместе с интерфейсом: на стенде замерено, что поток
 * не успевает выполнить ни первую запись, ни первый вызов импорта. Защита
 * нужна обработчик сигнала, а `rt_sigaction` на MIPS требует `sa_restorer`,
 * которого мы не подставляли.
 *
 * Пока защиты нет, запуск гостя выключен: разворачивать в устройство сборку,
 * которая гарантированно роняет `shell`, нельзя. Всё остальное — загрузка,
 * проверка адреса, поток — работает и проверено. */
#define GUEST_EXEC 1

/* Сколько вызовов импортов гость выполняет до принудительной остановки.
 *
 * Предел обязателен по причине, которая стоила рестарта интерфейса. Хост
 * выгружает нашу библиотеку сразу после возврата из `dune_run_native_code`
 * (`close lib` в `shell.log`). Если поток гостя в этот момент ещё жив, он
 * продолжает исполнять код по адресу, который уже не отображён, — и
 * `SIGSEGV` в нём убивает ВЕСЬ процесс вместе с интерфейсом.
 *
 * Останавливать нужно не ожидание, а сам гость: по исчерпании предела поток
 * завершается сам, сырым `exit`, и к возврату из плагина уже мёртв. */
#define CALL_LIMIT 50000000u

/* Блок сведений о потоке, который гость запрашивает через
 * `GetModuleIdByAddress`. Лежит в пуле трамплинов, то есть внутри отображения
 * гостя: гость по нему читает и пишет, и хостский адрес для него бессмыслен. */
#define THREAD_INFO_BYTES 16u

/* Блок `SceThreadContext`, который кладёт в `$k0` ядро при старте потока.
 *
 * Раскладка взята не из догадок, а из pspsdk
 * (`src/kernel/pspthreadman_kernel.h`, `struct SceThreadContext`):
 *
 *   +0x000  type
 *   +0x004  gpr[31]   — вот эти 4 байта гость и пишет: `sw s5,4($k0)`
 *   +0x080  fpr[32]
 *   +0x100  fc31, hi, lo, SR, EPC, +0x114, +0x118
 *   итого  0x11C = 284 байта
 *
 * Адрес обязан лежать ВНУТРИ образа гостя: гость по нему пишет, и хостский
 * адрес для него бессмыслен (как и для `THREAD_INFO_BYTES` выше).
 *
 * Прежде сюда передавался ноль, и игровой поток падал на `sw` по адресу из
 * `$k0`. Все прежние выводы о `$k0` сняты неверным способом: подстановки
 * значений делались на сборке, которая не доходила до этого кода —
 * `install_trampolines` обрывался на 257-м импорте из 269. */
#define THREAD_CTX_BYTES 0x11Cu

/* Размер блока контекста у ВЕРШИНЫ стека — 256 байт, см. `thread_ctx_addr()`.
 *
 * Одно и то же число задаёт и начальный `$sp` нового потока: по коду PPSSPP
 * (`FillStack`) стек начинается РОВНО под блоком, `SP = stack_top - 256`.
 * Пока `$sp` был на 16 байт ниже вершины, кадр `worker_thread` целиком лёг
 * внутрь блока, и обнуление контекста затирало слоты `gp`, `ra`, `s0..s4`.
 * Число вынесено, чтобы блок и `$sp` считались от общей константы и снова
 * не разошлись. */
#define THREAD_CTX_SPAN 256u

/* Хвост пула трамплинов: под контекст потока и блок `GetModuleIdByAddress`.
 *
 * Резервируется явно, иначе трамплины дойдут до этих адресов. */
#define POOL_TAIL_BYTES (THREAD_CTX_BYTES + THREAD_INFO_BYTES)

static struct {
	u32 calls;
	u32 limit_hit;
	u32 unknown;
	u32 log_overflow;
	u32 nid[LOG_MAX];
	u32 stub[LOG_MAX];
	u32 arg0[LOG_MAX];
	u32 arg1[LOG_MAX];
	u32 arg2[LOG_MAX];
	u32 arg3[LOG_MAX];
	/* Что обработчик вернул гостю (`$v0`).
	 *
	 * Снимается обёрткой `guest_import`, а не в теле: тело имеет несколько
	 * `return`, и дописывать запись в каждый — способ забыть один. */
	u32 ret[LOG_MAX];
	/* Слот, в который записан последний вызов; 0xFFFFFFFF — журнал полон. */
	volatile u32 last_slot;
	/* Голова списка atexit на момент вызова.
	 *
	 * Снимок после каждого конструктора показал: за `.cplinit` регистраций
	 * 23, а к моменту abort'а голова указывает на ПЕРВЫЙ зарегистрированный
	 * узел, который по коду гостя поставить в голову нечем. Это сужает
	 * момент, когда голова сменилась. Пишется в тот же журнал вызовов, то
	 * есть отдельного потока и ловушек не требуется. */
	u32 head[LOG_MAX];
} g_run;

/* Счётчик вызовов по индексу импорта — для поиска самых частых NID
 * в главном цикле. Игра делает миллионы вызовов, и без этого счётчика
 * невозможно узнать, какие импорсы доминируют после инициализации. */
#define NID_COUNT_SLOTS 512
static u32 g_nid_count[NID_COUNT_SLOTS];
/* Последний ra/a0 вызова по индексу: счётчик говорит СКОЛЬКО,
 * а ra/a0 — ГДЕ и С ЧЕМ вызывался. По ним в отчёте видны
 * места парковки потоков (SleepThreadCB главного), аргументы
 * редких вызовов (LoadModule: какой модуль), семафорные ID. */
static u32 g_nid_ra[NID_COUNT_SLOTS];
static u32 g_nid_a0[NID_COUNT_SLOTS];

/* Последние вызовы: отдельное кольцо, перезаписываемое.
 * Показывает, что игра делает СЕЙЧАС, а не в начале. */
static u32 g_recent_nid[32];
static u32 g_recent_ra[32];
static u32 g_recent_a0[32];
static u32 g_recent_slot;
static u32 g_recent_count;

static GuestModule *g_mod;
static u32 g_hle_nid[HLE_MAX_IMPORTS];
static u32 g_hle_count;

/* Время для GetSystemTimeWide: растёт на 1000 мкс за вызов. */
static u32 g_time_us;
/* Кнопки контроллера: 0 = ничего, потом START (0x8000). */
static u32 g_ctrl_buttons;

/* Метка «трамплин отработал», которую трамплин записывает сам перед
 * возвратом в гостя.
 *
 * Именно метка, а не счётчик: по ней отличают падение ВНУТРИ трамплина от
 * падения в коде гостя — регистры там почти одинаковы, и больше различить
 * нечем. Счётчик вызовов живёт отдельно, в `g_run.calls`; раньше метка и
 * счётчик были одной переменной, и трамплин затирал счётчик индексом. */
static volatile u32 g_tramp_mark;

/* Отметки о прыжке в гостя, пишутся ассемблером `guest_thread_enter`.
 *
 * Метки РАЗДЕЛЬНЫЕ — по одной на поток, и это не украшение. Поначалю была одна
 * переменная на оба потока, и она дала ложный ответ: её успевал установить
 * ГЛАВНЫЙ поток (он действительно входит в гостя и делает четыре вызова
 * импортов), а сбой при этом происходил в РАБОЧЕМ. Метка стояла, и это
 * выглядело как «гость введён» — то есть тест не отвечал на свой вопрос.
 * Значение `0x47554554` означает «прыжок состоялся».
 *
 * Отдельные глобальные переменные, а не поля в стеке потока: гость в первых
 * же инструкциях выделяет кадр на 1008 байт и затирает весь стек до вершины,
 * а блок контекста по смещению `+0xf8` перезаписывается собственной заливкой
 * `0xffffffff`. Обе попытки поставить метку на стек дали чужое значение. */
static volatile u32 g_mark_main;
static volatile u32 g_mark_worker;
static u32 g_host_gp;
static u32 g_thread_info;

/* Аргумент `sceKernelCpuSuspendIntr` — адрес блока памяти гостя.
 *
 * Гость передаёт его один раз, а пользуется им потом из структуры, которую
 * запрашивает через `GetModuleIdByAddress`. Сохраняем, потому что между этими
 * двумя вызовами аргумент уже не в регистре. */
static u32 g_suspend_arg;

/* Значения, которые читает отчёт. `volatile`: их пишет поток гостя, а читает
 * поток интерфейса, и без этого компилятор может закэшировать в регистре. */
volatile int  g_guest_pid;


/* Дескриптор отчёта для раннего журнала.
 *
 * Отчёт пишется после возврата из ожидания, но падение случается ДО него —
 * и тогда не остаётся ничего. Поэтому первые вызовы пишутся прямо из
 * обработчика: видно, доходят ли вызовы вообще и на каком именно обрывается. */
static int g_log_fd = -1;
/* 1000, не 32: 32 записи кончались на GeEdramGetAddr — самом
 * начале init, а ГЛАВНАЯ цель лога теперь — ДОГНАТЬ точку, где
 * init гостя останавливается на пути к init-done (0x327FF5),
 * который не ставится за 10+ минут (замер прогона 68). 1000
 * записей × 4 строки покрывают весь init с запасом. */
#define EARLY_LOG_MAX 1000

/* Запись без libc: `snprintf` из слоя тянул бы за собой состояние, а здесь
 * нужен только сырой `write`. */
static void early_log(const char *tag, u32 v)
{
	char buf[40];
	static const char digits[] = "0123456789ABCDEF";
	int i = 0, k, t;

	buf[i++] = ' ';
	buf[i++] = ' ';
	for (t = 0; tag[t] != 0 && i < 30; t++)
		buf[i++] = tag[t];
	buf[i++] = '=';
	for (k = 7; k >= 0; k--)
		buf[i++] = digits[(v >> (k * 4)) & 0xF];
	buf[i++] = '\n';
	(void)plat_write(g_log_fd, buf, (size_t)i);
}
volatile int  g_guest_calls;
/* «Окно ожидания плагина закрылось». Ставится в 1 в
 * guest_run_wait ПЕРЕД возвратом хосту: все парковки
 * SleepThreadCB видят флаг, завершают цикл доставки
 * колбеков и возвращаются — потоки честно умирают в
 * ExitDeleteThread, dlclose не застаёт живой хост-код
 * в нашей библиотеке (грабля сборки bc5641ea). */
volatile u32 g_time_to_exit;

/* --- СЕМПЛИРОВАНИЕ PC ГОСТЕВЫХ ПОТОКОВ ---------------------------------
 *
 * Прогон 70: главный поток после Font-инициализации растворился в
 * БЕЗИМПОРТНОМ коде — ни один счётчик его не видит. Чтобы найти точку
 * зависания, CB-цикл каждые ~2 с шлёт гостевым потокам SIGALRM;
 * обработчик печатает tid и ВСЕХ кандидатов PC из ucontext (какой из
 * них верный — решаем по попаданию в образ, как в fault-handler).
 *
 * Гости от сигнала не страдают: обработчик ничего не меняет в
 * контексте и возвращается — прерванный поток продолжает. */
#define SYS_tgkill_mips 4266
static u32 g_samp_n;   /* лимит напечатанных семплов */

static int guest_thread_fault(u32 tid);   /* определён ниже */
static void early_log(const char *tag, u32 v);

/* Замер 100: дренажный клапан — счётчик семплов внутри цикла дренажа
 * (заполняется обработчиком ниже, читается паркером SleepThreadCB).
 *
 * Замер 103: SOFTWARE GRAIN COMPLETER. По PPSSPP (Core/HW/SasAudio.h)
 * SAS — не фиксированное железо, а ПРОГРАММНЫЙ микшер на Media Engine:
 * игра ставит грейн в очередь, ME микшует и помечает буфер готовым,
 * аксессуар 0x08AB7228 потребляет пометку. ME у нас нет — семплер
 * снимает $s0 цикла дренажа (сам объект), а насос тика каждые ~46 мс
 * (грейн 2048/44100) помечает буфер готовым: [obj+0x27C]=1. Это шаг
 * «научиться вызовам» перед оффлоадом на DSP плеера. */
volatile u32  g_drain_spin;
static u32    g_drain_valve;
volatile u32  g_drain_obj;     /* $s0 цикла дренажа, снятый семплером */
static u32    g_drain_marks;
static volatile u32 g_sound_sema;  /* uid звукового семафора (замер 108) */
static u32    g_sound_signals;
static u32    g_drain_dumped;  /* 1 = дамп объекта уже сделан */

static void guest_samp_handler(int sig, void *info, void *uc)
{
	static const unsigned short off[] = { 0x10, 0x20, 0x28, 0x40, 0xF0 };
	const unsigned char *p;
	u32 tid = (u32)plat_gettid();
	int k;

	(void)sig;
	(void)info;
	/* Только гостевые потоки, только пока есть лимит:
	 * чужие (shell) не печатаем, чтобы не искать иголку. */
	if (g_log_fd < 0 || g_samp_n >= 300 || !guest_thread_fault(tid))
		return;
	g_samp_n++;
	early_log("SAMP", tid);
	for (k = 0; k < 5; k++) {
		u32 v;

		p = (const unsigned char *)uc + off[k];
		v = (u32)p[0] | ((u32)p[1] << 8)
		    | ((u32)p[2] << 16) | ((u32)p[3] << 24);
		early_log("SP", v);
		/* Замер 100: детектор спина дренажа — цикл
		 * 0x08AB43D0..E8 крутится, пока счётчик [obj+0x278]
		 * не опустошится, а опустошить его некому (sparse-
		 * таблица голосов пуста). Паркер по этому счётчику
		 * открывает дренажный клапан. */
		if (v >= 0x08AB43C8u && v <= 0x08AB43ECu) {
			g_drain_spin++;
			/* $s0 = gregs[16] — объект дренажа (цикл держит
			 * его в $s0 всё время: move s0,a0 в прологе). */
			g_drain_obj = *(const u32 *)((const unsigned char *)uc
			                          + 40 + 8 * 16);
		}
	}
}
volatile int  g_guest_finished;
volatile int  g_iow_count;

/* Снимок списка atexit гостя.
 *
 * Объявлены здесь, а рядом с функцией снимка, потому что к ним обращается
 * обработчик `IoWrite`, который идёт раньше по файлу.
 *
 * `g_atexit_snap_n == 0xFFFFFFFF` означает «снимок ещё не снимался» и
 * потому не путается с нулём — а ноль законно означает пустой список. */
#define ATEXIT_SNAP_MAX 64

/* Голова списка atexit после каждого конструктора `.cplinit`.
 *
 * Регистрации выполняются внутри конструкторов, а цикл — наш, поэтому это
 * самый дешёвый способ увидеть их порядок: без ловушки и без правки
 * гостевого кода. Размер ограничен, чтобы буфер не рос с числом
 * конструкторов. */
#define ATEXIT_CTOR_MAX 256
static volatile u32 g_ctor_head[ATEXIT_CTOR_MAX];
static volatile u32 g_ctor_len[ATEXIT_CTOR_MAX];
static volatile u32 g_ctor_head_n;

/* Значение головы списка как `vaddr` узла; 0 — пусто либо недоступно. */
static u32 atexit_head_node(void)
{
	u32 head, cur;

	head = guest_run_atexit_head();
	if (head == 0 || head + 4u > g_mod->map_size)
		return 0;
	cur = ((const u32 *)((const unsigned char *)g_mod->memory + head))[0];
	if (cur < g_mod->base)
		return 0;
	return cur - g_mod->base;
}

/* Длина списка atexit — длина обхода от головы.
 *
 * Прибор к атрибуции, а не к поиску. Снимок головы различает только
 * ИЗМЕНЕНИЕ головы; чужая регистрация между нашими конструкторами голову
 * меняет, но длину списка увеличивает на ЕДИНИЦУ сверх нашей. Длина
 * показывает счёт, а голова — нет.
 *
 * Предел обязателен: список может оказаться кольцом (замеренный обход
 * возвращается к голове через 42 узла), и без предела цикл не кончится.
 * Обход идёт по `vaddr`, а поле `next` лежит рантайм-адресом. */
#define ATEXIT_LEN_MAX 256

static u32 atexit_list_len(void)
{
	const unsigned char *mem;
	u32 head, cur, n;

	if (g_mod == 0 || g_mod->memory == 0)
		return 0;
	head = guest_run_atexit_head();
	if (head == 0 || head + 4u > g_mod->map_size)
		return 0;
	mem = (const unsigned char *)g_mod->memory;
	cur = ((const u32 *)(mem + head))[0];
	if (cur < g_mod->base)
		return 0;
	cur -= g_mod->base;

	for (n = 0; n < ATEXIT_LEN_MAX && cur != 0 && cur + 4u <= g_mod->map_size; n++) {
		u32 next = ((const u32 *)(mem + cur))[0];

		if (next < g_mod->base)
			break;
		next -= g_mod->base;
		if (next + 4u > g_mod->map_size || next == cur)
			break;
		cur = next;
	}
	return n;
}
static volatile u32 g_atexit_snap[ATEXIT_SNAP_MAX];
static volatile u32 g_atexit_snap_next[ATEXIT_SNAP_MAX];
static volatile u32 g_atexit_snap_n = 0xFFFFFFFFu;
static volatile u32 g_atexit_snap_head;

/* Сведения о сбое потока гостя, заполняются обработчиком. */
static volatile u32 g_guest_fault_sig;
static volatile u32 g_guest_fault_pc;
static volatile u32 g_guest_fault_addr;

/* Сколько инструкций `ins`/`ext` эмулировано обработчиком SIGILL.
 * Прибор к эмуляции SPECIAL3: 0 при отсутствии SIGILL, ≥1 — если
 * окно вокруг pc содержало эти инструкции. */
static u32 g_emul_ins;

/* Счётчики восстановления после сбоя.
 *
 * Предел обязателен: гость, зациклившийся на одном неотображённом адресе,
 * без него крутился бы вечно, а хост выгружает нашу библиотеку сразу после
 * возврата из плагина — выгружать её под работающим кодом нельзя. */
#define FAULT_RECOVER_MAX 128
#define FAULT_REPORT_MAX  8
/* Предел ПОДРЯД ИДУЩИХ сбоев на одном и том же `pc`.
 *
 * Зачем отдельный предел, когда есть общий: `strlen` по нулевому указателю
 * читает нулевую страницу ПОБАЙТОВО, и это один и тот же `pc` на 126 сбоях
 * подряд. Общий предел 128 при этом выбирается целиком, и до следующего
 * конструктора `.cplinit` прогон не доходит НИ РАЗУ — вся диагностика
 * уходила в одну счётную итерацию.
 *
 * Разница принципиальная: общий предел отвечает на «сколько сбоев всего», этот —
 * «где именно заклинюсь». Второй вопрос иначе не задаётся. */
#define FAULT_SPIN_MAX    8
static volatile u32 g_fault_total;    /* всего сбоев */
static volatile u32 g_fault_skipped;  /* из них пропущено инструкций */
static volatile u32 g_fault_spin;     /* сбоев подряд на этом pc */
static volatile u32 g_fault_spin_pc;  /* pc, на котором они идут */

/* --- обработчик импортов ---------------------------------------------- */

/* Какие NID мы подменяем.
 *
 * Ответы осмысленны ровно настолько, чтобы гость дошёл до игрового цикла:
 * привода на плеере нет, и «готов» необходим, иначе бесконечное ожидание.
 * Полноценной подмены системных вызовов PSP здесь нет — это наблюдение, а не
 * эмуляция, и об этом сказано в отчёте прямо. */
static int is_implemented(u32 nid)
{
	switch (nid) {
	case 0x8EF08FCEu:   /* sceUmdWaitDriveStat */
	case 0x6B4A146Cu:   /* sceUmdGetDriveStat */
	case 0x3F53E640u:   /* sceKernelSignalSema */
	case 0x4E3A1105u:   /* sceKernelWaitSema */
	case 0xD6DA4BA1u:   /* sceKernelCreateSema */
	case 0x28B6489Cu:   /* sceKernelDeleteSema */
	case 0x58B1F937u:   /* sceKernelPollSema */
	case 0xBC6FEBC5u:   /* sceKernelReferSemaStatus */
	case 0x446D8DE6u:   /* sceKernelCreateThread */
	case 0xF475845Du:   /* sceKernelStartThread */
	case 0x092968F4u:   /* sceKernelCpuSuspendIntr */
	case 0x5F10D406u:   /* sceKernelCpuResumeIntr */
	case 0x984C27E7u:   /* sceDisplayWaitVblankStart */
	case 0x36CDFADEu:   /* sceDisplayWaitVblank */
	case 0xE47E40E4u:   /* sceGeEdramGetAddr — VRAM */
	case 0x1F6752ADu:   /* sceGeEdramGetSize */
	case 0x0E20F177u:   /* sceDisplaySetMode */
	case 0x289D82FEu:   /* sceDisplaySetFrameBuf — кадр */
	case 0xEEDA2E54u:   /* sceDisplayGetFrameBuf */
	case 0x9C6EAAD7u:   /* sceDisplayGetVcount */
	case 0x03444EB4u:   /* sceGeListSync */
	case 0xB287BD61u:   /* sceGeDrawSync */
	case 0x4C06E472u:   /* sceGeContinue */
	case 0xB448EC0Du:   /* sceGeBreak */
	case 0xE0D68148u:   /* sceGeListUpdateStallAddr */
	case 0xA4FC06A4u:   /* sceGeSetCallback */
	case 0x05DB22CEu:   /* sceGeUnsetCallback */
	case 0xCEADEB47u:   /* sceKernelDelayThread */
	case 0x7591C7DBu:   /* sceKernelSetCompiledSdkVersion */
	case 0xF77D77CBu:   /* sceKernelSetCompilerVersion — имена уточнены по
	                     * jpcsp (HLE/SysMemUserForUser.java): psplibdoc Sony
	                     * группирует 0xF77D77CB и 0xEBD5C3E6 по библиотеке
	                     * SysMemUserForUser, но по смыслу это kernel-функции.
	                     * Прежняя подпись «SysMemUserForUser» была перепутана.
	                     * Обе возвращают 0, то есть обработчик по умолчанию
	                     * уже давал верный ответ — и они не были причиной
	                     * падения. */
	case 0xEBD5C3E6u:   /* sceKernelSetCompiledSdkVersion */
	case 0xEDBA5844u:   /* sceKernelDeleteCallback — возврат 0 = успех */
	case 0x237DBD4Fu:   /* sceKernelAllocPartitionMemory — арена `part_alloc` */
	case 0x9D9A5BA1u:   /* sceKernelGetBlockHeadAddr — `part_head` */
	case 0x37FB5C42u:   /* sceKernelGetGPI — 0: HOLD не нажат */
	case 0x52089CA1u:   /* sceKernelGetThreadStackFreeSize — живой остаток */
	case 0x82BC5777u:   /* sceKernelGetSystemTimeWide — возвращает растущее время */
	case 0x3A622550u:   /* sceCtrlPeekBufferPositive — нет кнопок */
	case 0x1F803938u:   /* sceCtrlReadBufferPositive — нет кнопок */
	case 0x0B588501u:   /* sceCtrlReadLatch — нет кнопок */
	case 0xA291F107u:   /* sceKernelMaxFreeMemSize — размер области VPL */
	case 0x56C039B5u:   /* sceKernelCreateVpl */
	case 0x89B3D48Cu:   /* sceKernelDeleteVpl */
	case 0x39810265u:   /* sceKernelReferVplStatus — freeSize строит кучи */
	case 0xAF36D708u:   /* sceKernelTryAllocateVpl */
	case 0xB736E9FFu:   /* sceKernelFreeVpl */
	case 0xBED27435u:   /* sceKernelAllocateVpl */
	case 0xEC0A693Fu:   /* sceKernelAllocateVplCB — пишет базу кучи */
	case 0x1D371B8Au:   /* sceKernelCancelVpl */
	case 0x67F17ED7u:   /* sceFontNewLib — хэндл с копией params */
	case 0x27F6E642u:   /* sceFontGetNumFontList */
	case 0xBC75D85Bu:   /* sceFontGetFontList */
	case 0xA834319Du:   /* sceFontOpen */
	case 0x57FCB733u:   /* sceFontOpenUserFile — отказ, PGF нет */
	case 0xBB8E7FE6u:   /* sceFontOpenUserMemory — отказ */
	case 0x0DA7535Eu:   /* sceFontGetFontInfo */
	case 0xDCC80C2Fu:   /* sceFontGetCharInfo */
	case 0x980F4895u:   /* sceFontGetCharGlyphImage — рендера нет */
	case 0xCA1E6945u:   /* sceFontGetCharGlyphImage_Clip — рендера нет */
	case 0x02D7F94Bu:   /* sceFontFlush */
	case 0x3AEA8CB6u:   /* sceFontClose */
	case 0x574B6FBCu:   /* sceFontDoneLib */
	case 0x293B45B8u:   /* sceKernelGetThreadId — uid текущего потока */
	case 0x55C20A00u:   /* sceKernelCreateEventFlag — возвращает uid > 0 */
	case 0xEF9E4C70u:   /* sceKernelDeleteEventFlag — возврат 0 = успех */
	case 0x9FA03CD3u:   /* sceKernelDeleteThread — возврат 0 = успех */
	case 0x278C0DF5u:   /* sceKernelWaitThreadEnd — возврат 0 = конец */
	case 0x1F4011E6u:   /* sceCtrlSetSamplingMode — возврат 0 = успех */
	case 0x6A2774F3u:   /* sceCtrlSetSamplingCycle — возврат 0 = успех */
	case 0x36AA6E91u:   /* sceImposeSetLanguageMode — возврат 0 = успех */
	case 0xCA04A2B9u:   /* sceKernelRegisterSubIntrHandler — без прерываний */
	case 0xFB8E22ECu:   /* sceKernelEnableSubIntr — без прерываний */
	case 0x39AF39A6u:   /* sceNetInit — сеть не нужна, «успех» */
	case 0x3F7AD767u:   /* sceRtcGetCurrentTick — растущие мкс */
	case 0xA5DA2406u:   /* sceUtilityGetSystemParamInt — пишет *a1 */
	case 0x5EC81C55u:   /* sceAudioChReserve — номер канала 0..7 */
	case 0x6FC46853u:   /* sceAudioChRelease — возврат 0 = успех */
	case 0x42778A9Fu:   /* __sceSasInit — пишет g_sas[] */
	case 0xE175EF66u:   /* __sceSasGetOutputmode — режим из g_sas[] */
	case 0xD5A229C9u:   /* __sceSasRevEVOL — возврат 0 = успех */
	case 0xF983B186u:   /* __sceSasRevVON — возврат 0 = успех */
	case 0xE81CAF8Fu:   /* sceKernelCreateCallback — возвращает uid > 0 */
	case 0x71BC9871u:   /* sceKernelChangeThreadPriority — возврат 0 = успех */
	case 0xEA748E31u:   /* sceKernelChangeCurrentThreadAttr — возврат 0 = успех */
	case 0x50F0C1ECu:   /* sceKernelStartModule — возврат 0 = успех */
	case 0x82826F70u:   /* sceKernelSleepThreadCB — возврат 0 = успех */
	case 0x9ACE131Eu:   /* sceKernelSleepThread — парковка 10 мс */
	case 0x809CE29Bu:   /* sceKernelExitDeleteThread — возврат 0 = успех */
	case 0x04B7766Eu:   /* scePowerRegisterCallback — возврат 0 = успех */
	case 0xAEE7404Du:   /* sceUmdRegisterUMDCallBack — возврат 0 = успех */
	case 0x4AC57943u:   /* sceKernelRegisterExitCallback — возврат 0 = успех */
	case 0xAB49E76Au:   /* sceGeListEnQueue */
	case 0x1C0D95A6u:   /* sceGeListEnQueueHead */
	case 0xA6BAB2E9u:   /* sceKernelStdout — дескриптор stdout, возврат 1 */
	case 0x42EC03ACu:   /* sceIoWrite — снимок буфера гостя в отчёт */
	case 0x54F5FB11u:   /* sceIoDevctl — регистрация MS-notify колбека */
		return 1;
	default:
		return 0;
	}
}

/* --- потоки гостя --------------------------------------------------------
 *
 * Настоящего планировщика нет, но и без него гость идёт дальше: он создаёт
 * поток, запускает его и уходит в игровой цикл уже внутри него. Раньше
 * `StartThread` не делал ничего, поэтому гость доводил до конца только
 * четыре вызова и возвращался из точки входа — то есть игровой код не
 * начинался ни разу.
 */

/* Стек одного запущенного потока гостя. Своё на каждый слот: поток живёт
 * дольше создавшего его, и общий стек занят самим собой. */
static unsigned char g_worker_stack[GUEST_THREAD_SLOTS]
                                    [GUEST_WORKER_STACK_BYTES]
	__attribute__((aligned(16)));

/* Что создано и что запущено.
 *
 * `entry` — адрес входа гостя, снятый из `$a1` при `CreateThread`.
 * `running` — запущен ли поток: без этого `StartThread` второй раз ничего
 * не делает, как и в жизни повторный запуск того же потока. */
static struct {
	u32 entry[GUEST_THREAD_SLOTS];
	u32 running[GUEST_THREAD_SLOTS];
	u32 tid[GUEST_THREAD_SLOTS];
	u32 calls[GUEST_THREAD_SLOTS];   /* вызовов импортов этим слотом */
	u32 faults[GUEST_THREAD_SLOTS];  /* сбоев НЕ из нулевой страницы */
	u32 uid[GUEST_THREAD_SLOTS];     /* PSP uid слота (next_id) */
	u32 next_id;
	/* Блок аргументов потока: `args` — длина, `argp` — указатель.
	 *
	 * Точка входа ПОТОКА у PSP получает `(SceSize args, void *argp)` из
	 * `sceKernelStartThread`, а НЕ `(argc, argv)`: разбор кода гостя
	 * (2026-10-14) показал, что точка входа главного игрового потока
	 * (runtime 0x08A9C958) САМА режет блок по '\0' в массив `argv[0..19]`
	 * на своём стеке, терминирует нулём и передаёт ДАЛЬШЕ уже пару
	 * `(argc, argv)`. Цепочка полная: `module_start` (он же `e_entry`,
	 * runtime 0x08A9C844) пересылает свои `(a0,a1)` в `StartThread` как
	 * `(a1,a2)` без изменений.
	 *
	 * Прежнее прочтение полей как `argc/argv` из `$a2/$a3` было ошибкой
	 * слотов: `$a3` содержит застывший аргумент `CreateThread`. С пустым
	 * блоком после разбора `argv[0] = NULL`, и игра падала на
	 * `strncmp(NULL, "disc0:", 6)` (runtime 0x08AD7788). Разбор
	 * цепочки — `docs/guest-boot-args.md`. */
	u32 args;
	u32 argp;
} g_thread;

/* Жив ли гость в этом потоке: сверяет tid прерванного потока с tid гостевых.
 *
 * Зачем. На четвёртом сбое прогона `tid` оказался `0x1FC4`, тогда как гостевые
 * сбои шли с `tid=0x6AC`, — то есть в наш обработчик сигнала зашёл ЧУЖОЙ
 * поток, а не гость. Без этой проверки его кадр (`ra=0x0A`, `pc=0x2CA00894`
 * вне образа) попадал в отчёт и в те же счётчики, что настоящие гостевые
 * сбои, и «гость ушёл в наш код» выглядело фактом.
 *
 * Проверяется само значение, а не `/proc`: в обработчике сигнала нельзя ни
 * открыть файл, ни позвать libc.
 *
 * Нулевой `tid` считается гosteвым НАМЕРЕННО: значение не получено — значит
 * проверить нечем, и хуже пропустить настоящий сбой, чем принять чужой. */
static int guest_thread_fault(u32 tid)
{
	u32 slot;

	if (tid == 0)
		return 1;
	if (tid == (u32)g_guest_pid)
		return 1;
	for (slot = 0; slot < GUEST_THREAD_SLOTS; slot++) {
		if (g_thread.tid[slot] != 0 && g_thread.tid[slot] == tid)
			return 1;
	}
	return 0;
}

static void worker_thread(void *arg);
static void early_log(const char *tag, u32 v);

/* АРЕНА РАЗДЕЛА PSP — `sceKernelAllocPartitionMemory` (0x237DBD4F) и
 * `sceKernelGetBlockHeadAddr` (0x9D9A5BA1).
 *
 * Обоснование — измерено, а не предположено. sbrk/инициализация кучи гостя
 * (`0x088041F8`) вызывает `AllocPartitionMemory(2, "UserSbrk", 3, 0x4000,
 * 4096)` по стабу `0x3026A0`; при возврате 0 выполняется
 * `blez v0,0x8804290`, переход БЕРЁТСЯ, вызов `GetBlockHeadAddr`
 * (`0x08804270`, стаб `0x3026A8`) ПРОПУСКАЕТСЯ, sbrk возвращает −1, malloc
 * отказывает, CRT0 зовёт `exit(1)` — а `exit` на этом госте заканчивается
 * вечным циклом (`li a0,1 … b сам на себя`). Лог того прогона совпадает:
 * `GetBlockHeadAddr` не появляется, потому что `blez` его перепрыгнул.
 *
 * Отсюда требование: возврат обязан быть НЕНУЛЕВЫМ дескриптором, а адрес —
 * указателем внутрь нашего блока, иначе цепочка повторится.
 *
 * Ограничение, честно и без обиняков: это БАМП-АЛЛОКАТОР БЕЗ ОСВОБОЖДЕНИЯ.
 * `FreePartitionMemory` (0xB6D61D02) — заглушка, память не возвращается
 * (измерено: 1 вызов из 538). Поэтому «достаточно одного раздела» здесь
 * проверено для текущего прогона, а не доказано для игры целиком: если гость
 * станет дробить и освобождать блоки, арена истощится.
 *
 * Выравнивание 64, а не 4/16: сюда же приходят буферы списков GE, которым
 * нужна кэш-линия. */
/* Арена разделов — ВНУТРИ отображения гостя (`g_mod->part_base`), а не в
 * нашем `.bss`.
 *
 * Измерение, которым это заменено: `GetBlockHeadAddr` вернул `0x2D168980` —
 * хостовский адрес. Гость пошёл по нему, прочитал нули вместо того, что на
 * PSP кладёт загрузчик, и упал на `memcmp(NULL,NULL,2)`. Плюс такой адрес
 * нельзя отдавать по существу: гость по нему пишет, а писать в чужую
 * `.bss` ему нельзя.
 *
 * Побочный выигрыш: было 8 МиБ статической арены в нашей библиотеке,
 * стало `GUEST_PART_RESERVE` (1 МиБ) в отображении гостя. */
#define PART_SLOTS		128u
#define PART_ALIGN		64u

static u32 g_part_off;
static u32 g_part_size[PART_SLOTS];
static u32 g_part_base[PART_SLOTS];

/* Снимки пути `sceKernelLoadModule` — последние `LM_SNAP_SLOTS` вызовов.
 *
 * `where` различает ОБЛАСТЬ, из которой снят путь: 0 — вне известных
 * отображений (снимок пуст, адрес напечатан только в кольце вызовов),
 * 1 — образ гостя, 2 — стек гостя. Имена модулей, собранные `strcat`-ом
 * на стеке, обязаны попасть в область 2. */
#define LM_SNAP_SLOTS GUEST_LM_SNAP_SLOTS
#define LM_SNAP_WORDS GUEST_LM_SNAP_WORDS
static u32 g_lm_words[LM_SNAP_SLOTS][LM_SNAP_WORDS];
static u32 g_lm_where[LM_SNAP_SLOTS];
static u32 g_lm_slot;
static u32 g_lm_count;
static u32 g_lm_uid;

/* Стек главного потока гостя определён ниже (около запуска), а пробе
 * `sceKernelLoadModule` нужен уже здесь — для проверки диапазона.
 * Объявление повторяет определение слово в слово. */
static unsigned char g_thread_stack[GUEST_STACK_BYTES]
	__attribute__((aligned(16)));

/* Выдать дескриптор на `bytes` байт. Дескриптор — индекс + 1, то есть никогда
 * не ноль: ноль означает отказ, и на нём гость уходит в тот же `exit(1)`. */
static u32 part_alloc(u32 bytes)
{
	u32 slot;
	u32 off;
	u32 need;

	if (bytes == 0)
		bytes = 4;
	/* Предел — размер резерва, а не константа: он задаётся загрузчиком при
	 * отображении гостя, и молчаливый выход за него означал бы запись в
	 * чужую память сразу за отображением. */
	if (g_mod == 0 || g_mod->part_base == 0)
		return 0;
	need = (bytes + (PART_ALIGN - 1u)) & ~(PART_ALIGN - 1u);
	off = (g_part_off + (PART_ALIGN - 1u)) & ~(PART_ALIGN - 1u);
	if ((u64)off + need > (u64)GUEST_PART_RESERVE)
		return 0;
	/* Вторая граница — фактический конец ОТОБРАЖЕНИЯ, а не резерв.
	 *
	 * `tramp_base` выравнивается вверх до 4 КиБ от `span`, а `map_size`
	 * считается от невыровненного `span`, поэтому запас между ними не
	 * гарантирован. Без этой проверки блок, выданный у самого края, ушёл бы
	 * писать за отображение — то есть в чужую память. */
	if ((u64)g_mod->part_base + off + need
	    > (u64)g_mod->base + (u64)g_mod->map_size)
		return 0;
	for (slot = 0; slot < PART_SLOTS; slot++) {
		if (g_part_size[slot] == 0) {
			g_part_off = off + need;
			g_part_size[slot] = bytes;
			g_part_base[slot] = off;
			return slot + 1;
		}
	}
	return 0;
}

/* Адрес начала блока по дескриптору. Неверный дескриптор — ноль, то есть
 * отказ, а не указатель в никуда. */
static u32 part_head(u32 handle)
{
	u32 slot = handle - 1u;

	if (handle == 0 || handle > PART_SLOTS || g_part_size[slot] == 0)
		return 0;
	if (g_mod == 0 || g_mod->part_base == 0)
		return 0;
	/* Адрес ВНУТРИ образа гостя: иначе гость читает нашу память вместо
	 * своей, и по ней ходит как по своей — см. комментарий у `g_part_off`. */
	return g_mod->part_base + g_part_base[slot];
}

/* КУЧА VPL PSP — семейство `sceKernel*Vpl`.
 *
 * Обоснование — прогон 35: гость строит свою многоуровневую кучу из
 * БОЛЬШИХ блоков VPL (таблица дескрипторов по vaddr 0x30B790, 6×16 Б,
 * суммарно 0x841B00). Пока семейство не было подменено, `AllocateVplCB`
 * не писала адрес блока в приёмник, гость брал вместо базы кучи мусор
 * и падал на `sw $zero,($a0)` в `HeapSystem` (vaddr 0x2B8134, runtime
 * 0x08ABC134).
 *
 * Пул — отдельная область `g_mod->vpl_base` (10 МиБ по 0x09000000):
 * суммарные размеры куч гостя не помещаются в резерв разделов.
 *
 * Ограничение, как у `part_alloc`: БАМП-АЛЛОКАТОР БЕЗ ОСВОБОЖДЕНИЯ.
 * `FreeVpl` — заглушка-счётчик; заметное `vpl_free=` в отчёте
 * означает, что арена истощается и заглушку надо делать честной. */
#define VPL_SLOTS		8u
#define VPL_ALIGN		64u

static struct {
	u32 size[VPL_SLOTS];   /* размер пула; 0 = слот свободен */
	u32 attr[VPL_SLOTS];
	u32 start[VPL_SLOTS];  /* начало пула, смещение от vpl_base */
	u32 used[VPL_SLOTS];   /* курсор бампа внутри пула */
	char name[VPL_SLOTS][32];
	u32 off;               /* глобальный курсор области */
	u32 created;           /* приборы для отчёта */
	u32 allocs;
	u32 frees;
	u32 fails;
} g_vpl;

/* Указатель на `len` байт памяти гостя: образ, любой из стеков или
 * область VPL (куча гостя живёт там с 2026-10-15). NULL — адрес вне
 * известных отображений.
 *
 * VPL-область разыменовывается напрямую: она отображена в нашем
 * процессе по фиксированным адресам, маску зеркал снимает вызывающий
 * (нормализованный адрес совпадает с кэшируемым). */
static const unsigned char *guest_read_ptr(u32 addr, u32 len)
{
	unsigned char *ws;
	u64 wend;

	if (g_mod != NULL && g_mod->memory != NULL
	    && addr >= g_mod->base
	    && (u64)addr + len <= (u64)g_mod->base + g_mod->map_size)
		return g_mod->memory + (addr - g_mod->base);
	if (addr >= (u32)(uintptr_t)&g_thread_stack[0]
	    && (u64)addr + len
	       <= (u64)(uintptr_t)&g_thread_stack[0] + GUEST_STACK_BYTES)
		return &g_thread_stack[addr - (u32)(uintptr_t)&g_thread_stack[0]];
	/* Площинный указатель: весь двумерный массив стеков непрерывен,
	 * а `[]` в C связывает крепче `&`, поэтому `&arr[0][0][k]` —
	 * синтаксическая ошибка, и адрес берётся один раз в локаль. */
	ws = &g_worker_stack[0][0];
	wend = (u64)(uintptr_t)ws + (u64)GUEST_THREAD_SLOTS
	       * GUEST_WORKER_STACK_BYTES;
	if (addr >= (u32)(uintptr_t)ws && (u64)addr + len <= wend)
		return ws + (addr - (u32)(uintptr_t)ws);
	/* VPL-область разыменовывается напрямую: она отображена в нашем
	 * процессе по фиксированным адресам, маску зеркал снимает вызывающий
	 * (нормализованный адрес совпадает с кэшируемым). Граница —
	 * фактический размер области: он меняется от прогона к прогону. */
	if (g_mod != NULL && g_mod->vpl_base != 0
	    && addr >= g_mod->vpl_base
	    && (u64)addr + len
	       <= (u64)g_mod->vpl_base + g_mod->vpl_bytes)
		return (const unsigned char *)(uintptr_t)addr;
	return NULL;
}

/* Указатель на имя VPL: 32 байта из любого известного отображения. */
static const unsigned char *vpl_name_ptr(u32 addr)
{
	return guest_read_ptr(addr, 32);
}

/* Снимок имени VPL: до 31 байта, всегда с завершающим нулём. */
static void vpl_snap_name(char *dst, u32 addr)
{
	const unsigned char *p = vpl_name_ptr(addr);
	u32 i;

	for (i = 0; i < 32; i++)
		dst[i] = 0;
	if (p == NULL)
		return;
	for (i = 0; i < 31; i++) {
		dst[i] = (char)p[i];
		if (p[i] == 0)
			break;
	}
}

/* Выдать блок `bytes` из пула `uid`. Бамп по 64 байта — то же
 * выравнивание, что у `part_alloc`: сюда приходят те же по духу
 * буферы. Отказ — ноль; приёмник при этом обнуляет вызывающий код,
 * чтобы гость не прочёл мусор вместо базы кучи. */
static u32 vpl_alloc_block(u32 uid, u32 bytes)
{
	u64 avail;
	u64 want;
	u32 at;
	u32 slot;

	if (uid == 0 || uid > VPL_SLOTS)
		return 0;
	slot = uid - 1u;
	if (g_vpl.size[slot] == 0)
		return 0;
	if (g_mod == NULL || g_mod->vpl_base == 0)
		return 0;
	if (bytes == 0)
		return 0;
	/* Сравнение в 64 битах: сложение `used + need > size` при
	 * огромном `bytes` переполняет 32 бита и пропускает отказ, а
	 * само `need` переполняется уже при `bytes > 0xFFFFFFC0`. */
	avail = (u64)g_vpl.size[slot] - g_vpl.used[slot];
	want = ((u64)bytes + (VPL_ALIGN - 1u)) & ~(u64)(VPL_ALIGN - 1u);
	if (want > avail)
		return 0;
	at = g_vpl.start[slot] + g_vpl.used[slot];
	g_vpl.used[slot] += (u32)want;
	return g_mod->vpl_base + at;
}

/* Опережающее объявление: помощники шрифтов ниже пишут в память
 * гостя, а определение `guest_write_u32` стоит в секции кадра. */
static void guest_write_u32(u32 addr, u32 value);

/* --- ШРИФТОВАЯ БИБЛИОТЕКА sceLibFont --------------------------------------
 *
 * Обоснование — прогон 36: с работающей кучей гость дошёл до
 * шрифтов, `sceFontNewLib` без подмены вернула 0, гость передал
 * ноль в `GetNumFontList(a0=0)` и пошёл читать поля ХЭНДЛА: первые
 * сбои — addr 0x4 и 0xC, а по раскладке `SceFontLibData`
 * (pspsdk/src/font/pspfont.h) это `maxFonts` и `alloc` (колбэк
 * выделения). То есть гость читает параметры ИЗ ХЭНДЛА, а не из
 * своей копии, — значит хэндл обязан нести копию params (44 Б),
 * как её хранит настоящее ядро.
 *
 * Раскладки — не по памяти, а по локальному эталону JPCSP
 * (`tmp/jpcsp/src/jpcsp/HLE/modules/sceFont.java` и классы
 * pspFontStyle/pspCharInfo): SceFontStyle 0xA8, SceFontInfo 0x108,
 * SceFontLibData 44, pspCharInfo 0x3C. Ошибки — SceKernelErrors
 * JPCSP: 0x80460003 INVALID_PARAMETER, 0x80460005 FILE_NOT_FOUND.
 *
 * Битовые паттерны float посчитаны скриптом, в коде НЕТ
 * float-арифметики: `-nostdlib` без libgcc, и деление дало бы
 * неразрешённый символ, на котором `dlopen` отвергнет библиотеку.
 *
 * Ограничение: РЕНДЕРА ГЛИФОВ НЕТ — PGF не разбирается.
 * `GetCharGlyphImage*` отвечают ошибкой; счётчик `font_glyph=`
 * в отчёте покажет, терпит ли гость отказ. Это следующий замер,
 * а не окончательное решение. */
#define FONT_LIBDATA_BYTES   44u
#define FONT_STYLE_WORDS     (0xA8u / 4u)
#define FONT_INFO_WORDS     (0x108u / 4u)
#define FONT_CHARINFO_WORDS (0x3Cu / 4u)
#define FONT_ERR_PARAM       0x80460003u
#define FONT_ERR_FILE        0x80460005u

static struct {
	u32 libs;    /* sceFontNewLib */
	u32 opens;   /* успешные sceFontOpen* */
	u32 glyphs;  /* отказы рендера глифов */
} g_font;

/* --- ФЛАГИ СОБЫТИЙ sceKernel*EventFlag --------------------------------
 *
 * Игра создаёт флаги событий (CreateEventFlag #0x1F) и затем ждёт их
 * (WaitSema/EventFlag по нашим подменённым семафорам). Без uid>0 игра
 * думает, что флаг не создан, и не идёт дальше инициализации.
 * БАМП без освобождения — как part_alloc/Vpl. */
#define EF_SLOTS		8u

static struct {
	u32 size[EF_SLOTS];   /* 1 = слот занят, 0 = свободен */
	u32 attr[EF_SLOTS];
	u32 init[EF_SLOTS];
	char name[EF_SLOTS][32];
	u32 created;
} g_ef;

/* --- ЯДРА SAS (sceSasCore) --------------------------------------------
 *
 * Замер 90: `__sceSasInit(core, …)` и семейство были нереализованы, и
 * звуковая подсистема LocoRoco шла по «0 = успех» без какой-либо записи
 * состояния. Структура ядра для игры opaque (аппаратное состояние, PSP
 * SDK требует только память выделить), поэтому держим ПАРАМЕТРЫ Init у
 * себя — их обязаны согласованно возвращать GetOutputmode и Rev*. */
#define SAS_CORES		4u

static struct {
	u32 core;    /* указатель ядра в памяти гостя; 0 = слот свободен */
	u32 grain;
	u32 voices;
	u32 mode;
	u32 rate;
} g_sas[SAS_CORES];

/* --- СЕМАФОРЫ (sceKernel*Sema) -----------------------------------------
 *
 * Замер 97 (eb5e7aee): CreateSema был лишь в is_implemented — ВСЕ восемь
 * семафоров игры получили uid 0 (у PSP uid всегда > 0), и WaitSema/Signal
 * крутились по невалидному uid. Блокировки выродились, порядок задач
 * рассыпался: task-воркер успевал чистить флаг готовности звука ДО
 * walk'а главного потока — аллокатор видел «звук выключен» и возвращал
 * NULL. Настоящее БЛОКИРОВАНИЕ WaitSema (паками по 5 мс до SignalSema)
 * восстанавливает PSP-порядок: воркер честно ждёт задачу, главный идёт
 * вперёд. Ожидание ограничено 60 с (WST в логе) и размыкается
 * g_time_to_exit — правило «ни один стаб не крутится вечно». */
#define SEMA_SLOTS		16u
#define SEMA_PARK_MS		5u
#define SEMA_STUCK_MS		60000u

static struct {
	u32 used;    /* 1 = слот занят */
	u32 count;   /* текущий счётчик */
	u32 max;
} g_sema[SEMA_SLOTS];
static u32 g_sema_created;
static u32 g_sema_waited;
static u32 g_sema_stuck;

/* Замер 105: гостевые адреса ЗА пределами образа (куча VPL
 * 0x09000000+, объект дренажа 0x09009488) — тоже наша память:
 * весь гость — фиксированные mmap, гостевый адрес = прямой
 * хост-указатель. Диапазон: образ/VPL/зеркала. */
static int guest_addr_direct_ok(u32 a)
{
	if (a >= 0x08800000u && a < 0x0A900000u)
		return 1;   /* образ + VPL (+запас) */
	if (a >= 0x48800000u && a < 0x4A900000u)
		return 1;   /* uncached-зеркала */
	return 0;
}

static s32 sema_uid_valid(u32 uid)
{
	return uid >= 1u && uid <= SEMA_SLOTS && g_sema[uid - 1u].used != 0;
}

/* --- ОБРАТНЫЕ ВЫЗОВЫ sceKernel*Callback ------------------------------
 *
 * `sceKernelCreateCallback` возвращает uid > 0. Игра может использовать
 * uid для дальнейших операций, поэтому отдаём 순новый дескриптор. */
#define CB_SLOTS		8u

static struct {
	u32 created;
	u32 fired;    /* исполнено колбеков нашим HLE */
} g_cb;

/* --- КОЛБЕКИ: гостевые функции, вызываемые НАШИМ HLE ---------------
 *
 * Модель PSP: `sceKernelCreateCallback(name, func, common)` даёт uid;
 * ядро зовёт `func(common, notifyArg)` в контексте того потока,
 * который вызвал сервисную *CB-функцию (SleepThreadCB, WaitSemaCB,
 * ...). У Локо Роко 2 есть поток ptnCallbackTh: он регистрирует
 * ptnExitCB/ptnPowerCB/ptnUmdStatCB/ptnMsStatCB (замер: ra
 * CreateCallback=0x088C2A2C, uid UMD=5 ушёл в
 * sceUmdRegisterUMDCallBack) и уходит в SleepThreadCB НАВСЕГДА
 * (после возврата его ждёт sceKernelExitDeleteThread — паттерн
 * «спать до выхода», 0x088C2B50→0x088C2B64).
 *
 * Гость — нативный MIPS-код в нашем адресном пространстве, ABI o32
 * совпадает: колбек вызывается ПРЯМЫМ вызовом указателя. Гость не
 * использует $gp для глобалей (весь доступ через lui+addiu —
 * проверено дизассембром), s-регистры callee-saved по o32. */
#define CBK_MAX     32
static u32 g_cbk_func[CBK_MAX];   /* uid → адрес гостевой функции */
static u32 g_cbk_arg[CBK_MAX];     /* uid → common (a0 при вызове) */
#define NTF_MAX     64
static struct {
	u32 uid;
	u32 arg;
} g_ntf[NTF_MAX];
static u32 g_ntf_head;
static u32 g_ntf_tail;

static void cbk_notify(u32 uid, u32 arg)
{
	u32 next = (g_ntf_tail + 1) % NTF_MAX;
	if (next == g_ntf_head)
		return;   /* переполнение — теряем, не роняем */
	g_ntf[g_ntf_tail].uid = uid;
	g_ntf[g_ntf_tail].arg = arg;
	g_ntf_tail = next;
}

/* Раз в ~2 с (40 чанков × 50 мс) послать гостевым потокам SIGALRM:
 * обработчик напечатает их PC.
 *
 * Прогоны 71–72 показали: список g_thread.tid[] ХРОМАЕТ — по
 * счётчикам живы 5–6 гостевых потоков (SleepThread 600K, муз-цикл
 * 100K, семафоры 400K), а отвечали на tgkill только двое-трое из
 * tid[]. Чтобы не гадать, кто потерялся, список берётся прямо из
 * ядра: /proc/self/task (getdents64). Чужие потоки shell
 * фильтруются ДО посылки: guest_thread_fault, как в обработчике. */
static int scan_threads(u32 *out, int max)
{
	static const char path[] = "/proc/self/task";
	static unsigned char buf[4096];
	long fd;
	int n = 0;

	fd = plat_syscall(4005 /* open */, (long)path,
	                  0x10000u /* O_DIRECTORY */, 0, 0);
	if (fd < 0)
		return 0;
	for (;;) {
		/* MIPS o32: getdents64 = 4219 (4000+219), НЕ 4204 —
		 * 4204 это rt_sigprocmask. Замер прогона 73: скан
		 * возвращал пусто, TIDN=0, семплы шли только по
		 * страховочному tid[]. */
		long r = plat_syscall(4219 /* getdents64 */, fd,
		                      (long)buf, 4096, 0);
		long off;

		if (r <= 0)
			break;
		for (off = 0; off < r && n < max; ) {
			/* struct linux_dirent64: u64 ino; s64 off;
			 * u16 reclen; u8 type; char name[] */
			unsigned short reclen =
			    (unsigned short)(buf[off + 16]
			                     | ((unsigned short)buf[off + 17] << 8));
			const char *p = (const char *)buf + off + 19;
			u32 tid = 0;

			while (*p >= '0' && *p <= '9')
				tid = tid * 10u + (u32)(*p++ - '0');
			if (tid != 0)
				out[n++] = tid;
			if (reclen == 0)
				break;
			off += reclen;
		}
		if (r < 4096)
			break;
	}
	plat_syscall(4006 /* close */, fd, 0, 0, 0);
	return n;
}

static void cbk_sample(void)
{
	static u32 tick;
	static u32 samp_tick;   /* накопительный: GAT-детектор */
	static u32 first_list = 1;
	static u32 gone_done;
	u32 self = (u32)plat_gettid();
	u32 pid = (u32)plat_getpid();
	u32 slot;

	if (++tick < 2)
		return;
	tick = 0;
	samp_tick++;
	if (g_samp_n >= 300)
		return;
	if (pid == 0)
		return;
	{
		static u32 tids[128];
		int n = scan_threads(tids, 128);
		int i;

		if (first_list) {
			first_list = 0;
			early_log("TIDN", (u32)n);
			for (i = 0; i < n && i < 16; i++)
				early_log("TID", tids[i]);
			for (slot = 0; slot < GUEST_THREAD_SLOTS; slot++)
				if (g_thread.tid[slot] != 0)
					early_log("TSLOT", g_thread.tid[slot]);
			if (g_guest_pid > 0)
				early_log("MTIDX", (u32)g_guest_pid);
		}
		/* GONE-детектор: смерть гостевого потока перестала быть
		 * невидимой (прогоны 74–76: главный игровой и ещё один
		 * поток мертвы без НИ ОДНОГО следа — ни фолта, ни
		 * SIGILL, ни выхода). Проверка — tgkill с сигналом 0:
		 * ядро отвечает ESRCH мёртвому tid. Печатаем ОДИН РАЗ
		 * с entry — кто и когда (tick = 2 с на единицу). */
		for (slot = 0; slot < GUEST_THREAD_SLOTS; slot++) {
			u32 tid = g_thread.tid[slot];

			if (tid == 0 || (gone_done & (1u << slot)))
				continue;
			if (plat_syscall(SYS_tgkill_mips, (long)pid,
			                 (long)tid, 0, 0) != 0) {
				gone_done |= 1u << slot;
				early_log("GONE!", g_thread.entry[slot]);
				early_log("GTID", tid);
				/* Накопительный тик: прежний GAT печатал
				 * ЛОКАЛЬНЫЙ tick, обнуляемый при каждом
				 * вызове, — всегда 0. Замеры 74–80: GAT
				 * не отличал «первый тик» от «сотого». */
				early_log("GAT", samp_tick * 2u);
				early_log("GCALL", (u32)g_guest_calls);
				/* Счётчик вызовов ИМЕННО этого слота:
				 * кто сколько сделал до смерти. */
				early_log("GTCL", g_thread.calls[slot]);
			}
		}
		for (i = 0; i < n; i++) {
			u32 tid = tids[i];

			if (tid != self && guest_thread_fault(tid))
				plat_syscall(SYS_tgkill_mips, (long)pid,
				             (long)tid, 14, 0);
		}
	}
	for (slot = 0; slot < GUEST_THREAD_SLOTS; slot++) {
		u32 tid = g_thread.tid[slot];

		if (tid != 0 && tid != self && guest_thread_fault(tid))
			plat_syscall(SYS_tgkill_mips, (long)pid, (long)tid,
			             14, 0);
	}
}

/* Исполнить все отложенные уведомления. Вызывается из SleepThreadCB:
 * ровно там, где PSP-ядро исполняло бы колбеки. */
static void cbk_deliver(void)
{
	while (g_ntf_head != g_ntf_tail) {
		u32 uid = g_ntf[g_ntf_head].uid;
		u32 arg = g_ntf[g_ntf_head].arg;
		void (*fn)(u32, u32);

		g_ntf_head = (g_ntf_head + 1) % NTF_MAX;
		if (uid == 0 || uid >= CBK_MAX)
			continue;
		fn = (void (*)(u32, u32))(uintptr_t)g_cbk_func[uid];
		if (fn == NULL)
			continue;
		/* Счётчик вызова колбека — иначе он невидим в отчёте
		 * (гостевая функция сама импортов может не делать). */
		g_cb.fired++;
		fn(g_cbk_arg[uid], arg);
	}
}

/* --- СУБ-ПРЕРЫВАНИЯ (sceKernelRegisterSubIntrHandler) -------------------
 *
 * Замер 92 (2ac0de9e), дамп walk'а: бэкенд аллокатора 0x08ABA704
 * возвращает 0, пока у глобального флага [0x08BAA7AC] не стоит бит 0.
 * Единственный писатель флага в госте — сеттер 0x08AB97F0, и ЕДИНСТВЕННЫЙ
 * его вызов (0x088C2530) передаёт 0, то есть ОБНУЛЯЕТ. Значит единицу
 * ставит не линейный код гостя, а обработчик суб-прерывания: игра
 * регистрирует RegisterSubIntrHandler(30=VBLANK, …) (замер по счётчику
 * a0=0x1E) — на PSP ядро зовёт его 60 раз в секунду, и этот тик качает
 * звуковой движок («SXGS»), поднимая флаг и заполняя таблицы. Наша
 * заглушка не вызывала обработчик НИ РАЗУ — насос стоял, аллокатор
 * навсегда видел «звук не готов» и возвращал NULL.
 *
 * Доставка — тот же приём, что у колбеков: ПРЯМОЙ вызов гостевой
 * функции (гость не использует `$gp` для глобалов — проверено
 * дизасмом, README §5). Качает его парк в `sceKernelSleepThreadCB`
 * (ptnCallbackTh), с периодом ~16 мс = VBlank. */
#define SI_SLOTS		4u

static struct {
	u32 intr;     /* линия прерывания (30 = VBLANK) */
	u32 slot;     /* слот: ядро PSP передаёт его первым аргументом */
	u32 handler;  /* гостевая функция */
	u32 common;   /* второй аргумент обработчика */
	u32 enabled;  /* EnableSubIntr прошёл */
} g_si[SI_SLOTS];
static u32 g_si_fired;
static u32 g_in_si_fire;   /* 1 = исполняем тик-обработчик */

static void si_fire(void)
{
	u32 i;

	for (i = 0; i < SI_SLOTS; i++) {
		void (*fn2)(u32, u32);

		if (g_si[i].handler == 0 || g_si[i].enabled == 0)
			continue;
		fn2 = (void (*)(u32, u32))(uintptr_t)g_si[i].handler;
		g_si_fired++;
		/* Замер 93: обработчик требует a0 == слоту (10).
		 * Замер 98: WaitSema внутри тика не блокирует.
		 * Замер 99: SIE/SIX — если SIE есть, а SIX нет,
		 * обработчик завис в гостевом коде. */
		if (g_si_fired <= 16u && g_log_fd >= 0)
			early_log("SIE", g_si[i].handler);
		g_in_si_fire = 1;
		fn2(g_si[i].slot, g_si[i].common);
		g_in_si_fire = 0;
		if (g_si_fired <= 16u && g_log_fd >= 0)
			early_log("SIX", g_si[i].handler);
	}
}

/* Нулевой блок арены разделов под фейковый объект ядра.
 * Возвращает ГОСТЕВОЙ адрес блока, 0 — арена исчерпана. */
static u32 font_obj_block(void)
{
	u32 block = part_head(part_alloc(256u));
	unsigned char *h;
	u32 i;

	if (block == 0 || g_mod == NULL || g_mod->memory == NULL)
		return 0;
	h = g_mod->memory + (block - g_mod->base);
	for (i = 0; i < 256u; i++)
		h[i] = 0;
	return block;
}

/* Слова стиля jpn0.pgf — единственного «системного» шрифта
 * (реестр JPCSP): игра японская (ULJS00042). Размеры 26.6→float,
 * имена посчитаны скриптом, не руками. */
static void font_style_words(u32 *w)
{
	u32 i;

	for (i = 0; i < FONT_STYLE_WORDS; i++)
		w[i] = 0;
	w[0] = 0x41220000u;   /* fontH 10.125 */
	w[1] = 0x41220000u;   /* fontV */
	w[2] = 0x43000000u;   /* fontHRes 128 */
	w[3] = 0x43000000u;   /* fontVRes */
	w[4] = 0x00000000u;   /* weight */
	w[5] = 0x00670001u;   /* family 1 (SANS_SERIF) | style 103 (DB) */
	w[6] = 0x00010000u;   /* subStyle 0 | language 1 (JAPANESE) */
	w[7] = 0x00010000u;   /* region 0 | country 1 (JAPAN) */
	w[8] = 0x2D545446u;   /* «FTT-» */
	w[9] = 0x5277654Eu;   /* «NewR» */
	w[10] = 0x6E69646Fu;  /* «odin» */
	w[11] = 0x6F725020u;  /* " Pro" */
	w[12] = 0x00424420u;  /* " DB\0" */
	w[24] = 0x306E706Au;  /* «jpn0» */
	w[25] = 0x6667702Eu;  /* «.pgf» */
}

/* Слова `SceFontInfo` (0x108): метрики максимального глифа в 26.6,
 * те же — float, счётчики символов, стиль по +0x5C, bpp по +0x104.
 * Значения метрик — правдоподобные для jpn0, НЕ измеренные:
 * расходуются на раскладку текста, неверные дают кривой набор
 * текста, а не сбой. */
static void font_info_words(u32 *w)
{
	u32 i;

	for (i = 0; i < FONT_INFO_WORDS; i++)
		w[i] = 0;
	w[0] = 0x300u;         /* maxGlyphWidth 12.0 */
	w[1] = 0x300u;         /* maxGlyphHeight */
	w[2] = 0x2D0u;         /* ascender 11.25 */
	w[3] = 0xFFFFFFD0u;    /* descender −0.75 */
	w[4] = 0xFFFFFF70u;    /* leftX −2.25 */
	w[5] = 0x2D0u;         /* baseY */
	w[6] = 0x00000000u;    /* minCenterX */
	w[7] = 0x00000000u;    /* maxTopY */
	w[8] = 0x300u;         /* advanceX */
	w[9] = 0x00000000u;    /* advanceY */
	w[10] = 0x41400000u;   /* float 12.0 */
	w[11] = 0x41400000u;
	w[12] = 0x41340000u;   /* float 11.25 */
	w[13] = 0xBF400000u;    /* float −0.75 */
	w[14] = 0xC0100000u;   /* float −2.25 */
	w[15] = 0x41340000u;
	w[16] = 0x00000000u;
	w[17] = 0x00000000u;
	w[18] = 0x41400000u;
	w[19] = 0x00000000u;
	w[20] = 0x000C000Cu;   /* maxWidth 12 | maxHeight 12 */
	w[21] = 0x2000u;       /* characters (реестр JPCSP) */
	w[22] = 0x00000000u;   /* subCharacters */
	font_style_words(&w[23]);   /* стиль по +0x5C */
	w[65] = 0x00000004u;   /* bpp PGF по +0x104 */
}

/* Слова `pspCharInfo` (0x3C): метрики ОДНОГО символа — для фейка
 * все символы одинаковые, 12×12. */
static void font_charinfo_words(u32 *w)
{
	u32 i;

	for (i = 0; i < FONT_CHARINFO_WORDS; i++)
		w[i] = 0;
	w[0] = 12u;            /* bitmapWidth */
	w[1] = 12u;            /* bitmapHeight */
	w[2] = 0x00000000u;    /* bitmapLeft */
	w[3] = 0x00000000u;    /* bitmapTop */
	w[4] = 0x300u;         /* sfp26 width 12.0 */
	w[5] = 0x300u;         /* sfp26 height */
	w[6] = 0x2D0u;         /* sfp26 ascender */
	w[7] = 0xFFFFFFD0u;    /* sfp26 descender −0.75 */
	w[8] = 0x00000000u;    /* bearingHX */
	w[9] = 0x00000000u;    /* bearingHY */
	w[10] = 0x00000000u;   /* bearingVX */
	w[11] = 0x00000000u;   /* bearingVY */
	w[12] = 0x300u;        /* advanceH */
	w[13] = 0x00000000u;   /* advanceV */
	w[14] = 0x00000000u;   /* выравнивание */
}

/* Выгрузить `n` слов локального буфера в память гостя. */
static void font_put_words(u32 addr, const u32 *w, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		guest_write_u32(addr + 4u * i, w[i]);
}

/* `CreateThread`: запомнить адрес входа и выдать идентификатор.
 *
 * Идентификатор не ноль: ноль означает «поток не создан», и на нём гость
 * прекращает инициализацию. */
static u32 thread_created(u32 entry)
{
	u32 slot;

	if (entry == 0)
		return 0;
	for (slot = 0; slot < GUEST_THREAD_SLOTS; slot++) {
		if (g_thread.running[slot] == 0 && g_thread.entry[slot] == 0) {
			g_thread.entry[slot] = entry;
			g_thread.next_id++;
			g_thread.uid[slot] = g_thread.next_id;
			return g_thread.next_id;   /* `$a1` — адрес входа, `$a0` — имя */
		}
	}
	return 0;
}

/* `StartThread`: поднять настоящий поток, который прыгнёт в код гостя.
 *
 * Стек передаётся значением, а не указателем: `plat_clone` кладёт в `$sp`
 * ребёнка именно это значение, и разыменовывать его не нужно. */
static u32 thread_started(void)
{
	unsigned long sp_top;
	u32 slot;

	for (slot = 0; slot < GUEST_THREAD_SLOTS; slot++) {
		if (g_thread.entry[slot] != 0 && g_thread.running[slot] == 0) {
			/* Верхушка стека — это НАЧАЛО следующего слота, а его
			 * передавать нельзя: проверка в `worker_thread` ищет слот
			 * условием `адрес < конец`, и на границе адрес относится
			 * уже к следующему слоту, у которого `entry` пуст.
			 *
			 * На живом плеере это стоило `WORKER=00000000` при
			 * `stEn=08ABCF68` — то есть адрес был верным числом и
			 * неверным местом, и выглядело как «поток не
			 * запустился», хотя он запустился и упал на нулевом
			 * входе. Отступ в 16 байтов держит адрес внутри слота. */
			sp_top = (unsigned long)&g_worker_stack[slot]
			                              [GUEST_WORKER_STACK_BYTES] - 16;
			/* `$sp` ребёнка — НИЖЕ блока контекста, а не у вершины.
			 *
			 * `arg` остаётся `sp_top`: это вершина стека, которую
			 * получит ГОСТЬ. А вот начальный `$sp` нашего потока должен
			 * быть ниже блока из 256 байт, который `worker_thread`
			 * заполнит для `$k0` гостя. Так и делает PPSSPP:
			 * `SP = stack_top - 256`, `K0 = SP`.
			 *
			 * Раньше оба значения совпадали, и тогда кадр
			 * `worker_thread` (56 байт) оказывался ЦЕЛИКОМ внутри
			 * блока: при `sp = sp_top - 16` кадр занимал
			 * `[sp_top-72, sp_top-16)`, а блок — `[sp_top-256, sp_top)`.
			 * Обнуление блока затирало слоты `gp`(+24), `ra`(+52),
			 * `s0..s4`(+32..+48). Наблюдалось ровно это:
			 * `lw gp,24(sp)` дал `0`, следующий `lw v0,-32588(gp)`
			 * обратился по `0xFFFF8074` и дал `SIGBUS`, а до
			 * `guest_thread_enter` поток не дошёл вовсе
			 * (`gmark_worker = 0`). */
			{
				long tid = plat_clone(worker_thread,
				                      (void *)(uintptr_t)sp_top,
				                      sp_top - THREAD_CTX_SPAN,
				                      PLAT_CLONE_THREAD_FLAGS);

				if (tid < 0)
					return -1;
				g_thread.tid[slot] = (u32)tid;
			}
			g_thread.running[slot] = 1;
			return 0;
		}
	}
	return 0;
}

/* `args` больше не читается: значение берётся у гостя из вызова
 * `CpuSuspendIntr`, а не из аргументов этого вызова. Параметр оставлен,
 * чтобы сигнатура совпадала с остальными обработчиками и её не пришлось
 * отличать в коде вызова. */

/* Блок `SceThreadContext` для `$k0` игрового потока.
 *
 * Раскладка — `SceThreadContext` из pspsdk, см. THREAD_CTX_BYTES.
 * Инициализируется целиком нулём: на старте потока ядро на PSP заполняет не всё
 * (в частности `gpr[]` — это сохранённый регистровый файл, а не входные
 * параметры), и выдумывать значения полей было бы подгонкой под симптом.
 * Значение имеет сам факт валидного записываемого адреса: падала инструкция
 * `sw s5,4($k0)`, то есть запись в `gpr[0]`.
 *
 * Возвращает 0, если блок выделить негде — тогда `$k0` останется нулём и
 * поведение будет прежним, то есть измеримым. */
static u32 thread_ctx_addr(u32 sp_top, u32 uid)
{
	volatile u32 *p;
	u32 ctx;
	u32 i;

	if (g_mod == 0 || g_mod->memory == 0)
		return 0;

	/* `SceKernelThreadContext` лежит в 256 байтах у ВЕРШИНЫ СТЕКА ПОТОКА.
	 *
	 * Модель взята из `Core/HLE/sceKernelThread.cpp` (`FillStack()`) PPSSPP и
	 * проверена чтением исходника:
	 *
	 *     context.r[MIPS_REG_SP] = currentStack.start + nt.stackSize;
	 *     context.r[MIPS_REG_SP] -= 256;
	 *     context.r[MIPS_REG_K0] = context.r[MIPS_REG_SP];
	 *     Memory::Memset(k0, 0, 0x100);
	 *     Memory::Write_U32(GetUID(),        k0 + 0xc0);
	 *     Memory::Write_U32(nt.initialStack, k0 + 0xc8);
	 *     Memory::Write_U32(0xffffffff,      k0 + 0xf8);
	 *     Memory::Write_U32(0xffffffff,      k0 + 0xfc);
	 *
	 * Прежняя модель (отдельный блок в пуле трамплинов) давала `$k0`, не связанный
	 * со стеком гостя. Замер это подтвердил: в момент первого сбоя `$k0` равен
	 * **0**, хотя трамплин ставит адрес контекста, — и наш адрес при этом лежит
	 * в `$s7`. `$k0` затирается между установкой трамплином и чтением гостем, и
	 * кто именно — пока не установлено; при модели «контекст на стеке» это
	 * перестаёт быть разницей: стек у потока свой, и область по `$k0` всегда
	 * валидна и записываема.
	 *
	 * `uid` — идентификатор потока: гость читает его из `k0 + 0xc0`.
	 * `sp_top` — вершина стека, кладётся в `k0 + 0xc8` как `initialStack`.
	 *
	 * Блок занимает ВЕРХНИЕ `THREAD_CTX_SPAN` байт стека, и начальный `$sp`
	 * потока обязан быть ровно под ним — иначе кадр самого потока ложится
	 * внутрь блока, и обнуление затирает его сохранённые регистры.
	 * См. `thread_started()`, где `$sp` и есть `sp_top - THREAD_CTX_SPAN`. */
	ctx = sp_top - THREAD_CTX_SPAN;
	p = (volatile u32 *)(uintptr_t)ctx;
	for (i = 0; i < THREAD_CTX_SPAN / 4; i++)
		p[i] = 0;
	p[0xc0 / 4] = uid;
	p[0xc8 / 4] = sp_top;
	p[0xf8 / 4] = 0xffffffffu;
	p[0xfc / 4] = 0xffffffffu;
	return ctx;
}

static u32 thread_info_addr(const u32 *args)
{
	volatile u32 *p;

	(void)args;

	/* Адрес возвращается ВСЕГДА, а не только при первом вызове.
	 *
	 * Раньше здесь стояло `если адрес уже есть — вернуть ноль`, и игровой
	 * поток, вызвавший импорт повторно, получал ноль вместо адреса. Ноль
	 * здесь не «неизвестно», а «адреса нет» — и блок памяти при этом лежит
	 * и готов.
	 *
	 * Связь этого места со сбоем по адресу `0x012D6E4C` НЕ установлена и
	 * прежняя запись о ней была неверна: при том сбое `pc` равен
	 * `si_addr`, а это выборка команды по неотображённому адресу, то есть
	 * `jr` по мусору, — а не запись по адресу из `$k0`. Пусть выглядит
	 * правдоподобно, повода связывать их нет. */
	if (g_mod == 0 || g_mod->memory == 0 || g_mod->tramp_base == 0)
		return 0;
	if (g_thread_info == 0)
		g_thread_info = g_mod->tramp_base + GUEST_TRAMP_SIZE
		              - THREAD_INFO_BYTES;
	p = (volatile u32 *)(uintptr_t)g_thread_info;
	/* Смещение 4 — адрес блока памяти гостя, который он передаёт в
	 * `sceKernelCpuSuspendIntr` (`0x092968F4`) и читает оттуда.
	 *
	 * Значение берётся у самого гостя, а не подставляется: подстановка
	 * была бы правдоподобной догадкой, которая в отчёте выглядит как
	 * результат. Замечено на плеере: при нуле в этом поле гость доходил
	 * дальше по цепочке вызовов импорта. */
	p[0] = g_suspend_arg;
	p[1] = 0;
	p[2] = 0;
	p[3] = 0;
	return g_thread_info;
}

static u32 guest_import_inner(u32 index, const u32 *args, u32 k0, u32 ra);

/* --- кадр гостя ---------------------------------------------------------- */

/* Последний кадр, заданный `sceDisplaySetFrameBuf`. */
static u32 g_fb_top;
static u32 g_fb_bw;
static u32 g_fb_fmt;
static u32 g_fb_count;
static u32 g_fb_vcount;
static u32 g_fb_dumps;        /* сколько PPM уже записано */

/* Нормализованный хостовый указатель на память гостя: VRAM или образ.
 * Зеркала снимаются маской — обе области отображены у нас в процессе
 * по своим адресам, поэтому нормализованный адрес ГОДИТСЯ как есть.
 * 0 — адрес вне известных областей. */
static const unsigned char *guest_fb_ptr(u32 addr)
{
	u32 a = addr & 0x3FFFFFFFu;

	if (g_mod == NULL || g_mod->memory == NULL)
		return NULL;
	if (a >= GUEST_VRAM_BASE
	    && a + 4u <= GUEST_VRAM_BASE + GUEST_VRAM_BYTES
	    && g_mod->vram_base != 0)
		return (const unsigned char *)(uintptr_t)a;
	if (a >= g_mod->base && a + 4u <= g_mod->base + g_mod->map_size)
		return (const unsigned char *)g_mod->memory
		       + (a - g_mod->base);
	return NULL;
}

/* Запись слова по адресу гостя: указатели приёмников `GetFrameBuf`
 * живут и в образе, и на стеке гостя (наш `.bss`), и в VRAM.
 *
 * Стеки НЕ нормализуются маской: их адреса лежат в `0x2CA…`, где
 * установлен бит 0x20000000 — маска зеркал `0x3FFFFFFF` испортила бы
 * такой адрес. Нормализация применяется только к образу и VRAM,
 * стеки проверяются по сырому адресу. */
static void guest_write_u32(u32 addr, u32 value)
{
	u32 a = addr & 0x3FFFFFFFu;

	if (g_mod == NULL || g_mod->memory == NULL)
		return;
	if (a >= g_mod->base && a + 4u <= g_mod->base + g_mod->map_size) {
		volatile u32 *p = (volatile u32 *)(g_mod->memory
		                                  + (a - g_mod->base));

		*p = value;
		return;
	}
	if (a >= GUEST_VRAM_BASE
	    && a + 4u <= GUEST_VRAM_BASE + GUEST_VRAM_BYTES
	    && g_mod->vram_base != 0) {
		volatile u32 *p = (volatile u32 *)(uintptr_t)a;

		*p = value;
		return;
	}
	/* Куча VPL гостя: область отображена у нас по фиксированному
	 * адресу, `a` уже нормализована маской (зеркало 0x49… снимается)
	 * и указывает в кэшируемый вид — ту же память. Граница —
	 * фактический размер области (лестница кандидатов в `map_vpl`). */
	if (g_mod->vpl_base != 0
	    && a >= g_mod->vpl_base
	    && a + 4u <= g_mod->vpl_base + g_mod->vpl_bytes) {
		volatile u32 *p = (volatile u32 *)(uintptr_t)a;

		*p = value;
		return;
	}
	if (addr >= (u32)(uintptr_t)&g_thread_stack[0]
	    && addr + 4u <= (u32)(uintptr_t)&g_thread_stack[0]
	                     + GUEST_STACK_BYTES) {
		*(volatile u32 *)(uintptr_t)addr = value;
		return;
	}
	if (addr >= (u32)(uintptr_t)&g_worker_stack[0][0]
	    && addr + 4u <= (u32)(uintptr_t)&g_worker_stack[0][0]
	                     + (u32)GUEST_THREAD_SLOTS
	                       * GUEST_WORKER_STACK_BYTES) {
		*(volatile u32 *)(uintptr_t)addr = value;
	}
}

/* Снимок кадра в PPM: `/tmp/run/psp_frame.ppm` (текущий) и
 * `/tmp/run/psp_fNNNN.ppm` для первых кадров. Высота — 272
 * (экран PSP), ширина — 480. stride — `bufferwidth` гостя.
 *
 * Форматы PSP: 0=5551, 1=565, 2=4444, 3=8888. */
static void frame_dump_ppm(void)
{
	static const char hdr_pre[] = "P6\n480 272\n255\n";
	const unsigned char *src;
	int fd;
	u32 x, y;
	u32 bpp = (g_fb_fmt == 3u) ? 4u : 2u;
	u32 stride = (g_fb_bw != 0u) ? g_fb_bw : 480u;
	u32 want_dump;

	if (g_fb_dumps >= 12u)
		return;
	want_dump = (g_fb_count <= 3u) || ((g_fb_count & 63u) == 0u);
	if (want_dump == 0u)
		return;

	src = guest_fb_ptr(g_fb_top);
	if (src == NULL)
		return;

	fd = plat_open_write("/tmp/run/psp_frame.ppm");
	if (fd < 0)
		return;
	{
		char hdr[16];
		u32 n = 0;

		while (hdr_pre[n] != '\0') {
			hdr[n] = hdr_pre[n];
			n++;
		}
		(void)plat_write(fd, hdr, n);
	}
	for (y = 0; y < 272u; y++) {
		static unsigned char row[480u * 3u];
		const unsigned char *srow = src + y * stride * bpp;

		for (x = 0; x < 480u; x++) {
			const unsigned char *p = srow + x * bpp;
			u32 c, r, g, b;

			if (g_fb_fmt == 3u) {
				r = p[0];
				g = p[1];
				b = p[2];
			} else {
				c = (u32)p[0] | ((u32)p[1] << 8);
				if (g_fb_fmt == 1u) {         /* 565 */
					r = ((c >> 0) & 0x1Fu) << 3;
					g = ((c >> 5) & 0x3Fu) << 2;
					b = ((c >> 11) & 0x1Fu) << 3;
				} else {                      /* 5551/4444 */
					r = ((c >> 0) & 0x1Fu) << 3;
					g = ((c >> 5) & 0x1Fu) << 3;
					b = ((c >> 10) & 0x1Fu) << 3;
				}
			}
			row[x * 3u + 0u] = (unsigned char)r;
			row[x * 3u + 1u] = (unsigned char)g;
			row[x * 3u + 2u] = (unsigned char)b;
		}
		(void)plat_write(fd, row, sizeof(row));
	}
	(void)plat_syscall(4006 /* close */, fd, 0, 0, 0);
	g_fb_dumps++;

	/* Пронумерованные снимки первых кадров — чтобы видно было
	 * последовательность, а не только последнее состояние. */
	if (g_fb_count <= 3u) {
		char path[] = "/tmp/run/psp_f0000.ppm";
		u32 v = g_fb_count;

		path[14] = (char)('0' + ((v / 1000u) % 10u));
		path[15] = (char)('0' + ((v / 100u) % 10u));
		path[16] = (char)('0' + ((v / 10u) % 10u));
		path[17] = (char)('0' + (v % 10u));
		fd = plat_open_write(path);
		if (fd < 0)
			return;
		{
			char hdr[16];
			u32 n = 0;

			while (hdr_pre[n] != '\0') {
				hdr[n] = hdr_pre[n];
				n++;
			}
			(void)plat_write(fd, hdr, n);
		}
		for (y = 0; y < 272u; y++) {
			static unsigned char row[480u * 3u];
			const unsigned char *srow =
			    src + y * stride * bpp;

			for (x = 0; x < 480u; x++) {
				const unsigned char *p = srow + x * bpp;
				u32 c, r, g, b;

				if (g_fb_fmt == 3u) {
					r = p[0];
					g = p[1];
					b = p[2];
				} else {
					c = (u32)p[0] | ((u32)p[1] << 8);
					r = ((c >> 0) & 0x1Fu) << 3;
					g = ((c >> 5) & 0x3Fu) << 3;
					b = ((c >> 11) & 0x1Fu) << 3;
				}
				row[x * 3u + 0u] = (unsigned char)r;
				row[x * 3u + 1u] = (unsigned char)g;
				row[x * 3u + 2u] = (unsigned char)b;
			}
			(void)plat_write(fd, row, sizeof(row));
		}
		(void)plat_syscall(4006 /* close */, fd, 0, 0, 0);
	}
}

/* Показ последнего кадра на экран плеера — вызывается из точки входа
 * плагина ПОСЛЕ отчёта: если конвейер упадёт, данные уже на диске.
 * Возвращает код шага `gfx_out_show` (0 = показан). */
u32 guest_run_show_last_frame(void);

u32 guest_run_show_last_frame(void)
{
	const unsigned char *src;

	if (g_fb_count == 0u)
		return 0xFFFFFF01u;
	src = guest_fb_ptr(g_fb_top);
	if (src == NULL)
		return 0xFFFFFF02u;
	/* stride зависит от формата: 8888 — 4 байта на пиксель,
	 * 16-битные форматы — 2. `bufferwidth` гостя — в пикселях. */
	{
		u32 bpp = (g_fb_fmt == 3u) ? 4u : 2u;
		u32 bw = (g_fb_bw != 0u) ? g_fb_bw : 480u;

		return (u32)gfx_out_show(src, 480u, 272u, bw * bpp,
		                         g_fb_fmt);
	}
}


/* Обёртка обработчика импортов.
 *
 * Единственная цель — записать возвращаемое значение в журнал. Тело
 * `guest_import_inner` имеет несколько `return`, и дописывать в каждый
 * значило бы надёжно забыть один из них, а потерянный возврат не видна: отчёт
 * выглядит правдоподобно, просто без значения. */
u32 guest_import(u32 index, const u32 *args, u32 k0, u32 ra)
{
	u32 r = guest_import_inner(index, args, k0, ra);

	if (g_run.last_slot < LOG_MAX)
		g_run.ret[g_run.last_slot] = r;
	return r;
}

/* Обработчик импортов: сюда приходят все вызовы гостя.
 *
 * Индекс — ключ ко всему: по нему восстанавливаются NID, номер трамплина и
 * адрес стаба. По NID обратного перехода нет: одна и та же функция встречается
 * в нескольких библиотеках, и по её названию адрес в госте не находится.
 * Поэтому в журнале пишется ИНДЕКС, а не имя. */
static u32 guest_import_inner(u32 index, const u32 *args, u32 k0, u32 ra)
{
	u32 nid;
	u32 t;

	/* `$k0` здесь НЕ печатается.
	 *
	 * Проба печатала, и из неё вышло ложное заключение «`$k0` ненулевой,
	 * значит причина не в нём». Это неверно: третий аргумент приходит из
	 * трамплина, который читает `$k0` из СВОЕГО кадра, а не из живой
	 * точки гостя, и печатает значение уже ПОСЛЕ того, как `$k0` мог быть
	 * затёрт. Значение `0x0019xxxx` — не регистр, а остаток кадра.
	 *
	 * Проверять `$k0` можно, только если он снят ДО всех наших вызовов,
	 * то есть из точки выхода гостя, а не из обработчика импорта. */
	/* Счётчик — ДО всех ранних выходов, намеренно.
	 *
	 * Он отвечает на измеренное противоречие: `calls=4`, а метка возврата
	 * трамплина стоит на индексе 84. Метка пишется ПОСЛЕ возврата из
	 * `guest_import`, значит трамплин 85-го импорта вернулся, не дойдя до
	 * обработчика. Если считать после проверки `index`, ранние выходы
	 * невидимы и противоречие неразличимо. */
	g_guest_calls++;
	if (index >= g_hle_count)
		return 0;
	/* Per-thread счётчик вызовов: по tid → слот.
	 * Замер 81: главный умер при g_guest_calls=290, но СКОЛЬКО
	 * вызовов сделал он — неизвестно (глобальный счётчик
	 * смешивает потоки). Это число печатает GTCL при GONE. */
	{
		u32 ctid = (u32)plat_gettid();
		u32 slot2;

		for (slot2 = 0; slot2 < GUEST_THREAD_SLOTS; slot2++) {
			if (g_thread.tid[slot2] != 0
			    && g_thread.tid[slot2] == ctid) {
				g_thread.calls[slot2]++;
				break;
			}
		}
	}
	if (g_run.calls >= CALL_LIMIT) {
		g_run.limit_hit = 1;
		g_guest_finished = 1;
		plat_thread_exit();          /* завершает только поток */
	}
	nid = g_hle_nid[index];
	if (g_log_fd >= 0 && g_run.calls < EARLY_LOG_MAX) {
		early_log("idx", index);
		early_log("nid", nid);
		early_log("ra ", ra);
		/* Прогон 77: главный игровой (0x08A9C958) и поток-
		 * получатель команд (0x08ACEB98) мертвы к первому
		 * тику семплера (GAT=0, ~2 с), БЕЗ единого следа
		 * (WDIED/SIGILLD/SPINDEAD нули). Ранний лог отвечает
		 * ЧТО вызывалось, но не КТО: без tid нельзя даже
		 * утверждать, что Font-инициализацию делал главный.
		 * Теперь каждый ранний вызов подписан потоком. */
		early_log("tid", (u32)plat_gettid());
		/* Это НЕ живое значение `$k0`, несмотря на то что тремплин
		 * передаёт регистр третьим аргументом.
		 *
		 * Измерено: трамплин, записывающий `$k0` сам и без C, дал
		 * `0x00000000`, а обработчик в том же прогоне напечатал
		 * `0x0018ED1E` и `0x001EB31E`. На MIPS o32 `$k0`/`$k1` входят в
		 * call-clobbered temporaries, и C-код обработчика (собранный
		 * `-fPIC`) затирает их через `lw $k0, %got(...)($gp)`.
		 *
		 * Поле оставлено, потому что мусор сам по себе полезен: по нему
		 * видно, что C-код отработал. Но как измерение `$k0` оно негодно —
		 * достоверно только то, что пишет трамплин (`tramp_k0` в отчёте). */
		early_log("k0h", k0);
	}
	g_run.calls++;

	/* Счётчик по индексу — для топ-NID в отчёте */
	if (index < NID_COUNT_SLOTS) {
		g_nid_count[index]++;
		g_nid_ra[index] = ra;
		g_nid_a0[index] = (args != NULL) ? args[0] : 0;
	}

	if (g_run.calls < LOG_MAX) {
		/* Индекс на единицу МЕНЬШЕ счётчика: счётчик уже увеличен выше, а
		 * нулевой слот отчёта — первый вызов. Со знаком «плюс» слот 0
		 * оставался пустым, и в отчёте первым шёл вызов с `nid=0` — то
		 * есть несуществующая запись выглядела настоящей. */
		t = g_run.calls - 1;
		g_run.nid[t] = nid;
		g_run.stub[t] = ra;
		g_run.arg0[t] = (args != NULL) ? args[0] : 0;
		g_run.arg1[t] = (args != NULL) ? args[1] : 0;
		/* Третий и четвёртый регистровые аргументы.
		 *
		 * Нужны, чтобы измерить, а не выдумать, семантику импортов: у
		 * `sceUtilityLoadModule` и `sceKernelLoadModule` она определяется
		 * тем, что гость делает с возвратом, а возврата без `$a0..$a3`
		 * не прочитать. */
		g_run.arg2[t] = (args != NULL) ? args[2] : 0;
		g_run.arg3[t] = (args != NULL) ? args[3] : 0;
		g_run.head[t] = atexit_head_node();
		g_run.last_slot = t;
	} else {
		g_run.log_overflow++;
		g_run.last_slot = 0xFFFFFFFFu;
		/* Последние вызовы: перезаписываемое кольцо. */
		{
			u32 ri = g_recent_slot;

			g_recent_nid[ri] = nid;
			g_recent_ra[ri] = ra;
			g_recent_a0[ri] = (args != NULL) ? args[0] : 0;
			g_recent_slot = (ri + 1u) % 32u;
			g_recent_count++;
		}
	}

	/* `sceKernelCreateThread(name, entry, ...)`.
	 *
	 * Запоминаем адрес входа и отдаём признак успеха — не ноль, потому что
	 * ноль означает «поток не создан», и гость на этом останавливается.
	 *
	 * Раскладка аргументов установлена измерением, а не разбором: `$a1`
	 * приходит равным `0x08ABCF68`, и это в точности адрес входа потока,
	 * который предсказывал статический разбор гостя. Пока `$a1` читался из
	 * слота, где лежал `$a2`, в отчёте стояло `0x00000020`, и по нему
	 * раскладку построить было нельзя — пришлось сначала вернуть честный
	 * слот и снять значение повторно. */
	if (nid == 0x446D8DE6u) {
		u32 ent = (args != NULL) ? args[1] : 0;
		static u32 ct_n;

		if (g_log_fd >= 0 && ct_n < 24) {
			ct_n++;
			early_log("CTX", (u32)plat_gettid());
			early_log("CTE", ent);
		}
		return thread_created(ent);
	}

	/* `sceKernelStartThread(id, ...)`: запускаем то, что создали.
	 *
	 * Аргументы печатаются: на PSP у этого вызова есть третий — указатель,
	 * который ядро кладёт в `$k0` потока. Если он есть, раскладка
	 * находится сразу и гадать не нужно; если все нули, значит `$k0`
	 * формируется иначе. */
	if (nid == 0xF475845Du) {
		/* Ядро PSP передаёт точке входа потка ДВА аргумента из этого
		 * вызова: `arglen` (`$a1`) и `argp` (`$a2`).
		 *
		 * Раскладка измерена по коду гостя, а не по документации:
		 * точка входа главного игрового потока (runtime 0x08A9C958)
		 * читает `$a0` как ДЛИНУ блока (`blez $a0` — пропустить разбор)
		 * и `$a1` как указатель на него, режет блок по '\0' в массив
		 * `argv[0..19]` на стеке и терминирует его нулём. А сам
		 * `module_start` (runtime 0x08A9C844, он же `e_entry`)
		 * пересылает СВОИ `(a0,a1)` в `StartThread` на месте
		 * `(a1,a2)` без изменений — эта цепочка и задаёт раскладку.
		 *
		 * Прежний код брал `$a2/$a3` — ошибка слотов: в `$a3` лежал
		 * застывший хвост от `CreateThread` (измерено: `a3=0x00040000`
		 * при `a1=a2=0`), а не аргумент. */
		g_thread.args = (args != NULL) ? args[1] : 0;
		g_thread.argp = (args != NULL) ? args[2] : 0;
		if (g_log_fd >= 0) {
			early_log("st_a0", (args != NULL) ? args[0] : 0);
			early_log("st_a1", (args != NULL) ? args[1] : 0);
			early_log("st_a2", (args != NULL) ? args[2] : 0);
			early_log("st_a3", (args != NULL) ? args[3] : 0);
			early_log("ent_a0", g_thread.args);
			early_log("ent_a1", g_thread.argp);
		}
		return thread_started();
	}

	/* `GetModuleIdByAddress` обязан вернуть УКАЗАТЕЛЬ на структуру: возврат
	 * нуля здесь стоил SIGSEGV в потоке гостя — он писал по `$k0`, а `$k0`
	 * равнялся нулю. Наполнить `$k0` кодом гостя не может: проверено
	 * инструментом по обеим исполняемым секциям, записи в `$k0` нет ни в
	 * одной инструкции. Значит значение приходит через возвращаемое
	 * значение импорта, и других путей нет. */
	if (nid == 0xD8B73127u)
		return thread_info_addr(args);

	/* `sceKernelCpuSuspendIntr`: единственный носитель адреса блока памяти
	 * гостя, который нужен структуре для `GetModuleIdByAddress`. */
	if (nid == 0x092968F4u) {
		if (args != NULL)
			g_suspend_arg = args[0];
		return 0;
	}

	/* `sceGeListEnQueue` (0xAB49E76A) / `sceGeListEnQueueHead` (0x1C0D95A6):
	 * гость сдаёт список команд GE. Разбираем его ходячим и печатаем
	 * классификацию примитивов — это измерение «2D или 3D» без эмуляции.
	 *
	 * Зеркала: у PSP одна память видна по ЧЕТЫРЁМ адресам — кэшируемое
	 * `0x08xxxxxx`, безкэшевое `0x48xxxxxx` (бит 30; гость сдаёт список
	 * GE именно через него и ПИШЕТ его туда же), KSEG0 `0x88xxxxxx`,
	 * KSEG1 `0xA8xxxxxx`. Зеркало отображено в `map_guest` вторым
	 * `MAP_SHARED` на общий файл, поэтому обращение гостя к `0x48…`
	 * работает нативно. Нормализация адресов при чтении остаётся в
	 * `ge_walk` — команды JUMP/CALL внутри списка могут ссылаться любым
	 * зеркалом.
	 *
	 * Прежнее разыменование сырого `0x48BA0E80` из `$a0` роняло наш же
	 * поток (измерено 2026-10-14: сбой в НАШЕМ коде, `pc=0x2CA07510`),
	 * а пропуск гостевых записей в `0x48…` терял данные и выращивал
	 * каскад из 66 сбоев, убивший весь `shell`. */
	if (nid == 0xAB49E76Au || nid == 0x1C0D95A6u) {
		if (g_log_fd >= 0) {
			early_log("GE_EnQ", (args != NULL) ? args[0] : 0);
			if (args != NULL && g_mod != NULL
			    && g_mod->memory != NULL) {
				GeList list;
				GeWalkResult r;
				/* Чтение — по ВСЕМУ отображению гостя с
				 * нормализацией зеркал (0x08/0x48/0x88/0xA8):
				 * списки GE строятся из кусков через JUMP/CALL,
				 * и стартовый адрес приходит любым зеркалом.
				 * Ходячий сам остановится на END, на выходе за
				 * отображение или по пределу шагов. */
				list.mem = (const u32 *)(uintptr_t)g_mod->memory;
				list.base = g_mod->base;
				list.size = g_mod->map_size;
				list.list_addr = args[0];
				if (ge_walk(&list, &r)) {
					early_log("p_spr", r.prim_sprites);
					early_log("p_3d", r.prim_3d);
					early_log("p_oth", r.prim_other);
					early_log("fin", r.finish);
					early_log("sig", r.signal);
					early_log("stp", r.steps);
				}
			}
		}
		return 0;
	}

	/* --- ДИСПЛЕЙ и GE: поверхность, кадры, синхронизация ------------
	 *
	 * Контракты сняты с прогонов (кольцо) и сверены с
	 * `data/psp_nids.tsv`; семантика аргументов `SetFrameBuf` —
	 * по формату вызова, измеренному в кольце, а не по памяти:
	 * a0=top, a1=bufferwidth, a2=pixelformat, a3=sync. */

	if (nid == 0xE47E40E4u)     /* sceGeEdramGetAddr */
		return (g_mod != NULL) ? g_mod->vram_base : 0;

	if (nid == 0x1F6752ADu)     /* sceGeEdramGetSize */
		return GUEST_VRAM_BYTES;

	if (nid == 0x36CDFADEu      /* sceDisplayWaitVblank */
	    || nid == 0x984C27E7u) { /* sceDisplayWaitVblankStart */
		/* Блокирующий vblank: кадр на PSP = 16.67 мс.
		 * Прогон 63 (сборка 0fce06d4): мгновенный возврат крутил
		 * таймерный цикл 0x088C17B0 на 180 тысяч/с —
		 * WaitVblank/DelayThread/GetTimeWide по 4.5M (ровно
		 * равные числа = три в одной итерации цикла) и
		 * съедали лимит 50M. На PSP это блокирующая
		 * синхронизация, поток кадрируется до 60 Гц. */
		plat_sleep_ms(16);
		return 0;
	}

	if (nid == 0x0E20F177u      /* sceDisplaySetMode */
	    || nid == 0x03444EB4u   /* sceGeListSync */
	    || nid == 0xB287BD61u   /* sceGeDrawSync */
	    || nid == 0x4C06E472u   /* sceGeContinue */
	    || nid == 0xB448EC0Du   /* sceGeBreak */
	    || nid == 0xE0D68148u)  /* sceGeListUpdateStallAddr */
		return 0;

	if (nid == 0xA4FC06A4u)     /* sceGeSetCallback — фальшивый id */
		return 1;

	if (nid == 0x05DB22CEu)     /* sceGeUnsetCallback */
		return 0;

	if (nid == 0x9C6EAAD7u)     /* sceDisplayGetVcount */
		return ++g_fb_vcount;

	if (nid == 0x289D82FEu) {   /* sceDisplaySetFrameBuf */
		u32 top = (args != NULL) ? args[0] : 0;
		u32 bw  = (args != NULL) ? args[1] : 0;
		u32 fmt = (args != NULL) ? args[2] : 0;

		g_fb_top = top;
		g_fb_bw = bw;
		g_fb_fmt = fmt;
		g_fb_count++;
		/* Снимок кадра в PPM — дешёвый и безопасный путь: файлы
		 * на tmpfs видны глазами и без экрана. Частота —
		 * первые кадры и каждый 64-й, лимит файлов жёсткий. */
		frame_dump_ppm();
		return 0;
	}

	if (nid == 0xEEDA2E54u) {   /* sceDisplayGetFrameBuf */
		/* Аргументы — УКАЗАТЕЛИ гостя на четыре приёмника.
		 * Пишем последние значения из `SetFrameBuf`. */
		if (args != NULL) {
			guest_write_u32(args[0], g_fb_top);
			guest_write_u32(args[1], g_fb_bw);
			guest_write_u32(args[2], g_fb_fmt);
			guest_write_u32(args[3], 0);       /* sync */
		}
		return 0;
	}

	/* `sceKernelAllocPartitionMemory`: размер в четвёртом аргументе. Замер
	 * реального вызова: `a0=2` (раздел), `a1="UserSbrk"`, `a2=3` (тип),
	 * `a3=0x4000` (размер), пятый аргумент `4096` (выравнивание). Отсюда
	 * берётся `args[3]`; выравнивание задаёт арена (64), а не гость —
	 * буферы списков GE приходят сюда же. Подробнее — см. комментарий
	 * над `part_alloc`. */
	if (nid == 0x237DBD4Fu) {
		if (args == NULL)
			return 0;
		return part_alloc(args[3]);
	}

	/* `sceKernelGetBlockHeadAddr`: десриптор в первом аргументе. Возврат —
	 * указатель внутрь арены; ноль означает отказ и ведёт гостя в тот же
	 * `exit(1)`, что и раньше, поэтому неверный дескриптор лучше считать
	 * отказом, чем выдавать чужой адрес. */
	/* `sceKernelLoadModule` (0x977DE386): путь лежит по `$a0`, и снимок
	 * этого пути — главный прибор прогона. Ожидание выведено из кода
	 * гостя, а не придумано: `F` (runtime 0x088C11FC) строит путь
	 * `strcat`-ом на НУЛЕВОМ буфере своего стека (первый байт свежего
	 * стека равен нулю — потому `strcat` работает как `strcpy`) и
	 * берёт имя из глобали `0x327F00 -> "psmf.prx"`.
	 *
	 * Чтение ТОЛЬКО по проверенному адресу: необследованный `p[k]` —
	 * падение потока, а проверка стоит четыре сравнения. Разрешены образ
	 * гостя и оба его стека — всё это отображено в нашем процессе.
	 *
	 * Возврат — ПОЛОЖИТЕЛЬНЫЙ дескриптор: гость проверяет результат
	 * командой `bgtz $v0` (0x088C13E0), и ноль уводит в ветку
	 * «*** failed to sceKernelLoadModule %s[%X]» с собственным `printf`
	 * гостя, которого у нас нет. */
	if (nid == 0x977DE386u) {
		u32 a = (args != NULL) ? args[0] : 0;
		u64 end = (u64)a + LM_SNAP_WORDS * 4u;
		u32 where = 0;
		u32 k;

		if (g_mod != NULL) {
			if (a >= g_mod->base
			    && end <= (u64)g_mod->base + g_mod->map_size)
				where = 1;
			else if (a >= (u32)(uintptr_t)g_thread_stack
			         && end <= (u64)(uintptr_t)g_thread_stack
			                  + GUEST_STACK_BYTES)
				where = 2;
			else if (a >= (u32)(uintptr_t)&g_worker_stack[0][0]
			         && end <= (u64)(uintptr_t)&g_worker_stack[0][0]
			                  + (u64)GUEST_THREAD_SLOTS
			                    * GUEST_WORKER_STACK_BYTES)
				where = 2;
		}
		g_lm_count++;
		g_lm_where[g_lm_slot] = where;
		for (k = 0; k < LM_SNAP_WORDS; k++)
			g_lm_words[g_lm_slot][k] = 0;
		if (where != 0) {
			const u32 *p = (const u32 *)(uintptr_t)a;

			for (k = 0; k < LM_SNAP_WORDS; k++)
				g_lm_words[g_lm_slot][k] = p[k];
		}
		g_lm_slot = (g_lm_slot + 1u) % LM_SNAP_SLOTS;
		return ++g_lm_uid + 0x100u;
	}

	/* `sceUtilityLoadModule(utility_type, ...)`: возвращает дескриптор
	 * загруженного утилиты-модуля. Игра зовит его 6 раз с типами
	 * 0x100, 0x101, 0x300-0x303; возврат 0 означает отказ, и дальше
	 * игра использует 0 как дескриптор — получает NULL при поиске
	 * модуля и падает на цепочке нулевых указателей. */
	if (nid == 0x2A2B3DE0u) {
		return ++g_lm_uid + 0x200u;
	}

	if (nid == 0x9D9A5BA1u) {
		if (args == NULL)
			return 0;
		return part_head(args[0]);
	}

	/* --- КУЧА VPL: MaxFreeMemSize и семейство -------------------------
	 *
	 * NID — из `data/psp_nids.tsv`, контракты — из замера прогона 35:
	 * `CreateVpl(name, partitionid, attr, size, opt)` получает `size`,
	 * построенный гостем из ответа `MaxFreeMemSize`:
	 * `(MaxFree − 0x100000) & ~0x3F`.
	 * `ReferVplStatus(uid, info)` — гость читает `freeSize` по
	 * `info+0x2C` (`s7 = *(sp+0x2C) − 0x10`) и из него вычитает
	 * размеры своих куч.
	 * `AllocateVpl*(uid, size, void **addr, u32 *timeout)` — адрес
	 * блока обязан попасть в ПРИЁМНИК: пропущенная запись и была
	 * сбоем прогона 35. `timeout` не пишется — настоящее ядро
	 * заполняет его только при ошибке ожидания, а у нас ожидания нет. */

	if (nid == 0xA291F107u) {   /* sceKernelMaxFreeMemSize */
		/* Остаток области, а не константа: настоящее ядро отвечает
		 * размером СВОБОДНОЙ памяти, и после созданного пула он
		 * меньше. Фактический размер области меняется от прогона к
		 * прогону (лестница кандидатов в `map_vpl`), и гость строит
		 * размеры своих куч ИЗ ОТВЕТА — то есть подстраивается. */
		if (g_mod == NULL || g_mod->vpl_base == 0)
			return 0;
		return g_mod->vpl_bytes - g_vpl.off;
	}

	if (nid == 0x56C039B5u) {   /* sceKernelCreateVpl */
		u32 size = (args != NULL) ? args[3] : 0;
		u32 span = (size + (VPL_ALIGN - 1u)) & ~(VPL_ALIGN - 1u);
		u32 start = (g_vpl.off + (VPL_ALIGN - 1u)) & ~(VPL_ALIGN - 1u);
		u32 slot;

		for (slot = 0; slot < VPL_SLOTS; slot++) {
			if (g_vpl.size[slot] != 0)
				continue;
			/* Ёмкость области, а не «наверняка поместится»: при
			 * нулевом `MaxFreeMemSize` гость просит 0xFF000000
			 * (переполнение `MaxFree − 0x100000`), и такой
			 * запрос обязан отвергаться, а не съедать курсор.
			 * Граница — фактический размер области, он меняется
			 * от прогона к прогону. */
			if (size == 0
			    || (u64)start + span > (u64)g_mod->vpl_bytes)
				break;
			g_vpl.size[slot] = size;
			g_vpl.attr[slot] = (args != NULL) ? args[2] : 0;
			g_vpl.start[slot] = start;
			g_vpl.used[slot] = 0;
			vpl_snap_name(g_vpl.name[slot],
			              (args != NULL) ? args[0] : 0);
			g_vpl.off = start + span;
			g_vpl.created++;
			return slot + 1;
		}
		g_vpl.fails++;
		return 0;
	}

	if (nid == 0xBED27435u      /* sceKernelAllocateVpl */
	    || nid == 0xAF36D708u   /* sceKernelTryAllocateVpl */
	    || nid == 0xEC0A693Fu) {  /* sceKernelAllocateVplCB */
		u32 block = vpl_alloc_block((args != NULL) ? args[0] : 0,
		                            (args != NULL) ? args[1] : 0);

		/* Приёмник обнуляется В ЛЮБОМ случае: пропущенная запись —
		 * это сбой прогона 35 в чистом виде, гость читает по
		 * неинициализированному адресу мусор вместо базы кучи. */
		if (args != NULL)
			guest_write_u32(args[2], block);
		if (block != 0) {
			g_vpl.allocs++;
			if (g_log_fd >= 0)
				early_log("vplA", block);
		} else {
			g_vpl.fails++;
		}
		/* ВОЗВРАЩАЕМ УКАЗАТЕЛЬ, НЕ 0.
		 *
		 * На PSP AllocateVpl/CB возвращает базу блока в `$v0`.
		 * Прежний `return 0` давал гостю NULL: он читал `$v0` как
		 * указатель и писал в `NULL | 0x40000000` (uncached-бит) —
		 * это ровно те 129 memset-фолтов на 0x40000000-0x4000007B,
		 * которые фолт-хендлер «успешно» пропускал, теряя данные.
		 * Отыскано трассировкой GJRA: главный гиб после
		 * AllocateVplCB → журнал → memset(NULL+uncached) →
		 * чтение потерянных нулей → смерть в чистом коде. */
		return block;
	}

	if (nid == 0x39810265u) {   /* sceKernelReferVplStatus */
		u32 uid = (args != NULL) ? args[0] : 0;
		u32 info = (args != NULL) ? args[1] : 0;
		/* Неверный uid не читает чужой слот: обе альтернативы ниже
		 * вычисляют `g_vpl.*[uid−1]` только при valid. */
		u32 valid = (uid != 0 && uid <= VPL_SLOTS
		             && g_vpl.size[uid - 1u] != 0);
		u32 pool = valid ? g_vpl.size[uid - 1u] : 0;
		u32 free_size = valid ? pool - g_vpl.used[uid - 1u] : 0;
		u32 i;
		u32 k;
		u32 w;

		/* `SceKernelVplInfo` (0x34 Б): size 0x00, name[32] 0x04,
		 * attr 0x24, poolSize 0x28, freeSize 0x2C,
		 * numWaitThreads 0x30. freeSize — единственное поле,
		 * которое гость заведомо читает (замер прогона 35). */
		if (info == 0)
			return 0;
		guest_write_u32(info + 0x00, 0x34);
		for (i = 0; i < 8; i++) {
			w = 0;
			for (k = 0; k < 4; k++) {
				unsigned char c = 0;

				if (valid)
					c = g_vpl.name[uid - 1u][4 * i + k];
				w |= (u32)c << (8 * k);
			}
			guest_write_u32(info + 0x04 + 4 * i, w);
		}
		guest_write_u32(info + 0x24,
				valid ? g_vpl.attr[uid - 1u] : 0);
		guest_write_u32(info + 0x28, pool);
		guest_write_u32(info + 0x2C, free_size);
		guest_write_u32(info + 0x30, 0);       /* numWaitThreads */
		return 0;
	}

	if (nid == 0xB736E9FFu) {   /* sceKernelFreeVpl — бамп без освобождения */
		g_vpl.frees++;
		return 0;
	}

	if (nid == 0x89B3D48Cu) {   /* sceKernelDeleteVpl */
		u32 uid = (args != NULL) ? args[0] : 0;

		/* Слот освобождается, курсор области — нет: как у
		 * `part_alloc`, освобождение без записи в отчёт не
		 * распознаешь, а тут счётчик `vpl_created=` не растёт
		 * повторно только потому, что слот один и тот же. */
		if (uid != 0 && uid <= VPL_SLOTS)
			g_vpl.size[uid - 1u] = 0;
		return 0;
	}

	if (nid == 0x1D371B8Au)     /* sceKernelCancelVpl */
		return 0;

	if (nid == 0xD6DA4BA1u) {   /* sceKernelCreateSema(name,attr,init,cur) */
		u32 slot;

		for (slot = 0; slot < SEMA_SLOTS; slot++) {
			if (g_sema[slot].used == 0) {
				g_sema[slot].used = 1;
				g_sema[slot].count =
				    (args != NULL) ? args[3] : 0;
				g_sema[slot].max = 0xFFFFFFFFu;
				g_sema_created++;
				if (g_sema_created <= 16u && g_log_fd >= 0) {
					early_log("CSN", slot + 1u);
					early_log("CSR", ra);
				}
				return slot + 1u;   /* uid > 0 ВСЕГДА */
			}
		}
		return 0x8002017Fu;   /* SCE_KERNEL_ERROR_NO_UID */
	}

	if (nid == 0x28B6489Cu) {   /* sceKernelDeleteSema */
		if (args != NULL && sema_uid_valid(args[0]))
			g_sema[args[0] - 1u].used = 0;
		return 0;
	}

	if (nid == 0x3F53E640u) {   /* sceKernelSignalSema */
		if (args == NULL || !sema_uid_valid(args[0]))
			return (s32)0x800201A1u;   /* UNKNOWN_UID */
		g_sema[args[0] - 1u].count++;
		return 0;
	}

	if (nid == 0x4E3A1105u) {   /* sceKernelWaitSema — БЛОКИРОВАТЬ */
		if (args == NULL || !sema_uid_valid(args[0]))
			return (s32)0x800201A1u;   /* UNKNOWN_UID */
		/* Внутри тик-обработчика блокировать НЕЛЬЗЯ: на PSP это
		 * контекст прерывания (замер 98 — паркер и семплер
		 * заморожены таким ожиданием). Отвечаем мгновенно. */
		if (g_in_si_fire != 0) {
			if (g_sema[args[0] - 1u].count == 0)
				return (s32)0x800201ADu;   /* SEMA_ZERO */
			g_sema[args[0] - 1u].count--;
			return 0;
		}
		{
			u32 uid = args[0];
			u32 waited = 0;
			static u32 wsw_n;

			g_sema_waited++;
			/* Замер 108: семафор звукового движка — ожидание
			 * с ra=0x08AB60D0 (зона SAS/грейнов). На PSP его
			 * поднимает аудио-прерывание по завершении буфера;
			 * наш насос поднимает его с периодом грейна. */
			if (ra == 0x08AB60D0u)
				g_sound_sema = uid;
			if (g_log_fd >= 0 && wsw_n < 32) {
				wsw_n++;
				early_log("WSW", uid);
				early_log("WST", (u32)plat_gettid());
				early_log("WSR", ra);
			}
			while (g_sema[uid - 1u].count == 0) {
				if (g_time_to_exit != 0)
					return (s32)0x800201A1u;
				if (waited >= SEMA_STUCK_MS) {
					g_sema_stuck++;
					return (s32)0x800201A1u;
				}
				plat_sleep_ms(SEMA_PARK_MS);
				waited += SEMA_PARK_MS;
			}
			g_sema[uid - 1u].count--;
			return 0;
		}
	}

	if (nid == 0x58B1F937u) {   /* sceKernelPollSema */
		if (args == NULL || !sema_uid_valid(args[0]))
			return (s32)0x800201A1u;
		if (g_sema[args[0] - 1u].count == 0)
			return (s32)0x800201ADu;   /* SEMA_ZERO */
		g_sema[args[0] - 1u].count--;
		return 0;
	}

	if (nid == 0x293B45B8u) {   /* sceKernelGetThreadId */
		/* Замер 90 (652ee635): прежний возврат `args[0]` — ЭХО
		 * случайного `$a0` вызывающего. У обёртки 0x08AB69A8
		 * аргументов нет, и в `$a0` оставалось всё, что было:
		 * известен случай 0xC6600BC0 (kseg2 — вообще не
		 * пользовательский адрес), который игра унесла в свои
		 * структуры. По контракту PSP вызов возвращает uid
		 * ТЕКУЩЕГО потока — ищем слот по tid. Не нашли
		 * (module_start-поток создан нами, не игрой) — 1. */
		{
			u32 now = (u32)plat_gettid();
			u32 gslot;

			for (gslot = 0; gslot < GUEST_THREAD_SLOTS; gslot++) {
				if (g_thread.tid[gslot] != 0
				    && g_thread.tid[gslot] == now)
					return g_thread.uid[gslot];
			}
		}
		return 1;
	}

	if (nid == 0x55C20A00u) {   /* sceKernelCreateEventFlag */
		u32 slot;

		for (slot = 0; slot < EF_SLOTS; slot++) {
			if (g_ef.size[slot] != 0)
				continue;
			g_ef.size[slot] = 1;
			g_ef.attr[slot] = (args != NULL) ? args[1] : 0;
			g_ef.init[slot] = (args != NULL) ? args[2] : 0;
			vpl_snap_name(g_ef.name[slot],
			              (args != NULL) ? args[0] : 0);
			g_ef.created++;
			return slot + 1;
		}
		return 0;
	}

	if (nid == 0x37FB5C42u) {   /* sceKernelGetGPI */
		/* ВОЗВРАЩАЕМ 0 = «HOLD не нажат, UMS-слот пуст».
		 *
		 * Прежняя заглушка `args[3] - 10340` читала ЧЕТВЁРТЫЙ
		 * аргумент вызова, которого у GetGPI НЕТ, — то есть
		 * мусор из кадра тримплина. Журнал-функция гостя
		 * (0x088C16E4) проверяет бит 0 = HOLD: при нечётном
		 * мусоре журнал уходил в ветку ПЕЧАТИ (vsnprintf +
		 * выгрузка через глобаль 0x3679DC), и поток гиб там
		 * без следа. Прогон 78, последние вызовы обоих
		 * погибших потоков — GetGPI из журнала (ra=
		 * 0x088C1714): главный прожил три чётных вызова
		 * (журнал молчал) и умер на четвёртом. На PSP без
		 * зажатого HOLD GetGPI возвращает «hold off». */
		/* ЖУРНАЛ-ТРАССИРОВКА: ra=088C1714 — вызов ИЗ журнала.
		 * Кто ВЫЗВАЛ журнал: в прологе журнала `sw ra,12(sp)`
		 * — адрес возврата в вызывающего лежит по
		 * guest_sp+12. Тримплин кладёт args[] на
		 * guest_sp-64+8, значит guest_sp = args + 56.
		 * Первые 4 журнала печатаем ra вызова — точка,
		 * после которой поток гибнет. */
		if (ra == 0x088C1714u) {
			static u32 gj_n;
			if (g_log_fd >= 0 && gj_n < 32) {
				u32 gsp = (u32)(uintptr_t)args + 56u;

				/* Стек ГОСТЯ (g_thread_stack/g_worker_stack) —
				 * В НАШЕМ .so, адрес 0x2Dxxxxxx — а НЕ в
				 * отображении образа гостя. Прошлая проверка
				 * `gsp < base+map_size` его НЕ ПРОПУСКАЛА
				 * (замер 83: GJRA пусто при 58 GetGPI).
				 * Читаем ПРЯМЫМ указателем.
				 *
				 * gj_n < 32: в цикле загрузки модулей 6
				 * журналов, в init — ещё ~10, и ФИНАЛЬНЫЕ
				 * три — которые нас интересуют. 32 покрывает
				 * все с запасом. */
				if (gsp > 0x1000u && gsp < 0x80000000u) {
					u32 jra = *(const u32 *)
					    (uintptr_t)(gsp + 12);
					early_log("GJRA", jra);
					gj_n++;
				}
			}
		}
		return 0;
	}

	if (nid == 0x52089CA1u) {   /* sceKernelGetThreadStackFreeSize */
		/* Дефолтная заглушка отдавала 0 — «стек ИСЧЕРПАН».
		 * Поток-получатель команд (entry 0x08ACEB98) после
		 * WaitSema позвал это (замер 78: ra=0x08ACED80,
		 * a0=0), затем журнал — и умер: для потока пустой
		 * стек — законная причина паниковать. Отдаём живое
		 * число — свободную половину воркер-стека. */
		return GUEST_WORKER_STACK_BYTES / 2u;
	}

	if (nid == 0x82BC5777u) {   /* sceKernelGetSystemTimeWide */
		/* Возвращаем микросекунды сboot ПСЕВДО-времени.
		 * На PSP это u64, но возвращаем u32 — младшую половину.
		 * Без растущего времени игра ждёт вечно: все таймеры
		 * и проверки «прошло ли время» дают 0 - 0 = 0.
		 * Счётчик увеличивается на каждый вызов ~1000 мкс,
		 * имитируя реальное время. */
		g_time_us += 1000u;
		return g_time_us;
	}

	if (nid == 0x1F803938u      /* sceCtrlReadBufferPositive */
	    || nid == 0x3A622550u) {  /* sceCtrlPeekBufferPositive */
		/* Буфер контроллера: SceCtrlData { u32 timestamp;
		 * u32 buttons; u8 x[2]; u8 y[2]; } — 16 байт.
		 * Возврат 1 = прочитан 1 буфер.
		 * Buttons: 0 = ничего не нажато, кнопка START = 0x8000.
		 * Сначала 2 секунды ничего, потом START — как будто
		 * игрок нажал кнопку. */
		if (args != NULL && args[0] != 0) {
			u32 buf = args[0];
			if (g_ctrl_buttons != 0) {
				guest_write_u32(buf + 4, g_ctrl_buttons);
				guest_write_u32(buf + 8, 0);
				guest_write_u32(buf + 12, 0);
			} else {
				guest_write_u32(buf + 4, 0);
				guest_write_u32(buf + 8, 0);
				guest_write_u32(buf + 12, 0);
			}
			guest_write_u32(buf, g_time_us);
		}
		/* Через 5M вызовов нажимаем START (0x8000) */
		if ((u32)g_guest_calls > 5000000u && g_ctrl_buttons == 0) {
			g_ctrl_buttons = 0x8000u;
		}
		return 1;
	}

	if (nid == 0x0B588501u) {   /* sceCtrlReadLatch */
		/* Возвращает u32: битовое поле нажатых кнопок.
		 * Возврат 0 = ничего не нажато. */
		return g_ctrl_buttons;
	}

	if (nid == 0xE81CAF8Fu) {   /* sceKernelCreateCallback */
		u32 uid = ++g_cb.created;
		/* Сохраняем гостевую функцию (a1) и common (a2):
		 * ядро PSP зовёт func(common, notifyArg) из контекста
		 * спящего *CB-потока. Замер: игра передаёт uid в
		 * UmdRegisterUMDCallBack/PowerRegisterCallback/devctl. */
		if (uid < CBK_MAX) {
			g_cbk_func[uid] = (args != NULL) ? args[1] : 0;
			g_cbk_arg[uid] = (args != NULL) ? args[2] : 0;
		}
		return uid;
	}

	if (nid == 0x50F0C1ECu) {   /* sceKernelStartModule — модуль запущен, возврат uid > 0 */
		return ++g_lm_uid + 0x100u;
	}

	if (nid == 0x82826F70u) {   /* sceKernelSleepThreadCB */
		/* PSP: уснуть до WakeupThread, исполняя отложенные
		 * колбеки в контексте ЭТОГО потока. ptnCallbackTh
		 * Локо Роко 2 живёт в этом вызове всю игру: колбеки
		 * power/UMD/MS — единственный канал внешних событий,
		 * а init-мастер стоит после Font-инициализации без
		 * них (замер 69: init-done=0, g_qhead=0).
		 *
		 * История двух крайностей:
		 *   - возврат через 10 мс (сборки 102805e6+) —
		 *     диспетчер шёл в ExitDeleteThread и умирал;
		 *   - вечный цикл (сборка bc5641ea) — dlcome
		 *     выгружал библиотеку под живым потоком →
		 *     рестарт-луп shell.
		 *
		 * Рабочий компромисс: паркуемся чанками ПОКА плагин
		 * ждёт гостя. guest_run_wait перед возвратом хосту
		 * ставит g_time_to_exit=1 и даёт циклу время
		 * выйти: поток возвращается и честно умирает в
		 * ExitDeleteThread-заглушке. dlclose чист. */
		while (g_time_to_exit == 0) {
			/* A/B 2026-10-21: доставка колбеков ВЫКЛЮЧЕНА.
			 *
			 * Поиск убийцы главного потока: прогоны 74–76 —
			 * главный игровой стабильно мёртв БЕЗ ЕДИНОГО
			 * следа. Подозреваемый №1 — наш cbk_deliver():
			 * он ПРЯМЫМ вызовом исполняет гостевые powerCB/
			 * umdCB/msCB в контексте ptnCallbackTh В МОМЕНТ,
			 * когда init-мастер ещё работает, и мог бы портить
			 * состояние (powerCB безусловно зовёт журнал-
			 * функцию 0x088C0DE4 и трогает 0x327FF4/95).
			 * Прогон без доставки: главный выжил → виноват
			 * deliver; снова мёртв → копаем глубже. cb_fired
			 * в отчёте должен остаться 0. */
			/* Замер 101: доставка ВКЛЮЧЕНА. Выключалась в A/B
			 * 2026-10-21 по подозрению в убийстве главного
			 * потока — подозрение снято замерами 97-100
			 * (убийцей были семафоры с uid 0 и пустая
			 * sparse-таблица). А уведомление UMD «диск готов»
			 * (arg 0x04000000) стоит в очереди с самого
			 * старта, и игра, запускающаяся с disc0:, без
			 * него не пойдёт по ветке загрузки контента. */
			cbk_deliver();
			/* ДРЕНАЖНЫЙ КЛАПАН (замер 100): если семплер трижды
			 * видел цикл дренажа 0x08AB43C8..EC — игра стоит в
			 * livelock из-за пустой sparse-таблицы голосов.
			 * Один раз заполняем её минимально-валидной
			 * записью (массив part_alloc 64К + один 316-байт
			 * подобъект, count=1) — тик 0x08AB6228 получит
			 * элемент, путь завершения 0x08AB2CB8 декрементирует
			 * счётчик, дренаж пойдёт. */
			if (g_drain_spin >= 3u && g_drain_valve == 0
			    && g_mod != 0 && g_mod->memory != 0) {
				u32 arr = part_head(part_alloc(64u * 1024u));
				u32 sub = part_head(part_alloc(0x13Cu));
				u32 tbl = 0x08BAEFDCu;

				if (arr != 0 && sub != 0
				    && tbl > g_mod->base
				    && tbl + 16u < g_mod->base + g_mod->map_size) {
					unsigned char *mem =
					    (unsigned char *)g_mod->memory;

					*(u32 *)(mem + tbl + 4u - g_mod->base) = arr;
					*(u32 *)(mem + tbl + 8u - g_mod->base) = 1u;
					*(u32 *)(mem + arr - g_mod->base + 4u) = sub;
					*(u32 *)(mem + arr - g_mod->base + 8u) = 0;
					*(u32 *)(mem + arr - g_mod->base + 12u) = 0;
					g_drain_valve = 1u;
					early_log("DVA", arr);
					early_log("DVS", sub);
				}
				g_drain_spin = 0;
			}
			cbk_sample();
			plat_sleep_ms(15);
		}
		return 0;
	}

	if (nid == 0x9ACE131Eu) {   /* sceKernelSleepThread */
		/* Прогон 80: семпля главного потока показывают его
		 * ВНУТРИ plat_sleep_ms (0x668C вход / 0x66E4 возврат
		 * из nanosleep) — ПОСЛЕ того как ранний лог (1000
		 * записей) исчерпан и его вызовы НЕВИДИМЫ.
		 * Значит главный жил после журнала×3 и КРУТИЛ
		 * цикл с парком. Какой парк — расшифровываем
		 * первыми 16 вызовами SleepThread С TID. */
		static u32 dbg_n;
		if (g_log_fd >= 0 && dbg_n < 16) {
			dbg_n++;
			early_log("STX", (u32)plat_gettid());
			early_log("STR", ra);
		}
		plat_sleep_ms(10);
		return 0;
	}

	if (nid == 0x809CE29Bu) {   /* sceKernelExitDeleteThread */
		/* PSP: завершить И УДАЛИТЬ поток, вызов НЕ
		 * возвращается. Прежняя «заглушка 0» позволяла
		 * потоку брести по коду ПОСЛЕ точки смерти:
		 * замер 69 — главный поток вышел из wait_forever
		 * и растворился в безимпортном блуждании. */
		plat_thread_exit();
		return 0;   /* недостижимо */
	}

	if (nid == 0x04B7766Eu) {   /* scePowerRegisterCallback(a0=slot, a1=cb uid) */
		u32 uid = (args != NULL) ? args[1] : 0;
		/* Событие «питание активно, не suspended»: гость в
		 * powerCB (0x088C28A8) сбросит флаг 0x327FF4 при
		 * arg & 0x04000000. Уведомление дойдёт, когда
		 * ptnCallbackTh войдёт в SleepThreadCB. */
		cbk_notify(uid, 0x04000000u);
		return 0;
	}

	if (nid == 0xAEE7404Du) {   /* sceUmdRegisterUMDCallBack(a0=cb uid) */
		u32 uid = (args != NULL) ? args[0] : 0;
		/* «UMD READABLE»: umdCB гостя (0x088C2928) реагирует
		 * только на arg & 0x0D (диск ДОСТАЛИ) — 0x20 для него
		 * тишина, как и должно быть при готовом диске. */
		cbk_notify(uid, 0x00000020u);
		return 0;
	}

	if (nid == 0x54F5FB11u) {   /* sceIoDevctl */
		/* Регистрация MS-notify: devctl("fatms0:",
		 * 0x2415821, &cbUid, 4, ...) — замер ra=0x088C2A60,
		 * a2 = указатель на uid колбека. MsCB гостя пуст
		 * (0x088C2954: `jr ra; move v0,zero`) — уведомление
		 * безвредно, но механика должна быть честной. */
		if (args != NULL && args[1] == 0x2415821u
		    && args[2] != 0 && g_mod != NULL) {
			u32 p = args[2];
			if (p >= g_mod->base
			    && p + 4 <= g_mod->base + g_mod->map_size) {
				u32 uid = *(u32 *)(g_mod->memory
						   + (p - g_mod->base));
				cbk_notify(uid, 0x00000001u);   /* MS inserted */
			}
		}
		return 0;
	}

	if (nid == 0x4AC57943u)     /* sceKernelRegisterExitCallback — только при выходе */
		return 0;

	if (nid == 0x71BC9871u)     /* sceKernelChangeThreadPriority */
		return 0;

	if (nid == 0xEA748E31u)     /* sceKernelChangeCurrentThreadAttr */
		return 0;

	/* --- ШРИФТОВАЯ БИБЛИОТЕКА: семейство sceLibFont ---------------
	 *
	 * Контракты — по JPCSP (локальный эталон, см. комментарий у
	 * `g_font`). Последовательность гостя измерена прогоном 36:
	 * NewLib → GetNumFontList → падение на нулевом хэндле. */

	if (nid == 0x67F17ED7u) {   /* sceFontNewLib(params, errPtr) */
		u32 block = font_obj_block();
		const unsigned char *p;
		unsigned char *h;
		u32 i;

		if (block == 0)
			return 0;
		/* Хэндл несёт копию `SceFontLibData` (44 Б): гость читает
		 * из ХЭНДЛА maxFonts(+4) и alloc(+0xC) — замер прогона 36.
		 * Блок уже обнулён `font_obj_block`. */
		h = g_mod->memory + (block - g_mod->base);
		p = guest_read_ptr((args != NULL) ? args[0] : 0,
		                   FONT_LIBDATA_BYTES);
		if (p != NULL) {
			for (i = 0; i < FONT_LIBDATA_BYTES; i++)
				h[i] = p[i];
		}
		if (args != NULL && args[1] != 0)
			guest_write_u32(args[1], 0);       /* errPtr */
		g_font.libs++;
		if (g_log_fd >= 0)
			early_log("fntN", block);
		return block;
	}

	if (nid == 0x27F6E642u) {   /* sceFontGetNumFontList(lib, errPtr) */
		if (args != NULL && args[1] != 0)
			guest_write_u32(args[1], 0);       /* errPtr */
		return 1;   /* внутренних шрифтов один: jpn0.pgf */
	}

	if (nid == 0xBC75D85Bu) {   /* sceFontGetFontList(lib, style, num) */
		u32 style[FONT_STYLE_WORDS];
		u32 num = (args != NULL) ? args[2] : 0;
		u32 dst = (args != NULL) ? args[1] : 0;

		if (dst == 0)
			return 0;
		if (num > 1)
			num = 1;
		if (num != 0) {
			font_style_words(style);
			font_put_words(dst, style, FONT_STYLE_WORDS);
		}
		return 0;
	}

	if (nid == 0xA834319Du) {   /* sceFontOpen(lib, index, mode, errPtr) */
		u32 index = (args != NULL) ? args[1] : 0;
		u32 block;

		/* Индекс знаковый: отрицательный в u32 превращается в
		 * огромное число и ловится той же проверкой `> 1`. */
		if (index > 1) {
			if (args != NULL && args[3] != 0)
				guest_write_u32(args[3], FONT_ERR_PARAM);
			return 0;
		}
		block = font_obj_block();
		if (block == 0)
			return 0;
		if (args != NULL && args[3] != 0)
			guest_write_u32(args[3], 0);       /* errPtr */
		g_font.opens++;
		return block;
	}

	/* `sceFontOpenUserFile`/`OpenUserMemory`: внешних PGF нет —
	 * честный отказ, иначе гость получит «открытый» шрифт без
	 * данных. */
	if (nid == 0x57FCB733u      /* sceFontOpenUserFile */
	    || nid == 0xBB8E7FE6u) {  /* sceFontOpenUserMemory */
		if (args != NULL && args[3] != 0)
			guest_write_u32(args[3],
			                (nid == 0x57FCB733u) ? FONT_ERR_FILE
			                                     : FONT_ERR_PARAM);
		return 0;
	}

	if (nid == 0x0DA7535Eu) {   /* sceFontGetFontInfo(font, info) */
		u32 info[FONT_INFO_WORDS];
		u32 dst = (args != NULL) ? args[1] : 0;

		if (dst == 0)
			return 0;
		font_info_words(info);
		font_put_words(dst, info, FONT_INFO_WORDS);
		return 0;
	}

	if (nid == 0xDCC80C2Fu) {   /* sceFontGetCharInfo(font, code, ptr) */
		u32 ci[FONT_CHARINFO_WORDS];
		u32 dst = (args != NULL) ? args[2] : 0;

		if (dst == 0)
			return 0;
		font_charinfo_words(ci);
		font_put_words(dst, ci, FONT_CHARINFO_WORDS);
		return 0;
	}

	/* Рендера глифов нет — ошибка с счётчиком; поведение гостя на
	 * отказ и есть следующий замер. */
	if (nid == 0x980F4895u      /* sceFontGetCharGlyphImage */
	    || nid == 0xCA1E6945u) {  /* …_Clip */
		g_font.glyphs++;
		return FONT_ERR_PARAM;
	}

	if (nid == 0x02D7F94Bu      /* sceFontFlush */
	    || nid == 0x3AEA8CB6u   /* sceFontClose */
	    || nid == 0x574B6FBCu)   /* sceFontDoneLib */
		return 0;

	/* `sceKernelStdout`: CRT0 держит дескриптор stdout, возврат 1 —
	 * единственный ненулевой файловый дескриптор. */
	if (nid == 0xA6BAB2E9u)
		return 1;

	/* `sceIoWrite(fd, buf, len)`: гостевой CRT0 пишет в stdout сообщение
	 * об ошибке перед `exit(1)`. Буфер лежит в BSS (за пределами filesz
	 * RW-сегмента), поэтому прочитать его из файла нельзя — снимаем здесь.
	 * Возврат всегда `len`, как настоящий PSP. */
	if (nid == 0x42EC03ACu) {
		static u32 iow_done;
		u32 buf, len, n, i, pos;
		char line[80];
		u64 end, start;

		g_iow_count++;
		/* Снимок списка atexit снимается ЗДЕСЬ и только здесь: гость
		 * напечатал сообщение об abort и вслед за тем вызывает `exit`,
		 * который проходит список и очищает его. В отчёте список уже
		 * пуст — это измерено, а не предположено, и без снимка выглядело
		 * бы как «список не строился вовсе». */
		if (g_atexit_snap_n == 0xFFFFFFFFu)
			guest_run_atexit_snapshot();
		if (args == NULL)
			return 0;
		buf = args[1];
		len = args[2];
		if (g_log_fd >= 0 && iow_done < 8
		    && g_mod != 0 && g_mod->memory != 0) {
			end = (u64)g_mod->base + (u64)g_mod->map_size;
			start = buf;
			if (start >= (u64)g_mod->base && start + 1u <= end) {
				u64 avail = end - start;
				const unsigned char *pv =
				    (const unsigned char *)g_mod->memory
				    + (buf - g_mod->base);

				n = len;
				if (n > 64u)
					n = 64u;
				if ((u64)n > avail)
					n = (u32)avail;
				pos = 0;
				line[pos++] = ' ';
				line[pos++] = ' ';
				line[pos++] = 'i';
				line[pos++] = 'o';
				line[pos++] = '=';
				for (i = 0; i < n; i++) {
					unsigned char c = pv[i];
					line[pos++] = (c >= 0x20 && c < 0x7f)
					    ? (char)c : '.';
				}
				line[pos++] = '\n';
				plat_write(g_log_fd, line, pos);
				iow_done++;
			}
		}
		return len;
	}

	if (nid == 0x4E3A1105u) {   /* sceKernelWaitSema */
		/* БЛОКИРУЮЩИЙ: на PSP поток спит, пока другой не
		 * просигналит. Прогон 63 мгновенного возврата: пара
		 * Wait/Signal крутилась 18.2M раз за 30 с (одинаково
		 * поровну — спин), CPU съедался у партнёра-потока.
		 * 1 мс на вызов заставляет пару честно ждать и
		 * высвобождает CPU остальным потокам.
		 *
		 * Прогон 81: главный ЖИЛ 17+ секунд ПОСЛЕ журнала×3
		 * (GCALL=290), семплировался в ЭТОМ парке, потом
		 * умер (GAT=10). Какой WaitSema-цикл — расшифровка
		 * первыми 16 вызовами с tid и ra. */
		static u32 dbg_n;
		if (g_log_fd >= 0 && dbg_n < 16) {
			dbg_n++;
			early_log("WSX", (u32)plat_gettid());
			early_log("WSR", ra);
		}
		plat_sleep_ms(1);
		return 0;
	}

	if (nid == 0xCEADEB47u) {   /* sceKernelDelayThread */
		/* a0 — МИКРОСЕКУНДЫ. Прогресса 63: мгновенный возврат
		 * крутил таймерный цикл 0x088C17B0 в паре с
		 * WaitVblank/GetTimeWide по 4.5M итераций (180k/с).
		 * Скругление вверх до мс; 0 — без сна.
		 *
		 * Прогон 80: главный спал в plat_sleep_ms — а его
		 * DelayThread-вызовы (если были) за пределами
		 * раннего лога. Логируем первые 16 с tid и a0. */
		static u32 dbg_n;
		u32 us = (args != NULL) ? args[0] : 0;
		if (g_log_fd >= 0 && dbg_n < 16) {
			dbg_n++;
			early_log("DTX", (u32)plat_gettid());
			early_log("DTR", ra);
			early_log("DTU", us);
		}
		if (us == 0)
			return 0;
		plat_sleep_ms((us + 999u) / 1000u);
		return 0;
	}

	/* --- Замер 90 (652ee635): звуковая инициализация LocoRoco шла
	 * по нереализованным вызовам, и «0 = успех» дефолта НЕ весь
	 * безобиден: у вызовов с OUT-АРГУМЕНТОМ игра читает из своей
	 * переменной мусор, потому что заглушка туда не писала.
	 * Ниже — минимально PSP-истинные ответы по факту вызовов
	 * прогона (см. docs/launcher-state.md, замер 90). --- */

	if (nid == 0x3F7AD767u) {   /* sceRtcGetCurrentTick — мкс, растут */
		g_time_us += 1000u;
		return g_time_us;
	}

	if (nid == 0xA5DA2406u) {   /* sceUtilityGetSystemParamInt */
		/* (id, out): пишем *out = 1. Для id 9 (громкость) это
		 * «включено»; язык и прочие id в этом прогоне не
		 * спрашивались. Без записи *out игра читала мусор. */
		if (args != NULL && args[1] != 0)
			guest_write_u32(args[1], 1u);
		return 0;
	}

	if (nid == 0x5EC81C55u) {   /* sceAudioChReserve — выделить канал */
		/* Замер 90: 4 вызова с a0=FFFFFFFF (авто-выбор), каждый
		 * получал 0 — четыре «канала 0». По контракту PSP
		 * возвращается НОМЕР канала (0..7), все разные. */
		static u32 audio_next;
		u32 ch = audio_next++;

		if (audio_next > 8u)
			audio_next = 8u;
		if (ch > 7u)
			return 0x80000000u;   /* PSP_AUDIO_ERROR_NOT_FOUND */
		return ch;
	}

	if (nid == 0x6FC46853u)     /* sceAudioChRelease — возврат 0 = успех */
		return 0;

	if (nid == 0x42778A9Fu) {   /* __sceSasInit(core, grain, voices, mode, rate) */
		/* Ядро SAS для игры OPAQUE (структура — аппаратное
		 * состояние), но параметры запоминаем: GetOutputmode и
		 * Rev* обязаны отвечать согласованно с Init. */
		u32 i;

		for (i = 0; i < SAS_CORES; i++) {
			if (g_sas[i].core == 0
			    || g_sas[i].core == (args != NULL ? args[0] : 0)) {
				g_sas[i].core = (args != NULL) ? args[0] : 0;
				g_sas[i].grain = (args != NULL) ? args[1] : 0;
				g_sas[i].voices = (args != NULL) ? args[2] : 0;
				g_sas[i].mode = (args != NULL) ? args[3] : 0;
				g_sas[i].rate = (args != NULL) ? args[4] : 0;
				break;
			}
		}
		return 0;
	}

	if (nid == 0xE175EF66u) {   /* __sceSasGetOutputmode(core) */
		u32 i;

		for (i = 0; i < SAS_CORES; i++)
			if (g_sas[i].core != 0
			    && g_sas[i].core == (args != NULL ? args[0] : 1u))
				return g_sas[i].mode;
		return 0;   /* PSP_SAS_OUTPUTMODE_MIXED */
	}

	if (nid == 0xD5A229C9u)     /* __sceSasRevEVOL — возврат 0 = успех */
		return 0;

	if (nid == 0xF983B186u)     /* __sceSasRevVON — возврат 0 = успех */
		return 0;

	if (nid == 0xCA04A2B9u) {   /* sceKernelRegisterSubIntrHandler */
		/* (intr, slot, handler, common) — см. блок `g_si`: без
		 * вызова обработчика звуковой движок не поднимает флаг
		 * готовности, и аллокатор возвращает NULL. */
		u32 i;

		for (i = 0; i < SI_SLOTS; i++) {
			if (g_si[i].handler == 0
			    || (g_si[i].intr == ((args != NULL) ? args[0] : 0)
			        && g_si[i].handler
			               == ((args != NULL) ? args[2] : 0))) {
				g_si[i].intr = (args != NULL) ? args[0] : 0;
				g_si[i].slot = (args != NULL) ? args[1] : 0;
				g_si[i].handler = (args != NULL) ? args[2] : 0;
				g_si[i].common = (args != NULL) ? args[3] : 0;
				break;
			}
		}
		return 0;
	}

	if (nid == 0xFB8E22ECu) {   /* sceKernelEnableSubIntr */
		u32 i;

		for (i = 0; i < SI_SLOTS; i++) {
			if (g_si[i].handler != 0
			    && g_si[i].intr == ((args != NULL) ? args[0] : 0)) {
				g_si[i].enabled = 1;
				break;
			}
		}
		return 0;
	}

	if (nid == 0x278C0DF5u)     /* sceKernelWaitThreadEnd — поток уже завершён */
		return 0;

	if (nid == 0xD59EAD2Fu) {   /* sceKernelWakeupThread */
		/* Замер 95: task-воркер (entry 0x088C295C) поллит
		 * g_qhead в цикле SleepThread; будить по-настоящему не
		 * нужно (поллинг), но ВЫЗОВ обязан быть видимым: если
		 * игра будит воркер, а мы молчим — это потерянное
		 * событие. Логируем первые 16: tid звонившего и uid. */
		static u32 wk_n;

		if (g_log_fd >= 0 && wk_n < 16) {
			wk_n++;
			early_log("WKX", (u32)plat_gettid());
			early_log("WKU", (args != NULL) ? args[0] : 0);
		}
		return 0;
	}

	if (!is_implemented(nid))
		g_run.unknown++;
	return 0;
}

/* --- защита потока от сбоя --------------------------------------------- */

/* Что в кадре, измерено пробой `tests/uctx_segv.c` на плеере, а не взято из
 * заголовка musl: `si_addr` лежит в `siginfo + 0x0C`, а PC — в
 * **`ucontext + 0xF0`**.
 *
 * Прежнее `+0x20` было неверным: это поле верно ТОЛЬКО для сбоя по данным, а
 * при сбое по выборке команды там лежит содержимое другого регистра. AGENTS.md
 * фиксирует `+0xF0` как единственное поле, годное для всех четырёх измеренных
 * видов сбоя.
 *
 * И даже `+0xF0` в кадре ГОСТЕВОГО потока не указывает на падающую инструкцию:
 * при `$k0`, указывающем на корректный `SceThreadContext`, `pc` = `0x08A9CA2C`
 * указывает на `lui a1,0x2e`, тогда как падать было бы `sw s5,4($k0)` перед ним,
 * и та при корректном `$k0` заведомо не падает. Локализовать падение по PC нельзя;
 * достоверен `si_addr`. */
#define SIGINFO_SI_ADDR 0x0C
#define UCONTEXT_PC     0x20

/* Смещение регистра N в кадре сигнала: `40 + 8·N`, где N — номер регистра
 * MIPS. Измерено пробой `tests/uctx_layout.c`, не взято из заголовка musl:
 * там раскладка другая.
 *
 * Именно `40`, а не `36`. Первая версия была `0x24 + 8·N` = `36 + 8·N`, то
 * есть на 4 байта назад для всех регистров, и это молча портило всё: по
 * смещению 268 лежит ВТОРАЯ половина слота `$gp` (264..271), где по
 * построению нули, поэтому `$sp` читался как 0 при настоящем `0x2CADFD80`,
 * и ни один кандидат не мог совпасть с `si_addr`. Число выглядело правдоподобно
 * — рядом стоял текст «проверяется согласованностью по шести регистрам», и он
 * был написан ДО проверки. Согласованность теперь проверяется гейтом
 * `tools/check_uctx.sh`, а не комментарием.
 *
 * Измеренные значения для сверки: `N = 2` → 56 (`$v0`), `N = 4` → 72 (`$a0`),
 * `N = 25` → 240 (`$t9`), `N = 28` → 264 (`$gp`), `N = 29` → 272 (`$sp`),
 * `N = 30` → 280 (`$fp`), `N = 31` → 288 (`$ra`).
 *
 * Регистры 26 и 27 (`$k0`, `$k1`) ядро не заполняет, и читать их нельзя:
 * на месте них остаётся то, что оставила musl. */
#define UCONTEXT_GREG(n) (40 + 8 * (n))

/* Кандидаты на смещение PC в `ucontext`.
 *
 * Печатаются все разом: верное определяется по тому, какое из них содержит
 * адрес образа гостя, а не по тому, какое названо верным в документации. */
static const unsigned short probe_off[] = { 0x10, 0x20, 0x28, 0x40, 0xF0 };

/* Какая ИМЕННО инструкция упала.
 *
 * Поле PC указывает неоднозначно, и это измерено, а не предположено: в одних
 * сбоях падает инструкция по самому `pc`, в других — по `pc - 4`, и признака,
 * который их различает, в самом поле нет.
 *
 *   сбой #5, `pc = 0x08ADC82C` — это `and $a2,$a2,$a3`; такая инструкция
 *          падать не может, значит падает по `pc - 4`;
 *   сбой #7, `pc = 0x08ADC838`, `si_addr = 0x98` — по `pc - 4` лежит
 *          `sw $t2,4($sp)`, а `$sp` в кадре равен `0x2CADFD80`, то есть
 *          адрес `0x2CADFD84` и падать он не может; значит для этого сбоя
 *          падает сам `pc` — `sw $t1,8($t0)` при `$t0 = 0x90` даёт
 *          ровно `0x98`.
 *
 * Оба вывода верны одновременно. Прежнее утверждение «PC указывает на
 * инструкцию ПОСЛЕ падающей, это подтверждено на двух точках» было верно
 * для первых сбоев и обобщено без основания: подтверждать надо было все
 * сбои, а не два из первых.
 *
 * Рассуждением здесь нельзя — оба кандидата правдоподобны. Замером можно:
 * ядро отдаёт `si_addr` точно, а для каждого кандидата эффективный адрес
 * считается однозначно. Тот, у кого адрес совпал с `si_addr`, и есть
 * падающая инструкция. Проверка идёт при каждом сбое, поэтому правило
 * определяется само и не переносится из одного прогона в другой.
 *
 * Печатаются оба кандидата, включая несовпавший: иначе при промахе мышцы
 * не отличим «инструкция не load/store» от «регистр не тот». */
static int fault_which_instruction(const unsigned char *uc,
                                   const GuestModule *mod,
                                   u32 pc_v, u32 want, char *out)
{
	/* Только эти коды обращаются к памяти; остальные (арифметика, переходы,
	 * `lui`) падать по данным не могут. */
	static const unsigned char ls_op[8] = { 0x20, 0x21, 0x23, 0x24,
	                                        0x25, 0x28, 0x29, 0x2B };
	const unsigned char *img;
	unsigned char m[4];
	u32 word;
	unsigned op, base, imm, i;
	int ci = 0, ck, found;

	for (i = 0; i < 2; i++) {
		u32 at = (i == 0) ? pc_v : pc_v - 4;

		/* Кандидат должен лежать в образе гостя: за его пределами читать
		 * нечего, и адрес `pc - 4` на границе образа даст чужую страницу. */
		if (mod == 0 || mod->memory == 0 || at < mod->base ||
		    at + 4 > mod->base + mod->map_size || (at & 3) != 0) {
			out[ci++] = ' ';
			out[ci++] = 'i';
			out[ci++] = (char)('0' + (int)i);
			out[ci++] = '=';
			out[ci++] = '-';
			out[ci++] = '\n';
			continue;
		}

		/* `img` присваивается ПОСЛЕ проверок: иначе обращение к
		 * `mod->memory` при нулевом поле рушит обработчик сигнала, то есть
		 * ровно там, где диагностика нужнее всего. */
		img = (const unsigned char *)mod->memory;
		m[0] = img[at - mod->base];
		m[1] = img[at - mod->base + 1];
		m[2] = img[at - mod->base + 2];
		m[3] = img[at - mod->base + 3];
		word = (u32)m[0] | ((u32)m[1] << 8) | ((u32)m[2] << 16)
		     | ((u32)m[3] << 24);
		op = word >> 26;
		found = 0;
		for (ck = 0; ck < 8; ck++) {
			if (ls_op[ck] == op)
				found = 1;
		}

		out[ci++] = ' ';
		out[ci++] = 'i';
		out[ci++] = (char)('0' + (int)i);
		out[ci++] = '=';
		for (ck = 7; ck >= 0; ck--)
			out[ci++] = "0123456789ABCDEF"[(word >> (ck * 4)) & 0xF];

		if (found) {
			u32 rv;
			u32 eff;
			int rd_ok = 1;

			base = (word >> 21) & 31;
			imm = word & 0xFFFF;
			/* `imm` расширяется ЗНАКОМ: без этого `lw` по отрицательному
			 * смещению даст адрес на 64 КиБ больше и совпадение с
			 * `si_addr` будет ложным. */
			if (imm & 0x8000)
				imm |= 0xFFFF0000;
			rv = (u32)uc[UCONTEXT_GREG(base)]
			   | ((u32)uc[UCONTEXT_GREG(base) + 1] << 8)
			   | ((u32)uc[UCONTEXT_GREG(base) + 2] << 16)
			   | ((u32)uc[UCONTEXT_GREG(base) + 3] << 24);
			eff = rv + imm;
			if (base == 26 || base == 27)
				rd_ok = 0;
			out[ci++] = ' ';
			out[ci++] = 'r';
			/* Шестнадцатеричным, а НЕ `'0' + номер`: номера до 31, и
			 * для 26, 29, 30, 31 сложение даёт `J`, `M`, `N`, `?` —
			 * то есть в отчёт попадали не имена регистров, а символы
			 * ASCII. Первые номера выглядели правильно, поэтому ошибка
			 * не бросалась в глаза тем более, что печаталось всего
			 * семь символов подряд. */
			out[ci++] = "0123456789ABCDEF"[(base >> 4) & 0xF];
			out[ci++] = "0123456789ABCDEF"[base & 0xF];
			out[ci++] = '=';
			/* `$k0` (26) и `$k1` (27) в кадре ОТСУТСТВУЮТ: ядро их не
			 * заполняет, и на месте них остаётся то, что оставила musl.
			 * Печатать это как значение нельзя — так в отчёт попадал
			 * «нулевой `$k0`», из которого потом вырос вывод о сбое.
			 * Правило проекта: у значения, которого в кадре нет, обязана
			 * стоять метка «недействительно». */
			if (base == 26 || base == 27) {
				out[ci++] = 'N';
				out[ci++] = '/';
				out[ci++] = 'A';
				out[ci++] = '?';
			} else {
				for (ck = 7; ck >= 0; ck--)
					out[ci++] = "0123456789ABCDEF"[(rv >> (ck * 4)) & 0xF];
			}
			out[ci++] = ' ';
			out[ci++] = 'e';
			out[ci++] = '=';
			for (ck = 7; ck >= 0; ck--)
				out[ci++] = "0123456789ABCDEF"[(eff >> (ck * 4)) & 0xF];
			/* Совпадение по eff проверяется только когда базовый регистр
			 * действительно есть в кадре. Для `$k0`/`$k1` eff вычислен из
			 * мусора, и «совпадение» было бы случайным. */
			if (eff == want && rd_ok) {
				out[ci++] = ' ';
				out[ci++] = 'M';
				out[ci++] = 'A';
				out[ci++] = 'T';
				out[ci++] = 'C';
				out[ci++] = 'H';
			}

			/* Разыменование: что ЛЕЖИТ по вычисленному адресу.
			 *
			 * Зачем. Вся последняя цепочка локализовалась вручную по двум
			 * РАЗНЫМ снимкам — сначала адрес `lw a1,…` из дампа регистров,
			 * потом значение по нему отдельным `dump` с плеера. Два снимка
			 * дают совпадение, которого нет: `a1` в момент сбоя и значение
			 * по `12(a1)` читались в разное время, а гость между тем
			 * продолжает исполняться. Три раза подряд это давало «связную»,
			 * но неверную цепочку (`0x79DC`/`0xB9DC`, `lui 0x0032`/`0x08B2`) —
			 * в обоих случаях файл или прошлый снимок подмешивались в новый.
			 *
			 * Здесь адрес и значение берутся ОДНИМ замером, из одного кадра,
			 * и печатаются парой. Совпадает ли `eff` со `si_addr` — видно по
			 * `MATCH`, и то же поле отвечает, откуда взялся указатель.
			 *
			 * Читается только если адрес внутри отображения гостя: за его
			 * пределами чтение в обработчике сигнала рухнет тем же сигналом,
			 * и диагностика пропадёт целиком. */
			if (rd_ok && mod != 0 && mod->memory != 0 && mod->base != 0 &&
			    eff >= mod->base &&
			    /* Граница БЕЗ переполнения. Прежний вид
			     * `eff + 4 <= mod->base + mod->map_size` сворачивается,
			     * если `eff >= 0xFFFFFFFC`: сложение даёт 0..3, оно
			     * меньше конца отображения, и проверка ПРОХОДИТ.
			     * Замер на плеере: вложенный сбой `FAULT=4
			     * sig=000B addr=00000002 pc=2D0C8AE8`, а `pc` по
			     * замеру `$gp` = `0x2D0EC010` и смещения `_gp` =
			     * `0x28010` лежит в `guest_fault_handler` на
			     * инструкции `lwl t1,3(v0)` — то есть в самом
			     * обработчике, на чтении `deref=`. `si_addr = 2`
			     * означает `v0 = 0xFFFFFFFF`, то есть `eff` дошёл сюда
			     * переполнением. У сбоя `FAULT=3` строка `i0/i1`
			     * ОТСУТСТВУЕТ, хотя печатается для каждого сбоя, и
			     * после `FAULT=3` конструкторы больше не идут — прогон
			     * кончился именно здесь.
			     *
			     * Сверка с формой, уже принятой в загрузчике:
			     * `(u64)patch_addr + 4 > (u64)m->base + m->map_size`. */
			    (u64)eff + 4 <= (u64)mod->base + mod->map_size) {
				const unsigned char *pv = (const unsigned char *)mod->memory
				                          + (eff - mod->base);
				u32 val = (u32)pv[0] | ((u32)pv[1] << 8)
				        | ((u32)pv[2] << 16) | ((u32)pv[3] << 24);

				out[ci++] = ' ';
				out[ci++] = 'd';
				out[ci++] = 'e';
				out[ci++] = 'r';
				out[ci++] = 'e';
				out[ci++] = 'f';
				out[ci++] = '=';
				for (ck = 7; ck >= 0; ck--)
					out[ci++] = "0123456789ABCDEF"[(val >> (ck * 4)) & 0xF];
			}
		} else {
			out[ci++] = ' ';
			out[ci++] = 'n';
			out[ci++] = 'o';
			out[ci++] = 'm';
			out[ci++] = 'e';
			out[ci++] = 'm';
		}
		out[ci++] = '\n';
	}
	return ci;
}

/* Сырой дамп начала кадра — для ПЕРВОГО сбоя прогона.
 *
 * Зачем. Метод определения падающей инструкции по `pc` и `pc - 4` имеет
 * дыру, и дыра обнаружилась на первом же сбое: при верном `$k0`
 * (`k0 = 0x08BCBED4`, проверено в отчёте) инструкция `sw $s5,4($k0)` пишет по
 * адресу `0x08BCBED8` и падать не может, а `lui` по самому `pc` не падает
 * тоже. То есть поле PC здесь не указывает ни на падающую инструкцию, ни на
 * следующую, и оба кандидата отпадают.
 *
 * `si_addr` не помогает: для сбоя #9 в этом же прогоне он остался нулевым,
 * хотя вычисленный адрес был `0xFFFFFFFF`.
 *
 * Поэтому вместо выбора поля по документации печатаются ВСЕ подряд. Раньше
 * именно так нашли PC, и дёшево: адрес сбоя на MIPS лежит в `sigcontext`
 * (`sc_badvaddr`), но где именно — на этой прошивке не измерено, а по
 * заголовку musl раскладка уже не совпадает.
 *
 * Слово, попавшее внутрь образа гостя, помечается `*` — это и есть маркер
 * «похоже на адрес», по которому поле опознаётся эмпирически, а не
 * предположительно.
 *
 * Печатается только для первого сбоя: дамп занимает десятки строк, а нужен
 * он для определения раскладки, то есть один раз. */
static int fault_raw_dump(const unsigned char *uc, const void *info,
                          const GuestModule *mod, char *out)
{
	const unsigned char *from[2];
	int n, i, ci = 0;

	from[0] = uc;
	from[1] = (const unsigned char *)info;

	/* Первая половина: начало кадра и `siginfo`.
	 *
	 * Почему 20 слов мало: первый дамп на 20 слов подтвердил, что PC лежит по
	 * смещению 32 (помечен `*` как адрес гостя), и показал отравленец
	 * `0xFFFFABCD` по смещению 64 — но `$k0` лежит по смещению 248, и до
	 * него дамп не доехал. Без него нельзя проверить главную версию: падает
	 * `sw $s5,4($k0)`, и вопрос в том, что в `$k0`.
	 *
	 * С 2026-10-14 слов кадра 96: за регистрами общего назначения
	 * (40 + 8*32 = 296) в sigcontext лежит `sc_cause` — регистр CP0
	 * Cause МОМЕНТА исключения. Для SIGILL от запрещённой инструкции
	 * ExcCode = 0x0A (Cause[6:2], то есть в слове биты 0x28), и
	 * возможно бит BD 0x8000, если исключение случилось в слоте
	 * задержки. Это единственный способ отличить «ядро дало pc со
	 * смещением» от «pc верен, но слово легально»: сигнатура SIGILL от
	 * 2026-10-14 ровно такая — pc на `lui`, pc-4 на `move`, оба
	 * легальны. */
	for (n = 0; n < 2; n++) {
		for (i = 0; i < (n == 0 ? 96 : 20); i++) {
			const unsigned char *q = from[n] + i * 4;
			u32 v = (u32)q[0] | ((u32)q[1] << 8)
			      | ((u32)q[2] << 16) | ((u32)q[3] << 24);
			int k;
			int in_guest = (mod != 0 && mod->memory != 0
			                && mod->base != 0
			                && v >= mod->base
			                && v < mod->base + mod->map_size);

			if (ci > 0 && ci < 3900)
				out[ci++] = '\n';
			if (n == 0) {
				out[ci++] = 'R';
				out[ci++] = (char)('0' + (i / 10));
				out[ci++] = (char)('0' + (i % 10));
			} else {
				out[ci++] = 'S';
				out[ci++] = (char)('0' + (i / 10));
				out[ci++] = (char)('0' + (i % 10));
			}
			out[ci++] = '=';
			for (k = 7; k >= 0; k--)
				out[ci++] = "0123456789ABCDEF"[(v >> (k * 4)) & 0xF];
			if (in_guest)
				out[ci++] = '*';
		}
	}

	/* Вторая половина: регистровый файл по измеренной раскладке
	 * `gregs[i]` = 40 + 8·i, с ИМЕНАМИ регистров.
	 *
	 * Имена нужны не для красоты: отравленец `0xFFFFABCD` в одном слоте
	 * отличить от настоящего значения можно только по имени — у `$v1` он
	 * в кадре оказался по смещению 64, и без подписи это выглядело бы как
	 * «`$v1` равен мусору». */
	{
		static const char *const regn[32] = {
			"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
			"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
			"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
			"t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
		};

		for (i = 0; i < 32; i++) {
			const unsigned char *q = uc + 40 + 8 * i;
			u32 v = (u32)q[0] | ((u32)q[1] << 8)
			      | ((u32)q[2] << 16) | ((u32)q[3] << 24);
			int k;
			int in_guest = (mod != 0 && mod->memory != 0
			                && mod->base != 0
			                && v >= mod->base
			                && v < mod->base + mod->map_size);

			out[ci++] = '\n';
			out[ci++] = 'G';
			out[ci++] = (char)('0' + (i / 10));
			out[ci++] = (char)('0' + (i % 10));
			out[ci++] = ' ';
			{
				/* Имя печатается СТРОКОЙ, а не тремя символами:
				 * у `"at"` третий символ — `'\0'`, и запись обрывалась на
				 * нулевом байте: в отчёт попадало `G01 at` без значения. */
				const char *nm = regn[i];

				while (*nm != '\0')
					out[ci++] = *nm++;
			}
			out[ci++] = '=';
			for (k = 7; k >= 0; k--)
				out[ci++] = "0123456789ABCDEF"[(v >> (k * 4)) & 0xF];
			if (in_guest)
				out[ci++] = '*';
		}
	}
	out[ci++] = '\n';
	return ci;
}

/* Обработчик сбоя потока гостя.
 *
 * Завершает ПОТОК, а не весь процесс: `SIGSEGV` в любом потоке убивает всю
 * программу, а программа здесь — интерфейс плеера. Никаких вызовов, кроме
 * сырого `write`, обработчик не делает: `strlen` и прочее здесь лишний риск.
 * Адрес PC печатается как есть — против фильтра «похоже на адрес», который в
 * проекте уже один раз показал неверный диагноз на неинициализированном
 * поле. */
static void guest_fault_handler(int sig, void *info, void *uc)
{
	static const char digits[] = "0123456789ABCDEF";
	char buf[160];
	const unsigned char *pc = (const unsigned char *)uc + UCONTEXT_PC;
	const unsigned char *addr = (const unsigned char *)info + SIGINFO_SI_ADDR;
	u32 pc_v = (u32)pc[0] | ((u32)pc[1] << 8)
	           | ((u32)pc[2] << 16) | ((u32)pc[3] << 24);
	u32 ad_v = (u32)addr[0] | ((u32)addr[1] << 8)
	           | ((u32)addr[2] << 16) | ((u32)addr[3] << 24);
	int i = 0, k, n;

	/* Чужой поток: печатается ОТДЕЛЬНОЙ строкой и уходит из обработчика.
	 *
	 * Зачем. `SA_ONSTACK` на этом плеере не работает (замер ранее), поэтому
	 * обработчик гостя исполняется на стеке прерванного кода — и сигнал
	 * ЧУЖОГО потока `shell` тоже доходит до него. Замерено: на четвёртом
	 * сбое прогона `tid = 0x1FC4` против `0x6AC` у гостевых, а `pc` выходил
	 * за образ и не принадлежал ни одному живому процессу.
	 *
	 * Без этой проверки чужой кадр (`ra = 0x0A`, `si_addr = 2`) шёл в тот же
	 * отчёт и в те же счётчики, что настоящие гостевые сбои. На этом уже был
	 * построен вывод «гость ушёл в наш код», который пришлось снять.
	 *
	 * Счётчики гостя НЕ трогаем: `g_fault_total` считает ТОЛЬКО гостя, и
	 * предел восстановления должен отмеряться по гостю. */
	if (!guest_thread_fault((u32)plat_gettid())) {
		/* Печатается просто и по полям, без разбора шаблона: `tid`
		 * чужого потока известен, а каждый символ строки виден
		 * глазами. Дефект разбора шаблона здесь уже был — двойной
		 * проход с ручным `i += 8`. */
		i = 0;
		buf[i++] = ' ';
		buf[i++] = 'F'; buf[i++] = 'O'; buf[i++] = 'R';
		buf[i++] = 'E'; buf[i++] = 'I'; buf[i++] = 'G';
		buf[i++] = 'N';
		buf[i++] = ' '; buf[i++] = 's'; buf[i++] = 'i';
		buf[i++] = 'g'; buf[i++] = '=';
		for (k = 3; k >= 0; k--)
			buf[i++] = digits[((u32)sig >> (k * 4)) & 0xF];
		buf[i++] = ' '; buf[i++] = 'a'; buf[i++] = 'd';
		buf[i++] = 'd'; buf[i++] = 'r'; buf[i++] = '=';
		for (k = 7; k >= 0; k--)
			buf[i++] = digits[(ad_v >> (k * 4)) & 0xF];
		buf[i++] = ' '; buf[i++] = 'p'; buf[i++] = 'c';
		buf[i++] = '=';
		for (k = 7; k >= 0; k--)
			buf[i++] = digits[(pc_v >> (k * 4)) & 0xF];
		buf[i++] = ' '; buf[i++] = 't'; buf[i++] = 'i';
		buf[i++] = 'd'; buf[i++] = '=';
		for (k = 7; k >= 0; k--)
			buf[i++] = digits[((u32)plat_gettid() >> (k * 4)) & 0xF];
		buf[i++] = '\n';
		(void)plat_write(g_log_fd, buf, (size_t)i);

		/* PC правится и для чужого/неучтённого потока, ЕСЛИ он внутри
		 * образа гостя.
		 *
		 * Зачем обязательная правка. Без неё `return` из обработчика
		 * оставляет PC на той же недопустимой инструкции, и сигнал
		 * приходит снова — до смерти процесса. Это не теория: прогон с
		 * клоном конструктора дал `FOREIGN sig=0004 pc=2CB601DC`
		 * (`SIGILL`) от неучтённого потока, и `root.log` показал
		 * `child process terminated, status = 5` — процесс умер, интерфейс
		 * погас. Пользователь видел чёрный экран.
		 *
		 * Правка только по адресу ВНУТРИ образа: за его пределами трогать
		 * чужой кадр нельзя — испортится не то поле, а причина будет не в
		 * гoste. Для чужого адреса обработчик просто выходит, и сигнал
		 * повторяется, но это лучше порчи чужого кадра. */
		if (g_mod != 0 && g_mod->memory != 0 && pc_v >= g_mod->base &&
		    pc_v < g_mod->base + g_mod->map_size) {
			unsigned char *pcp = (unsigned char *)uc + UCONTEXT_PC;
			u32 np = pc_v + 4;

			/* Запись — ровно как в основном пути восстановления:
			 * байтами от младшего к старшему, один и тот же приём. */
			pcp[0] = (unsigned char)(np & 0xFF);
			pcp[1] = (unsigned char)((np >> 8) & 0xFF);
			pcp[2] = (unsigned char)((np >> 16) & 0xFF);
			pcp[3] = (unsigned char)((np >> 24) & 0xFF);
		}
		return;
	}

	g_fault_total++;
	g_guest_fault_sig = (u32)sig;
	g_guest_fault_pc = pc_v;
	g_guest_fault_addr = ad_v;

	/* Ранний маркер для SIGILL — первой строкой, до любой другой
	 * печати.
	 *
	 * Зачем: прогон 2026-10-14 (сборка `ab616f6e`) поставил
	 * `fault_sig=4` и `fault_pc=0x08AB9C7C` в финальный отчёт —
	 * значит, обработчик ДОШЁЛ до этих присваиваний, — но НИ ОДНОЙ
	 * строки сбоя в файле не оказалось, при том что live-лог до
	 * самого сбоя писался. Где именно обработчик теряет печать,
	 * без маркера не выяснить: он отвечает «вошёл и дошёл сюда»,
	 * а не «напечатал всё». */
	if (sig == 4 && g_log_fd >= 0) {
		char mb[64];
		int mi = 0, mk;

		mb[mi++] = ' ';
		mb[mi++] = 'S'; mb[mi++] = 'I'; mb[mi++] = 'G';
		mb[mi++] = 'I'; mb[mi++] = 'L'; mb[mi++] = 'L';
		mb[mi++] = '-'; mb[mi++] = 'S'; mb[mi++] = 'E';
		mb[mi++] = 'E'; mb[mi++] = 'N'; mb[mi++] = ' ';
		mb[mi++] = 'p'; mb[mi++] = 'c'; mb[mi++] = '=';
		for (mk = 7; mk >= 0; mk--)
			mb[mi++] = digits[(pc_v >> (mk * 4)) & 0xF];
		mb[mi++] = '\n';
		(void)plat_write(g_log_fd, mb, (size_t)mi);
	}

	/* Счётчик «подряд на этом pc» и печать момента его срабатывания.
	 *
	 * Печатается ДО проверки, иначе отличить «предел не сработал» от
	 * «предел сработал позже, чем казалось» нечем: строка `FAULTSPIN`
	 * прямо отвечает, почему поток завершён. */
	if (g_fault_spin_pc == pc_v) {
		g_fault_spin++;
	} else {
		g_fault_spin_pc = pc_v;
		g_fault_spin = 1;
	}
	if (g_fault_spin > FAULT_SPIN_MAX && g_log_fd >= 0) {
		char cb[80];
		int ci = 0, ck;

		cb[ci++] = ' ';
		cb[ci++] = 'F'; cb[ci++] = 'A'; cb[ci++] = 'U';
		cb[ci++] = 'L'; cb[ci++] = 'T'; cb[ci++] = 'S';
		cb[ci++] = 'P'; cb[ci++] = 'I'; cb[ci++] = 'N';
		cb[ci++] = '=';
		for (ck = 7; ck >= 0; ck--)
			cb[ci++] = digits[(g_fault_total >> (ck * 4)) & 0xF];
		cb[ci++] = ' '; cb[ci++] = 'n'; cb[ci++] = 'i';
		cb[ci++] = 'c'; cb[ci++] = 'e'; cb[ci++] = '=';
		for (ck = 7; ck >= 0; ck--)
			cb[ci++] = digits[(g_fault_spin >> (ck * 4)) & 0xF];
		cb[ci++] = ' '; cb[ci++] = 'p'; cb[ci++] = 'c';
		cb[ci++] = '=';
		for (ck = 7; ck >= 0; ck--)
			cb[ci++] = digits[(pc_v >> (ck * 4)) & 0xF];
		cb[ci++] = '\n';
		(void)plat_write(g_log_fd, cb, (size_t)ci);
	}

	/* Компактная строка печатается для КАЖДОГО сбоя, а подробный дамп — только
	 * для первых нескольких.
	 *
	 * Сначала было наоборот, и это стоило ровно той строки, ради которой
	 * прогон и делался: фатальным оказался девятый сбой, подробный дамп
	 * печатался только до восьмого, и значение `$a0` в момент смерти потока
	 * не попало в отчёт вовсе. Подробный дамп нужен для разбора закономерности,
	 * компактная строка — для того, чтобы знать, где кончилось. */
	if (g_log_fd >= 0) {
		char cb[64];
		int ci = 0, ck;

		cb[ci++] = ' ';
		cb[ci++] = 'F'; cb[ci++] = 'A'; cb[ci++] = 'U';
		cb[ci++] = 'L'; cb[ci++] = 'T'; cb[ci++] = '=';
		for (ck = 7; ck >= 0; ck--)
			cb[ci++] = digits[(g_fault_total >> (ck * 4)) & 0xF];
		cb[ci++] = ' '; cb[ci++] = 's'; cb[ci++] = 'i';
		cb[ci++] = 'g'; cb[ci++] = '=';
		for (ck = 3; ck >= 0; ck--)
			cb[ci++] = digits[((u32)sig >> (ck * 4)) & 0xF];
		cb[ci++] = ' '; cb[ci++] = 'a'; cb[ci++] = 'd';
		cb[ci++] = 'd'; cb[ci++] = 'r'; cb[ci++] = '=';
		for (ck = 7; ck >= 0; ck--)
			cb[ci++] = digits[(ad_v >> (ck * 4)) & 0xF];
		cb[ci++] = ' '; cb[ci++] = 'p'; cb[ci++] = 'c';
		cb[ci++] = '=';
		for (ck = 7; ck >= 0; ck--)
			cb[ci++] = digits[(pc_v >> (ck * 4)) & 0xF];
		cb[ci++] = '\n';
		(void)plat_write(g_log_fd, cb, (size_t)ci);
	}

	/* Расшифровка инструкции — для КАЖДОГО сбоя, а не только первых
	 * `FAULT_REPORT_MAX`.
	 *
	 * Это ровно тот слепой угол, на который уже наступали: фатальный сбой
	 * оказался девятым, подробный дамп печатался до восьмого, и у
	 * ФАТАЛЬНОГО сбоя не оказалось НИ ОДНОЙ строки — при том что сам сбой
	 * (`FAULT=9 sig=0A`) в отчёт попал. Три дня назад это стоило нам вывода
	 * «восстановление работает», хотя работала ровно та строка, которую
	 * вынесли наружу.
	 *
	 * Расшифровка — две короткие строки на сбой, и она дешевле полного
	 * дампа регистров, который печатается ограниченно и по делу: полный
	 * дамп нужен, чтобы увидеть закономерность, расшифровка — чтобы узнать,
	 * где кончилось. */
	if (g_log_fd >= 0) {
		char wb[128];
		int wn;

		wn = fault_which_instruction(uc, g_mod, pc_v, ad_v, wb);
		(void)plat_write(g_log_fd, wb, (size_t)wn);

		/* Окно слов вокруг pc — ТОЛЬКО для SIGILL.
		 *
		 * Зачем: у этого ядра pc при SIGILL НЕ указывает на
		 * запрещённую инструкцию. Измерено дважды: на `cache 0x1B`
		 * (2026-10-14) запрещённая лежала на pc-4; на сбое внутри
		 * `HeapSystem` (2026-10-14, сборки `9032bd80`/`ab616f6e`,
		 * ДВА повтора с тем же pc) ЛЕГАЛЬНЫМИ оказались и pc, и
		 * pc-4, и слот задержки — зато на 0x1BC раньше лежит
		 * выравнивание кучи из двух `ins` (SPECIAL3, MIPS32R2).
		 * Окно pc-0x200..pc+0x10 даёт сырые слова: класс запрещённой
		 * (SPECIAL3/2, CACHE, COP1) виден по опкодам прямо из
		 * дампа, SPECIAL3 помечается звёздочкой.
		 *
		 * ЭМУЛЯЦИЯ ins/ext по окну: в PSP-тулчейне SPECIAL3 — это
		 * `ins`/`ext` (271 во всём `.text`, `rdhwr` — ни одной).
		 * Процессор плеера их не выполняет, как не выполнял и
		 * `cache`. Пропускать нельзя: обе пишут РЕГИСТР, потеря —
		 * это неверные выравнивания кучи. Обе операции ИДЕМПОТЕНТНЫ,
		 * поэтому эмуляция уже исполненного слова безвредна, а
		 * неисполненного — правильна. Нашли — продолжаем поток с
		 * reported pc; не нашли ни одного — SIGILL чужой, поток
		 * завершится ниже, как прежде.
		 *
		 * Ограничение окна образа гостя: как и везде, чтение только по
		 * проверенному диапазону. */
		if (sig == 4 && g_mod != 0 && g_mod->memory != 0 &&
		    pc_v >= g_mod->base + 0x200 &&
		    pc_v + 0x14 <= g_mod->base + g_mod->map_size) {
			char ob[2048];
			int oi = 0, ok;
			int emul = 0;
			int emul_last = -1000;   /* слово последней эмуляции ДО pc */

			for (ok = -128; ok <= 4; ok++) {
				const unsigned char *q =
				    (const unsigned char *)g_mod->memory
				    + (pc_v - g_mod->base) + ok * 4;
				u32 v = (u32)q[0] | ((u32)q[1] << 8)
				      | ((u32)q[2] << 16)
				      | ((u32)q[3] << 24);

				ob[oi++] = ' ';
				ob[oi++] = 'w';
				if (ok < 0) {
					ob[oi++] = '-';
					ob[oi++] =
					    (char)digits[(-(s32)ok >> 4) & 0xF];
					ob[oi++] = (char)digits[(-ok) & 0xF];
				} else {
					ob[oi++] = '+';
					ob[oi++] = (char)digits[(ok >> 4) & 0xF];
					ob[oi++] = (char)digits[ok & 0xF];
				}
				ob[oi++] = '=';
				for (wn = 7; wn >= 0; wn--)
					ob[oi++] = digits[(v >> (wn * 4)) & 0xF];
				if ((v >> 26) == 0x1Fu
				    && ((v & 0x3Fu) == 0u || (v & 0x3Fu) == 4u))
					ob[oi++] = '*';   /* ins/ext — эмулировано */
				else if (((v >> 26) == 0x1Fu
				          && (v & 0x3Fu) == 0x20u
				          && (((v >> 6) & 31u) == 0x10u
				              || ((v >> 6) & 31u) == 0x18u
				              || ((v >> 6) & 31u) == 0x02u))
				         || ((v >> 26) == 0x00u
				             && ((v >> 21) & 31u) == 1u
				             && (v & 0x3Fu) == 0x02u)
				         || ((v >> 26) == 0x00u
				             && ((v >> 6) & 31u) == 1u
				             && (v & 0x3Fu) == 0x06u))
					ob[oi++] = '*';   /* r2: seb/seh/wsbh/rotr/rotrv */
				ob[oi++] = '\n';

				/* `ins`/`ext` по битам слова:
				 *   EXT: rt-поле = приёмник, rs = источник,
				 *        rd-поле = size-1, sa = pos;
				 *   INS: rd-поле = приёмник И pos+size-1,
				 *        rt = источник, sa = pos.
				 * Кадр гостя: gregs[n] = 40 + 8n (проба
				 * `uctx_layout.c`). Запись — байтами, как
				 * везде в этом обработчике. */
				if ((v >> 26) == 0x1Fu
				    && ((v & 0x3Fu) == 0u || (v & 0x3Fu) == 4u)) {
					u32 rt = (v >> 16) & 31u;
					u32 rd = (v >> 11) & 31u;
					u32 sa = (v >> 6) & 31u;
					u32 size, mask, nv;
					unsigned char *rp;

					if ((v & 0x3Fu) == 0u) {   /* EXT */
						const unsigned char *sp2 =
						    (const unsigned char *)uc
						    + UCONTEXT_GREG((v >> 21) & 31u);
						u32 sv = (u32)sp2[0]
						    | ((u32)sp2[1] << 8)
						    | ((u32)sp2[2] << 16)
						    | ((u32)sp2[3] << 24);

						size = rd + 1u;
						mask = (size >= 32u)
						    ? 0xFFFFFFFFu
						    : ((1u << size) - 1u);
						nv = (sv >> sa) & mask;
						rp = (unsigned char *)
						    uc + UCONTEXT_GREG(rt);
					} else {                    /* INS */
						const unsigned char *tp2 =
						    (const unsigned char *)uc
						    + UCONTEXT_GREG(rt);
						u32 rt_v = (u32)tp2[0]
						    | ((u32)tp2[1] << 8)
						    | ((u32)tp2[2] << 16)
						    | ((u32)tp2[3] << 24);

						size = (rd >= sa)
						    ? (rd - sa + 1u) : 0u;
						mask = (size >= 32u)
						    ? 0xFFFFFFFFu
						    : ((size == 0u)
						       ? 0u
						       : ((1u << size) - 1u));
						mask <<= sa;
						rp = (unsigned char *)
						    uc + UCONTEXT_GREG(rd);
						nv = ((u32)rp[0]
						      | ((u32)rp[1] << 8)
						      | ((u32)rp[2] << 16)
						      | ((u32)rp[3] << 24))
						     & ~mask;
						nv |= (rt_v << sa) & mask;
					}
					rp[0] = (unsigned char)(nv & 0xFF);
					rp[1] = (unsigned char)((nv >> 8) & 0xFF);
					rp[2] = (unsigned char)((nv >> 16) & 0xFF);
					rp[3] = (unsigned char)((nv >> 24) & 0xFF);
					emul++;
					g_emul_ins++;
					if (ok <= 0)
						emul_last = ok;
				}

				/* --- ЭМУЛЯЦИЯ остальных MIPS32r2-инструкций
				 * PSP Allegrex (плеер — 4KEc, r1).
				 *
				 * Замер прогона 75: главный игровой поток
				 * и ещё один гостевой умерли ТИХО (WDIED нет,
				 * ExitThread нет, SPINDEAD нет) — единственный
				 * оставшийся путь: SIGILL-ветка ниже, убивающая
				 * БЕЗ печати. Скан .text гостя нашёл виновников:
				 * seb ×95, seh ×51, rotr ×53, rotrv ×15 —
				 * ядро плеера их НЕ ИСПОЛНЯЕТ.
				 *
				 * Все четыре ИДЕМПОТЕНТНЫ (rd := f(rt[, rs])),
				 * эмуляция по окну безопасна и при повторном
				 * прохождении. Кодирование:
				 *   seb/seh/wsbh: SPECIAL3 (op 0x1F),
				 *     funct 0x20, sa-поле: 0x10/0x18/0x02;
				 *   rotr:  SPECIAL, funct 0x02 (srl), rs-поле = 1;
				 *   rotrv: SPECIAL, funct 0x06 (srlv), sa-поле = 1.
				 * movn/movz (311 шт.) ядро исполняет само.
				 *
				 * Нестандартные SPECIAL3 funct 0x20 с чужими
				 * sa (0x03/0x14 — 7 слов в образе) НЕ трогаем:
				 * при встрече их выдаст SIGILLD-печать ниже. */
				if ((v >> 26) == 0x1Fu && (v & 0x3Fu) == 0x20u) {
					u32 sa = (v >> 6) & 31u;
					u32 rt = (v >> 16) & 31u;
					u32 rd = (v >> 11) & 31u;
					const unsigned char *tp2 =
					    (const unsigned char *)uc
					    + UCONTEXT_GREG(rt);
					u32 rv = (u32)tp2[0]
					    | ((u32)tp2[1] << 8)
					    | ((u32)tp2[2] << 16)
					    | ((u32)tp2[3] << 24);
					unsigned char *rp = (unsigned char *)uc
					    + UCONTEXT_GREG(rd);
					u32 nv = rv;
					int did = 0;

					if (sa == 0x10u) {         /* seb */
						nv = (u32)(signed char)(rv & 0xFFu);
						did = 1;
					} else if (sa == 0x18u) {  /* seh */
						nv = (u32)(signed short)(rv & 0xFFFFu);
						did = 1;
					} else if (sa == 0x02u) {  /* wsbh */
						nv = ((rv & 0x00FF00FFu) << 8)
						    | ((rv & 0xFF00FF00u) >> 8);
						did = 1;
					}
					if (did) {
						rp[0] = (unsigned char)(nv & 0xFF);
						rp[1] = (unsigned char)((nv >> 8) & 0xFF);
						rp[2] = (unsigned char)((nv >> 16) & 0xFF);
						rp[3] = (unsigned char)((nv >> 24) & 0xFF);
						emul++;
						g_emul_ins++;
						if (ok <= 0)
							emul_last = ok;
					}
				} else if ((v >> 26) == 0x00u
				           && ((v >> 21) & 31u) == 1u
				           && (v & 0x3Fu) == 0x02u) {
					/* rotr rd, rt, sa */
					u32 sa = (v >> 6) & 31u;
					u32 rt = (v >> 16) & 31u;
					u32 rd = (v >> 11) & 31u;
					const unsigned char *tp2 =
					    (const unsigned char *)uc
					    + UCONTEXT_GREG(rt);
					u32 rv = (u32)tp2[0]
					    | ((u32)tp2[1] << 8)
					    | ((u32)tp2[2] << 16)
					    | ((u32)tp2[3] << 24);
					unsigned char *rp = (unsigned char *)uc
					    + UCONTEXT_GREG(rd);
					u32 nv = (sa == 0)
					    ? rv
					    : ((rv >> sa) | (rv << (32u - sa)));

					rp[0] = (unsigned char)(nv & 0xFF);
					rp[1] = (unsigned char)((nv >> 8) & 0xFF);
					rp[2] = (unsigned char)((nv >> 16) & 0xFF);
					rp[3] = (unsigned char)((nv >> 24) & 0xFF);
					emul++;
					g_emul_ins++;
					if (ok <= 0)
						emul_last = ok;
				} else if ((v >> 26) == 0x00u
				           && ((v >> 6) & 31u) == 1u
				           && (v & 0x3Fu) == 0x06u) {
					/* rotrv rd, rt, rs */
					u32 rs = (v >> 21) & 31u;
					u32 rt = (v >> 16) & 31u;
					u32 rd = (v >> 11) & 31u;
					const unsigned char *tp2 =
					    (const unsigned char *)uc
					    + UCONTEXT_GREG(rt);
					const unsigned char *sp2 =
					    (const unsigned char *)uc
					    + UCONTEXT_GREG(rs);
					u32 rv = (u32)tp2[0]
					    | ((u32)tp2[1] << 8)
					    | ((u32)tp2[2] << 16)
					    | ((u32)tp2[3] << 24);
					u32 amt = ((u32)sp2[0]
					           | ((u32)sp2[1] << 8)
					           | ((u32)sp2[2] << 16)
					           | ((u32)sp2[3] << 24)) & 31u;
					unsigned char *rp = (unsigned char *)uc
					    + UCONTEXT_GREG(rd);
					u32 nv = (amt == 0)
					    ? rv
					    : ((rv >> amt) | (rv << (32u - amt)));

					rp[0] = (unsigned char)(nv & 0xFF);
					rp[1] = (unsigned char)((nv >> 8) & 0xFF);
					rp[2] = (unsigned char)((nv >> 16) & 0xFF);
					rp[3] = (unsigned char)((nv >> 24) & 0xFF);
					emul++;
					g_emul_ins++;
					if (ok <= 0)
						emul_last = ok;
				}
			}
			(void)plat_write(g_log_fd, ob, (size_t)oi);

			/* Нашли ins/ext — продолжаем ПОСЛЕ ПОСЛЕДНЕГО
			 * эмулированного слова, а не с reported pc.
			 *
			 * Измерено 2026-10-14 (прогон `34593d8a`): при
			 * продолжении с reported pc у сбоя в `HeapSystem`
			 * поток ПРОПУСТИЛ 0x1BC кода между `ins`-блоком и
			 * pc — инициализация кучи сломалась, и второй SIGILL
			 * случился уже в другом месте той же функции. Ядро
			 * при SIGILL даёт pc с непредсказуемым смещением, и
			 * доверять ему точку продолжения нельзя.
			 *
			 * Идемпотентность ins/ext делает эмуляцию безопасной
			 * и при повторном прохождении, если часть окна уже
			 * исполнялась. Возврат — через `plat_signal_restorer`,
			 * ядро восстановит контекст с исправленным PC. */
			if (emul > 0) {
				unsigned char *pcp =
				    (unsigned char *)uc + UCONTEXT_PC;
				u32 np = (emul_last == -1000)
				    ? pc_v
				    : (u32)((s32)pc_v + emul_last * 4 + 4);

				pcp[0] = (unsigned char)(np & 0xFF);
				pcp[1] = (unsigned char)((np >> 8) & 0xFF);
				pcp[2] = (unsigned char)((np >> 16) & 0xFF);
				pcp[3] = (unsigned char)((np >> 24) & 0xFF);
				g_fault_skipped++;
				return;
			}
		}

		/* Сырой кадр — для первого сбоя прогона и для сбоя, ушедшего ЗА
		 * ОБРАЗ ГОСТЯ.
		 *
		 * Почему второе условие: сбой внутри образа и сбой вне образа — это
		 * разные события, и печатать их по одному правилу нельзя. Вне
		 * образа лежит наш собственный код, и полный дамп регистров там
		 * единственный способ узнать, откуда гость взял адрес возврата
		 * (первый раз такой сбой дал `pc = 0x2CA008D8` внутри
		 * `load_guest_report`). Укороченная `r0..r7` из восьми регистров
		 * для этого недостаточно — в ней нет `$s0..$s7` и `$ra` рядом с
		 * `$sp`.
		 *
		 * Проверка границ — та же, что у пропуска инструкции: по
		 * `mod->base` и `mod->map_size`, без assumptions о базе. */
		if (g_mod != 0 && g_mod->memory != 0 &&
		    (g_fault_total == 1 || pc_v < g_mod->base ||
		     pc_v >= g_mod->base + g_mod->map_size)) {
			/* 4096: с 2026-10-14 дампит 96 слов кадра (до
			 * `sc_cause` включительно), 20 слов `siginfo` и 32
			 * регистра с именами — 1024 уже не хватает. */
			char rb[4096];
			int rn;

			rn = fault_raw_dump(uc, info, g_mod, rb);
			(void)plat_write(g_log_fd, rb, (size_t)rn);
		}

		/* ДИАГНОСТИКА ЗАМЕРА 91 (ef6da1e7): первый сбой в зоне
		 * walk'а 0x08AB0xxx..0x08AB2xxx — дампить ТАБЛИЦУ (это
		 * `$a1` прогона: 0x08BAEFDC, поля +4 массив, +8 count),
		 * сам МАССИВ ([a1+4], элементы по 16 Б: +0 ключ, +4 база
		 * подобъекта, +12 индекс) и ПОЛЯ МЕНЕДЖЕРА аллокатора
		 * 0x08BB7D80+0xC0..0x180 — там лежат бэкенды (+0xD4,
		 * +0xF0, +0x154, +0x168), которые возвращают 0 и из-за
		 * которых гость льёт memset(uncached(NULL)). Яд
		 * 0xC6600BC0 в `$v1` пережил три сборки — статически
		 * производитель не найден, смотрим память по факту. */
		if (g_fault_total == 1 && pc_v >= 0x08AB0000u
		    && pc_v < 0x08AB3000u && g_mod != 0
		    && g_mod->memory != 0) {
			u32 a1 = *(const u32 *)((const unsigned char *)uc
			                        + 40 + 8 * 5);   /* $a1 */
			u32 arrp = 0;
			u32 wa;

			early_log("WA1", a1);
			for (wa = 0; wa < 16u; wa++) {
				u32 ad = a1 + 4u * wa;
				if (ad < g_mod->base
				    || ad + 4u > g_mod->base + g_mod->map_size)
					break;
				early_log("WAT",
				    *(const u32 *)((const unsigned char *)
				                   g_mod->memory
				                   + (ad - g_mod->base)));
			}
			if (a1 >= g_mod->base && a1 + 8u < g_mod->base + g_mod->map_size) {
				arrp = *(const u32 *)((const unsigned char *)
				         g_mod->memory + (a1 + 4u - g_mod->base));
				early_log("WA2", arrp);
				for (wa = 0; wa < 40u; wa++) {
					u32 ad = arrp + 4u * wa;
					if (ad < g_mod->base || ad + 4u
					    > g_mod->base + g_mod->map_size)
						break;
					early_log("WAR",
					    *(const u32 *)((const unsigned char *)
					                   g_mod->memory
					                   + (ad - g_mod->base)));
				}
			}
			for (wa = 0; wa < 40u; wa++) {
				u32 ad = 0x08BB7D80u + 0xC0u + 4u * wa;
				if (ad < g_mod->base
				    || ad + 4u > g_mod->base + g_mod->map_size)
					break;
				early_log("WBM",
				    *(const u32 *)((const unsigned char *)
				                   g_mod->memory
				                   + (ad - g_mod->base)));
			}
			/* Замер 94: флаг готовности (file-init 7, бэкенду
			 * нужен бит 0) и условие VBlank-handshake на МОМЕНТ
			 * сбоя — построитель 0x088C2xxx регистрирует
			 * методы и обнуляет флаг; кто поднимает обратно —
			 * вопрос, на который отвечают эти два слова. */
			/* Замер 96: адрес флага исправлен — прежний был на
			 * 0x40000 выше (повтор исходной ошибки SGSB).
			 * Верный: vaddr 0x3667AC = runtime 0x08B6A7AC,
			 * file-init 7. Зачистка (F_build) по boot-порядку
			 * идёт ПОСЛЕ walk'а — на момент сбоя ждём 7. */
			early_log("WSF",
			    *(const u32 *)((const unsigned char *)g_mod->memory
			                   + (0x08B6A7ACu - g_mod->base)));
			early_log("WSC",
			    *(const u32 *)((const unsigned char *)g_mod->memory
			                   + (0x08B2D7D8u - g_mod->base)));
			/* Замер 95: адрес флага выведен из релокационной
			 * пары и может быть смещён — дампим окрестности
			 * обоих слов целиком. */
			for (wa = 0; wa < 24u; wa++) {
				u32 ad = 0x08B6A780u + 4u * wa;
				if (ad + 4u > g_mod->base + g_mod->map_size)
					break;
				early_log("WN1",
				    *(const u32 *)((const unsigned char *)
				                   g_mod->memory
				                   + (ad - g_mod->base)));
			}
			for (wa = 0; wa < 8u; wa++) {
				u32 ad = 0x08B2D7C0u + 4u * wa;
				early_log("WN2",
				    *(const u32 *)((const unsigned char *)
				                   g_mod->memory
				                   + (ad - g_mod->base)));
			}
			/* Игровой массив объектов ($a2 замера 90):
			 * 10 элементов по 16 Б — базы подобъектов с
			 * ядом живут здесь. */
			for (wa = 0; wa < 40u; wa++) {
				u32 ad = 0x08BB0000u + 4u * wa;
				if (ad + 4u > g_mod->base + g_mod->map_size)
					break;
				early_log("WN3",
				    *(const u32 *)((const unsigned char *)
				                   g_mod->memory
				                   + (ad - g_mod->base)));
			}
			/* Очередь задач на момент сбоя. */
			early_log("WQH",
			    *(const u32 *)((const unsigned char *)g_mod->memory
			                   + (0x0882CEE4u - g_mod->base)));
			early_log("WQT",
			    *(const u32 *)((const unsigned char *)g_mod->memory
			                   + (0x0882CEE8u - g_mod->base)));
		}
	}

	/* Подробный дамп регистров печатается только для первых сбоев: при
	 * пределе 128 он занял бы сотни строк и закопал то, ради чего запускают
	 * прогон, — в том числе `GE_EnQ`. */
	if (g_fault_total <= FAULT_REPORT_MAX && g_log_fd >= 0) {

	/* Смещения, которые могут содержать PC, читаются ВСЕ и печатаются все.
	 *
	 * Раньше печаталось одно поле, выбранное по документации, и этого
	 * оказалось недостаточно: `+0xF0`, помеченное в AGENTS.md как верное,
	 * в кадре гостевого потока содержит `0x1234`, то есть не PC. AGENTS.md
	 * там же отмечает, что кадр гостя не совпал ни с одним измеренным
	 * случаем, — и это подтвердилось. Поле надо ОПРЕДЕЛЯТЬ замером, а не
	 * переносить из чужой пробы: верное то, где лежит адрес образа гостя
	 * (`0x08804000..0x08BBB6EC`).
	 *
	 * Заодно исправлен дефект печати: поле `sig=` выводило `pc` обрезанным
	 * до четырёх разрядов, из-за чего два разных поля выглядели одинаково и
	 * выглядят как «и сигнал, и PC равны 0x1234». */
	for (n = 0; n < (int)(sizeof(probe_off) / sizeof(probe_off[0])); n++) {
		const unsigned char *q =
			(const unsigned char *)uc + probe_off[n];
		u32 v = (u32)q[0] | ((u32)q[1] << 8)
		      | ((u32)q[2] << 16) | ((u32)q[3] << 24);

		buf[i++] = ' ';
		buf[i++] = 'u';
		buf[i++] = (char)('0' + ((probe_off[n] >> 4) & 0xF));
		buf[i++] = (char)('0' + (probe_off[n] & 0xF));
		buf[i++] = '=';
		for (k = 7; k >= 0; k--)
			buf[i++] = digits[(v >> (k * 4)) & 0xF];
	}

	buf[i++] = ' ';
	buf[i++] = 't';
	buf[i++] = 'i';
	buf[i++] = 'd';
	buf[i++] = '=';
	for (k = 7; k >= 0; k--)
		buf[i++] = digits[((u32)plat_gettid() >> (k * 4)) & 0xF];
	buf[i++] = '\n';
	/* Регистры гостя, которых в кадре ЕСТЬ.
	 *
	 * Зачем. `pc` в кадре гостевого потока не указывает на падающую
	 * инструкцию: по нему `lui`, падать не может, а `sw` перед ним при
	 * заведомо корректном `$k0` писала бы внутрь образа гостя, и `si_addr`
	 * был бы не нулём. Значит локализовать падение по PC нельзя, и остаётся
	 * единственный доступный источник — сами регистры.
	 *
	 * Смещения измерены пробой `tests/uctx_layout.c`, а не взяты из
	 * заголовка musl: `gregs[i]` = 40 + 8·i. `$k0`/`$k1` в кадре ОТСУТСТВУЮТ
	 * (пропуск между `$t9`=240 и `$gp`=264), поэтому они не печатаются —
	 * печатать нечего, и «нулевое значение» означало бы «регистра нет».
	 *
	 * Печатаются отдельной строкой, потому что строка уже не помещается. */
	if (g_log_fd >= 0) {
		char rb[128];
		int ri = 0, rk;
		/* `gregs[i]` = 40 + 8·i (проба `tests/uctx_layout.c`), PC = 32.
		 * Значения подставлены числами, а не формулой, — формула здесь
		 * уже была написана неверно один раз. */
		static const unsigned short gregs_of[] = {
			32,   /* $pc  */
			72,   /* $a0  */
			80,   /* $a1  */
			240,  /* $t9  */
			264,  /* $gp  */
			272,  /* $sp  */
			280,  /* $fp  */
			288   /* $ra  */
		};

		for (n = 0; n < (int)(sizeof(gregs_of) / sizeof(gregs_of[0])); n++) {
			const unsigned char *q =
				(const unsigned char *)uc + gregs_of[n];
			u32 v = (u32)q[0] | ((u32)q[1] << 8)
			      | ((u32)q[2] << 16) | ((u32)q[3] << 24);

			rb[ri++] = ' ';
			rb[ri++] = 'r';
			rb[ri++] = (char)('0' + n);
			rb[ri++] = '=';
			for (rk = 7; rk >= 0; rk--)
				rb[ri++] = digits[(v >> (rk * 4)) & 0xF];
		}
		rb[ri++] = '\n';
		(void)plat_write(g_log_fd, rb, (size_t)ri);
	}
	buf[i++] = ' ';
	buf[i++] = 's';
	buf[i++] = 'i';
	buf[i++] = 'g';
	buf[i++] = '=';
	for (k = 3; k >= 0; k--)
		buf[i++] = digits[(((u32)sig) >> (k * 4)) & 0xF];
	buf[i++] = ' ';
	buf[i++] = 'a';
	buf[i++] = 'd';
	buf[i++] = 'd';
	buf[i++] = 'r';
	buf[i++] = '=';
	for (k = 7; k >= 0; k--)
		buf[i++] = digits[(ad_v >> (k * 4)) & 0xF];
	buf[i++] = '\n';
	(void)plat_write(g_log_fd, buf, (size_t)i);
	}

	/* --- восстановление вместо гибели потока ---------------------------
	 *
	 * Раньше обработчик завершал поток, и гость на каждом обращении к
	 * неотображённому адресу умирал. Из-за этого дойти до игрового кода было
	 * нельзя, а локализация падения ничего не давала: поле PC в кадре
	 * гостевого потока указывает на `lui`, который падать не может (проверено
	 * дампом ЗАГРУЖЕННОГО образа), то есть адрес инструкции недостоверен.
	 *
	 * Отсюда решение: вместо выяснения, ГДЕ падает, сделать так, чтобы
	 * падение не было фатальным — пропустить инструкцию и продолжить.
	 *
	 * Отображение страницы по `MAP_FIXED` здесь НЕ применяется: при `si_addr`
	 * внутри образа гостя это уничтожило бы его данные. Пропуск инструкции
	 * обращение просто не выполняет, и для наблюдения этого достаточно.
	 *
	 * PC правится ТОЛЬКО если лежит внутри образа гостя: иначе испортилось
	 * бы не то поле кадра.
	 *
	 * Предел обязателен: без него гость, зациклившийся на одном обращении,
	 * крутился бы вечно и держал `dlclose` при живом коде.
	 * Счётчик увеличен выше, до печати.
	 *
	 * Замер 89 (bd18dff0) изменил ДВЕ вещи в этом пределе:
	 *
	 * 1. Сбои НУЛЕВОЙ страницы (все 4 зеркала PSP, `(addr &
	 *    0x3FFFFFFF) < 0x10000`) в предел НЕ входят. memset
	 *    316 байт по 0x40000000 — это 316 ПОБАЙТОВЫХ сбоев,
	 *    каждый из которых обработчик ниже честно эмулирует;
	 *    прежде 129-й из них убивал главный поток посреди
	 *    заливки (FAULTCAP=81, GONE!=08A9C958). Тот же
	 *    принцип уже применён в SPINDEAD ниже.
	 * 2. Предел стал НА ПОТОК, а не общий: `g_fault_total`
	 *    считался на всех потоках сразу, и невинный поток
	 *    умирал за чужой шторм. Считает `g_thread.faults[]`
	 *    по слоту текущего tid; для потока вне таблицы —
	 *    прежний общий счётчик. */
	{
		u32 fslot;
		int have_fslot = 0;

		if ((ad_v & 0x3FFFFFFFu) >= 0x10000u) {
			for (fslot = 0; fslot < GUEST_THREAD_SLOTS; fslot++) {
				if (g_thread.tid[fslot] != 0
				    && g_thread.tid[fslot] == (u32)plat_gettid()) {
					g_thread.faults[fslot]++;
					have_fslot = 1;
					break;
				}
			}
			if ((have_fslot && g_thread.faults[fslot] > FAULT_RECOVER_MAX)
			    || (!have_fslot && g_fault_total > FAULT_RECOVER_MAX)) {
				if (g_log_fd >= 0) {
					char cb[64];
					int ci = 0, ck;

					cb[ci++] = ' ';
					cb[ci++] = 'F'; cb[ci++] = 'A'; cb[ci++] = 'U';
					cb[ci++] = 'L'; cb[ci++] = 'T'; cb[ci++] = 'C';
					cb[ci++] = 'A'; cb[ci++] = 'P'; cb[ci++] = '=';
					for (ck = 7; ck >= 0; ck--)
						cb[ci++] = digits[(g_fault_total >> (ck * 4)) & 0xF];
					cb[ci++] = ' ';
					cb[ci++] = 's'; cb[ci++] = 'k'; cb[ci++] = 'i';
					cb[ci++] = 'p'; cb[ci++] = ' '; cb[ci++] = '=';
					for (ck = 7; ck >= 0; ck--)
						cb[ci++] = digits[(g_fault_skipped >> (ck * 4)) & 0xF];
					cb[ci++] = '\n';
					(void)plat_write(g_log_fd, cb, (size_t)ci);
				}
				plat_thread_exit();
			}
		}
	}

	/* Обрыв по «подряд на этом pc» — ПОСЛЕ печати компактной строки сбоя,
	 * иначе в отчёте не окажется последнего сбоя перед смертью потока.
	 *
	 * Именно так уже терялась информация: фатальный сбой печатался, а
	 * подробный дамп до него не доходил, и вывод приходилось строить
	 * на неполном отчёте. */
	if (g_fault_spin > FAULT_SPIN_MAX
	    && (ad_v & 0x3FFFFFFFu) >= 0x10000u) {
		/* Нулевая страница (все 4 зеркала PSP) НЕ считается
		 * зацикливанием: `memset(NULL, …)` / `strlen(NULL)`
		 * легально проходят сотни итераций на одном pc, и
		 * NULL-deref обработчик ниже успешно их обрабатывает.
		 * SPINDEAD обязан убивать только настоящие зацикливания. */
		if (g_log_fd >= 0) {
			char cb[64];
			int ci = 0, ck;

			cb[ci++] = ' ';
			cb[ci++] = 'S'; cb[ci++] = 'P'; cb[ci++] = 'I';
			cb[ci++] = 'N'; cb[ci++] = 'D'; cb[ci++] = 'E';
			cb[ci++] = 'A'; cb[ci++] = 'D'; cb[ci++] = '=';
			for (ck = 7; ck >= 0; ck--)
				cb[ci++] = digits[(g_fault_spin >> (ck * 4)) & 0xF];
			cb[ci++] = ' '; cb[ci++] = 't'; cb[ci++] = 'o';
			cb[ci++] = 't'; cb[ci++] = 'a'; cb[ci++] = 'l';
			cb[ci++] = '=';
			for (ck = 7; ck >= 0; ck--)
				cb[ci++] = digits[(g_fault_total >> (ck * 4)) & 0xF];
			cb[ci++] = '\n';
			(void)plat_write(g_log_fd, cb, (size_t)ci);
		}
		plat_thread_exit();
	}

	if (g_mod != 0 && g_mod->memory != 0 &&
	    pc_v >= g_mod->base &&
	    pc_v < g_mod->base + g_mod->map_size &&
	    (sig == 11 /* SIGSEGV */ || sig == 10 /* SIGBUS */)) {
		/* NULL dereference: чтение по адресу 0.
		 *
		 * Ядро отдаёт `pc` как адрес инструкции ПОСЛЕ падающей
		 * (в слоте задержки), поэтому падающая инструкция по
		 * адресу `pc_v - 4`. Декодируем её: если это загрузка
		 * (`lb`/`lh`/`lw`/`lbu`/`lhu`/`lwl`/`lwr`) с базой $zero,
		 * симулируем чтение байта 0 — пишем 0 в приёмник и
		 * продвигаем PC на 4. Это заставляет `strlen(NULL)` /
		 * `strncpy(NULL,…)` вернуть 0 сразу, а не зацикливаться
		 * на последовательных сбоях по адресу 0.
		 *
		 * `jr $zero` аналогично: продвигаем PC на 8 (мимо `jr`
		 * и слота задержки). */
		if ((ad_v & 0x3FFFFFFFu) < 0x10000u && pc_v >= 4) {
			/* NULL dereference: si_addr в нулевой странице.
			 * Ядро отдаёт pc либо как адрес падающей инструкции,
			 * либо как адрес следующей (зависит от вида сбоя и
			 * позиции в слоте задержки). Проверяем оба адреса:
			 * pc-4 и pc. Для загрузки обнуляем приёмник (чтение
			 * из нулевой страницы — всё равно 0) и продвигаем PC.
			 * Это заставляет strlen(NULL)/strncpy(NULL,…) вернуть
			 * 0 сразу, а не зацикливаться на сбоях.
			 *
			 * jr с любым регистром — пропускаем обе инструкции. */
			u32 check_addrs[2] = { pc_v - 4u, pc_v };
			int handled = 0;

			for (int ci = 0; ci < 2 && !handled; ci++) {
				u32 check_pc = check_addrs[ci];
				if (check_pc < g_mod->base
				    || check_pc >= g_mod->base + g_mod->map_size)
					continue;
				const unsigned char *ip =
				    (const unsigned char *)g_mod->memory
				    + (check_pc - g_mod->base);
				u32 instr = (u32)ip[0] | ((u32)ip[1] << 8)
				          | ((u32)ip[2] << 16)
				          | ((u32)ip[3] << 24);
				u32 op = instr >> 26;
				u32 rt = (instr >> 16) & 31u;

				/* Загрузки: lb(0x20) lh(0x21) lwl(0x22) lw(0x23)
				 * lbu(0x24) lhu(0x25) lwr(0x26)
				 * Записи: sb(0x28) sh(0x29) swl(0x2A) sw(0x2B)
				 * swr(0x2E) — запись в нулевую страницу
				 * отбрасывается, приёмник не нужен. */
				if (op == 0x20u || op == 0x21u || op == 0x22u
				    || op == 0x23u || op == 0x24u || op == 0x25u
				    || op == 0x26u
				    || op == 0x28u || op == 0x29u || op == 0x2Au
				    || op == 0x2Bu || op == 0x2Eu) {
					if (op >= 0x20u && op <= 0x26u) {
						/* Загрузка: обнуляем приёмник */
						unsigned char *rtp =
						    (unsigned char *)uc + 40 + 8 * rt;
						rtp[0] = 0;
						rtp[1] = 0;
						rtp[2] = 0;
						rtp[3] = 0;
					}
					/* Продвигаем PC за инструкцию */
					unsigned char *pcp =
					    (unsigned char *)uc + UCONTEXT_PC;
					u32 np = check_pc + 4u;
					pcp[0] = (unsigned char)(np & 0xFF);
					pcp[1] = (unsigned char)((np >> 8) & 0xFF);
					pcp[2] = (unsigned char)((np >> 16) & 0xFF);
					pcp[3] = (unsigned char)((np >> 24) & 0xFF);
					g_fault_skipped++;
					handled = 1;
				}
				if (op == 0x00u && (instr & 0x3Fu) == 0x08u) {
					/* jr — цель вызова недопустима
					 * (иначе бы мы не попали в обработчик),
					 * пропускаем обе инструкции. */
					unsigned char *pcp =
					    (unsigned char *)uc + UCONTEXT_PC;
					u32 np = check_pc + 8u;
					pcp[0] = (unsigned char)(np & 0xFF);
					pcp[1] = (unsigned char)((np >> 8) & 0xFF);
					pcp[2] = (unsigned char)((np >> 16) & 0xFF);
					pcp[3] = (unsigned char)((np >> 24) & 0xFF);
					g_fault_skipped++;
					handled = 1;
				}
			}
			if (handled)
				return;
		}

		unsigned char *pcp = (unsigned char *)uc + UCONTEXT_PC;
		/* Возврат на `pc + 4`, и это решение ПРИНЯТО ПО ИЗМЕРЕНИЮ
		 * продвижения, а не по разбору поля PC.
		 *
		 * Данные противоречивы, и противоречие не разрешено:
		 *
		 *  за `pc` = адрес падающей инструкции говорит проверка MATCH:
		 *  при `pc = 0x08ADC838` слово `AD090008` = `sw $t1,8($t0)` даёт
		 *  эффективный адрес `0x98`, что совпало с `si_addr = 0x98`
		 *  в точности;
		 *
		 *  против него говорят адреса первых сбоев, где по `pc` стоит
		 *  `lui` (0x08A9CA2C) — такая инструкция падать не может, значит
		 *  `pc` указывает на инструкцию ПОСЛЕ падающей.
		 *
		 * Оба наблюдения сделаны на одном поле в одном ядре, поэтому одно
		 * из них неверно, и какое — не установлено.
		 *
		 * Выбор сделан по измеримому последствию, потому что оно важнее
		 * нерешённого вопроса:
		 *
		 *  при `np = pc` гость зацикливается — сбои #7…#81 идут по
		 *  0x08ADC838 с одинаковыми регистрами, то есть падающая
		 *  инструкция исполняется заново и снова падает. Прогресса ноль;
		 *  при `np = pc + 4` тот же гость проходит восемь РАЗНЫХ адресов
		 *  (0x08A9CA2C → 0x08AD6644 → … → 0x08ADF074).
		 *
		 * Значит `np = pc` неверно на практике, даже если неверно по
		 * существу. Цена решения известна: пропускается на одну инструкцию
		 * больше нужного. Она измерима — по испорченным регистрам гостя
		 * видно, чего он лишился.
		 *
		 * Настоящее правило определяется отдельно: если MATCH на `pc`
		 * подтвердится на сбоях, где `si_addr` не равен нулю, то поле
		 * указывает на падающую инструкцию и нужно ставить `pc + 4`;
		 * если нет — падающая по `pc - 4`, и нужно `pc + 4` с пропуском
		 * через одну. */
		u32 np = pc_v + 4;

		pcp[0] = (unsigned char)(np & 0xFF);
		pcp[1] = (unsigned char)((np >> 8) & 0xFF);
		pcp[2] = (unsigned char)((np >> 16) & 0xFF);
		pcp[3] = (unsigned char)((np >> 24) & 0xFF);
		g_fault_skipped++;
		/* Возврат идёт через `plat_signal_restorer`, который делает
		 * `rt_sigreturn`: ядро восстановит контекст целиком, включая
		 * исправленный нами PC. */
		return;
	}

	/* SIGBUS пропускается НАРЯДУ с SIGSEGV.
	 *
	 * Раньше пропускался только `SIGSEGV`, и на этом был построен весь
	 * план: «пусть гость спотыкается и идёт дальше». Но гость упирался в
	 * первый же `SIGBUS` и умирал, то есть план работал ровно до первого
	 * сбоя другого вида — а их в прогоне девять, и восемь из девяти были
	 * `SIGSEGV`.
	 *
	 * Основание, что это тот же класс, а не новый: разобранный фатальный
	 * сбой — `lb $a2,0($a0)` при `$a0 = 0xFFFFFFFF`, то есть гость читает
	 * по указателю `-1`. `SIGBUS` здесь закономерен: `0xFFFFFFFF` лежит в
	 * KSEG1, это адресное пространство ядра, и обращение к нему из
	 * пользовательского режима даёт bus error, а не `SIGSEGV`. Данные
	 * неотображённые в обоих случаях, отличается только вид отказа ядра.
	 *
	 * `SIGILL` (4) НЕ пропускается — это уже не отказ по данным, а
	 * исполнение мусора, и пропуск там лишь отодвинет конец. */
	/* SIGILL — поток завершается.
	 *
	 * Рестарт интерфейса 2026-10-10 приписан здесь `SIGILL`, и это было
	 * НЕПРОВЕРЕННОЕ предположение, позже опровергнутое: настоящей причиной
	 * оказался `SIGBUS`, для которого обработчика не было. Запись исправлена,
	 * потому что неверная версия в коде переживает сама себя.
	 *
	 * Обработчик на `SIGILL` (4) поставлен вместе с остальными: без него
	 * действие по умолчанию убивает ВЕСЬ процесс вместе с `shell`, а это
	 * ровно тот класс отказа, которого мы добиваемся избежать.
	 *
	 * Пропускать `SIGILL` нельзя: недопустимая инструкция не станет
	 * допустимой, и гость лишь посыплет такими же сбоями, пока не упрётся
	 * в предел. Поток завершается — теперь это не фатально для интерфейса. */
	/* Печать ДО смерти: прогон 75 вскрыл, что эта ветка убивала
	 * потоки ГОСТЯ ТИХО (главный игровой и ещё один) — без единой
	 * строки в логе. Теперь последняя инструкция и адрес всегда
	 * видны: если эмуляция выше не справилась, виновник опознан. */
	if (g_log_fd >= 0) {
		char sb_[128];
		int si2 = 0, sk;
		u32 ins = 0;

		if (g_mod != 0 && g_mod->memory != 0
		    && pc_v >= g_mod->base
		    && pc_v + 4 <= g_mod->base + g_mod->map_size) {
			const unsigned char *q =
			    (const unsigned char *)g_mod->memory
			    + (pc_v - g_mod->base);

			ins = (u32)q[0] | ((u32)q[1] << 8)
			    | ((u32)q[2] << 16) | ((u32)q[3] << 24);
		}
		sb_[si2++] = ' '; sb_[si2++] = 'S'; sb_[si2++] = 'I';
		sb_[si2++] = 'G'; sb_[si2++] = 'I'; sb_[si2++] = 'L';
		sb_[si2++] = 'L'; sb_[si2++] = 'D'; sb_[si2++] = '=';
		for (sk = 7; sk >= 0; sk--)
			sb_[si2++] = (char)digits[(pc_v >> (sk * 4)) & 0xF];
		sb_[si2++] = ' '; sb_[si2++] = 'i'; sb_[si2++] = 'n';
		sb_[si2++] = 's'; sb_[si2++] = '=';
		for (sk = 7; sk >= 0; sk--)
			sb_[si2++] = (char)digits[(ins >> (sk * 4)) & 0xF];
		sb_[si2++] = '\n';
		(void)plat_write(g_log_fd, sb_, (size_t)si2);
	}
	plat_thread_exit();
}

/* Переход в гостевую точку входа.
 *
 * Пишется ассемблером по двум причинам. Первая: `$sp` гостя отличается от
 * нашего, а обычная функция восстановит прежний в коде выхода — но выхода у
 * нас нет. Вторая: `$k0` и `$k1` не восстанавливаются ни ядром, ни
 * обработчиками сигнала на MIPS, их нет в кадре сигнала вообще.
 *
 * `$k0` гость читает как входной параметр и по нему пишет. Для точки входа
 * он нулевой: модуль её как указателя не использует, а настоящее значение
 * требуется при запуске потока гостя.
 *
 * Путь выхода — сырой `exit`: завершает только поток. */
__asm__(
	"    .set noreorder\n"
	"    .text\n"
	"    .globl guest_thread_enter\n"
	"    .type guest_thread_enter, @function\n"
	"guest_thread_enter:\n"
	/* Адрес метки входа — ШЕСТОЙ аргумент, читается ДО смены `$sp`.
	 *
	 * Читать его надо здесь: ниже `$sp` уже равен вершине стека гостя, и
	 * смещение `20($sp)` указало бы совсем в другое место. */
	"    lw     $12, 20($sp)\n"  /* t4 = адрес метки, шестой аргумент.
				      * Смещение СОВПАДАЕТ со смещением
				      * записи у вызывающего: `jalr` в o32
				      * НЕ уменьшает `$sp`, адрес возврата
				      * лежит в `$ra`, а не на стеке. */
	"    move   $9, $4\n"        /* t1 = адрес гостевой точки входа */
	"    move   $10, $5\n"       /* t2 = вершина стека гостя */
	"    lw     $11, 16($sp)\n"  /* k0 = пятый аргумент, на стеке по o32.
				      *
				      * Именно 16, БЕЗ сдвига на 8. Прежде
				      * стояло 24, и это было неверно: сдвиг
				      * считался из предположения, что `jalr`
				      * кладёт адрес возврата на стек. На MIPS
				      * o32 он этого не делает — замер на живом
				      * плеере: `lw $11,24($sp)` вернул ровно
				      * то, что `guest_thread` положил в СВОЙ
				      * кадр по `24(sp)` (`sw gp,24(sp)`),
				      * а не пятый аргумент, который лежит
				      * там же по `16(sp)` и равен нулю.
				      * Прежняя «проверка» подтверждала лишь
				      * согласованность двух неверных
				      * сторон и давала правдоподобный
				      * мусор вместо адреса контекста. */
	"    move   $26, $11\n"      /* k0 */
	"    move   $27, $0\n"       /* k1 = 0 */
	/* `$t8` и `$t9` обнуляются обязательно: переход сделан прыжком, и гость
	 * унаследовал бы в `$t9` адрес хостского кода. Свежий поток PSP таких
	 * значений не наследует, поэтому это не подгонка под симптом. */
	"    move   $24, $0\n"       /* t8 = 0 */
	"    move   $25, $0\n"       /* t9 = 0 */
	"    move   $29, $10\n"      /* sp = вершина стека гостя */
	"    move   $4, $6\n"        /* a0 */
	"    move   $5, $7\n"        /* a1 */
	"    la     $31, 1f\n"       /* ra = путь выхода */
	/* МАРКЕР: пишем отличимое значение по адресу из шестого аргумента.
	 *
	 * Зачем. Замер показал, что рабочий поток падает В НАШЕМ C-КОДЕ
	 * (`$pc` в кадре сбоя — адрес внутри `worker_thread`, а `$ra` и `$fp` —
	 * хостовые), и доходит он до этого места не всегда. Метка отвечает на
	 * вопрос, БЫЛ ЛИ гость введён, и различает три случая:
	 *   маркера нет — поток упал ДО прыжка, в самом `worker_thread` или в
	 *                 прологе `guest_thread_enter`, гость не исполнялся ни разу;
	 *   маркер есть, `$ra` гостевой — гость исполнялся;
	 *   маркер есть, `$ra` хостовый — гость вводился, но сбой случился позже.
	 *
	 * Почему не в стеке. Две первые версии теста ставили метку на стек
	 * потока, и обе дали неверный результат по разным причинам:
	 *   `+0xf8` от начала блока контекста — это ровно то место, куда сама
	 *     заливка контекста пишет `0xffffffff`, значение читалось как чужое;
	 *   `вершина − 240` — попадает внутрь кадра гостя: он в первых же
	 *     инструкциях выделяет `addiu $sp,$sp,-1008` и использует весь стек
	 *     до вершины, метка затиралась.
	 * Отдельная глобальная переменная не перезаписывается ни гостем, ни
	 * заливкой, а её адрес известен вызывающему. */
	"    lui    $13, 0x4755\n"   /* 'GU' */
	"    ori    $13, $13, 0x4554\n" /* 'EST' */
	"    sw     $13, 0($12)\n"
	"    jr     $9\n"
	"    nop\n"
	/* Сюда гость попадает, когда его точка входа ВОЗВРАЩАЕТСЯ.
	 *
	 * Проверено на живом плеере: сбой с `si_addr = 0` и `pc` внутри этой
	 * функции означал, что до сюда выполнение доходит. Отметку об уходе
	 * здесь поставить нельзя — ни один регистр не переживает гостя:
	 * попытка передать адрес флага в `$k1` дала запись по нулю. Живость
	 * потока спрашивает хост, `plat_thread_alive`. */
	"1:\n"
	"    li     $4, 0\n"
	"    li     $2, 4001\n"      /* __NR_exit: завершает только поток */
	"    syscall\n"
	"    li     $2, 4001\n"
	"    syscall\n"
	"    .set reorder\n"
	"    .size guest_thread_enter, .-guest_thread_enter\n");

/* Вызвать ОДИН конструктор из `.cplinit` и вернуться.
 *
 * Отдельная функция, потому что у конструктора и у точки входа модуля
 * разный контракт: конструктор возвращается, точка входа — нет. Считать
 * оба одним `guest_thread_enter` нельзя, там на выходе стоит `exit`.
 *
 * Наружу не возвращается ничего: `$ra` гостя мы не восстанавливаем, из
 * гостя нечего восстанавливать. Возврат делается через `jr $ra`, который
 * ставит вызывающий.
 *
 * `$sp` НЕ меняется: у вызывающего своя, выровненная на 8 как требует o32,
 * а гостевой кадр конструктора выделится сам. Главный поток гостя вызывает
 * это до `guest_thread_enter`, то есть на ХОСТОВОМ стеке. */
__asm__(
	"    .set noreorder\n"
	"    .text\n"
	"    .globl guest_call_ctor\n"
	"    .type guest_call_ctor, @function\n"
	"guest_call_ctor:\n"
	/* Сохраняем всё, что обязан беречь callee по o32, ПЕРЕД прыжком.
	 *
	 * Без этого следующий вызов в `guest_thread` читал бы `g_mod->base`
	 * по затертому `$gp`, то есть сбой был бы НАШ, а не гостевой, и
	 * выглядел бы ровно так, как выглядели прежние ложные выводы. */
	"    addiu $29, $29, -48\n"
	"    sw    $16, 0($29)\n"
	"    sw    $17, 4($29)\n"
	"    sw    $18, 8($29)\n"
	"    sw    $19, 12($29)\n"
	"    sw    $20, 16($29)\n"
	"    sw    $21, 20($29)\n"
	"    sw    $22, 24($29)\n"
	"    sw    $23, 28($29)\n"
	"    sw    $28, 32($29)\n"
	"    sw    $30, 36($29)\n"
	"    sw    $31, 40($29)\n"
	"    move   $25, $4\n"        /* t9 = адрес конструктора */
	"    move   $26, $0\n"        /* k0 = 0: конструктору контекста потока
	                               * не полагается, `$a0` на PSP при инициализации
	                               * модуля тоже не значим */
	"    move   $24, $0\n"        /* t8 = 0: гость без PIC его не ставит */
	"    move   $28, $0\n"        /* fp = 0: иначе гость примет за свой кадр */
	"    move   $4, $0\n"         /* a0 = 0 */
	"    move   $5, $0\n"         /* a1 = 0 */
	"    move   $6, $0\n"         /* a2 = 0 */
	"    move   $7, $0\n"         /* a3 = 0 */
	/* `jalr`, а не `jr`: только `jalr` записывает `$ra` адресом следующей
	 * команды, то есть началом блока восстановления ниже. С `jr $25` `$ra`
	 * оставался от `guest_thread`, конструктор возвращался мимо
	 * восстановления, и после КАЖДОГО конструктора `$sp` висел на 48 байт
	 * ниже, а `$s0–$s8`, `$gp`, `$ra` не восстанавливались. Замер на
	 * устройстве: `$gp` == `$s6` == адрес `early_log` вместо `_gp` —
	 * ровно тот слот, который читает `lw $gp,24($sp)` при недостающих 48
	 * байтах. Блок восстановления был мёртвым кодом: ни один переход во
	 * всём модуле не вёл на его первую команду. */
	"    jalr   $25\n"
	"    nop\n"
	/* Сюда конструктор вернулся `jr $ra`. Восстанавливаем сохранённое. */
	"    lw    $16, 0($29)\n"
	"    lw    $17, 4($29)\n"
	"    lw    $18, 8($29)\n"
	"    lw    $19, 12($29)\n"
	"    lw    $20, 16($29)\n"
	"    lw    $21, 20($29)\n"
	"    lw    $22, 24($29)\n"
	"    lw    $23, 28($29)\n"
	"    lw    $28, 32($29)\n"
	"    lw    $30, 36($29)\n"
	"    lw    $31, 40($29)\n"
	"    addiu $29, $29, 48\n"
	"    jr     $31\n"
	"    nop\n"
	"    .set reorder\n"
	"    .size guest_call_ctor, .-guest_call_ctor\n");

extern void guest_call_ctor(u32 fn);

/* `mark_addr` — адрес глобальной переменной, куда ассемблер записывает
 * отметку о прыжке в гостя. Передаётся аргументом, а не берётся из `$gp`:
 * в прологе этой функции `$gp` ещё не выставлен, а адрес глобала известен
 * только вызывающему. */
extern void guest_thread_enter(u32 entry, u32 sp, u32 a0, u32 a1, u32 k0,
                               u32 mark_addr);

/* Обойти `.cplinit`: вызвать каждый конструктор по порядку.
 *
 * Зачем: на настоящей PSP ядро проходит `cplinit` ДО точки входа модуля.
 * Наш загрузчик этого не делал ни разу, и все 161 конструктор модуля
 * молча не выполнялись — то есть статические инициализаторы C++ (глобальные
 * объекты с конструкторами) не отработали, и гость вязался с нулями.
 *
 * Таблица лежит в ЗАГРУЖЕННОМ образе, её указатели уже абсолютные —
 * `find_cplinit` читает секцию после релокаций. Указатель идёт через
 * слово, поэтому шаг 8, а не 4.
 *
 * Порядок обязателен: конструкторы зависят друг от друга, как и на PSP.
 *
 * Вызов идёт ПРЯМО, со стека гостевого потока. Это не выбор, а откат:
 * отдельный поток на конструктор РОНЯЛ `shell`, и замер это показал
 * однозначно — `FOREIGN sig=0004 pc=2CB601DC tid=0x20C2` там же, где
 * `cid=0x20C2`. Поток конструктора умирал от SIGILL, а фильтр
 * `guest_thread_fault()` не знал про него (`tid` не равен `g_guest_pid`
 * и не записан в `g_thread.tid[]`), принимал сигнал за чужой и уходил
 * из обработчика, не тронув PC. SIGILL шёл по умолчанию,
 * `root.log` давал `child process terminated, status = 5` — процесс
 * умирал, интерфейс гас. **Если снова понадобится клон, первым делом
 * записывать его `tid` в `g_thread.tid[]`.** */
/* A/B 2026-10-13: пропуск конструктора с заданным индексом в `.cplinit`.
 *
 * Зачем, измерение, а не версия. Установлено тремя независимыми способами:
 *
 *  1. `atexit` (`0x08AD0B0C`) — чистая вставка в голову: `узел+0 = ГОЛОВА`,
 *     `ГОЛОВА = узел`. Отказ ровно при `узел+0 != 0`.
 *  2. Переменная головы (`vaddr 0x003B1AA0`) пишется во всём госте
 *     РОВНО ОДНОЙ инструкцией — `0x08AD0B48`. Поиск по аддЕНДУ символа
 *     (`lui 0x0002` + `0x4260`) дал 4 обращения, все внутри `atexit`.
 *  3. `atexit(0x324ACC)` собирается в коде РОВНО В ОДНОМ МЕСТЕ —
 *     `0x08805044` в `ctor[00]`; проверено датафлоу по всем 44 вызовам, с
 *     тремя пройденными контролями (`ctor[0]`, `ctor[1]`, `0x08A72EB4`).
 *
 * Но голова после `.cplinit` равна `0x32F874`, а к импорту #7 снова
 * `0x324ACC` — то есть `atexit(0x324ACC)` был вызван повторно, уже из
 * `main`. У `ctor[00]` нет ни одного `jal`, и он не встречается как данные
 * нигде, кроме `.cplinit`.
 *
 * Пропуск `ctor[00]` — проверка удалением: если abort исчезнет, значит
 * конструктор был выполнен дважды; если останется — виноват не он. */
static const u32 g_ctor_ab_skip = 0;

/* Голова списка atexit перед входом в гостя — по каждому потоку отдельно.
 * Цель — атрибуция, см. комментарии у точек записи. */
static u32 g_head_before_main;
static u32 g_head_before_worker;
static u32 g_len_before_main;
static u32 g_len_before_worker;

u32 guest_run_head_before_main(void)   { return g_head_before_main; }
u32 guest_run_head_before_worker(void) { return g_head_before_worker; }
u32 guest_run_len_before_main(void)    { return g_len_before_main; }
u32 guest_run_len_before_worker(void)  { return g_len_before_worker; }

static void run_cplinit(void)
{
	u32 *tab = (u32 *)(uintptr_t)g_mod->cplinit_runtime;
	u32 i;

	if (tab == 0 || g_mod->cplinit_count == 0)
		return;

	if (g_log_fd >= 0) {
		early_log("cpl", g_mod->cplinit_count);
		early_log("cpr", g_mod->cplinit_runtime);
	}

	/* Предел цикла — из модуля, но не выше жёсткой константы: значение
	 * пришло из файла гостя, и на чужом модуле оно может быть каким угодно. */
	for (i = 0; i < g_mod->cplinit_count && i < GUEST_CPLINIT_MAX; i++) {
		u32 fn = tab[i * 2];

		if (fn == 0)
			continue;
		if (i == g_ctor_ab_skip) {
			if (g_log_fd >= 0)
				early_log("csk", i);
			continue;
		}
		if (g_log_fd >= 0)
			early_log("cfn", fn);
		guest_call_ctor(fn);
		/* Голова списка atexit ПОСЛЕ каждого конструктора.
		 *
		 * Регистрации идут внутри `.cplinit`, а цикл конструкторов — наш,
		 * поэтому снимок после каждого шага даёт порядок регистраций без
		 * всякой ловушки и без правки гостевого кода. Именно так найден
		 * замыкающий узел: видно, после какого конструктора голова
		 * начинает указывать внутрь списка.
		 *
		 * Хранится `vaddr` узла, а не адрес переменной, иначе все строки
		 * были бы одинаковыми. */
		if (i < ATEXIT_CTOR_MAX) {
			g_ctor_head[i] = atexit_head_node();
			g_ctor_len[i] = atexit_list_len();
		}
		g_ctor_head_n = (i + 1 > g_ctor_head_n) ? (i + 1) : g_ctor_head_n;
	}
}

/* Пустой поток: пишет отметку и завершается, гостя не касаясь.
 *
 * Нужен, чтобы отделить два объяснения падения. `SIGSEGV` в любом потоке
 * убивает весь процесс, и при запуске гостя падение не говорит, чьё оно:
 * гостевого кода или нашего кода ожидания. Поток без гостя отвечает на это
 * измерением, а не рассуждением — и стоит десятка строк. */
static void probe_thread(void *arg)
{
	int fd = (int)(long)arg;

	plat_write(fd, "thread: PROBE OK\n", 17);
	plat_thread_exit();
}

/* Жив ли хоть один поток, запущенный гостем через `StartThread`.
 *
 * Отдельная функция, потому что их несколько, а `plat_clone` отдаёт tid
 * только в вызывающем — сохранять надо каждый. */
static int guest_threads_alive(void)
{
	u32 slot;

	for (slot = 0; slot < GUEST_THREAD_SLOTS; slot++) {
		if (g_thread.tid[slot] != 0 &&
		    plat_thread_alive((int)g_thread.tid[slot]))
			return 1;
	}
	return 0;
}

/* Тело потока гостя, запущенного самим гостем через `StartThread`.
 *
 * Отдельное от `guest_thread` тело намеренно: у них разные стеки и разные
 * роли. `guest_thread` запускает модуль и уходит, а этот поток — сам
 * игровой, и из него гость уже не возвращается. */
static void worker_thread(void *arg)
{
	u32 *sp = (u32 *)arg;
	u32 slot;

	for (slot = 0; slot < GUEST_THREAD_SLOTS; slot++) {
		if ((unsigned long)arg >= (unsigned long)&g_worker_stack[slot] &&
		    (unsigned long)arg <
		    (unsigned long)&g_worker_stack[slot][GUEST_WORKER_STACK_BYTES])
			break;
	}
	if (slot >= GUEST_THREAD_SLOTS)
		plat_thread_exit();

	if (g_log_fd >= 0) {
		early_log("WORKER", g_thread.entry[slot]);
		early_log("wtid", (u32)plat_gettid());
		early_log("stk", (u32)(uintptr_t)sp);
	}

	/* `$k0` — адрес `SceThreadContext`, а не ноль.
	 *
	 * На PSP в `$k0` ядро кладёт контекст потока, и раскладка известна точно:
	 * `pspthreadman_kernel.h`. Гость его использует — падает он ровно на
	 * `sw s5,4($k0)`, то есть на записи в `gpr[0]`, — значит адрес обязан
	 * быть корректным и записываемым.
	 *
	 * Прежние выводы о `$k0` («подмена значения не сдвинула `pc`») сняты
	 * неверным способом: их получали на сборке, где гость до этого кода не
	 * доходил. Подстановка обязана сопровождаться логом, иначе отличить
	 * «не помогло» от «не применялось» нечем. */
	{
		/* `$k0` — адрес `SceKernelThreadContext`, а не ноль.
		 *
		 * Адрес берётся от ВЕРШИНЫ СТЕКА ПОТОКА, а не из отдельного блока: см.
		 * `thread_ctx_addr()`. Разница принципиальна, и замер её показал —
		 * при отдельном блоке `$k0` до гостя не дошёл (был равен 0), а сам
		 * адрес контекста гость держал в `$s7`.
		 *
		 * `$a0` и `$a1` — `args` и `argp` из `sceKernelStartThread`,
		 * то есть ДЛИНА блока аргументов и указатель на него: точка
		 * входа потока сама режет блок в `argv[]`. Прежде здесь стоял
		 * идентификатор потока, а `$a1` был нулём; гость падал на
		 * `strlen(0)`. Идентификатор при этом остаётся доступен гостю
		 * из `k0 + 0xc0`, то есть адрес контекста.
		 * Обоснование по коду гостя — в `g_thread.args`. */
		u32 k0 = thread_ctx_addr((u32)(uintptr_t)sp, g_thread.next_id);

		if (g_log_fd >= 0)
			early_log("k0", k0);

		/* Голова списка atexit НЕПОСРЕДСТВЕННО перед входом в игровой
		 * поток. Прибор к атрибуции, а не к поиску: по коду гостя
		 * зарегистрировать узел `0x324ACC` некому — адрес собирается
		 * ровно в одном месте, `ctor[00]`, а он на слайде `g_ctor_ab_skip`
		 * пропускается и всё равно узел оказывается в списке.
		 *
		 * Если голова уже сменилась к моменту входа, регистрировал
		 * главный поток; если нет — регистрирует сам игровой. */
		g_head_before_worker = atexit_head_node();
		g_len_before_worker = atexit_list_len();
		if (g_log_fd >= 0) {
			early_log("hdw", g_head_before_worker);
			early_log("ldw", g_len_before_worker);
		}

		guest_thread_enter(g_thread.entry[slot], (u32)(uintptr_t)sp,
		                   g_thread.args, g_thread.argp, k0,
		                   (u32)(uintptr_t)&g_mark_worker);
	}
	/* Гость ВЕРНУЛСЯ из точки входа потока. На PSP это невозможно
	 * без явного Exit/ExitDelete — возврат из entry тоже завершает
	 * поток, но наша обёртка обязана это ЗАФИКСИРОВАТЬ: без журнала
	 * смерть потока невидима (прогон 74: главный игровой 0x56D и
	 * 0x570 мертвы, при ExitThread-count=0 — путь смерти был
	 * неизвестен). Адрес entry и tid — в лог, потом убиваем. */
	if (g_log_fd >= 0) {
		early_log("WDIED", g_thread.entry[slot]);
		early_log("WDTID", (u32)plat_gettid());
	}
	plat_thread_exit();
}

/* Тело потока. Больше ничего не вызывает: после старта библиотека может быть
 * выгружена хостом, и обращаться к нам нельзя. */
static void guest_thread(void *arg)
{
	u32 *sp = (u32 *)arg;

	/* Отметка ДО входа в гостя. Без неё падение внутри
	 * `guest_thread_enter` неотличимо от падения гостевого кода: первых
	 * инструкций мы не видим, а первых вызовов импортов может не быть вовсе. */
	if (g_log_fd >= 0) {
		(void)plat_write(g_log_fd, "guest: THREAD START\n", 20);
		early_log("mtid", (u32)plat_gettid());
	}

	/* A/B 2026-10-13 ОТЫГРАН, переключатель возвращён в 1.
	 *
	 * Ответ измерения: при `0` конструкторов гость доходит до точки входа и
	 * падает по `si_addr = 0` на `0x08A9CA2C`, дальше крутится в
	 * `strlen`-подобном цикле, `abort` НЕ возникает (`iow_count = 0`). При `1`
	 * все 161 конструктор отрабатывают, гость доходит до точки входа и умирает
	 * в `atexit`. Значит дефект в том, что делают конструкторы, а сама
	 * повторная регистрация происходит в `main` — конструкторы логируются при
	 * ВХОДЕ, и все 161 вернулись без сбоя.
	 *
	 * Конструкторы нужны, поэтому возвращаем `1`. Переключатель оставлен
	 * константой, а не удалённым вызовом: иначе компилятор ругается на
	 * неиспользуемую функцию, а ноль предупреждений — требование проекта.
	 * Разбор цепочки — `docs/guest-atexit-abort.md`. */
/* A/B 2026-10-13 (второй отыгран): переключатель в 0.
 *
 * Первый A/B дал ответ, который тогда был прочитан неверно. Измерение
 * `ctor[..].len=` показало: за наш проход список растёт до **40** узлов, из
 * них 22 — те, чьи головы мы наблюдали, и 18 — зарегистрированы теми же
 * конструкторами, но внутри, между регистрациями. **Все 40 — наши**, чужих
 * потоков нет.
 *
 * После нашего прохода гость делает РОВНО ДВЕ регистрации (`0x324ACC`, затем
 * `0x324ADC`) — это узлы `ctor[00]` и `ctor[01]`, то есть гость проходит
 * инициализаторы ПОВТОРНО. Обе законны (их связи были нулевыми), кольцо
 * замыкается, и следующая регистрация отвергается — это и есть abort.
 *
 * Значит двойной проход — наш и гостевой. На настоящей PSP он один.
 * Проверка: `ldm`/`ldw` покажут, растёт ли длина списка без нас. */
static const int g_run_cplinit_for_ab = 0;

	if (g_run_cplinit_for_ab)
		run_cplinit();

	/* Голова списка atexit перед входом в главный поток гостя — вторая
	 * точка той же атрибуции, что `hdw` в `worker_thread`.
	 *
	 * Длина здесь — главный прибор этого прогона: при выключенном нашем
	 * проходе `.cplinit` она покажет, проходит ли гость инициализаторы
	 * сам. Если длина растёт без нас, наш проход не нужен, а двойной
	 * проход и был причиной abort. */
	g_head_before_main = atexit_head_node();
	g_len_before_main = atexit_list_len();
	if (g_log_fd >= 0) {
		early_log("hdm", g_head_before_main);
		early_log("ldm", g_len_before_main);
	}

	/* Блок аргументов загрузки, который ядро PSP передаёт `module_start`
	 * дисковой игры. Длина — с завершающим нулём: точка входа режет блок
	 * по '\0', а границу `args` использует как конец разбора. */
	static const char g_boot_args[] =
		"disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";

	/* `module_start` принимает `(SceSize args, void *argp)` — блок
	 * аргументов загрузки, как на PSP. Прежде здесь передавались нули,
	 * и главный игровой поток после разбора получал `argv[0] = NULL`:
	 * игра сравнивает ПРЕФИКС `argv[0]` с `"disc0:"`, `"wlan0:"`,
	 * `"ms0:"` (три `strncmp` на runtime 0x088C1290..0x088C12C8) и по
	 * нему решает, где лежат её данные. С пустым блоком она падала на
	 * `strncmp(NULL, "disc0:", 6)`.
	 *
	 * Путь — стандартный путь дисковой игры: `module_start` пересылает
	 * блок в `StartThread` без изменений (разобрано по коду), а
	 * `argv[0]` с этим префиксом — единственное, что читает игра.
	 * Полная цепочка — `docs/guest-boot-args.md`. */
	guest_thread_enter(g_mod->base + g_mod->entry,
	                   (u32)(uintptr_t)sp,
	                   (u32)sizeof(g_boot_args),
	                   (u32)(uintptr_t)g_boot_args, 0,
	                   (u32)(uintptr_t)&g_mark_main);
	g_guest_finished = 1;            /* сюда не доходим: поток завершается сам */
}

/* --- запуск ------------------------------------------------------------- */

static unsigned char g_thread_stack[GUEST_STACK_BYTES]
	__attribute__((aligned(16)));

/* Значение `$gp`, которое вычислит пролог `guest_import`.
 *
 * `$gp` обязан быть передан в трамплин числом: сам гость его затирает, и
 * после вызова импорта GOT-слот читался бы не оттуда. Три инструкции
 * опознаются строго — иначе в трамплин попадёт мусор вместо `$gp`. */
static int gp_value(u32 fn, u32 *out)
{
	const u32 *p = (const u32 *)(uintptr_t)fn;
	u32 w0 = p[0], w1 = p[1], w2 = p[2];
	u32 hi, lo;

	if ((w0 & 0xFFFF0000u) != 0x3C1C0000u)   /* lui  $gp,imm */
		return 0;
	if ((w1 & 0xFFFF0000u) != 0x279C0000u)   /* addiu $gp,$gp,imm */
		return 0;
	if (w2 != 0x0399E021u)                    /* addu $gp,$gp,$t9 */
		return 0;
	hi = w0 & 0xFFFFu;
	lo = w1 & 0xFFFFu;
	/* `addiu` расширяет непосредственный операнд ЗНАКОМ: без этого `$gp`
	 * получался на `0x10000` больше. */
	*out = (hi << 16) + (u32)(int32_t)(int16_t)(u16)lo + fn;
	return 1;
}

static int install_trampolines(const GuestImportTable *t, u32 host_fn)
{
	u32 li, ii, index;

	g_hle_count = 0;
	index = 0;
	for (li = 0; li < t->count; li++) {
		for (ii = 0; ii < t->libs[li].count; ii++) {
			GuestStubPatch patch;

			if (index >= HLE_MAX_IMPORTS) {
				/* Молчаливый отказ здесь стоил реального прогона:
				 * `run_start=0xFFFFFFFE` без указания, на каком импорте
				 * остановились. Сообщаем оба числа. */
				early_log("IMPLIM", index);
				early_log("IMPCNT", t->total_imports);
				return 0;
			}
			patch.target = host_fn;
			patch.stub = t->libs[li].imports[ii].stub_runtime;
			/* Метку возврата трамплин пишет сам: это `g_tramp_mark`, куда
			 * он кладёт `$k0` после вызова. Адрес `SceThreadContext`,
			 * который трамплин УСТАНАВЛИВАЕТ в `$k0`, выделяется один раз
			 * и общий для потоков: подставлять надо литерал при сборке
			 * трамплина, а не читать откуда-то на лету. */
			/* Адрес контекста берётся от вершины стека ПОТОКА, а не из пула:
			 * см. `thread_ctx_addr()`. Здесь стек ещё не создан, поэтому
			 * берётся вершина будущего стека первого слота — трамплин
			 * подставляет одно и то же значение при каждом вызове, и важно,
			 * чтобы оно было валидным и записываемым. */
			if (!guest_build_import_trampoline(g_mod, &patch, host_fn,
			                                   g_host_gp, index,
			                                   (u32)(uintptr_t)&g_tramp_mark,
			                                   thread_ctx_addr(
			                                       (u32)(uintptr_t)g_worker_stack
			                                       + GUEST_WORKER_STACK_BYTES
			                                       - 16,
			                                       1))) {
				/* Причина может быть в адресе стаба, в пуле или в самой
				 * функции — она пишет её в `g_mod->error`. Сообщаем всё
				 * нужное, чтобы отличать «нет стаба» от «пул исчерпан». */
				early_log("TRMFAIL", index);
				early_log("TRMSTUB", patch.stub);
				early_log("TRMPOOL", g_mod->tramp_next);
				return 0;
			}
			g_hle_nid[index] = t->libs[li].imports[ii].nid;
			index++;
		}
	}
	g_hle_count = index;
	guest_flush_icache(g_mod->memory, g_mod->map_size);
	return 1;
}

/* Поток без гостя: пишет отметку и завершается.
 *
 * Отдельный шаг потому, что `SIGSEGV` в любом потоке убивает весь процесс.
 * При запуске гостя падение не говорит, чьё оно — гостевого кода или нашего
 * кода ожидания. Пустой поток отвечает на это измерением. */
int guest_run_probe(int fd)
{
	static unsigned char probe_stack[16 * 1024] __attribute__((aligned(16)));
	unsigned long sp_top;
	long pid;

	sp_top = (unsigned long)&probe_stack[sizeof(probe_stack)];
	pid = plat_clone(probe_thread, (void *)(long)fd, sp_top,
	                 PLAT_CLONE_THREAD_FLAGS);
	return (int)pid;
}

/* Построить трамплины и запустить гостя в потоке.
 *
 * Возвращает 0 при успехе, отрицательное значение при отказе с кодом причины:
 * -1 не опознан пролог обработчика, -2 не построены трамплины, -3 не создан
 * поток, -5 не поставился обработчик сигнала.
 *
 * Сам гостевой код отсюда не возвращается, ждать его не нужно: отчёт снимает
 * `guest_run_wait`. */
int guest_run_start(GuestModule *m, const GuestImportTable *t, int log_fd)
{
	unsigned long sp_top;
	long pid;

	if (GUEST_EXEC == 0)
		return -4;                 /* запуск запрещён: см. GUEST_EXEC */

	g_mod = m;
	g_log_fd = log_fd;
	memset(&g_run, 0, sizeof(g_run));
	g_thread_info = 0;
	g_tramp_mark = 0;
	g_mark_main = 0;
	g_mark_worker = 0;
	g_guest_calls = 0;
	g_time_to_exit = 0;
	g_guest_finished = 0;
	g_fault_total = 0;
	g_fault_skipped = 0;
	g_fault_spin = 0;
	g_fault_spin_pc = 0;
	g_guest_fault_sig = 0;

	if (!gp_value((u32)(uintptr_t)&guest_import, &g_host_gp))
		return -1;
	if (!install_trampolines(t, (u32)(uintptr_t)&guest_import))
		return -2;

	/* СГS-ДИАГНОСТИКА: структура 316-байтных элементов.
	 *
	 * Прогоны 86: 5 новых SIGBUS в 0x08AB-зоне — гость
	 * читает sparse-массив (элементы 0x13C байт) по цепочке
	 * `база+4 → массив`, `база+8 → count`, `элемент+4 →
	 * подобъект`, `элемент+12 → индекс`, и подобъект-база
	 * приходит мусорной (NULL/-1). Пара lui/addiu на
	 * 0x08AB202C/0x08AB2034 (file vaddr 0x2AE02C/34)
	 * указывает на БАЗУ — после релокаций читаем её
	 * прямо из образа.
	 *
	 * Замер 89 (сборка bd18dff0): прежняя формула добавляла
	 * `+4` к старшей половине и давала 0x08BEEFDC — на
	 * 0x40000 выше реальной базы. Релоцированная пара
	 * вычисляет hi=0x8BB, lo=0xEFDC (знаково −0x1024):
	 * (0x8BB<<16) − 0x1024 = 0x08BAEFDC — .bss внутри
	 * RW-сегмента (0x08B91840..0x08BBB6EC), за file-концом
	 * сегмента, то есть таблица изначально нулевая. Тот же
	 * адрес виден в дампе кадра сбоя (R20=0x08BAEFDC).
	 * Заправка по смещённой базе уходила в часть-арену
	 * (0x08BEEFDC ≥ 0x08BCC000) впустую: реальная таблица
	 * оставалась пустой, диспетчер аллокатора (0x08A38180)
	 * получал 0 от обоих бэкендов и шёл заливать
	 * memset(uncached(NULL)=0x40000000, 0, 0x13C). */
	if (g_log_fd >= 0 && g_mod != 0 && g_mod->memory != 0) {
		u32 hi_v, lo_v, base;
		u32 va_hi = 0x08AB202Cu - g_mod->base;
		u32 va_lo = 0x08AB2034u - g_mod->base;

		hi_v = *(const u32 *)((const unsigned char *)g_mod->memory + va_hi);
		lo_v = *(const u32 *)((const unsigned char *)g_mod->memory + va_lo);
		base = ((hi_v & 0xFFFF) << 16) + ((s32)(s16)(lo_v & 0xFFFF));
		early_log("SGSB", base);
		/* Поля структуры: +4 = массив, +8 = count */
		early_log("SGSA", *(const u32 *)((const unsigned char *)g_mod->memory + base - g_mod->base + 4));
		early_log("SGSN", *(const u32 *)((const unsigned char *)g_mod->memory + base - g_mod->base + 8));

		/* ЗАПОЛНЯЕМ СТРУКТУРУ МИНИМАЛЬНО-ВАЛИДНЫМИ ДАННЫМИ.
		 *
		 * Замер 87: SGSA=0, SGSN=0 — таблица ПУСТАЯ. На ПСП её
		 * заполняет ЗАГРУЖЕННЫЙ МОДУЛЬ (SAS/Atrac, попавший на
		 * 0x08BE0000-зону через LoadModule). Наша заглушка
		 * LoadModule «успех» ничего не грузит — 5 SIGBUS
		 * (0x08AB0C28..0x08AB20AC) гибнут на NULL-подобъектах.
		 *
		 * Заполняем: массив = part_alloc(64КБ), count=1,
		 * элемент[0]: +4 = подобъект-база (part_alloc 316б),
		 * +12 = индекс 0. Гость читает валидную таблицу. */
		{
			u32 arr_off = base - g_mod->base + 4;
			u32 cnt_off = base - g_mod->base + 8;
			u32 arr, elem, sub;
			u32 arr_uid, sub_uid;
			unsigned char *mem = (unsigned char *)g_mod->memory;

			/* part_alloc возвращает ДЕСКРИПТОР (slot+1),
			 * НЕ указатель! Замер 88: SGSF=1, SGSS=2 — гость
			 * прочитал их как адреса → фолты по 1. Указатель
			 * даёт part_head(дескриптор) — как в PSP-контракте
			 * AllocPartitionMemory → GetBlockHeadAddr. */
			arr_uid = part_alloc(64 * 1024);
			sub_uid = part_alloc(0x13C);
			arr = part_head(arr_uid);
			sub = part_head(sub_uid);
			if (arr != 0 && sub != 0) {
				elem = arr;
				*(u32 *)(mem + arr_off) = arr;
				*(u32 *)(mem + cnt_off) = 1;
				/* элемент[0] = 16 байт: +4 подобъект-база,
				 * +8 ?, +12 индекс */
				*(u32 *)(mem + elem - g_mod->base + 4) = sub;
				*(u32 *)(mem + elem - g_mod->base + 8) = 0;
				*(u32 *)(mem + elem - g_mod->base + 12) = 0;
				early_log("SGSF", arr);
				early_log("SGSS", sub);
			}
		}
	}

	sp_top = (unsigned long)&g_thread_stack[sizeof(g_thread_stack)];
	/* Второй аргумент — указатель на вершину стека потока: именно его
	 * `plat_clone` кладёт в `$sp` ребёнка. Передаётся как значение, а не
	 * как `void *`: смысл не в разыменовании, а в самом адресе. */
	/* Обработчик ставится ТОЛЬКО на время прогона гостя и снимается сразу.
	 *
	 * Иначе сигнал пришёл бы и от кода самого `shell`, и мы вышли бы из
	 * потока, скрыв чужую ошибку. Окно узкое, побочный эффект ограничен.
	 *
	 * Ставится на ТРИ сигнала, а не на один. На MIPS: 4 = SIGILL,
	 * 10 = SIGBUS, 11 = SIGSEGV. Понадобились все три: с обработчиком
	 * только на SIGSEGV гость после пропусков исполнил недопустимую
	 * инструкцию, SIGILL ушёл по умолчанию и УБИЛ ВЕСЬ ПРОЦЕСС вместе с
	 * интерфейсом. Обработчик обязан быть на всех сигналах, которые гость
	 * может вызвать, иначе «защита потока» защищает только часть случаев. */
	{
		static const int fatal_sigs[3] = { 11 /* SIGSEGV */,
		                                  10 /* SIGBUS  */,
		                                  4  /* SIGILL */ };
		unsigned si;

		for (si = 0; si < 3; si++) {
			if (plat_signal_raw(fatal_sigs[si],
			                    (void *)guest_fault_handler,
			                    0x8 /* SA_SIGINFO на MIPS */) != 0)
				return -5;
		}
	}

	/* Сигнал семплирования PC: тот же ucontext, тот же приём
	 * «все кандидаты смещения PC», но обработчик НЕ меняет
	 * контекст — гость продолжает как ни в чём не бывало.
	 * Снимается в guest_run_wait вместе с остальными. */
	if (plat_signal_raw(14 /* SIGALRM */,
	                    (void *)guest_samp_handler,
	                    0x8 /* SA_SIGINFO */) != 0)
		return -5;

	pid = plat_clone(guest_thread, (void *)sp_top, sp_top,
	                 PLAT_CLONE_THREAD_FLAGS);
	if (pid < 0) {
		(void)plat_signal_reset(11);
		return -3;
	}
	g_guest_pid = (int)pid;
	return 0;
}

/* Ждёт, пока гость выполнит `want` вызовов импортов либо завершится.
 *
 * Нужно, чтобы отчёт снимался после работы гостя: поток получает процессор
 * не мгновенно, и без ожидания счётчик был бы нулевым при perfectly
 * работающем госте.
 *
 * Признак смерти спрашиваем у ядра (`plat_thread_alive`), а не берём из C:
 * гость уходит сырым `exit` из ассемблерной точки выхода, минуя C, и флаг
 * там выставить нечем. Проверяются ВСЕ потоки гостя, а не только создающий:
 * главный умирает первым, игровой живёт дольше, и выход при живом игровом
 * потоке означает `dlclose` под работающим кодом. */
void guest_run_wait(u32 want, u32 max_ms)
{
	u32 ms;

	for (ms = 0; ms < max_ms; ms++) {
		if ((u32)g_guest_calls >= want)
			break;
		/* НЕ выходим по первому сбою.
		 *
		 * Раньше здесь стояло `if (guest_run_fault()) break;`, и это было
		 * верно, пока обработчик завершал поток. Теперь обработчик на SIGSEGV
		 * возвращается и гость продолжает работать, поэтому выход по сбою
		 * означал бы: вернуться из плагина, пока гость ещё исполняется, —
		 * а хост сразу после этого делает `dlclose`, то есть выгружает нашу
		 * библиотеку ПОД РАБОТАЮЩИМ КОДОМ гостя. Именно это и привело к
		 * рестарту интерфейса.
		 *
		 * Теперь единственное условие выхода — гость действительно умер,
		 * и это спрашивается у ядра, а не берётся из C. */
		if (!plat_thread_alive(g_guest_pid) && !guest_threads_alive()) {
			g_guest_finished = 1;
			break;
		}
		/* Насос суб-прерываний (замер 98: перенесён СЮДА с
		 * паркера — если тик-обработчик зависнет в гостевом
		 * спине, паркер продолжит писать семплы, и заморозка
		 * будет видна вживую, а не остановкой отчёта).
		 * Период ~15 мс = VBlank. Лог SIE/SIX — первые 16
		 * входов: SIE без SIX = обработчик не вернулся. */
		if ((ms & 15u) == 0u)
			si_fire();
		/* Грэйн-комплитер: раз в ~46 мс (60 Гц тик ×3 ≈ грейн
		 * 2048 сэмплов на 44100) пометить буфер дренажа
		 * готовым, как это сделал бы Media Engine. Поле
		 * [obj+0x27C] — аргумент аксессуара 0x08AB7228
		 * (addiu s1,a0,636 в цикле 0x08AB43D0). */
		/* Замер 104: одноразовый дамп объекта дренажа —
		 * различает две гипотезы GRM=0: (A) g_drain_obj не
		 * снимается (неверен офсет $s0 в кадре семплера —
		 * тогда DGO=0), (B) объект валиден и поле [obj+0x27C]
		 * ненулевое (семантика «поставлен», ME должен
		 * потреблять — тогда DGO валиден и DW-слова покажут
		 * протокол). */
		if (g_drain_spin >= 3u && g_drain_dumped == 0
		    && g_mod != 0 && g_mod->memory != 0) {
			u32 o = g_drain_obj;
			u32 wk;

			static const char *dwt[16] = {
				"DW0", "DW1", "DW2", "DW3", "DW4", "DW5",
				"DW6", "DW7", "DW8", "DW9", "DWA", "DWB",
				"DWC", "DWD", "DWE", "DWF"
			};

			g_drain_dumped = 1u;
			early_log("DGO", o);
			for (wk = 0; wk < 16u; wk++) {
				u32 ad = o + 0x270u + 4u * wk;

				if (o == 0 || !guest_addr_direct_ok(ad))
					break;
				early_log(dwt[wk],
				    *(volatile u32 *)(uintptr_t)ad);
			}
		}
		/* Замер 108: аудио-прерывание — поднять звуковой
		 * семафор (тот, что ждёт зона 0x08AB60D0). На PSP
		 * это делает HW по готовности буфера. */
		if ((ms % 46u) == 0u && g_sound_sema != 0
		    && g_sound_sema >= 1u && g_sound_sema <= SEMA_SLOTS
		    && g_sema[g_sound_sema - 1u].used != 0) {
			g_sema[g_sound_sema - 1u].count++;
			g_sound_signals++;
		}
		/* Замер 106: ПЕРИОДИЧЕСКИЙ комплитер. Замер 105 показал:
		 * одна пометка разомкнула весь конвейер — игра прошла
		 * 50М вызовов и жила до конца окна. Пометка каждые
		 * ~46 мс при нулевом поле = постоянный «тик Media
		 * Engine» (грейн 2048/44100). Лог — первые 16. */
		if ((ms % 46u) == 0u && g_drain_obj != 0) {
			u32 obj = g_drain_obj;
			u32 fld = obj + 0x27Cu;

			/* Замер 109: динамический дамп протокола грейна —
			 * на каждом тике (первые 60) печатать счётчик,
			 * поле и состояние: если игра потребляет, счётчик
			 * [0x278] уменьшается, поле [0x27C] циклит
			 * 0→1→0, состояние [0x2A8] меняется. Стоит на
			 * месте — значит гейт в другом месте. */
			{
				static u32 dyn_n;

				if (dyn_n < 60u && g_log_fd >= 0
				    && guest_addr_direct_ok(obj + 0x2A8u)) {
					dyn_n++;
					early_log("DYC",
					    *(volatile u32 *)(uintptr_t)(obj + 0x278u));
					early_log("DYF",
					    *(volatile u32 *)(uintptr_t)(obj + 0x27Cu));
					early_log("DYS",
					    *(volatile u32 *)(uintptr_t)(obj + 0x2A8u));
				}
			}
			if (guest_addr_direct_ok(fld)
			    && *(volatile u32 *)(uintptr_t)fld == 0u) {
				*(volatile u32 *)(uintptr_t)fld = 1u;
				g_drain_marks++;
				if (g_drain_marks <= 16u && g_log_fd >= 0)
					early_log("GRM", obj);
			}
		}
		plat_sleep_ms(1);
	}
	/* Окно закрыто: обработчик снят сразу, чтобы сигнал от кода самого
	 * `shell` не пришёл к нему и не был скрыт нашим выходом из потока. */
	(void)plat_signal_reset(11);
	(void)plat_signal_reset(10);
	(void)plat_signal_reset(4);
	(void)plat_signal_reset(14);   /* семплирование PC */

	/* Разбудить ВСЕ парковки SleepThreadCB и дать им выйти.
	 *
	 * Зачем: цикл доставки колбеков в SleepThreadCB живёт в
	 * хост-потоке ГОСТЯ внутри нашей библиотеки. Хост после
	 * возврата отсюда делает dlclose — выгрузить библиотеку
	 * под живым циклом нельзя (грабля сборки bc5641ea:
	 * рестарт-луп интерфейса). Флаг + пауза дают циклу
	 * увидеть флаг (чанк сна 50 мс), вернуться в гостя и
	 * дойти до ExitDeleteThread, который теперь честно
	 * завершает поток. 150 мс = три чанка — с запасом. */
	g_time_to_exit = 1;
	plat_sleep_ms(150);
}

/* --- сведения о сбое ---------------------------------------------------- */

int guest_run_fault(void)
{
	return (g_guest_fault_sig != 0) ? 1 : 0;
}

u32 guest_run_fault_pc(void)
{
	return g_guest_fault_pc;
}

u32 guest_run_fault_addr(void)
{
	return g_guest_fault_addr;
}

u32 guest_run_fault_sig(void)
{
	return g_guest_fault_sig;
}

/* --- доступ для отчёта ------------------------------------------------- */

u32 guest_run_calls(void)      { return (u32)g_guest_calls; }

u32 guest_run_nid_count(u32 i)
{
	return (i < NID_COUNT_SLOTS) ? g_nid_count[i] : 0;
}

u32 guest_run_nid_ra(u32 i)
{
	return (i < NID_COUNT_SLOTS) ? g_nid_ra[i] : 0;
}

u32 guest_run_nid_a0(u32 i)
{
	return (i < NID_COUNT_SLOTS) ? g_nid_a0[i] : 0;
}

u32 guest_run_recent_nid(u32 i)
{
	return (i < 32) ? g_recent_nid[i] : 0;
}

u32 guest_run_recent_ra(u32 i)
{
	return (i < 32) ? g_recent_ra[i] : 0;
}

u32 guest_run_recent_a0(u32 i)
{
	return (i < 32) ? g_recent_a0[i] : 0;
}
u32 guest_run_unknown(void)    { return g_run.unknown; }
u32 guest_run_tramp(void)      { return g_tramp_mark; }

/* Отметки о прыжке в гостя, по одной на поток: `0x47554554` — состоялся,
 * `0` — нет. Глобальные, а не поля в стеке: см. `g_mark_main`. */
u32 guest_run_mark_main(void)    { return g_mark_main; }
u32 guest_run_mark_worker(void)  { return g_mark_worker; }
u32 guest_run_overflow(void)   { return g_run.log_overflow; }
u32 guest_run_limit_hit(void)  { return g_run.limit_hit; }
u32 guest_run_hle_count(void)  { return g_hle_count; }
u32 guest_run_iow_count(void)  { return (u32)g_iow_count; }
int  guest_run_pid(void)       { return g_guest_pid; }
int  guest_run_finished(void)  { return g_guest_finished; }

/* --- список atexit гостя, только чтение --------------------------------- */

/* Смещения внутри функции регистрации деструктора гостя.
 *
 * `runtime 0x08AD0B0C` = `vaddr 0x2CCB0C`. Функция начинается с
 * `addiu $sp,$sp,-0x10`, а `lui $s1,0x2` / `lw $a0,0x4260($s1)` адресуют
 * голову списка.
 *
 * Проверено по `.rel.text`: по `0x2CCB28` стоит `R_MIPS_HI16` (тип 0x05),
 * по `0x2CCB2C` — `R_MIPS_LO16` (тип 0x06), символ у них один (номер 256).
 * То есть это обычная пара HI16/LO16, у которой младшая половина стоит
 * **не в addiu, а в непосредственном поле `lw`**.
 *
 * Отсюда важное: обе половины надо брать из ЗАГРУЖЕННОГО образа. Значение
 * `0x4260` из файла — это адденд, а после релокации там лежит
 * `(A + S) & 0xFFFF`, и для `S`, не кратного 64 КиБ, оно другое. Ровно
 * эта ошибка — «сложить старшую половину с аддендом из файла» — уже стоила
 * одного прогона на плеере (5908 неверных указателей), поэтому здесь она
 * запрещена явно. */
#define ATEXIT_HEAD_HI_VADDR  0x0002CCB28u
#define ATEXIT_HEAD_LO_VADDR  0x0002CCB2Cu
#define ATEXIT_HEAD_FILE_LO   0x00004260u

/* Верхняя граница обхода: и размер ответа, и размер отметок визитов. */
#define ATEXIT_MAX 128

/* Причины, по которым адрес головы может не получиться.
 *
 * Без них все пути `return 0` неразличимы, и отчёт не отвечает на
 * вопрос «почему». Это ровно тот дефект, который запрещён: значение,
 * которого может не быть, обязано иметь метку «недействительно», иначе
 * ноль выдаётся за «адрес равен нулю». */
#define ATEXIT_WHY_NOMAP 1u     /* отображение гостя недоступно */
#define ATEXIT_WHY_RANGE 2u     /* сами инструкции вне отображения */
#define ATEXIT_WHY_OUT   3u     /* вычисленный адрес вне отображения */
#define ATEXIT_WHY_OK    4u     /* адрес получен */

/* Сырые половины пары HI16/LO16 в ЗАГРУЖЕННОМ образе.
 *
 * Возвращает причину (`ATEXIT_WHY_*`), а половины кладёт по указателям,
 * чтобы отчёт показал их независимо от того, сложился ли адрес. Именно
 * так выяснилось, что пара в файле и пара в памяти расходятся: без
 * печати сырых значений отчёт дал бы `0x00000000` и ничего не объяснил. */
u32 guest_run_atexit_raw(u32 *hi_out, u32 *lo_out)
{
	const u32 *ins;
	u32 hi = 0, lo = 0;

	if (hi_out != 0)
		*hi_out = 0;
	if (lo_out != 0)
		*lo_out = 0;
	if (g_mod == 0 || g_mod->memory == 0)
		return ATEXIT_WHY_NOMAP;
	if (ATEXIT_HEAD_LO_VADDR + 4u > g_mod->map_size)
		return ATEXIT_WHY_RANGE;
	ins = (const u32 *)((const unsigned char *)g_mod->memory
	                    + ATEXIT_HEAD_HI_VADDR);
	hi = ins[0] & 0xFFFFu;
	lo = ins[1] & 0xFFFFu;
	if (hi_out != 0)
		*hi_out = hi;
	if (lo_out != 0)
		*lo_out = lo;
	return ATEXIT_WHY_OK;
}

/* Голова списка atexit как `vaddr`, 0 если адрес получить не удалось.
 *
 * ЗАМЕР 2026-10-13, который переписал эту функцию. Первая версия складывала
 * половины пары HI16/LO16 и сравнивала результат с `map_size`, то есть с
 * РАЗМЕРОМ отображения, а не с его границей. Отчёт дал
 * `atexit_head = 0`, и это выглядело как «голова списка пуста».
 *
 * На деле пара в ЗАГРУЖЕННОМ образе даёт `0x08BB5AA0`, а это
 * РАНТАЙМ-адрес: за вычетом базы получается `0x003B1AA0`, что попадает
 * ровно в `.bss` (`0x395740..0x3B76EC`). То есть адрес был верный всё это
 * время, а ломалась единица измерения в сравнении.
 *
 * Урок: у адреса в отчёте обязана быть названа единица. «Голова равна нулю»
 * и «адрес не удалось получить» — разные утверждения, и путать их нельзя
 * тем более, что нулевое значение в списке вполне законно.
 *
 * Ноль здесь означает ПУСТОЙ список, а не ошибку; различает их код
 * причины, который печатается рядом. */
u32 guest_run_atexit_head(void)
{
	u32 hi = 0, lo = 0, addr;
	s32 slo;

	if (guest_run_atexit_raw(&hi, &lo) != ATEXIT_WHY_OK)
		return 0;
	/* Знаковое расширение обязательно: без него адрес уезжает на 64 КиБ
	 * ровно в тех случаях, когда младшая половина `>= 0x8000`, то есть
	 * примерно в половине модуля. */
	slo = (s32)(s16)lo;
	addr = (hi << 16) + (u32)slo;
	/* Граница отображения, а не его размер: `base <= addr` и
	 * `addr + 4 <= base + map_size`. Именно так уже сделано в проверке
	 * сбоя, где несовпадение единиц стоило падения обработчика. */
	if (addr < g_mod->base || addr + 4u > g_mod->base + g_mod->map_size)
		return 0;
	return addr - g_mod->base;
}

/* Значение младшей половины пары в файле — только для сверки в отчёте. */
u32 guest_run_atexit_file_lo(void)
{
	return ATEXIT_HEAD_FILE_LO;
}

/* Узлы списка atexit по порядку, от головы. Возвращает число записанных.
 *
 * Список singly-linked: поле `next` лежит по смещению 0 в узле — это видно
 * из функции регистрации (`sw $a0,($s0)` / `lw $a1,($a0)`).
 *
 * Обход ограничен ТРЕМЯ способами, и это важно: `max`, проверкой попадания
 * в отображение и проверкой самопетли. Отметки визитов по адресу НЕ
 * делаются — массив на `ATEXIT_MAX` байт индексировался бы величиной
 * `узел >> 2`, а она для гостя доходит до `0xF4000`, то есть это был бы
 * выход за массив на пять порядков. Цикл длиной больше `max` не мешает:
 * повторяющиеся узлы просто выведутся повторно, а выход всё равно
 * ограничен `max` (Power of Ten, правило 2).
 *
 * Значения — `vaddr` гостя, ноль означает «конец списка». */
u32 guest_run_atexit_nodes(u32 *out, unsigned max)
{
	const unsigned char *mem;
	u32 head, cur, n;

	if (out == 0 || max == 0 || g_mod == 0 || g_mod->memory == 0)
		return 0;
	head = guest_run_atexit_head();
	if (head == 0)
		return 0;
	if (max > ATEXIT_MAX)
		max = ATEXIT_MAX;
	mem = (const unsigned char *)g_mod->memory;

	/* Сдвиг на один, из-за которого обход выдавал мусор.
	 *
	 * `guest_run_atexit_head()` возвращает АДРЕС переменной «голова», а не
	 * её значение. Первый узел списка лежит по этому адресу, то есть его
	 * надо прочитать. Первая версия начинала с самого адреса и печатала
	 * первым элементом `0x003B1AA0` — адрес переменной, — а настоящий
	 * первый узел уезжал на позицию `1`. */
	cur = ((const u32 *)(mem + head))[0];
	if (cur < g_mod->base)
		return 0;
	cur -= g_mod->base;

	for (n = 0; n < max && cur != 0 && cur + 4u <= g_mod->map_size; ) {
		const u32 *node;
		u32 next;

		node = (const u32 *)(mem + cur);
		out[n++] = cur;
		/* `next` лежит в узле РАНТАЙМ-адресом, а `cur` у нас `vaddr`,
		 * поэтому переводим, иначе почти каждая ссылка окажется за
		 * границей отображения. */
		if (node[0] < g_mod->base)
			break;
		next = node[0] - g_mod->base;
		if (next + 4u > g_mod->map_size || next == cur)
			break;
		cur = next;
	}
	return n;
}

/* Снимок списка atexit в глобальный буфер.
 *
 * Снимок берётся В МОМЕНТ ПЕЧАТИ abort'а, из обработчика `IoWrite`.
 *
 * ОТЗОВАННОЕ УТВЕРЖДЕНИЕ: снимок был нужен потому, что якобы «к моменту
 * отчёта список уже очищен `exit`». Это неверно — на прогоне список
 * содержал не меньше 16 узлов и в отчёте тоже. Утверждение было выведено
 * из `atexit_head = 0`, а ноль тогда означал не пустой список, а сломанную
 * диагностику: сравнение шло с РАЗМЕРОМ отображения вместо его ГРАНИЦЫ.
 * То есть неверное значение было истолковано как измерение — ровно то,
 * что запрещено.
 *
 * Снимок в `IoWrite` оставлен не потому, что так надо, а потому, что
 * момент заведомо предшествует `exit` и даёт состояние списка именно в
 * момент отказа. Стоимость нулевая, вызывается один раз.
 *
 * Пишет в глобалы, потому что вызывается из гостевого потока, а отчёт
 * читает из другого. Число узлов ограничено сверху `ATEXIT_SNAP_MAX`.

 * Значение `0xFFFFFFFF` в счётчике означает «снимок ещё не снимался» —
 * поэтому оно и отличается от нуля, который законно означает пустой
 * список. Переменные объявлены выше, у обработчика `IoWrite`. */

void guest_run_atexit_snapshot(void)
{
	u32 head, cur, n = 0;

	g_atexit_snap_n = 0xFFFFFFFFu;     /* «ещё не снималось» */
	g_atexit_snap_head = 0;
	if (g_mod == 0 || g_mod->memory == 0)
		return;
	head = guest_run_atexit_head();
	g_atexit_snap_head = head;
	if (head == 0 || head + 4u > g_mod->map_size)
		return;
	/* Первый узел — хранящееся в переменной значение, а не её адрес;
	 * иначе первым элементом оказывается сама переменная. */
	cur = ((const u32 *)((const unsigned char *)g_mod->memory + head))[0];
	if (cur < g_mod->base)
		return;
	cur -= g_mod->base;
	for (; n < ATEXIT_SNAP_MAX && cur != 0 && cur + 4u <= g_mod->map_size; ) {
		u32 next;

		g_atexit_snap[n] = cur;
		next = ((const u32 *)((const unsigned char *)g_mod->memory + cur))[0];
		/* Сохраняем и СЫРОЕ значение поля: без него кольцо в списке и
		 * обрыв цепочки выглядят одинаково, а различать их приходится по
		 * догадке. */
		g_atexit_snap_next[n] = next;
		n++;
		if (next < g_mod->base)
			break;
		next -= g_mod->base;
		if (next + 4u > g_mod->map_size || next == cur)
			break;
		cur = next;
	}
	g_atexit_snap_n = n;
}

/* Чтение N слов образа гостя по `vaddr` — только для отчёта.
 *
 * Нужно увидеть РАНТАЙМ-значения вокруг узла списка: в файле там нули, а
 * пишет туда конструктор, и по файлу судить нельзя.
 * Возвращает число прочитанных слов; ноль означает «не прочитано» и
 * отличим от «прочитаны нули» по самому числу. */
u32 guest_run_peek(u32 vaddr, unsigned n, u32 *out)
{
	u32 got = 0;

	if (out == 0 || n == 0 || g_mod == 0 || g_mod->memory == 0)
		return 0;
	while (got < n && vaddr + 4u <= g_mod->map_size) {
		out[got] = ((const u32 *)((const unsigned char *)g_mod->memory
		                          + vaddr))[0];
		vaddr += 4;
		got++;
	}
	return got;
}

u32 guest_run_atexit_snap_next_at(u32 i)
{
	return (i < ATEXIT_SNAP_MAX) ? g_atexit_snap_next[i] : 0;
}

u32 guest_run_ctor_head_n(void)
{
	return g_ctor_head_n;
}

u32 guest_run_ctor_head_at(u32 i)
{
	return (i < ATEXIT_CTOR_MAX) ? g_ctor_head[i] : 0;
}

/* Длина списка atexit после конструктора `i`. */
u32 guest_run_ctor_len_at(u32 i)
{
	return (i < ATEXIT_CTOR_MAX) ? g_ctor_len[i] : 0;
}

u32 guest_run_atexit_snap_n(void)
{
	return g_atexit_snap_n;
}

u32 guest_run_atexit_snap_head(void)
{
	return g_atexit_snap_head;
}

u32 guest_run_atexit_snap_at(u32 i)
{
	return (i < ATEXIT_SNAP_MAX) ? g_atexit_snap[i] : 0;
}

u32 guest_run_nid(u32 i)  { return (i < LOG_MAX) ? g_run.nid[i]  : 0; }
u32 guest_run_stub(u32 i) { return (i < LOG_MAX) ? g_run.stub[i] : 0; }
u32 guest_run_arg0(u32 i) { return (i < LOG_MAX) ? g_run.arg0[i] : 0; }
u32 guest_run_arg1(u32 i) { return (i < LOG_MAX) ? g_run.arg1[i] : 0; }
u32 guest_run_arg2(u32 i) { return (i < LOG_MAX) ? g_run.arg2[i] : 0; }
u32 guest_run_arg3(u32 i) { return (i < LOG_MAX) ? g_run.arg3[i] : 0; }
u32 guest_run_ret(u32 i)   { return (i < LOG_MAX) ? g_run.ret[i] : 0; }

u32 guest_run_lm_count(void) { return g_lm_count; }
u32 guest_run_lm_slot(void)  { return g_lm_slot; }

/* Сколько 64-битных операций переписано в 32-битные при загрузке.
 * Прибор к правке от 2026-10-15: SIGILL на `dadd` в куче. */
u32 guest_run_patch_64(void)
{
	return (g_mod != NULL) ? g_mod->patch_64 : 0;
}

/* --- доступ отчёта к кадру --- */

u32 guest_run_fb_top(void)   { return g_fb_top; }

/* Чтение u32 из гостевой памяти ПО ВИРТУАЛЬНОМУ АДРЕСУ ОБРАЗА
 * (vaddr, без базы 0x08804000). Для отчёта: прямая видимость
 * внутренних глобалей игры — очереди задач, power-флагов. */
u32 guest_run_state(u32 vaddr)
{
	if (g_mod == NULL || g_mod->memory == 0)
		return 0xDEADBEEFu;
	if ((u64)vaddr + 4 > (u64)g_mod->span)
		return 0xDEADBEAFu;
	return *(const u32 *)(g_mod->memory + vaddr);
}
u32 guest_run_fb_bw(void)    { return g_fb_bw; }
u32 guest_run_fb_fmt(void)   { return g_fb_fmt; }
u32 guest_run_fb_count(void) { return g_fb_count; }
u32 guest_run_fb_dumps(void) { return g_fb_dumps; }

u32 guest_run_vram_base(void)
{
	return (g_mod != NULL) ? g_mod->vram_base : 0;
}

/* Нулевая страница + скретчпад: приборы к правке по замеру 90. */
u32 guest_run_null_mirror(void)
{
	return (g_mod != NULL) ? g_mod->null_mirror : 0;
}

u32 guest_run_scr_base(void)
{
	return (g_mod != NULL) ? g_mod->scr_base : 0;
}

/* --- куча VPL: приборы к подмене семейства `sceKernel*Vpl` --- */

u32 guest_run_vpl_base(void)
{
	return (g_mod != NULL) ? g_mod->vpl_base : 0;
}

u32 guest_run_vpl_mirror(void)
{
	return (g_mod != NULL) ? g_mod->vpl_mirror : 0;
}

/* Фактический размер области VPL: лестница кандидатов в `map_vpl`
 * берёт то, что свободно, и размер меняется от прогона к прогону. */
u32 guest_run_vpl_bytes(void)
{
	return (g_mod != NULL) ? g_mod->vpl_bytes : 0;
}

u32 guest_run_vpl_created(void) { return g_vpl.created; }
u32 guest_run_vpl_allocs(void)  { return g_vpl.allocs; }
u32 guest_run_vpl_frees(void)  { return g_vpl.frees; }
u32 guest_run_vpl_fails(void)   { return g_vpl.fails; }
u32 guest_run_vpl_used(void)    { return g_vpl.off; }

/* --- шрифтовая библиотека: приборы к подмене sceLibFont --- */

u32 guest_run_font_libs(void)  { return g_font.libs; }
u32 guest_run_font_opens(void) { return g_font.opens; }
u32 guest_run_font_glyphs(void) { return g_font.glyphs; }

u32 guest_run_ef_created(void) { return g_ef.created; }

u32 guest_run_cb_created(void) { return g_cb.created; }

u32 guest_run_cb_fired(void)   { return g_cb.fired; }
u32 guest_run_si_fired(void)  { return g_si_fired; }

/* Семафоры (замер 97): uid-таблица и честное блокирование. */
u32 guest_run_sema_created(void) { return g_sema_created; }
u32 guest_run_sema_waited(void)  { return g_sema_waited; }
u32 guest_run_sema_stuck(void)   { return g_sema_stuck; }
u32 guest_run_drain_marks(void)  { return g_drain_marks; }

/* Сколько `ins`/`ext` эмулировано при SIGILL. 0 — SIGILL не было
 * или в окне не нашлось SPECIAL3. */
u32 guest_run_emul_ins(void)
{
	return g_emul_ins;
}

/* Адрес зеркала без кэша. 0 — зеркала нет: tmpfs недоступен или адрес
 * занят. Прибор к двойному отображению из `map_guest`. */
u32 guest_run_mirror_base(void)
{
	return (g_mod != NULL) ? g_mod->mirror_base : 0;
}

/* Сколько `cache`-инструкций заменено на NOP при загрузке. 0 — правка
 * не применена (например, гость не загружался). */
u32 guest_run_cache_nops(void)
{
	return (g_mod != NULL) ? g_mod->cache_nops : 0;
}

u32 guest_run_lm_where(u32 slot)
{
	return (slot < LM_SNAP_SLOTS) ? g_lm_where[slot] : 0;
}

u32 guest_run_lm_word(u32 slot, u32 k)
{
	if (slot >= LM_SNAP_SLOTS || k >= LM_SNAP_WORDS)
		return 0;
	return g_lm_words[slot][k];
}

/* Голова списка atexit на момент вызова `i`. 0 — список пуст либо снимок
 * не сделан (например, вызов случился до загрузки гостя). */
u32 guest_run_call_head(u32 i) { return (i < LOG_MAX) ? g_run.head[i] : 0; }

u32 guest_run_log_max(void)
{
	return LOG_MAX;
}

u32 guest_run_target(void)
{
	return CALL_LIMIT;
}
