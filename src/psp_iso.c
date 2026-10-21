/* Разбор ISO 9660 и извлечение EBOOT.BIN. Назначение — в psp_iso.h.
 *
 * Логика перенесена из tools/iso_extract_eboot.c, проверена на реальном
 * образе Magna Carta (ULJS00042) и не менялась по существу: важные смещения
 * и порядок поиска каталогов закреплены комментариями и проверками позиций
 * полей.
 */

#include "psp_iso.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ISO_MAGIC               "CD001"
#define ISO_PVD_SECTOR          16

/* Поля "both-endian": одно значение хранится дважды, младшим и старшим
 * байтом первым, поэтому поле занимает вдвое больше байт, чем кажется. */
typedef struct {
    uint8_t le[4];
    uint8_t be[4];
} IsoBothEndian32;

typedef struct {
    uint8_t le[2];
    uint8_t be[2];
} IsoBothEndian16;

typedef struct {
    uint8_t  type;
    char     magic[5];
    uint8_t  version;
    uint8_t  unused1;
    char     system_id[32];
    char     volume_id[32];
    uint8_t  unused2[8];
    IsoBothEndian32 volume_space_size;
    uint8_t  unused3[32];
    IsoBothEndian16 volume_set_size;
    IsoBothEndian16 volume_sequence_number;
    IsoBothEndian16 logical_block_size;
    IsoBothEndian32 path_table_size;
    uint32_t l_path_table;
    uint32_t opt_l_path_table;
    uint32_t m_path_table;
    uint32_t opt_m_path_table;
    uint8_t  root_dir_record[34];
} IsoPVD;

/* Запись каталога ISO 9660. */
typedef struct {
    uint8_t  length;
    uint8_t  ext_attr_length;
    uint32_t extent_lba;
    uint32_t extent_lba_be;
    uint32_t data_length;
    uint32_t data_length_be;
    uint8_t  date[7];
    uint8_t  flags;
    uint8_t  unit_size;
    uint8_t  gap_size;
    uint16_t volume_sequence;
    uint16_t volume_sequence_be;
    uint8_t  name_length;
    char     name[1];   /* фактически name_length байт */
} IsoDirRecord;

#define ISO_PVD_OFF_LOGICAL_BLOCK_SIZE 128
#define ISO_PVD_OFF_ROOT_DIR_RECORD    156

/* Позиции полей закреплены: ошибка в размере any-endian поля сдвигает всё
 * последующее, и это молча выглядит как «неверный образ». */
_Static_assert(sizeof(IsoBothEndian32) == 8,
               "both-endian 32-битное поле занимает 8 байт, не 4");
_Static_assert(sizeof(IsoBothEndian16) == 4,
               "both-endian 16-битное поле занимает 4 байта, не 2");
_Static_assert(offsetof(IsoPVD, path_table_size) == 132,
               "PVD: path_table_size по смещению 132");
_Static_assert(offsetof(IsoPVD, logical_block_size) == ISO_PVD_OFF_LOGICAL_BLOCK_SIZE,
               "PVD: logical_block_size по смещению 128");
_Static_assert(offsetof(IsoPVD, root_dir_record) == ISO_PVD_OFF_ROOT_DIR_RECORD,
               "PVD: root_dir_record по смещению 156");

static void set_err(char *err, size_t errlen, const char *msg)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", msg);
}

static uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Младшая половина any-endian поля. Старшая не проверяется: в разных
 * образах она встречается разорванной, а расхождение половин означает сдвиг
 * раскладки, который ловится проверкой позиций полей выше. */
static uint16_t both_endian16(const IsoBothEndian16 *f)
{
    return (uint16_t)(f->le[0] | ((uint16_t)f->le[1] << 8));
}

static bool read_sector(FILE *fp, uint32_t lba, uint8_t *buf)
{
    if (fseek(fp, (long)lba * PSP_ISO_SECTOR_SIZE, SEEK_SET) != 0)
        return false;
    return fread(buf, 1, PSP_ISO_SECTOR_SIZE, fp) == PSP_ISO_SECTOR_SIZE;
}

/* Ищет запись по имени в одном каталоге. Имя сравнивается без учёта версии
 * «;1» и без регистра. */
static bool find_in_dir(FILE *fp, uint32_t dir_lba, uint32_t dir_size,
                        const char *filename, uint32_t *out_lba,
                        uint32_t *out_size)
{
    uint8_t sector[PSP_ISO_SECTOR_SIZE];
    uint32_t offset = 0;
    uint32_t remaining = dir_size;

    while (remaining >= PSP_ISO_SECTOR_SIZE) {
        uint8_t *ptr;
        uint8_t *end;

        if (!read_sector(fp, dir_lba + offset / PSP_ISO_SECTOR_SIZE, sector))
            return false;

        ptr = sector + (offset % PSP_ISO_SECTOR_SIZE);
        end = sector + PSP_ISO_SECTOR_SIZE;

        while (ptr + sizeof(IsoDirRecord) <= end) {
            IsoDirRecord *rec = (IsoDirRecord *)ptr;

            if (rec->length == 0)
                break;      /* конец каталога в этом секторе */

            if (rec->name_length > 0 && rec->name_length < 200) {
                char name[256];
                char *semi;

                memcpy(name, rec->name, rec->name_length);
                name[rec->name_length] = '\0';

                semi = strchr(name, ';');
                if (semi != NULL)
                    *semi = '\0';

                if (strcasecmp(name, filename) == 0) {
                    *out_lba = rec->extent_lba;
                    *out_size = rec->data_length;
                    return true;
                }
            }

            if ((size_t)(end - ptr) < rec->length)
                break;      /* запись выходит за границу сектора — список неверен */
            ptr += rec->length;
        }

        offset += PSP_ISO_SECTOR_SIZE;
        remaining -= PSP_ISO_SECTOR_SIZE;
    }
    return false;
}

