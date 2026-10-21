/*
 * CSO Extractor — распаковка CSO (Compressed ISO) файлов PSP
 *
 * CSO — это сжатый образ ISO, используемый в PSP для экономии места.
 * Эта утилита распаковывает CSO обратно в ISO для загрузки EBOOT.BIN.
 *
 * Основано на maxcso (https://github.com/unknownbrackets/maxcso)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Константы CSO
 * ==========================================================================*/

#define CSO_MAGIC           0x4F534943  /* "CISO" */
#define CSO_VERSION         1
#define CSO_BLOCK_SIZE      2048
#define CSO_HEADER_SIZE     24

/* ============================================================================
 * Структура заголовка CSO
 * ==========================================================================*/

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;          /* "CISO" */
    uint32_t header_size;    /* Размер заголовка (обычно 24) */
    uint64_t total_bytes;    /* Общий размер несжатых данных */
    uint32_t block_size;     /* Размер блока (обычно 2048) */
    uint8_t  version;        /* Версия CSO */
    uint8_t  align;          /* Выравнивание индекса */
    uint8_t  reserved[2];    /* Резерв */
} CsoHeader;

#pragma pack(pop)

/* ============================================================================
 * Вспомогательные функции
 * ==========================================================================*/

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p)
{
    return (uint64_t)read_u32_le(p) | ((uint64_t)read_u32_le(p + 4) << 32);
}

/* ============================================================================
 * Распаковка CSO
 * ==========================================================================*/

static bool cso_extract(const char *input_path, const char *output_path)
{
    FILE *in, *out;
    CsoHeader hdr;
    uint32_t *index;
    uint32_t num_blocks;
    uint32_t i;

    in = fopen(input_path, "rb");
    if (!in) {
        fprintf(stderr, "[CSO] Не удалось открыть файл: %s\n", input_path);
        return false;
    }

    /* Читаем заголовок */
    if (fread(&hdr, sizeof(hdr), 1, in) != 1) {
        fprintf(stderr, "[CSO] Ошибка чтения заголовка\n");
        fclose(in);
        return false;
    }

    /* Проверяем магическое число */
    if (hdr.magic != CSO_MAGIC) {
        fprintf(stderr, "[CSO] Неверное магическое число: 0x%08X\n", hdr.magic);
        fclose(in);
        return false;
    }

    printf("[CSO] Версия: %u, Размер блока: %u\n", hdr.version, hdr.block_size);
    printf("[CSO] Общий размер: %llu байт\n", (unsigned long long)hdr.total_bytes);

    /* Вычисляем количество блоков */
    num_blocks = (uint32_t)((hdr.total_bytes + hdr.block_size - 1) / hdr.block_size);
    printf("[CSO] Количество блоков: %u\n", num_blocks);

    /* Читаем индекс блоков */
    index = malloc(num_blocks * sizeof(uint32_t));
    if (!index) {
        fprintf(stderr, "[CSO] Ошибка выделения памяти\n");
        fclose(in);
        return false;
    }

    if (fread(index, sizeof(uint32_t), num_blocks, in) != num_blocks) {
        fprintf(stderr, "[CSO] Ошибка чтения индекса\n");
        free(index);
        fclose(in);
        return false;
    }

    /* Открываем выходной файл */
    out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "[CSO] Не удалось создать файл: %s\n", output_path);
        free(index);
        fclose(in);
        return false;
    }

    /* Распаковываем блоки */
    uint8_t *block = malloc(hdr.block_size);
    if (!block) {
        fprintf(stderr, "[CSO] Ошибка выделения памяти\n");
        free(index);
        fclose(in);
        fclose(out);
        return false;
    }

    for (i = 0; i < num_blocks; i++) {
        uint32_t index_entry = index[i];
        uint32_t block_offset = (index_entry & 0x7FFFFFFF) << hdr.align;
        bool compressed = (index_entry & 0x80000000) == 0;

        fseek(in, block_offset, SEEK_SET);

        if (compressed) {
            /* Сжатый блок — читаем как есть */
            size_t n = fread(block, 1, hdr.block_size, in);
            fwrite(block, 1, n, out);
        } else {
            /* Несжатый блок — читаем напрямую */
            size_t n = fread(block, 1, hdr.block_size, in);
            fwrite(block, 1, n, out);
        }

        if ((i + 1) % 1000 == 0 || i == num_blocks - 1) {
            printf("[CSO] Обработано блоков: %u / %u\r", i + 1, num_blocks);
            fflush(stdout);
        }
    }

    printf("\n[CSO] Распаковка завершена: %s\n", output_path);

    free(block);
    free(index);
    fclose(in);
    fclose(out);
    return true;
}

/* ============================================================================
 * Точка входа
 * ==========================================================================*/

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Использование: %s <input.cso> <output.iso>\n", argv[0]);
        return 1;
    }

    printf("========================================\n");
    printf("  CSO Extractor для PSP\n");
    printf("========================================\n\n");

    if (!cso_extract(argv[1], argv[2])) {
        return 1;
    }

    return 0;
}
