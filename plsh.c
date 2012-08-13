/*
 * PL/sh language handler
 *
 * Copyright © 2012 by Peter Eisentraut
 * See the COPYING file for details.
 *
 */

#include <postgres.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <access/heapam.h>
#include <catalog/catversion.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <commands/trigger.h>
#include <libpq/pqsignal.h>
#include <utils/syscache.h>
#include <utils/builtins.h>
#include <utils/rel.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>


#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif


#define _textout(x) (DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(&x))))



/*
 * Convert the C string "input" to a Datum of type "typeoid".
 */
static Datum
cstring_to_type(char * input, Oid typeoid)
{
	HeapTuple typetuple;
	Form_pg_type pg_type_entry;
	Datum ret;

	typetuple = SearchSysCache(TYPEOID, ObjectIdGetDatum(typeoid), 0, 0, 0);
	if (!HeapTupleIsValid(typetuple))
		elog(ERROR, "cache lookup failed for type %u", typeoid);

	pg_type_entry = (Form_pg_type) GETSTRUCT(typetuple);

	ret = OidFunctionCall3(pg_type_entry->typinput,
						   CStringGetDatum(input),
						   0, -1);

	ReleaseSysCache(typetuple);

	PG_RETURN_DATUM(ret);
}



/*
 * Convert the Datum "input" that is of type "typeoid" to a C string.
 */
static char *
type_to_cstring(Datum input, Oid typeoid)
{
	HeapTuple typetuple;
	Form_pg_type pg_type_entry;
	Datum ret;

	typetuple = SearchSysCache(TYPEOID, ObjectIdGetDatum(typeoid), 0, 0, 0);
	if (!HeapTupleIsValid(typetuple))
		elog(ERROR, "cache lookup failed for type %u", typeoid);

	pg_type_entry = (Form_pg_type) GETSTRUCT(typetuple);

	ret = OidFunctionCall3(pg_type_entry->typoutput,
						   input,
						   0, -1);

	ReleaseSysCache(typetuple);

	return DatumGetCString(ret);
}



/*
 * SIGCHLD handler
 */
static volatile pid_t child_pid;
static int child_status;


#if 0
static void
sigchld_handler(int signum)
{
	pid_t pid;
	int status, serrno;
	serrno = errno;

	while (1)
	{
		pid = waitpid (WAIT_ANY, &status, WNOHANG);
		if (pid < 0)
		{
			perror ("waitpid");
			break;
		}
		if (pid == 0)
			break;
		if (pid == child_pid)
			child_status = status;
	}
	errno = serrno;
}
#endif



/*
 * Read from "file" until EOF or error.  Return the content in
 * palloc'ed memory.  On error return NULL and set errno.
 */
static char *
read_from_file(FILE * file)
{
	char * buffer = NULL;
	ssize_t len = 0;

	do {
		char buf[512];
		ssize_t l;

		l = fread(buf, 1, 512, file);
		if (buffer)
			buffer = repalloc(buffer, len + l + 1);
		else
			buffer = palloc(l + 1);
		strncpy(buffer + len, buf, l);
		buffer[len + l] = '\0';
		len += l;

		if (feof(file))
		{
			break;
		}
		if (ferror(file))
		{
			return NULL;
			break;
		}
	} while(1);

	return buffer;
}



#define SPLIT_MAX 64

/*
 * Split the "string" at space boundaries.  The number of resulting
 * strings is in argcp, the actual strings in argv.  argcp should be
 * allocated to expect SPLIT_MAX strings.  "string" will be clobbered.
 */
static void
split_string(char *argv[], int *argcp, char *string)
{
	char * s = string;

	while (s && *s && *argcp < SPLIT_MAX)
	{
		while (*s == ' ')
			++s;
		if (*s == '\0')
			break;
		argv[(*argcp)++] = s;
		while (*s && *s != ' ')
			++s;
		if (*s)
			*s++ = '\0';
	}
}



/*
 * Make a safe temporary file.  Fill in for portability later...
 */
static int
my_mktemp(char *name)
{
	return mkstemp(name);
}



/*
 * Internal handler function
 */
