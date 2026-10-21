/*
 * Раскладка ucontext на плеере — измерение, а не предположение.
 *
 * Зачем отдельная проба. В рантайме гостя адрес ловушки ищется в ucontext
 * сравнением с заранее известным адресом стаба, и он находится на 32 байтах от
 * начала контекста. Но из этого нельзя заключить, где лежат `$a0`..`$a3`:
 * перебор по `gregs[]` с их номерами даёт значения, которые не сходятся с
 * сигнатурой вызова. Значит раскладку надо измерить отдельно.
 *
 * Метод. Гость загружает в регистры заранее известные значения, выполняет
 * `break`, и обработчик просматривает контекст, отмечая смещения, где эти
 * значения встретились. Каждое значение уникально, поэтому смещение определяется
 * фактом, а не догадкой о раскладке структуры.
 *
 * Чего проба не делает. Она не открывает устройства Sigma и не трогает NAND:
 * только mmap, один сигнал и запись в память.
 *
 * Почему отдельный файл, а не фаза в tests/trap_probe.c. У той пробы нет
 * разделяемой страницы, и счётчики, которые должен увидеть родитель, после
 * `fork` живут в его копии — то есть всегда читаются как нули. Для проверки
 * «ловушка сработала» это неважно, а для измерения раскладки неприемлемо.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdint.h>
#include <asm/cachectl.h>
#include <asm/unistd.h>

typedef unsigned int u32;
typedef unsigned char u8;

/* Метки в регистрах. Значения меньше 0x8000, чтобы каждое помещалось в
 * `addiu $rt, $0, imm` одной инструкцией и не требовало `lui`.
 *
 * Метки стоят во ВСЕХ регистрах, а не только в аргументах. Повод — измерение в
 * рантайме: при сбое потока `$ra` указывал в середину базового блока, а `$fp`
 * и `$at` содержали адреса нашего образа. Либо гость портит управление, либо
 * мы восстанавливаем не тот регистр — и пока это не проверено поимённо,
 * гадать бесполезно. */
#define MARK_A0 0x1111u
#define MARK_A1 0x2222u
#define MARK_A2 0x3333u
#define MARK_A3 0x4444u
#define MARK_V0 0x5555u
/* `$sp` помечается не значением, а адресом: обработчик сигнала работает на
 * прерванном стеке, и недопустимый `$sp` убивает его раньше, чем он что-либо
 * измерит. Проба это уже наблюдала — SIGSEGV при нуле сработавших ловушек.
 * Поэтому `$sp` уводится на вершину гостевого стека плюс SP_MARK_OFF, и меткой
 * служит сам адрес: он известен и уникален. */
#define SP_MARK_OFF 0x6666u
#define MARK_RA 0x7777u
#define MARK_GP 0x0A0Au       /* $gp ожидаем как 0x0000A0A0 */

#define MARK_COUNT 32      /* регистров помечено больше восьми */

/* Коды инструкций MIPS. */
#define W_NOP      0x00000000u
#define W_BREAK    0x0000000Du
#define W_SYSCALL  0x0000000Cu

/* `addiu $rt, $0, imm`: opcode 001001, rs = $0, rt = получатель. */
#define W_LI(rt, imm) ((0x09u << 26) | ((u32)(rt) << 16) | (u32)(imm))

/* __NR_exit на MIPS o32 = 4000 + 1. */
#define NR_EXIT 4001
#define PROBE_EXIT_CODE 42

/* Гость: метки в регистрах, ловушка, затем выход системным вызовом.
 *
 * Возврат из гостя делать нельзя: `$ra` после перехода остаётся адресом
 * возврата в этот файл, и `jr $ra` ушёл бы обратно в C посреди подготовки
 * аргументов. Выход делается системным вызовом — как в tests/trap_probe.c. */
