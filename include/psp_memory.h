/*
 * PSP Memory — виртуальная память PSP
 *
 * Основана на подходе PSPRecomp:
 *   - VRAM (2 MB) по адресу 0x04000000
 *   - RAM (32/64 MB) по адресу 0x08000000
 *   - Канонизация адресов (cached/uncached aliases)
 *   - Fast paths для горячих путей
 */

#ifndef PSP_MEMORY_H
#define PSP_MEMORY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ============================================================================
 * Константы памяти PSP
 * ==========================================================================*/

#define PSP_VRAM_PHYSICAL_BASE      0x04000000
#define PSP_VRAM_SIZE               (2 * 1024 * 1024)  /* 2 MB */
#define PSP_VRAM_MIRROR_COUNT       4
#define PSP_VRAM_ADDRESS_SPAN       (PSP_VRAM_SIZE * PSP_VRAM_MIRROR_COUNT)

#define PSP_PHYSICAL_BASE           0x08000000
#define PSP_RAM_SIZE                (32 * 1024 * 1024)  /* 32 MB */

/* ============================================================================
 * Канонизация адресов
 * ==========================================================================*/

static inline uint32_t psp_address_canonical(uint32_t address)
{
    return address & 0x1FFFFFFF;
}

/* ============================================================================
 * GuestMemory — виртуальная память PSP
 * ==========================================================================*/

typedef struct {
    uint8_t *vram;           /* VRAM (2 MB) */
    uint8_t *ram;            /* RAM (32 MB) */
    uint32_t ram_size;       /* Размер RAM */
} PspGuestMemory;

/* Инициализация GuestMemory */
static inline void psp_memory_init(PspGuestMemory *mem)
{
    mem->vram = calloc(1, PSP_VRAM_SIZE);
    mem->ram_size = PSP_RAM_SIZE;
    mem->ram = calloc(1, mem->ram_size);
}

/* Очистка GuestMemory */
static inline void psp_memory_shutdown(PspGuestMemory *mem)
{
    free(mem->vram);
    free(mem->ram);
    mem->vram = NULL;
    mem->ram = NULL;
}

/* Проверка, что адрес в VRAM */
static inline bool psp_is_vram(uint32_t canonical_addr)
{
    return canonical_addr >= PSP_VRAM_PHYSICAL_BASE &&
           canonical_addr < PSP_VRAM_PHYSICAL_BASE + PSP_VRAM_ADDRESS_SPAN;
}

/* Проверка, что адрес в RAM */
static inline bool psp_is_ram(uint32_t canonical_addr, uint32_t ram_size)
{
    return canonical_addr >= PSP_PHYSICAL_BASE &&
           canonical_addr < PSP_PHYSICAL_BASE + ram_size;
}

/* Проверка, что адрес валиден */
static inline bool psp_memory_contains(PspGuestMemory *mem, uint32_t address, size_t len)
{
    uint32_t c = psp_address_canonical(address);
    uint64_t end = (uint64_t)c + len;

    if (psp_is_vram(c) && end <= PSP_VRAM_PHYSICAL_BASE + PSP_VRAM_ADDRESS_SPAN)
        return true;
    if (psp_is_ram(c, mem->ram_size) && end <= PSP_PHYSICAL_BASE + mem->ram_size)
        return true;
    return false;
}

/* Чтение 8 бит */
static inline uint8_t psp_memory_load8(PspGuestMemory *mem, uint32_t address)
{
    uint32_t c = psp_address_canonical(address);
    if (psp_is_vram(c)) {
        return mem->vram[(c - PSP_VRAM_PHYSICAL_BASE) & (PSP_VRAM_SIZE - 1)];
    }
    if (psp_is_ram(c, mem->ram_size)) {
        return mem->ram[c - PSP_PHYSICAL_BASE];
    }
    return 0;
}

/* Чтение 16 бит (little-endian) */
static inline uint16_t psp_memory_load16(PspGuestMemory *mem, uint32_t address)
{
    uint16_t value;
    memcpy(&value, &mem->ram[psp_address_canonical(address) - PSP_PHYSICAL_BASE], sizeof(value));
    return value;
}

/* Чтение 32 бит (little-endian) */
static inline uint32_t psp_memory_load32(PspGuestMemory *mem, uint32_t address)
{
    uint32_t value;
    memcpy(&value, &mem->ram[psp_address_canonical(address) - PSP_PHYSICAL_BASE], sizeof(value));
    return value;
}

/* Запись 8 бит */
static inline void psp_memory_store8(PspGuestMemory *mem, uint32_t address, uint8_t value)
{
    uint32_t c = psp_address_canonical(address);
    if (psp_is_vram(c)) {
        mem->vram[(c - PSP_VRAM_PHYSICAL_BASE) & (PSP_VRAM_SIZE - 1)] = value;
    } else if (psp_is_ram(c, mem->ram_size)) {
        mem->ram[c - PSP_PHYSICAL_BASE] = value;
    }
}

/* Запись 16 бит (little-endian) */
static inline void psp_memory_store16(PspGuestMemory *mem, uint32_t address, uint16_t value)
{
    memcpy(&mem->ram[psp_address_canonical(address) - PSP_PHYSICAL_BASE], &value, sizeof(value));
}

/* Запись 32 бит (little-endian) */
static inline void psp_memory_store32(PspGuestMemory *mem, uint32_t address, uint32_t value)
{
    memcpy(&mem->ram[psp_address_canonical(address) - PSP_PHYSICAL_BASE], &value, sizeof(value));
}

/* Копирование в память */
static inline void psp_memory_copy_in(PspGuestMemory *mem, uint32_t address,
                                       const void *src, size_t len)
{
    uint32_t c = psp_address_canonical(address);
    if (psp_is_vram(c)) {
        size_t offset = (c - PSP_VRAM_PHYSICAL_BASE) & (PSP_VRAM_SIZE - 1);
        memcpy(&mem->vram[offset], src, len);
    } else if (psp_is_ram(c, mem->ram_size)) {
        memcpy(&mem->ram[c - PSP_PHYSICAL_BASE], src, len);
    }
}

/* Копирование из памяти */
static inline void psp_memory_copy_out(PspGuestMemory *mem, uint32_t address,
                                        void *dst, size_t len)
{
    uint32_t c = psp_address_canonical(address);
    if (psp_is_vram(c)) {
        size_t offset = (c - PSP_VRAM_PHYSICAL_BASE) & (PSP_VRAM_SIZE - 1);
        memcpy(dst, &mem->vram[offset], len);
    } else if (psp_is_ram(c, mem->ram_size)) {
        memcpy(dst, &mem->ram[c - PSP_PHYSICAL_BASE], len);
    }
}

/* Заполнение нулями */
static inline void psp_memory_zero(PspGuestMemory *mem, uint32_t address, size_t len)
{
    uint32_t c = psp_address_canonical(address);
    if (psp_is_vram(c)) {
        size_t offset = (c - PSP_VRAM_PHYSICAL_BASE) & (PSP_VRAM_SIZE - 1);
        memset(&mem->vram[offset], 0, len);
    } else if (psp_is_ram(c, mem->ram_size)) {
        memset(&mem->ram[c - PSP_PHYSICAL_BASE], 0, len);
    }
}

#endif /* PSP_MEMORY_H */
