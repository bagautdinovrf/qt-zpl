# QtZpl

Нативная библиотека на C++23 и Qt 6.11 для разбора ZPL и растрового рендеринга этикеток. Библиотека не зависит от среды выполнения Go.

```cpp
auto result = QtZpl::render(zpl);
if (result) {
    const QList<QImage>& labels = result->labels;
}
```

По умолчанию парсер работает в отказоустойчивом режиме: неподдерживаемые команды сохраняются в публичной модели документа и добавляются в список диагностик.

Цель совместимости: каждый пример, опубликованный в
[веб-демо go-zpl](https://stirlingmarketinggroup.github.io/go-zpl/), должен
локально отрисовываться с той же геометрией и видимым содержимым, что и
проверенный эталон [Labelary](https://labelary.com/). Демо go-zpl даёт только
ZPL-фикстуры (`tests/corpus/go-zpl-demo/`); эталонные PNG — bitonal Labelary
(`tests/golden/go-zpl-demo/`). Это критерий приёмки; перечень реализованных
команд пока не обеспечивает полную пиксельную совместимость со всем
демонстрационным набором.

Сейчас реализован рендеринг штрихкодов Code 128 (`^BC`), Code 39 (`^B3`),
EAN-13 (`^BE`) и квадратного DataMatrix ECC200 (`^BX`). Для Code 128
поддерживаются стандартные режимы Subset B, автоматический режим `A`, строгий
режим числовых пар `C` и управляющие последовательности Zebra. Режимы `U`/`D`
и необязательная контрольная цифра UCC Mod 10 явно не поддерживаются. GS1
DataMatrix выбирается идентификатором формата `1`; настроенный символ
экранирования поддерживает `x1` для FNC1 и десятичные последовательности байтов
`xdNNN` (например, `|d029` для разделителя GS).

## Сборка

### Windows (рекомендуется)

Скрипт `tools/build_helper.py` сам поднимает окружение Qt 6.11 и MSVC 2022 и
вызывает CMake/Ninja.

**Без параметров** — собирает Debug и Release в отдельных каталогах, без тестов:

```text
py -3 tools/build_helper.py
py -3 tools/build_helper.py all --clean
```

Каталоги: `build_agent_debug`, `build_agent_release`.

**Одна конфигурация** — укажите `--config` (каталог подбирается автоматически):

```text
py -3 tools/build_helper.py all --clean --config Debug
py -3 tools/build_helper.py test --config Debug
py -3 tools/build_helper.py build --config Debug --target QtZpl

py -3 tools/build_helper.py all --clean --config Release
py -3 tools/build_helper.py build --config Release --target QtZpl
```

Свой каталог сборки — только если нужен нестандартный путь:

```text
py -3 tools/build_helper.py build --config Release --build-dir build_custom
```

Повторная сборка без переконфигурации:

```text
py -3 tools/build_helper.py build --config Debug
py -3 tools/build_helper.py build --config Release
```

### Установка в другой проект

```text
set QTZPL_ROOT=C:\QtZpl\Release
py -3 tools/build_helper.py install --clean --config Release --build-dir build_agent_release --install-prefix C:\QtZpl\Release
```

Debug-установка — `--config Debug`. Другой kit Qt — `--qt-version` и `--compiler`.

### Ручная сборка (CMake)

Если окружение Qt и MSVC уже настроено в терминале:

```text
cmake -S . -B build_debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
cmake --build build_debug
ctest --test-dir build_debug --output-on-failure

cmake -S . -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
cmake --build build_release
```

По умолчанию в `build_helper.py` используется Qt **6.11.1**, комплект **msvc2022_64**.
Параметры `--qt-version` и `--compiler` переключают другой kit из `C:\Qt`.

## Примеры рендеринга

Пример `qtzpl_gallery` создаёт несколько PNG-файлов из реальных строк ZPL:

```text
py -3 tools/build_helper.py gallery
```

Сгенерированные примеры сохраняются в каталоге `examples/rendered`.

Для рендеринга собственного ZPL вставьте его в переменную `ZPL` в корневом
файле `render_helper.py`, затем выполните:

```text
py -3 render_helper.py
```

Каждый блок `^XA...^XZ` сохраняется отдельным файлом `rendered/label_N.png`.

### Сравнение с Labelary golden

Утилита `qtzpl_compare_golden` рендерит ZPL и сравнивает результат с эталонным
PNG (ink bounds, число расхождающихся пикселей, Jaccard, diff-overlay):

```text
py -3 tools/build_helper.py build --config Debug --target qtzpl_compare_golden

build_agent_debug\examples\qtzpl_compare_golden.exe ^
  tests\corpus\go-zpl-demo\hello.zpl ^
  tests\golden\go-zpl-demo\hello-page-1-labelary-bitonal.png ^
  build_agent_debug\hello-actual.png ^
  build_agent_debug\hello-diff.png ^
  --width 812 --height 609 --dpi 203 --ignore-label-home
```

Параметры `--width`, `--height`, `--dpi` и `--ignore-label-home` берите из
`tests/corpus/go-zpl-demo/manifest.json` для соответствующей фикстуры. На
2026-08-12 для `hello.zpl` QR совпадает с Labelary, а текст Font 0 (`^A0` при
`^FO`) — нет; см. `AGENTS.md`.
