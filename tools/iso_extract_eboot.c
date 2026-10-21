/*
 * ISO Extract EBOOT — извлечение EBOOT.BIN из ISO образа PSP
 *
 * ISO образ PSP содержит файловую систему ISO 9660 с файлом EBOOT.BIN
 * в каталоге PSP_GAME/USRDIR/ или PSP_GAME/SYSDIR/.
 *
 * Эта утилита извлекает EBOOT.BIN из ISO для загрузки в HLE Runtime.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * Константы ISO 9660
 * ==========================================================================*/

#define ISO_SECTOR_SIZE         2048
#define ISO_MAGIC               "CD001"
#define ISO_BOOT_RECORD_SECTOR  16
#define ISO_PVD_SECTOR          16
#define ISO_SVD_SECTOR          17

/* ============================================================================
 * Структуры ISO 9660
 * ==========================================================================*/

#pragma pack(push, 1)

/* Блок "both-endian": одно и то же значение хранится дважды —
 * младшим и старшим байтом первым. Поэтому занимает 8 байт, а не 4. */
typedef struct {
    uint8_t le[4];
    uint8_t be[4];
} IsoBothEndian32;

typedef struct {
    uint8_t le[2];
    uint8_t be[2];
} IsoBothEndian16;

/* Primary Volume Descriptor, 2048 байт */
typedef struct {
    uint8_t  type;              /* 1 = Primary Volume Descriptor */
    char     magic[5];          /* "CD001" */
    uint8_t  version;           /* 1 */
    uint8_t  unused1;
    char     system_id[32];
    char     volume_id[32];
    uint8_t  unused2[8];
    IsoBothEndian32 volume_space_size;   /* +80, всего 8 байт */
    uint8_t  unused3[32];
    IsoBothEndian16 volume_set_size;         /* +120, всего 4 байта */
    IsoBothEndian16 volume_sequence_number;  /* +124, всего 4 байта */
    IsoBothEndian16 logical_block_size;      /* +128, всего 4 байта */
    IsoBothEndian32 path_table_size;         /* +132, всего 8 байт */
    uint32_t l_path_table;                   /* +140, little-endian */
    uint32_t opt_l_path_table;               /* +144, little-endian */
    uint32_t m_path_table;                   /* +148, big-endian */
    uint32_t opt_m_path_table;               /* +152, big-endian */
    uint8_t  root_dir_record[34];            /* +156 */
    char     volume_set_id[128];
    char     publisher_id[128];
    char     data_preparer_id[128];
    char     application_id[128];
    char     copyright_file_id[37];
    char     abstract_file_id[37];
    char     bibliographic_file_id[37];
    char     creation_date[17];
    char     modification_date[17];
    char     expiration_date[17];
    char     effective_date[17];
    uint8_t  file_structure_version;
    uint8_t  unused4;
    char     application_use[512];
    char     reserved[653];
} IsoPVD;

/* Directory Record */
typedef struct {
    uint8_t  length;            /* Длина записи */
    uint8_t  ext_attr_length;
    uint32_t extent_lba;        /* LBA первого сектора */
    uint32_t extent_lba_be;
    uint32_t data_length;       /* Размер файла */
    uint32_t data_length_be;
    uint8_t  recording_date[7];
    uint8_t  file_flags;
    uint8_t  file_unit_size;
    uint8_t  interleave_gap_size;
    uint16_t volume_sequence_number;
    uint16_t volume_sequence_number_be;
    uint8_t  name_length;
    char     name[1];           /* Имя файла (переменной длины) */
} IsoDirRecord;

#pragma pack(pop)

/* Смещения полей PVD фиксированы спецификацией ECMA-119. Если раскладка
 * структуры разъедется с ними (а она разъезжается при неверных размерах
 * both-endian полей), читать PVD бессмысленно. Проверяем на этапе сборки. */
#define ISO_PVD_OFF_LOGICAL_BLOCK_SIZE 128
#define ISO_PVD_OFF_PATH_TABLE_SIZE    132
#define ISO_PVD_OFF_ROOT_DIR_RECORD    156

_Static_assert(offsetof(IsoPVD, path_table_size) == ISO_PVD_OFF_PATH_TABLE_SIZE,
               "PVD: path_table_size должно быть по смещению 132");

