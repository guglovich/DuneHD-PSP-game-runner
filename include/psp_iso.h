/* Поиск и извлечение EBOOT.BIN из образа ISO 9660.
 *
 * Модуль общий для лаунчера и утилиты `iso_extract_eboot`, чтобы разбор
 * образа не расходился между ними. Логика проверена на реальном образе
 * Magna Carta (ULJS00042).
 *
 * Что важно в формате, из-за чего это отдельный модуль, а не пятнадцать строк:
 *
 *   * поля PVD типа "both-endian" занимают по 8 байт, а не по 4: одно
 *     значение хранится дважды, младшим и старшим байтом первым. Из-за
 *     неверного размера `path_table_size` все последующие поля PVD сдвигаются;
 *   * `root_dir_record` лежит по смещению 156, а не 152;
 *   * обе позиции закреплены `_Static_assert`, иначе ошибка молча даёт
 *     «неверное магическое число» или нулевой LBA корневого каталога;
 *   * у розничных игр EBOOT.BIN лежит в PSP_GAME/SYSDIR, а USRDIR — каталог
 *     для homebrew, поэтому порядок поиска SYSDIR, потом USRDIR.
 */

#ifndef PSP_ISO_H
#define PSP_ISO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PSP_ISO_SECTOR_SIZE 2048

/* Что нашлось в образе. Заполняется psp_iso_find_eboot(). */
typedef struct {
    char     volume_id[33];      /* метка тома из PVD */
    uint32_t psp_game_lba;       /* начало каталога PSP_GAME */
    uint32_t psp_game_size;
    char     eboot_dir[16];      /* в каком каталоге нашлось: SYSDIR или USRDIR */
    uint32_t eboot_lba;
    uint32_t eboot_size;
} PspIsoLayout;

/* Найти EBOOT.BIN в образе. Не читает сам файл, только разбирает дерево.
 *
 * err — необязательный буфер для сообщения об ошибке; если не NULL, должен
 * быть не короче errlen байт. Сообщение пишется на русском, без перевода строки
 * в конце. */
bool psp_iso_find_eboot(const char *iso_path, PspIsoLayout *out,
                        char *err, size_t errlen);

/* Найти и записать EBOOT.BIN в out_path. */
bool psp_iso_extract_eboot(const char *iso_path, const char *out_path,
                           PspIsoLayout *layout_out,
                           char *err, size_t errlen);

/* Только проверка: это ISO игры PSP, а не фильм.
 *
 * Критерий — наличие EBOOT.BIN, то есть проверяется ровно то, что нужно для
 * запуска. Эвристика «есть ли строка PSP_GAME в первых секторах» не годится:
 * она зависит от того, где именно оказались корневые записи каталога. */
bool psp_iso_is_game(const char *iso_path);

#endif /* PSP_ISO_H */