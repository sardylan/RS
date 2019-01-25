add_executable(rs41 rs41.c)
target_link_libraries(rs41 m ecc)

add_executable(rs41ecc rs41ecc.c)
target_link_libraries(rs41ecc m ecc)

add_executable(rs41ptu rs41ptu.c)
target_link_libraries(rs41ptu m ecc)

add_executable(rs41sg rs41sg.c)
target_link_libraries(rs41sg m ecc)