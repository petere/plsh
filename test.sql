\set ECHO all
DROP SCHEMA plsh_test CASCADE;
CREATE SCHEMA plsh_test;
SET search_path TO plsh_test;

CREATE FUNCTION valtest(text) RETURNS text AS 'foo' LANGUAGE plsh;

CREATE FUNCTION shtest (text, text) RETURNS text AS '
#!/bin/sh
echo "One: $1 Two: $2"
if test $1 = $2; then
    echo ''this is an error'' 1>&2
fi
exit 0
' LANGUAGE plsh;

SELECT shtest('foo', 'bar');
SELECT shtest('xxx', 'xxx');

CREATE FUNCTION shtrigger() RETURNS trigger AS '
#!/bin/sh
(
for arg do
    echo "Arg is $arg"
done
) >> /tmp/voodoo-pgplsh-test
exit 0
' LANGUAGE plsh;

CREATE TABLE pfoo (a int, b text);

CREATE TRIGGER testtrigger AFTER INSERT ON pfoo
    FOR EACH ROW EXECUTE PROCEDURE shtrigger('dummy');

INSERT INTO pfoo VALUES (1, 'one');
INSERT INTO pfoo VALUES (2, 'two');
INSERT INTO pfoo VALUES (3, 'three');

\! cat /tmp/voodoo-pgplsh-test
\! rm /tmp/voodoo-pgplsh-test
