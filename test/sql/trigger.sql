\! mkdir /tmp/plsh-test && chmod a+rwx /tmp/plsh-test

CREATE FUNCTION shtrigger() RETURNS trigger AS $$
#!/bin/sh
(
for arg do
    echo "Arg is $arg"
done

printenv | LC_ALL=C sort | grep '^PLSH_TG_'
) >> /tmp/plsh-test/foo
chmod a+r /tmp/plsh-test/foo
exit 0
$$ LANGUAGE plsh;

CREATE TABLE pfoo (a int, b text);

CREATE TRIGGER testtrigger AFTER INSERT ON pfoo
    FOR EACH ROW EXECUTE PROCEDURE shtrigger('dummy');

CREATE TRIGGER testtrigger2 BEFORE UPDATE ON pfoo
    FOR EACH STATEMENT EXECUTE PROCEDURE shtrigger('dummy2');

INSERT INTO pfoo VALUES (1, 'one');
INSERT INTO pfoo VALUES (2, 'two');
INSERT INTO pfoo VALUES (3, 'three');

UPDATE pfoo SET b = 'oneone' WHERE a = 1;

\! cat /tmp/plsh-test/foo
\! rm -r /tmp/plsh-test
