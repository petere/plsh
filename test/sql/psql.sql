CREATE TABLE pbar (a int, b text);
INSERT INTO pbar VALUES (1, 'one'), (2, 'two');

CREATE FUNCTION query (x int) RETURNS text
LANGUAGE plsh
AS $$
#!/bin/sh
if command -v psql >/dev/null; then
    psql -X -At -c "select b from pbar where a = $1"
else
    echo 'no PATH?' 1>&2
fi
$$;

SELECT query(1);
