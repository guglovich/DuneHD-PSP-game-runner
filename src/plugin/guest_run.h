/*
 * Интерфейс запуска гостя для нативного плагина Dune.
 *
 * Узкий намеренно: печать отчёта остаётся в `dune_plugin.c`, здесь всё, что
 * исполняется после старта потока, и всё, что обязано пережить выгрузку
 * библиотеки хостом.
 */

#ifndef GUEST_RUN_H
#define GUEST_RUN_H

#include <stdint.h>

typedef uint32_t u32;

/* Заголовок подключается, а не объявляется вперёд: в `guest_load.h` структуры
 * безымянные (`typedef struct { ... } GuestModule;`), и упреждающее
 * `struct GuestModule` объявило бы СОВСЕМ ДРУГОЙ тип. Компилятор на это
 * отвечает `-Wincompatible-pointer-types`, то есть расхождение типов, а не
 * формальность. */
#include "guest_load.h"

/* Создаёт поток без гостя: он пишет отметку в `fd` и завершается.
 *
 * Отдельный шаг потому, что `SIGSEGV` в любом потоке убивает весь процесс.
 * При запуске гостя падение не говорит, чьё оно — гостевого кода или нашего
 * кода ожидания. Пустой поток отвечает на это измерением.
 *
 * Возвращает pid потока либо отрицательное значение при отказе. */
int guest_run_probe(int fd);

/* Строит трамплины импортов и запускает гостя в отдельном потоке.
 *
 * Возвращает 0 при успехе; отрицательное значение — отказ, модуль без
 * отрицательного кода: -1 не опознан пролог обработчика, -2 не построены
 * трамплины, -3 не создан поток.
 *
 * Сам гостевой код не возвращается: поток завершается сам. */
int guest_run_start(GuestModule *m, const GuestImportTable *t, int log_fd);

/* Ждёт, пока гость выполнит `want` вызовов импортов либо завершится.
 * `max_ms` ограничивает ожидание, чтобы зависший гость не держал плеер. */
void guest_run_wait(u32 want, u32 max_ms);

/* Обработчик импортов. Адрес передаётся в трамплины числом при сборке. */
u32 guest_import(u32 index, const u32 *args, u32 k0, u32 ra);

/* Сведения о сбое потока гостя, снятые обработчиком. Поля PC и адрес
 * определены измерением, а не по заголовку: см. комментарий у обработчика. */
int  guest_run_fault(void);
u32  guest_run_fault_sig(void);
u32  guest_run_fault_pc(void);
u32  guest_run_fault_addr(void);

/* Доступ для отчёта. */
u32 guest_run_calls(void);

/* Счётчик вызовов по индексу импорта — для топ-NID в отчёте. */
u32 guest_run_nid_count(u32 i);
u32 guest_run_nid_ra(u32 i);
u32 guest_run_nid_a0(u32 i);
/* Последние вызовы (перезаписываемое кольцо из 32). */
u32 guest_run_recent_nid(u32 i);
u32 guest_run_recent_ra(u32 i);
u32 guest_run_recent_a0(u32 i);
u32 guest_run_unknown(void);
u32 guest_run_tramp(void);

/* Размерность снимков пути `sceKernelLoadModule`: одна константа для
 * `guest_run.c` (хранение) и отчёта (печать), чтобы они не разошлись. */
#define GUEST_LM_SNAP_SLOTS 4
#define GUEST_LM_SNAP_WORDS 8

/* Отметки ассемблера о прыжке в гостя: `0x47554554` — состоялся,
 * `0` — поток упал раньше. Метки РАЗДЕЛЬНЫЕ по потокам: общая переменная
 * давала ложный ответ, потому что её успевал ставить главный поток, пока
 * сбой происходил в рабочем. */
u32 guest_run_mark_main(void);
u32 guest_run_mark_worker(void);
u32 guest_run_overflow(void);

/* 1, если гость остановлен по исчерпании предела вызовов, а не вышел сам. */
u32 guest_run_limit_hit(void);
u32 guest_run_hle_count(void);
u32 guest_run_iow_count(void);
u32 guest_run_atexit_head(void);
u32 guest_run_atexit_raw(u32 *hi_out, u32 *lo_out);
void guest_run_atexit_snapshot(void);
u32  guest_run_atexit_snap_n(void);
u32  guest_run_atexit_snap_head(void);
u32  guest_run_atexit_snap_at(u32 i);
u32  guest_run_atexit_snap_next_at(u32 i);
u32  guest_run_peek(u32 vaddr, unsigned n, u32 *out);
u32  guest_run_ctor_head_n(void);
/* Снимки пути `sceKernelLoadModule`: число вызовов и по слоту — область
 * (0 — вне известных отображений, 1 — образ гостя, 2 — стек гостя) и
 * слова пути. Слоты идут в порядке вызова, `guest_run_lm_slot` —
 * следующий перезаписываемый. */
