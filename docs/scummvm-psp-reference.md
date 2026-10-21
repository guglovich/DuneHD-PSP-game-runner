# ScummVM-PSP как тестовый гость — проверка предложения

Проверено 2026-10-13 по дереву `scummvm/scummvm@master`, каталог
`backends/platform/psp/`. Всё ниже — из прочитанных файлов; непроверенное
помечено явно. Предложение, из которого взят этот разбор, утверждало, что
ScummVM-PSP — «идеальный тестовый гость» именно потому, что он рисует «через
GU/GE» и потому «проверяет GE-интерпретатор».

**Главный вывод: как гость для HLE — да и с высокой ценой; как гость для
GE-интерпретатора — нет, и это не мелочь.** ScummVM-PSP не использует GE ни
разу. Он рисует через GU — немедленный режим. Это другой конвейер, и наш
основной проект (LocoRoco 2) упирается именно в GE.

---

## 1. Состав бэкенда (прочитан каталог целиком)

`psp.mk`, `module.mk`, `osys_psp.cpp`, `thread.cpp`, `memory.cpp`,
`powerman.cpp`, `input.cpp`, `pspkeyboard.cpp`, `cursor.cpp`, `rtc.cpp`,
`trace.cpp`, `audio.cpp`, `mp3.cpp`, `png_loader.cpp`, `image_viewer.cpp`,
`display_client.cpp`, `display_manager.cpp`, `default_display_client.cpp`,
`psppixelformat.cpp`, `tests.cpp`, `dummy.cpp`, `kbd.zip`, `README.PSP`.

`module.mk` перечисляет 21 объект, все они входят в `EBOOT`.

## 2. Графика: GU, а не GE — главная поправка

Прочитаны оба файла графики целиком.

`display_client.cpp` (`GuRenderer`, `Palette`, `Buffer`, `Vertex`):

```c
sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, vertices);
```

`display_manager.cpp` (`MasterGuRenderer`, `DisplayManager`, `VramAllocator`):

```c
sceGuInit();  sceGuStart(0, _displayList);
sceGuDrawBuffer(GU_PSM_8888, (void *)0, PSP_BUFFER_WIDTH);
sceGuDispBuffer(480, 272, (void *)(PSP_FRAME_SIZE * sizeof(uint32)), PSP_BUFFER_WIDTH);
sceGuOffset(...); sceGuViewport(2048, 2048, 480, 272); sceGuScissor(...);
sceGuClear(...); sceGuFinish(); sceGuSync(0, 0);
sceGuSwapBuffers(); sceDisplayWaitVblankStart();
sceGuTerm();
```

Ни одного `sceGe*` в прочитанных файлах нет. Разбор GE в бэкенде не
проводился — `trace.cpp`/`osys_psp.cpp` не читались; вывод «GE не используется»
относится к файлам графики, а не ко всему бэкенду.

**Почему это важно для нас.** Собственные измерения LocoRoco 2 дают:
`sceGeListEnQueue` 3, `sceGeListSync` 2, `sceGeDrawSync` 2,
`sceGeEdramGetAddr` 7 — игра отдаёт GE списки команд в EDRAM. Это и есть
нерешённая задача проекта (GE-интерпретатор). GU и GE — разные машины:
у GU свой конвейер и свой список передачи (`_displayList[2048]`, передаётся
`sceGuStart(0, list)`), у GE — EDRAM-списки. Общий у них только интерфейс
ожидания: `sceGuSync(0,0)` против `sceGeDrawSync`/`sceGeListSync`.

