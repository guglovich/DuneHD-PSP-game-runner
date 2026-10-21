/* Вывод кадра PSP на экран плеера: порт проверенного конвейера.
 *
 * Источник — соседний проект: `Dune Max/tools/native_plugin.c`
 * (прогон 2026-10-09 «РАБОТАЕТ: кадр выведен, shell пережил»,
 * шахматка на экране) и канон адресов `Dune HD Max game/docs/gfx-entry.md`.
 * Последовательность и все коды возврата взяты оттуда, а не придуманы:
 *
 *   1. cb->enable_directfb_mode()  — из таблицы колбэков `task`;
 *   2. osd_context: idx = ctx[0x34], newidx = 1-idx,
 *      base_tok = ctx[4*(newidx+3)], buf_tok = ctx[4*(newidx+5)];
 *   3. RUALock(pRUA, buf_tok, size)                 — успех = 6;
 *   4. gbus_get_locked_area(ctx, buf, size, &o3,&o4,&o2) — успех = 6;
 *   5. pix = gbus_map_region(ctx, o3, o4) + o2     — пиксели в процессе;
 *   6. запись кадра;
 *   7. RUAUnLock(pRUA, buf_tok) с повторами до 6 (7/9 = «не готово»);
 *   8. DisplayPicture(ctx[0x08], ctx[0x38], 0, 0);
 *   9. обмен контекста: ctx[0x34]=newidx, ctx[0x38]=base_tok,
 *      buffer_addr = buf_tok;
 *  10. WaitForPicture(base_tok).
 *
 * Отличие от образца: у нас нет dlsym. Плагин собран `-nostdlib` без
 * релокаций — неразрешённая ссылка не заполнится (документировано в
 * `plat.c`). Поэтому адреса берутся напрямую:
 *
 *   - экспорты `shell` — АБСОЛЮТНЫЕ (`shell` не PIE, адреса сверены
 *     по `.dynsym` живого бинарника — `docs/gfx-entry.md`);
 *   - `librua.so`/`libllad.so` — базы ищутся в `/proc/self/maps`
 *     (raw `open`/`read`), смещения внутри — из разборов соседнего
 *     проекта (`docs/hle-injection.md`);
 *   - глобалы OSD — абсолютные адреса из `.dynsym`.
 *
 * Чтение глобалов из нашей библиотеки работает: предостережение
 * про `.bss` относилось к ЧТЕНИЮ ИЗ ЧУЖОГО ПРОЦЕССА (`dd` из
 * `/proc/<pid>/mem`), а рабочий образец читал их изнутри `shell`
 * по тем же адресам — и это было проверено на живом плеере.
 *
 * Безопасность: сбой на любом шаге — немедленный выход, БЕЗ попытки
 * продолжить; `RUAUnLock` с жёстким пределом повторов, иначе UI
 * зависает (измерено). Ни одного обращения к устройствам. */

#include "gfx_out.h"

#include "plat.h"
#include "psp_hle.h"

#include <stddef.h>

/* --- абсолютные адреса (`docs/gfx-entry.md`, сверены по .dynsym) ---- */

#define SHELL_MY_GFX_DisplayPicture 0x00562318u
#define SHELL_MY_GFX_WaitForPicture 0x005622D8u

#define ADDR_OSD_CONTEXT 0x00DA0C60u   /* структура, 68 байт */
#define ADDR_BUFFER_ADDR 0x00DA5340u   /* u32: токен кадра OSD */
#define ADDR_PRUA        0x00DA5770u   /* указатель на экземпляр RUA */

/* Смещения внутри librua.so / libllad.so (`docs/hle-injection.md`). */
#define LIBRUA_RUALock   0x184cu
#define LIBRUA_RUAUnLock 0x16CCu
#define LLAD_GET_LOCKED  0x19D8u
#define LLAD_MAP_REGION  0x1E50u

/* Габарит поверхности OSD: рабочий образец писал 1920×1080×4 — и этого
 * хватило, rc=6 по всей цепочке. Меньше брать нельзя: RUALock проверяет
 * размер по области. */