u32  guest_run_lm_count(void);
u32  guest_run_lm_slot(void);
u32  guest_run_lm_where(u32 slot);
u32  guest_run_lm_word(u32 slot, u32 k);
u32  guest_run_cache_nops(void);
u32  guest_run_mirror_base(void);
u32  guest_run_emul_ins(void);
u32  guest_run_patch_64(void);
u32  guest_run_fb_top(void);
u32  guest_run_state(u32 vaddr);
u32  guest_run_fb_bw(void);
u32  guest_run_fb_fmt(void);
u32  guest_run_fb_count(void);
u32  guest_run_fb_dumps(void);
u32  guest_run_vram_base(void);

/* Нулевая страница + скретчпад (замер 90): uncached-вид 0x40000000 и
 * кэшируемый скретчпад 0x10000. Нули — адреса заняты, зона на
 * фолт-эмуляции. */
u32  guest_run_null_mirror(void);
u32  guest_run_scr_base(void);

/* Куча VPL: адреса области и приборы к подмене семейства
 * `sceKernel*Vpl` (прогон 35: без адреса блока гость падал в
 * `HeapSystem`). 0 у `vpl_base` — области нет, `MaxFreeMemSize`
 * отвечала нулём. `vpl_used` — курсор бампа против фактического
 * `vpl_bytes`; заметный `vpl_frees` означает, что бамп без
 * освобождения истощает арену. */
u32  guest_run_vpl_base(void);
u32  guest_run_vpl_mirror(void);
u32  guest_run_vpl_bytes(void);
u32  guest_run_vpl_created(void);
u32  guest_run_vpl_allocs(void);
u32  guest_run_vpl_frees(void);
u32  guest_run_vpl_fails(void);
u32  guest_run_vpl_used(void);

/* Шрифтовая библиотека: приборы к подмене sceLibFont (прогон 36:
 * нулевой хэндл NewLib валил гость в его шрифтовом коде).
 * `font_glyph` — число отказов рендера глифов: рендера PGF нет,
 * и заметное число показывает, как часто гость в него попадает. */
u32  guest_run_font_libs(void);
u32  guest_run_font_opens(void);
u32  guest_run_font_glyphs(void);

/* Флаги событий: прибор к подмене sceKernelCreateEventFlag (прогон 38:
 * игра создаёт флаги, получает 0 и не идёт дальше инициализации).
 * `ef_created` — число созданных флагов. */
u32  guest_run_ef_created(void);

/* Обратные вызовы: прибор к подмене sceKernelCreateCallback
 * (прогон 39: игра создаёт callback, получает uid > 0). */
u32  guest_run_cb_created(void);
u32  guest_run_cb_fired(void);
u32  guest_run_si_fired(void);
u32  guest_run_sema_created(void);   /* замер 97: uid-таблица семафоров */
u32  guest_run_sema_waited(void);
u32  guest_run_sema_stuck(void);   /* тики суб-прерываний (замер 92) */

/* Показ последнего кадра гостя на экран плеера (конвейер RUA/GFX).
 * Вызывается из точки входа плагина ПОСЛЕ записи отчёта.
 * 0 — показан; 1..12 — шаг остановки; 0xFFFFFF01/2 — кадра нет. */
u32  guest_run_show_last_frame(void);
u32  guest_run_ctor_head_at(u32 i);
u32  guest_run_ctor_len_at(u32 i);
u32 guest_run_atexit_file_lo(void);
u32 guest_run_atexit_nodes(u32 *out, unsigned max);
u32 guest_run_log_max(void);

/* Предел числа вызовов, на котором гость останавливается сам. */
u32 guest_run_target(void);
int  guest_run_pid(void);
int  guest_run_finished(void);

/* Записи журнала вызовов: по индексу вызова. */
u32 guest_run_nid(u32 i);
u32 guest_run_stub(u32 i);
u32 guest_run_arg0(u32 i);
u32 guest_run_arg1(u32 i);
u32 guest_run_arg2(u32 i);
u32 guest_run_arg3(u32 i);
u32 guest_run_ret(u32 i);
u32 guest_run_call_head(u32 i);
u32 guest_run_head_before_main(void);
u32 guest_run_head_before_worker(void);
u32 guest_run_len_before_main(void);
u32 guest_run_len_before_worker(void);

#endif /* GUEST_RUN_H */