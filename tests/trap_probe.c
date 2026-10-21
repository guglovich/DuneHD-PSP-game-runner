/* Проба механизма ловушки на плеере.
 *
 * Зачем. В рантайме ловушка в стабе импорта срабатывает — адрес инструкции
 * при сбое равен первому стабу, — но дальше поведение неясное. Разбираться
 * внутри рантайма дорого: там гость, релокации, 147 стабов. Здесь ничего
 * этого нет, и каждый вопрос проверяется напрямую.
 *
 * Первая версия пробы зависла: процесс крутился 3 минуты 19 секунд CPU, его
 * пришлось убить вручную. Причина оказалась главным результатом всех
 * замеров — обработчик отрабатывал, возвращал управление, но адрес
 * инструкции не восстанавливался, и `break` выполнялся снова. Отсюда два
 * правила, учтённые здесь:
 *
 *   1. Проба обязана быть ограничена по времени принудительно, иначе зависание
 *      оборачивается ручным убийством процесса на плеере. Страховка — alarm.
 *   2. Выход из обработчика делается через siglongjmp: даже если механизм не
 *      работает, разбор всегда завершается и печатает результат.
 *
 * Две фазы, потому что сдвиг адреса не работает и нужен обход.
 *
 * Фаза 1 — сдвиг адреса инструкции. Проверяет, восстанавливается ли
 * изменённый PC. Результат замеров: нет. Ловушка повторяется бесконечно.
 *
 * Фаза 2 — подмена инструкции на месте ловушки. Раз PC не восстанавливается,
 * процессор после возврата перечитывает инструкку по тому же адресу, а
 * значит исполнит уже подменённую. Обработчик пишет на место ловушки
 * `jr $ra; nop` и сбрасывает кэш команд. Сброс обязателен: процессор MIPS
 * исполняет из отдельного кэша, и без него продолжение пойдёт по
 * закэшированному `break`.
 *
 * Проверочная последовательность не собрана ассемблером, а записана словами
 * в страницу с правом исполнения. Причина практическая: адрес метки,
 * объявленной во встроенном ассемблере, gcc в режиме по умолчанию адресует
 * через GOT16, на который такой символ не отображается, и сборка падает с
 * «can't find matching LO16 reloc against .text». Плюс `-fno-pic` не
 * подходит: musl собирает shared-объект, и не-абиcalls-код с ним не
 * связывается. Ручная сборка словами снимает обе проблемы и делает адрес
 * ловушки точно известным.
 *
 * Устройства Sigma не открываются: только mmap, сигналы и запись в память.
 */

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

typedef uint32_t u32;
typedef unsigned int (*code_fn)(void);

/* Предел на всю пробу: без него зависание оборачивается ручным убийством
 * процесса на плеере, что уже случалось. */
#define PROBE_ALARM_SEC 8

/* Больше этого числа ловушек подряд считаем признаком того, что ловушка не
 * срабатывает повторно после подмены: иначе фаза не завершится никогда. */
#define TRAP_LIMIT 64

/* Коды инструкций MIPS. */
#define W_BREAK    0x0000000Du   /* break            */
#define W_LI_V0    0x2402600Du   /* addiu $2,$0,imm  */
#define W_JR_RA    0x03E00008u   /* jr   $31         */
#define W_NOP      0x00000000u   /* nop              */

/* Исходная последовательность, 16 байт: ловушка, маркер, возврат. */
static const u32 CODE_ORIG[4] = { W_BREAK, W_LI_V0, W_JR_RA, W_NOP };

/* Последовательность фазы 3: ловушка, затем выход из процесса.
 *
 *   0x0000000D  break
 *   0x24020FA1  addiu $2, $0, 4001   (__NR_exit на MIPS o32)
 *   0x2404002A  addiu $4, $0, 42      код выхода
 *   0x0000000C  syscall
 *   0x00000000  nop
 *
 * Выход делается системным вызовом, а не возвратом, потому что после смены
 * `$sp` возвращаться некуда: кадр вызывающего лежит на старом стеке, и
 * компилятор восстановил бы `$sp` по нему. Процесс просто завершается, а
 * код выхода говорит родителю, дошла ли ловушка до обработчика. */
static const u32 CODE_EXIT[5] = {
    W_BREAK,
    0x24020FA1u,
    0x2404002Au,
    0x0000000Cu,
    W_NOP,
};

#define PROBE_EXIT_CODE 42

/* Маркер из кода после ловушки: попадает в $v0, то есть и в результат
 * функции. */
#define PROBE_MARKER 0x600Du

