/*
 * Измерение: где в `ucontext` лежит адрес инструкции сбоя для SIGSEGV.
 *
 * Зачем. Для SIGTRAP поле по смещению 32 — это PC, измерено пробой
 * `tests/uctx_layout.c`. Для SIGSEGV оно недостоверно: в отчёте о сбое
 * гостя поле выдавало то `0x08ABD018`, то `0x08ABD01C`, то `0x08ABD014`
 * при одном и том же падении, и `si_addr` при этом тоже различался.
 *
 * Пока это так, сбой гостя локализовать нечем: видно, что упало, но не видно
 * на какой инструкции, и работа по импортам стоит.
 *
 * Как вызывается сбой. Чтение по адресу 0 на этом ядре SIGSEGV НЕ даёт
 * (записано в `Projects/AGENTS.md`), поэтому память выделяется, снимается
 * `munmap`, и после этого чтение гарантированно падает.
 *
 * Откуда известен PC. Инструкция, порождающая сбой, помечена глобальной
 * меткой в отдельном ассемблерном фрагменте, и её адрес передаётся обработчику
 * ДО сбой. Первая версия пробы подставляла адрес снятой страницы и искала
 * его в кадре — это ошибка: там надо искать адрес инструкции.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unistd.h>

#define SCAN_BYTES 1024

/* Значения-метки, уникальные и легко узнаваемые. */
#define M_AT  0x11111111u
#define M_V0  0x22222222u
#define M_A0  0x33333333u
#define M_A1  0x44444444u
#define M_A2  0x55555555u
#define M_A3  0x66666666u
#define M_T0  0x77777777u
#define M_T9  0x88888888u
#define M_GP  0x99999999u
#define M_SP  0xAAAA0000u
#define M_FP  0xBBBB0000u
#define M_RA  0xCCCC0000u

/* Инструкция, порождающая сбой, и её точный адрес.
 *
 * `$a0` — адрес снятой страницы. Метка стоит ПЕРЕД чтением, то есть адрес
 * инструкции `lw` равен адресу метки. */
__asm__(
    ".set noreorder\n"
    ".text\n"
    ".globl segv_site\n"
    ".type segv_site, @function\n"
    "segv_site:\n"
    "    lw     $2, 0($4)\n"
    "    nop\n"
    ".set reorder\n"
    ".size segv_site, .-segv_site\n");

extern unsigned segv_site;

/* Третий вид сбоя: ЗАПИСЬ по снятой странице.
 *
 * Его нельзя не измерить: первые два (чтение и выборка команды) дали разные
 * смещения PC, а гостевой поток падает именно на записи
 * (`0x08ABD018  sw $s5,4($k0)`). Если для записи раскладка снова другая, то
 * PC, прочитанный по смещению, измеренному для чтения, окажется чужим —
 * ровно то, что произошло с рантаймом. */
__asm__(
    ".set noreorder\n"
    ".text\n"
    ".globl store_site\n"
    ".type store_site, @function\n"
    "store_site:\n"
    "    sw     $2, 0($4)\n"
    "    nop\n"
    ".set reorder\n"
    ".size store_site, .-store_site\n");

extern unsigned store_site;

/* Второй вид сбоя: ВЫБОРКА КОМАНДЫ по снятой странице.
 *
 * Это не то же самое, что сбой по данным. На MIPS для выборки команды ядро
 * строит иной кадр, и PC может лежать в нём иначе — а именно такой сбой
 * подозревается в гостевом потоке (`si_addr` = 0 при живом `$k0`: читать
 * нечего, значит прыгнули по нулю). */
__asm__(
    ".set noreorder\n"
    ".text\n"
    ".globl fetch_site\n"
    ".type fetch_site, @function\n"
    "fetch_site:\n"
    "    jr     $4\n"
    "    nop\n"
    ".set reorder\n"
    ".size fetch_site, .-fetch_site\n");

extern unsigned fetch_site;

/* Четвёртый вид: выборка команды ПО АДРЕСУ 0.
 *
 * Это ровно то, что делает гостевой поток: `si_addr` = 0 и PC = 0. Отдельно
 * от `fetch` (адрес высокий, снятая страница) — ядро может обрабатывать
 * нулевой адрес особым путём, и тогда раскладка окажется третьей. Три
 * предыдущих измерения шли по непустому адресу и этого не покрывали. */
__asm__(
    ".set noreorder\n"
    ".text\n"
    ".globl fetch0_site\n"
    ".type fetch0_site, @function\n"
    "fetch0_site:\n"
    "    jr     $0\n"
    "    nop\n"
    ".set reorder\n"
    ".size fetch0_site, .-fetch0_site\n");

extern unsigned fetch0_site;

static volatile unsigned fault_expected_pc;
static volatile unsigned fault_addr;

