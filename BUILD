Din "x64 Native Tools Command Prompt for VS", in folderul cu sursele + t4_common.h din kit:
  cl /W4 /std:c17 /DUNICODE /D_UNICODE t4_worker.cpp t4_lib.cpp /Fe:t4_worker.exe /link shell32.lib
  cl /W4 /std:c17 /DUNICODE /D_UNICODE t4_coord.cpp  t4_lib.cpp /Fe:t4_coord.exe  /link shell32.lib
Test:  t4_coord.exe <Kit>\T4\days 4   apoi   python <Kit>\T4\check_t4.py
 