/* Порядок важен: `$sp` не помечается — см. SP_MARK_OFF. */
static const u32 CODE_MARK[] = {
    W_LI(4, MARK_A0),         /* $a0  */
    W_LI(5, MARK_A1),         /* $a1  */
    W_LI(6, MARK_A2),         /* $a2  */
    W_LI(7, MARK_A3),         /* $a3  */
    W_LI(2, MARK_V0),         /* $v0  */
    W_LI(31, MARK_RA),        /* $ra  */
    W_LI(28, MARK_GP),        /* $gp  */
    W_LI(1, 0x1001),          /* $at  */
    W_LI(3, 0x1003),          /* $v1  */
    W_LI(24, 0x1008),         /* $t8  */
    W_LI(25, 0x1009),         /* $t9  */
    W_LI(26, 0x100A),         /* $k0  */
    W_LI(27, 0x100B),         /* $k1  */
    W_LI(16, 0x1010),         /* $s0  */
    W_LI(17, 0x1011),         /* $s1  */
    W_LI(18, 0x1012),         /* $s2  */
    W_LI(19, 0x1013),         /* $s3  */
    W_LI(20, 0x1014),         /* $s4  */
    W_LI(21, 0x1015),         /* $s5  */
    W_LI(22, 0x1016),         /* $s6  */
    W_LI(23, 0x1017),         /* $s7  */
    W_LI(30, 0x101E),         /* $fp  */
    W_BREAK,
    W_NOP,                    /* обязателен: подмена занимает два слова */
    W_LI(2, NR_EXIT),
    W_LI(4, PROBE_EXIT_CODE),
    W_SYSCALL,
    W_NOP,
};

/* Метка -> имя регистра. Ключ ищется линейно, их немного. */
static const struct { u32 v; const char *n; } MARK_NAMES[] = {
    { MARK_A0, "$a0" }, { MARK_A1, "$a1" }, { MARK_A2, "$a2" },
    { MARK_A3, "$a3" }, { MARK_V0, "$v0" }, { MARK_RA, "$ra" },
    { MARK_GP, "$gp" }, { 0x1001, "$at" }, { 0x1003, "$v1" },
    { 0x1008, "$t8" }, { 0x1009, "$t9" }, { 0x100A, "$k0" },
    { 0x100B, "$k1" }, { 0x1010, "$s0" }, { 0x1011, "$s1" },
    { 0x1012, "$s2" }, { 0x1013, "$s3" }, { 0x1014, "$s4" },
    { 0x1015, "$s5" }, { 0x1016, "$s6" }, { 0x1017, "$s7" },
    { 0x101E, "$fp" },
};
#define K_BREAK_WORD_INDEX 2   /* li $k0, li $k1, break */
#define BREAK_WORD_INDEX 22  /* 22 метки регистров, затем break */
#define MARK_NAMES_N ((int)(sizeof(MARK_NAMES) / sizeof(MARK_NAMES[0])))

/* Смещение PC в ucontext на этой прошивке: 32 байта от начала контекста,
 * см. docs/guest-entry.md. */
#define UCTX_PC_OFFSET 32

/* Границы просмотра контекста. */
#define UCTX_SCAN_BYTES 1024

/* ------------------------------------------------------------------------- */
/* Разделяемое состояние                                                       */
/* ------------------------------------------------------------------------- */

struct probe_state {
    u32 lo;                    /* начало гостевого кода */
    u32 hi;                    /* конец гостевого кода */
    u32 hits_off[MARK_COUNT];  /* смещения найденных меток */
    u32 hits_val[MARK_COUNT];  /* значения найденных меток */
    int hits;                  /* сколько всего найдено */
    int traps;                 /* сработало ловушек */
    int patched;
    u32 sp_mark;               /* помеченный $sp: вершина стека + смещение */
    u32 pc_at_32;              /* что лежит на измеренном смещении PC */
    u32 pc_found_off;          /* где на самом деле лежит адрес инструкции */
    /* Индекс слова `break` в гостевом коде. Обработчик подменяет именно его.
     * Раньше он подменял слова 0 и 1, и это работало только пока `break` стоял
     * первым. После того как в код добавили метки для всех регистров, `break`
     * уехал в середину, и обработчик портил первые инструкции вместо ловушки:
     * ловушка срабатывала десятки тысяч раз, гость не доходил до кода после
     * неё, и все измеренные «после ловушки» значения были недостоверны. */
    u32 break_idx;
    /* Разделяемая исполняемая страница. Её обязательно отображать ДО fork:
     * `MAP_SHARED|MAP_ANONYMOUS`, созданный потомком, родителю не виден вовсе —
     * общей становится только физическая подложка между теми отображениями,
     * которые есть у обоих. Читая результат по адресу страницы потомка,
     * родитель падал с SIGSEGV и терял вывод всех фаз. */
    u32 shared_code;
    /* Слова гостевого кода вокруг ловушки, снятые обработчиком уже после
     * подмены: так видно, что именно оказалось в памяти. */
    u32 code_around[5];

