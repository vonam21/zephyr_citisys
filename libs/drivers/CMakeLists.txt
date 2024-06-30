add_subdirectory_ifdef(CONFIG_LIBS_MODEM modem)

zephyr_system_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
list(APPEND SYSCALL_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include)
