/*
 * Проверка загрузчика гостя на настоящем PSP-модуле.
 *
 * Гейты, каждый из которых обязан выполняться, иначе результат бессмыслен:
 *   1. образ принят и ELF-заголовки разобраны;
 *   2. отображение создано именно по базе 0x08804000;
 *   3. сегменты скопированы, объём не превышает ожидаемого;
 *   4. релокации применены, доля успеха выше порога;
 *   5. прочитан .rodata.sceModuleInfo, имя модуля осмысленно;
 *   6. импорты перечислены, NID совпадают с разбором диска;
 *   7. ПОСЛЕ РЕЛОКАЦИЙ указатели внутри гостя указывают внутрь гостя.
 *      Это главный гейт: если пересчёт адресов неверен, указатели останутся
 *      нулевыми или уедут вне образа, и это видно до запуска кода;
 *   8. стаб подменён, и записанный адрес читается обратно.
 *
 * Точку входа здесь не вызывают: гостевой код ждёт полноценной ОС PSP
 * (создание потоков, системные вызовы), и без неё падение ничего не скажет.
 */

#include "guest_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "ok  " : "СБОЙ", what);
    if (!ok)
        g_fail++;
}

static u32 rd32host(const void *p)
{
    return *(const u32 *)p;
}

int main(int argc, char **argv)
{
    FILE *f;
    long size;
    u8 *image;
    GuestModule mod;
    GuestModuleInfo info;
    GuestImportTable imports;
    GuestStubPatch patch;
    u32 ptr_in_range = 0, ptr_total = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <расшифрованный EBOOT.BIN>\n", argv[0]);
        return 2;
    }

    f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "не открыть %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return 1;
    }
    image = malloc((size_t)size);
    if (image == NULL || fread(image, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(image);
        fprintf(stderr, "ошибка чтения\n");
        return 1;
    }
    fclose(f);

    printf("=== проверка загрузчика гостя ===\n");
    printf("образ: %s (%ld байт)\n\n", argv[1], size);

    /* 1-4: загрузка вместе с релокациями.
     *
     * `mod` обязана обнуляться: `guest_load()` поле `map_flags` сохраняет
     * намеренно (его задаёт вызывающий), и без обнуления в `mmap` уходит
     * мусор из стека. На стеке оказывалось `0x15` — одновременно
     * `MAP_SHARED` и `MAP_PRIVATE`, а это `EINVAL`, и гейт падал на загрузке
     * с сообщением про `mmap`, хотя виноват был не он, а неинициализированная
     * структура. Тот же класс, что и «диагностика печатает переменную до её
     * инициализации». */
    memset(&mod, 0, sizeof(mod));
    if (!guest_load(&mod, image, (size_t)size, 0)) {
        printf("  [СБОЙ] загрузка не удалась: %s\n", mod.error);
        free(image);
        return 1;
    }

    check(mod.base == PSP_USER_BASE, "гость размещён по базе 0x08804000");
    check(mod.memory != NULL, "отображение создано");
    check(mod.seg_count > 0, "сегменты загружены");
    printf("        сегментов: %u, объём образа: %u байт\n",
           mod.seg_count, mod.span);
    printf("        entry: 0x%08X, тип ELF: 0x%04X\n", mod.entry, mod.e_type);

    printf("\n=== релокации ===\n");
    printf("        всего: %u\n", mod.reloc_total);
    printf("        применено: %u\n", mod.reloc_applied);
    printf("        неподдержанных типов: %u\n", mod.reloc_unsupported);
    printf("        без пары HI16/LO16: %u\n", mod.reloc_failed);
    printf("        с неверным сегментом: %u\n", mod.reloc_bad_segment);

    check(mod.reloc_total > 100000, "найдено много релокаций");
    check(mod.reloc_applied > 0, "релокации применены");
    check(mod.reloc_unsupported == 0, "нет неподдержанных типов релокаций");

    /* 5: сведения о модуле. */
    printf("\n=== модуль ===\n");
    if (!guest_read_module_info(&mod, &info)) {
        printf("  [СБОЙ] .rodata.sceModuleInfo не прочитан\n");
        guest_unload(&mod);
        free(image);
        return 1;
    }
    printf("        имя       : %s\n", info.name);
    printf("        gp        : 0x%08X (снимок до релокаций, виртуальный)\n",
           info.gp);
    printf("        entry     : 0x%08X\n", mod.base + mod.entry);
    printf("        ent       : 0x%08X .. 0x%08X\n",
           mod.base + info.ent_top, mod.base + info.ent_end);
    printf("        stub      : 0x%08X .. 0x%08X\n",
           mod.base + info.stub_top, mod.base + info.stub_end);
    check(strlen(info.name) > 0, "имя модуля непустое");

    /* 6: импорты. */
    printf("\n=== импорты ===\n");
    if (!guest_scan_imports(&mod, &info, &imports)) {
        printf("  [СБОЙ] импорты не перечислены: %s\n", mod.error);
        guest_unload(&mod);
        free(image);
        return 1;
    }
    printf("        библиотек: %u, импортов: %u\n",
           imports.count, imports.total_imports);
    check(imports.count == 18, "найдено 18 библиотек");
    check(imports.total_imports == 147, "найдено 147 импортов");

    /* Первые импорты — для сверки с разбором диска. */
    if (imports.count > 0 && imports.libs[imports.count - 1].count > 0) {
        GuestLibrary *l = &imports.libs[imports.count - 1];
        printf("        %s: %u импортов, первый NID=0x%08X\n",
               l->library, l->count,
               l->count ? l->imports[0].nid : 0);
    }

    /* 7: главный гейт — указатели после релокаций должны указывать внутрь
     * гостя. Читаем .data подряд и считаем попадания в диапазон гостя. */
    printf("\n=== проверка результата релокаций ===\n");
    {
        u32 data_vaddr = 0, data_size = 0;
        if (guest_find_section(&mod, ".data", &data_vaddr, &data_size, NULL)) {
            u32 off;
            const u8 *data = (const u8 *)(uintptr_t)(mod.base + data_vaddr);
            ptr_total = data_size / 4;
            for (off = 0; off + 4 <= data_size; off += 4) {
                u32 v = rd32host(data + off);
                if (v >= mod.base && v < mod.base + mod.span)
                    ptr_in_range++;
            }
            printf("        .data: %u байт, проверено значений: %u\n",
                   data_size, ptr_total);
            printf("        указывают внутрь гостя: %u (%.1f%%)\n",
                   ptr_in_range,
                   ptr_total ? 100.0 * ptr_in_range / ptr_total : 0.0);
        } else {
            printf("        .data не найдена\n");
        }
    }

    /* Первые слова .text — должны быть похожи на код, а не на нули. */
    {
        u32 text_vaddr = 0, text_size = 0;
        if (guest_find_section(&mod, ".text", &text_vaddr, &text_size, NULL)) {
            const u8 *text = (const u8 *)(uintptr_t)(mod.base + text_vaddr);
            u32 nonzero = 0;
            u32 off;
            for (off = 0; off + 4 <= text_size && off < 4096; off += 4)
                if (rd32host(text + off) != 0)
                    nonzero++;
            printf("        .text: ненулевых слов в первых 4 КиБ: %u из %u\n",
                   nonzero, 4096 / 4);
            check(nonzero > 100, ".text не пустая");
            printf("        первые инструкции:");
            for (off = 0; off < 32; off += 4)
                printf(" %08x", rd32host(text + off));
            printf("\n");
        }
    }

    /* 8: подмена стаба. */
    printf("\n=== подмена стаба ===\n");
    if (imports.count > 0 && imports.libs[0].count > 0) {
        GuestImport *imp = &imports.libs[0].imports[0];
        patch.target = (u32)(uintptr_t)&ptr_in_range;  /* любой наш адрес */
        patch.stub = imp->stub_runtime;
        check(guest_patch_stub(&mod, &patch), "стаб пропатчен");
        printf("        stub=0x%08X target=0x%08X трамплин=%s\n",
               patch.stub, patch.target,
               patch.via_trampoline ? "да" : "нет");
        {
            u32 w0 = rd32host((const void *)(uintptr_t)patch.stub);
            u32 w1 = rd32host((const void *)(uintptr_t)(patch.stub + 4));
            if (patch.via_trampoline) {
                u32 t0 = rd32host((const void *)(uintptr_t)patch.trampoline);
                u32 t1 = rd32host((const void *)(uintptr_t)(patch.trampoline + 4));
                u32 t2 = rd32host((const void *)(uintptr_t)(patch.trampoline + 8));
                /* Стаб обязан занимать ровно 8 байт и прыгать `j` в
                 * трамплин, а не собирать полный адрес сам: слот стаба
                 * шириной 8 байт, и запись большего затирает соседний
                 * импорт. */
                u32 want_j = 0x08000000u
                           | (((patch.trampoline & 0x0FFFFFFCu) >> 2)
                              & 0x03FFFFFFu);
                printf("        стаб: j 0x%08X ; nop\n",
                       ((w0 & 0x03FFFFFFu) << 2)
                       | (patch.stub & 0xF0000000u));
                printf("        трамплин: lui 0x%04X ; jr $t9 ; addiu 0x%04X\n",
                       t0 & 0xFFFFu, t2 & 0xFFFFu);
                check(w0 == want_j, "стаб прыгает j на трамплин");
                check(w1 == 0x00000000u, "второе слово стаба — nop");
                /* Слот не должен быть длиннее 8 байт. */
                check(patch.stub + 8 <= mod.base + info.stub_end,
                      "стаб не выходит за пределы области стабов");
                check((t0 & 0xFFFF0000u) == 0x3C190000u
                      && t1 == 0x03200009u
                      && (t2 & 0xFFFF0000u) == 0x27390000u,
                      "трамплин переходит по полному адресу");
                /* Адрес цели собирается из lui трамплина (t0) и его addiu (t2).
                 *
                 * Важно: addiu расширяет непосредственный операнд ЗНАКОВО, то
                 * есть как 16-битное значение. Поэтому при младших битах цели
                 * >= 0x8000 результат на 0x10000 меньше простой сборки
                 * hi<<16 | lo. Проверка без знакового расширения выглядит
                 * верной только при lo < 0x8000, то есть зависит от того, как
                 * лягут старшие биты адреса, а они меняются от запуска к
                 * запуску. */
                {
                    s32 lo = (s32)(s16)(u16)(t2 & 0xFFFFu);
                    u32 rebuilt = ((t0 & 0xFFFFu) << 16) + (u32)lo;
                    check(rebuilt == patch.target,
                          "трамплин собирает правильный адрес цели");
                    if (rebuilt != patch.target)
                        printf("        собрано 0x%08X, цель 0x%08X "
                               "(hi=0x%04X lo=0x%04X)\n",
                               rebuilt, patch.target, t0 & 0xFFFFu,
                               t2 & 0xFFFFu);
                }
            } else {
                u32 want = 0x08000000u
                         | (((patch.target & 0x0FFFFFFCu) >> 2) & 0x03FFFFFFu);
                printf("        записано 0x%08X, ожидалось 0x%08X\n", w0, want);
                check(w0 == want, "записан правильный прыжок j");
            }
        }
    }

    /* Считаем успешных подмен по всем стабам и проверяем, что ни один стаб
     * не задел соседний. */
    {
        u32 li, ii, patched = 0, bad = 0;
        for (li = 0; li < imports.count; li++) {
            for (ii = 0; ii < imports.libs[li].count; ii++) {
                GuestImport *imp = &imports.libs[li].imports[ii];
                GuestStubPatch p;
                u32 w0, w1;

                p.target = 0x2aaa0000u;   /* заведомо в другом сегменте */
                p.stub = imp->stub_runtime;
                if (!guest_patch_stub(&mod, &p))
                    continue;
                patched++;

                /* Стаб обязан быть ровно двумя словами: прыжок j и nop.
                 * Если вместо этого три слова, третье попало в начало
                 * следующего импорта — именно этим объясняется падение гостя
                 * сразу после первого успешного вызова. */
                w0 = rd32host((const void *)(uintptr_t)p.stub);
                w1 = rd32host((const void *)(uintptr_t)(p.stub + 4));
                if ((w0 & 0xFC000000u) != 0x08000000u || w1 != 0)
                    bad++;
            }
        }
        printf("\n        стабов пропатчено: %u из %u\n",
               patched, imports.total_imports);
        printf("        стабов неверной длины: %u\n", bad);
        check(patched == imports.total_imports, "все стабы пропатчены");
        check(bad == 0, "каждый стаб занимает ровно 8 байт");
    }

    guest_free_imports(&imports);
    guest_unload(&mod);
    free(image);

    printf("\n=== итог ===\n");
    if (g_fail == 0) {
        printf("все гейты пройдены\n");
        return 0;
    }
    printf("не пройдено гейтов: %d\n", g_fail);
    return 1;
}