    /* Фаза SIGSEGV: адрес сработавшей инструкции известен заранее, поэтому
     * смещение поля находится сравнением. */
    u32 segv_trap_addr;        /* адрес инструкции, на которую свалимся */
    u32 segv_off;              /* найденное смещение в ucontext */
    u32 segv_at_32;            /* что лежит на +32 в этом кадре */
    int  segv_seen;

    /* Сохраняются ли $k0/$k1 при возврате из обработчика. */
    u32  k0_slot;              /* адрес слова в разделяемом стеке */
    u32  k0_before, k0_after;
    u32  k1_before, k1_after;
};

static struct probe_state *g_sh;

/* ------------------------------------------------------------------------- */

/* Гость фазы выживания $k0/$k1: метка, ловушка, запись $k0 после ловушки.
 *
 * Смысл: в ucontext полей $k0 и $k1 НЕТ — метки не нашлись ни по одному
 * смещению. Если при этом musl при возврате из обработчика их затирает, то
 * гость теряет $k0 после КАЖДОГО вызова импорта. А гостевой код проверяет
 * `$k0` на ноль как входной параметр (`beqz $k0`) — то есть падение с
 * `si_addr` = 0 объясняется этим напрямую.
 *
 * Запись идёт в стек гостя, а стек отображён MAP_SHARED, поэтому родитель
 * прочитает результат. Адрес в коде гостя не нужен.
 */
#define MARK_K0 0x100Au
#define MARK_K1 0x100Bu
#define SW_A0_RT(rt, off) ((0x2Bu << 26) | (4u << 21) | ((u32)(rt) << 16) \
                           | (u32)(off))

static const u32 CODE_KSURV[] = {
    W_LI(26, MARK_K0),        /* $k0 */
    W_LI(27, MARK_K1),        /* $k1 */
    W_BREAK,
    W_NOP,                    /* обязателен: подмена занимает два слова */
    /* Пишем через `$a0`: `layout_enter` передаёт первый аргумент как `$a0`,
     * то есть адрес гостевого кода, а страница отображёна MAP_SHARED до fork.
     * Через стек писать нельзя — `$sp` после ловушки в первой версии фазы
     * сам стал причиной SIGSEGV.
     *
     * Слово 24 — метка «подмена прошла, гость дошёл дальше». Без неё при
     * сбое нельзя отличить «$k0 затёрт» от «гость не дошёл», а это уже
     * порождало ложный вывод. */
    /* Смещения далеко за пределами самого кода. Первые метки стояли по
     * смещениям 0, 16, 20, 24 — то есть ровно поверх инструкций, и первая же
     * запись `0xDEADBEEF` затирала `li $k0`, после чего гость начинал с
     * заведомо недопустимой команды и падал с SIGILL. Метки не должны
     * накладываться на код. */
    SW_A0_RT(0, 0x108),       /* sw   $zero, 0x108($a0)  <- дошли сюда */
    SW_A0_RT(26, 0x100),      /* sw   $26,   0x100($a0)  <- $k0 после */
    SW_A0_RT(27, 0x104),      /* sw   $27,   0x104($a0)  <- $k1 после */
    W_LI(2, NR_EXIT),
    W_LI(4, PROBE_EXIT_CODE),
    W_SYSCALL,
    W_NOP,
};

