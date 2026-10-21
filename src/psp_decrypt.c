/*
 * pspdecrypt — расшифровка розничного PSP EBOOT.BIN (формат ~PSP).
 *
 * Проверено на ULJS00042 (Magna Carta Portable): вход ~PSP, тег 0xC0CB167C.
 *
 * Ключевой факт, установленный по свежему PPSSPP: DISC_ID для расшифровки
 * НЕ НУЖЕН. Ключ выбирается по четырёхбайтовому тегу на смещении 0xD0
 * самой прошивки; для розничного EBOOT это всегда 0xC0CB167C, которому
 * соответствует ключ g_keyEBOOT2xx и код 0x5D. Схема с выводом k1 из SHA1
 * от DISC_ID — это PSP 1.00, ~2004 года, и в современных инструментах её
 * уже нет.
 *
 * Используется libkirk (PPSSPP, GPL-2.0), файлы в third_party/libkirk.
 * Лицензия LGPL/GPL на проект распространяется соответственно.
 *
 * Проверка результата: у корректно расшифрованного файла первые четыре
 * байта — магия ELF (7f 45 4c 46). Это служит и гейтом: без правильного
 * ключа результат не будет начинаться с неё.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "psp_decrypt.h"
#include "kirk_engine.h"
#include "SHA1.h"

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;

/* ---------------------------------------------------------------------------
 * Заголовок ~PSP. Раскладка подтверждена структурой PSP_Header из
 * Core/ELF/PrxDecrypter.h: modname действительно лежит по смещению 0x0A.
 * -------------------------------------------------------------------------*/
#pragma pack(push, 1)
typedef struct {
    u8      signature[4];       /* 0x00 "~PSP" */
    u16     attribute;          /* 0x04 */
    u16     comp_attribute;     /* 0x06 */
    u8      module_ver_lo;      /* 0x08 */
    u8      module_ver_hi;      /* 0x09 */
    char    modname[28];        /* 0x0A */
    u8      version;            /* 0x26 */
    u8      nsegments;          /* 0x27 */
    u32     elf_size;           /* 0x28 */
    u32     psp_size;           /* 0x2C */
    u32     entry;              /* 0x30 */
    u32     modinfo_offset;     /* 0x34 */
    s32     bss_size;           /* 0x38 */
    u16     seg_align[4];       /* 0x3C */
    u32     seg_address[4];     /* 0x44 */
    s32     seg_size[4];        /* 0x54 */
    u32     reserved[5];        /* 0x64 */
    u32     devkitversion;      /* 0x78 */
    u32     decrypt_mode;       /* 0x7C */
    u8      key_data0[0x30];    /* 0x80 */
    s32     comp_size;          /* 0xB0 */
    s32     field_80;           /* 0xB4 */
    s32     reserved2[2];       /* 0xB8 */
    u8      key_data1[0x10];    /* 0xC0 */
    u32     tag;                /* 0xD0 — выбирает ключ */
    u8      scheck[0x58];       /* 0xD4 */
    u32     key_data2;          /* 0x12C */
    u32     oe_tag;             /* 0x130 */
    u8      key_data3[0x1C];    /* 0x134 */
} PspEbootHeader;
#pragma pack(pop)

_Static_assert(sizeof(PspEbootHeader) == 0x150, "заголовок ~PSP должен быть 0x150 байт");

/* ---------------------------------------------------------------------------
 * Ключ розничного EBOOT. Таблица из Core/ELF/PrxDecrypter.cpp:
 *     { 0xC0CB167C, g_keyEBOOT2xx, 0x5D, 0x5D },
 * Ключ хранится уже «развёрнутым», поэтому kirk7 к нему не применяется.
 * -------------------------------------------------------------------------*/
