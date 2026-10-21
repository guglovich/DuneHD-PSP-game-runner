/*
 * PSP Launcher — система автозапуска PSP игр на Dune HD Max
 *
 * Интеграция с прошивкой:
 *   1. Плагин регистрирует действие для файлов PSP игр
 *   2. Скрипт запускается при открытии совместимых форматов
 *   3. Система различает ISO от PSP от фильмов
 */

#include "psp_launcher.h"
#include "psp_iso.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <errno.h>

/* ============================================================================
 * Пути к файлам прошивки
 * ==========================================================================*/

#define PLUGIN_DIR          "/persistfs/plugins/psp_launcher"
/* Манифест, который читает shell. Соглашения `action.sh` у плеера нет. */
#define PLUGIN_MANIFEST     "/persistfs/plugins/psp_launcher/dune_plugin.xml"
#define PLUGIN_HOOK         "/persistfs/plugins/psp_launcher/psp_autostart.sh"
/* Имя для <run_string>: путь относительный, WorkingDirectory — каталог плагина. */
#define PLUGIN_HOOK_NAME    "psp_autostart.sh"
#define PSP_HLE_BINARY      "/D/psp_launcher/bin/psp-hle"

/* Загрузчик musl. Обязателен и запускается первым аргументом.
 *
 * psp-hle собран как динамический MIPS-бинарник, его PT_INTERP указывает на
 * /lib/ld-musl-mipsel.so.1, а такого пути на плеере нет — загрузчик лежит
 * рядом с бинарником. Поэтому прямой exec psp-hle не работает никогда:
 * shell сообщает «No such file or directory» на существующий файл. Проверено
 * на устройстве. */
#define PSP_LOADER           "/D/psp_launcher/bin/ld-musl-mipsel.so.1"

/* Рабочие каталоги на плеере: /D/PSP_GAMES не существует, проверено. */
#define PSP_GAMES_DIR       "/D/psp_launcher/games"
#define PSP_EBOOT_DIR       "/D/psp_launcher/games/eboot"

/* Длина буфера под сообщение об ошибке — чтобы не тащить snprintf в каждом
 * вызывающем месте и не получать обрезанный текст. */
#define LAUNCHER_ERR_MAX    200

/* ============================================================================
 * Определение типа файла
 * ==========================================================================*/

PspFileType psp_detect_file_type(const char *path)
{
    FILE *fp;
    unsigned char magic[4];
    size_t n;

    if (!path) {
        return PSP_FILE_UNKNOWN;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        return PSP_FILE_UNKNOWN;
    }

    n = fread(magic, 1, 4, fp);
    fclose(fp);

    if (n < 4) {
        return PSP_FILE_UNKNOWN;
    }

    /* Проверяем магические числа */

    /* EBOOT.BIN бывает двух видов, и розничный — как раз второй.
     *
     * Расшифрованный образ начинается с \x7fELF, а розничный зашифрован
     * схемой ~PSP и начинается с символов "~PSP". Проверка только на ELF
     * означала, что лаунчер не признаёт ни одну розничную игру и отдаёт
     * файл как "неизвестный", то есть не запускает ничего.
     *
     * Именно так выглядит настоящий файл: 7e 50 53 50. */
    if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        return PSP_FILE_EBOOT_BIN;
    }
    if (magic[0] == '~' && magic[1] == 'P' && magic[2] == 'S' && magic[3] == 'P') {
        return PSP_FILE_EBOOT_BIN;
    }

    /* CSO (Compressed ISO) */
    if (magic[0] == 'C' && magic[1] == 'I' && magic[2] == 'S' && magic[3] == 'O') {
        return PSP_FILE_CSO;
    }

    /* PBP (PSP Boot Package) */
    if (magic[0] == 0x00 && magic[1] == 'P' && magic[2] == 'B' && magic[3] == 'P') {
        return PSP_FILE_PBP;
    }

    /* ISO 9660 (проверяем на наличие PSP_GAME) */
    /* ISO 9660 начинается с "CD001" на смещении 0x8001 */
    fp = fopen(path, "rb");
    if (fp) {
        unsigned char iso_magic[5];
        fseek(fp, 0x8001, SEEK_SET);
        n = fread(iso_magic, 1, 5, fp);
        fclose(fp);

        if (n == 5 && memcmp(iso_magic, "CD001", 5) == 0) {
            /* Это ISO, но нужно проверить, игра ли это PSP */
            if (psp_is_game_iso(path)) {
                return PSP_FILE_ISO;
            } else {
                return PSP_FILE_VIDEO_ISO;
            }
        }
    }

    return PSP_FILE_UNKNOWN;
}