/* Гость фазы SIGSEGV: чтение по нулю, то есть SIGSEGV с `si_addr` = 0 — ровно
 * то, что наблюдается в рантайме.
 *
 *   3c040000  lui   $4,0
 *   0080f809  jalr  $4        <- прыжок по нулю, выборка инструкции
 *   ...выход
 *
 * Фаза прошла три редакции, и это стоит записать.
 * 1. `lui $4,0` + `nop`: сбоя нет — `lui` только загружает ноль в регистр и к
 *    памяти не обращается; код выхода 42, обработчик не вызван.
 * 2. `lw $4,0($4)`: тоже сбоя нет, ребёнок вышел с кодом 42. Значит на этом
 *    ядре чтение по адресу 0 не даёт SIGSEGV — а в рантайме `si_addr` = 0
 *    наблюдается, и это важное различие: там это **выборка инструкции** по
 *    нулевому адресу, то есть прыжок, а не обращение к данным.
 * 3. `jalr $4`: то, что нужно.
 */
#define SEGV_WORD_OFFSET 1      /* индекс слова `lw $4,0($4)` в CODE_SEGV */
static const u32 CODE_SEGV[10] = {
    0x3C040000u,       /* lui   $4, 0            */
    0x0080F809u,       /* jalr  $4               <- прыжок по нулю */
    W_NOP,
    W_LI(2, NR_EXIT),
    W_LI(4, PROBE_EXIT_CODE),
    W_SYSCALL,
    W_NOP,
    W_NOP,
    W_NOP,
};

__asm__(
    "    .set noreorder\n"
    "    .text\n"
    "    .globl layout_enter\n"
    "    .type layout_enter, @function\n"
    "layout_enter:\n"
    "    move  $9, $4\n"       /* t1 = адрес гостевого кода */
    "    move  $10, $5\n"      /* t2 = вершина стека гостя */
    "    move  $29, $10\n"     /* sp = вершина стека гостя */
    "    jr    $9\n"
    "    nop\n"
    "    .set reorder\n"
    "    .size layout_enter, .-layout_enter\n");

extern void layout_enter(u32 guest_pc, u32 stack_top);

/* ------------------------------------------------------------------------- */
/* Обработчик                                                                 */
/* ------------------------------------------------------------------------- */

/* Сброс кэша команд — прямым системным вызовом.
 *
 * `__builtin___clear_cache` здесь не годится: это вложенный вызов, а `$t9` в
 * обработчике равен 0, и подмена ловушки не видна кэшу команд. Ловушка
 * срабатывала 683282 раза в первой версии пробы и 96854 в фазе выживания —
 * гость просто не доходил до кода после `break`, и все значения после ловушки
 * были недостоверны. Это стоило одного ложного вывода: «$k0 затирается».
 * Настоящий вывод делается только после того, как подмена проходит с первого
 * раза, что видно по счётчику ловушек, равному 1.
 */
static void layout_flush_icache(void *addr, u32 len)
{
    register long r_a0 __asm__("$4") = (long)(uintptr_t)addr;
    register long r_a1 __asm__("$5") = (long)len;
    register long r_a2 __asm__("$6") = BCACHE;
    register long r_v0 __asm__("$2") = __NR_cacheflush;

    __asm__ __volatile__("syscall"
                         : "+r"(r_v0)
                         : "r"(r_a0), "r"(r_a1), "r"(r_a2)
                         : "$3", "$7", "$8", "$9", "$10", "$11", "$12", "$13",
                           "$14", "$15", "$24", "$25", "hi", "lo", "memory");
}

/* Ни одной функции, кроме как измерения: обработчик не имеет права вызывать
 * их — `$t9` в нём равен 0, потому что прерванный код не выставлял его. */