static const u32 g_keyEBOOT2xx[36] = {
    0xDA8E36FA, 0x5DD97447, 0x76C19874, 0x97E57EAF, 0x1CAB09BD, 0x9835BAC6,
    0x03D39281, 0x03B205CF, 0x2882E734, 0xE714F663, 0xB96E2775, 0xBD8AAFC7,
    0x1DD3EC29, 0xECA4A16C, 0x5F69EC87, 0x85981E92, 0x7CFCAE21, 0xBAE9DD16,
    0xE6A97804, 0x2EEE02FC, 0x61DF8A3D, 0xDD310564, 0x9697E149, 0xC2453F3B,
    0xF91D8456, 0x39DA6BC8, 0xB3E5FEF5, 0x89C593A3, 0xFB5C8ABC, 0x6C0B7212,
    0xE10DD3CB, 0x98D0B2A8, 0x5FD61847, 0xF0DC2357, 0x7701166A, 0x0F5C3B68,
};

#define TAG_RETAIL_EBOOT 0xC0CB167Cu
#define KIRK_CODE_EBOOT2  0x5D

/* ---------------------------------------------------------------------------
 * Раскладка PRX типа 1 (Core/ELF/PrxDecrypter.cpp, PRXType1).
 * kirkBlock собирается из двух несмежных кусков файла, поэтому читается
 * прямо из буфера, а не как единая структура.
 * -------------------------------------------------------------------------*/
typedef struct {
    u8 tag[4];        /* 0xD0 */
    u8 sha1[0x14];    /* 0xD4 */
    u8 unused[0x28];  /* 0xE8 */
    u8 kirkBlock[0x90];
    u8 prxHeader[0x80];
} PrxType1;

static void prx_type1_load(PrxType1 *t, const u8 *prx)
{
    memcpy(t->tag, prx + 0xD0, sizeof(t->tag));
    memcpy(t->sha1, prx + 0xD4, sizeof(t->sha1));
    memcpy(t->unused, prx + 0xE8, sizeof(t->unused));
    /* kirkBlock: 0x40 байта из 0x110, затем 0x50 байт из 0x80 */
    memcpy(t->kirkBlock, prx + 0x110, 0x40);
    memcpy(t->kirkBlock + 0x40, prx + 0x80, 0x50);
    memcpy(t->prxHeader, prx, sizeof(t->prxHeader));
}

static void prx_type1_decrypt(PrxType1 *t, int key)
{
    kirk7(t->sha1 + 0x0C, t->sha1 + 0x0C, 0xA0, key);
}

/* XOR-развёртка заголовка KIRK. Смещения 0x14 и 0x20 — из эталонного кода. */
static void decrypt_kirk_header_type0(u8 *out, const u8 *in, const u8 *xorbuf, int key)
{
    size_t i;
    for (i = 0; i < 0x70; i++)
        out[i] = in[i] ^ xorbuf[i + 0x14];
    kirk7(out, out, 0x70, key);
    for (i = 0; i < 0x70; i++)
        out[i] = out[i] ^ xorbuf[i + 0x20];
}

/* Проверка, что объём вывода KIRK не выходит за буфер.
 * Заголовок сам по себе размеров не ограничивает, поэтому размер блока
 * берётся из расшифрованного заголовка, а не от вызывающего кода. */
static bool kirk_output_fits(u32 header_size, u32 data_offset, u32 data_size, u32 size)
{
    u64 aligned = ((u64)data_size + 15) & ~(u64)15;
    return (u64)header_size + (u64)data_offset + aligned <= (u64)size;
}

/* ---------------------------------------------------------------------------
 * Расшифровка.
 * -------------------------------------------------------------------------*/