/* Значение, которое обработчик кладёт в $v0 при подмене. Если подмена
 * работает, вызывающий получит именно его. */
#define PROBE_V0 0x1234u

/* Смена `$sp` на стек гостя и передача управления.
 *
 * Отдельная функция на ассемблере: менять `$sp` внутри обычной нельзя —
 * компилятор считает его неизменным и в своём коде выхода восстановил бы
 * прежнее значение. Пролог и эпилог здесь тоже недопустимы по той же причине.
 *
 * `.set noreorder` обязателен: ассемблер по умолчанию переставляет инструкции
 * в слот задержки `jr`. */
__asm__(
    "    .set noreorder\n"
    "    .text\n"
    "    .globl probe_guest_enter\n"
    "    .type probe_guest_enter, @function\n"
    "probe_guest_enter:\n"
    "    move  $8, $4\n"      /* t0 = адрес гостевого кода */
    "    move  $9, $5\n"      /* t1 = вершина стека гостя */
    "    move  $29, $9\n"     /* sp = вершина стека гостя */
    "    jr    $8\n"
    "    nop\n"
    "    .set reorder\n"
    "    .size probe_guest_enter, .-probe_guest_enter\n");

extern void probe_guest_enter(u32 guest_pc, u32 stack_top);

/* ------------------------------------------------------------------------- */
/* Состояние                                                                  */
/* ------------------------------------------------------------------------- */

static char g_altstack[64 * 1024];
static int g_altstack_ok;

static sigjmp_buf g_jmp;
static volatile int g_jmp_valid;

/* Режим обработчика: 0 — сдвиг адреса, 1 — подмена инструкции на месте.
 * Выбирается один раз перед фазой, чтобы обработчик оставался простым. */
static volatile int g_mode_oneshot;

static u32 *g_code;                  /* исполняемая страница */
static u32  g_probe_lo, g_probe_hi;  /* диапазон опознания ловушки */

static volatile int g_traps;
static volatile int g_pc_field = -1;
static volatile u32 g_v0_before;
static volatile u32 g_handler_sp;
static volatile int g_advance_tried;
static volatile int g_reached_limit;

static volatile int g_patched;       /* инструкция подменена */
static volatile unsigned int g_oneshot_ret;

/* ------------------------------------------------------------------------- */

static void guard_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)sig;
    (void)si;
    (void)ctx;
    printf("\n[СТРАХОВКА] проба превысила %d с, аварийный выход\n",
           PROBE_ALARM_SEC);
    fflush(stdout);
    _exit(3);
}

static void bail_out(void)
{
    g_reached_limit = 1;
    if (g_jmp_valid)
        siglongjmp(g_jmp, 1);
    _exit(4);
}

static void trap_handler(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    unsigned i;
    int field;

    (void)sig;
    (void)si;

    /* Поле с адресом инструкции ищется перебором: адрес ловушки известен
     * заранее, поэтому поле определяется фактом, а не предположением о
     * раскладке структуры. Ищется один раз — дальше индекс уже известен. */
    if (g_pc_field < 0) {
        for (i = 0; i < 35; i++) {
            u32 v = (u32)uc->uc_mcontext.gregs[i];
            if (v >= g_probe_lo && v < g_probe_hi) {
                g_pc_field = (int)i;
                break;
            }
        }
    }
    field = g_pc_field;
    g_handler_sp = (u32)uc->uc_mcontext.gregs[29];

    if (g_mode_oneshot) {
        g_traps++;
        if (g_traps > TRAP_LIMIT)
            bail_out();
        if (field < 0)
            return;               /* адрес не опознан — подменять нечего */

        /* Подмена на месте ловушки: процессор после возврата перечитает
         * инструкцию по этому же адресу. */
        g_code[0] = W_JR_RA;
        g_code[1] = W_NOP;
        __builtin___clear_cache((char *)g_code,
                                (char *)g_code + 2 * sizeof(u32));
        uc->uc_mcontext.gregs[2] = PROBE_V0;
        g_patched = 1;
        return;
    }

    g_traps++;
    if (g_traps > TRAP_LIMIT)
        bail_out();
    if (g_v0_before == 0)
        g_v0_before = (u32)uc->uc_mcontext.gregs[2];

    /* Фаза 1: единственный способ пропустить ловушку — сдвинуть адрес
     * инструкции на четыре байта вперёд. */
    if (field >= 0) {
        u32 pc = (u32)uc->uc_mcontext.gregs[field];

        uc->uc_mcontext.gregs[field] = pc + 4;
        g_advance_tried = 1;
    }
}