**Следствие.** ScummVM-PSP закрывает HLE-часть проекта (потоки, семафоры,
callback'и, таймеры, ввод, питание, память, файлы), но **не даёт нагрузки на
GE-интерпретатор**. Если приоритет — GE, нужен другой гость: коммерческая
игра с GE или собственный синтетический список GE.

## 3. Аудио: MPEG используется — «без MPEG/ATRAC» неверно

`README.PSP`, дословно:

> The PSP version of ScummVM uses the Media Engine to accelerate decoding of
> MP3 files. This means that if you have the choice of compressing using Ogg
> files or MP3 files, choose MP3 -- the game will generally run faster.

Плюс `mp3.cpp` (14 301 Б) входит в `MODULE_OBJS`, и в `README.PSP` в списке
зависимостей есть **libmad** и **libTremor** (помечены опциональными), то есть
декодирование Ogg тоже присутствует.

Media Engine на PSP — это `sceMpeg`/`libmpeg` (Irmillion). Значит заявка
«весь HLE-набор без MPEG/ATRAC» не соответствует действительности: MPEG в
наборе есть, и это один из самых тяжёлых кусков PSP API.

Практическое следствие: **брать ScummVM-PSP первым гостем нельзя** — он
зацепит MPEG и уведёт нас в самую дорогую часть HLE, прежде чем будет
проверена базовая работа с потоками и памятью.

## 4. Что действительно ценно

**4.1 Кэш-когерентность с графикой — сделана явно и подтверждена кодом.**

```c
void GuRenderer::cacheInvalidate(void *pointer, uint32 size) {
    sceKernelDcacheWritebackInvalidateRange(pointer, size);
}
```

вызывается на каждом выделении буфера и палитры, **до** перевода адреса в
uncached. То есть порядок «сбросить кэш → отдать указатель графике»
обязателен. Это ровно то, что наша графика тоже обязана делать; проверено и
зафиксировано.

**4.2 Uncached-зеркало используется для всего, что отдаётся графике.**

```c
_values = memalign(64, amountInBytes);
GuRenderer::cacheInvalidate(_values, amountInBytes);
_values = UNCACHED(_values);
```

То же в `Buffer::allocate`. Выравнивание 64 байта. Указание «VRAM и
uncached-зеркало» из предложения подтверждено, но конкретнее: uncached
нужен не только для VRAM, а для **любого** буфера, который читает GU.

**4.3 CLUT8 подтверждён.**

`GU_PSM_T8` / `GU_PSM_T4`, `sceGuClutMode(...)`, `sceGuClutLoad(...)`;
палитра 8 бит = 256 записи, 4 бита = 16. Есть `flipNibbles()` для
«странного» 4-битного формата PSP.

**4.4 Swizzle не используется.**

`sceGuTexImage(0, texW, texH, stride, ptr)` — линейная память со
stride'ом. Swizzle-текстур в бэкенде нет; упоминание swizzle в предложении
этим кодом не подтверждается.

**4.5 Размеры текстур — степени двойки, максимум 512.**

`scaleUpToPowerOfTwo()` ограничивает 512; кадр режется на плитки 512×512.
Экран 480×272, буфер 512×272×4.

**4.6 Потоки, семафоры и callback'и — да, и нагрузка настоящая.**

`thread.cpp` в составе; `DisplayCbThread` с приоритетом
`PRIORITY_DISPLAY_THREAD` и стеком `STACK_DISPLAY_THREAD`; семафор
`_renderSema.take()/.give()` для синхронизации с рендером.

Отдельно ценно: **`sceKernelCreateCallback` / `sceKernelSleepThreadCB` /
`sceKernelNotifyCallback`** — callback-модель вместо `sceGeDrawSync`. Для
нашего планировщика это другой и не менее нужный набор примитивов.

**4.7 Прочее из HLE-набора, по составу подтверждено:** `sceCtrl` (аналог как
мышь, `input.cpp`, `pspkeyboard.cpp`), питание (`powerman.cpp`), RTC
(`rtc.cpp`), память (`memory.cpp`), `sceIo`/файлы, `sceAudio`.

## 5. Требования к сборке и риски для нашего рантайма

Из `README.PSP` и `psp.mk`:

| что | значение |
|---|---|
| тулчейн | `svn://svp.pspdev.org/psp/trunk/psptoolchain` (psptoolchain + pspsdk) |
| configure | `../../configure --host=psp --enable-plugins --default-dynamic` |
| обязательные библиотеки | zlib, libpng |
| опциональные | libmad, libTremor |
| финальная сборка | `strip` → **`psp-fixup-imports`** → `pack-pbp` |
| `EBOOT.PBP` | `param.sfo`, `icon0.png`, NULL, `pic0.png`, `pic1.png`, NULL, `scummvm_stripped`, NULL |
| `PARAM.SFO` | `mksfoex -d MEMSIZE=1` — **MEMSIZE = 1 МиБ** |
| версия | `README.PSP` помечен `2026.4.1git`, схема версий `ГГГГ.М.Р` |

Два риска, оба существенные для плана «M1.5»:

1. **`--default-dynamic` требует загрузки модулей во время работы гостя.**
   ScummVM грузит игровые движки как PSP-модули. Наш рантайм не имеет ни
   `dlopen`, ни `sceKernelLoadModule` с загрузкой образа. Брать сборку с
   `--default-dynamic` — значит сразу упираться в HLE загрузчика модулей.
   Для приёмочного теста HLE это лишнее: нужен `--disable-dynamic-loading`
   (статические плагины).
2. **Версия.** Предложение советует «старый релиз 1.x». Схема версий
   ScummVM — `ГГГГ.М.Р`, релизов «1.x» нет. Текущий `master` помечен
   `2026.4.1git`. Отдельного смысла брать именно старый релиз нет; выбор
   версии должен следовать из нужного набора движков, а не из «покороче».

**MEMSIZE=1 в `PARAM.SFO`** стоит учесть отдельно: это размер раздела,
запрашиваемый модулем при загрузке. Наша подмена
`sceKernelAllocPartitionMemory`/`sceKernelGetBlockHeadAddr` должна корректно
относиться к этому значению — наш текущий гость, судя по отчёту, работает
с `AllocPartitionMemory(2, …)`.

## 6. Архитектурный урок — подтверждается

«Жёсткая граница переносимое ядро / платформенный бэкенд» в ScummVM
реализована как `OSystem` + `backends/platform/psp`. Игровые движки в
`engines/` ничего не знают о PSP; всё PSP-специфичное — в 21 объекте
бэкенда. У нас та же граница уже есть и неявна: загрузчик, HLE и GE-интерпретатор
разрабатываются на хосте с оракулом PPSSPP, а Dune-специфика изолирована в
плагине. Ничего нового отсюда брать нечего — урок полезен как подтверждение,
а не как образец для копирования.

## 7. Решение

**Брать, но не первым и не для GE.** Порядок применения:

| роль | оценка |
|---|---|
| Приёмочный тест HLE: потоки, семафоры, callback, таймеры, память, файлы | **годен**, при сборке без динамической загрузки и без аудио-движков |
| Нагрузка на GE-интерпретатор | **не годен** — GE не использует |
| Нагрузка на SIGILL/VFPU | **не проверено** — рукописных MIPS/VFPU в бэкенде я не искал; в `engines/` их тоже нет, они в ScummVM отсутствуют как класс, движки написаны на C++. Утверждение «содержит рукописные MIPS/VFPU-оптимизации» **не подтверждено и, скорее всего, неверно** |
| Аудио | **мешает** — тянет MPEG |

Один пункт в предложении («1.x достаточно, он меньше и проще») неверен по
схеме версий, один («без MPEG/ATRAC») неверен по составу, один («GE-интерпретатор»
— главная ценность) неверен по факту отсутствия GE. Остальное — про потоки,
2D-профиль, uncached-зеркало и кэш-когерентность — подтвердилось.

**Практический первый шаг, если всё же брать:** собрать без
`--default-dynamic` и без `mp3.o`, вынуть ELF из `EBOOT.PBP` (инструмент уже
есть — для `~PSP`-шифрованных EBOOT он уже разбирался в этом проекте) и
прогнать под рантаймом. Ожидаемая ценность: трасса стартовой
последовательности NID от crt0 pspsdk как вторая точка сверки с PPSSPP
поверх гипотез о неопознанных NID вида `0xF77D77CB`.

## Что осталось непроверенным

- `osys_psp.cpp`, `thread.cpp`, `memory.cpp`, `input.cpp`, `audio.cpp`,
  `mp3.cpp`, `trace.cpp`, `powerman.cpp`, `rtc.cpp` — **не читал**. Утверждения
  про потоки и семафоры подтверждены по `display_manager.cpp` и составу
  `module.mk`, а не по `thread.cpp` целиком.
- GE в этих файлах — не искал.
- Рукописные MIPS/VFPU — не искал нигде.
- Полный список NID импортов собранного ELF — потребует самой сборки.