/* Вход в гостевой код: загрузить, подменить стабы и вызвать точку входа.
 *
 * Что это доказывает. До сих пор проверялось, что модуль «правильно лежит»
 * в памяти. Но лежать и работать — разные вещи: релокации могут быть верны,
 * а код всё равно упасть на первой же инструкции. Здесь выполняется настоящий
 * переход на гостевую точку входа.
 *
 * Исполнять гостя можно только на MIPS, то есть на плеере: на хосте x86 этот
 * тест запускать бессмысленно.
 *
 * Как устроено. Все стабы подменяются на одну функцию, которая считает вызовы и
 * возвращает 0. Гость при этом ничего не может сделать, кроме как уйти в
 * подменённую функцию, поэтому любое его действие видно в логе, а падение
 * означает, что дело в стеке или в релокациях, а не в отсутствии ОС.
 *
 * Почему не подменяется gp. Гостевой код линкован как НЕ-PIC: обращения идут
 * через lui/addiu с релокациями, а не через $gp. Проверено на всём .text
 * модуля — 736 071 строка дизасма, обращение через (gp) ровно одно, и оно
 * выглядит как данные, разобранные как код. Значение gp из сведений о модуле
 * для исполнения не требуется, поэтомуinline-ассемблер не нужен вовсе.
 *
 * Стек. Гостевой код сразу пишет прологом (addiu sp,sp,-16 ; sw ra,8(sp)),
 * поэтому нужен настоящий стек. Отдаём ему отдельный отрезок, чтобы порча
 * гостевым кодом не задела стек проверки.
 *
 * Отдельный процесс и предел времени. Падение или зависание гостя не должны
 * уносить проверку, поэтому запуск идёт в потомке, а в потомке стоит alarm —
 * иначе зациклившийся гость будет крутиться до бесконечности.
 */

#include "guest_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

/* Предел счёта вызовов: гостевой код может вызывать подмену миллионы раз, а
 * нам достаточно факта и первых вызовов. */
#define TRACE_LIMIT   1000
/* Предел времени на гостевой код, секунды. */
#define TRACE_TIMEOUT 10

/* Обёртка входа в гостя.
 *
 * Отдельная функция на ассемблере потому, что менять указатель стеля внутри
 * обычной функции нельзя: компилятор считает $sp неизменным и в своём коде
 * выхода восстановил бы прежнее значение, то есть вернул бы управление в
 * середину гостевого кода. Обёртка не имеет ни пролога, ни эпилога, поэтому
 * менять $sp в ней безопасно.
 *
 * Если гость всё-таки вернётся, он вернётся по $ra прямо в вызывающий код, на
 * место после вызова обёртки. Регистры соответствуют MIPS o32:
 *   $4 = адрес точки входа, $5 = вершина стека гостя,
 *   $6 и $7 = аргументы точки входа.
 *
 * Две особенности, обе стоили падения, и обе неочевидны.
 *
 * Первая: `.set noreorder` обязателен. Ассемблер GNU по умолчанию переставляет
 * инструкции в слот задержки `jr`, и запись `move sp,a1` перед `jr` была
 * перенесена ПОСЛЕ него. Проверено дизасмом собранного бинарника.
 *
 * Вторая: $5 нельзя использовать и как «вершину стека», и как «второй
 * аргумент гостя». После `move a1,a3` в $5 лежит уже аргумент, а не стек, и
 * `move sp,a1` записал в стек неверное значение — гостевой пролог исполнил
 * `addiu sp,sp,-16` от нуля и получил $sp = 0xFFFFFFF0. Поэтому промежуточные
 * значения кладутся в $8 и $9, которые аргументами не являются.
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

/* Адрес, на котором гостевой код сломался.
 *
 * Без этого падение неинформативно: SIGBUS и SIGSEGV на MIPS возникают и при
 * выборке команды по неотображённому адресу, и при невыровненном обращении, и
 * скажут только «гость упал». С si_addr различаются сразу: сбой в гостевой
 * памяти, в стеке гостя или где-то по чужому адресу.
 *
 * Сведения лежат в разделяемой анонимной странице, а не в обычных переменных:
 * запуск идёт в потомке, и после fork страницы копируются — запись потомка в
 * свою копию родитель не увидит.
 */