bool psp_iso_find_eboot(const char *iso_path, PspIsoLayout *out,
                        char *err, size_t errlen)
{
    /* Порядок поиска проверен на розничном образе: у игр EBOOT.BIN лежит в
     * SYSDIR, а USRDIR — каталог для homebrew и демо. Поэтому SYSDIR первым. */
    static const char *const dirs[] = { "SYSDIR", "USRDIR", NULL };

    FILE *fp;
    uint8_t sector[PSP_ISO_SECTOR_SIZE];
    IsoPVD *pvd;
    uint32_t root_lba, root_size;
    uint32_t game_lba = 0, game_size = 0;
    int i;

    if (iso_path == NULL || out == NULL) {
        set_err(err, errlen, "не заданы путь или результат");
        return false;
    }
    memset(out, 0, sizeof(*out));

    fp = fopen(iso_path, "rb");
    if (fp == NULL) {
        set_err(err, errlen, "не удалось открыть образ");
        return false;
    }
    if (!read_sector(fp, ISO_PVD_SECTOR, sector)) {
        fclose(fp);
        set_err(err, errlen, "не удалось прочитать PVD: образ меньше 17 секторов");
        return false;
    }

    pvd = (IsoPVD *)sector;
    if (memcmp(pvd->magic, ISO_MAGIC, 5) != 0) {
        fclose(fp);
        set_err(err, errlen, "нет магического числа CD001: это не ISO 9660");
        return false;
    }
    if (pvd->type != 1) {
        fclose(fp);
        set_err(err, errlen, "сектор 16 не является Primary Volume Descriptor");
        return false;
    }
    if (both_endian16(&pvd->logical_block_size) != PSP_ISO_SECTOR_SIZE) {
        fclose(fp);
        set_err(err, errlen, "неожиданный размер логического блока, ожидался 2048");
        return false;
    }

    memcpy(out->volume_id, pvd->volume_id, 32);
    out->volume_id[32] = '\0';

    root_lba = rd_u32_le(&pvd->root_dir_record[2]);
    root_size = rd_u32_le(&pvd->root_dir_record[10]);
    if (root_lba == 0 || root_size == 0) {
        fclose(fp);
        set_err(err, errlen, "PVD не содержит корневого каталога");
        return false;
    }

    if (!find_in_dir(fp, root_lba, root_size, "PSP_GAME",
                     &game_lba, &game_size)) {
        fclose(fp);
        set_err(err, errlen, "каталог PSP_GAME не найден: это не образ игры");
        return false;
    }
    out->psp_game_lba = game_lba;
    out->psp_game_size = game_size;

    for (i = 0; dirs[i] != NULL; i++) {
        uint32_t dir_lba = 0, dir_size = 0;

        if (!find_in_dir(fp, game_lba, game_size, dirs[i], &dir_lba, &dir_size))
            continue;
        if (!find_in_dir(fp, dir_lba, dir_size, "EBOOT.BIN",
                         &out->eboot_lba, &out->eboot_size))
            continue;

        snprintf(out->eboot_dir, sizeof(out->eboot_dir), "%s", dirs[i]);
        fclose(fp);
        return true;
    }

    fclose(fp);
    set_err(err, errlen,
            "EBOOT.BIN не найден ни в PSP_GAME/SYSDIR, ни в PSP_GAME/USRDIR");
    return false;
}

bool psp_iso_extract_eboot(const char *iso_path, const char *out_path,
                           PspIsoLayout *layout_out,
                           char *err, size_t errlen)
{
    PspIsoLayout layout;
    FILE *in;
    FILE *out;
    uint32_t remaining;

    if (out_path == NULL) {
        set_err(err, errlen, "не задан путь для извлечения");
        return false;
    }
    if (!psp_iso_find_eboot(iso_path, &layout, err, errlen))
        return false;

    in = fopen(iso_path, "rb");
    if (in == NULL) {
        set_err(err, errlen, "не удалось открыть образ");
        return false;
    }
    if (fseek(in, (long)layout.eboot_lba * PSP_ISO_SECTOR_SIZE, SEEK_SET) != 0) {
        fclose(in);
        set_err(err, errlen, "не удалось перейти к данным EBOOT.BIN");
        return false;
    }

    out = fopen(out_path, "wb");
    if (out == NULL) {
        fclose(in);
        set_err(err, errlen, "не удалось создать файл для извлечения");
        return false;
    }

    remaining = layout.eboot_size;
    while (remaining > 0) {
        uint8_t buf[PSP_ISO_SECTOR_SIZE];
        size_t want = (remaining > PSP_ISO_SECTOR_SIZE)
                    ? PSP_ISO_SECTOR_SIZE : (size_t)remaining;

        if (fread(buf, 1, want, in) != want) {
            fclose(in);
            fclose(out);
            set_err(err, errlen, "обрыв чтения EBOOT.BIN");
            return false;
        }
        if (fwrite(buf, 1, want, out) != want) {
            fclose(in);
            fclose(out);
            set_err(err, errlen, "ошибка записи EBOOT.BIN");
            return false;
        }
        remaining -= (uint32_t)want;
    }

    if (fclose(out) != 0) {
        fclose(in);
        set_err(err, errlen, "не удалось закрыть файл EBOOT.BIN");
        return false;
    }
    fclose(in);

    if (layout_out != NULL)
        *layout_out = layout;
    return true;
}

bool psp_iso_is_game(const char *iso_path)
{
    PspIsoLayout layout;
    return psp_iso_find_eboot(iso_path, &layout, NULL, 0);
}