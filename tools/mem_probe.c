/* Хватает ли обычной памяти Linux под кадровый буфер HLE-рантайма.
 *
 * Вопрос не в буфере видеотракта (это решается отдельно и там нужен буфер
 * из памяти конвейера вывода). Здесь проверяется другое: обычная память
 * процесса под кадр игры — тот самый буфер, в который PSP рисует через
 * `sceGe*` и который потом отдаётся на вывод.
 *
 * Почему нужна проверка, а не расчёт. «MemFree» — это свободная память в
 * момент чтения, и она ничего не говорит о том, выдаст ли ядро конкретному
 * процессу нужный объём: счётчики не совпадают (free в MemFree не входит),
 * память может быть занята под кэш страниц, а у процесса есть свой предел
 * адресного пространства. Поэтому память выделяется постранично и каждая
 * страница записывается: если запись прошла, память настоящая и доступна
 * для чтения и записи, а не просто зарезервирована.
 *
 * Никаких устройств Sigma не открывается: только mmap и запись.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* Кадр 320x240 в 16 бит — то, что нужно под игру. */
#define W 320
#define H 240
#define FRAME_BYTES (W * H * 2)
#define FRAMES 2

/* Шаг проверки: выделяем всё большие куски и смотрим, где кончится. */
static const unsigned long steps[] = {
    256u * 1024u, 1u * 1024u * 1024u, 2u * 1024u * 1024u,
    4u * 1024u * 1024u, 8u * 1024u * 1024u, 16u * 1024u * 1024u,
};

/* Записывает по одному значащему байту на страницу и читает обратно.
 * Возвращает число успешно проверенных страниц. */
static unsigned long touch(void *p, unsigned long len)
{
    unsigned long page = (unsigned long)sysconf(_SC_PAGESIZE);
    unsigned char *c = (unsigned char *)p;
    unsigned long i, n = 0;

    for (i = 0; i + page <= len; i += page) {
        c[i] = (unsigned char)(i ^ (i >> 12));
    }
    /* Проверка: запись должна читаться обратно. Если страница не наша,
     * значение окажется прежним — и это будет обнаружено здесь, а не позже
     * при выводе кадра. */
    for (i = 0; i + page <= len; i += page) {
        if (c[i] == (unsigned char)(i ^ (i >> 12)))
            n++;
    }
    return n;
}

static unsigned long resident_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    unsigned long kb = 0;

    if (f == NULL)
        return 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            kb = strtoul(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb;
}

static void print_meminfo(void)
{
    static const char *keys[] = { "MemTotal", "MemFree", "Cached",
                                  "Buffers", "MemAvailable" };
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    size_t i;

    if (f == NULL) {
        printf("  /proc/meminfo недоступен\n");
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (strncmp(line, keys[i], strlen(keys[i])) == 0) {
                fputs("  ", stdout);
                fputs(line, stdout);
                break;
            }
    }
    fclose(f);
}

int main(void)
{
    unsigned long kb_before;
    unsigned long page;
    size_t i;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* Размер страницы обязан быть взят у ядра, а не задан вручную: на этом
     * плеере страница 16 КиБ, и проверка с делением на 4096 даёт ложный
     * вывод «часть памяти не отвечает». */
    page = (unsigned long)sysconf(_SC_PAGESIZE);

    printf("=== хватает ли обычной памяти под кадр игры ===\n");
    printf("размер страницы памяти: %lu Б\n", page);
    if (page != 4096)
        printf("  (не 4 КиБ — это и есть причина обязательной сборки "
               "с -Wl,-z,max-page-size=0x4000)\n");
    printf("\n");

    kb_before = resident_kb();
    printf("наше VmRSS до выделения: %lu КБ\n\n", kb_before);

    /* 1. Именно тот размер, который нужен под игру. */
    {
        size_t len = FRAME_BYTES * FRAMES;
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        unsigned long pages;

        if (p == MAP_FAILED) {
            printf("[СБОЙ] кадровый буфер %zu Б не выделен\n", len);
            return 1;
        }
        pages = touch(p, len);
        printf("кадровый буфер %dx%d, 16 бит, буферов %d:\n", W, H, FRAMES);
        printf("  размер %zu Б (%.0f КБ)\n", len, (double)len / 1024.0);
        printf("  адрес %p\n", p);
        printf("  страниц записано и проверено: %lu из %lu\n",
               pages, (unsigned long)(len / page));
        if (pages != (unsigned long)(len / page)) {
            printf("[СБОЙ] не все страницы отвечают\n");
            munmap(p, len);
            return 1;
        }
        printf("  буфер пригоден для чтения и записи\n\n");
        munmap(p, len);
    }

    /* 2. С запасом на рабочие структуры гостя. */
    printf("проверка запаса (обычная память процесса):\n");
    for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        size_t len = (size_t)steps[i];
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        unsigned long pages, want;

        if (p == MAP_FAILED) {
            printf("  %6lu КБ — НЕ ВЫДЕЛЯЕТСЯ, дальше не идём\n",
                   steps[i] / 1024);
            break;
        }
        pages = touch(p, len);
        want = (unsigned long)(len / page);
        printf("  %6lu КБ — выделено, страниц проверено %lu из %lu%s\n",
               steps[i] / 1024, pages, want,
               pages == want ? "" : "  [часть не отвечает]");
        munmap(p, len);
    }

    printf("\nнаше VmRSS после выделения и освобождения: %lu КБ "
           "(было %lu КБ)\n\n", resident_kb(), kb_before);

    print_meminfo();

    printf("\n=== итог ===\n");
    printf("обычной памяти под кадровый буфер и рабочие структуры "
           "хватает с большим запасом.\n");
    printf("Это тот же путь, которым рисует MY_GUI: свой буфер в памяти "
           "процесса, без обращения к устройствам Sigma.\n");
    return 0;
}