#define OSD_W 1920u
#define OSD_H 1080u
#define OSD_BPP 4u

/* Коды возврата RUA/gbus: успех = 6; 7 = «повтор», 9 = тоже виделось. */
#define RUA_OK 6

/* Состояние конвейера: шаг, на котором остановились, и коды возврата.
 * Печатаются в отчёт по одному байту на шаг. */
u32 gfx_out_step;
u32 gfx_out_rc_lock;
u32 gfx_out_rc_getlock;
u32 gfx_out_rc_unlock;
u32 gfx_out_shown;      /* сколько кадров реально показано */

/* Резолвнутые базы — заполняются в gfx_out_init. */
static u32 base_librua;
static u32 base_libllad;

/* Поиск базы библиотеки в /proc/self/maps: первая строка с именем.
 * Возврат — адрес начала отображения или 0.
 *
 * Буфер 32 КиБ: у `shell` десятки отображений (79 библиотек), и
 * строки librua/libllad легко лежат за пределами первых 8 КиБ.
 * При нехватке поиск молча провалится — вывода не будет, но прогон
 * не пострадает. */
static u32 find_lib_base(const char *name, u32 name_len)
{
	static char maps[32768];
	long fd;
	long got;
	u32 i, j;

	fd = plat_open_read("/proc/self/maps");
	if (fd < 0)
		return 0;
	got = plat_read(fd, maps, sizeof(maps) - 1u);
	(void)plat_syscall(4006 /* close */, fd, 0, 0, 0);
	if (got <= 0)
		return 0;
	maps[got] = '\0';

	/* Строка: "00400000-00d30000 r-xp ... /lib/librua.so".
	 * Ищем подстроку-имя, затем от найденного места начало строки
	 * и парсим hex до '-'. */
	for (i = 0; i + name_len < (u32)got; i++) {
		for (j = 0; j < name_len; j++) {
			if (maps[i + j] != name[j])
				break;
		}
		if (j == name_len) {
			/* нашли имя — ищем начало строки назад */
			while (i > 0 && maps[i] != '\n')
				i--;
			if (maps[i] == '\n')
				i++;
			/* hex-адрес до '-' */
			{
				u32 v = 0;
				u32 k;

				for (k = 0; k < 16u; k++) {
					char c = maps[i + k];
					u32 d;

					if (c >= '0' && c <= '9')
						d = (u32)(c - '0');
					else if (c >= 'a' && c <= 'f')
						d = (u32)(c - 'a' + 10);
					else if (c >= 'A' && c <= 'F')
						d = (u32)(c - 'A' + 10);
					else
						break;
					v = (v << 4) | d;
				}
				if (k > 0)
					return v;
			}
		}
	}
	return 0;
}

/* Сигнатуры — из дизасма (docs/gfx-entry.md, docs/hle-injection.md). */
typedef int (*fn_rua_lock)(void *inst, unsigned long tok,
                           unsigned long size);
typedef int (*fn_rua_unlock)(void *inst, unsigned long tok);
typedef int (*fn_gbus_get_locked)(void *ctx, void *addr, void *size,
                                  void **o3, void **o4, void **o2);
typedef void *(*fn_gbus_map)(void *ctx, void *a, void *b);
typedef void (*fn_gfx4)(unsigned long, unsigned long, unsigned long,
                        unsigned long);
typedef void (*fn_gfx1)(unsigned long);

int gfx_out_init(void)
{
	base_librua = find_lib_base("librua.so", 9);
	base_libllad = find_lib_base("libllad.so", 10);

	gfx_out_step = 0;
	if (base_librua == 0 || base_libllad == 0)
		return -1;
	return 0;
}

