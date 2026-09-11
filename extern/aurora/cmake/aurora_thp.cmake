add_library(aurora_thp STATIC
        lib/dolphin/thp/THPAudio.cpp
        lib/dolphin/thp/THPDec.cpp
)
add_library(aurora::thp ALIAS aurora_thp)
set_target_properties(aurora_thp PROPERTIES FOLDER "aurora")
target_link_libraries(aurora_thp PUBLIC aurora::core)
