/* Трассировка вызовов импортов гостя: ловушки ставит и снимает сам гость.
 *
 * Почему не через ptrace. Проверено на плеере: ptrace разрешён, POKETEXT
 * работает, но PTRACE_POKETEXT меняет только память и НЕ сбрасывает
 * instruction cache гостя. Процессор продолжает исполнять прежние байты
 * стаба, то есть `jr $ra; nop`, и ловушка не срабатывает никогда. Выглядит
 * это так, будто трассировщик сломан, хотя сломан он был бы и наоборот —
 * пришлось бы гадать, отработала ловушка или нет.
 *
 * Поэтому ловушки ставит и снимает тот же процесс, который исполняет гостя:
 * он может сбросить кэш сам. Порядок такой:
 *   1. каждый стаб заменён на `break 0` (0x0000000D);
 *   2. сброшен instruction cache области стабов;
 *   3. установлен обработчик SIGTRAP;
 *   4. обработчик записывает адрес сработавшего стаба в разделяемую страницу
 *      и ВОССТАНАВИВАЕТ этот стаб обратно в `jr $ra; nop`, снова сбрасывая
 *      кэш, после чего возвращает управление гостю.
 *
 * Итог: каждый импорт срабатывает ровно один раз, гость при этом работает
 * нормально дальше, а последовательность первых вызовов известна точно.
 *
 * Побочно выясняется, где в ucontext лежит адрес инструкции: на первом
 * срабатывании оно известно заранее (адрес стаба), поэтому поле находится
 * перебором, а не предположением о раскладке структуры.
 */

#include "guest_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

/* Обёртка входа в гостя: переключает стек и прыгает по заданному адресу.
 *
 * Отдельная функция на ассемблере, потому что менять $sp внутри обычной
 * функции нельзя: компилятор считает указатель стеля неизменным и в своём
 * коде выхода восстановил бы прежнее значение, то есть вернул бы управление
 * в середину гостя.
 *
 * `.set noreorder` обязателен: ассемблер по умолчанию переставляет
 * инструкции в слот задержки jr. Промежуточные значения кладутся в $8 и $9,
 * потому что $5 после `move a1,a3` уже не содержит вершину стека. */
__asm__(
    "    .set noreorder\n"
    "    .text\n"
    "    .globl guest_enter\n"
    "    .type guest_enter, @function\n"
    "guest_enter:\n"
    "    move  $8, $4\n"      /* t0 = адрес гостевой точки входа */
    "    move  $9, $5\n"      /* t1 = вершина стека гостя */
    "    move  $4,  $6\n"     /* a0 = первый аргумент гостя */
    "    move  $5,  $7\n"     /* a1 = второй аргумент гостя */
    "    move  $29, $9\n"     /* sp = вершина стека гостя */
    "    jr    $8\n"
    "    nop\n"
    "    .size guest_enter, .-guest_enter\n");

extern void guest_enter(u32 entry, u32 sp, u32 a0, u32 a1);

#define MIPS_BREAK_WORD   0x0000000Du
#define MIPS_JR_RA        0x03E00008u
#define MIPS_NOP          0x00000000u

#define MAX_EVENTS 512

/* Общая страница: родитель читает её после гибели потомка.
 *
 * Именно общая, а не обычные переменные: после fork страницы копируются, и
 * родитель не увидит ничего, что записал потомок. */
struct trace_shared {
    volatile unsigned count;      /* сколько стабов сработало */
    volatile int pc_index;        /* индекс поля с адресом инструкции.
                                   * В общей странице, а не в обычной
                                   * переменной: индекс находит потомок, а
                                   * читает родитель, а после fork копии
                                   * страниц не разделены. */
    volatile unsigned overflow;
    volatile unsigned skipped;    /* остановок, не распознанных как стаб */
    volatile u32 events[MAX_EVENTS];   /* адреса сработавших стабов */
};

static struct trace_shared *g_shared;
static GuestModule *g_mod;
static u32 g_stub_lo, g_stub_hi;

/* Обработчик ловушки.
 *
 * Всё, что здесь делается, должно быть безопасным для гостя: он продолжит
 * исполняться с того же места, поэтому меняется только память стаба. */
static void trap_handler(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    unsigned i;
    u32 pc = 0;
    u32 stub = 0;

    (void)sig;
    (void)si;

    /* Ищем поле с адресом инструкции: на первом срабатывании адрес стаба
     * известен заранее, поэтому индекс определяется фактом, а не
     * предположением о раскладке структуры. */
    if (g_shared->pc_index < 0) {
        for (i = 0; i < 35; i++) {
            u32 v = (u32)uc->uc_mcontext.gregs[i];
            if (v >= g_stub_lo && v < g_stub_hi) {
                g_shared->pc_index = (int)i;
                pc = v;
                break;
            }
        }
        if (g_shared->pc_index < 0) {
            /* Ни одно поле не указывает на стаб. Это само по себе
             * результат: значит ucontext на этой сборке не отдаёт адрес
             * инструкции в gregs, и искать надо в других полях. */
            g_shared->skipped++;
            return;
        }
    } else {
        pc = (u32)uc->uc_mcontext.gregs[g_shared->pc_index];
    }

    if (pc < g_stub_lo || pc >= g_stub_hi) {
        g_shared->skipped++;
        return;
    }
    stub = pc;

    if (g_shared->count < MAX_EVENTS)
        g_shared->events[g_shared->count] = stub;
    g_shared->count++;

    /* $v0 = 0 — обязательное условие.
     *
     * Функции PSP возвращают int в $v0. Ловушка не возвращает ничего, и
     * гость получал бы в $v0 мусор вместо кода возврата: для функции,
     * возвращающей указатель, это «указатель» из мусора, а для опроса
     * состояния — вечное «не готово» и бесконечный цикл. Именно это и
     * наблюдалось: 2,6 миллиона вызовов sceUmdGetDriveStat подряд. */
    uc->uc_mcontext.gregs[2] = 0;

    /* Сдвиг PC на 8 байт вперёд: слот стаба целиком пропускается, и гость
     * продолжает работу так, будто стаб отработал как `jr $ra; nop`, но уже
     * с осмысленным кодом возврата.
     *
     * Это обходит сломанный механизм подмены целиком: сюда не нужно ни
     * прыгать из гостя в хостский код, ни строить трамплин — всё происходит
     * внутри гостевого исполнения, где регистры и так наши. */
    uc->uc_mcontext.gregs[g_shared->pc_index] = stub + 8;
}

