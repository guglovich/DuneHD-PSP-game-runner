/* Трассировка вызовов импортов гостевого кода через точки останова.
 *
 * Зачем. Единственный способ узнать, куда гостевой код передаёт управление
 * при первом же вызове импорта. Все прочие способы исчерпаны: чтение памяти,
 * разбор дизасма, сравнение цепочек вызовов — дали противоречивые результаты.
 *
 * Почему точки останова, а не пошаговое выполнение. На этом ядре ptrace
 * разрешён (PTRACE_TRACEME, PTRACE_GETREGS, PTRACE_CONT работают), но
 * PTRACE_SINGLESTEP не работает — проверено отдельной пробой. Единственный
 * оставшийся способ проследить путь — подставить в интересующие места
 * инструкцию `break` и ловить SIGTRAP.
 *
 * Почему не нужна раскладка регистров. Для software-breakpoint ядро сообщает
 * в siginfo адрес точки останова, то есть si_addr — это и есть нужный адрес.
 * Определять, где в PTRACE_GETREGS лежит EPC, не требуется.
 *
 * Как. Родитель и потомок независимо загружают один и тот же образ по
 * одной базе, поэтому адреса стабов у них совпадают. Потомок готовит гостя
 * и останавливается; родитель подставляет `break` в каждый стаб через
 * PTRACE_POKETEXT и запускает исполнение. Каждый SIGTRAP — это вызов
 * импорта, адрес известен из si_addr.
 *
 * Предел вызовов обязателен: гость может звать импорты миллионы раз, и без
 * ограничения трассировка не кончится.
 */

#include "guest_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

/* Обёртка входа в гостя: переключает стек и прыгает по заданному адресу.
 *
 * Отдельная функция на ассемблере, потому что менять $sp внутри обычной
 * функции нельзя: компилятор считает указатель стеля неизменным и в своём
 * коде выхода восстановил бы прежнее значение, то есть вернул бы управление
 * в середину гостя.
 *
 * `.set noreorder` обязателен: ассемблер по умолчанию переставляет
 * инструкции в слот задержки jr, и запись перед jr оказывалась после него.
 */
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

extern void guest_enter(u32 entry, u32 sp, u32 arg0, u32 arg1);

/* MIPS `break`: opcode SPECIAL (0), funct 0x0D. */
#define MIPS_BREAK_WORD 0x0000000Du

/* Область стабов в памяти гостя. По ней опознаётся поле PC в дампе
 * регистров: адрес сработавшей ловушки лежит именно здесь, а ничего
 * другого из гостя в этот диапазон не попадает. */
struct stub_range { u32 lo, hi; };

#define MAX_TRACE_CALLS 4000
#define MAX_REPORT      120

/* Отображение гостя в адресном пространстве этого процесса. Загрузка
 * выполняется и в родителе, и в потомке: адреса стабов у них совпадают,
 * и родителю не нужно читать память потомка. */
struct view {
    GuestModule mod;
    GuestModuleInfo info;
    GuestImportTable imports;
};