Datum
handler_internal(Oid function_oid, FunctionCallInfo fcinfo, bool execute)
{
	HeapTuple proctuple;
	Form_pg_proc pg_proc_entry;
	char * sourcecode;
	char * rest;
	size_t len;
	char tempfile[24];
	int fd;
	FILE * file;
	int stdout_pipe[2];
	int stderr_pipe[2];
	int i;
	int ac;
	char * arguments[FUNC_MAX_ARGS + 2];
	char * stdout_buffer;
	char * stderr_buffer;
	bool return_null;
	char * s;
	HeapTuple returntuple = NULL;
	Datum prosrcdatum;
	bool isnull;

	proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(function_oid), 0, 0, 0);
	if (!HeapTupleIsValid(proctuple))
		elog(ERROR, "cache lookup failed for function %u", function_oid);

	prosrcdatum = SysCacheGetAttr(PROCOID, proctuple, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");

	sourcecode = pstrdup(DatumGetCString(DirectFunctionCall1(textout,
															 prosrcdatum)));

	pg_proc_entry = (Form_pg_proc) GETSTRUCT(proctuple);


	/* find shell and arguments */

	/*
	 * Accept one blank line at the start, to allow coding like this:
	 *   CREATE FUNCTION .... AS '
	 *   #!/bin/sh
	 *   ...
	 *   ' LANGUAGE plsh;
	 */
	while (sourcecode[0] == '\n' || sourcecode[0] == '\r')
		sourcecode++;

	elog(DEBUG2, "source code of function %u:\n%s", function_oid,
		 sourcecode);

	if (strlen(sourcecode) < 3
		|| (strncmp(sourcecode, "#!/", 3) != 0
			&& strncmp(sourcecode, "#! /", 4) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid start of script: %-.10s...", sourcecode),
				 errdetail("Script code must start with \"#!/\" or \"#! /\".")));

	rest = sourcecode + strcspn(sourcecode, "/");
	len = strcspn(rest, "\n\r");
	s = palloc(len + 1);
	strncpy(s, rest, len);
	s[len] = '\0';
	rest += len + 1;

	ac = 0;
	split_string(arguments, &ac, s);

	elog(DEBUG2, "using shell \"%s\"", arguments[0]);


	/* validation stops here */
	if (!execute)
	{
		ReleaseSysCache(proctuple);
		PG_RETURN_VOID();
	}

	/* copy source to temp file */

	strcpy(tempfile, "/tmp/.pgplsh-XXXXXX");
	fd = my_mktemp(tempfile);
	if (fd == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create temporary file: %m")));

	file = fdopen(fd, "w");
	if (!file)
	{
		close(fd);
		remove(tempfile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file stream to temporary file: %m")));
	}

	fprintf(file, "%s", rest);
	if (ferror(file))
	{
		fclose(file);
		remove(tempfile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write script to temporary file: %m")));
	}

	fclose(file);

	elog(DEBUG2, "source code is now in file \"%s\"", tempfile);


	/* evaluate arguments */

	arguments[ac++] = tempfile;

	if (CALLED_AS_TRIGGER(fcinfo))
	{
		TriggerData *trigdata = (TriggerData *) fcinfo->context;
		Trigger *trigger = trigdata->tg_trigger;
		TupleDesc tupdesc = trigdata->tg_relation->rd_att;
		HeapTuple oldtuple = trigdata->tg_trigtuple;

		/* first the CREATE TRIGGER fixed arguments */
		for (i = 0; i < trigger->tgnargs; i++)
		{
			arguments[ac++] = trigger->tgargs[i];
		}

		for (i = 0; i < tupdesc->natts; i++)
		{
			char * s;
			bool isnull;
			Datum attr;

			attr = heap_getattr(oldtuple, i + 1, tupdesc, &isnull);
			if (isnull)
				s = "";
			else
				s = type_to_cstring(attr, tupdesc->attrs[i]->atttypid);

			elog(DEBUG2, "arg %d is \"%s\" (type %u)", i, s,
				 tupdesc->attrs[i]->atttypid);

			arguments[ac++] = s;
		}

		/* since we can't alter the tuple anyway, set up a return
           tuple right now */
		if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
			returntuple = trigdata->tg_trigtuple;
		else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
			returntuple = trigdata->tg_trigtuple;
		else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			returntuple = trigdata->tg_newtuple;
		else
			elog(ERROR, "unrecognized trigger action: not INSERT, DELETE, or UPDATE");
	}
	else /* not trigger */
	{
		for (i = 0; i < pg_proc_entry->pronargs; i++)
		{
			char * s;

			if (PG_ARGISNULL(i))
				s = "";
			else
#if CATALOG_VERSION_NO >= 200503281 /* 8.1devel or later */
				s = type_to_cstring(PG_GETARG_DATUM(i),
									pg_proc_entry->proargtypes.values[i]);
#else
				s = type_to_cstring(PG_GETARG_DATUM(i),
									pg_proc_entry->proargtypes[i]);
#endif

			elog(DEBUG2, "arg %d is \"%s\"", i, s);

			arguments[ac++] = s;
		}
	}

	/* terminate list */
	arguments[ac] = NULL;


	/* start process voodoo */

	if (pipe(stdout_pipe) == -1)
	{
		remove(tempfile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not make pipe: %m")));
	}
	if (pipe(stderr_pipe) == -1)
	{
		remove(tempfile);
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not make pipe: %m")));
	}

#if 0
	pqsignal(SIGCHLD, SIG_DFL);
#endif

	child_pid = fork();

	if (child_pid == -1)		/* fork failed */
	{
		remove(tempfile);
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		close(stderr_pipe[0]);
		close(stderr_pipe[1]);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("fork failed: %m")));
	}
	else if (child_pid == 0)	/* child */
	{
		/* close reading end */
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);

		dup2(stdout_pipe[1], 1);
		dup2(stderr_pipe[1], 2);
		close(stdout_pipe[1]);
		close(stderr_pipe[1]);
		execv(arguments[0], arguments);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not exec: %m")));
	}

	/* parent continues... */
	close(stdout_pipe[1]);	/* writing end */
	close(stderr_pipe[1]);


	/* fetch return value from stdout */

	return_null = false;

	file = fdopen(stdout_pipe[0], "r");
	if (!file)
	{
		remove(tempfile);
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file stream to stdout pipe: %m")));
	}

	stdout_buffer = read_from_file(file);
	fclose(file);
	if (!stdout_buffer)
	{
		remove(tempfile);
		close(stderr_pipe[0]);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read script's stdout: %m")));
	}

	len = strlen(stdout_buffer);
	if (len == 0)
		return_null = true;
	/* strip one trailing newline */
	else if (stdout_buffer[len - 1] == '\n')
		stdout_buffer[len - 1] = '\0';
	elog(DEBUG2, "stdout was \"%s\"", stdout_buffer);


	/* print stderr as error */

	file = fdopen(stderr_pipe[0], "r");
	if (!file)
	{
		remove(tempfile);
		close(stderr_pipe[0]);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file stream to stderr pipe: %m")));
	}

	stderr_buffer = read_from_file(file);
	fclose(file);
	if (!stderr_buffer)
	{
		remove(tempfile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read script's stderr: %m")));
	}

	len = strlen(stderr_buffer);
	if (stderr_buffer[len - 1] == '\n')
		stderr_buffer[len - 1] = '\0';

	if (stderr_buffer[0] != '\0')
	{
		remove(tempfile);
		ereport(ERROR,
				(errmsg("%s: %s", NameStr(pg_proc_entry->proname), stderr_buffer)));
	}


	/* block and wait for the script to finish */
	{
		pid_t dead;
		do
			dead = wait(&child_status);
		while (dead > 0 && dead != child_pid);

		remove(tempfile);

		if (dead != child_pid)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("wait failed: %m")));
	}

#if 0
	pqsignal(SIGCHLD, SIG_IGN);
#endif

	if (WIFEXITED(child_status))
	{
		if (WEXITSTATUS(child_status) != 0)
			ereport(ERROR,
					(errmsg("script exited with status %d",
						   WEXITSTATUS(child_status))));
	}
	if (WIFSIGNALED(child_status))
	{
		ereport(ERROR,
				(errmsg("script was terminated by signal %d",
						(int)WTERMSIG(child_status))));
	}

	ReleaseSysCache(proctuple);

	if (CALLED_AS_TRIGGER(fcinfo))
	{
		PG_RETURN_DATUM(PointerGetDatum(returntuple));
	}
	else
	{
		if (return_null)
			PG_RETURN_NULL();
		else
			PG_RETURN_DATUM(cstring_to_type(stdout_buffer,
											pg_proc_entry->prorettype));
	}
}



/*
 * The PL handler
 */
PG_FUNCTION_INFO_V1(plsh_handler);

Datum
plsh_handler(PG_FUNCTION_ARGS)
{
	return handler_internal(fcinfo->flinfo->fn_oid, fcinfo, true);
}



/*
 * Validator function
 */
PG_FUNCTION_INFO_V1(plsh_validator);

Datum
plsh_validator(PG_FUNCTION_ARGS)
{
	return handler_internal(PG_GETARG_OID(0), fcinfo, false);
}
