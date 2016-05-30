PL/sh Procedural Language Handler for PostgreSQL
================================================

PL/sh is a procedural language handler for PostgreSQL that allows you
to write stored procedures in a shell of your choice.  For example,

    CREATE FUNCTION concat(text, text) RETURNS text AS '
    #!/bin/sh
    echo "$1$2"
    ' LANGUAGE plsh;

The first line must be a `#!`-style line that indicates the shell to
use.  The rest of the function body will be executed by that shell in
a separate process.  The arguments are available as `$1`, `$2`, etc.,
as usual.  (This is the shell's syntax. If your shell uses something
different then that's what you need to use.)  The return value will
become what is printed to the standard output, with a newline
stripped.  If nothing is printed, a null value is returned.  If
anything is printed to the standard error, then the function aborts
with an error and the message is printed.  If the script does not exit
with status 0 then an error is raised as well.

The shell script can do anything you want, but you can't access the
database directly.  Trigger functions are also possible, but they
can't change the rows.  Needless to say, this language should not be
declared as `TRUSTED`.

The distribution also contains a test suite in the directory `test/`,
which contains a simplistic demonstration of the functionality.

I'm interested if anyone is using this.

Peter Eisentraut <peter@eisentraut.org>

Database Access
---------------

You can't access the database directly from PL/sh through something
like SPI, but PL/sh sets up libpq environment variables so that you
can easily call `psql` back into the same database, for example

    CREATE FUNCTION query (x int) RETURNS text
    LANGUAGE plsh
    AS $$
    #!/bin/sh
    psql -At -c "select b from pbar where a = $1"
    $$;

Note: The "bin" directory is prepended to the path, but only if the `PATH` environment variable is already set.

Triggers
--------

In a trigger procedure, trigger data is available to the script
through environment variables (analogous to PL/pgSQL):

* `PLSH_TG_NAME`: trigger name
* `PLSH_TG_WHEN`: `BEFORE`, `INSTEAD OF`, or `AFTER`
* `PLSH_TG_LEVEL`: `ROW` or `STATEMENT`
* `PLSH_TG_OP`: `DELETE`, `INSERT`, `UPDATE`, or `TRUNCATE`
* `PLSH_TG_TABLE_NAME`: name of the table the trigger is acting on
* `PLSH_TG_TABLE_SCHEMA`: schema name of the table the trigger is acting on

Event Triggers
--------------

In an event trigger procedure, the event trigger data is available to
the script through the following environment variables:

* `PLSH_TG_EVENT`: event name
* `PLSH_TG_TAG`: command tag

Inline Handler
--------------

PL/sh supports the `DO` command.  For example:

    DO E'#!/bin/sh\nrm -f /tmp/file' LANGUAGE plsh;

If data do not fit command line
-------------------------------

In some cases you may hit the limitations of command line passing data to PL/sh.
In that case you may find useful plsh2 language which is similar to plsh with the first
argument of a function is passed via stdin to the shell while the second and next arguments
are passed as `$1`, `$2`, etc.


Installation
------------

You need to have PostgreSQL 8.4 or later, and you need to have the
server include files installed.

To build and install PL/sh, use this procedure:

    make
    make install

The include files are found using the `pg_config` program that is
included in the PostgreSQL installation.  To use a different
PostgreSQL installation, point configure to a different `pg_config` like
so:

    make PG_CONFIG=/else/where/pg_config
    make install PG_CONFIG=/else/where/pg_config

Note that generally server-side modules such as this one have to be
recompiled for every major PostgreSQL version (that is, 8.4, 9.0,
...).

To declare the language in a database, use the extension system with
PostgreSQL version 9.1 or later.  Run

    CREATE EXTENSION plsh;

inside the database of choice.  To upgrade from a previous
installation that doesn't use the extension system, use

    CREATE EXTENSION plsh FROM unpackaged;

Use `DROP EXTENSION` to remove it.

With versions prior to PostgreSQL 9.1, use

    psql -d DBNAME -f .../share/contrib/plsh.sql

with a server running.  To drop it, use `droplang plsh`, or `DROP
FUNCTION plsh_handler(); DROP LANGUAGE plsh;` if you want to do it
manually.

Test suite
----------

[![Build Status](https://secure.travis-ci.org/petere/plsh.png)](http://travis-ci.org/petere/plsh)

To run the test suite, execute

    make installcheck

after installation.
