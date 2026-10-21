/*
 * Syscall HLE — перехват системных вызовов PSP
 *
 * Вызовы ОС PSP (sceKernelCreateThread, sceDisplaySetMode, sceCtrlReadBufferPositive)
 * подменяются нативными функциями Linux-стека:
 *   - POSIX threads (pthread_create, pthread_exit)
 *   - DirectFB (для дисплея)
 *   - Linux input (/dev/input/event* для контроллера)
 */

#include "psp_hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/time.h>

/* ============================================================================
 * Состояние контроллера
 * ==========================================================================*/

static int g_input_fd = -1;
static PspCtrlData g_ctrl_data = {0};
static pthread_mutex_t g_ctrl_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Кнопки Linux input -> PSP Ctrl */
static const struct {
    int linux_key;
    u32 psp_btn;
} keymap[] = {
    {KEY_ESC,       PSP_CTRL_START},
    {KEY_ENTER,     PSP_CTRL_CROSS},
    {KEY_SPACE,     PSP_CTRL_CIRCLE},
    {KEY_TAB,       PSP_CTRL_TRIANGLE},
    {KEY_BACKSPACE, PSP_CTRL_SQUARE},
    {KEY_UP,        PSP_CTRL_UP},
    {KEY_DOWN,      PSP_CTRL_DOWN},
    {KEY_LEFT,      PSP_CTRL_LEFT},
    {KEY_RIGHT,     PSP_CTRL_RIGHT},
    {KEY_PAGEUP,    PSP_CTRL_LTRIGGER},
    {KEY_PAGEDOWN,  PSP_CTRL_RTRIGGER},
    {KEY_HOME,      PSP_CTRL_HOME},
    {KEY_END,       PSP_CTRL_SELECT},
    {KEY_LEFTALT,   PSP_CTRL_HOLD},
    {KEY_F1,        PSP_CTRL_NOTE},
    {KEY_F2,        PSP_CTRL_SCREEN},
    {KEY_F3,        PSP_CTRL_VOLUP},
    {KEY_F4,        PSP_CTRL_VOLDOWN},
};

#define KEYMAP_SIZE (sizeof(keymap) / sizeof(keymap[0]))

/* ============================================================================
 * Состояние дисплея
 * ==========================================================================*/

static PspDisplayMode g_display_mode = {
    .width = 480,
    .height = 272,
    .pixel_format = PSP_DISPLAY_PIXEL_FORMAT_8888,
    .buffer_format = PSP_DISPLAY_SETBUF_NEXTFRAME
};

static void *g_framebuffer = NULL;
static u32 g_framebuffer_size = 0;

/* ============================================================================
 * Внутренние функции
 * ==========================================================================*/

/* Открытие устройства ввода */
static bool open_input_device(void)
{
    const char *devices[] = {
        "/dev/input/event0",
        "/dev/input/event1",
        "/dev/input/event2",
        NULL
    };
    int i;

    for (i = 0; devices[i]; i++) {
        g_input_fd = open(devices[i], O_RDONLY | O_NONBLOCK);
        if (g_input_fd >= 0) {
            printf("[HLE] Устройство ввода открыто: %s\n", devices[i]);
            return true;
        }
    }

    fprintf(stderr, "[HLE] Не удалось открыть устройство ввода\n");
    return false;
}