static void layout_handler(int sig, siginfo_t *si, void *ctx)
{
    const u8 *p = (const u8 *)ctx;
    unsigned off;

    (void)sig;
    (void)si;

    /* Подмена идемпотентна и делается на каждом срабатывании.
     *
     * Раньше обработчик при повторной ловушке выходил сразу, и проба зависала
     * намертво: если по какой-то причине подмена не видна кэшу команд,
     * процессор исполняет `break` снова, обработчик ничего не делает, и
     * `break` выполняется снова — бесконечный цикл без всякого признака.
     * Теперь каждый проход подменяет ловушку заново, и второй проход
     * проходит дальше. */
    g_sh->traps++;

    g_sh->pc_at_32 = *(const volatile u32 *)(const void *)(p + UCTX_PC_OFFSET);

    for (off = 0; off + 4 <= UCTX_SCAN_BYTES; off += 4) {
        u32 v = *(const volatile u32 *)(const void *)(p + off);
        int known = 0;

        {
            int q;

            for (q = 0; q < MARK_NAMES_N; q++)
                if (v == MARK_NAMES[q].v || v == g_sh->sp_mark) {
                    known = 1;
                    break;
                }
        }

        if (v >= g_sh->lo && v < g_sh->hi && g_sh->pc_found_off == 0)
            g_sh->pc_found_off = off ? off : 0xFFFFFFFFu;

        if (known && g_sh->hits < MARK_COUNT) {
            g_sh->hits_off[g_sh->hits] = off;
            g_sh->hits_val[g_sh->hits] = v;
            g_sh->hits++;
        }
    }

    /* Ловушку снимаем заменой на две пустые инструкции: исполнение
     * продолжается по следующей инструкции, до `syscall` выхода. */
    {
        u32 *code = (u32 *)(uintptr_t)g_sh->lo;

        code[g_sh->break_idx] = W_NOP;
        code[g_sh->break_idx + 1] = W_NOP;
        {
            unsigned k;

            for (k = 0; k < 5; k++)
                g_sh->code_around[k] =
                    code[g_sh->break_idx - 2 + k];
        }
        g_sh->patched = 1;
    }
    layout_flush_icache((void *)(uintptr_t)g_sh->lo + 4 * g_sh->break_idx,
                        2 * sizeof(u32));
}

/* Обработчик SIGSEGV: тот же поиск, но по заранее известному адресу
 * инструкции `lui $4, 0`. Ни одной функции — `$t9` равен 0. */