/* Сведения, которые должен увидеть родитель.
 *
 * Страница общая (MAP_SHARED), потому что запуск идёт в потомке: после fork
 * страницы копируются, и запись потомка в свою копию родитель не увидит.
 * Обычная переменная-счётчик даёт всегда 0 и выглядит как «подменённые стабы
 * не вызваны» независимо от того, что на самом деле произошло. */
struct fault_info {
    volatile u32 addr;
    volatile int sig;
    /* Регистры контекста в сбое: индексы 24..31 покрывают t8, t9, k0, k1,
     * gp, sp, fp, ra. */
    volatile u32 greg[8];
    /* Адрес инструкции сбоя. В musl поле называется pc и лежит сразу за
     * regmask/status в mcontext_t — это значение CP0 EPC, то есть ровно та
     * инструкция, на которой процессор остановился. */
    volatile u32 pc;
    volatile u32 status;
    /* Счётчик вызовов подменённых импортов — тоже здесь, по той же
     * причине. */
    volatile unsigned long calls;
    volatile unsigned long overflow;
    volatile u32 last_ra;
};

/* В ucontext от musl массив gregs содержит ровно 32 элемента — по одному на
 * общий регистр. Значения cp0 (в том числе EPC) в него не входят, поэтому
 * берём только 24..31: t8, t9, k0, k1, gp, sp, fp, ra. */
#define GREG_FIRST 24
#define GREG_COUNT 8

static struct fault_info *g_fault;

/* Обработчик ничего не пытается починить: состояние гостя после сбоя
 * недостоверно. Только сообщить и выйти понятным кодом.
 *
 * Снимается весь контекст, а не только si_addr: при сбое по выборке команды
 * на MIPS si_addr равен нулю и ничего не говорит, а адрес инструкции лежит
 * в EPC среди cp0-значений.
 */
static void fault_handler(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;

    if (g_fault != NULL) {
        int i;
        g_fault->addr = (u32)(uintptr_t)si->si_addr;
        g_fault->sig = sig;
        for (i = 0; i < GREG_COUNT; i++)
            g_fault->greg[i] = (u32)uc->uc_mcontext.gregs[GREG_FIRST + i];
        g_fault->pc = (u32)uc->uc_mcontext.pc;
        g_fault->status = uc->uc_mcontext.status;
    }
    _exit(100 + sig);
}

static void install_fault_handlers(void)
{
    struct sigaction sa;
    stack_t ss;
    static char altstack[64 * 1024];
    int sigs[3];
    int i;

    /* Обработчик обязан иметь собственный стек.
     *
     * У гостя указатель стека переключён на его собственный отрезок, и если
     * сбой происходит именно там, где стек гостя непригоден, обработчик,
     * запущенный на нём же, не выполнится — процесс убьётся с исходным
     * сигналом, и адрес сбоя останется неизвестным. */
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    sigs[0] = SIGBUS;
    sigs[1] = SIGSEGV;
    sigs[2] = SIGILL;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;

    for (i = 0; i < 3; i++)
        sigaction(sigs[i], &sa, NULL);
}

/* Подмена импорта: считает вызов и возвращает 0. Нулевой возврат выбран
 * потому, что почти все вызываемые функции возвращают код ошибки, и ноль для
 * них означает «успех» — гость пойдёт дальше по своему коду, а не зависнет на
 * первой нереализованной функции. */
static void trace_call(unsigned int which)
{
    unsigned long n;
    (void)which;
    if (g_fault == NULL)
        return;
    n = g_fault->calls;
    if (n >= TRACE_LIMIT) {
        g_fault->overflow++;
        return;
    }
    g_fault->calls = n + 1;
}

static u8 *load_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    u8 *buf;
    long size;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)size;
    return buf;
}

/* Проверка цепочки вызова в памяти перед входом.
 *
 * Чтение обратно показывает, что записано, но НЕ показывает, что процессор
 * исполнит то же: на MIPS инструкции берутся из отдельного кэша команд. Если
 * сброс кэша не сработал, процессор исполнит прежнее содержимое — все слова
 * будут верными, а поведение нет. */
