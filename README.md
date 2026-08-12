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
проверенный эталон [Labelary](https://labelary.com/). Это критерий приёмки;
перечень реализованных команд пока не обеспечивает полную пиксельную
совместимость со всем демонстрационным набором.

Сейчас реализован рендеринг штрихкодов Code 128 (`^BC`), Code 39 (`^B3`),
EAN-13 (`^BE`) и квадратного DataMatrix ECC200 (`^BX`). Для Code 128
поддерживаются стандартные режимы Subset B, автоматический режим `A`, строгий
режим числовых пар `C` и управляющие последовательности Zebra. Режимы `U`/`D`
и необязательная контрольная цифра UCC Mod 10 явно не поддерживаются. GS1
DataMatrix выбирается идентификатором формата `1`; настроенный символ
экранирования поддерживает `x1` для FNC1 и десятичные последовательности байтов
`xdNNN` (например, `|d029` для разделителя GS).

## Сборка

```text
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
cmake --build build
ctest --test-dir build --output-on-failure
```

В Windows используйте вспомогательный скрипт, который настраивает окружение Qt
и MSVC и выполняет Debug-сборку в изолированном каталоге `build_agent_debug`:

```text
py -3 tools/build_helper.py all --clean
py -3 tools/build_helper.py test --config Debug
py -3 tools/build_helper.py build --target QtZpl
```

Чтобы установить динамическую библиотеку для использования в другом проекте,
задайте переменную `QTZPL_ROOT` либо явно передайте префикс установки:

```text
set QTZPL_ROOT=C:\QtZpl\RelWithDebInfo
py -3 tools/build_helper.py install --clean --config RelWithDebInfo
py -3 tools/build_helper.py install --install-prefix C:\QtZpl\RelWithDebInfo
```

По умолчанию используется Qt 6.11.1 с комплектом `msvc2022_64`. Для выбора
другого установленного комплекта предназначены параметры `--qt-version` и
`--compiler`.

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
