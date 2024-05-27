set(PROJECT_LIBS_BASE ${CMAKE_CURRENT_SOURCE_DIR})

if (CONFIG_LIBS_PROJECT)
    list(APPEND SYSCALL_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include)

    zephyr_sources(utils.c)
    add_subdirectory(drivers)
endif()