static int prepare(const char *path, struct view *v)
{
    FILE *f = fopen(path, "rb");
    u8 *image;
    long size;

    if (f == NULL) {
        fprintf(stderr, "не открыть %s\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return 0;
    }
    image = malloc((size_t)size);
    if (image == NULL || fread(image, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(image);
        return 0;
    }
    fclose(f);

    if (!guest_load(&v->mod, image, (size_t)size, 0)) {
        fprintf(stderr, "загрузка не удалась: %s\n", v->mod.error);
        free(image);
        return 0;
    }
    if (!guest_read_module_info(&v->mod, &v->info) ||
        !guest_scan_imports(&v->mod, &v->info, &v->imports)) {
        fprintf(stderr, "импорты не перечислены: %s\n", v->mod.error);
        free(image);
        return 0;
    }
    return 1;
}

/* Адрес стаба -> описание импорта, для отчёта о вызовах. */
static const GuestImport *find_import(const struct view *v, u32 stub,
                                      unsigned *lib_index)
{
    u32 li, ii;
    for (li = 0; li < v->imports.count; li++)
        for (ii = 0; ii < v->imports.libs[li].count; ii++)
            if (v->imports.libs[li].imports[ii].stub_runtime == stub) {
                if (lib_index != NULL)
                    *lib_index = li;
                return &v->imports.libs[li].imports[ii];
            }
    return NULL;
}

/* Ищет в дампе регистров слово, попадающее в диапазон стабов: это и есть
 * адрес инструкции, на которой сработала ловушка, то есть поле PC.
 *
 * Так раскладка структуры определяется фактом, а не предположением о
 * заголовках ядра: si_addr на этом ядре для ptrace-остановок не заполняется,
 * а перебирать известные смещения вслепую — значит гадать. */
static int find_pc_offset(const unsigned int *regs, int count,
                         const struct stub_range *sr)
{
    int i;
    for (i = 0; i < count; i++)
        if (regs[i] >= sr->lo && regs[i] < sr->hi)
            return i;
    return -1;
}

static void print_stub_table(const struct view *v)
{
    u32 li, ii;
    for (li = 0; li < v->imports.count; li++) {
        for (ii = 0; ii < v->imports.libs[li].count; ii++) {
            const GuestImport *im = &v->imports.libs[li].imports[ii];
            printf("0x%08X %-24s 0x%08X\n", im->stub_runtime,
                   v->imports.libs[li].library, im->nid);
        }
    }
}

int main(int argc, char **argv)
{
    struct view v;
    struct view cv;
    pid_t pid;
    int status = 0;
    unsigned long calls = 0;
    int reported = 0;
    int pc_offset = -1;
    struct stub_range sr;
    unsigned int regs[40];

    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <расшифрованный EBOOT.BIN>\n", argv[0]);
        return 2;
    }

    /* Порядок принципиален: сначала fork, потом загрузка у обоих.
     *
     * Иначе не выйдет: fork копирует и отображение гостя тоже, поэтому в
     * потомке адрес 0x08804000 окажется уже занят родительским, и загрузка
     * справедливо откажется его трогать. Ошибка «адрес занят, mmap вернул
     * 0x2B5AC000» — ровно этот случай, и она же доказывает, что проверка
     * занятости адреса в загрузчике работает. */
    pid = fork();
    if (pid < 0) {
        printf("[СБОЙ] fork не удался\n");
        return 1;
    }

    if (pid == 0) {
        /* Потомок загружает гостя в СВОЁМ адресном пространстве: иначе
         * родитель подставил бы ловушки в неотображённые адреса.
         * База та же, поэтому адреса стабов совпадают. */
        static u32 child_stack[65536];   /* 256 КиБ */

        if (!prepare(argv[1], &cv)) {
            fprintf(stderr, "потомок: загрузка не удалась\n");
            _exit(91);
        }
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(90);
        /* Без TRACESYSGOOD остановки на системных вызовах неотличимы от
         * точек останова: на MIPS каждый вызов даёт SIGTRAP, и трассировщик
         * принял бы их за срабатывания ловушек — то есть за вызовы импортов.
         * С этим флагом остановки на вызовах приходят как SIGTRAP|0x80 и
         * отбрасываются явно. Флаг задаёт родитель после первой остановки:
         * из самого потомка PTRACE_SETOPTIONS не проходит. */
        raise(SIGSTOP);              /* ловушки ещё не подставлены */
        guest_enter(cv.mod.base + cv.mod.entry,
                    (u32)(uintptr_t)child_stack + sizeof(child_stack), 0, 0);
        _exit(0);
    }

    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        printf("[СБОЙ] потомок не остановился\n");
        return 1;
    }

    /* Родитель грузит гостя в своём пространстве — только ради списка
     * адресов стабов. С базой одинаковой адреса совпадают, читать память
     * потомка не требуется. */
    if (!prepare(argv[1], &v)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return 1;
    }

    printf("=== трассировка вызовов импортов ===\n");
    printf("модуль %s, импортов %u, гостя 0x%08X .. 0x%08X\n",
           v.info.name, v.imports.total_imports, v.mod.base,
           v.mod.base + v.mod.span);
    printf("стабов к трассировке: %u\n\n", v.imports.total_imports);

    if (ptrace(PTRACE_SETOPTIONS, pid, 0,
               (void *)(long)PTRACE_O_TRACESYSGOOD) != 0) {
        printf("[ПРЕДУПРЕЖДЕНИЕ] PTRACE_O_TRACESYSGOOD не задался: "
               "остановки на системных вызовах будут считаться за "
               "срабатывания ловушек\n");
    }

    sr.lo = sr.hi = 0;
    {
        u32 li, ii;
        for (li = 0; li < v.imports.count; li++)
            for (ii = 0; ii < v.imports.libs[li].count; ii++) {
                u32 a = v.imports.libs[li].imports[ii].stub_runtime;
                if (sr.lo == 0 || a < sr.lo)
                    sr.lo = a;
                if (a + 8 > sr.hi)
                    sr.hi = a + 8;
            }
    }
    printf("область стабов: 0x%08X .. 0x%08X\n", sr.lo, sr.hi);

    /* Подставляем ловушку в каждый стаб гостя этого процесса. */
    {
        u32 li, ii;
        unsigned planted = 0, failed = 0;

        for (li = 0; li < v.imports.count; li++) {
            for (ii = 0; ii < v.imports.libs[li].count; ii++) {
                u32 stub = v.imports.libs[li].imports[ii].stub_runtime;
                long prev = ptrace(PTRACE_POKETEXT, pid,
                                   (void *)(uintptr_t)stub,
                                   (void *)(long)MIPS_BREAK_WORD);
                if (prev == -1)
                    failed++;
                else
                    planted++;
            }
        }
        printf("ловушек подставлено: %u, не удалось: %u\n", planted, failed);
        if (planted == 0) {
            printf("[СБОЙ] PTRACE_POKETEXT не работает — трассировка "
                   "невозможна\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return 1;
        }
    }

    if (ptrace(PTRACE_CONT, pid, 0, 0) != 0) {
        printf("[СБОЙ] PTRACE_CONT не удался\n");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return 1;
    }

    while (calls < MAX_TRACE_CALLS) {
        /* Именно waitid, а не waitpid: waitpid не заполняет siginfo_t, и
         * si_addr остаётся нулём — то есть ловушка срабатывает, а адрес
         * неизвестен. С waitid ядро сообщает адрес точки останова. */
        siginfo_t si;
        memset(&si, 0, sizeof(si));

        if (waitid(P_PID, (id_t)pid, &si, WSTOPPED) < 0) {
            printf("[СБОЙ] waitid не удался\n");
            break;
        }
        if (si.si_code == CLD_EXITED) {
            printf("потомок вышел, код %d\n", si.si_status);
            break;
        }
        if (si.si_code == CLD_KILLED || si.si_code == CLD_DUMPED) {
            printf("потомок убит сигналом %d\n", si.si_status);
            break;
        }
        /* Ловушка приходит как CLD_TRAPPED, а не CLD_STOPPED: константы
         * CLD_* в Linux сдвинуты на единицу относительно POSIX, и ловушка
         * от ptrace — это именно TRAPPED. */
        if (si.si_code != CLD_TRAPPED && si.si_code != CLD_STOPPED) {
            printf("остановка не распознана: si_code=%d si_pid=%d "
                   "si_status=%d si_addr=0x%08X\n",
                   si.si_code, (int)si.si_pid, si.si_status,
                   (unsigned)si.si_addr);
            break;
        }
        /* Остановка на системном вызове: пропускаем, это не ловушка. */
        if (si.si_status == (SIGTRAP | 0x80))
            continue;
        if (si.si_status != SIGTRAP) {
            printf("остановка по сигналу %d после %lu вызовов\n",
                   si.si_status, calls);
            break;
        }

        {
            unsigned int regs[64];
            int off;
            memset(regs, 0, sizeof(regs));
            if (ptrace(PTRACE_GETREGS, pid, 0, regs) != 0) {
                printf("[СБОЙ] PTRACE_GETREGS не удался\n");
                break;
            }
            off = find_pc_offset(regs, 64, &sr);
            if (off < 0) {
                int k;
                printf("[СБОЙ] среди 64 слов нет адреса стаба. Дамп:\n");
                for (k = 0; k < 64; k++) {
                    if (regs[k] != 0)
                        printf("  [%2d] = 0x%08X\n", k, regs[k]);
                }
                printf("  ищем в диапазоне 0x%08X .. 0x%08X\n", sr.lo, sr.hi);
                break;
            }
            if (pc_offset < 0) {
                pc_offset = off;
                printf("поле PC в структуре регистров: слово %d "
                       "(откалибровано по первому срабатыванию)\n", pc_offset);
            }
            if (off != pc_offset) {
                printf("[СБОЙ] поле PC сместилось: было %d, стало %d\n",
                       pc_offset, off);
                break;
            }
        }

        calls++;
        if (reported < MAX_REPORT) {
            u32 stub = 0;
            unsigned vi = 0;
            const GuestImport *im;

            if (ptrace(PTRACE_GETREGS, pid, 0, regs) != 0)
                break;
            stub = regs[pc_offset];
            im = find_import(&v, stub, &vi);
            reported++;
            printf("  вызов %4lu: стаб 0x%08X", calls, stub);
            if (im != NULL)
                printf("  %-24s NID 0x%08X",
                       v.imports.libs[vi].library, im->nid);
            else
                printf("  (вне списка стабов)");
            printf("\n");
        }

        if (ptrace(PTRACE_CONT, pid, 0, 0) != 0) {
            printf("[СБОЙ] PTRACE_CONT после остановки не удался\n");
            break;
        }
    }

    if (calls >= MAX_TRACE_CALLS) {
        printf("\nдостигнут предел %d вызовов, трассировка остановлена\n",
               MAX_TRACE_CALLS);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    printf("\nвсего вызовов импортов: %lu\n", calls);

    /* Список стабов наружу: его используют другие инструменты, чтобы
     * сопоставить трассировку с именами. */
    printf("\n--- таблица стабов (адрес, библиотека, NID) ---\n");
    print_stub_table(&v);

    guest_free_imports(&v.imports);
    guest_unload(&v.mod);
    return 0;
}