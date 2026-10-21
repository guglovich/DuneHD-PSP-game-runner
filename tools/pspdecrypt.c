/*
 * pspdecrypt — расшифровка розничного PSP EBOOT.BIN (формат ~PSP).
 *
 * Инструмент существует для ручной проверки: расшифровка, заголовок,
 * контрольная сумма. Сам разбор формата вынесен в src/psp_decrypt.c,
 * потому что то же самое делает рантайм при автозапуске, и два независимых
 * экземпляра кода разойдутся.
 *
 * Использование: pspdecrypt <EBOOT.BIN> [вывод]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp_decrypt.h"

int main(int argc, char **argv)
{
    FILE *f;
    u8 *data;
    u8 *plain;
    long size;
    const char *why = NULL;
    size_t plain_size = 0;
    const char *out_path;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Использование: %s <EBOOT.BIN> [вывод]\n", argv[0]);
        return 2;
    }

    f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "не открыть: %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        fprintf(stderr, "пустой файл\n");
        return 1;
    }

    data = malloc((size_t)size);
    if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        fprintf(stderr, "ошибка чтения\n");
        return 1;
    }
    fclose(f);

    if (memcmp(data, "~PSP", 4) != 0) {
        if (memcmp(data, "\x7f" "ELF", 4) == 0) {
            printf("Файл уже является ELF, расшифровка не нужна.\n");
            printf("Размер: %ld байт\n", size);
            free(data);
            return 0;
        }
        fprintf(stderr, "не ~PSP и не ELF: %.4s\n", data);
        free(data);
        return 1;
    }

    psp_decrypt_dump_header(data, (size_t)size);

    printf("=== расшифровка ===\n");
    plain = psp_decrypt_eboot(data, (size_t)size, &plain_size, &why);
    if (plain == NULL) {
        fprintf(stderr, "НЕ УДАЛОСЬ: %s\n", why);
        free(data);
        return 1;
    }

    /* Гейт: корректный результат обязан начинаться с магии ELF. */
    if (plain_size < 4 || memcmp(plain, "\x7f" "ELF", 4) != 0) {
        fprintf(stderr, "НЕ УДАЛОСЬ: результат не начинается с магии ELF\n");
        fprintf(stderr, "первые байты: %02x %02x %02x %02x\n",
                plain[0], plain[1], plain[2], plain[3]);
        free(plain);
        free(data);
        return 1;
    }

    printf("OK: контрольная сумма сошлась, магия ELF на месте\n");
    printf("   размер результата: %zu\n", plain_size);
    printf("   первые байты: %02x %02x %02x %02x %02x %02x\n",
           plain[0], plain[1], plain[2], plain[3], plain[4], plain[5]);

    out_path = (argc == 3) ? argv[2] : NULL;
    if (out_path != NULL) {
        FILE *o = fopen(out_path, "wb");
        if (o == NULL) {
            fprintf(stderr, "не создать: %s\n", out_path);
        } else {
            fwrite(plain, 1, plain_size, o);
            fclose(o);
            printf("   записано: %s\n", out_path);
        }
    }

    free(plain);
    free(data);
    return 0;
}