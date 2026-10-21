/* Самопроверка ходячего по GE-спискам.
 *
 * Синтетические списки: линейный со спрайтами, линейный с
 * 3D-треугольниками, список с CALL/RET/JUMP и граничный
 * случай пустого списка. Каждый прогон — на заведомо
 * известный ответ; ходячий обязан совпасть. */

#include "ge_walk.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *what, u32 got, u32 want)
{
    if (got != want) {
        printf("FAIL %s: got %u, want %u\n", what, got, want);
        failures++;
    }
}

static void check_bool(const char *what, bool got, bool want)
{
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

/* Слово команды: cmd — старший байт, arg — 24 бита. */
static u32 ins(u32 cmd, u32 arg)
{
    return (cmd << 24) | (arg & 0x00FFFFFFu);
}

#define CMD_PRIM    0x04u
#define CMD_JUMP    0x08u
#define CMD_CALL    0x0Au
#define CMD_RET     0x0Bu
#define CMD_END     0x0Cu
#define CMD_FINISH  0x0Fu
#define CMD_BASE    0x10u

/* Заполнение GeList в новой схеме: тестовый массив изображает собой
 * ВСЁ отображение гостя с началом в `addr`. */
static void set_list(GeList *l, const u32 *arr, u32 words, u32 addr)
{
    l->mem = arr;
    l->base = addr;
    l->size = words * 4u;
    l->list_addr = addr;
}

int main(void)
{
    GeWalkResult r;
    GeList l;

    /* --- 1. линейный список: 2 спрайта, FINISH, END --- */
    {
        u32 list[] = {
            ins(CMD_PRIM,   (6u << 16) | 2u),   /* спрайт, 2 вершины */
            ins(CMD_PRIM,   (6u << 16) | 2u),   /* спрайт, 2 вершины */
            ins(CMD_FINISH, 0u),
            ins(CMD_END,    0u),
        };
        set_list(&l, list, 4u, 0x04000000u);
        check("спрайты: ge_walk", ge_walk(&l, &r) ? 1u : 0u, 1u);
        check("спрайты: count", r.prim_sprites, 2u);
        check("спрайты: 3d",    r.prim_3d, 0u);
        check("спрайты: verts", r.max_vertices, 2u);
        check("спрайты: finish", r.finish, 1u);
        check_bool("спрайты: has_2d", r.has_2d, true);
        check_bool("спрайты: has_3d", r.has_3d, false);
    }

    /* --- 2. линейный список: 3D-треугольники --- */
    {
        u32 list[] = {
            ins(CMD_PRIM, (3u << 16) | 3u),   /* треугольник, 3 вершины */
            ins(CMD_PRIM, (4u << 16) | 6u),   /* strip, 6 вершин */
            ins(CMD_PRIM, (5u << 16) | 9u),   /* fan, 9 вершин */
            ins(CMD_END,  0u),
        };
        set_list(&l, list, 4u, 0x04000000u);
        check("3d: ge_walk", ge_walk(&l, &r) ? 1u : 0u, 1u);
        check("3d: count", r.prim_3d, 3u);
        check("3d: sprites", r.prim_sprites, 0u);
        check("3d: verts", r.max_vertices, 9u);
        check_bool("3d: has_2d", r.has_2d, false);
        check_bool("3d: has_3d", r.has_3d, true);
    }

    /* --- 3. смешанный: спрайт и треугольник --- */
    {
        u32 list[] = {
            ins(CMD_PRIM, (6u << 16) | 2u),
            ins(CMD_PRIM, (3u << 16) | 3u),
            ins(CMD_END,  0u),
        };
        set_list(&l, list, 3u, 0x04000000u);
        check("смеш: ge_walk", ge_walk(&l, &r) ? 1u : 0u, 1u);
        check("смеш: sprites", r.prim_sprites, 1u);
        check("смеш: 3d", r.prim_3d, 1u);
        check_bool("смеш: has_2d", r.has_2d, true);
        check_bool("смеш: has_3d", r.has_3d, true);
    }

    /* --- 4. CALL/RET: спрайт в подпрограмме --- */
    {
        /* адреса в GE собираются как (BASE | arg); BASE вмещает
         * 24 бита, поэтому база списка — в пределах 16 МБ. */
        u32 list[] = {
            ins(CMD_BASE,   0x00010000u),            /* 0: база списка */
            ins(CMD_CALL,   0x0Cu),                   /* 1: вызов на +0x0C (idx 3) */
            ins(CMD_END,    0u),                      /* 2: конец (куда вернёмся) */
            ins(CMD_PRIM,  (6u << 16) | 2u),          /* 3: спрайт */
            ins(CMD_RET,   0u),                       /* 4: возврат на idx 2 */
        };
        set_list(&l, list, 5u, 0x00010000u);
        check("call: ge_walk", ge_walk(&l, &r) ? 1u : 0u, 1u);
        check("call: sprites", r.prim_sprites, 1u);
        check("call: 3d", r.prim_3d, 0u);
        check("call: depth", r.calls, 1u);
        check("call: steps", r.steps, 5u);   /* BASE, CALL, PRIM, RET, END */
    }

    /* --- 5. JUMP назад: цикл, ограничен шагами --- */
    {
        /* 0: BASE ; 1: PRIM sprite ; 2: JUMP на +0x04 (idx 1) */
        u32 list[] = {
            ins(CMD_BASE,   0x00010000u),
            ins(CMD_PRIM,  (6u << 16) | 2u),
            ins(CMD_JUMP,   0x04u),
        };
        set_list(&l, list, 3u, 0x00010000u);
        check("jump: ge_walk", ge_walk(&l, &r) ? 1u : 0u, 1u);
        check_bool("jump: has_2d", r.has_2d, true);
        check_bool("jump: has_3d", r.has_3d, false);
        check("jump: steps capped", r.steps, 65536u);
    }

    /* --- 6. PRIM с нулём вершин не считается --- */
    {
        u32 list[] = {
            ins(CMD_PRIM, (6u << 16) | 0u),   /* 0 вершин — пропуск */
            ins(CMD_END,  0u),
        };
        set_list(&l, list, 2u, 0x04000000u);
        check("zero: ge_walk", ge_walk(&l, &r) ? 1u : 0u, 1u);
        check("zero: sprites", r.prim_sprites, 0u);
        check("zero: steps", r.steps, 2u);
    }

    /* --- 7. ЗЕРКАЛО БЕЗ КЭША: старт по адресу с битом 0x40000000 --- */
    {
        u32 list[] = {
            ins(CMD_PRIM,   (6u << 16) | 2u),
            ins(CMD_END,    0u),
        };
        /* Список лежит по 0x04010000 (как бы в отображении), но гость
         * сдал его зеркалом 0x44010000. Нормализация при чтении обязана
         * привести адрес обратно. */
        set_list(&l, list, 2u, 0x04010000u);
        l.list_addr = 0x44010000u;
        check("mirror: ge_walk", ge_walk(&l, &r) ? 1u : 0u, 1u);
        check("mirror: sprites", r.prim_sprites, 1u);
        check("mirror: steps", r.steps, 2u);
    }

    /* --- 8. JUMP МЕЖДУ БУФЕРАМИ: список из двух кусков --- */
    {
        /* Кусок A: два спрайта, затем JUMP на кусок B.
         * Кусок B: треугольник, FINISH, END.
         * Прежняя модель «один массив» обрывалась на переходе; новая
         * читает по всему отображению и обязана дойти до END. */
        u32 piece_a[] = {
            ins(CMD_PRIM,   (6u << 16) | 2u),
            ins(CMD_PRIM,   (6u << 16) | 2u),
            ins(CMD_JUMP,   0x00020000u),   /* цель: (BASE|arg) = 0x00020000 */
        };
        u32 piece_b[] = {
            ins(CMD_PRIM,   (3u << 16) | 3u),
            ins(CMD_FINISH, 0u),
            ins(CMD_END,    0u),
        };
        /* «Отображение»: кусок A с 0x00010000, кусок B с 0x00020000.
         * Читатель обязан брать оба из одного mem-буфера, поэтому
         * куски кладём в общий массив с дыркой между ними. */
        u32 img[0x20000 / 4u];
        memset(img, 0, sizeof(img));
        memcpy(&img[(0x00010000u - 0x00010000u) / 4u],
               piece_a, sizeof(piece_a));
        memcpy(&img[(0x00020000u - 0x00010000u) / 4u],
               piece_b, sizeof(piece_b));
        set_list(&l, img, (u32)(sizeof(img) / 4u), 0x00010000u);
        check("jumpbuf: ge_walk", ge_walk(&l, &r) ? 1u : 0u, 1u);
        check("jumpbuf: sprites", r.prim_sprites, 2u);
        check("jumpbuf: 3d", r.prim_3d, 1u);
        check("jumpbuf: finish", r.finish, 1u);
        check("jumpbuf: steps", r.steps, 6u);
    }

    /* --- 9. граничные: пустой и нулевые аргументы --- */
    {
        check("empty: walk", ge_walk(NULL, &r) ? 1u : 0u, 0u);
        check("null: walk", ge_walk(&l, NULL) ? 1u : 0u, 0u);
    }

    if (failures != 0) {
        printf("ge_walk_test: %d FAILURES\n", failures);
        return 1;
    }
    printf("ge_walk_test: all passed\n");
    return 0;
}