/* Проверка, что файл — игра PSP */
bool psp_is_game_file(const char *path)
{
    PspFileType type = psp_detect_file_type(path);
    return (type == PSP_FILE_EBOOT_BIN ||
            type == PSP_FILE_ISO ||
            type == PSP_FILE_CSO ||
            type == PSP_FILE_PBP);
}

/* Проверка, что ISO — игра PSP (а не фильм).
 *
 * Критерий — наличие EBOOT.BIN, то есть ровно то, что нужно для запуска.
 * Прежняя эвристика «есть ли строка PSP_GAME в первых десяти секторах»
 * не годится: она зависит от того, где именно в образе оказались корневые
 * записи каталога, и на части образов даёт ложный ответ в обе стороны. */
bool psp_is_game_iso(const char *iso_path)
{
    char err[LAUNCHER_ERR_MAX];

    if (!psp_iso_is_game(iso_path)) {
        fprintf(stderr, "[LAUNCHER] ISO не содержит EBOOT.BIN: %s\n", iso_path);
        return false;
    }
    (void)err;
    return true;
}

/* Извлечение EBOOT.BIN из образа ISO.
 *
 * Прежде здесь стояло `cp` образа на место EBOOT.BIN: файл копировался
 * целиком, и на его месте оказывался ISO размером в гигабайт, который
 * запустить нельзя. Теперь разбирается дерево ISO 9660 и пишется сам
 * EBOOT.BIN. */
bool psp_extract_eboot(const char *image_path, const char *output_path)
{
    char err[LAUNCHER_ERR_MAX];
    PspIsoLayout layout;

    if (!psp_iso_extract_eboot(image_path, output_path, &layout,
                               err, sizeof(err))) {
        fprintf(stderr, "[LAUNCHER] Извлечение не удалось: %s (%s)\n",
                image_path, err);
        return false;
    }

    fprintf(stderr,
            "[LAUNCHER] Извлечено из %s: %u Б, каталог PSP_GAME/%s, "
            "том \"%s\"\n",
            image_path, layout.eboot_size, layout.eboot_dir, layout.volume_id);
    return true;
}

/* ============================================================================
 * Запуск игр
 * ==========================================================================*/

/* Запуск HLE Runtime на указанном файле.
 *
 * Раньше здесь был `system("... &")`, что плохо по трём причинам сразу:
 *   * `&` отцепляет процесс, и его вывод теряется — а лог рантайма нужен
 *     больше всего при разборе падений;
 *   * путь подставляется в строку команды, поэтому имя файла с пробелом
 *     или кавычкой ломает команду; на плеере имена игр такие и есть;
 *   * через shell проходит лишний процесс, а его код возврата ничего не
 *     говорит о том, запустилась ли игра.
 *
 * Теперь прямой fork/exec: аргументы не требуют экранирования, вывод
 * остаётся привязанным, а результат waitpid говорит, чем закончилось. */
