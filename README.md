## Введение
Этот репозиторий является форком исходного репозитория userland.
Весь репозиторий нужен фактически для работы с одним исходником - `farvcam.c`, который находится в директории `host_applications/linux/apps/raspicam/farvcam.c`. Код создан на базе программ `raspivid.c`, `raspistill.c`, а также с помощью советов на форумах raspberry pi и stackoverflow.
Исходник `farvcam.c` является ключевой программой для видеорегистратора farvater и имеет следующие функции:
1. Запись видео по состоянию цифрового выхода телематического терминала CAN-WAY.
2. Получение фотоснимка по запросу терминала и передача фотографии на сервер мониторинга (например Wialon IPS).
3. Сохранение фотографий и видео на SD-карте расберри, при этом файлы сохраняются в папке, работающей как образ флеш-накопителя.

Разрешение видео по умолчанию - 1920 на 1080. Получение фотографии можно выполнять одновременно с записью видео, поскольку фактически используется кадр из видеопотока. Поэтому фотография имеет такое же разрешение, как и видео (1920 на 1080).
**Программа ещё сырая и многие вещи в ней еще не доделаны и не проверены**.

Вся работа с проектом ведётся с помощью `CMake` в среде VScode (с расширением `CMake`). Версия `CMake` на момент написания `README.md` - 3.17.3.
Для работы с исходником с помощью *кросс-компиляции* нужно сделать следующие основные шаги.
## Настройка toolchain
Для данного проекта использовался следующий toolchain: https://github.com/Pro/raspi-toolchain
Чтобы получить и установить toolchain нужно следовать инструкции варианта **Build the toolchain from source**. После этого должен установиться С компилятор `/opt/cross-pi-gcc/bin/arm-linux-gnueabihf-gcc-8.3.0`.
## Установка библиотеки pigpio в raspberry pi
Нужно установить библиотеку pigpio в расберри, поскольку она используется в `farvcam.c`. Ссылка на инструкцию по установке https://abyz.me.uk/rpi/pigpio/download.html.
## Синхронизация файлов `rootfs` raspberry pi
Этот пункт нужно выполнять в соответствии с этой инструкцией [Cross Compiler CMake Usage Guide with rsynced Raspberry Pi 32 bit OS](https://github.com/abhiTronix/raspberry-pi-cross-compilers/wiki/Cross-Compiler-CMake-Usage-Guide-with-rsynced-Raspberry-Pi-32-bit-OS#cross-compiler-cmake-usage-guide-with-rsynced-raspberry-pi-32-bit-os). При этом шаги 1-3 из раздела **Steps/Settings for Host Machine (PC/Laptop)** можно пропустить, потому что мы уже настроили toolchain на предыдущем шаге. Также стоит отметить, что шаг **8. Setup Important Symlinks** может сработать не совсем верно (не факт что потом проект соберётся), поэтому вместо команд на шаге 8 нужны эти:
```
 sudo ln -sf /usr/include/arm-linux-gnueabihf/asm/* /usr/include/asm/
 sudo ln -sf /usr/include/arm-linux-gnueabihf/gnu/* /usr/include/gnu/
 sudo ln -sf /usr/include/arm-linux-gnueabihf/bits/* /usr/include/bits/
 sudo ln -sf /usr/include/arm-linux-gnueabihf/sys/* /usr/include/sys/
 sudo ln -sf /usr/include/arm-linux-gnueabihf/openssl/* /usr/include/openssl/
 sudo ln -sf /usr/lib/arm-linux-gnueabihf/crtn.o /usr/lib/crtn.o
 sudo ln -sf /usr/lib/arm-linux-gnueabihf/crt1.o /usr/lib/crt1.o
 sudo ln -sf /usr/lib/arm-linux-gnueabihf/crti.o /usr/lib/crti.o
 ```
 Также в этой инструкции нам не нужны пункты с созданием проекта, поскольку проект уже есть (useland). 
 Стоит отметить, что мне удавалось делать кросс-компиляцию userland без указанной инструкции; использовались шаги из этой ссылки - второй ответ [Building for newer Raspbian Debian Buster images and ARMv6](https://stackoverflow.com/questions/19162072/how-to-install-the-raspberry-pi-cross-compiler-on-my-linux-host-machine). Однако инструкция [Cross Compiler CMake Usage Guide with rsynced Raspberry Pi 32 bit OS](https://github.com/abhiTronix/raspberry-pi-cross-compilers/wiki/Cross-Compiler-CMake-Usage-Guide-with-rsynced-Raspberry-Pi-32-bit-OS#cross-compiler-cmake-usage-guide-with-rsynced-raspberry-pi-32-bit-os) всё-таки предпочтительнее, поскольку в ней есть исправление `symlinks` с помощью питоновского скрипта. 
## Настройка репозитория userland
Важный файл тулчейна https://github.com/Pro/raspi-toolchain - `Toolchain-rpi.cmake`, без него ничего собираться не будет. В нём необходимо добавить путь к директории `rootfs`, в которую копировались все системные файлы расберри из предыдущего шага. Для этого нужно добавить следующую строку в файл `Toolchain-rpi.cmake`:
```cmake
set(ENV{RASPBIAN_ROOTFS} "$HOME/Projects/raspberrypi/rootfs") # здесь нужно указать путь к директории rootfs
```
перед условием `if("$ENV{RASPBIAN_ROOTFS}" STREQUAL "")`.
Также нужно добавить строку
```cmake
set(ENV{RASPBERRY_VERSION} 1)
```
в самом начале файла.
Сам файл `Toolchain-rpi.cmake` можно расположить в любом месте, хотя правильнее перенести его в директорию `makefiles/cmake/toolchains`.
В среде VScode для конфигурации проекта необходимо открыть папку userland. Для удобства настройки проекта используется файл `cmake-kits.json`, в котором прописан компилятор и тулчейн. Если собирать проект без привязки к VScode то нужно при запуске `CMake` указывать путь к тулчейн файлу и путь к компилятору. Для этого сначала нужно создать папку `build` и перейти в неё:
```
mkdir build
cd build
```
Затем нужно выполнить такую команду (**это еще надо проверить**):
```
/usr/local/bin/cmake --no-warn-unused-cli -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_C_COMPILER:FILEPATH=/opt/cross-pi-gcc/bin/arm-linux-gnueabihf-gcc-8.3.0 -DCMAKE_TOOLCHAIN_FILE:FILEPATH=toolchain_path/Toolchain-rpi.cmake ..
```
Однако эта команда выдаст ошибку, что не удалось найти библиотеку `pigpio`. Чтобы CMake смог найти эту библиотеку необходимо создать файл `pigpioConfig.cmake` со следующим содержимым:
```cmake
################################################################################
### Find the pigpio shared libraries.
################################################################################

# Find the path to the pigpio includes.
find_path(pigpio_INCLUDE_DIR 
    NAMES pigpio.h pigpiod_if.h pigpiod_if2.h
    HINTS /home/xbron/Projects/raspberrypi/rootfs/usr/local/include) # указывается путь к include, где установлены заголовочные файлы в rootfs
    
# Find the pigpio libraries.
find_library(pigpio_LIBRARY 
    NAMES libpigpio.so
    HINTS /home/xbron/Projects/raspberrypi/rootfs/usr/local/lib) # указывается путь к lib, где установлены файлы библиотеки в rootfs
find_library(pigpiod_if_LIBRARY 
    NAMES libpigpiod_if.so
    HINTS /home/xbron/Projects/raspberrypi/rootfs/usr/local/lib) # указывается путь к lib, где установлены файлы библиотеки в rootfs
find_library(pigpiod_if2_LIBRARY 
    NAMES libpigpiod_if2.so
    HINTS /home/xbron/Projects/raspberrypi/rootfs/usr/local/lib) # указывается путь к lib, где установлены файлы библиотеки в rootfs
    
# Set the pigpio variables to plural form to make them accessible for 
# the paramount cmake modules.
set(pigpio_INCLUDE_DIRS ${pigpio_INCLUDE_DIR})
set(pigpio_INCLUDES     ${pigpio_INCLUDE_DIR})

# Handle REQUIRED, QUIET, and version arguments 
# and set the <packagename>_FOUND variable.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(pigpio 
    DEFAULT_MSG 
    pigpio_INCLUDE_DIR pigpio_LIBRARY pigpiod_if_LIBRARY pigpiod_if2_LIBRARY)
```
Далее этот файл нужно поместить в директорию `cmake` папки rootfs, `$HOME/Projects/raspberrypi/rootfs/usr/local/lib/cmake/`.
При этом есть вероятность, что собрать проект всё равно не удастся. Тогда нужно удалить все файлы в `build` и попробовать снова. Если ошибок нет, то следует далее ввести команду `make -j`, находясь в папке `build`.

В VScode с расширением CMake необходимо выбрать цель (target) `farvcam` и затем нажать `Build`. При этом `CMake` может поругаться, что не может найти `pigpio`, тогда нужно в VScode нажать `Сtrl+Shift+P` и выбрать `CMake: Clean`, после чего попробовать снова собрать проект.
