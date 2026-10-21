/*
 * PSP HLE Runtime — заголовочный файл
 * Портирование PSP игр на Dune HD Max (Sigma SMP8645, MIPS32 LE)
 *
 * Архитектура:
 *   1. ELF Loader — загрузка расшифрованного EBOOT.BIN (MIPS ELF)
 *   2. Syscall HLE — перехват sce* системных вызовов
 *   3. GFX Translator — трансляция sceGu* в DirectFB
 *
 * Важно: PSP и Dune HD Max — оба MIPS32 LE, поэтому код игр
 * запускается напрямую, без рекомпиляции или трансляции инструкций.
 */

#ifndef PSP_HLE_H
#define PSP_HLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * Базовые типы PSP
 * ==========================================================================*/

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* ============================================================================
 * PSP Memory Map
 * ==========================================================================*/

#define PSP_VRAM_BASE       0x04000000
#define PSP_VRAM_SIZE       (2 * 1024 * 1024)  /* 2 MB */
#define PSP_RAM_BASE        0x08000000
#define PSP_RAM_SIZE        (32 * 1024 * 1024)  /* 32 MB */

/* ============================================================================
 * PSP ELF формат
 * ==========================================================================*/

#define ELF_MAGIC           0x464C457F
#define ELF_CLASS_32        1
#define ELF_DATA_2LSB       1
#define EM_MIPS             8

#define PT_LOAD             1
#define PT_DYNAMIC          2

#define SHT_PROGBITS        1
#define SHT_SYMTAB          2
#define SHT_STRTAB          3
#define SHT_NOBITS          8

/* ============================================================================
 * PSP Syscall константы
 * ==========================================================================*/

/* Kernel */
#define PSP_SYSCALL_KERNEL_CREATE_THREAD     0x00000001
#define PSP_SYSCALL_KERNEL_EXIT_THREAD       0x00000002
#define PSP_SYSCALL_KERNEL_SLEEP_THREAD      0x00000003
#define PSP_SYSCALL_KERNEL_WAKEUP_THREAD     0x00000004
#define PSP_SYSCALL_KERNEL_SUSPEND_THREAD    0x00000005
#define PSP_SYSCALL_KERNEL_RESUME_THREAD     0x00000006

/* Display */
#define PSP_SYSCALL_DISPLAY_SET_MODE        0x00000100
#define PSP_SYSCALL_DISPLAY_SET_FRAMEBUF    0x00000101
#define PSP_SYSCALL_DISPLAY_WAIT_VSYNC      0x00000102

/* Ctrl */
#define PSP_SYSCALL_CTRL_READ_BUFFER        0x00000200
#define PSP_SYSCALL_CTRL_PEEK_BUFFER        0x00000201

/* GE (Graphics Engine) */
#define PSP_SYSCALL_GE_LIST                 0x00000300
#define PSP_SYSCALL_GE_DRAW                 0x00000301
#define PSP_SYSCALL_GE_WAIT                 0x00000302

/* ============================================================================
 * PSP графические константы (sceGu*)
 * ==========================================================================*/

#define PSP_DISPLAY_PIXEL_FORMAT_565    0
#define PSP_DISPLAY_PIXEL_FORMAT_5551   1
#define PSP_DISPLAY_PIXEL_FORMAT_4444   2
#define PSP_DISPLAY_PIXEL_FORMAT_8888   3

#define PSP_DISPLAY_SETBUF_IMMEDIATE    0
#define PSP_DISPLAY_SETBUF_NEXTFRAME    1

#define PSP_PRIMITIVE_POINTS            0
#define PSP_PRIMITIVE_LINES             1
#define PSP_PRIMITIVE_LINE_STRIP        2
#define PSP_PRIMITIVE_TRIANGLES         3
#define PSP_PRIMITIVE_TRIANGLE_STRIP    4
#define PSP_PRIMITIVE_TRIANGLE_FAN      5
#define PSP_PRIMITIVE_SPRITES           6

/* ============================================================================
 * Структуры данных PSP
 * ==========================================================================*/

typedef struct {
    u32 width;
    u32 height;
    u32 pixel_format;
    u32 buffer_format;
} PspDisplayMode;

typedef struct {
    u32 buttons;
    s32 analog_x;
    s32 analog_y;
    u32 timestamp;
} PspCtrlData;

#define PSP_CTRL_SELECT     0x00000001
#define PSP_CTRL_START      0x00000002
#define PSP_CTRL_UP         0x00000004
#define PSP_CTRL_RIGHT      0x00000008
#define PSP_CTRL_DOWN       0x00000010
#define PSP_CTRL_LEFT       0x00000020
#define PSP_CTRL_LTRIGGER   0x00000040
#define PSP_CTRL_RTRIGGER   0x00000080
#define PSP_CTRL_TRIANGLE   0x00000100
#define PSP_CTRL_CIRCLE     0x00000200
#define PSP_CTRL_CROSS      0x00000400
#define PSP_CTRL_SQUARE     0x00000800
#define PSP_CTRL_HOME       0x00001000
#define PSP_CTRL_HOLD       0x00002000
#define PSP_CTRL_NOTE       0x00004000
#define PSP_CTRL_SCREEN     0x00008000
#define PSP_CTRL_VOLUP      0x00010000
#define PSP_CTRL_VOLDOWN    0x00020000

/* ============================================================================
 * Структуры HLE Runtime
 * ==========================================================================*/

typedef struct {
    void *base;
    u32   entry;
    u32   size;
    char *name;
} PspElf;

typedef struct {
    PspElf elf;
    void *stack;
    u32   stack_size;
    bool  running;
    int   exit_code;
} PspHleContext;

/* ============================================================================
 * Прототипы функций
 * ==========================================================================*/

/* ELF Loader */
bool psp_elf_load(const char *path, PspElf *elf);
void psp_elf_unload(PspElf *elf);

/* Syscall HLE */
void psp_syscall_init(void);
void psp_syscall_execute(u32 syscall_num, u32 *args, u32 *ret);
void psp_syscall_shutdown(void);

/* GFX Translator */
void psp_gfx_init(void);
void psp_gfx_set_mode(u32 width, u32 height, u32 format);
void psp_gfx_set_framebuf(void *addr, u32 width, u32 height, u32 format);
void psp_gfx_wait_vsync(void);
void psp_gfx_draw_arrays(u32 primitive, u32 count, void *vertices, void *texcoords);
void psp_gfx_shutdown(void);

/* Runtime */
bool psp_hle_init(const char *eboot_path);
void psp_hle_run(void);
void psp_hle_shutdown(void);

#endif /* PSP_HLE_H */
