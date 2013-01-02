CREATE FUNCTION pg_catalog.plsh_inline_handler(internal) RETURNS void
    AS '$libdir/plsh'
    LANGUAGE C;

CREATE OR REPLACE LANGUAGE plsh
    HANDLER pg_catalog.plsh_handler
    INLINE pg_catalog.plsh_inline_handler
    VALIDATOR pg_catalog.plsh_validator;
