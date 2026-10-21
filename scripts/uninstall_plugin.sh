#!/bin/bash
# Удаление плагина автозапуска PSP игр с Dune HD Max

set -e

PLUGIN_NAME="psp_launcher"
PLUGIN_DIR="/persistfs/plugins/${PLUGIN_NAME}"

echo "========================================"
echo "  Удаление PSP Launcher"
echo "========================================"
echo ""

# Проверяем наличие плагина
if [ ! -d "${PLUGIN_DIR}" ]; then
    echo "Плагин не установлен"
    exit 0
fi

# Удаляем каталог плагина
echo "[1/1] Удаление плагина..."
rm -rf "${PLUGIN_DIR}"

echo ""
echo "========================================"
echo "  Удаление завершено!"
echo "========================================"
echo ""
echo "Плагин удалён: ${PLUGIN_DIR}"
