@echo off
setlocal enabledelayedexpansion

:: Имя выходного файла
set "OUTPUT_FILE=project_sources.txt"

:: Очищаем или создаем выходной файл
echo. > "%OUTPUT_FILE%"

:: Ищем файлы .c, .h, Makefile, CMakeLists.txt рекурсивно (/s) в формате путей (/b)
:: findstr /v "\." исключает из путей любые папки или файлы, содержащие точку в начале имени каталога (например, \.git\ или \.vs\)
for /f "delims=" %%F in ('dir /s /b *.c *.h *Makefile* *CMakeLists.txt* *partitions.csv* *sdkconfig.defaults* *platformio.ini* 2^>nul ^| findstr /v "\\\."') do (
    
    :: Проверяем, не является ли файл самим выходным файлом или скриптом
    if /i not "%%~nxF"=="%OUTPUT_FILE%" if /i not "%%~nxF"=="%~nx0" (
        
        echo add to the list: %%~nxF...
        
        :: Записываем заголовок с путем к файлу
        echo ================================================== >> "%OUTPUT_FILE%"
        echo FILE: %%F >> "%OUTPUT_FILE%"
        echo ================================================== >> "%OUTPUT_FILE%"
        echo. >> "%OUTPUT_FILE%"
        
        :: Копируем содержимое файла
        type "%%F" >> "%OUTPUT_FILE%"
        
        echo. >> "%OUTPUT_FILE%"
        echo. >> "%OUTPUT_FILE%"
    )
)

echo Done^^! All sources and build files are saved in: %OUTPUT_FILE%
pause