static void dump_chain(const GuestModule *mod,
                       const GuestImportTable *imports)
{
    const u32 *stub, *tramp;
    u32 pc_jal, jal_word, jal_target;

    if (imports->count == 0 || imports->libs[0].count == 0)
        return;
    stub = (const u32 *)(uintptr_t)imports->libs[0].imports[0].stub_runtime;
    tramp = (const u32 *)(uintptr_t)mod->tramp_base;

    printf("цепь вызова (первый импорт):\n");
    printf("  стаб     0x%08X: %08x %08x\n",
           (u32)(uintptr_t)stub, stub[0], stub[1]);
    printf("  трамплин 0x%08X: %08x %08x %08x\n",
           (u32)(uintptr_t)tramp, tramp[0], tramp[1], tramp[2]);

    /* Первый jal из точки входа гостя: пятая инструкция, смещение 0x1c. */
    pc_jal = mod->base + mod->entry + 0x1c;
    jal_word = *(const u32 *)(uintptr_t)pc_jal;
    jal_target = ((jal_word & 0x03FFFFFFu) << 2) | (pc_jal & 0xF0000000u);
    printf("  jal гостя 0x%08X: %08x -> цель 0x%08X (стаб 0x%08X) %s\n",
           pc_jal, jal_word, jal_target, (u32)(uintptr_t)stub,
           jal_target == (u32)(uintptr_t)stub ? "совпал" : "НЕ СОВПАЛ");
}

/* Подмена стабов на обработчик, живущий ВНУТРИ гостя.
 *
 * Зачем варианты. Наблюдение противоречивое: с подменой гость падает на
 * ПЕРВОМ вызове импорта, а совсем без подмены (стабы `jr $ra; nop`) уходит
 * далеко по своему коду. Значит подмена меняет поведение гостя, и непонятно,
 * что именно — запись в память или порча регистров. Варианты отличаются от
 * исходного стаба ровно одной деталью, поэтому сравнение однозначно:
 *
 *   0  jr $ra ; nop                  байт в байт как исходный стаб
 *   1  addiu $t8,$zero,1 ; jr ; nop  портится только $t8, память не трогается
 *   2  lui/addiu $t9 ; jr ; nop      портятся $t8 и $t9, память не трогается
 *   3  запись $ra в кольцевой буфер  полная запись, ради которой всё затевалось
 *
 * Если вариант 1 или 2 ломает гостя, а 0 не ломает — виноват регистр: значит
 * гостевой код использует $t8/$t9 сверх того, что позволяет ABI. Если
 * ломает только 3 — виновата запись в память.
 *
 * Обработчик лежит в гостевом отображении, поэтому наш код не участвует.
 */
#define HANDLER_MAX_WORDS 16

static u32 install_guest_handler(GuestModule *mod, GuestImportTable *imports,
                                 struct fault_info *shared, int variant)
{
    u32 h = mod->tramp_next;
    u32 *code = (u32 *)(uintptr_t)h;
    u32 n = 0;
    u32 base = (u32)(uintptr_t)shared;
    u32 li, ii;

    if (variant == 3) {
        u32 hi = (base + 0x8000u) >> 16;
        u32 lo = base & 0xFFFFu;
        code[n++] = 0x3C080000u | (hi & 0xFFFFu);   /* lui   $t8,hi */
        code[n++] = 0x25080000u | lo;               /* addiu $t8,$t8,lo */
        code[n++] = 0x8D090000u;                     /* lw    $t9,0($t8) */
        code[n++] = 0xAD880010u;                     /* sw    $ra,16($t8) */
        code[n++] = 0x25290001u;                     /* addiu $t9,$t9,1 */
        code[n++] = 0xAD090000u;                     /* sw    $t9,0($t8) */
    } else if (variant == 2) {
        code[n++] = 0x3C090000u;                     /* lui   $t9,hi */
    } else if (variant == 1) {
        code[n++] = 0x24080001u;                     /* addiu $t8,$zero,1 */
    }
    /* $v0 = 0 перед возвратом — обязательно.
     *
     * Функции PSP возвращают int в $v0, и трамплин $v0 не трогает: гость
     * получал бы мусор вместо кода возврата. Для функции, возвращающей
     * указатель на память, это означает «указатель» из мусора и немедленный
     * прыжок по произвольному адресу. Именно это и наблюдалось: процессор
     * оказывался в данных.
     *
     * Слово 0x00000000 здесь работает дважды: это `sll zero,zero,0`, то есть
     * nop, и одновременно `addu $v0,$zero,$zero`, то есть запись нуля в $v0. */
    code[n++] = 0x00000000u;                         /* $v0 = 0 */
    code[n++] = 0x03E00008u;                         /* jr    $ra */
    code[n++] = 0x00000000u;                         /* nop в слоте задержки */
    while (n < HANDLER_MAX_WORDS)
        code[n++] = 0x00000000u;