u8 *psp_decrypt_eboot(const u8 *in, size_t size, size_t *out_size, const char **why)
{
    KirkState kirk;
    u8 *out;
    u8 xorbuf[0x90];
    PrxType1 type1;
    SHA_CTX ctx;
    u8 digest[0x14];
    u8 *kirk_header;
    u32 tag;
    const u32 offset = sizeof(PspEbootHeader) - sizeof(KIRK_CMD1_HEADER) - 0x80;
    u32 decrypt_size;

    *why = NULL;

    if (size < sizeof(PspEbootHeader)) {
        *why = "файл меньше заголовка ~PSP";
        return NULL;
    }

    tag = *(const u32 *)(in + 0xD0);
    if (tag != TAG_RETAIL_EBOOT) {
        *why = "тег не соответствует розничному EBOOT (ожидался 0xC0CB167C)";
        return NULL;
    }

    kirk_init(&kirk);
    memcpy(xorbuf, g_keyEBOOT2xx, sizeof(xorbuf));

    prx_type1_load(&type1, in);
    prx_type1_decrypt(&type1, KIRK_CODE_EBOOT2);

    /* Контрольная сумма заголовка: подтверждает, что ключ верный. */
    SHAInit(&ctx);
    SHAUpdate(&ctx, xorbuf, 0x14);
    SHAUpdate(&ctx, type1.unused, sizeof(type1.unused));
    SHAUpdate(&ctx, type1.kirkBlock, sizeof(type1.kirkBlock));
    SHAUpdate(&ctx, type1.prxHeader, sizeof(type1.prxHeader));
    SHAFinal(digest, &ctx);

    if (memcmp(digest, type1.sha1, sizeof(digest)) != 0) {
        *why = "контрольная сумма заголовка не сошлась — ключ неверен";
        return NULL;
    }

    out = malloc(size);
    if (out == NULL) {
        *why = "не хватило памяти под результат";
        return NULL;
    }
    memcpy(out, in, size);

    kirk_header = out + offset;
    memcpy(kirk_header, type1.kirkBlock, sizeof(KIRK_CMD1_HEADER));
    memcpy(kirk_header + sizeof(KIRK_CMD1_HEADER), type1.prxHeader, 0x80);
    decrypt_kirk_header_type0(kirk_header, type1.kirkBlock, xorbuf, KIRK_CODE_EBOOT2);

    {
        const KIRK_CMD1_HEADER *h = (const KIRK_CMD1_HEADER *)kirk_header;
        if (!kirk_output_fits((u32)sizeof(KIRK_CMD1_HEADER), h->data_offset,
                              h->data_size, size)) {
            free(out);
            *why = "размер блока из заголовка выходит за пределы файла";
            return NULL;
        }
    }

    if (kirk_sceUtilsBufferCopyWithRange(&kirk, out, (int)size, kirk_header,
                                        (int)(size - offset),
                                        KIRK_CMD_DECRYPT_PRIVATE) != 0) {
        free(out);
        *why = "KIRK отклонил блок (неверный ключ или данные)";
        return NULL;
    }

    decrypt_size = *(const u32 *)(in + 0xB0);
    *out_size = (decrypt_size > 0 && decrypt_size <= size) ? decrypt_size : size;
    return out;
}

/* ------------------------------------------------------------------------- */
void psp_decrypt_dump_header(const u8 *d, size_t size)
{
    const PspEbootHeader *h = (const PspEbootHeader *)d;
    char name[29];

    memcpy(name, h->modname, 28);
    name[28] = '\0';

    printf("=== заголовок ~PSP ===\n");
    printf("  сигнатура      : %.4s\n", h->signature);
    printf("  версия         : %u\n", h->version);
    printf("  имя модуля     : %s\n", name);
    printf("  сегментов      : %u\n", h->nsegments);
    printf("  elf_size       : %u\n", h->elf_size);
    printf("  psp_size       : %u\n", h->psp_size);
    printf("  entry          : 0x%08X\n", h->entry);
    printf("  modinfo_offset : 0x%08X\n", h->modinfo_offset);
    printf("  bss_size       : 0x%08X\n", h->bss_size);
    printf("  seg_align[0]   : %u\n", h->seg_align[0]);
    printf("  seg_size[0]    : %u\n", h->seg_size[0]);
    printf("  decrypt_mode   : 0x%08X\n", h->decrypt_mode);
    printf("  тег            : 0x%08X\n", h->tag);
    printf("  размер файла   : %zu\n", size);
    printf("  comp_size      : %d\n", h->comp_size);
    printf("\n");
}

int psp_is_encrypted_eboot(const u8 *data, size_t size)
{
    if (data == NULL || size < 4)
        return 0;
    return memcmp(data, "~PSP", 4) == 0;
}
