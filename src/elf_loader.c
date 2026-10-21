/*
 * ELF Loader — загрузка расшифрованного PSP EBOOT.BIN
 *
 * PSP EBOOT.BIN после расшифровки — это стандартный 32-bit ELF (LE).
 * Загрузчик мапит секции .text и .data в RAM.
 *
 * Важно: PSP и Dune HD Max — оба MIPS32 LE, поэтому код игр
 * запускается напрямую, без рекомпиляции или трансляции инструкций.
 */

#include "psp_hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <elf.h>
#include <errno.h>

/* Проверка, что файл — валидный ELF */
static bool is_valid_elf(const Elf32_Ehdr *ehdr)
{
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        return false;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        return false;
    }
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }
    if (ehdr->e_machine != EM_MIPS) {
        return false;
    }
    return true;
}

/* Загрузка ELF-файла в память */
bool psp_elf_load(const char *path, PspElf *elf)
{
    FILE *fp;
    Elf32_Ehdr ehdr;
    Elf32_Phdr *phdrs;
    void *base;
    u32 min_addr = 0xFFFFFFFF;
    u32 max_addr = 0;
    int i;

    if (!elf || !path) {
        return false;
    }

    memset(elf, 0, sizeof(*elf));

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[ELF] Не удалось открыть файл: %s\n", path);
        return false;
    }

    /* Читаем ELF-заголовок */
    if (fread(&ehdr, sizeof(ehdr), 1, fp) != 1) {
        fprintf(stderr, "[ELF] Ошибка чтения заголовка\n");
        fclose(fp);
        return false;
    }

    if (!is_valid_elf(&ehdr)) {
        fprintf(stderr, "[ELF] Невалидный ELF-файл\n");
        fclose(fp);
        return false;
    }

    /* Читаем программные заголовки */
    phdrs = malloc(ehdr.e_phnum * sizeof(Elf32_Phdr));
    if (!phdrs) {
        fprintf(stderr, "[ELF] Ошибка выделения памяти\n");
        fclose(fp);
        return false;
    }

    fseek(fp, ehdr.e_phoff, SEEK_SET);
    if (fread(phdrs, sizeof(Elf32_Phdr), ehdr.e_phnum, fp) != ehdr.e_phnum) {
        fprintf(stderr, "[ELF] Ошибка чтения программных заголовков\n");
        free(phdrs);
        fclose(fp);
        return false;
    }

    /* Вычисляем диапазон адресов для PT_LOAD секций */
    for (i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            if (phdrs[i].p_vaddr < min_addr) {
                min_addr = phdrs[i].p_vaddr;
            }
            if (phdrs[i].p_vaddr + phdrs[i].p_memsz > max_addr) {
                max_addr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
            }
        }
    }

    if (min_addr == 0xFFFFFFFF) {
        fprintf(stderr, "[ELF] Нет PT_LOAD секций\n");
        free(phdrs);
        fclose(fp);
        return false;
    }

    /* Выделяем память под весь ELF */
    elf->size = max_addr - min_addr;
    elf->base = mmap(NULL, elf->size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (elf->base == MAP_FAILED) {
        fprintf(stderr, "[ELF] Ошибка mmap: %s\n", strerror(errno));
        free(phdrs);
        fclose(fp);
        return false;
    }

    /* Копируем PT_LOAD секции */
    for (i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            void *dst = (char *)elf->base + (phdrs[i].p_vaddr - min_addr);
            fseek(fp, phdrs[i].p_offset, SEEK_SET);
            if (fread(dst, 1, phdrs[i].p_filesz, fp) != phdrs[i].p_filesz) {
                fprintf(stderr, "[ELF] Ошибка чтения секции %d\n", i);
                munmap(elf->base, elf->size);
                free(phdrs);
                fclose(fp);
                return false;
            }
            /* Заполняем нулями остаток (BSS) */
            if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
                memset((char *)dst + phdrs[i].p_filesz, 0,
                       phdrs[i].p_memsz - phdrs[i].p_filesz);
            }
        }
    }

    elf->entry = ehdr.e_entry;
    elf->name = strdup(path);

    printf("[ELF] Загружен: %s\n", path);
    printf("[ELF] База: %p, Размер: %u байт, Точка входа: 0x%08X\n",
           elf->base, elf->size, elf->entry);

    free(phdrs);
    fclose(fp);
    return true;
}

/* Выгрузка ELF из памяти */
void psp_elf_unload(PspElf *elf)
{
    if (!elf) {
        return;
    }
    if (elf->base && elf->size > 0) {
        munmap(elf->base, elf->size);
    }
    if (elf->name) {
        free(elf->name);
    }
    memset(elf, 0, sizeof(*elf));
}
