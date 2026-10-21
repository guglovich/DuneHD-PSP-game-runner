# Команды GE: спецификация из jpcsp

Канон по **формату команд** GE. Источник — `jpcsp` (GPL), файлы указаны
при каждом блоке. Всё ниже снято чтением кода, а не по памяти.

Зачем отдельно: игра **Magna Carta PSP — полноценная 3D**, а GE мы не
эмулируем. Значит разбор GE нужен нам для одного — отличать 2D-команды от
3D и не пытаться исполнять последнее.

## Формат слова команды

```
bit 31..24  = cmd         команда
bit 23..0   = normalArgument   аргумент (24 бита)
```

```java
// jpcsp: graphics/VideoEngine.java
public static int command(int instruction)      { return (instruction >>> 24); }
private static int intArgument(int instruction)  { return (instruction & 0x00FFFFFF); }
private static int instruction(int cmd, int value) {
    return intArgument(value) | (cmd << 24);
}
private static float floatArgument(int normalArgument) {
    return Float.intBitsToFloat(normalArgument << 8);   // сдвиг на 8, не на 24!
}
```

**`floatArgument` сдвигает аргумент на 8 разрядов**, не на 24. Это не
опечатка: у чисел с плавающей точкой в арге старший байт занимает команда,
и он выбрасывается именно так.

## Команды примитивов

```
0x00 NOP         0x01 VADDR        0x02 IADDR
0x04 PRIM        0x05 BEZIER       0x06 SPLINE
0x07 BBOX
```

`PRIM` — главная рисующая команда:

```java
// jpcsp: graphics/VideoEngine.java, executeCommandPRIM()
int numberOfVertex = normalArgument & 0xFFFF;
int type            = (normalArgument >> 16) & 0x7;
if (numberOfVertex == 0) return;
if (!Memory.isAddressGood(context.vinfo.ptr_vertex)) { error(...); return; }
```

| type | что рисует |
|---|---|
| `0` `PRIM_POINT` | точки |
| `1` `PRIM_LINE` | отрезки |
| `2` `PRIM_LINES_STRIPS` | ломаные |
| `3` `PRIM_TRIANGLE` | треугольники |
| `4` `PRIM_TRIANGLE_STRIPS` | полосы треугольников |
| `5` `PRIM_TRIANGLE_FANS` | веера треугольников |
| `6` **`PRIM_SPRITES`** | **квады 2D — то, что нам нужно** |
| `7` `PRIM_CONTINUE_PREVIOUS_PRIM` | продолжение предыдущего |

**Для 2D интересует `type = 6` (`PRIM_SPRITES`).** Вся 2D-отрисовка через
GE идёт именно спрайтами: два вершины, левый верх и правый низ.

## Переходы и вызовы

```
0x08 JUMP   прыжок на новый адрес
0x09 BJUMP  условный прыжок
0x0A CALL   вызов
0x0B RET    возврат
0x0C END    конец списка
```

**Адрес прыжка — относительный и с базой:**

```java
// jpcsp: HLE/kernel/types/PspGeList.java
public int getAddressRelOffset(int argument) {
    return Memory.normalizeAddress((videoEngine.getBase() | argument)
                                   + videoEngine.getBaseOffset());
}
public void jumpRelativeOffset(int argument) { setPc(getAddressRelOffset(argument)); }
public void ret() {
    if (!isStackEmpty()) {
        videoEngine.setBaseOffset(popStack());
        setPc(popStack());
    }
}
```

`base` — регистр GE `BASE` (команда `0x10`), смещение — `OFFSET_ADDR`
(`0x13`). То есть адрес в команде — **не** абсолютный, а `(BASE | arg) +
offset`. Это важно: абсолютный адрес из команды взять нельзя, нужны оба
регистра.

`CALL`/`RET` используют стек из двух слов: возвращаемый PC и текущий
`baseOffset`. При возврате база тоже восстанавливается.

## Завершение и сигналы

```
0x0E SIGNAL   прерывание сигнала
0x0F FINISH   завершение отрисовки
```

```java
// jpcsp: graphics/VideoEngine.java, executeCommandSIGNAL()
int behavior = (normalArgument >> 16) & 0xFF;
int signal   = normalArgument & 0xFFFF;
```

`FINISH` перед срабатыванием сохраняет GE в память и ждёт завершения всех
команд рендеринга (`fenceSync`/`clientWaitSync`). Для нас это значит:
после `FINISH` кадр считается готовым.

## Пропуск избыточных команд

jpcsp не исполняет команду повторно, если она «чисто состояние» и значение то
же:

```java
// jpcsp: graphics/GeCommands.java
public static boolean[] pureStateCommands;
...
if (GeCommands.pureStateCommands[command]) {
    if (currentListCMDValues[command] == instruction) { return; }
    currentListCMDValues[command] = instruction;
}
```

К чисто состоянию отнесены: `BASE`, `VTYPE`, `REGION1/2`, `LTE*`, `CPE`,
`BCE`, `TME`, `FGE`, `DTE`, `ABE`, `ATE`, `ZTE`, `STE`, `AAE`, `PCE`, `CTE`,
`LOE`, морф-веса `MW0..7`, `PSUB`, `PPRIM`, `PFACE`, `XSCALE`/`YSCALE` и
ряд матричных. Рисующие (`PRIM`, `BEZIER`, `SPLINE`, `BBOX`) и переходы
**не** пропускаются никогда.

## Отсюда — что нам нужно для 2D

1. **`PRIM` с `type = 6` (SPRITES)** — рисование 2D. Остальные типы для 2D
   не интересны, а `type = 3..5` — это уже 3D-геометрия.
2. **`VADDR`/`IADDR`** — указатели на вершины/индексы; для спрайтов
   индексы не нужны.
3. **`SIGNAL`/`FINISH`** — границы кадра.
4. **`BASE`/`OFFSET_ADDR`** — для корректного чтения адресов `JUMP`/`CALL`.

Итого минимальный разбор для 2D: распознать `PRIM` и его тип, пропускать
всё остальное. Это закрывает вопрос «2D или 3D» без эмуляции геометрии.

## Реализация

Ходячий — `src/ge_walk.c` (`ge_walk(const GeList *, GeWalkResult *)`).
Берёт плоский массив слов команд и адрес его начала в госте, обходит
`JUMP`/`CALL`/`RET` через `(BASE | arg) + baseOffset` (`baseOffset =
arg << 8` со знаком, по jpcsp), классифицирует `PRIM`:
`type = 6` → спрайты (2D), `type = 3..5` → 3D-геометрия, считает
`FINISH`/`SIGNAL` и максимум вершин. Цикл ограничен `GE_WALK_MAX_STEPS`,
глубина `CALL` — `GE_WALK_MAX_CALLS`, рекурсии нет.

Самопроверка — `tests/ge_walk_test.c` (синтетические списки: спрайты,
3D-треугольники, смешанный, `CALL`/`RET`, бесконечный `JUMP`, ноль
вершин, граничные). Проверено на хосте: `ge_walk_test: all passed`.

Замер по LocoRoco 2 (`UCES-01059`): `sceGe_user` — 11 импортов,
включая `sceGeListEnQueue`/`sceGeListEnQueueHead`. Значит игра
сдаёт списки в GE — рендер 3D, а не 2D-спрайты. Ходячий при запуске
должен подтвердить это подсчётом типов на реальных списках.