static void segv_handler(int sig, siginfo_t *si, void *ctx)
{
    const u8 *p = (const u8 *)ctx;
    unsigned off;

    (void)sig;
    (void)si;

    if (g_sh->segv_seen)
        return;
    g_sh->segv_seen = 1;
    g_sh->segv_at_32 = *(const volatile u32 *)(const void *)(p + UCTX_PC_OFFSET);

    for (off = 0; off + 4 <= UCTX_SCAN_BYTES; off += 4) {
        u32 v = *(const volatile u32 *)(const void *)(p + off);

        if (v == g_sh->segv_trap_addr) {
            g_sh->segv_off = off;
            return;
        }
    }
    g_sh->segv_off = 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------------- */

static const char *mark_name(u32 v)
{
    int q;

    if (v == 0x6666u)
        return "помеченный $sp";
    for (q = 0; q < MARK_NAMES_N; q++)
        if (v == MARK_NAMES[q].v)
            return MARK_NAMES[q].n;
    return "?";
}

int main(void)
{
    struct sigaction sa;
    pid_t pid;
    int status = 0;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);

    g_sh = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_sh == MAP_FAILED) {
        printf("[СБОЙ] не удалось выделить разделяемую страницу\n");
        return 1;
    }
    memset((void *)g_sh, 0, 65536);

    g_sh->shared_code = (u32)(uintptr_t)
        mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_sh->shared_code == (u32)(uintptr_t)MAP_FAILED) {
        printf("[СБОЙ] не удалось выделить разделяемую исполняемую страницу\n");
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = layout_handler;
    /* Без SA_NODEFER: повторный сигнал должен вставать в очередь, а не
     * порождать вложенность. */
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &sa, NULL);

    printf("=== раскладка ucontext на плеере ===\n\n");

    pid = fork();
    if (pid < 0) {
        printf("[СБОЙ] fork не удался\n");
        return 1;
    }

    if (pid == 0) {
        void *code = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *stack = mmap(NULL, 4u << 20, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (code == MAP_FAILED || stack == MAP_FAILED)
            _exit(2);

        memcpy(code, CODE_MARK, sizeof(CODE_MARK));
        __builtin___clear_cache((char *)code, (char *)code + sizeof(CODE_MARK));

        g_sh->lo = (u32)(uintptr_t)code;
        g_sh->hi = g_sh->lo + (u32)sizeof(CODE_MARK);
        g_sh->sp_mark = (u32)(uintptr_t)stack + (3u << 20) + SP_MARK_OFF;
        g_sh->break_idx = BREAK_WORD_INDEX;
        alarm(10);            /* страховка: проба не должна висеть */

        layout_enter(g_sh->lo, g_sh->sp_mark);
        _exit(3);
    }

    if (waitpid(pid, &status, 0) < 0) {
        printf("[СБОЙ] waitpid не удался\n");
        return 1;
    }

    printf("потомок            : код %d, сигнал %d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    printf("ловушек            : %d\n", g_sh->traps);
    printf("подмена ловушки    : %s\n", g_sh->patched ? "да" : "НЕТ");
    printf("адрес ловушки      : 0x%08X\n", g_sh->lo);
    printf("помеченный $sp     : 0x%08X\n", g_sh->sp_mark);
    printf("что на +%d         : 0x%08X\n", UCTX_PC_OFFSET, g_sh->pc_at_32);
    printf("адрес инструкции найден на смещении: 0x%08X\n\n", g_sh->pc_found_off);

    if (g_sh->hits == 0) {
        printf("ВЫВОД: метки в контексте не найдены — раскладку так установить\n"
               "       не удалось.\n");
        return 1;
    }

    printf("Найденные метки (смещение: значение = регистр):\n");
    for (i = 0; i < g_sh->hits; i++)
        printf("  +%03d: 0x%08X  %s\n", g_sh->hits_off[i],
               g_sh->hits_val[i], mark_name(g_sh->hits_val[i]));

    /* Фаза выживания $k0/$k1. */
    pid = fork();
    if (pid == 0) {
        /* Стек MAP_SHARED — родитель прочитает, что осталось в $k0/$k1. */
        void *stack = mmap(NULL, 4u << 20, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        /* Код берётся из общей страницы, отображённой родителем до fork. */
        void *code = (void *)(uintptr_t)g_sh->shared_code;
        u32 top;

        if (stack == MAP_FAILED || code == MAP_FAILED)
            _exit(2);
        memcpy(code, CODE_KSURV, sizeof(CODE_KSURV));
        __builtin___clear_cache((char *)code,
                                (char *)code + sizeof(CODE_KSURV));

        top = (u32)(uintptr_t)stack + (3u << 20);
        g_sh->k0_slot = top - 4;
        /* Метка «гость сюда не дошёл». Иначе 0 в слоте нельзя отличить от
         * «$k0 затирается нулём» — именно эта ошибка была сделана в первой
         * версии фазы, где значение читалось из кода после возврата потомка,
         * а потомок до возврата не доходил. */
        *(volatile u32 *)((u8 *)code + 0x100) = 0xDEADBEEFu;
        *(volatile u32 *)((u8 *)code + 0x104) = 0xDEADBEEFu;
        *(volatile u32 *)((u8 *)code + 0x108) = 0xDEADBEEFu;
        g_sh->k0_before = MARK_K0;
        g_sh->k1_before = MARK_K1;
        g_sh->lo = (u32)(uintptr_t)code;
        g_sh->hi = g_sh->lo + (u32)sizeof(CODE_KSURV);
        g_sh->break_idx = K_BREAK_WORD_INDEX;

        alarm(10);
        layout_enter(g_sh->lo, top);
        _exit(3);
    }
    if (waitpid(pid, &status, 0) < 0) {
        printf("[СБОЙ] waitpid не удался\n");
        return 1;
    }
    /* Значения читаются из разделяемого стека, куда их записал сам гость.
     * Код после возврата потомка не годится: до возврата он может не дойти,
     * и тогда переменная остаётся нулевой — это уже выдавалось за «$k0
     * затирается». */
    printf("\n--- фаза: переживают ли $k0/$k1 ловушку ---\n");
    printf("потомок             : код %d, сигнал %d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    printf("код гостя           : 0x%08X, слот результата готов: %s\n",
           g_sh->lo, g_sh->lo ? "да" : "НЕТ");
    /* Чтение защищено проверкой: страницу кода отображает ПОТОМОК, и если он
     * не дошёл до `mmap`, `g_sh->lo` остаётся нулём, а прежняя версия читала
     * по нему и роняла уже родителя — проба теряла всё, включая первую фазу. */
    if (g_sh->lo) {
        const u8 *res = (const u8 *)(uintptr_t)g_sh->lo;
        u32 got0 = *(const volatile u32 *)(const void *)(res + 0x100);
        u32 got1 = *(const volatile u32 *)(const void *)(res + 0x104);

        printf("ловушек             : %d\n", g_sh->traps);
        printf("подмена ловушки     : %s\n", g_sh->patched ? "да" : "НЕТ");
        printf("код вокруг ловушки : %08X %08X %08X %08X %08X\n",
               g_sh->code_around[0], g_sh->code_around[1], g_sh->code_around[2],
               g_sh->code_around[3], g_sh->code_around[4]);
        printf("гость дошёл дальше  : %s\n",
               *(const volatile u32 *)(const void *)(res + 0x108) != 0xDEADBEEFu
                   ? "да" : "НЕТ");
        printf("$k0 было 0x%08X, после 0x%08X  %s\n", g_sh->k0_before, got0,
               got0 == g_sh->k0_before ? "СОХРАНЁН"
               : (got0 == 0xDEADBEEFu ? "НЕ ДОШЁЛ (недействительно)"
                                      : "ЗАТЁРТ"));
        printf("$k1 было 0x%08X, после 0x%08X  %s\n", g_sh->k1_before, got1,
               got1 == g_sh->k1_before ? "СОХРАНЁН"
               : (got1 == 0xDEADBEEFu ? "НЕ ДОШЁЛ (недействительно)"
                                      : "ЗАТЁРТ"));
    } else {
        printf("ФАЗА НЕ ВЫПОЛНЕНА: потомок не дошёл до отображения кода.\n");
    }

    /* Фаза SIGSEGV. Тот же вопрос для другого сигнала: в рантайме сбой потока
     * показывал PC по смещению +32, но инструкция там была заведомо неисполняемой
     * (`beql`), то есть поле для SIGSEGV не то же, что для SIGTRAP. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    pid = fork();
    if (pid == 0) {
        void *code = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *stack = mmap(NULL, 4u << 20, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (code == MAP_FAILED || stack == MAP_FAILED)
            _exit(2);

        memcpy(code, CODE_SEGV, sizeof(CODE_SEGV));
        __builtin___clear_cache((char *)code, (char *)code + sizeof(CODE_SEGV));
        /* Адрес именно той инструкции, которая обращается по нулю. */
        g_sh->segv_trap_addr = (u32)(uintptr_t)code +
                               SEGV_WORD_OFFSET * (u32)sizeof(u32);

        /* Страховка обязательна: обработчик возвращается, `jalr` после
         * возврата исполняется снова, ловушка повторяется — без alarm проба
         * крутилась бы вечно. */
        alarm(10);
        layout_enter(g_sh->segv_trap_addr,
                     (u32)(uintptr_t)stack + (3u << 20));
        _exit(3);
    }

    if (waitpid(pid, &status, 0) < 0) {
        printf("[СБОЙ] waitpid не удался\n");
        return 1;
    }

    printf("\n--- фаза SIGSEGV: поле адреса инструкции ---\n");
    printf("инструкция `lui $4,0` по адресу : 0x%08X\n", g_sh->segv_trap_addr);
    printf("потомок                          : код %d, сигнал %d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    printf("обработчик вызван                : %s\n",
           g_sh->segv_seen ? "да" : "НЕТ");
    printf("что на +%d                       : 0x%08X\n", UCTX_PC_OFFSET,
           g_sh->segv_at_32);
    if (g_sh->segv_off == 0xFFFFFFFFu)
        printf("ВЫВОД: адрес инструкции в кадре SIGSEGV НЕ найден — поле\n"
               "       по смещению +%d для этого сигнала не годится.\n",
               UCTX_PC_OFFSET);
    else
        printf("адрес инструкции найден на смещении +%03d (0x%08X)\n",
               g_sh->segv_off, g_sh->segv_off);

    printf("\nВЫВОД: раскладка измерена, а не выведена из заголовка.\n");
    return 0;
}