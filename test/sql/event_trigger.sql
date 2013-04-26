\! mkdir /tmp/plsh-test && chmod a+rwx /tmp/plsh-test

CREATE FUNCTION evttrigger() RETURNS event_trigger AS $$
#!/bin/sh
(
echo "---"
for arg do
    echo "Arg is '$arg'"
done

printenv | LC_ALL=C sort | grep '^PLSH_TG_'
) >> /tmp/plsh-test/bar
chmod a+r /tmp/plsh-test/bar
exit 0
$$ LANGUAGE plsh;

CREATE EVENT TRIGGER testtrigger ON ddl_command_start
    EXECUTE PROCEDURE evttrigger();

CREATE TABLE test (a int, b text);
DROP TABLE test;

DROP EVENT TRIGGER testtrigger;

\! cat /tmp/plsh-test/bar
\! rm -r /tmp/plsh-test
