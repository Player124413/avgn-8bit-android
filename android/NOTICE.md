# Сторонние компоненты и лицензии

Порт для Android не был бы возможен без чужого кода. Ниже — что именно используется, откуда взято и под
какой лицензией распространяется.

## Butterscotch — рантайм GameMaker (AGPL-3.0)

`runtime/` — это **Butterscotch**, свободный рантайм, который исполняет `data.win`:

* апстрим: <https://github.com/ButterscotchRunner/Butterscotch>, коммит `46b572b`;
* полный текст лицензии: `runtime/LICENSE` (GNU AGPL-3.0).

Взятая за основу копия — не чистый апстрим, а **форк из PS Vita-порта**: файл `source/runtime-src.tar.gz`
внутри `AVGN8bit-Vita-1.10.vpk` в этом репозитории. В форке 21 изменённый файл ядра относительно
апстрима (в основном `gl/gl_renderer.c` + `gl_renderer.h`, `runner.c`, `data_win.c`, `event_table.c`,
`vm.c`, `renderer.h`, `common.h`), плюс свои файлы: `src/vita/*`, `src/boot_progress.*`,
`src/font_ttf.*` (FreeType), `src/image/txtr_stream.*`, `src/loop.c`.

Что изменил уже этот порт (Android):

* `runtime/src/android/main.c` — JNI-класс переименован в `ru/avgn8bit/Native`, добавлен
  `setBaseDir()` (рантайм переключается в приватную папку приложения, чтобы файлы-маркеры, кэш
  текстур и логи писались рядом с `data.win`);
* удалены платформы и бэкенды, которые на Android не компилируются (PS2/PS3/Switch/Web,
  `src/video/ffmpeg`, десктопный `src/loop.c`); сборка — `android/native/CMakeLists.txt`;
* добавлен `android/` целиком: Gradle-проект, Compose-интерфейс, сенсорное управление, редактор
  раскладки, сборщик `data_vita.win` (`GameDataPatcher.kt`), шейдеры блита и CRT.

Код порта (Kotlin + CMake + правки в `runtime/`) распространяется по той же **AGPL-3.0**, потому что
является производной работой от Butterscotch.

`android/app/src/main/kotlin/ru/avgn8bit/GameGL.kt` — EGL-обёртка, переработанная из
`src/android/frontend/.../ButterscotchEGL.kt` апстрима (та же схема «контекст живёт дольше окна»,
добавлены `teardown`/`hasSurface`/`updateSize`).

## Патч игры

`android/app/src/main/assets/data_vita.patch` — тот же дифф `data.win → data_vita.win`, что лежит в
Vita-сборке (`data_vita.patch` внутри VPK). Это **не файлы игры**: 568 КБ инструкций «скопировать
такой-то кусок исходного data.win». Сама игра, её графика, звук, тексты и видео принадлежат
правообладателям и в репозиторий не входят — приложение читает копию, которую вы импортируете.

## Библиотеки в `runtime/vendor/`

| Компонент | Лицензия | Файл |
|---|---|---|
| FreeType 2.13.3 (`font_add`) | FTL / GPLv2 (двойная) | `vendor/freetype/LICENSE.TXT` |
| Box2D (C-порт, физика) | MIT | `vendor/box2d/LICENSE` |
| miniz (zlib/deflate) | MIT | `vendor/miniz/LICENSE` |
| miniaudio (звук, OpenSL ES) | public domain / MIT-0 | `vendor/miniaudio/miniaudio.h` |
| bzip2 1.0.8 (`vendor/bzip2`) | BSD-подобная (bzip2 license) | заголовки файлов |
| stb_image / stb_vorbis / stb_ds | MIT / public domain | заголовки файлов |
| SHA-1 (Steve Reid) | public domain | `vendor/sha1/sha1.c` |
| MD5 (RSA Data Security) | разрешительная, см. текст | `vendor/md5/md5.c` |
| base64 (WEI Zhicheng) | public domain | `vendor/base64/base64.c` |

## Шрифты и иконка

Шрифты игры (`Silver.ttf`, `LanaPixel.ttf`, `Probly8-CJK.ttf`) не поставляются с портом — они
берутся из вашей копии игры при импорте. Иконка приложения нарисована для этого порта (векторная,
`android/app/src/main/res/mipmap-*/`, `drawable/ic_launcher_foreground.xml`).

## Отказ от притязаний

Игра *Angry Video Game Nerd 8-bit* и все её ассеты принадлежат их авторам и издателям. Этот
репозиторий не связан с ними, не распространяет игру и не претендует на неё. Используя порт, вы
работаете со своей легальной копией игры.
