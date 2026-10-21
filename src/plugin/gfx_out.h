#ifndef GFX_OUT_H
#define GFX_OUT_H

/* Вывод кадра PSP на экран плеера через RUA/GFX-конвейер shell.
 *
 * Порт проверенного образца `Dune Max/tools/native_plugin.c`
 * (прогон 2026-10-09: кадр показан, shell пережил). Подробности
 * механики и обоснования безопасности — у реализации в `gfx_out.c`
 * и в `docs/gfx-entry.md` / `docs/frame-output-gfx.md`. */

/* Поля отчёта: состояние конвейера после прогона. */
extern unsigned int gfx_out_step;
extern unsigned int gfx_out_rc_lock;
extern unsigned int gfx_out_rc_getlock;
extern unsigned int gfx_out_rc_unlock;
extern unsigned int gfx_out_shown;

/* Найти базы librua.so/libllad.so по /proc/self/maps.
 * 0 — успех; -1 — база не найдена (вывод невозможен, но не фатален). */
int gfx_out_init(void);

/* Показать кадр на экране. src — ХОСТОВЫЙ указатель на пиксели
 * (вызывающий обязан нормализовать адрес гостя сам), w/h — габарит,
 * stride — байты на строку, fmt — формат PSP (0=5551, 1=565,
 * 2=4444, 3=8888). Возврат: 0 — показан; 1..12 — шаг остановки. */
int gfx_out_show(const unsigned char *src, unsigned int w,
                 unsigned int h, unsigned int stride, unsigned int fmt);

#endif /* GFX_OUT_H */