/* ------------------------------------------------------------------------- */

static void install(void)
{
    struct sigaction sa;
    stack_t ss;

    ss.ss_sp = g_altstack;
    ss.ss_size = sizeof(g_altstack);
    ss.ss_flags = 0;
    g_altstack_ok = sigaltstack(&ss, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = trap_handler;
    /* SA_NODEFER не ставится: повторный сигнал того же вида должен вставать
     * в очередь, а не порождать вложенность. */
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGTRAP, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = guard_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGALRM, &sa, NULL);
}

static void reset_code(void)
{
    memcpy(g_code, CODE_ORIG, sizeof(CODE_ORIG));
    __builtin___clear_cache((char *)g_code, (char *)g_code + sizeof(CODE_ORIG));
}

static int sp_is_altstack(u32 sp)
{
    return sp >= (u32)(uintptr_t)g_altstack &&
           sp < (u32)(uintptr_t)(g_altstack + sizeof(g_altstack));
}

static void print_common(const char *title)
{
    printf("%s\n", title);
    printf("  1. обработчик вызван      : %s (ловушек %d)\n",
           g_traps > 0 ? "ДА" : "НЕТ", g_traps);
    printf("  2. поле адреса инструкции : ");
    if (g_pc_field >= 0)
        printf("gregs[%d]\n", g_pc_field);
    else
        printf("НЕ НАЙДЕНО среди 35\n");
    printf("  3. SA_ONSTACK             : $sp обработчика 0x%08X -> %s\n",
           g_handler_sp,
           sp_is_altstack(g_handler_sp) ? "да, отдельный стек"
                                        : "НЕТ, свой стек вызова");
}

/* ------------------------------------------------------------------------- */

static void phase_advance_pc(code_fn code, const char *title)
{
    unsigned int marker = 0;

    g_mode_oneshot = 0;
    g_traps = 0;
    g_pc_field = -1;
    g_v0_before = 0;
    g_advance_tried = 0;
    g_reached_limit = 0;
    reset_code();

    printf("\n--- %s ---\n", title);

    if (sigsetjmp(g_jmp, 1) == 0) {
        g_jmp_valid = 1;
        marker = code();
        g_jmp_valid = 0;
        printf("  4. сдвиг PC на 4          : %s\n",
               g_advance_tried ? "выполнен" : "не выполнен");
        printf("  5. маркер после ловушки   : 0x%08X -> %s\n", marker,
               marker == PROBE_MARKER ? "инструкция пропущена"
                                      : "НЕ пропущена");
    } else {
        printf("  4. сдвиг PC на 4          : выполнен, но НЕ ДАЛ ЭФФЕКТА\n");
    }

    print_common("  сводка:");
    if (g_reached_limit)
        printf("  ВЫВОД: сдвинутый адрес инструкции НЕ ВОССТАНАВЛИВАЕТСЯ при\n"
               "         возврате из обработчика. `break` выполняется снова.\n");
}

static void phase_patch_in_place(code_fn code, const char *title)
{
    g_mode_oneshot = 1;
    g_traps = 0;
    g_pc_field = -1;
    g_patched = 0;
    g_reached_limit = 0;
    g_oneshot_ret = 0;
    reset_code();

    printf("\n--- %s ---\n", title);

    if (sigsetjmp(g_jmp, 1) == 0) {
        g_jmp_valid = 1;
        g_oneshot_ret = code();
        g_jmp_valid = 0;
    } else {
        printf("  ловушка не прекратилась\n");
    }

    print_common("  сводка:");
    printf("  4. подмена на месте       : %s\n",
           g_patched ? "выполнена" : "НЕ выполнена");
    printf("  5. результат функции      : 0x%08X (ожидалось 0x%08X) -> %s\n",
           g_oneshot_ret, (unsigned)PROBE_V0,
           g_oneshot_ret == PROBE_V0
               ? "ПОДМЕНА РАБОТАЕТ"
               : "подмена не сработала");

    if (!g_reached_limit && g_patched && g_oneshot_ret == PROBE_V0)
        printf("  ВЫВОД: подмена работает. Ловушка срабатывает один раз,\n"
               "         после неё стаб возвращает управление сам, а $v0\n"
               "         берётся из обработчика.\n");
    else if (g_reached_limit)
        printf("  ВЫВОД: подмена не помогла, ловушка продолжает срабатывать.\n");
}

/* ------------------------------------------------------------------------- */
/* Фаза 3: та же ловушка, но на переключённом стеке                           */
/* ------------------------------------------------------------------------- */