bool psp_launch_eboot(const char *eboot_path)
{
    pid_t pid;

    if (eboot_path == NULL) {
        return false;
    }

    fprintf(stderr, "[LAUNCHER] Запуск: %s %s %s\n", PSP_LOADER,
            PSP_HLE_BINARY, eboot_path);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[LAUNCHER] fork не удался: %s\n", strerror(errno));
        return false;
    }
    if (pid == 0) {
        char *argv[4];
        argv[0] = (char *)PSP_LOADER;
        argv[1] = (char *)PSP_HLE_BINARY;
        argv[2] = (char *)eboot_path;
        argv[3] = NULL;
        execv(PSP_LOADER, argv);
        /* Сюда попадаем только при ошибке exec. */
        fprintf(stderr, "[LAUNCHER] Не удалось запустить %s: %s\n",
                PSP_LOADER, strerror(errno));
        _exit(127);
    }

    return true;
}

bool psp_launch_iso(const char *iso_path)
{
    char eboot_path[1024];

    if (iso_path == NULL) {
        return false;
    }

    if (mkdir(PSP_EBOOT_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[LAUNCHER] Не удалось создать %s: %s\n",
                PSP_EBOOT_DIR, strerror(errno));
        return false;
    }

    snprintf(eboot_path, sizeof(eboot_path), "%s/EBOOT.BIN", PSP_EBOOT_DIR);

    if (!psp_extract_eboot(iso_path, eboot_path))
        return false;

    return psp_launch_eboot(eboot_path);
}

bool psp_launch_game(const char *path)
{
    PspFileType type;

    if (!path) {
        return false;
    }

    type = psp_detect_file_type(path);

    switch (type) {
    case PSP_FILE_EBOOT_BIN:
        return psp_launch_eboot(path);

    case PSP_FILE_ISO:
    case PSP_FILE_CSO:
        return psp_launch_iso(path);

    case PSP_FILE_PBP:
        /* Извлекаем EBOOT.BIN из PBP */
        return false;

    default:
        fprintf(stderr, "[LAUNCHER] Неизвестный тип файла: %s\n", path);
        return false;
    }
}

/* ============================================================================
 * Интеграция с прошивкой
 * ==========================================================================*/

/* ---------------------------------------------------------------------------
 * Установка плагина.
 *
 * Формат выяснен по штатным плагинам плеера, а не придуман:
 *
 *   /persistfs/plugins/<имя>/
 *       dune_plugin.xml      манифест, его читает shell при старте
 *       dune_plugin.sign     подпись (46 Б) и dune_plugin_signer_id.txt
 *       dune_plugin_metadata.xml  необязателен: у duneext его нет
 *       <Имя>.php            класс для плагинов типа php
 *
 * Прежняя версия писала `action.sh` и `plugin.php`. Такого соглашения у
 * плеера нет, и автозапуск не мог сработать никогда. Плеер сообщает об этом
 * прямо в своём журнале:
 *
 *   [populate_plugins] failed to read plugin manifest from
 *   '/persistfs/plugins/psp_launcher/dune_plugin.xml': No such file or directory
 *
 * Тип плагина задаётся наличием одноимённого `.php`: без него — `plain`.
 * Сообщение об успехе выглядит так: `Plugin '<имя>' of type 'plain' is
 * registered`.
 *
 * Хук взят у `duneext`: глобальное действие типа `plugin_system` с
 * `<run_string>`,Relative-путь исполняется из каталога плагина.
 * ------------------------------------------------------------------------- */

