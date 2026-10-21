/*
 * PSP ELF — загрузчик ELF с поддержкой PSP-специфичных релокаций
 *
 * Основана на подходе PSPRecomp:
 *   - Проверка формата (не зашифрован, не PBP)
 *   - Загрузка PT_LOAD сегментов
 *   - Обработка PSP-специфичных релокаций (R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16)
 *   - Поиск ModuleInfo
 *   - Сканирование импортов
 */

#ifndef PSP_ELF_H
#define PSP_ELF_H

#include "psp_memory.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * Константы ELF
 * ==========================================================================*/

#define ELF_MAGIC           0x464C457F  /* "\x7fELF" */
#define ELF_CLASS_32        1
#define ELF_DATA_2LSB       1
#define EM_MIPS             8

#define PT_LOAD             1
#define PT_DYNAMIC          2

#define SHT_PROGBITS        1
#define SHT_SYMTAB          2
#define SHT_STRTAB          3
#define SHT_NOBITS          8
#define SHT_REL             9

/* PSP-специфичные типы секций */
#define SHT_PSP_REL         0x700000A0

/* Типы релокаций MIPS */
#define R_MIPS_32           2
#define R_MIPS_26           4
#define R_MIPS_HI16         5
#define R_MIPS_LO16         6

/* Тип ELF-файла PSP */
#define ELF_TYPE_PSP_PRX    0xFFA0

/* ============================================================================
 * Структуры ELF
 * ==========================================================================*/

#pragma pack(push, 1)

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
    uint32_t offset;
    uint32_t info;
} Elf32_Rel;

/* PSP ModuleInfo */
typedef struct {
    uint16_t attributes;
    uint8_t  version[2];
    char     name[28];
    uint32_t gp;
    uint32_t ent_top;
    uint32_t ent_end;
    uint32_t stub_top;
    uint32_t stub_end;
} PspModuleInfo;

#pragma pack(pop)

/* ============================================================================
 * Структуры загрузчика
 * ==========================================================================*/

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} PspElfSegment;

typedef struct {
    char     name[256];
    uint32_t type;
    uint32_t flags;
    uint32_t addr;
    uint32_t offset;
    uint32_t size;
    uint32_t link;
    uint32_t info;
    uint32_t addralign;
    uint32_t entsize;
} PspElfSection;

typedef struct {
    char     library[256];
    uint32_t nid;
    uint32_t stub_address;
} PspImport;

typedef struct {
    uint8_t *data;
    size_t   size;
    uint16_t type;
    uint32_t entry;

    PspElfSegment *segments;
    size_t num_segments;

    PspElfSection *sections;
    size_t num_sections;

    PspModuleInfo module_info;
    bool has_module_info;

    PspImport *imports;
    size_t num_imports;
} PspElfImage;

/* ============================================================================
 * Функции загрузчика
 * ==========================================================================*/

/* Загрузка ELF из файла */
bool psp_elf_load(const char *path, PspElfImage *image, PspGuestMemory *mem);

/* Выгрузка ELF */
void psp_elf_unload(PspElfImage *image);

/* Применение релокаций */
bool psp_elf_apply_relocations(PspElfImage *image, PspGuestMemory *mem, uint32_t load_base);

/* Сканирование импортов */
bool psp_elf_scan_imports(PspElfImage *image, PspGuestMemory *mem, uint32_t load_base);

/* Получение адреса точки входа */
uint32_t psp_elf_get_entry(PspElfImage *image, uint32_t load_base);

#endif /* PSP_ELF_H */
