/* Раскладка регистров в PTRACE_GETREGS на этом ядре — определяется фактом.
 *
 * Зачем. Чтобы читать адрес инструкции и адрес возврата из трассировщика,
 * нужно знать, где в структуре они лежат. Догадываться по заголовкам ядра
 * рискованно: раскладка отличается между версиями, а нам важна именно эта.
 *
 * Как. Потомок записывает в каждый доступный регистр свою метку
 * (0xA0 + номер), затем останавливается. Родитель читает структуру и ищет
 * метки. Так смещения определяются измерением, а не предположением.
 *
 * Не записываются: zero по определению, sp — иначе не вернуться, и ra —
 * он принадлежит вызывающему коду.
 *
 * Одновременно проверяется PTRACE_CONT с ловушкой `break`: одиночные шаги
 * на этом ядре не работают, а вот продолжение исполнения с прерыванием по
 * breakpoint — единственный способ проследить путь гостя.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

/* Регистры MIPS o32 в порядке номеров ABI: 1=at, 2..3=v0,v1, 4..7=a0..a3,
 * 8..15=t0..t7, 16..23=s0..s7, 24..27=t8,t9,k0,k1, 28=gp, 29=sp, 30=fp,
 * 31=ra. */
#define MARK_BASE 0xA0

/* Какие регистры помечаем: все, кроме zero (0), sp (29) и ra (31). */
static const int marked[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30
};
#define MARK_COUNT ((int)(sizeof(marked) / sizeof(marked[0])))

/* Регистры нельзя перечислить в asm-шаблоне по номеру, поэтому метки
 * расставляются явно: так надёжнее, чем таблица «номер → asm-строка». */
__asm__(
    "    .set noreorder\n"
    "    .globl plant_marks_asm\n"
    "    .type plant_marks_asm, @function\n"
    /* a0..a3 = метки 4..7 */
    "plant_marks_asm:\n"
    "    ori  $4,  $0, 0xA4\n"
    "    ori  $5,  $0, 0xA5\n"
    "    ori  $6,  $0, 0xA6\n"
    "    ori  $7,  $0, 0xA7\n"
    "    ori  $8,  $0, 0xA8\n"
    "    ori  $9,  $0, 0xA9\n"
    "    ori  $10, $0, 0xAA\n"
    "    ori  $11, $0, 0xAB\n"
    "    ori  $12, $0, 0xAC\n"
    "    ori  $13, $0, 0xAD\n"
    "    ori  $14, $0, 0xAE\n"
    "    ori  $15, $0, 0xAF\n"
    "    ori  $16, $0, 0xB0\n"
    "    ori  $17, $0, 0xB1\n"
    "    ori  $18, $0, 0xB2\n"
    "    ori  $19, $0, 0xB3\n"
    "    ori  $20, $0, 0xB4\n"
    "    ori  $21, $0, 0xB5\n"
    "    ori  $22, $0, 0xB6\n"
    "    ori  $23, $0, 0xB7\n"
    "    ori  $24, $0, 0xB8\n"
    "    ori  $25, $0, 0xB9\n"
    "    ori  $26, $0, 0xBA\n"
    "    ori  $27, $0, 0xBB\n"
    "    ori  $28, $0, 0xBC\n"
    "    ori  $30, $0, 0xBE\n"
    "    jr   $31\n"
    "    nop\n"
    "    .size plant_marks_asm, .-plant_marks_asm\n");

extern void plant_marks_asm(void);

int main(void)
{
    pid_t pid;
    int status = 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== раскладка регистров в PTRACE_GETREGS ===\n");

    pid = fork();
    if (pid < 0) {
        printf("[СБОЙ] fork не удался\n");
        return 1;
    }

    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(90);
        raise(SIGSTOP);
        plant_marks_asm();
        raise(SIGSTOP);
        _exit(0);
    }

    if (waitpid(pid, &status, 0) < 0) {
        printf("[СБОЙ] первый waitpid не удался\n");
        return 1;
    }

    /* Первый останов — перед метками: продолжаем до второго. */
    if (ptrace(PTRACE_CONT, pid, 0, 0) != 0) {
        printf("[СБОЙ] PTRACE_CONT не удался\n");
        return 1;
    }
    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        printf("[СБОЙ] второй останов не получен, status=0x%04X\n", status);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return 1;
    }

    /* Второй останов — метки расставлены. */
    {
        unsigned int v[80];
        int i;
        int ok = 1;

        memset(v, 0, sizeof(v));
        if (ptrace(PTRACE_GETREGS, pid, 0, v) != 0) {
            printf("[СБОЙ] PTRACE_GETREGS не удался\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return 1;
        }

        printf("структура: 32-битные поля\n");
        printf("смещение(байт)  значение  регистр\n");
        for (i = 0; i < 40; i++) {
            unsigned int val = v[i];
            int reg = -1;
            int k;
            for (k = 0; k < MARK_COUNT; k++)
                if (val == (unsigned int)(MARK_BASE + marked[k]))
                    reg = marked[k];
            if (reg < 0 && val != 0)
                ok = 0;
            if (reg >= 0)
                printf("  %3d       0x%08X  $%d\n", i * 4, val, reg);
        }
        printf("все ли метки найдены: %s\n", ok ? "да" : "НЕТ");

        /* Значения вне помеченных — полезно для понимания раскладки. */
        printf("\nнепомеченные ненулевые поля:\n");
        for (i = 0; i < 40; i++) {
            int reg = -1;
            int k;
            for (k = 0; k < MARK_COUNT; k++)
                if (v[i] == (unsigned int)(MARK_BASE + marked[k]))
                    reg = marked[k];
            if (reg < 0 && v[i] != 0)
                printf("  %3d       0x%08X\n", i * 4, v[i]);
        }
    }

    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return 0;
}