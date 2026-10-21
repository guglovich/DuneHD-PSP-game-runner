/*
 * PSP Launcher — система автозапуска PSP игр на Dune HD Max
 *
 * Интеграция с прошивкой:
 *   1. Плагин регистрирует действие для файлов PSP игр
 *   2. Скрипт запускается при открытии совместимых форматов
 *   3. Система различает ISO от PSP от фильмов
 *
 * Форматы файлов:
 *   - EBOOT.BIN — расшифрованный ELF-файл PSP
 *   - ISO — образ диска PSP
 *   - CSO — сжатый образ диска PSP
 *   - PBP — контейнер PSP (содержит EBOOT.BIN)
 */

#ifndef PSP_LAUNCHER_H
#define PSP_LAUNCHER_H

#include "psp_hle.h"
#include <stdbool.h>

/* ============================================================================
 * Типы файлов PSP
 * ==========================================================================*/

typedef enum {
    PSP_FILE_UNKNOWN = 0,
    PSP_FILE_EBOOT_BIN,      /* Расшифрованный EBOOT.BIN (ELF) */
    PSP_FILE_ISO,            /* Образ диска PSP */
    PSP_FILE_CSO,            /* Сжатый образ диска PSP */
    PSP_FILE_PBP,            /* Контейнер PSP */
    PSP_FILE_VIDEO_ISO,      /* Фильм ISO (не PSP) */
} PspFileType;

/* ============================================================================
 * Структуры данных
 * ==========================================================================*/

typedef struct {
    PspFileType type;
    char path[1024];
    char eboot_path[1024];   /* Путь к EBOOT.BIN (если есть) */
    bool is_valid;
} PspFileInfo;

/* ============================================================================
 * Функции определения типа файла
 * ==========================================================================*/

/* Определение типа файла PSP */
PspFileType psp_detect_file_type(const char *path);

/* Проверка, что файл — игра PSP */
bool psp_is_game_file(const char *path);

/* Проверка, что ISO — игра PSP (а не фильм) */
bool psp_is_game_iso(const char *iso_path);

/* Извлечение EBOOT.BIN из ISO/CSO */
bool psp_extract_eboot(const char *image_path, const char *output_path);

/* ============================================================================
 * Функции запуска
 * ==========================================================================*/

/* Запуск PSP игры */
bool psp_launch_game(const char *path);

/* Запуск EBOOT.BIN */
bool psp_launch_eboot(const char *eboot_path);

/* Запуск ISO/CSO */
bool psp_launch_iso(const char *iso_path);

/* ============================================================================
 * Интеграция с прошивкой
 * ==========================================================================*/

/* Установка плагина автозапуска */
bool psp_launcher_install(void);

/* Удаление плагина автозапуска */
bool psp_launcher_uninstall(void);

/* Проверка, установлен ли плагин */
bool psp_launcher_is_installed(void);

#endif /* PSP_LAUNCHER_H */
