\! mkdir /tmp/plsh-test && chmod a+rwx /tmp/plsh-test

DO E'#!/bin/sh\necho inline > /tmp/plsh-test/inline.out; chmod a+r /tmp/plsh-test/inline.out' LANGUAGE plsh;

\! cat /tmp/plsh-test/inline.out
\! rm -r /tmp/plsh-test