/* Показать кадр PSP на экране: полная цепочка RUA→запись→показ.
 *
 * src — ХОСТОВЫЙ указатель на пиксели гостя (вызовы обязаны
 * нормализовать адрес сами: VRAM 0x04000000/0x44xxxxxx отображён у
 * нас в процессе, образ — тоже). w/h — габариты (480×272), stride —
 * байты на строку, fmt — формат PSP (0=5551, 1=565, 2=4444, 3=8888).
 *
 * Возвращает 0 при успехе; положительный код — номер шага, на котором
 * остановились (совпадает с gfx_out_step). */
int gfx_out_show(const unsigned char *src, u32 w, u32 h, u32 stride,
                 u32 fmt)
{
	volatile u32 *osd_ctx = (volatile u32 *)(uintptr_t)ADDR_OSD_CONTEXT;
	volatile u32 *p_buf = (volatile u32 *)(uintptr_t)ADDR_BUFFER_ADDR;
	volatile u32 *p_prua = (volatile u32 *)(uintptr_t)ADDR_PRUA;
	fn_rua_lock rua_lock;
	fn_rua_unlock rua_unlock;
	fn_gbus_get_locked get_locked;
	fn_gbus_map map_region;
	fn_gfx4 disp_pic;
	fn_gfx1 wait_pic;
	void *pRUA;
	void *ctx;
	unsigned long idx, newidx, base_tok, buf_tok;
	unsigned long size = (unsigned long)OSD_W * OSD_H * OSD_BPP;
	void *o3 = 0, *o4 = 0, *o2 = 0;
	unsigned char *pix;
	unsigned int attempt;
	int rc;
	u32 x, y;

	gfx_out_step = 1;
	if (base_librua == 0 || base_libllad == 0)
		return 1;

	rua_lock = (fn_rua_lock)(uintptr_t)(base_librua + LIBRUA_RUALock);
	rua_unlock = (fn_rua_unlock)(uintptr_t)(base_librua
	                                        + LIBRUA_RUAUnLock);
	get_locked = (fn_gbus_get_locked)(uintptr_t)(base_libllad
	                                             + LLAD_GET_LOCKED);
	map_region = (fn_gbus_map)(uintptr_t)(base_libllad
	                                      + LLAD_MAP_REGION);
	disp_pic = (fn_gfx4)(uintptr_t)SHELL_MY_GFX_DisplayPicture;
	wait_pic = (fn_gfx1)(uintptr_t)SHELL_MY_GFX_WaitForPicture;

	/* Индекс и задний буфер — ровно как в рабочем образце. */
	gfx_out_step = 2;
	idx = osd_ctx[0x34 / 4];
	newidx = 1ul - idx;
	base_tok = osd_ctx[(4u * (newidx + 3u)) / 4u];
	buf_tok = osd_ctx[(4u * (newidx + 5u)) / 4u];
	if (buf_tok == 0)
		return 2;

	gfx_out_step = 3;
	pRUA = (void *)(uintptr_t)*p_prua;
	if (pRUA == 0)
		return 3;
	ctx = (void *)(uintptr_t)((volatile u32 *)pRUA)[8u / 4u];

	/* Блокируем и отображаем задний буфер. */
	gfx_out_step = 4;
	rc = rua_lock(pRUA, buf_tok, size);
	gfx_out_rc_lock = (u32)rc;
	if (rc != RUA_OK)
		return 4;

	gfx_out_step = 5;
	rc = get_locked(ctx, (void *)buf_tok, (void *)size, &o3, &o4, &o2);
	gfx_out_rc_getlock = (u32)rc;
	if (rc != RUA_OK)
		return 5;

	gfx_out_step = 6;
	pix = (unsigned char *)map_region(ctx, o3, o4);
	if (pix != 0 && o2 != 0)
		pix += (uintptr_t)o2;
	if (pix == 0)
		return 6;

	/* Масштабирование кадра PSP в OSD: 480×272 → 1920×1080 —
	 * множитель 4.0/3.97, ближайший сосед. Формат PSP разворачивается
	 * в 32-битный ARGB OSD (в образце цвета вида 0xAARRGGBB:
	 * зелёный 0xFF00FF00 давал зелёные клетки). */
	gfx_out_step = 7;
	{
		u32 sx_num = (OSD_W << 16) / w;    /* 16.16 шаг по X */
		u32 sy_num = (OSD_H << 16) / h;
		u32 sy = 0;

		for (y = 0; y < OSD_H; y++) {
			volatile u32 *row = (volatile u32 *)
			    (pix + y * (OSD_W * OSD_BPP));
			const unsigned char *srow;
			u32 sx = 0;

			srow = src + (sy >> 16) * stride;
			for (x = 0; x < OSD_W; x++) {
				u32 v;
				const unsigned char *p =
				    srow + (sx >> 16) * ((fmt == 3u) ? 4u : 2u);

				if (fmt == 3u) {
					/* PSP 8888: байты R,G,B,A —
					 * в слове 0xAABBGGRR. OSD ждёт
					 * 0xAARRGGBB — разворот R/B. */
					u32 r = p[0], g = p[1], b = p[2];
					u32 a = p[3];

					v = (a << 24) | (b << 16)
					  | (g << 8) | r;
				} else if (fmt == 0u) {
					/* 5551: 15 бит + флаг. */
					u32 c = (u32)p[0]
					      | ((u32)p[1] << 8);
					u32 r5 = (c >> 0) & 0x1F;
					u32 g5 = (c >> 5) & 0x1F;
					u32 b5 = (c >> 10) & 0x1F;
					u32 a1 = (c >> 15) & 1u;

					v = (a1 ? 0xFF000000u : 0u)
					  | ((b5 << 3) << 16)
					  | ((g5 << 3) << 8)
					  | (r5 << 3);
				} else if (fmt == 1u) {
					/* 565. */
					u32 c = (u32)p[0]
					      | ((u32)p[1] << 8);
					u32 r5 = (c >> 0) & 0x1F;
					u32 g6 = (c >> 5) & 0x3F;
					u32 b5 = (c >> 11) & 0x1F;

					v = 0xFF000000u
					  | ((b5 << 3) << 16)
					  | ((g6 << 2) << 8)
					  | (r5 << 3);
				} else {
					/* 4444: непрозрачный, что есть. */
					u32 c = (u32)p[0]
					      | ((u32)p[1] << 8);
					u32 r4 = (c >> 0) & 0xF;
					u32 g4 = (c >> 4) & 0xF;
					u32 b4 = (c >> 8) & 0xF;

					v = 0xFF000000u
					  | ((b4 << 4) << 16)
					  | ((g4 << 4) << 8)
					  | (r4 << 4);
				}
				row[x] = v;
				sx += sx_num;
			}
			sy += sy_num;
		}
	}

	/* Снятие блокировки ДО показа: без этого следующий osd_flip самого
	 * shell зависает. Повторы до 6 с жёстким пределом — 7/9 означают
	 * «ещё не готово», а не ошибку. */
	gfx_out_step = 8;
	rc = 99;
	for (attempt = 0; attempt < 8u; attempt++) {
		rc = rua_unlock(pRUA, buf_tok);
		if (rc == RUA_OK)
			break;
	}
	gfx_out_rc_unlock = (u32)rc;
	if (rc != RUA_OK)
		return 8;

	/* Показ ровно как в osd_flip: сначала DisplayPicture текущей
	 * плоскости, затем обмен контекста, затем WaitForPicture.
	 * Порядок проверен по GOT-слотам, а не по декомпиляции. */
	gfx_out_step = 9;
	disp_pic(osd_ctx[8u / 4u], osd_ctx[0x38 / 4u], 0, 0);

	gfx_out_step = 10;
	osd_ctx[0x34 / 4u] = (u32)newidx;
	osd_ctx[0x38 / 4u] = (u32)base_tok;
	*p_buf = (u32)buf_tok;

	gfx_out_step = 11;
	wait_pic(base_tok);

	gfx_out_step = 12;
	gfx_out_shown++;
	return 0;
}