static void handler(int sig, siginfo_t *si, void *ctx)
{
    const unsigned char *p = (const unsigned char *)ctx;
    unsigned off;
    int found;

    printf("сигнал            : %d\n", sig);
    printf("ожидаемый PC      : 0x%08X\n", fault_expected_pc);
    printf("ожидаемый адрес   : 0x%08X\n", fault_addr);
    printf("si_addr           : 0x%08X\n",
           (unsigned)(uintptr_t)si->si_addr);

    /* 1. Где в кадре лежит PC, и ЧТО лежит на каждом из таких смещений.
     *
     * PC находится сразу в трёх местах (+0x20, +0x40, +0xF0 — на этом ядре).
     * Какое из них достоверно для рантайма, выясняется сравнением: если все
     * три равны ожидаемому, все пригодны; если расходятся, читать надо то,
     * которое указывает на инструкцию, а не на следующую за ней.
     *
     * Первая версия пробы печатала только смещения, и этого не хватало:
     * непонятно, совпадают ли по значению. */
    printf("PC в кадре        : ");
    found = 0;
    for (off = 0; off + 4 <= SCAN_BYTES; off += 4) {
        unsigned v;

        memcpy(&v, p + off, sizeof(v));
        if (v == fault_expected_pc) {
            printf("+0x%03X%s", off, found ? ", " : "");
            found = 1;
        }
    }
    printf("%s\n", found ? "" : " НЕ НАЙДЕН НИГДЕ");

    printf("значения по +32/+40/+F0: ");
    {
        static const unsigned probe_offs[3] = { 0x20, 0x40, 0xF0 };
        unsigned i2;

        for (i2 = 0; i2 < 3; i2++) {
            unsigned v2;

            memcpy(&v2, p + probe_offs[i2], sizeof(v2));
            printf("0x%08X ", v2);
        }
        printf("\n");
    }

    /* 2. Все метки: где какая. Печатаются поимённо, потому что одиночный
     *    фильтр «похоже на адрес» молчит из-за нулевых старших слов. */
    printf("метки в кадре     :\n");
    for (off = 0; off + 4 <= SCAN_BYTES; off += 4) {
        unsigned v;

        memcpy(&v, p + off, sizeof(v));
        switch (v) {
        case M_AT:  printf("  +0x%03X  $at\n", off); break;
        case M_V0:  printf("  +0x%03X  $v0\n", off); break;
        case M_A0:  printf("  +0x%03X  $a0\n", off); break;
        case M_A1:  printf("  +0x%03X  $a1\n", off); break;
        case M_A2:  printf("  +0x%03X  $a2\n", off); break;
        case M_A3:  printf("  +0x%03X  $a3\n", off); break;
        case M_T0:  printf("  +0x%03X  $t0\n", off); break;
        case M_T9:  printf("  +0x%03X  $t9\n", off); break;
        case M_GP:  printf("  +0x%03X  $gp\n", off); break;
        case M_SP:  printf("  +0x%03X  $sp\n", off); break;
        case M_FP:  printf("  +0x%03X  $fp\n", off); break;
        case M_RA:  printf("  +0x%03X  $ra\n", off); break;
        default: break;
        }
    }

    _exit(0);
}

/* Выполнить сбой нужного вида. Отдельная функция, а не кусок main: режим
 * после fork вызывает её и в потомке, иначе получился бы fork внутри
 * проверки и рекурсия.
 *
 * Ни одна ветка не возвращается: обработчик завершает процесс через _exit. */
static void do_one(const char *mode, void *p)
{
    if (strcmp(mode, "fetch0") == 0) {
        void (*go)(void) = (void (*)(void))&fetch0_site;

        fault_expected_pc = 0;   /* PC и ЕСТЬ ноль, искать надо ноль */
        go();
    } else if (strcmp(mode, "fetch") == 0) {
        void (*go)(void) = (void (*)(void))&fetch_site;

        fault_expected_pc = (unsigned)(uintptr_t)&fetch_site;
        go();
    } else if (strcmp(mode, "store") == 0) {
        void (*go)(unsigned) = (void (*)(unsigned))&store_site;

        fault_expected_pc = (unsigned)(uintptr_t)&store_site;
        go((unsigned)(uintptr_t)p);
    } else {
        unsigned (*site)(unsigned) = (unsigned (*)(unsigned))&segv_site;

        fault_expected_pc = (unsigned)(uintptr_t)&segv_site;
        site((unsigned)(uintptr_t)p);
    }

    printf("сигнала не было — проба не сработала\n");
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    const char *mode;
    void *p;

    /* Вывод небуферизованный: обработчик завершает процесс через `_exit`,
     * который буферы НЕ сбрасывает, и без этого весь вывод пропадает. */
    setvbuf(stdout, NULL, _IONBF, 0);

    mode = (argc > 1) ? argv[1] : "data";
    if (strcmp(mode, "data") != 0 && strcmp(mode, "fetch") != 0
        && strcmp(mode, "store") != 0 && strcmp(mode, "fetch0") != 0) {
        fprintf(stderr,
                "Использование: uctx_segv [data|fetch|store|fetch0]\n");
        return 2;
    }
    printf("вид сбоя         : %s\n",
           strcmp(mode, "fetch") == 0 ? "выборка команды"
           : strcmp(mode, "fetch0") == 0 ? "выборка команды по адресу 0"
           : strcmp(mode, "store") == 0 ? "запись" : "чтение");

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        perror("sigaction");
        return 2;
    }

    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    if (munmap(p, 4096) != 0) {
        perror("munmap");
        return 2;
    }
    fault_addr = (unsigned)(uintptr_t)p;

    /* Режим после fork: гостевой поток падает именно в потомке, на стеке,
     * выделенном родителем через mmap. Это единственная измеренная разница
     * между пробой и реальностью, и кадр сигнала может отличаться из-за неё. */
    if (getenv("FORKED") != NULL) {
        pid_t pid = fork();
        int st;

        if (pid == 0) {
            printf("в потомке, pid=%d\n", (int)getpid());
            do_one(mode, p);
            _exit(1);
        }
        if (pid < 0) {
            perror("fork");
            return 2;
        }
        if (waitpid(pid, &st, 0) < 0) {
            perror("waitpid");
            return 2;
        }
        printf("потомок завершился: код %d, сигнал %d\n",
               WIFEXITED(st) ? WEXITSTATUS(st) : -1,
               WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        return 0;
    }

    do_one(mode, p);
    printf("сигнала не было — проба не сработала\n");
    return 1;
}