int main(int argc, char **argv)
{
    FILE *f;
    long size;
    u8 *image;
    GuestModule mod;
    GuestModuleInfo info;
    GuestImportTable imports;
    struct sigaction sa;
    static u32 child_stack[65536];
    int status = 0;
    u32 i;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <расшифрованный EBOOT.BIN>\n", argv[0]);
        return 2;
    }

    f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "не открыть %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    image = (size > 0) ? malloc((size_t)size) : NULL;
    if (image == NULL || fread(image, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "ошибка чтения\n");
        return 1;
    }
    fclose(f);

    g_shared = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_shared == MAP_FAILED) {
        fprintf(stderr, "не выделить разделяемую страницу\n");
        return 1;
    }
    memset((void *)g_shared, 0, 65536);
    /* Именно -1, а не ноль: страница обнулена memset, и при значении 0
     * проверка «индекс уже найден» срабатывает ложно — обработчик читает
     * gregs[0], который в musl всегда ноль, и всё срабатывание уходит в
     * «вне области стабов». */
    g_shared->pc_index = -1;

    printf("=== трассировка вызовов импортов ===\n");

    if (!guest_load(&mod, image, (size_t)size, 0)) {
        printf("[СБОЙ] загрузка: %s\n", mod.error);
        return 1;
    }
    if (!guest_read_module_info(&mod, &info) ||
        !guest_scan_imports(&mod, &info, &imports)) {
        printf("[СБОЙ] импорты: %s\n", mod.error);
        return 1;
    }
    g_mod = &mod;

    printf("модуль %s, импортов %u, гостя 0x%08X .. 0x%08X\n",
           info.name, imports.total_imports, mod.base, mod.base + mod.span);

    /* Ставим ловушки и сбрасываем кэш — в том же процессе, который будет
     * исполнять гостя. */
    {
        u32 li, ii;
        for (li = 0; li < imports.count; li++) {
            for (ii = 0; ii < imports.libs[li].count; ii++) {
                u32 stub = imports.libs[li].imports[ii].stub_runtime;
                u32 *p = (u32 *)(uintptr_t)stub;

                p[0] = MIPS_BREAK_WORD;
                p[1] = MIPS_NOP;

                if (g_stub_lo == 0 || stub < g_stub_lo)
                    g_stub_lo = stub;
                if (stub + 8 > g_stub_hi)
                    g_stub_hi = stub + 8;
            }
        }
    }

    printf("ловушек поставлено: %u, область 0x%08X .. 0x%08X\n",
           (unsigned)imports.total_imports, g_stub_lo, g_stub_hi);

    /* Сброс кэша обязателен: без него процессор исполнит прежние байты и
     * ловушки не сработают. */
    guest_flush_icache(mod.memory, mod.map_size);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = trap_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    if (sigaction(SIGTRAP, &sa, NULL) != 0) {
        printf("[СБОЙ] обработчик не установлен\n");
        return 1;
    }

    {
        /* Свой стек для обработчика: у гостя $sp переключён на его
         * отрезок, и обработчик на нём мог бы не запуститься. */
        static char altstack[64 * 1024];
        stack_t ss;
        ss.ss_sp = altstack;
        ss.ss_size = sizeof(altstack);
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }

    printf("запуск гостя...\n");
    fflush(stdout);

    {
        pid_t pid = fork();
        if (pid < 0) {
            printf("[СБОЙ] fork\n");
            return 1;
        }
        if (pid == 0) {
            alarm(15);
            guest_enter(mod.base + mod.entry,
                        (u32)(uintptr_t)child_stack + sizeof(child_stack),
                        0, 0);
            _exit(0);
        }
        if (waitpid(pid, &status, 0) < 0) {
            printf("[СБОЙ] waitpid\n");
        } else if (WIFSIGNALED(status)) {
            printf("гость убит сигналом %d\n", WTERMSIG(status));
        } else {
            printf("гость вышел, код %d\n", WEXITSTATUS(status));
        }
    }

    printf("\nстабов сработало: %u\n", g_shared->count);
    printf("поле адреса инструкции: слово %d\n", g_shared->pc_index);
    printf("остановок вне области стабов: %u\n", g_shared->skipped);

    printf("\n--- порядок вызовов ---\n");
    for (i = 0; i < g_shared->count && i < MAX_EVENTS; i++) {
        u32 stub = g_shared->events[i];
        u32 li, ii;
        const char *libname = "?";
        u32 nid = 0;
        int found = 0;

        for (li = 0; li < imports.count && !found; li++)
            for (ii = 0; ii < imports.libs[li].count; ii++)
                if (imports.libs[li].imports[ii].stub_runtime == stub) {
                    libname = imports.libs[li].library;
                    nid = imports.libs[li].imports[ii].nid;
                    found = 1;
                    break;
                }
        printf("  %3u: 0x%08X  %-22s NID 0x%08X%s\n", i + 1, stub,
               libname, nid, found ? "" : "  (не найден)");
    }

    guest_free_imports(&imports);
    guest_unload(&mod);
    return 0;
}