    mod->tramp_next = h + HANDLER_MAX_WORDS * 4;
    guest_flush_icache((void *)(uintptr_t)h, HANDLER_MAX_WORDS * 4);

    for (li = 0; li < imports->count; li++) {
        for (ii = 0; ii < imports->libs[li].count; ii++) {
            u8 *stub = (u8 *)(uintptr_t)imports->libs[li].imports[ii].stub_runtime;
            u32 imm = (h & 0x0FFFFFFCu) >> 2;

            /* Стаб: j обработчик ; nop — ровно 8 байт.
             *
             * Старший байт 0x08, а НЕ 0x80: опкод j в битах 31..26 равен
             * 000010, то есть 0x08 в старшем байте. При 0x80 получается
             * инструкция SPECIAL с мусорными полями. */
            stub[0] = (u8)(0x08 | ((imm >> 24) & 0x03));
            stub[1] = (u8)((imm >> 16) & 0xFF);
            stub[2] = (u8)((imm >> 8) & 0xFF);
            stub[3] = (u8)(imm & 0xFF);
            stub[4] = 0;
            stub[5] = 0;
            stub[6] = 0;
            stub[7] = 0;
            guest_flush_icache(stub, 8);
        }
    }
    return h;
}

int main(int argc, char **argv)
{
    u8 *image;
    size_t image_size = 0;
    GuestModule mod;
    GuestModuleInfo info;
    GuestImportTable imports;
    void *stack;
    const size_t STACK_SIZE = 256u * 1024u;
    u32 entry_addr;
    int patched = 0;
    int guest_handler;
    int no_patch;
    int handler_variant = 3;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <расшифрованный EBOOT.BIN> "
                        "[--guest-handler N|--no-patch]\n", argv[0]);
        return 2;
    }
    guest_handler = (argc > 2 && strcmp(argv[2], "--guest-handler") == 0);
    no_patch = (argc > 2 && strcmp(argv[2], "--no-patch") == 0);
    if (guest_handler && argc > 3) {
        char *end = NULL;
        handler_variant = (int)strtol(argv[3], &end, 10);
        if (end == argv[3])
            handler_variant = 3;
    }

    image = load_file(argv[1], &image_size);
    if (image == NULL) {
        fprintf(stderr, "не прочитать %s\n", argv[1]);
        return 1;
    }

    printf("=== вход в гостевой код ===\n");
    printf("образ: %s (%lu байт)\n\n", argv[1], (unsigned long)image_size);

    if (!guest_load(&mod, image, image_size, 0)) {
        printf("[СБОЙ] загрузка: %s\n", mod.error);
        free(image);
        return 1;
    }
    if (!guest_read_module_info(&mod, &info)) {
        printf("[СБОЙ] сведения о модуле не прочитаны\n");
        guest_unload(&mod);
        free(image);
        return 1;
    }
    if (!guest_scan_imports(&mod, &info, &imports)) {
        printf("[СБОЙ] импорты не перечислены: %s\n", mod.error);
        guest_unload(&mod);
        free(image);
        return 1;
    }

    printf("модуль %s, импортов %u\n", info.name, imports.total_imports);

    g_fault = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_fault == MAP_FAILED) {
        printf("[СБОЙ] не выделить разделяемую страницу\n");
        free(image);
        return 1;
    }

    stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        printf("[СБОЙ] не выделить стек гостя\n");
        guest_free_imports(&imports);
        guest_unload(&mod);
        free(image);
        return 1;
    }

    /* Подмена стабов: либо на функцию хостской программы, либо на обработчик
     * внутри гостевого образа. */
    if (no_patch) {
        /* Стабы не трогаем. В розничном образе они `jr $ra; nop`, то есть
         * безобидные: каждый вызов импорта просто возвращается назад. Это
         * позволяет посмотреть, куда гость полезет сам, без нашей подмены, и
         * отличить «гость упирается в отсутствие ОС» от «наша подмена ломает
         * что-то по дороге». */
        patched = 0;
        printf("подмена: нет, стабы оставлены как в образе (jr $ra; nop)\n");
    } else if (guest_handler) {
        install_guest_handler(&mod, &imports, g_fault, handler_variant);
        patched = (int)imports.total_imports;
        printf("подмена: обработчик внутри гостя, вариант %d\n",
               handler_variant);
    } else {
        u32 li, ii;
        for (li = 0; li < imports.count; li++) {
            for (ii = 0; ii < imports.libs[li].count; ii++) {
                GuestStubPatch p;
                p.target = (u32)(uintptr_t)&trace_call;
                p.stub = imports.libs[li].imports[ii].stub_runtime;
                if (!guest_patch_stub(&mod, &p)) {
                    printf("[СБОЙ] стаб %d не пропатчен: %s\n", patched,
                           mod.error);
                    guest_free_imports(&imports);
                    guest_unload(&mod);
                    free(image);
                    return 1;
                }
                patched++;
            }
        }
        printf("подмена: функции хостской программы\n");
    }
    printf("стабов подменено: %d\n", patched);

    entry_addr = mod.base + mod.entry;

    dump_chain(&mod, &imports);

    /* Что реально лежит в памяти вокруг предполагаемой точки сбоя.
     *
     * Смещение 0x2CF2B4 — это .lib.ent + 4, второе слово таблицы
     * экспортируемых функций модуля. Если в файле там одно, а в памяти
     * другое, значит под вывод попала релокация, и это уже другой диагноз,
     * чем «гость прыгнул в данные». */
    {
        u32 v = 0x2CF2A0;
        const u32 *q = (const u32 *)(uintptr_t)(mod.base + v);
        int i;
        printf("память гостя со смещения 0x%08X:\n", v);
        for (i = 0; i < 10; i++)
            printf("  0x%08X: 0x%08X\n", v + 4 * i, q[i]);
    }

    /* Печатаем собственный $gp. Обработчики гостя вызываются из гостевого
     * кода с тем $gp, какой останется после его работы, поэтому важно знать,
     * совпадает ли он с нашим: если нет, любой наш код, скомпилированный с
     * оптимизацией через $gp, при вызове из гостя уйдёт мимо своего образа. */
    {
        register u32 own_gp __asm__("$gp");
        extern char _gp[];
        printf("раскладка нашей памяти:\n");
        printf("  trace_call   = 0x%08X\n", (u32)(uintptr_t)&trace_call);
        printf("  &_gp         = 0x%08X (символ компоновщика)\n",
               (u32)(uintptr_t)_gp);
        printf("  $gp в коде   = 0x%08X\n", own_gp);
        printf("  расхождение  = %d\n",
               (int)(own_gp - (u32)(uintptr_t)_gp));
    }

    /* Перед прыжком показываем, что по точке входа действительно лежит код, а
     * не нули: при неверных релокациях или отсутствии сброса кэша это видно
     * сразу, и искать потом нечего. */
    printf("\nзапуск точки входа 0x%08X в потомке...\n", entry_addr);
    printf("стек гостя: 0x%08X .. 0x%08X\n",
           (u32)(uintptr_t)stack, (u32)(uintptr_t)stack + STACK_SIZE);
    {
        const u32 *p = (const u32 *)(uintptr_t)entry_addr;
        int i;
        printf("первые инструкции: ");
        for (i = 0; i < 6; i++)
            printf("%08x ", p[i]);
        printf("\n");
    }

    {
        pid_t pid = fork();
        int status = 0;

        if (pid < 0) {
            printf("[СБОЙ] fork не удался\n");
            guest_free_imports(&imports);
            guest_unload(&mod);
            free(image);
            return 1;
        }
        if (pid == 0) {
            /* Предел времени: иначе зациклившийся гость не закончится. */
            alarm(TRACE_TIMEOUT);
            signal(SIGALRM, SIG_DFL);
            install_fault_handlers();

            guest_enter(entry_addr, (u32)(uintptr_t)stack + STACK_SIZE, 0, 0);
            _exit(0);
        }

        if (waitpid(pid, &status, 0) < 0) {
            printf("[СБОЙ] waitpid не удался\n");
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            printf("[СБОЙ] гость убит сигналом %d", sig);
            if (sig == SIGALRM)
                printf(" (превышено время %d с)\n", TRACE_TIMEOUT);
            else
                printf("\n");
        } else if (WEXITSTATUS(status) >= 100) {
            /* Обработчик сработал: сообщаем адрес. */
            printf("[СБОЙ] сигнал %d, si_addr 0x%08X\n",
                   g_fault->sig, g_fault->addr);
            {
                int i;
                int found = 0;
                printf("       инструкция сбоя (EPC): 0x%08X, CP0 status: 0x%08X\n",
                       g_fault->pc, g_fault->status);
                if (g_fault->pc != 0) {
                    u32 word = *(const u32 *)(uintptr_t)(g_fault->pc & ~3u);
                    if (g_fault->pc >= mod.base &&
                        g_fault->pc < mod.base + mod.span)
                        printf("       это гостевой код, слово 0x%08X "
                               "(смещение 0x%08X от базы)\n", word,
                               g_fault->pc - mod.base);
                    else
                        printf("       это не гостевой код; слово по этому "
                               "адресу 0x%08X\n", word);
                    /* Слова перед точкой сбоя: показывают, какой приказ
                     * привёл к ней. */
                    printf("       предыдущие слова:");
                    {
                        int k;
                        for (k = 1; k <= 4; k++) {
                            u32 a = (g_fault->pc & ~3u) - 4 * k;
                            u32 w2 = *(const u32 *)(uintptr_t)a;
                            printf(" [%08X]=%08X", a, w2);
                        }
                        printf("\n");
                    }
                }
                printf("       контекст: ");
                for (i = 0; i < GREG_COUNT; i++) {
                    u32 v = g_fault->greg[i];
                    printf("g[%d]=0x%08X ", GREG_FIRST + i, v);
                    if (!found && v >= mod.base && v < mod.base + mod.span)
                        found = 1;
                }
                printf("\n");
                printf("       гость 0x%08X .. 0x%08X, стек 0x%08X .. 0x%08X\n",
                       mod.base, mod.base + mod.span,
                       (u32)(uintptr_t)stack,
                       (u32)(uintptr_t)stack + STACK_SIZE);
                printf("       среди регистров есть адрес гостевого кода: %s\n",
                       found ? "да" : "нет");
            }
            if (g_fault->addr >= mod.base && g_fault->addr < mod.base + mod.map_size)
                printf("       адрес внутри гостевого образа "
                       "(смещение 0x%08X от базы 0x%08X)\n",
                       g_fault->addr - mod.base, mod.base);
            else if (g_fault->addr >= (u32)(uintptr_t)stack &&
                     g_fault->addr < (u32)(uintptr_t)stack + STACK_SIZE)
                printf("       адрес внутри стека гостя\n");
            else
                printf("       адрес вне гостевого образа и вне стека гостя\n");
        } else {
            printf("гость вернулся, код выхода %d\n", WEXITSTATUS(status));
        }
    }

    printf("\nвызовов подменённых импортов: %lu\n", g_fault->calls);
    if (g_fault->calls > 0)
        printf("адрес последнего места вызова: 0x%08X "
               "(смещение 0x%08X в гостевом коде)\n", g_fault->last_ra,
               (g_fault->last_ra >= mod.base)
                   ? g_fault->last_ra - mod.base : 0);
    if (g_fault->overflow)
        printf("вызовов сверх лимита %d: %lu\n", TRACE_LIMIT,
               g_fault->overflow);

    guest_free_imports(&imports);
    guest_unload(&mod);
    free(image);

    printf("\n=== итог ===\n");
    if (g_fault->calls == 0)
        printf("подменённые стабы не вызваны: падение вышло раньше первого "
               "вызова\n");
    else
        printf("гость исполнил код и дошёл до вызовов импортов\n");
    return 0;
}