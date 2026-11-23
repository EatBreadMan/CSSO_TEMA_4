Din "x64 Native Tools Command Prompt for VS", in folderul CSSO_Tema_4 cu sursele + t4_common.h:
  cl /W4 /std:c++17 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /O2 t4_worker.cpp t4_lib.cpp /Fe:t4_worker.exe /link shell32.lib
  cl /W4 /std:c++17 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /O2 t4_coord.cpp t4_engine.cpp t4_lib.cpp /Fe:t4_coord.exe /link shell32.lib
  cl /W4 /std:c++17 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /O2 t4_gui.cpp t4_engine.cpp t4_lib.cpp /Fe:t4_gui.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:t4_gui.manifest user32.lib gdi32.lib comctl32.lib ole32.lib uuid.lib shell32.lib

Sau pur si simplu rulati:
  build.bat

Testare:
  t4_coord.exe C:\T4\days 4
  python C:\T4\check_t4.py

Rulare Experimente (E1) si verificare:
  python run_experiments.py

Interfata grafica (GUI):
  t4_gui.exe
