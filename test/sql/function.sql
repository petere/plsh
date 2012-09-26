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
