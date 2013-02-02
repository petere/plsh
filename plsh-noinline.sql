CREATE FUNCTION plsh_handler() RETURNS language_handler
    AS '$libdir/plsh'
    LANGUAGE C;

CREATE FUNCTION plsh_validator(oid) RETURNS void
    AS '$libdir/plsh'
    LANGUAGE C;

CREATE LANGUAGE plsh
    HANDLER plsh_handler
    VALIDATOR plsh_validator;

COMMENT ON LANGUAGE plsh IS 'PL/sh procedural language';