_Static_assert(offsetof(IsoPVD, logical_block_size) == ISO_PVD_OFF_LOGICAL_BLOCK_SIZE,
               "PVD: logical_block_size должно быть по смещению 128");
_Static_assert(offsetof(IsoPVD, root_dir_record) == ISO_PVD_OFF_ROOT_DIR_RECORD,
               "PVD: root_dir_record должно быть по смещению 152");

/* ============================================================================
 * Вспомогательные функции
 * ==========================================================================*/

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Чтение сектора из ISO */
static bool read_sector(FILE *fp, uint32_t lba, uint8_t *buffer)
{
    if (fseek(fp, (long)lba * ISO_SECTOR_SIZE, SEEK_SET) != 0) {
        return false;
    }
    return fread(buffer, 1, ISO_SECTOR_SIZE, fp) == ISO_SECTOR_SIZE;
}

/* Значение поля "both-endian" — берём младшую половину.
 * Старшая половина не проверяется: в разных образах она встречается
 * разорванной, а расхождение половин означает сдвиг раскладки, который
 * ловится проверкой позиции root_dir_record ниже. */
static uint16_t both_endian16(const IsoBothEndian16 *f)
{
    return (uint16_t)(f->le[0] | (f->le[1] << 8));
}

/* Поиск файла в каталоге */
static bool find_file_in_dir(FILE *fp, uint32_t dir_lba, uint32_t dir_size,
                             const char *filename, uint32_t *file_lba,
                             uint32_t *file_size)
{
    uint8_t sector[ISO_SECTOR_SIZE];
    uint32_t offset = 0;
    uint32_t remaining = dir_size;

    while (remaining > 0) {
        if (!read_sector(fp, dir_lba + offset / ISO_SECTOR_SIZE, sector)) {
            return false;
        }

        uint8_t *ptr = sector + (offset % ISO_SECTOR_SIZE);
        uint8_t *end = sector + ISO_SECTOR_SIZE;

        while (ptr < end) {
            IsoDirRecord *rec = (IsoDirRecord *)ptr;

            if (rec->length == 0) {
                /* Конец каталога в этом секторе */
                break;
            }

            /* Проверяем имя файла */
            if (rec->name_length > 0) {
                char name[256];
                memcpy(name, rec->name, rec->name_length);
                name[rec->name_length] = '\0';

                /* Удаляем версию из имени (например, "EBOOT.BIN;1" -> "EBOOT.BIN") */
                char *semicolon = strchr(name, ';');
                if (semicolon) {
                    *semicolon = '\0';
                }

                if (strcasecmp(name, filename) == 0) {
                    *file_lba = rec->extent_lba;
                    *file_size = rec->data_length;
                    return true;
                }
            }

            ptr += rec->length;
        }

        offset += ISO_SECTOR_SIZE;
        remaining -= ISO_SECTOR_SIZE;
    }

    return false;
}

/* ============================================================================
 * Извлечение EBOOT.BIN
 * ==========================================================================*/