/* Стек гостя отдаётся не с вершины отображения, а ниже, и сверху остаётся
 * запас — ровно как в рантайме. */
#define GUEST_STACK_RESERVE (4u * 1024u * 1024u)
#define GUEST_STACK_COMMIT  (1u * 1024u * 1024u)

/* Работает в потомке: после смены `$sp` возвращаться в этот код нельзя,
 * кадр лежит на старом стеке. Выход делается системным вызовом внутри
 * гостевой последовательности, а решение принимает родитель по коду выхода. */
static void phase_switched_stack(void)
{
    pid_t pid;
    int status = 0;

    printf("\n--- фаза 3: ловушка на переключённом стеке гостя ---\n");

    pid = fork();
    if (pid < 0) {
        printf("  fork не удался\n");
        return;
    }
    if (pid == 0) {
        void *guest_mem = mmap(NULL, GUEST_STACK_RESERVE, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *guest_code = mmap(NULL, 4096,
                                PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (guest_mem == MAP_FAILED || guest_code == MAP_FAILED) {
            printf("  [потомок] не удалось выделить память гостя\n");
            _exit(2);
        }
        memcpy(guest_code, CODE_EXIT, sizeof(CODE_EXIT));
        __builtin___clear_cache((char *)guest_code,
                                (char *)guest_code + sizeof(CODE_EXIT));

        /* Обработчик опознаёт адрес ловушки по этому диапазону. */
        g_probe_lo = (u32)(uintptr_t)guest_code;
        g_probe_hi = g_probe_lo + (u32)sizeof(CODE_EXIT);
        g_traps = 0;
        g_patched = 0;
        g_pc_field = -1;

        printf("  [потомок] стек гостя 0x%08X .. 0x%08X, sp отдан 0x%08X\n",
               (u32)(uintptr_t)guest_mem,
               (u32)(uintptr_t)guest_mem + GUEST_STACK_RESERVE,
               (u32)(uintptr_t)guest_mem + GUEST_STACK_COMMIT);

        probe_guest_enter(g_probe_lo,
                          (u32)(uintptr_t)guest_mem + GUEST_STACK_COMMIT);
        _exit(3);            /* сюда не дойти: код выхода ставит syscall */
    }

    if (waitpid(pid, &status, 0) < 0) {
        printf("  waitpid не удался\n");
        return;
    }

    printf("  1. потомок завершился    : код %d, сигнал %d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    printf("  2. ловушек в потомке     : %d\n", g_traps);
    printf("  3. подмена выполнена     : %s\n",
           g_patched ? "да" : "НЕТ");

    if (WIFEXITED(status) && WEXITSTATUS(status) == PROBE_EXIT_CODE &&
        g_patched)
        printf("  ВЫВОД: на переключённом стеке ловушка доставляется и подмена\n"
               "         работает. Рантайм падает по другой причине.\n");
    else
        printf("  ВЫВОД: на переключённом стеке ловушка НЕ доставляется.\n");
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    code_fn code;
    void *page;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* Обычная память на плеере не исполняется, нужна страница с правом
     * исполнения. */
    page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        printf("[СБОЙ] не удалось выделить исполняемую страницу\n");
        return 1;
    }
    g_code = (u32 *)page;
    g_probe_lo = (u32)(uintptr_t)page;
    g_probe_hi = g_probe_lo + (u32)sizeof(CODE_ORIG);
    code = (code_fn)(uintptr_t)page;

    printf("=== проба механизма ловушки ===\n");
    printf("адрес ловушки  : 0x%08X .. 0x%08X\n", g_probe_lo, g_probe_hi);
    printf("сигнал         : SIGTRAP\n");

    install();
    printf("sigaltstack    : %s\n",
           g_altstack_ok == 0 ? "применён" : "НЕ ПРИМЕНЁН");

    alarm(PROBE_ALARM_SEC);

    phase_advance_pc(code, "фаза 1: сдвиг адреса инструкции на 4");
    phase_patch_in_place(code, "фаза 2: подмена инструкции на месте ловушки");
    phase_switched_stack();

    printf("\n=== итог ===\n");
    if (g_patched && !g_reached_limit && g_oneshot_ret == PROBE_V0 &&
        g_pc_field >= 0)
        printf("Способ пригоден: стаб с ловушкой, подменяемый на jr/nop при\n"
               "первом срабатывании. Сдвигать адрес инструкции не нужно.\n");
    else
        printf("Способ непригоден ни в одной из форм.\n");
    return 0;
}