/*
 * Загрузка гостевого модуля PSP: сегменты, релокации, стабы.
 * Подробности назначения функций — в include/guest_load.h.
 */

#include "guest_load.h"
#include "guest_reloc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Опережающее объявление: используется в guest_load до определения. */
static bool read_module_info_raw(GuestModule *m, GuestModuleInfo *out);

/* Объём отображения: сегмент гостя плюс пул трамплинов плюс запас.
 * Пул обязан лежать в том же отображении, а не в памяти поверх гостя:
 * трамплин — это исполняемый код, и писать его в гостевые данные нельзя. */
#define GUEST_MAP_HEADROOM (4u * 1024u * 1024u)

/* Число записей HI16/LO16 в одном модуле — верхняя граница для поиска пары. */
#define RELOC_CHUNK 4096

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

static u32 host_at(u32 runtime_addr)
{
    return *(const u32 *)(const void *)(uintptr_t)runtime_addr;
}

/* ------------------------------------------------------------------------- */

/* Смещение начала данных от начала отображения гостя.
 *
 * Берётся из таблицы секций: первый раздел с флагом записи, но БЕЗ флага
 * исполнения. Это `.data`. С него снимается право исполнения — см. вызов в
 * `map_guest`.
 *
 * 0, если раздел не найден: тогда снимать нечего и `mprotect` не
 * вызывается. Значение 0 НЕ означает «данных нет» — это отдельный случай,
 * и о нём сообщается полем `data_protected`. */
static u32 data_section_off(GuestModule *m)
{
    const u8 *d = m->image;
    u16 shnum = rd16(d + 48);
    u16 shentsize = rd16(d + 46);
    u32 shoff = rd32(d + 0x20);
    u32 best = 0;
    u16 i;
    m->sec_diag[0] = shnum;
    m->sec_diag[1] = shentsize;
    m->sec_diag[2] = shoff;
    m->sec_diag[3] = 0;
    m->sec_diag[4] = 0;
    if ((u64)shoff + (u64)shnum * shentsize > m->image_size) {
        m->sec_diag[4] = 1;   /* таблица не помещается в файл */
        return 0;
    }
    /* `e_shstrndx` здесь НЕ нужен: имя раздела не требуется, нужен только
     * флаг. Первая версия функции читала его как `rd32(d + 0x30)`, то есть
     * захватывала ещё и `e_shnum`, получала `0x1001C`, и проверка
     * «индекс вне таблицы» отсекала ВСЕ разделы — `mprotect` не вызывался
     * вовсе, и отчёт честно писал «НЕТ — mprotect не сработал». */

    for (i = 0; i < shnum; i++) {
        u32 o = shoff + (u32)i * shentsize;
        u32 flags = rd32(d + o + 8);
        u32 addr = rd32(d + o + 12);
        u32 size = rd32(d + o + 20);

        if (size == 0)
            continue;
        if ((flags & 0x1) == 0)          /* нет записи */
            continue;
        if ((flags & 0x4) != 0)          /* есть исполнение */
            continue;
        m->sec_diag[3]++;
        if (addr > best && (u64)addr + size <= m->span)
            best = addr;
    }
    return best;
}

static bool compute_span(GuestModule *m)
{
    const u8 *d = m->image;
    u16 phnum = rd16(d + 44);
    u32 phoff = rd32(d + 28);
    u32 phentsize = rd16(d + 42);
    u32 need = 0;
    u16 i;

    for (i = 0; i < phnum; i++) {
        u32 off = phoff + (u32)i * phentsize;
        u32 p_type, p_vaddr, p_memsz;

        if ((u64)off + 32 > (u64)m->image_size) {
            snprintf(m->error, sizeof(m->error),
                     "заголовок сегмента %u вне файла", i);
            return false;
        }
        p_type = rd32(d + off);
        p_vaddr = rd32(d + off + 8);
        p_memsz = rd32(d + off + 20);
        if (p_type != 1 || p_memsz == 0)
            continue;
        if ((u64)p_vaddr + p_memsz > 0xF0000000u) {
            snprintf(m->error, sizeof(m->error),
                     "сегмент %u не помещается в 32-битное пространство", i);
            return false;
        }
        if (p_vaddr + p_memsz > need)
            need = p_vaddr + p_memsz;
    }

    if (need == 0) {
        snprintf(m->error, sizeof(m->error), "нет сегментов с данными");
        return false;
    }
    m->span = need;
    return true;
}

/* Куча VPL — парой на общий tmpfs-файл, той же схемой, что VRAM.
 *
 * Почему отдельная область, а не резерв разделов: гость строит свои
 * кучи из блоков VPL суммарно 0x841B00 (замер прогона 35), и в
 * `GUEST_PART_RESERVE` (1 МиБ) это не помещается.
 *
 * АДРЕС НЕ ДЕТЕРМИНИРОВАН между экземплярами `shell`: прогон 36
 * лёг на 0x09000000 целиком, прогон 37 (2026-10-15) нашёл его
 * занятым — область не отобразилась, вся куча деградировала и
 * `HeapSystem` вернулся. Поэтому не один адрес, а ЛЕСТНИЦА
 * кандидатов в окне «конец карты образа .. 0x0A000000» и ЛЕСТНИЦА
 * размеров: фактический размер попадает в `vpl_bytes`, и HLE
 * отвечает `MaxFreeMemSize` им — гость строит размеры своих куч
 * из ответа, то есть подстраивается (замер прогона 35: размеры
 * дескрипторов куч вычислены из freeSize).
 *
 * Отказ не рушит загрузку: `vpl_base` остаётся нулём, HLE отвечает
 * `MaxFreeMemSize` честным нулём, и гость ведёт себя как раньше —
 * сбой кучи при этом виден в отчёте по полям `vpl_*`. */
#define VPL_CANDIDATES 5u
#define VPL_SIZES      5u

static const u32 vpl_addr[VPL_CANDIDATES] = {
	GUEST_VPL_BASE,
	GUEST_VPL_BASE + 0x00500000u,
	GUEST_VPL_BASE - 0x00100000u,
	GUEST_VPL_BASE - 0x00200000u,
	GUEST_VPL_BASE - 0x00300000u,
};
static const u32 vpl_size[VPL_SIZES] = {
	GUEST_VPL_BYTES, 0x00800000u, 0x00600000u, 0x00400000u, 0x00200000u,
};