/* Поток обработки событий ввода */
static void *input_thread(void *arg)
{
    struct input_event ev;
    int i;

    (void)arg;

    while (g_input_fd >= 0) {
        ssize_t n = read(g_input_fd, &ev, sizeof(ev));
        if (n == sizeof(ev)) {
            if (ev.type == EV_KEY) {
                pthread_mutex_lock(&g_ctrl_mutex);
                for (i = 0; i < KEYMAP_SIZE; i++) {
                    if (keymap[i].linux_key == ev.code) {
                        if (ev.value == 1) {
                            g_ctrl_data.buttons |= keymap[i].psp_btn;
                        } else if (ev.value == 0) {
                            g_ctrl_data.buttons &= ~keymap[i].psp_btn;
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&g_ctrl_mutex);
            }
        } else {
            usleep(1000);  /* 1 ms */
        }
    }

    return NULL;
}

/* ============================================================================
 * Инициализация HLE
 * ==========================================================================*/

static pthread_t g_input_tid;
static bool g_initialized = false;

void psp_syscall_init(void)
{
    if (g_initialized) {
        return;
    }

    printf("[HLE] Инициализация syscall HLE...\n");

    /* Открываем устройство ввода */
    if (!open_input_device()) {
        fprintf(stderr, "[HLE] Предупреждение: ввод недоступен\n");
    }

    /* Запускаем поток обработки ввода */
    if (g_input_fd >= 0) {
        pthread_create(&g_input_tid, NULL, input_thread, NULL);
    }

    g_initialized = true;
    printf("[HLE] Syscall HLE инициализирован\n");
}

/* ============================================================================
 * Обработка системных вызовов
 * ==========================================================================*/

void psp_syscall_execute(u32 syscall_num, u32 *args, u32 *ret)
{
    if (!ret) {
        return;
    }

    *ret = 0;

    switch (syscall_num) {
    /* === Kernel: потоки === */
    case PSP_SYSCALL_KERNEL_CREATE_THREAD:
        /* args: (name, entry, arg, stack_size, priority) */
        {
            pthread_t tid;
            pthread_create(&tid, NULL, (void *(*)(void *))args[1], (void *)args[2]);
            *ret = (u32)tid;
            printf("[HLE] sceKernelCreateThread: entry=%p, tid=%p\n",
                   (void *)args[1], (void *)tid);
        }
        break;

    case PSP_SYSCALL_KERNEL_EXIT_THREAD:
        pthread_exit((void *)args[0]);
        break;

    case PSP_SYSCALL_KERNEL_SLEEP_THREAD:
        usleep(args[0] * 1000);
        break;

    case PSP_SYSCALL_KERNEL_WAKEUP_THREAD:
        /* В реальности нужна условная переменная, но для HLE просто возвращаем */
        break;

    case PSP_SYSCALL_KERNEL_SUSPEND_THREAD:
        break;

    case PSP_SYSCALL_KERNEL_RESUME_THREAD:
        break;

    /* === Display === */
    case PSP_SYSCALL_DISPLAY_SET_MODE:
        /* args: (width, height, format, buffer_format) */
        g_display_mode.width = args[0];
        g_display_mode.height = args[1];
        g_display_mode.pixel_format = args[2];
        g_display_mode.buffer_format = args[3];
        printf("[HLE] sceDisplaySetMode: %ux%u, format=%u\n",
               g_display_mode.width, g_display_mode.height,
               g_display_mode.pixel_format);
        break;

    case PSP_SYSCALL_DISPLAY_SET_FRAMEBUF:
        /* args: (addr, width, height, format) */
        g_framebuffer = (void *)args[0];
        g_display_mode.width = args[1];
        g_display_mode.height = args[2];
        g_display_mode.pixel_format = args[3];
        g_framebuffer_size = g_display_mode.width * g_display_mode.height * 4;
        printf("[HLE] sceDisplaySetFrameBuf: addr=%p, %ux%u\n",
               g_framebuffer, g_display_mode.width, g_display_mode.height);
        break;

    case PSP_SYSCALL_DISPLAY_WAIT_VSYNC:
        /* В реальности нужно ждать vsync, но для HLE просто возвращаем */
        usleep(16666);  /* ~60 Hz */
        break;

    /* === Ctrl === */
    case PSP_SYSCALL_CTRL_READ_BUFFER:
        /* args: (data, count) */
        pthread_mutex_lock(&g_ctrl_mutex);
        memcpy((void *)args[0], &g_ctrl_data, sizeof(PspCtrlData));
        pthread_mutex_unlock(&g_ctrl_mutex);
        break;

    case PSP_SYSCALL_CTRL_PEEK_BUFFER:
        /* args: (data, count) */
        pthread_mutex_lock(&g_ctrl_mutex);
        memcpy((void *)args[0], &g_ctrl_data, sizeof(PspCtrlData));
        pthread_mutex_unlock(&g_ctrl_mutex);
        break;

    /* === GE (Graphics Engine) === */
    case PSP_SYSCALL_GE_LIST:
        /* Обработка display list — будет в gfx_translator.c */
        break;

    case PSP_SYSCALL_GE_DRAW:
        /* Запуск отрисовки */
        break;

    case PSP_SYSCALL_GE_WAIT:
        /* Ожидание завершения */
        break;

    default:
        printf("[HLE] Неизвестный syscall: 0x%08X\n", syscall_num);
        *ret = -1;
        break;
    }
}

/* ============================================================================
 * Очистка
 * ==========================================================================*/

void psp_syscall_shutdown(void)
{
    if (!g_initialized) {
        return;
    }

    if (g_input_fd >= 0) {
        close(g_input_fd);
        g_input_fd = -1;
    }

    pthread_join(g_input_tid, NULL);
    pthread_mutex_destroy(&g_ctrl_mutex);

    g_initialized = false;
    printf("[HLE] Syscall HLE остановлен\n");
}
