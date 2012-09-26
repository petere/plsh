-- CR/LF test
CREATE FUNCTION crlf_test() RETURNS text LANGUAGE plsh
AS E'\r\n#!/bin/sh\r\necho OK\r\n';

SELECT crlf_test();