static void map_vpl(GuestModule *m)
{
    void *v1;
    void *v2;
    u32 si;
    u32 ci;
    int fd;

    m->vpl_base = 0;
    m->vpl_mirror = 0;
    m->vpl_bytes = 0;
    fd = open("/tmp/.psp_guest_vpl", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    for (si = 0; si < VPL_SIZES && m->vpl_base == 0; si++) {
        if (ftruncate(fd, (off_t)vpl_size[si]) != 0)
            continue;
        for (ci = 0; ci < VPL_CANDIDATES && m->vpl_base == 0; ci++) {
            u32 want = vpl_addr[ci];
            u32 want_mirror = want + 0x40000000u;
            size_t bytes = vpl_size[si];

            v1 = mmap((void *)(uintptr_t)want, bytes,
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (v1 == MAP_FAILED) {
                v1 = NULL;
            } else if ((uintptr_t)v1 != want) {
                munmap(v1, bytes);
                v1 = NULL;
            }
            if (v1 == NULL)
                continue;
            v2 = mmap((void *)(uintptr_t)want_mirror, bytes,
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (v2 == MAP_FAILED) {
                v2 = NULL;
            } else if ((uintptr_t)v2 != want_mirror) {
                munmap(v2, bytes);
                v2 = NULL;
            }
            if (v1 != NULL) {
                m->vpl_base = want;
                m->vpl_bytes = (u32)bytes;
                if (v2 != NULL)
                    m->vpl_mirror = want_mirror;
            } else if (v2 != NULL) {
                /* Основного отображения нет — зеркало одно не
                 * годится: `MaxFreeMemSize` обязан отвечать за
                 * кэшируемый адрес. */
                munmap(v2, bytes);
            }
        }
    }
    close(fd);
    (void)unlink("/tmp/.psp_guest_vpl");
}

static bool map_guest(GuestModule *m)
{
    u32 data_off = 0;

    void *want = (void *)(uintptr_t)m->base;
    size_t map_size;
    void *p;
    int mirror_fd;

    /* Отображение должно покрывать образ, пул трамплинов и резерв под
     * разделы гостя, лежащие сразу за ним: всё это должно быть в одном
     * отображении и в том же 256 МБ сегменте, иначе адрес, который мы
     * отдаём гостю, окажется за пределами образа. */
    map_size = ((size_t)m->span + GUEST_TRAMP_SIZE + GUEST_PART_RESERVE
                + 0xFFFFu) & ~(size_t)0xFFFFu;

    /* Основное отображение делается на ОБЩИЙ файл в tmpfs, а не на
     * анонимную память: у PSP одна память видна по нескольким адресам
     * (кэшируемый `0x08…` и зеркало без кэша `0x48…`), и одну память
     * на двух адресах даёт только общий файл под двумя
     * `MAP_SHARED`-отображениями.
     *
     * Без зеркала гость падает на собственных обращениях к `0x48…`:
     * измерено 2026-10-14 — цикл записи списка GE шёл по
     * `0x48BA0E80..0x48BA0E94` (vaddr 0x236D9C, 8 сбоев подряд), и
     * каждый пропуск инструкции терял запись. Из потерянных записей
     * вырос каскад из 66 сбоев и гибель ВСЕГО `shell` (status = 11).
     *
     * Отказ tmpfs не рушит загрузку: файл не открылся — отображаем
     * анонимно, как прежде, зеркала не будет, и гость будет
     * «спотыкаться» на `0x48…` по-старому. */
    /* Флаги — макросами <fcntl.h>, а не числом 0x0302: у MIPS значения
     * свои (O_CREAT 0x100, O_TRUNC 0x200, то есть 0x302 — числа
     * совпадают с прежними бит-в-бит), у хоста другие, и число без
     * O_CREAT на хосте НЕ создаёт файл. Измерено пробой
     * tmp/vpl_probe.c (2026-10-15): до правки хостовые тесты молча
     * не покрывали ни зеркало образа, ни VRAM. */
    mirror_fd = open("/tmp/.psp_guest_map", O_RDWR | O_CREAT | O_TRUNC,
                     0600);
    if (mirror_fd >= 0 && ftruncate(mirror_fd, (off_t)map_size) != 0) {
        close(mirror_fd);
        mirror_fd = -1;
    }
    if (mirror_fd >= 0) {
        p = mmap(want, map_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_SHARED | (int)m->map_flags, mirror_fd, 0);
    } else {
        p = mmap(want, map_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | (int)m->map_flags, -1, 0);
    }

    /* Без защиты от подмены адреса проверяем результат вручную: если
     * вернулся другой адрес, отображение уже чужое, и трогать его нельзя. */
    if (p == MAP_FAILED) {
        snprintf(m->error, sizeof(m->error),
                 "mmap 0x%08X не удался: %s", m->base, strerror(errno));
        if (mirror_fd >= 0)
            close(mirror_fd);
        return false;
    }
    if ((uintptr_t)p != m->base) {
        snprintf(m->error, sizeof(m->error),
                 "адрес 0x%08X занят, mmap вернул 0x%08X", m->base,
                 (unsigned)(uintptr_t)p);
        munmap(p, map_size);
        if (mirror_fd >= 0)
            close(mirror_fd);
        return false;
    }

    /* ЗЕРКАЛО БЕЗ КЭША (бит 0x40000000) — второе отображение того же
     * файла на `base + 0x40000000`. Отделяется СРАЗУ за основным,
     * пока дескриптор открыт: содержимое на этот момент нулевое, а
     * сегменты гостя лягут в файл позднее через основное отображение —
     * зеркало увидит их, это одна память.
     *
     * Право исполнения зеркалу НЕ даётся: гость в него только пишет и
     * читает. Если мусорный переход уведёт исполнение в зеркало,
     * процесс получит сбой по исполнению, а не выполнит данные как
     * код. По той же причине `mprotect` ниже трогает только основное
     * отображение: у зеркала нет кода.
     *
     * `MAP_FIXED` не используется: подсказанный адрес ядро отдаёт,
     * если он свободен. Занятость видна как чужой адрес в возврате —
     * тогда отображение снимается и зеркало просто отсутствует,
     * загрузка продолжается.
     *
     * Файл отвязывается сразу: отображения держат inode, и мусора в
     * tmpfs после выгрузки или гибели процесса не остаётся. */
    m->mirror_base = 0;
    if (mirror_fd >= 0) {
        void *q = mmap((void *)(uintptr_t)(m->base + 0x40000000u),
                       map_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, mirror_fd, 0);

        if (q != MAP_FAILED) {
            if ((uintptr_t)q == m->base + 0x40000000u)
                m->mirror_base = m->base + 0x40000000u;
            else
                munmap(q, map_size);   /* адрес занят — снимаем чужой */
        }
        close(mirror_fd);
        (void)unlink("/tmp/.psp_guest_map");
    }

    /* VRAM PSP — той же парой на отдельный tmpfs-файл: 2 МиБ по
     * 0x04000000 (кэшируемое) и 0x44000000 (зеркало без кэша).
     *
     * Игра рисует GE-списки в VRAM и обращается к нему любым зеркалом;
     * `sceGeEdramGetAddr` (0xE47E40E4) обязана вернуть 0x04000000,
     * иначе графический путь гостя разводится (измерено 2026-10-15:
     * прогон с возвратом 0 не дошёл ни до одного GE-списка).
     *
     * Отказ не рушит загрузку: vram_base=0, HLE вернёт 0, и гость
     * будет вести себя как раньше. Без исполнения — только RW. */
    m->vram_base = 0;
    m->vram_mirror = 0;
    {
        int vfd = open("/tmp/.psp_guest_vram", O_RDWR | O_CREAT | O_TRUNC,
                       0600);

        if (vfd >= 0 && ftruncate(vfd, (off_t)GUEST_VRAM_BYTES) != 0) {
            close(vfd);
            vfd = -1;
        }
        if (vfd >= 0) {
            void *v1 = mmap((void *)(uintptr_t)GUEST_VRAM_BASE,
                            GUEST_VRAM_BYTES, PROT_READ | PROT_WRITE,
                            MAP_SHARED, vfd, 0);
            void *v2;

            if (v1 == MAP_FAILED) {
                v1 = NULL;
            } else if ((uintptr_t)v1 != GUEST_VRAM_BASE) {
                munmap(v1, GUEST_VRAM_BYTES);
                v1 = NULL;
            }
            v2 = mmap((void *)(uintptr_t)GUEST_VRAM_MIRROR,
                      GUEST_VRAM_BYTES, PROT_READ | PROT_WRITE,
                      MAP_SHARED, vfd, 0);
            if (v2 == MAP_FAILED) {
                v2 = NULL;
            } else if ((uintptr_t)v2 != GUEST_VRAM_MIRROR) {
                munmap(v2, GUEST_VRAM_BYTES);
                v2 = NULL;
            }
            if (v1 != NULL) {
                m->vram_base = GUEST_VRAM_BASE;
                if (v2 != NULL)
                    m->vram_mirror = GUEST_VRAM_MIRROR;
            } else if (v2 != NULL) {
                munmap(v2, GUEST_VRAM_BYTES);
            }
            close(vfd);
            (void)unlink("/tmp/.psp_guest_vram");
        }
    }

    /* Куча VPL — за VRAM: не зависит от карты образа, только от
     * собственных фиксированных адресов PSP. */
    map_vpl(m);

    /* НУЛЕВАЯ СТРАНИЦА + СКРЕТЧПАД — регион [0x0, 0x14000), ОБА вида.
     *
     * Прогоны 39–45: цепочка NULL-указателей в потоке колбэков давится
     * на `strlen(NULL+8)` — отсюда читаемые нули в 0x0..0xFFFF.
     *
     * Замер 90 (652ee635) добавил вторую половину: uncached-вид
     * 0x4000xxxx НЕ был отображён, и побайтовая заливка региона
     * 0x40000000.. шла 65 537 фолтами — каждый байт обрабатывался
     * эмуляцией, а на границе скретчпада (0x40010000, вне «нулевой
     * страницы» обработчика) поток был убит SPINDEAD. На PSP это
     * настоящая память: скретчпад — 16 КиБ быстрой SRAM по 0x10000,
     * читаемая и записываемая через оба зеркала.
     *
     * Оба вида — из ОДНОГО tmpfs-файла: запись через кэшируемый вид
     * видна через uncached и наоборот, как на PSP. Право исполнения
     * не даётся — данных в этом регионе кода не бывает.
     *
     * Отказ любого вида не рушит загрузку: uncached-фолты в зоне
     * обрабатывает эмуляция нулевой страницы (как до этой правки). */
    m->null_mirror = 0;
    m->scr_base = 0;
    {
        enum { NULLREG_BYTES = 0x14000u };   /* 0x0..0x13FFF */
        int nfd = open("/tmp/.psp_guest_null", O_RDWR | O_CREAT | O_TRUNC,
                       0600);

        if (nfd >= 0 && ftruncate(nfd, (off_t)NULLREG_BYTES) != 0) {
            close(nfd);
            nfd = -1;
        }
        if (nfd >= 0) {
            void *u1 = mmap((void *)(uintptr_t)0x40000000u,
                            NULLREG_BYTES, PROT_READ | PROT_WRITE,
                            MAP_SHARED, nfd, 0);

            if (u1 == MAP_FAILED) {
                u1 = NULL;
            } else if ((uintptr_t)u1 != 0x40000000u) {
                munmap(u1, NULLREG_BYTES);
                u1 = NULL;
            }

            /* Кэшируемый вид — тем же файлом на адрес 0; при отказе
             * (mmap_min_addr) — хотя бы скретчпад 0x10000 со смещением
             * файла 0x10000; при полном отказе — прежний диагностический
             * аноним на адрес 0 без согласованности с uncached. */
            void *c1 = mmap((void *)(uintptr_t)0u, NULLREG_BYTES,
                            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                            nfd, 0);
            void *c2 = NULL;

            if (c1 == MAP_FAILED || (uintptr_t)c1 != 0u) {
                c1 = NULL;
                c2 = mmap((void *)(uintptr_t)0x10000u, 0x4000u,
                          PROT_READ | PROT_WRITE, MAP_SHARED, nfd,
                          (off_t)0x10000u);
                if (c2 == MAP_FAILED) {
                    c2 = NULL;
                } else if ((uintptr_t)c2 != 0x10000u) {
                    munmap(c2, 0x4000u);
                    c2 = NULL;
                }
                if (c2 == NULL && u1 == NULL) {
                    void *z = mmap((void *)(uintptr_t)0u, 0x10000u,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                                   -1, 0);
                    (void)(z == MAP_FAILED);
                }
            }

            if (u1 != NULL)
                m->null_mirror = 0x40000000u;
            if (c1 != NULL || c2 != NULL)
                m->scr_base = 0x10000u;

            close(nfd);
            (void)unlink("/tmp/.psp_guest_null");
        }
    }

    m->memory = p;
    m->map_size = map_size;
    m->tramp_base = (u32)((uintptr_t)p + ((m->span + 0xFFFu) & ~0xFFFu));
    m->tramp_next = m->tramp_base;
    /* Резерв под разделы — сразу за пулом трамплинов и внутри того же
     * отображения. `memset` не нужен: страницы и так анонимные и нулевые,
     * а лишняя запись на 1 МиБ при старте ничего не добавляет. */
    m->part_base = m->tramp_base + GUEST_TRAMP_SIZE;

    /* С данных снимается ПРАВО ВЫПОЛНЕНИЯ.
     *
     * Зачем. Отображение гостя одно и помещается в RWX, а кроме кода там
     * лежат `.data` (730 КБ) и `.bss`. Пока данные исполнимы, гость может
     * прыгнуть туда и выполнить их как код — и это происходит молча: симптомы
     * в том, что регистры, которые не пишет ни одна инструкция `.text`
     * (проверено инструментом по обеим исполняемым секциям), всё равно
     * меняются, а прогоны недетерминированы.
     *
     * Право исполнения нужно только `.text` и `.sceStub.text` (стабы) и пулу
     * трамплинов, который лежит после образа. Данные же не исполняются
     * никогда — на настоящей PSP страницы кода и данных разделены.
     *
     * Проверка возврата обязательна: если `mprotect` не сработал, писать об
     * этом надо, а не молча полагать, что защита есть. */
    data_off = data_section_off(m);
    if (data_off > 0 && (size_t)data_off < map_size) {
        /* Адрес и длина выравниваются по странице САМИ: `mprotect` требует
         * выровненный адрес, и без этого возвращает EINVAL. На MIPS страница
         * 16 КиБ, на хосте 4 КиБ — берём размер у ядра, а не задаём руками. */
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        size_t start = ((size_t)data_off / page) * page;

        if (page == 0)
            page = 4096;
        if (mprotect((u8 *)p + start, map_size - start,
                     PROT_READ | PROT_WRITE) != 0) {
            snprintf(m->error, sizeof(m->error),
                     "mprotect 0x%08X+%lu на данные не удался: %s",
                     (unsigned)((uintptr_t)p + start),
                     (unsigned long)(map_size - start), strerror(errno));
            return false;
        }
        m->data_protected = 1;
        m->data_off = (u32)start;
        data_off = (u32)start;
    }
    m->data_off = data_off;
    return true;
}

static bool load_segments(GuestModule *m)
{
    const u8 *d = m->image;
    u16 phnum = rd16(d + 44);
    u32 phoff = rd32(d + 28);
    u32 phentsize = rd16(d + 42);
    u16 i;

    m->seg_count = 0;

    for (i = 0; i < phnum; i++) {
        u32 off = phoff + (u32)i * phentsize;
        u32 p_type, p_offset, p_vaddr, p_filesz, p_memsz, dst;

        if ((u64)off + 32 > (u64)m->image_size)
            return false;

        p_type = rd32(d + off);
        p_offset = rd32(d + off + 4);
        p_vaddr = rd32(d + off + 8);
        p_filesz = rd32(d + off + 16);
        p_memsz = rd32(d + off + 20);

        if (p_type != 1)
            continue;
        if (p_filesz > p_memsz) {
            snprintf(m->error, sizeof(m->error),
                     "сегмент %u: filesz %u > memsz %u", i, p_filesz, p_memsz);
            return false;
        }
        if (p_memsz == 0)
            continue;
        if ((u64)p_offset + p_filesz > (u64)m->image_size) {
            snprintf(m->error, sizeof(m->error),
                     "сегмент %u читает за пределами файла", i);
            return false;
        }

        dst = m->base + p_vaddr;
        if ((u64)dst + p_memsz > (u64)m->base + m->map_size) {
            snprintf(m->error, sizeof(m->error),
                     "сегмент %u не помещается в отображение: 0x%08X+0x%X",
                     i, dst, p_memsz);
            return false;
        }

        memcpy((void *)(uintptr_t)dst, d + p_offset, p_filesz);
        if (p_memsz > p_filesz)
            memset((void *)(uintptr_t)(dst + p_filesz), 0, p_memsz - p_filesz);

        if (m->seg_count < GUEST_MAX_SEGMENTS) {
            m->segments[m->seg_count].vaddr = p_vaddr;
            m->segments[m->seg_count].memsz = p_memsz;
            m->segments[m->seg_count].runtime = dst;
            m->seg_count++;
        }
    }

    if (m->seg_count == 0) {
        snprintf(m->error, sizeof(m->error), "не загружен ни один сегмент");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Релокации                                                                  */
/* ------------------------------------------------------------------------- */

/* Пересчёт HI16 по паре HI16/LO16.
 *
 * ФОРМУЛА И ПРИЧИНА ЕЁ ПЕРЕСМОТРА — в `include/guest_reloc.h`, там же
 * замер, который прежнюю опроверг. Коротко: прежний вариант опускал
 * младшую половину адреса (`corrected = value - lo_signed` сводил всё к
 * `hi16_адденда + S`), и 5908 указателей из 24571 пары HI16/LO16 выходили
 * на 64 КиБ ниже намеренного адреса. На первом же конструкторе `.cplinit`
 * гость читал по смещённому адресу слово `0xFFFFFFFF` и разыменовал его.
 *
 * Функция вынесена в заголовок, чтобы гейт проверял ЭТОТ код, а не
 * отдельную копию модели. */
static u32 relocate_hi(u32 op, s32 lo_signed, u32 target)
{
    return guest_relocate_hi(op, lo_signed, target);
}

static bool apply_section_relocs(GuestModule *m, u32 sh_off, u32 sh_size,
                                 u32 sh_info)
{
    const u8 *d = m->image;
    u32 count = sh_size / 8;
    u32 *orig;          /* исходные слова: LO16 надо читать до правок */
    u8   *valid;
    u32 k;
    bool ok = true;

    if (count == 0)
        return true;

    orig = calloc(count, sizeof(u32));
    valid = calloc(count, 1);
    if (orig == NULL || valid == NULL) {
        free(orig);
        free(valid);
        snprintf(m->error, sizeof(m->error), "нет памяти под релокации");
        return false;
    }

    /* Первый проход: проверить адреса и запомнить исходные слова.
     * Без этого пара HI16/LO16 читала бы уже исправленное значение. */
    for (k = 0; k < count; k++) {
        u32 r = sh_off + k * 8;
        u32 info = rd32(d + r + 4);
        u32 type = info & 0x0Fu;
        u32 patch_seg = (info >> 8) & 0xFFu;
        u32 rel = rd32(d + r);
        u32 patch_addr;

        if (patch_seg >= m->seg_count) {
            valid[k] = 0;
            m->reloc_bad_segment++;
            continue;
        }
        patch_addr = m->segments[patch_seg].runtime + rel;
        if ((u64)patch_addr + 4 > (u64)m->base + m->map_size) {
            valid[k] = 0;
            m->reloc_bad_segment++;
            continue;
        }
        if (((patch_addr & 3u) != 0) && (type != GUEST_R_MIPS_32)) {
            valid[k] = 0;
            snprintf(m->error, sizeof(m->error),
                     "невыровненный патч релокации 0x%08X", patch_addr);
            ok = false;
            break;
        }
        orig[k] = host_at(patch_addr);
        valid[k] = 1;
    }

    /* Второй проход: применить. */
    if (ok) {
        for (k = 0; k < count; k++) {
            u32 r = sh_off + k * 8;
            u32 info = rd32(d + r + 4);
            u32 type = info & 0x0Fu;
            u32 patch_seg = (info >> 8) & 0xFFu;
            u32 target_seg = (info >> 16) & 0xFFu;
            u32 rel, patch_addr, target_addr, op;

            m->reloc_total++;
            if (!valid[k])
                continue;
            if (target_seg >= m->seg_count) {
                m->reloc_bad_segment++;
                continue;
            }

            rel = rd32(d + r);
            patch_addr = m->segments[patch_seg].runtime + rel;
            target_addr = m->segments[target_seg].runtime;
            op = orig[k];

            switch (type) {
            case GUEST_R_MIPS_32:
                op += target_addr;
                break;

            case GUEST_R_MIPS_26:
                op = (op & 0xFC000000u) |
                     (((op & 0x03FFFFFFu) + (target_addr >> 2)) &
                      0x03FFFFFFu);
                break;

            case GUEST_R_MIPS_HI16: {
                bool paired = false;
                u32 identity = info >> 8;
                u32 j;
                for (j = k + 1; j < count; j++) {
                    u32 next_info = rd32(d + sh_off + j * 8 + 4);
                    u32 next_type = next_info & 0x0Fu;
                    if (next_type == GUEST_R_MIPS_HI16)
                        continue;
                    if (next_type != GUEST_R_MIPS_LO16)
                        continue;
                    if ((next_info >> 8) != identity)
                        continue;
                    if (!valid[j])
                        continue;
                    {
                        u32 lo_tgt = (next_info >> 16) & 0xFFu;
                        s32 lo_signed;
                        if (lo_tgt >= m->seg_count)
                            break;
                        lo_signed = (s32)(s16)(u16)(orig[j] & 0xFFFFu);
                        op = relocate_hi(op, lo_signed,
                                         m->segments[lo_tgt].runtime);
                        paired = true;
                    }
                    break;
                }
                if (!paired) {
                    m->reloc_failed++;
                    continue;
                }
                break;
            }

            case GUEST_R_MIPS_LO16:
                op = (op & 0xFFFF0000u) | ((op + target_addr) & 0xFFFFu);
                break;

            default:
                m->reloc_unsupported++;
                continue;
            }

            wr32((u8 *)(uintptr_t)patch_addr, op);
            m->reloc_applied++;
        }
    }

    free(orig);
    free(valid);
    (void)sh_info;
    return ok;
}

static bool apply_relocations(GuestModule *m)
{
    const u8 *d = m->image;
    u16 shnum = rd16(d + 48);
    u16 shentsize = rd16(d + 46);
    u32 shoff = rd32(d + 32);
    u16 i;

    for (i = 0; i < shnum; i++) {
        u32 sec = shoff + (u32)i * shentsize;
        u32 sh_type, sh_offset, sh_size, sh_info, sh_entsize;

        if ((u64)sec + 40 > (u64)m->image_size) {
            snprintf(m->error, sizeof(m->error),
                     "заголовок секции %u вне файла", i);
            return false;
        }

        sh_type = rd32(d + sec + 4);
        if (sh_type != GUEST_SHT_REL)
            continue;

        sh_offset = rd32(d + sec + 16);
        sh_size = rd32(d + sec + 20);
        sh_info = rd32(d + sec + 28);
        sh_entsize = rd32(d + sec + 36);

        if (sh_entsize != 8) {
            snprintf(m->error, sizeof(m->error),
                     "секция релокаций: размер записи %u, ожидался 8",
                     sh_entsize);
            return false;
        }
        if ((u64)sh_offset + sh_size > (u64)m->image_size) {
            snprintf(m->error, sizeof(m->error),
                     "секция релокаций читает за пределами файла");
            return false;
        }
        if (!apply_section_relocs(m, sh_offset, sh_size, sh_info))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */

void guest_flush_icache(void *addr, size_t len)
{
    __builtin___clear_cache((char *)addr, (char *)addr + len);
}

/* Перевести vaddr модуля в адрес в памяти. */
bool guest_runtime_addr(const GuestModule *m, u32 vaddr, u32 *out)
{
    u16 i;

    for (i = 0; i < m->seg_count; i++) {
        if (vaddr < m->segments[i].vaddr)
            continue;
        if (vaddr >= m->segments[i].vaddr + m->segments[i].memsz)
            continue;
        *out = m->segments[i].runtime + (vaddr - m->segments[i].vaddr);
        return true;
    }
    return false;
}

/* Найти `.cplinit` ПОСЛЕ релокаций: только теперь указатели в ней
 * абсолютные. Размер секции — 1296 байт, указатель идёт через слово,
 * поэтому берём каждое второе слово. */
static void find_cplinit(GuestModule *m)
{
    u32 vaddr = 0, size = 0, rt = 0;

    m->cplinit_runtime = 0;
    m->cplinit_count   = 0;

    if (!guest_find_section(m, ".cplinit", &vaddr, &size, NULL))
        return;
    if (!guest_runtime_addr(m, vaddr, &rt))
        return;

    m->cplinit_runtime = rt;
    m->cplinit_count   = size / 8;     /* указатель + слово-разделитель */
}

/* Заменить инструкции, которых нет в CPU плеера, — ДО релокаций.
 *
 * Часть 1: `cache` (опкод 0x2F) → NOP.
 *
 * Измерено 2026-10-14: процессор плеера не выполняет cache-операцию
 * 0x1B и поднимает SIGILL. У гостя такая инструкция РОВНО ОДНА —
 * vaddr 0x237070, обычный PSP-код чистки кэша после загрузки модулей.
 *
 * NOP вместо `cache` безопасен по ISA: `cache` управляет только
 * состоянием кэша и не меняет архитектурных результатов.
 *
 * Часть 2: 64-битные операции MIPS64 → их 32-битные эквиваленты.
 *
 * Измерено 2026-10-14 (прогон `34593d8a`): SIGILL в инициализации
 * кучи на `dadd` (SPECIAL funct 0x2C) — objdump тулчейна опознал и
 * его, и остальных; счёт по образу:
 *
 *   dadd   (0x2C)  40   daddu (0x2D)  86   dsub/dsubu (0x2E/2F)  0
 *   dmult  (0x1C)  12   dsrlv (0x16)  51
 *
 * PSP-тулчейн эмитит их свободно; у 32-битного CPU плеера их нет.
 * Переписываются в 32-битные эквиваленты, и это ТОЧНО, а не «почти»:
 *
 *   dadd/daddu/dsub/dsubu → addu/subu: регистры гостя 32-битные,
 *     младшие 32 бита 64-битной суммы/разности равны 32-битной
 *     операции, а старшие половины в образе НИКТО не читает —
 *     64-битных сдвигов и пересылок (dsll/dsrl/dmfc1/ldc1) в `.text`
 *     нет, проверено полным проходом;
 *   dmult/dmultu → mult/multu: произведение двух знаковых 32-битных
 *     значений в HI:LO у 32-битной и 64-битной операций ОДИНАКОВО
 *     по определению — DMULT отличается только при ненулевых
 *     старших половинах операндов, которых у 32-битных регистров нет;
 *   ddiv/ddivu → div/divu: частное знаковых 32-битных значений
 *     помещается в 32 бита во всех случаях, кроме углового
 *     (-2^31)/(-1), которого в игровом коде нет;
 *   dsrlv X, $zero, Y → `addiu X, $zero, 0`: ВСЕ 51 срабатывание
 *     сдвигают РОВНО $zero (проверено полным проходом по операндам),
 *     результат любого логического сдвига нуля — ноль.
 *
 * Правится именно ДО релокаций: после них слово могло оказаться
 * релоцированным указателем, и перепись испортила бы данные. До
 * релокаций ложных срабатываний нет — на все найденные слова
 * релокаций ноль (проверено по `.rel.text`).
 *
 * Полный сброс icache в `guest_load` идёт ПОСЛЕ релокаций — отдельного
 * сброса здесь не нужно. */
static void patch_unsupported_ops(GuestModule *m)
{
	/* Только `.text`. `.cplinit` сюда НЕЛЬЗЯ: это таблица УКАЗАТЕЛЕЙ
	 * на конструкторы, а не код (в `find_cplinit` прямо написано:
	 * указатель идёт через слово), и перепись «похожего» слова там
	 * калечила указатели. Измерено 2026-10-15: проход по `.cplinit`
	 * испортил 16 указателей, конструкторы прыгали на невыровненные
	 * адреса, и прогон умер на 129 SIGBUS с pc вида `0x08810563`
	 * (`patch_64` вышел 205 вместо 189 — лишние 16 и есть те слова). */
	static const char *const code_secs[] = { ".text" };
	u32 i;

	m->cache_nops = 0;
	m->patch_64 = 0;
	for (i = 0; i < (u32)(sizeof(code_secs) / sizeof(code_secs[0])); i++) {
		u32 vaddr, size, rt, k;

		if (!guest_find_section(m, code_secs[i], &vaddr, &size, NULL))
			continue;
		if (!guest_runtime_addr(m, vaddr, &rt))
			continue;
		for (k = 0; k + 4 <= size; k += 4) {
			u32 *p = (u32 *)(uintptr_t)(rt + k);
			u32 x = *p;
			u32 op = x >> 26;
			u32 fn = x & 0x3Fu;

			if (op == 0x2Fu) {              /* cache */
				*p = 0;
				m->cache_nops++;
				continue;
			}
			if (op != 0u)
				continue;

			/* 64-битные SPECIAL. Регистровые поля НЕ меняются —
			 * у всех заменяемых операций кодировка полей та же,
			 * кроме dsrlv (см. ниже). */
			if (fn == 0x2Cu || fn == 0x2Du) {       /* dadd/daddu */
				*p = (x & ~0x3Fu) | 0x21u;   /* addu  */
				m->patch_64++;
			} else if (fn == 0x2Eu || fn == 0x2Fu) { /* dsub/dsubu */
				*p = (x & ~0x3Fu) | 0x23u;   /* subu  */
				m->patch_64++;
			} else if (fn == 0x1Cu || fn == 0x1Du) { /* dmult/dmultu */
				*p = (x & ~0x3Fu) | 0x18u;   /* mult  */
				m->patch_64++;
			} else if (fn == 0x1Eu || fn == 0x1Fu) { /* ddiv/ddivu */
				*p = (x & ~0x3Fu) | 0x1Au;   /* div   */
				m->patch_64++;
			} else if (fn == 0x16u) {               /* dsrlv */
				/* Только вырожденная форма — сдвиг $zero; её и
				 * искали: все 51 срабатывания в образе такие. */
				if (((x >> 16) & 0x1Fu) == 0u
				    && ((x >> 6) & 0x1Fu) == 0u) {
					u32 rd = (x >> 11) & 0x1Fu;

					/* addiu rd, $zero, 0 */
					*p = 0x24000000u | (rd << 16);
					m->patch_64++;
				}
			}
		}
	}
}

bool guest_load(GuestModule *m, const u8 *image, size_t image_size,
                u32 base_override)
{
    u32 map_flags_in;

    if (m == NULL || image == NULL)
        return false;

    /* `map_flags` — единственное поле, которое переживает обнуление: его
     * задаёт вызывающий ДО вызова, и обычный `memset` его стирал, из-за чего
     * `MAP_FIXED` молча не применялся, а отчёт писал «адрес занят» при
     * свободном адресе. Сохраняем и восстанавливаем явно.
     *
     * Отдельный параметр функции вместо этого потребовал бы править шесть
     * мест вызова ради одного необязательного флага. */
    map_flags_in = m->map_flags;

    memset(m, 0, sizeof(*m));
    m->map_flags = map_flags_in;
    m->image = image;
    m->image_size = image_size;

    if (image_size < 52 || memcmp(image, "\x7f" "ELF", 4) != 0 ||
        image[4] != 1 || image[5] != 1) {
        snprintf(m->error, sizeof(m->error), "вход не является ELF32 LE");
        return false;
    }

    m->entry = rd32(image + 24);
    m->e_type = rd16(image + 16);

    if (!compute_span(m))
        return false;

    m->base = (base_override != 0) ? base_override : PSP_USER_BASE;

    if (!map_guest(m))
        return false;
    if (!load_segments(m))
        return false;

    /* Инструкции без поддержки CPU → NOP/32-битные эквиваленты — до
     * релокаций и до всякого исполнения. Причины и доказательства
     * точности — у самой функции. */
    patch_unsupported_ops(m);

    /* Сведения о модуле читаются здесь, до релокаций: после них поля
     * gp/ent_top/ent_end/stub_top/stub_end уже станут абсолютными. */
    m->has_info = guest_read_module_info(m, &m->info);

    if (!apply_relocations(m))
        return false;

    /* Обязательный шаг: без сброса кэша процессор исполнит мусор. */
    guest_flush_icache(m->memory, m->map_size);

    find_cplinit(m);

    m->loaded = true;
    return true;
}

/* Найти строку по имени среди имён секций. */
bool guest_find_section(const GuestModule *m, const char *want, u32 *vaddr,
                        u32 *size, u32 *type)
{
    const u8 *d = m->image;
    u16 shnum = rd16(d + 48);
    u16 shentsize = rd16(d + 46);
    u32 shoff = rd32(d + 32);
    u16 shstrndx = rd16(d + 50);
    u32 strtab_sec = shoff + (u32)shstrndx * shentsize;
    u32 strtab_off;
    u16 i;

    if ((u64)strtab_sec + 40 > (u64)m->image_size)
        return false;
    strtab_off = rd32(d + strtab_sec + 16);

    for (i = 0; i < shnum; i++) {
        u32 sec = shoff + (u32)i * shentsize;
        u32 name_off, name_at;

        if ((u64)sec + 40 > (u64)m->image_size)
            return false;
        name_off = rd32(d + sec);
        name_at = strtab_off + name_off;
        if (name_at >= m->image_size)
            continue;
        if (strncmp((const char *)(d + name_at), want, strlen(want)) != 0)
            continue;

        if (vaddr)
            *vaddr = rd32(d + sec + 12);
        if (size)
            *size = rd32(d + sec + 20);
        if (type)
            *type = rd32(d + sec + 4);
        return true;
    }
    return false;
}

/* Чтение сведений о модуле из загруженного образа, до релокаций. */
static bool read_module_info_raw(GuestModule *m, GuestModuleInfo *out)
{
    u32 vaddr = 0, size = 0;
    const u8 *mi;

    if (out == NULL)
        return false;
    if (!guest_find_section(m, ".rodata.sceModuleInfo", &vaddr, &size, NULL))
        return false;
    if (size < 52 || (u64)vaddr + 52 > (u64)m->span)
        return false;

    mi = (const u8 *)(uintptr_t)(m->base + vaddr);
    memset(out, 0, sizeof(*out));
    memcpy(out->name, mi + 4, 28);
    out->name[28] = '\0';
    out->gp       = rd32(mi + 32);
    out->ent_top  = rd32(mi + 36);
    out->ent_end  = rd32(mi + 40);
    out->stub_top = rd32(mi + 44);
    out->stub_end = rd32(mi + 48);
    return true;
}

/* Отдаёт сведения о модуле, снятые при загрузке до релокаций.
 *
 * После применения релокаций поля gp/ent_top/ent_end/stub_top/stub_end
 * содержат уже абсолютные адреса в памяти, поэтому повторное прибавление
 * базы дало бы адрес за пределами образа. Отсюда хранение снимка. */
bool guest_read_module_info(GuestModule *m, GuestModuleInfo *out)
{
    if (m == NULL || out == NULL)
        return false;
    if (m->has_info) {
        *out = m->info;
        return true;
    }
    return read_module_info_raw(m, out);
}

bool guest_scan_imports(GuestModule *m, const GuestModuleInfo *info,
                        GuestImportTable *out)
{
    u32 cursor, end;

    if (!m->loaded || info == NULL || out == NULL)
        return false;

    memset(out, 0, sizeof(*out));
    if (info->stub_end <= info->stub_top)
        return false;
    if ((u64)info->stub_end > (u64)m->span)
        return false;

    cursor = m->base + info->stub_top;
    end = m->base + info->stub_end;
    out->libs = calloc(64, sizeof(GuestLibrary));
    if (out->libs == NULL) {
        snprintf(m->error, sizeof(m->error), "нет памяти под список библиотек");
        return false;
    }

    while (cursor < end && out->count < 64) {
        u32 libname, length_words, count, nid_table, stub_table, step;
        const char *lib;
        GuestLibrary *g;
        u32 i;

        if ((u64)(cursor - m->base) + 20 > (u64)m->span)
            break;

        libname      = rd32((const u8 *)(uintptr_t)cursor);
        length_words = host_at(cursor + 8) & 0xFFu;
        count        = rd16((const u8 *)(uintptr_t)(cursor + 10));
        nid_table    = rd32((const u8 *)(uintptr_t)(cursor + 12));
        stub_table   = rd32((const u8 *)(uintptr_t)(cursor + 16));

        /* Здесь указатели уже абсолютные.
         *
         * Таблица стабов лежит в секции .lib.stub, а на неё есть ровно
         * 18x3 = 54 релокации — по одной на каждый из трёх указателей в
         * записи (имя библиотеки, таблица NID, таблица стабов). После
         * применения релокаций все три уже содержат адреса в памяти, и
         * прибавление базы второй раз уводит их за пределы отображения.
         * Проверено на настоящем модуле: 54 релокации при 18 библиотеках.
         *
         * Границы образа берём из отображения, а не из info->stub_* : те
         * два поля приходят из снимка, снятого ДО релокаций, то есть это
         * виртуальные адреса, и они уже переведены в cursor/end выше. */
        if (libname < m->base || libname + 32 > m->base + m->map_size ||
            nid_table < m->base ||
            nid_table + 4 * (count ? count : 1) > m->base + m->map_size ||
            stub_table < m->base ||
            stub_table + 8 * (count ? count : 1) > m->base + m->map_size ||
            count > 4096) {
            snprintf(m->error, sizeof(m->error),
                     "запись импортов выглядит неверно: name=0x%08X "
                     "nid=0x%08X stub=0x%08X count=%u",
                     libname, nid_table, stub_table, count);
            return false;
        }

        lib = (const char *)(uintptr_t)libname;
        g = &out->libs[out->count];
        memset(g, 0, sizeof(*g));
        strncpy(g->library, lib, sizeof(g->library) - 1);

        g->imports = calloc(count ? count : 1, sizeof(GuestImport));
        if (g->imports == NULL) {
            snprintf(m->error, sizeof(m->error), "нет памяти под импорты");
            return false;
        }

        for (i = 0; i < count; i++) {
            g->imports[i].nid =
                rd32((const u8 *)(uintptr_t)(nid_table + 4 * i));
            g->imports[i].stub_runtime = stub_table + 8 * i;
        }
        g->count = count;
        out->total_imports += count;
        out->count++;

        step = (length_words * 4u > 20u) ? length_words * 4u : 20u;
        step = (step + 3u) & ~3u;
        cursor += step;
    }

    return out->count > 0;
}

void guest_free_imports(GuestImportTable *t)
{
    u32 i;
    if (t == NULL || t->libs == NULL)
        return;
    for (i = 0; i < t->count; i++)
        free(t->libs[i].imports);
    free(t->libs);
    t->libs = NULL;
    t->count = 0;
}

/* ------------------------------------------------------------------------- */
/* Подмена стабов                                                              */
/* ------------------------------------------------------------------------- */

bool guest_patch_stub(GuestModule *m, GuestStubPatch *p)
{
    u8 *stub;

    if (m == NULL || p == NULL || p->stub == 0 || p->target == 0)
        return false;

    stub = (u8 *)(uintptr_t)p->stub;
    p->via_trampoline = false;
    p->trampoline = 0;

    /* Прямой прыжок годится только внутри одного 256 МБ сегмента: у j в
     * непосредственном операнде 24 бита цели, а старшую четверть процессор
     * берёт из $pc. */
    if (((p->target ^ p->stub) & 0xF0000000u) == 0) {
        u32 imm = (p->target & 0x0FFFFFFCu) >> 2;
        wr32(stub, 0x08000000u | (imm & 0x03FFFFFFu));
        wr32(stub + 4, 0x00000000u);   /* nop в слоте задержки */
        guest_flush_icache(stub, 8);
        return true;
    }

    /* Разные сегменты. Стаб прыгает на трамплин, который лежит в том же
     * сегменте, а трамплин переходит по полному 32-битному адресу через
     * регистр $t9. Трамплин берётся из пула внутри отображения гостя:
     * писать исполняемый код поверх гостевых данных нельзя. */
    if (m->tramp_base == 0) {
        snprintf(m->error, sizeof(m->error), "пул трамплинов не создан");
        return false;
    }
    if ((u64)m->tramp_next + 16 > (u64)m->tramp_base + GUEST_TRAMP_SIZE) {
        snprintf(m->error, sizeof(m->error), "пул трамплинов исчерпан");
        return false;
    }

    {
        u32 tr = m->tramp_next;
        u32 hi = (p->target + 0x8000u) >> 16;
        u32 lo = p->target & 0xFFFFu;
        u8 *t = (u8 *)(uintptr_t)tr;
        u32 imm;

        /* Трамплин: lui $t9,hi ; jr $t9 ; addiu $t9,$t9,lo
         * addiu стоит в слоте задержки jr, поэтому полный адрес успевает
         * собраться до перехода. Трамплин принадлежит нам, поэтому его длина
         * не ограничена. */
        wr32(t + 0, 0x3C190000u | (hi & 0xFFFFu));
        wr32(t + 4, 0x03200009u);
        wr32(t + 8, 0x27390000u | lo);
        wr32(t + 12, 0x00000000u);

        /* Стаб: j трамплин ; nop — ровно 8 байт, как и положено слоту.
         *
         * Здесь принципиально НЕ lui + jr + nop: слот стаба шириной 8 байт,
         * и запись двенадцати байтов затирала начало СЛЕДУЮЩЕГО стаба, то
         * есть соседнего импорта. Гость звал испорченный стаб и получал мусор.
         *
         * Прыжок `j` здесь допустим: трамплин лежит в том же отображении, а
         * значит в том же 256 МБ сегменте, что и сам стаб. Полный 32-битный
         * адрес цели на этом участке не нужен — он собирается в трамплине. */
        imm = (tr & 0x0FFFFFFCu) >> 2;
        wr32(stub + 0, 0x08000000u | (imm & 0x03FFFFFFu));
        wr32(stub + 4, 0x00000000u);

        m->tramp_next = tr + 16;
        p->via_trampoline = true;
        p->trampoline = tr;

        guest_flush_icache(t, 16);
        guest_flush_icache(stub, 8);
        return true;
    }
}

/* ------------------------------------------------------------------------- */
/* Трамплин вызова импорта                                                     */
/* ------------------------------------------------------------------------- */

/* Коды инструкций MIPS, собираемых здесь вручную. Формат: opcode в старших
 * 6 битах, rs в 25..21, rt в 20..16, rd в 15..11, shamt 10..6, funct 5..0.
 *
 * ВАЖНО: в `I_SW_SP`/`I_LW_SP` поле rs кодируется обязательно. Макросы
 * без rs дали `sw $31, 0($zero)` — запись по адресу НОЛЬ, потому что
 * отсутствующее поле не заполнялось нулём само по себе, а получалось
 * `rs = 0`. Это обнаружено выгрузкой трамплина с плеера и разбором, а не
 * чтением исходника. */
/* Загрузка и сохранение идут через `$sp`, поэтому rs равен R_SP. Промежуточные
 * макросы I_RS/I_SW/I_LW были написаны так, будто rs задаётся: rs в них не
 * кодировался вовсе, и получалось `sw $31, 0($zero)` — запись по адресу НОЛЬ.
 * Проверка на плеере: выгруженный трамплин показывал `sw $31,0($0)`. */
#define I_SW_SP(rt, off)    ((0x2Bu << 26) | ((u32)R_SP << 21) | ((u32)(rt) << 16) \
                            | ((u32)(off) & 0xFFFFu))
#define I_LW_SP(rt, off)    ((0x23u << 26) | ((u32)R_SP << 21) | ((u32)(rt) << 16) \
                            | ((u32)(off) & 0xFFFFu))
#define I_ADDIU(rt, rs, imm) ((0x09u << 26) | ((u32)(rs) << 21) | ((u32)(rt) << 16) \
                              | ((u32)(imm) & 0xFFFFu))
#define I_LUI(rt, imm)      ((0x0Fu << 26) | ((u32)(rt) << 16) | ((u32)(imm) & 0xFFFFu))
#define I_ORI(rt, rs, imm)  ((0x0Du << 26) | ((u32)(rs) << 21) | ((u32)(rt) << 16) \
                             | ((u32)(imm) & 0xFFFFu))
/* Запись по произвольному регистру базы: метка возврата пишется по адресу из
 * `$t0`, а не по `$sp`, и раньше такого макроса не было. */
#define I_SW(rt, rs, off)   ((0x2Bu << 26) | ((u32)(rs) << 21) | ((u32)(rt) << 16) \
                             | ((u32)(off) & 0xFFFFu))
#define I_MOVE(rd, rs)      ((u32)(rs) << 21 | (u32)(rd) << 11 | 0x21u)
#define I_JALR(rs)          ((u32)(rs) << 21 | 31u << 11 | 0x09u)
#define I_JR(rs)            ((u32)(rs) << 21 | 0x08u)
#define I_NOP               0x00000000u

/* Номера регистров. */
#define R_ZERO 0
#define R_V1   3
#define R_A0   4
#define R_A1   5
#define R_A2   6
#define R_A3   7
#define R_SP   29
#define R_T0   8
#define R_T1   9
#define R_T2   10
#define R_T8   24
#define R_T9   25
#define R_GP   28
#define R_K0   26
#define R_K1   27
#define R_RA   31

bool guest_build_import_trampoline(GuestModule *m, GuestStubPatch *p,
                                   u32 host_fn, u32 host_gp, u32 index,
                                   u32 host_trap, u32 host_k0)
{
    u8 *t;
    u32 tr, imm;
    u32 fn_hi, fn_lo, gp_hi, gp_lo, trg_hi, trg_lo;
    u32 k0_hi, k0_lo;

    if (m == NULL || p == NULL || p->stub == 0 || host_fn == 0)
        return false;
    if (m->tramp_base == 0) {
        snprintf(m->error, sizeof(m->error), "пул трамплинов не создан");
        return false;
    }
    if ((u64)m->tramp_next + GUEST_TRAMP_BYTES >
        (u64)m->tramp_base + GUEST_TRAMP_SIZE) {
        snprintf(m->error, sizeof(m->error), "пул трамплинов исчерпан");
        return false;
    }

    tr = m->tramp_next;
    t = (u8 *)(uintptr_t)tr;
    /* Половины адреса собираются `addiu`, а НЕ `ori`.
     *
     * Формула `(v + 0x8000) >> 16` и `v & 0xFFFF` дают пару, которую
     * складывает `addiu`: его непосредственный операнд расширяется знаком, и
     * при `lo >= 0x8000` лишние `0x10000` съедаются. У `ori` расширения НЕТ,
     * `ori` только затирает младшие биты, и тот же самый расчёт даёт адрес,
     * завышенный на `0x10000`.
     *
     * Замерено на плеере: `hle_import` = `0x2AAAB3C4`, в трамплин уходило
     * `0x2AABB3C4`, то есть вызов уезжал на `0x10000` выше — в `.rodata`, где
     * по этому адресу лежит не код. Отсюда и был `SIGILL`, и «ноль
     * обработанных импортов»: первый же вызов уходил не туда.
     *
     * Второй парой собирается `$gp`, и ошибка там выглядела бы так же. */
    fn_hi = (host_fn + 0x8000u) >> 16;
    fn_lo = host_fn & 0xFFFFu;
    gp_hi = (host_gp + 0x8000u) >> 16;
    gp_lo = host_gp & 0xFFFFu;
    /* Без округления `+ 0x8000`: половины складываются через `ori`, у которого
     * НЕТ знакового расширения, и округление завысило бы адрес на `0x10000`.
     * Это ровно тот дефект, что был с `fn`/`$gp`; здесь он повторён по невниманию
     * и приводил к тому, что метка возврата писалась не туда и не появлялась
     * вовсе: замер «ни один трамплин не вернулся» при семи обработанных
     * вызовах. */
    trg_hi = host_trap >> 16;
    trg_lo = host_trap & 0xFFFFu;

    /* Адрес `SceThreadContext`, который трамплин УСТАНАВЛИВАЕТ в `$k0` при
     * каждом возврате в гостя. Половины — через `addiu`, как у `$fn` и `$gp`
     * выше: `ori` не расширяет операнд знаком и уводит адрес на `0x10000`.
     *
     * Проба с подстановкой заведомо известного адреса ПРОВЕДЕНА и дала ответ:
     * `si_addr` остался нулём, то есть запись идёт не через наш `$k0`.
     * Восстановлен настоящий `host_k0`. */
    k0_hi = (host_k0 + 0x8000u) >> 16;
    k0_lo = host_k0 & 0xFFFFu;

    /* Кадр на гостевом стеке: 64 байта. Всё, что трамплин меняет, кроме `$v0`
     * (это результат импорта), кладётся туда и возвращается обратно.
     *
     * Сохраняются `$ra`, `$a0`..`$a3`, `$t8`, `$t9`, `$k0`, `$k1` и три
     * аргумента из стека вызова гостя. `$k0`/`$k1` — не ради вежливости:
     * именно на них ломается этот гость, см. объявление функции.
     *
     * Смещения стековых аргументов: кадр уменьшает `$sp` на 64, поэтому
     * `16` байт от `$sp` гостя оказываются на `80` от нового. */
    wr32(t + 0,  I_ADDIU(R_SP, R_SP, -GUEST_TRAMP_FRAME));
    wr32(t + 4,  I_SW_SP(R_RA, 0));
    wr32(t + 8,  I_SW_SP(R_A0, 8));
    /* `$a1` лежит в своём слоте, а не вместо `$a2`.
     *
     * Слота для него не было, и в `+12` клалась `$a2` — подмена была
     * оставлена как диагностика после того, как замер дал противоречие:
     * статический разбор кода гостя (выгрузка загруженного образа плюс
     * декомпиляция в Ghidra) показывал в `$a1` адрес входа потока
     * `0x08ABCF68`, а в отчёте стояло значение, совпадавшее с сохранённым
     * `$ra` (`0x08ABCF40`).
     *
     * Противоречие разрешилось в пользу статического разбора, и сделала это
     * подмена: в слот `+12` она клала `$a2`, а отчёт показал `0x00000020` —
     * то есть НЕ `$ra`. Значит запись и чтение исправны, `$a1` у гостя
     * действительно равен адресу возврата, и врёт разбор. Слот честный,
     * `args[1]` в отчёте — настоящий `$a1`.
     *
     * Значение `0x00000020`, кстати, пригодилось: с ним картина запуска
     * потока перестала быть неверной — `a1=0x20` это НЕ приоритет. */
    wr32(t + 12, I_SW_SP(R_A0 + 1, 12));
    wr32(t + 16, I_SW_SP(R_A0 + 2, 16));
    wr32(t + 20, I_SW_SP(R_A0 + 3, 20));
    wr32(t + 24, I_LW_SP(R_T0, GUEST_TRAMP_FRAME + 16));
    wr32(t + 28, I_SW_SP(R_T0, 24));
    wr32(t + 32, I_LW_SP(R_T1, GUEST_TRAMP_FRAME + 20));
    wr32(t + 36, I_SW_SP(R_T1, 28));
    wr32(t + 40, I_LW_SP(R_T2, GUEST_TRAMP_FRAME + 24));
    wr32(t + 44, I_SW_SP(R_T2, 32));
    wr32(t + 48, I_SW_SP(R_T8, 40));
    wr32(t + 52, I_SW_SP(R_T9, 44));
    wr32(t + 56, I_SW_SP(R_K0, 48));
    wr32(t + 60, I_SW_SP(R_K1, 52));

    /* `$gp` и адрес функции — абсолютными значениями, собранными из двух
     * половин. Релокации не используются: она и не линкуется по локальному
     * символу в shared-объекте, а значение и так известно на момент загрузки.
     * Нижняя половина кладётся через `addiu` — см. расчёт половин выше. */
    wr32(t + 64, I_LUI(R_T9, fn_hi));
    wr32(t + 68, I_ADDIU(R_T9, R_T9, fn_lo));
    wr32(t + 72, I_LUI(R_T8, gp_hi));
    wr32(t + 76, I_ADDIU(R_T8, R_T8, gp_lo));
    wr32(t + 80, I_MOVE(R_GP, R_T8));

    /* Вызов: `hle_import(index, &args[0], $k0)`.
     *
     * `$k0` передаётся ТОЛЬКО чтобы измерить: гость использует его как входной
     * параметр, прочитать его из кадра сигнала нельзя (его там нет), и потому
     * единственный способ узнать, доходит ли он до вызова, — передать его
     * обработчику и напечатать. */
    wr32(t + 84, I_ADDIU(R_A0, R_ZERO, index));
    wr32(t + 88, I_ADDIU(R_A1, R_SP, 8));
    wr32(t + 92, I_MOVE(R_A2, R_K0));
    wr32(t + 96, I_MOVE(R_A3, R_RA));
    wr32(t + 100, I_JALR(R_T9));
    wr32(t + 104, I_NOP);

    /* Восстановление. `$v0` не трогаем — это результат импорта. */
    wr32(t + 108, I_LW_SP(R_RA, 0));
    wr32(t + 112, I_LW_SP(R_A0, 8));
    wr32(t + 116, I_LW_SP(R_A0 + 1, 12));
    wr32(t + 120, I_LW_SP(R_A0 + 2, 16));
    wr32(t + 124, I_LW_SP(R_A0 + 3, 20));
    wr32(t + 128, I_LW_SP(R_T8, 40));
    wr32(t + 132, I_LW_SP(R_T9, 44));
    /* `$k0` НЕ восстанавливается — в него УСТАНАВЛИВАЕТСЯ адрес `SceThreadContext`.
     *
     * Прежде здесь стояло `lw $k0,48(sp)`, то есть восстановление того, что
     * гость имел. Измерено, что гость имеет там ноль: запись `$k0` самим
     * трамплином, без C, дала `0x00000000`. Дальше гость падает на
     * `sw s5,4($k0)` — запись по адресу `4`, а наблюдаемый `si_addr` = 0,
     * то есть к моменту сбоя `$k0` уехал на `0xFFFFFFFC`.
     *
     * Установка один раз при старте потока не выдерживает и четырёх вызовов:
     * к моменту падения регистр уже не тот. Трамплин — единственное место,
     * где `$k0` под нашим контролем, поэтому ставим его здесь, при каждом
     * возврате в гостя.
     *
     * `$k1` гостю не нужен, восстанавливается как был.
     *
     * Замечание о прошлом A/B: подстановка заведомо адреса гостя проверялась
     * по ЖУРНАЛУ обработчика, а он печатает наш собственный мусор вместо
     * регистра (на MIPS o32 `$k0`/`$k1` — call-clobbered temporaries, их
     * затирает C-код с `-fPIC`). То есть тот эксперимент просто не читал
     * `$k0`, и вывод «кандидат закрыт» был неверен. */
    wr32(t + 136, I_LUI(R_K0, k0_hi));
    wr32(t + 140, I_ADDIU(R_K0, R_K0, k0_lo));
    wr32(t + 144, I_LW_SP(R_K1, 52));
    /* Перед возвратом в гостя записываем свой индекс в разделяемую страницу.
     *
     * Зачем. Нужен однозначный признак «трамплин дошёл до возврата»: по
     * одному отчёту о сбое это не определить — регистры гостя после вызова
     * импорта и перед ним почти одинаковы, а PC не извлекается надёжно. Метка
     * отвечает на вопрос прямо: значение в странице — индекс последнего
     * импорта, чей трамплин вернулся в гостя.
     *
     * Адрес страницы передаётся как литерал, как и адрес функции: он известен
     * на момент загрузки, а `$gp` гостя для этого недоступен. */
    wr32(t + 148, I_ADDIU(R_SP, R_SP, GUEST_TRAMP_FRAME));
    wr32(t + 152, I_LUI(R_T0, trg_hi));
    wr32(t + 156, I_ORI(R_T0, R_T0, trg_lo));
    /* В метку пишется `$k0`, а НЕ индекс импорта.
     *
     * Индекс и так виден: обработчик печатает его сам (`idx=`), так что
     * ничего не теряется. А вот `$k0` больше негде увидеть: в кадре
     * сигнала его нет, и единственный способ снять живое значение —
     * записать его из трамплина, где C-кода ещё не было.
     *
     * Это принципиально: обработчик импорта — обычная C-функция, а на
     * MIPS o32 `$k0` входит в call-clobbered temporaries и ею затирается.
     * Поэтому значение, дошедшее до обработчика третьим аргументом, может
     * отличаться от того, что гость имел на входе в трамплин, и метод
     * «прочитал в обработчике» не отличает одно от другого. */
    wr32(t + 160, I_MOVE(R_T1, R_K0));
    wr32(t + 164, I_SW(R_T1, R_T0, 0));
    wr32(t + 168, I_JR(R_RA));
    /* Вместо `nop` в слоте задержки — `move $v1,zero`.
     *
     * На MIPS o32 64-битный возврат идёт парой: `$v0` = младшие
     * 32 бита, `$v1` = старшие. `sceKernelGetSystemTimeWide` возвращает
     * `u64`, и гостевой таймерный цикл (0x088C17B0) читает ОБА:
     * `move s3,v1` / `move s1,v1`, затем 64-битное вычитание с заёмом
     * (`sltu`/`subu`) и перевод в `float` с поправкой 4294967296.0.
     *
     * До этой правки `$v1` после вызова содержал мусор из C-эпилога
     * `guest_import` — счётчик в отчёте зафиксировал 2.2 млн итераций
     * этого цикла: разность времени получалась случайной, и цикл,
     * который должен закрываться за ~17 проходов, не закрывался
     * никогда. Ноль в `$v1` даёт корректное малое `u64`.
     *
     * Для остальных импортов это тоже строго лучше: `$v1` входит в
     * call-clobbered по o32, гостевой код не имеет права рассчитывать
     * на его сохранность, а детерминированный ноль вместо мусора
     * закрывает и пары `v0/v1` структурных возвратов (поля читаются
     * как нули, что согласуется с возвратом 0 остальных стабов). */
    wr32(t + 172, I_ADDIU(R_V1, R_ZERO, 0));

    /* Стаб: `j` на трамплин и `nop` в слоте задержки, ровно 8 байт. Трамплин
     * лежит в том же отображении, а значит в том же 256 МБ сегменте, так что
     * `j` достаёт его без полного адреса. */
    imm = (tr & 0x0FFFFFFCu) >> 2;
    wr32((u8 *)(uintptr_t)p->stub, 0x08000000u | (imm & 0x03FFFFFFu));
    wr32((u8 *)(uintptr_t)p->stub + 4, I_NOP);

    m->tramp_next = tr + GUEST_TRAMP_BYTES;
    p->via_trampoline = true;
    p->trampoline = tr;
    p->stub_index = index;

    /* Сброс кэша обязателен: и для трамплина, и для стаба. */
    guest_flush_icache(t, GUEST_TRAMP_BYTES);
    guest_flush_icache((void *)(uintptr_t)p->stub, 8);
    return true;
}

void guest_unload(GuestModule *m)
{
    if (m == NULL)
        return;
    /* Зеркало снимается ПЕРВЫМ: оно алиасит ту же память, и порядок
     * тут не важен, но так проще читать. */
    if (m->mirror_base != 0) {
        munmap((void *)(uintptr_t)m->mirror_base, m->map_size);
        m->mirror_base = 0;
    }
    if (m->vram_mirror != 0) {
        munmap((void *)(uintptr_t)m->vram_mirror, GUEST_VRAM_BYTES);
        m->vram_mirror = 0;
    }
    if (m->vram_base != 0) {
        munmap((void *)(uintptr_t)m->vram_base, GUEST_VRAM_BYTES);
        m->vram_base = 0;
    }
    if (m->memory != NULL) {
        munmap(m->memory, m->map_size);
        m->memory = NULL;
    }
    m->loaded = false;
}