@echo off
cd /d "%~dp0"
cmake -S . -B build || exit /b 1
cmake --build build --config Release || exit /b 1
ctest --test-dir build -C Release --output-on-failure
echo.
echo Built. Run:  build\Release\novapm.exe  (GUI)   or  build\Release\novapm-cli.exe help
echo (build\novapm.exe if you used MinGW/Ninja)   Package: cd build ^&^& cpack -C Release
