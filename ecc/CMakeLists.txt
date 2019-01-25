add_library(ecc
        bch_ecc.h
        bch_ecc.c)

target_include_directories(ecc
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})