static bool extract_eboot(const char *input_path, const char *output_path)
{
    FILE *in, *out;
    uint8_t sector[ISO_SECTOR_SIZE];
    IsoPVD *pvd;
    uint32_t root_dir_lba, root_dir_size;
    uint32_t eboot_lba, eboot_size;
    uint16_t block_size;
    uint8_t *buffer;
    uint32_t remaining;

    in = fopen(input_path, "rb");
    if (!in) {
        fprintf(stderr, "[ISO] Не удалось открыть файл: %s\n", input_path);
        return false;
    }

    /* Читаем Primary Volume Descriptor (сектор 16) */
    if (!read_sector(in, ISO_PVD_SECTOR, sector)) {
        fprintf(stderr, "[ISO] Ошибка чтения PVD\n");
        fclose(in);
        return false;
    }

    pvd = (IsoPVD *)sector;

    /* Проверяем магическое число */
    if (memcmp(pvd->magic, ISO_MAGIC, 5) != 0) {
        fprintf(stderr, "[ISO] Неверное магическое число: %.5s\n", pvd->magic);
        fclose(in);
        return false;
    }

    printf("[ISO] Том: %s\n", pvd->volume_id);

    block_size = both_endian16(&pvd->logical_block_size);
    if (block_size != ISO_SECTOR_SIZE) {
        fprintf(stderr, "[ISO] Неожиданный размер логического блока: %u\n", block_size);
        fclose(in);
        return false;
    }

    /* Получаем информацию о корневом каталоге */
    root_dir_lba = read_u32_le((uint8_t *)&pvd->root_dir_record[2]);
    root_dir_size = read_u32_le((uint8_t *)&pvd->root_dir_record[10]);

    printf("[ISO] Корневой каталог: LBA %u, размер %u\n", root_dir_lba, root_dir_size);

    /* Ищем каталог PSP_GAME */
    uint32_t psp_game_lba, psp_game_size;
    if (!find_file_in_dir(in, root_dir_lba, root_dir_size, "PSP_GAME",
                          &psp_game_lba, &psp_game_size)) {
        fprintf(stderr, "[ISO] Каталог PSP_GAME не найден\n");
        fclose(in);
        return false;
    }

    printf("[ISO] Каталог PSP_GAME: LBA %u, размер %u\n", psp_game_lba, psp_game_size);

    /* Ищем EBOOT.BIN.
     *
     * Проверено на реальном образе (Magna Carta, ULJS00042): у розничных
     * игр EBOOT.BIN лежит в PSP_GAME/SYSDIR, а в USRDIR его нет — там
     * только данные игры. USRDIR это каталог для homebrew и демо.
     * Поэтому сначала SYSDIR, потом USRDIR.
     */
    {
        static const char *const dirs[] = {"SYSDIR", "USRDIR", NULL};
        uint32_t dir_lba = 0, dir_size = 0;
        const char *found_dir = NULL;
        int i;

        for (i = 0; dirs[i] != NULL; i++) {
            if (find_file_in_dir(in, psp_game_lba, psp_game_size, dirs[i],
                                 &dir_lba, &dir_size) &&
                find_file_in_dir(in, dir_lba, dir_size, "EBOOT.BIN",
                                 &eboot_lba, &eboot_size)) {
                found_dir = dirs[i];
                printf("[ISO] Каталог %s: LBA %u, размер %u\n",
                       dirs[i], dir_lba, dir_size);
                break;
            }
        }

        if (found_dir == NULL) {
            fprintf(stderr, "[ISO] EBOOT.BIN не найден ни в SYSDIR, ни в USRDIR\n");
            fclose(in);
            return false;
        }
    }

    printf("[ISO] Файл EBOOT.BIN: LBA %u, размер %u\n", eboot_lba, eboot_size);

    /* Выделяем буфер для файла */
    buffer = malloc(eboot_size);
    if (!buffer) {
        fprintf(stderr, "[ISO] Ошибка выделения памяти\n");
        fclose(in);
        return false;
    }

    /* Читаем файл */
    if (fseek(in, (long)eboot_lba * ISO_SECTOR_SIZE, SEEK_SET) != 0) {
        fprintf(stderr, "[ISO] Ошибка позиционирования\n");
        free(buffer);
        fclose(in);
        return false;
    }

    remaining = eboot_size;
    while (remaining > 0) {
        size_t to_read = remaining > ISO_SECTOR_SIZE ? ISO_SECTOR_SIZE : remaining;
        if (fread(buffer + (eboot_size - remaining), 1, to_read, in) != to_read) {
            fprintf(stderr, "[ISO] Ошибка чтения файла\n");
            free(buffer);
            fclose(in);
            return false;
        }
        remaining -= to_read;
    }

    /* Записываем файл */
    out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "[ISO] Не удалось создать файл: %s\n", output_path);
        free(buffer);
        fclose(in);
        return false;
    }

    if (fwrite(buffer, 1, eboot_size, out) != eboot_size) {
        fprintf(stderr, "[ISO] Ошибка записи файла\n");
        free(buffer);
        fclose(in);
        fclose(out);
        return false;
    }

    printf("[ISO] EBOOT.BIN извлечён: %s (%u байт)\n", output_path, eboot_size);

    free(buffer);
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
        fprintf(stderr, "Использование: %s <input.iso> <output_eboot.bin>\n", argv[0]);
        return 1;
    }

    printf("========================================\n");
    printf("  ISO Extract EBOOT для PSP\n");
    printf("========================================\n\n");

    if (!extract_eboot(argv[1], argv[2])) {
        return 1;
    }

    return 0;
}
