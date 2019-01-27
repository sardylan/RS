add_executable(rs41dm_dft
        rs41dm_dft.c
        demod_dft.c)

target_link_libraries(rs41dm_dft m ecc)

add_executable(dfm09dm_dft
        dfm09dm_dft.c
        demod_dft.c)

target_link_libraries(dfm09dm_dft m ecc)
