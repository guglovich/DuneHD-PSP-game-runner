/*
 * Сборка байтов трамплина импорта и выгрузка их в файл для дизасма.
 *
 * Зачем. Трамплин — это код, который пишется в отображение гостя, и
 * проверить его на плеере иначе нечем: сбой в нём выглядит как `SIGILL`
 * внутри гостя. Единственный оставшийся инструмент — собрать те же байты
 * здесь и прочитать их.
 *
 * ГЛАВНОЕ: построитель здесь — НАСТОЯЩИЙ, из `src/guest_load.c`.
 *
 * Первая версия инструмента копировала сборку трамплина к себе: собственные
 * макросы `I_*`, собственные номера регистров, собственный
 * `GUEST_TRAMP_BYTES`. Дублирование было объявлено «намеренным, чтобы можно
 * было сверить байты, не линкуясь с рантаймом», и из этого вырос дефект:
 * правка рантайма (установка `$k0` в `SceThreadContext`) в копию не попала,
 * а гейт `tools/check_tramp.sh` остался ЗЕЛЁНЫМ — он сверял устаревшую копию
 * и ничего не проверял. Это тот же класс отказа, что и в гейтах релоциций:
 * «при смене формы кода старый тест остаётся зелёным и молчит».
 * Теперь копии нет, а гейт проверяет ровно то, что уедет в гостя.
 *
 * Использование: tramp_dump <выходной_файл>
 */

#include "guest_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

/* Адреса фиксированные, иначе вывод невоспроизводим и гейт не работает:
 * байты `lui`/`ori`/`sw` кодируют сами эти адреса.
 *
 * Всё в одном 256 МБ сегменте — этого требует стаб `j`, который достаёт
 * трамплин без полного 32-битного адреса.
 *
 * Пул трамплинов начинается не в начале отображения, чтобы адрес стаба и адрес
 * трамплина не совпали: иначе сверка не отличила бы «стаб прыгает на трамплин»
 * от «стаб прыгает на себя». */
#define TRAMP_BASE 0x08CA3500u
#define STUB_ADDR  0x08CB0000u
#define MAP_BASE   0x08CA0000u
#define MAP_SIZE   0x20000u

/* Адреса хостской функции, `$gp`, метки и контекста — те, что были на плеере.
 *
 * У всех младшие биты `>= 0x8000` (`fn` -> `0xB3C4`, `gp` -> `0x0010`,
 * `k0` -> `0xBED4`). При `lo < 0x8000` инструкции `ori` и `addiu` дают
 * одинаковый адрес, и гейт был бы слеп к ошибке, из-за которой трамплин звал
 * адрес на `0x10000` выше и ронял гостя с `SIGILL`. Именно этот случай и надо
 * держать — он самый частый в адресах плеера. */
#define HOST_FN   0x2AAAB3C4u
#define HOST_GP   0x2AAD0010u
#define HOST_TRAP 0x2AAAB3C4u
/* Настоящий адрес `SceThreadContext`, который трамплин УСТАНАВЛИВАЕТ в `$k0`.
 * Взят с плеера, а не произвольный: его и должен был получить гость. */
#define HOST_K0   0x08BCBED4u

int main(int argc, char **argv)
{
    GuestModule m;
    GuestStubPatch patch;
    unsigned char *map;
    FILE *f;
    u32 n;

    if (argc != 2) {
        fprintf(stderr, "использование: %s <выходной_файл>\n", argv[0]);
        return 2;
    }

    map = mmap((void *)(uintptr_t)MAP_BASE, MAP_SIZE,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    /* `MAP_FIXED` обязан вернуть именно запрошенный адрес: от него зависит
     * воспроизводимость байтов, а гейт сверяет их с ожидаемыми. */
    if ((u32)(uintptr_t)map != MAP_BASE) {
        fprintf(stderr, "mmap вернул 0x%08X вместо 0x%08X\n",
                (u32)(uintptr_t)map, MAP_BASE);
        return 2;
    }

    memset(&m, 0, sizeof(m));
    m.memory = (void *)(uintptr_t)MAP_BASE;
    m.map_size = MAP_SIZE;
    m.tramp_base = TRAMP_BASE;
    m.tramp_next = TRAMP_BASE;

    patch.target = HOST_FN;
    patch.stub = STUB_ADDR;
    if (!guest_build_import_trampoline(&m, &patch, HOST_FN, HOST_GP,
                                       0, HOST_TRAP, HOST_K0)) {
        fprintf(stderr, "построитель отказал: %s\n", m.error);
        return 2;
    }

    f = fopen(argv[1], "wb");
    if (f == NULL) {
        perror(argv[1]);
        return 2;
    }
    n = 8u + GUEST_TRAMP_BYTES;
    if (fwrite((const void *)(uintptr_t)STUB_ADDR, 1, 8, f) != 8
        || fwrite((const void *)(uintptr_t)patch.trampoline, 1,
                  GUEST_TRAMP_BYTES, f) != GUEST_TRAMP_BYTES) {
        perror("запись");
        fclose(f);
        return 2;
    }
    if (fclose(f) != 0) {
        perror("закрытие");
        return 2;
    }
    printf("записано  : %u байт (8 стаба + %u трамплина) в %s\n",
           n, (unsigned)GUEST_TRAMP_BYTES, argv[1]);
    printf("трамплин  : 0x%08X, стаб: 0x%08X, индекс: %u\n",
           patch.trampoline, patch.stub, patch.stub_index);
    return 0;
}