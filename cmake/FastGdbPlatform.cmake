# Platform-specific source configuration applied after targets are declared.
# MinGW exposes a narrow variadic ::open from its CRT. Only gdb_table.cpp needs
# the UTF-8 CreateFileW redirect, so keep the preprocessor switch scoped to that
# translation unit rather than leaking an `open` macro through public headers.
if(WIN32 AND MINGW)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/gdb_table.cpp"
        PROPERTIES COMPILE_DEFINITIONS FAST_GDB_REDIRECT_POSIX_OPEN=1)
endif()
