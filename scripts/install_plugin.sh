#!/bin/bash
# Установка плагина автозапуска PSP игр на Dune HD Max

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_NAME="psp_launcher"
PLUGIN_DIR="/persistfs/plugins/${PLUGIN_NAME}"
PSP_HLE_BIN="${SCRIPT_DIR}/../psp-hle"

echo "========================================"
echo "  Установка PSP Launcher"
echo "========================================"
echo ""

# Проверяем наличие бинарника
if [ ! -f "${PSP_HLE_BIN}" ]; then
    echo "Ошибка: бинарник psp-hle не найден"
    echo "Сначала соберите проект: make"
    exit 1
fi

# Создаём каталог плагина
echo "[1/4] Создание каталога плагина..."
mkdir -p "${PLUGIN_DIR}"

# Копируем бинарник
echo "[2/4] Копирование бинарника..."
cp "${PSP_HLE_BIN}" "${PLUGIN_DIR}/"
chmod +x "${PLUGIN_DIR}/psp-hle"

# Создаём скрипт действия
echo "[3/4] Создание скрипта действия..."
cat > "${PLUGIN_DIR}/action.sh" << 'EOF'
#!/bin/sh
# PSP Launcher — скрипт автозапуска

FILE="$1"

if [ -z "$FILE" ]; then
    exit 1
fi

# Определяем тип файла
case "$FILE" in
    *.BIN|*.bin)
        # EBOOT.BIN
        /persistfs/plugins/psp_launcher/psp-hle "$FILE" &
        ;;
    *.ISO|*.iso)
        # ISO образ
        /persistfs/plugins/psp_launcher/psp-hle "$FILE" &
        ;;
    *.CSO|*.cso)
        # CSO образ
        /persistfs/plugins/psp_launcher/psp-hle "$FILE" &
        ;;
    *.PBP|*.pbp)
        # PBP контейнер
        /persistfs/plugins/psp_launcher/psp-hle "$FILE" &
        ;;
esac
EOF
chmod +x "${PLUGIN_DIR}/action.sh"

# Создаём PHP-плагин
echo "[4/4] Создание PHP-плагина..."
cat > "${PLUGIN_DIR}/plugin.php" << 'EOF'
<?php
// PSP Launcher — плагин автозапуска

class PspLauncher extends UserInputHandler {
    public function __construct() {
        parent::__construct('psp_launcher');
    }

    public function create_action($caption, $handler, $params = []) {
        $action = UserInputHandlerRegistry::create_action($this, $handler);
        $action['caption'] = $caption;
        $action['params'] = $params;
        return $action;
    }

    public function get_actions() {
        $actions = array();

        // Действие для запуска PSP игр
        $actions[] = $this->create_action(
            'Запустить PSP игру',
            'psp_launch_game',
            array('type' => 'file')
        );

        return $actions;
    }
}
EOF

echo ""
echo "========================================"
echo "  Установка завершена!"
echo "========================================"
echo ""
echo "Плагин установлен в: ${PLUGIN_DIR}"
echo ""
echo "Теперь при открытии файлов PSP игр (EBOOT.BIN, ISO, CSO, PBP)"
echo "в файловом менеджере плеера будет запущен HLE Runtime."
echo ""
echo "Для удаления плагина выполните: ./uninstall_plugin.sh"
