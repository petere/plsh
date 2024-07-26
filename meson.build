project('plsh', ['c'])

pg_config = find_program('pg_config')

bindir = run_command(pg_config, '--bindir', check: true).stdout().strip()
includedir_server = run_command(pg_config, '--includedir-server', check: true).stdout().strip()
pkglibdir = run_command(pg_config, '--pkglibdir', check: true).stdout().strip()
sharedir = run_command(pg_config, '--sharedir', check: true).stdout().strip()

shared_module('plsh', 'plsh.c',
              include_directories: [includedir_server],
              install: true,
              install_dir: pkglibdir,
              name_prefix: '',
             )

install_data('plsh.control',
             'plsh--1--2.sql',
             'plsh--unpackaged--1.sql',
             install_dir: sharedir / 'extension',
            )

install_data('plsh-inline.sql',
             rename: 'plsh--2.sql',
             install_dir: sharedir / 'extension',
            )

pg_regress = find_program('pg_regress',
                          dirs: [pkglibdir / 'pgxs/src/test/regress']
                         )

regress_tests = ['init', 'function', 'trigger', 'crlf', 'psql', 'inline', 'event_trigger']

test('regress',
     pg_regress,
     args: ['--bindir', bindir,
            '--inputdir', meson.current_source_dir() / 'test',
           ] + regress_tests,
    )
