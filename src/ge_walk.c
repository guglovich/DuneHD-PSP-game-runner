/* Ходячий по спискам команд GE.
 *
 * Задача — ровно та, что в docs/ge-commands.md: отличить
 * 2D-примитивы (PRIM type=6, спрайты) от 3D-геометрии
 * (type=3,4,5), не эмулируя растеризацию. Это диагностика
 * «2D или 3D» и фундамент под будущий перевод спрайтов в
 * команды GFX.
 *
 * Формат слова и переходы — по jpcsp (см. docs/ge-commands.md):
 *   PRIM        type = (arg >> 16) & 7, вершин = arg & 0xFFFF
 *   JUMP/CALL   цель = (BASE | arg) + baseOffset
 *   OFFSET_ADDR baseOffset = arg << 8 (со знаком)
 *   CALL кладёт (PC+4, baseOffset) в стек, RET снимает оба
 *
 * Модель ходячего эквивалентна jpcsp: там pc шагает на 4 до
 * выполнения, поэтому JUMP/CALL/RET работают с адресом
 * следующей команды — ровно так же, как здесь: мы читаем по
 * pc, а переход ставит pc на следующую исполняемую команду. */

#include "ge_walk.h"

#include <stddef.h>
#include <string.h>

/* Ограничения для конечности цикла (правило 2 Power of Ten).
 * Список конечен, но JUMP может вернуть назад, поэтому
 * число шагов ограничено. Глубина CALL тоже конечна. */
#define GE_WALK_MAX_STEPS 65536u
#define GE_WALK_MAX_CALLS 16u

/* Старший байт — команда. */
#define GE_CMD_PRIM     0x04u
#define GE_CMD_JUMP     0x08u
#define GE_CMD_CALL     0x0Au
#define GE_CMD_RET      0x0Bu
#define GE_CMD_END      0x0Cu
#define GE_CMD_SIGNAL   0x0Eu
#define GE_CMD_FINISH   0x0Fu
#define GE_CMD_BASE     0x10u
#define GE_CMD_OFFSET   0x13u

/* Типы примитивов PRIM (биты 16..18). */
#define GE_PRIM_TYPE(v)      (((v) >> 16) & 0x7u)
#define GE_PRIM_VERTICES(v)  ((v) & 0xFFFFu)
#define GE_PRIM_TRIANGLE     3u
#define GE_PRIM_SPRITES      6u

static u32 read_cmd(const GeList *list, u32 addr, bool *ok)
{
    /* Нормализация зеркал PSP: одна память видна по адресам
     * 0x08xxxxxx (кэш), 0x48xxxxxx (без кэша — гость сдаёт списки
     * GE именно через него), 0x88xxxxxx (KSEG0), 0xA8xxxxxx (KSEG1).
     * Маска `0x3FFFFFFF` приводит любое зеркало к кэшируемому. */
    u32 a = addr & 0x3FFFFFFFu;

    *ok = false;
    if ((a & 3u) != 0u) {          /* невыровнено */
        return 0u;
    }
    if (a < list->base || a + 4u > list->base + list->size) {
        return 0u;                 /* вне отображения гостя */
    }
    *ok = true;
    return list->mem[(a - list->base) >> 2];
}

bool ge_walk(const GeList *list, GeWalkResult *out)
{
    u32 pc;              /* адрес текущей команды */
    u32 base;            /* регистр BASE (0x10) */
    s32 base_off;        /* OFFSET_ADDR (0x13), со знаком */
    u32 steps;
    u32 call_top;
    u32 call_pc[GE_WALK_MAX_CALLS];
    s32 call_off[GE_WALK_MAX_CALLS];

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (list == NULL || list->mem == NULL || list->size == 0u) {
        return false;
    }

    pc       = list->list_addr;
    base     = 0u;
    base_off = 0;
    steps    = 0u;
    call_top = 0u;

    while (steps < GE_WALK_MAX_STEPS) {
        u32 cmd;
        u32 argument;
        u32 command;
        bool valid;

        cmd = read_cmd(list, pc, &valid);
        if (!valid) {
            break;                     /* вышли за список */
        }
        steps++;

        command  = cmd >> 24;
        argument = cmd & 0x00FFFFFFu;

        switch (command) {
        case GE_CMD_PRIM: {
            u32 type   = GE_PRIM_TYPE(argument);
            u32 verts  = GE_PRIM_VERTICES(argument);
            if (verts == 0u) {         /* jpcsp: вершин 0 — пропуск */
                pc += 4u;
                break;
            }
            if (verts > out->max_vertices) {
                out->max_vertices = verts;
            }
            if (type == GE_PRIM_SPRITES) {
                out->prim_sprites++;
                out->has_2d = true;
            } else if (type >= GE_PRIM_TRIANGLE && type <= 5u) {
                out->prim_3d++;
                out->has_3d = true;
            } else {
                out->prim_other++;
            }
            pc += 4u;
            break;
        }
        case GE_CMD_JUMP: {
            pc = (base | argument) + (u32)base_off;
            break;
        }
        case GE_CMD_CALL: {
            if (call_top < GE_WALK_MAX_CALLS) {
                call_pc[call_top]  = pc + 4u;   /* адрес возврата */
                call_off[call_top] = base_off;
                call_top++;
                if (call_top > out->calls) {
                    out->calls = call_top;
                }
            }
            pc = (base | argument) + (u32)base_off;
            break;
        }
        case GE_CMD_RET: {
            if (call_top > 0u) {
                call_top--;
                base_off = call_off[call_top];
                pc       = call_pc[call_top];
            } else {
                pc += 4u;   /* пустой стек: jpcsp PC не меняет */
            }
            break;
        }
        case GE_CMD_END: {
            /* конец списка — останавливаемся */
            out->steps = steps;
            return true;
        }
        case GE_CMD_BASE: {
            base = argument;
            pc += 4u;
            break;
        }
        case GE_CMD_OFFSET: {
            base_off = (s32)((u32)argument << 8);
            pc += 4u;
            break;
        }
        case GE_CMD_SIGNAL: {
            out->signal++;
            pc += 4u;
            break;
        }
        case GE_CMD_FINISH: {
            out->finish++;
            /* граница кадра; список может быть длиннее */
            pc += 4u;
            break;
        }
        default:
            /* Остальные — состояние и указатели вершин
             * (VADDR/IADDR/BASE-матрицы). Для диагностики
             * типов не важны; просто идём дальше. */
            pc += 4u;
            break;
        }
    }

    out->steps = steps;
    return true;
}