bool psp_launcher_install(void)
{
    FILE *fp;

    if (mkdir(PLUGIN_DIR, 0755) != 0 && errno != EEXIST) {
        return false;
    }

    /* Скрипт автозапуска. Сначала он обязан оставить след в журнале: без
     * отметки невозможно отличить «хук не вызвался» от «вызвался и ничего
     * не сделал». */
    fp = fopen(PLUGIN_HOOK, "w");
    if (fp == NULL) {
        fprintf(stderr, "[LAUNCHER] Не создать %s: %s\n", PLUGIN_HOOK,
                strerror(errno));
        return false;
    }
    fprintf(fp, "#!/bin/sh\n");
    fprintf(fp, "# Автозапуск HLE-рантайма. Вызывается из dune_plugin.xml,\n");
    fprintf(fp, "# WorkingDirectory — каталог плагина.\n");
    fprintf(fp, "MARK=/tmp/psp_launcher_hook.log\n");
    fprintf(fp, "echo \"$(date '+%%Y-%%m-%%d %%H:%%M:%%S') вызов, аргменты: $*\" >> \"$MARK\"\n");
    fprintf(fp, "GAME=\"/D/psp_launcher/games/EBOOT_enc.BIN\"\n");
    fprintf(fp, "if [ -f \"$GAME\" ]; then\n");
    /* Загрузчик обязателен: бинарник динамический, его PT_INTERP указывает на
     * /lib/ld-musl-mipsel.so.1, а такого пути на плеере нет. */
    fprintf(fp, "    %s %s \"$GAME\" >> \"$MARK\" 2>&1\n", PSP_LOADER,
            PSP_HLE_BINARY);
    fprintf(fp, "    echo \"код возврата: $?\" >> \"$MARK\"\n");
    fprintf(fp, "fi\n");
    fclose(fp);
    if (chmod(PLUGIN_HOOK, 0755) != 0) {
        return false;
    }

    /* Манифест. Минимальный: имя, подпись и хук на старте. `duneext` доказал,
     * что `global_actions/boot` достаточно, а `entry_points` для проверки
     * хука не требуется. */
    fp = fopen(PLUGIN_MANIFEST, "w");
    if (fp == NULL) {
        fprintf(stderr, "[LAUNCHER] Не создать %s: %s\n", PLUGIN_MANIFEST,
                strerror(errno));
        return false;
    }
    fprintf(fp, "<dune_plugin>\n");
    fprintf(fp, "  <name>psp_launcher</name>\n");
    fprintf(fp, "  <caption>PSP</caption>\n");
    /* Тип обязателен: без него плеер пишет
     *   [populate_plugins] invalid plugin manifest from '...': type is missing
     * В прошивке 20 плагинов объявляют `php` и один `plain`. Нам нужен
     * `plain`: PHP-класс не требуется, хук запускается через <run_string>. */
    fprintf(fp, "  <type>plain</type>\n");
    fprintf(fp, "  <global_actions>\n");
    fprintf(fp, "    <boot>\n");
    fprintf(fp, "      <type>plugin_system</type>\n");
    fprintf(fp, "      <data>\n");
    /* Амперсанд внутри XML экранируется, иначе файл не разбирается. */
    fprintf(fp, "        <run_string>%s boot &amp;</run_string>\n", PLUGIN_HOOK_NAME);
    fprintf(fp, "      </data>\n");
    fprintf(fp, "    </boot>\n");
    fprintf(fp, "  </global_actions>\n");
    fprintf(fp, "</dune_plugin>\n");
    fclose(fp);

    printf("[LAUNCHER] Плагин установлен в %s\n", PLUGIN_DIR);
    printf("[LAUNCHER] Манифест: %s\n", PLUGIN_MANIFEST);
    printf("[LAUNCHER] Хук автозапуска: %s\n", PLUGIN_HOOK);
    printf("[LAUNCHER] Событие проверяется журналом shell: "
           "\"Plugin 'psp_launcher' of type 'plain' is registered\"\n");
    printf("[LAUNCHER] Сработавший хук оставит отметку в /tmp/psp_launcher_hook.log\n");
    return true;
}

bool psp_launcher_uninstall(void)
{
    char cmd[2048];

    snprintf(cmd, sizeof(cmd), "rm -rf %s", PLUGIN_DIR);
    return (system(cmd) == 0);
}

bool psp_launcher_is_installed(void)
{
    struct stat st;
    return (stat(PLUGIN_DIR, &st) == 0 && S_ISDIR(st.st_mode));
}
