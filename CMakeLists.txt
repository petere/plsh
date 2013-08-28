cmake_minimum_required (VERSION 2.8)


set (PG_CONFIG "pg_config" CACHE FILEPATH "path to pg_config")

execute_process (COMMAND ${PG_CONFIG} --includedir-server
                 OUTPUT_VARIABLE pg_includedir_server
                 OUTPUT_STRIP_TRAILING_WHITESPACE)

if (pg_includedir_server)
  message ("includedir = ${pg_includedir_server}")
else (pg_includedir_server)
  message (FATAL_ERROR "no includedir found")
endif (pg_includedir_server)

execute_process (COMMAND ${PG_CONFIG} --pkglibdir
                 OUTPUT_VARIABLE pg_pkglibdir
                 OUTPUT_STRIP_TRAILING_WHITESPACE)

execute_process (COMMAND ${PG_CONFIG} --bindir
                 OUTPUT_VARIABLE pg_bindir
                 OUTPUT_STRIP_TRAILING_WHITESPACE)


include_directories (${pg_includedir_server})

set (CMAKE_SHARED_MODULE_PREFIX "")
set (CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-undefined,dynamic_lookup")
add_library (plsh MODULE plsh.c)

install (TARGETS plsh DESTINATION ${pg_pkglibdir})

add_custom_target (run ALL DEPENDS plsh.sql)

add_custom_command (OUTPUT plsh.sql
                    COMMAND cp ${CMAKE_SOURCE_DIR}/plsh-inline.sql plsh.sql
                    MAIN_DEPENDENCY plsh-inline.sql)

set (PG_REGRESS_TESTS init function trigger crlf psql inline)

add_custom_target (installcheck
                   COMMAND ${pg_pkglibdir}/pgxs/src/test/regress/pg_regress --psqldir='${pg_bindir}' --inputdir=${CMAKE_SOURCE_DIR}/test --dbname=contrib_regression ${PG_REGRESS_TESTS})
