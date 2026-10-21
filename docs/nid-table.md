# Справочник NID и сопоставление импортов

## Откуда взялся справочник

Ключевой вопрос проекта: как назвать импорты, если в файле только числа.

Оказалось, NID — это **не хеш, а табличная константа**, и она прямо
записана в исходниках официального PSP SDK. Формат такой:

```asm
IMPORT_FUNC "sceDisplay",0x0E20F177,sceDisplaySetMode
```

То есть в SDK рядом стоят и NID, и текстовое имя. Скрипт
`tools/build_nid_table.py` выгружает все 105 файлов `.S` из `src/` и
разбирает их.

## Что получилось

```
файлов обработано    : 105 из 105
объявлений IMPORT_FUNC: 3115
уникальных NID       : 2661
безымянных           : 626
библиотек            : 95
```

Справочник: `data/psp_nids.tsv`, формат `NID, библиотека, имя, файл, unnamed`.

Безымянные — это экспорты, которые PSP SDK объявляет как
`sceMpeg_11CAB459`: имя в макросе совпадает с NID. Такие строки
помечены признаком `unnamed`, по ним назначение не следует.

## Сопоставление на реальном файле

`tools/resolve_imports.py tmp/real/EBOOT.dec.BIN`:

```
всего импортов      : 147
названо             : 143
есть, но безымянные : 1
нет в справочнике   : 3
покрытие именами    : 97.3%
```

**Блокер снят: имена функций получены.**

Найденные импорты выглядят так:

```
sceDisplay      sceDisplaySetMode, sceDisplaySetFrameBuf,
                sceDisplayGetVcount, sceDisplayWaitVblank,
                sceDisplayWaitVblankStart, sceDisplayGetFramePerSec
sceCtrl         sceCtrlReadBufferPositive, sceCtrlPeekBufferPositive
sceGe_user      sceGeListEnQueue, sceGeListSync, sceGeDrawSync,
                sceGeEdramGetSize, sceGeEdramSetAddrTranslation, ...
ThreadManForUser  sceKernelCreateThread, sceKernelStartThread,
                sceKernelExitThread, sceKernelDelayThread, ...
```

## Что осталось неназванным

| NID | Библиотека |
|---|---|
| `0x4571CC64` | sceMpeg |
| `0xF0A26395` | ModuleMgrForUser |
| `0xF77D77CB` | SysMemUserForUser |
| `0xB011922F` | sceAudio (безымянный экспорт в SDK) |

Первые три отсутствуют в этой ревизии SDK: функции появились позже.
Строки вида `sceMpegRingbufferQueryMemSize` в справочнике есть, а
`0x4571CC64` — нет, значит это более позднее добавление, либо внутренний
экспорт без документированного имени.

Это не препятствие: 143 из 147 покрыты, а оставшиеся можно определить
по месту использования в коде.

## Что из этого следует для подмены вызовов

Теперь известно, какой стаб соответствует какой функции. Значит:

* `sceDisplaySetMode`, `sceDisplaySetFrameBuf`, `sceDisplayWaitVblank`
  подменяются на наши — это ровно тот путь, что нужен для вывода кадра;
* `sceCtrlReadBufferPositive` — ввод, который сейчас недоступен
  (см. `player-recon.md`);
* `sceGe_*` — графика, 11 функций;
* `sceKernelCreateThread` — запуск гостевого кода;
* `sceIoOpen/Read/Close` — файловая система, её можно перенаправить на
  файлы HDD без обращения к PSP-овому UMD.

## Грабли

**NID в разных инструментах должен быть в одном формате.** Первый вариант
`elf_imports.py` отдавал NID как `int`, а справочник ключует по строке
`"0x4571CC64"`. Сопоставление дало 0% при 147 совпадениях по сути.
Формат зафиксирован строкой `"0x%08X"` в обоих местах.

Признак такой ошибки — NID в выводе десятичные и нулевое покрытие. Если
покрытие 0%, а импорты есть, сначала проверять формат NID, а не справочник.

## Проверка на плеере

Расшифровка проверена на целевой платформе, результат побайтово совпал
с хостовым:

```
md5 на плеере : d7b929af21c7ae3bf89824ddf0c40fb2
md5 на хосте  : d7b929af21c7ae3bf89824ddf0c40fb2
```

То есть `libkirk` корректно работает на MIPS-тулчейне проекта, и
расхождений из-за endianness или компилятора нет.