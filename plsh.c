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
#include <commands/dbcommands.h>
#include <commands/trigger.h>
#include <libpq/pqsignal.h>
#include <postmaster/postmaster.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/builtins.h>
#include <utils/rel.h>
#if PG_VERSION_NUM >= 100000
#include <utils/varlena.h>
#endif
#if PG_VERSION_NUM >= 90300
#include <access/htup_details.h>
#include <commands/event_trigger.h>
#define HAVE_EVENT_TRIGGERS 1
#endif

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>


PG_MODULE_MAGIC;


#define _textout(x) (DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(&x))))


#ifndef HAVE_EVENT_TRIGGERS
typedef void EventTriggerData;
#define CALLED_AS_EVENT_TRIGGER(x) 0
#endif


#if PG_VERSION_NUM < 110000 && !defined(TupleDescAttr)
#define TupleDescAttr(tupdesc, i) ((tupdesc)->attrs[(i)])
#endif


static char * handler_internal2(const char *tempfile, char * const * arguments, const char *proname, TriggerData *trigger_data, EventTriggerData *event_trigger_data);



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
 * Find shell and arguments in source code
 */
void
parse_shell_and_arguments(const char *sourcecode, int *argcp, char **arguments, const char **restp)
{
	const char *rest;
	size_t len;
	char * s;

	/*
	 * Accept one blank line at the start, to allow coding like this:
	 *   CREATE FUNCTION .... AS '
	 *   #!/bin/sh
	 *   ...
	 *   ' LANGUAGE plsh;
	 */
	while (sourcecode[0] == '\n' || sourcecode[0] == '\r')
		sourcecode++;

	elog(DEBUG2, "source code of function:\n%s", sourcecode);

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
	rest += len;
	if (*rest)
		rest++;

	*argcp = 0;
	split_string(arguments, argcp, s);
	*restp = rest;

	elog(DEBUG2, "using shell \"%s\"", arguments[0]);
}



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



static char *
write_to_tempfile(const char *data)
{
	char *tmpdir_envvar;
	static char tempfile[MAXPGPATH];
	int fd;
	FILE * file;

	if ((tmpdir_envvar = getenv("TMPDIR")))
		snprintf(tempfile, sizeof(tempfile), "%s/plsh.XXXXXX", tmpdir_envvar);
	else
		strcpy(tempfile, "/tmp/plsh-XXXXXX");

	fd = mkstemp(tempfile);
	if (fd == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create temporary file \"%s\": %m", tempfile)));

	file = fdopen(fd, "w");
	if (!file)
	{
		close(fd);
		remove(tempfile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file stream to temporary file: %m")));
	}

	fprintf(file, "%s", data);
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

	return tempfile;
}


/*
 * Set environment variables corresponding to trigger data
 */
static void
set_trigger_data_envvars(TriggerData *trigdata)
{
	const char *tg_when_str = NULL;
	const char *tg_level_str = NULL;
	const char *tg_op_str = NULL;

	setenv("PLSH_TG_NAME", trigdata->tg_trigger->tgname, 1);

	if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		tg_when_str = "BEFORE";
#ifdef TRIGGER_FIRED_INSTEAD
	else if (TRIGGER_FIRED_INSTEAD(trigdata->tg_event))
		tg_when_str = "INSTEAD OF";
#endif
	else if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		tg_when_str = "AFTER";
	if (tg_when_str)
		setenv("PLSH_TG_WHEN", tg_when_str, 1);

	if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		tg_level_str = "ROW";
	else if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		tg_level_str = "STATEMENT";
	if (tg_level_str)
		setenv("PLSH_TG_LEVEL", tg_level_str, 1);

	if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		tg_op_str = "DELETE";
	else if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		tg_op_str = "INSERT";
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		tg_op_str = "UPDATE";
#ifdef TRIGGER_FIRED_BY_TRUNCATE
	else if (TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
		tg_op_str = "TRUNCATE";
#endif
	if (tg_op_str)
		setenv("PLSH_TG_OP", tg_op_str, 1);

	setenv("PLSH_TG_TABLE_NAME", NameStr(trigdata->tg_relation->rd_rel->relname), 1);
	setenv("PLSH_TG_TABLE_SCHEMA", get_namespace_name(trigdata->tg_relation->rd_rel->relnamespace), 1);
}


/*
 * Set environment variables corresponding to event trigger data
 */
static void
set_event_trigger_data_envvars(EventTriggerData *evttrigdata)
{
#ifdef HAVE_EVENT_TRIGGERS
	setenv("PLSH_TG_EVENT", evttrigdata->event, 1);
	setenv("PLSH_TG_TAG",
#if PG_VERSION_NUM >= 130000
	       GetCommandTagName(evttrigdata->tag),
#else
	       evttrigdata->tag,
#endif
	       1);
#endif
}


/*
 * Set environment variables for libpq access
 */
void
set_libpq_envvars(void)
{
	setenv("PGAPPNAME", "plsh", 1);
	unsetenv("PGCLIENTENCODING");
	setenv("PGDATABASE", get_database_name(MyDatabaseId), 1);
#if PG_VERSION_NUM >= 90300
	if (Unix_socket_directories)
	{
		char       *rawstring;
		List       *elemlist;

		rawstring = pstrdup(Unix_socket_directories);

		if (!SplitDirectoriesString(rawstring, ',', &elemlist))
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax for \"unix_socket_directories\"")));

		if (list_length(elemlist))
			setenv("PGHOST", linitial(elemlist), 1);
		else
			setenv("PGHOST", "localhost", 0);
	}
#else
	if (UnixSocketDir && *UnixSocketDir)
		setenv("PGHOST", UnixSocketDir, 1);
#endif
	else
		setenv("PGHOST", "localhost", 0);

	{
		char buf[16];
		sprintf(buf, "%u", PostPortNumber);
		setenv("PGPORT", buf, 1);
	}

	if (getenv("PATH"))
	{
		char buf[MAXPGPATH];
		char *p;

		strlcpy(buf, my_exec_path, sizeof(buf));
		p = strrchr(buf, '/');
		snprintf(p, sizeof(buf) - (p - buf), ":%s", getenv("PATH"));
		setenv("PATH", buf, 1);
	}
}


/*
 * Block and wait for the script to finish
 */
static int
wait_and_cleanup(pid_t child_pid, const char *tempfile)
{
	pid_t dead;
	int child_status;

	do
		dead = wait(&child_status);
	while (dead > 0 && dead != child_pid);

	remove(tempfile);

	if (dead != child_pid)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("wait failed: %m")));

	return child_status;
}


/*
 * Internal handler function
 */
Datum
handler_internal(Oid function_oid, FunctionCallInfo fcinfo, bool execute)
{
	HeapTuple proctuple;
	Form_pg_proc pg_proc_entry;
	const char * sourcecode;
	const char * rest;
	char *tempfile;
	int i;
	int argc;
	char * arguments[FUNC_MAX_ARGS + 2];
	char * ret;
	HeapTuple returntuple = NULL;
	Datum prosrcdatum;
	bool isnull;

	proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(function_oid), 0, 0, 0);
	if (!HeapTupleIsValid(proctuple))
		elog(ERROR, "cache lookup failed for function %u", function_oid);

	prosrcdatum = SysCacheGetAttr(PROCOID, proctuple, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");

	sourcecode = DatumGetCString(DirectFunctionCall1(textout, prosrcdatum));

	parse_shell_and_arguments(sourcecode, &argc, arguments, &rest);

	/* validation stops here */
	if (!execute)
	{
		ReleaseSysCache(proctuple);
		PG_RETURN_VOID();
	}

	tempfile = write_to_tempfile(rest);
	arguments[argc++] = tempfile;

	/* evaluate arguments */

	pg_proc_entry = (Form_pg_proc) GETSTRUCT(proctuple);

	if (CALLED_AS_TRIGGER(fcinfo))
	{
		TriggerData *trigdata = (TriggerData *) fcinfo->context;
		Trigger *trigger = trigdata->tg_trigger;
		TupleDesc tupdesc = trigdata->tg_relation->rd_att;
		HeapTuple oldtuple = trigdata->tg_trigtuple;

		/* first the CREATE TRIGGER fixed arguments */
		for (i = 0; i < trigger->tgnargs; i++)
		{
			arguments[argc++] = trigger->tgargs[i];
		}

		if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
			for (i = 0; i < tupdesc->natts; i++)
			{
				char * s;
				bool attr_isnull;
				Datum attr;

				attr = heap_getattr(oldtuple, i + 1, tupdesc, &attr_isnull);
				if (attr_isnull)
					s = "";
				else
					s = type_to_cstring(attr, TupleDescAttr(tupdesc, i)->atttypid);

				elog(DEBUG2, "arg %d is \"%s\" (type %u)", i, s,
					 TupleDescAttr(tupdesc, i)->atttypid);

				arguments[argc++] = s;
			}

		/* since we can't alter the tuple anyway, set up a return
		   tuple right now */
		if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
			returntuple = trigdata->tg_trigtuple;
		else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
			returntuple = trigdata->tg_trigtuple;
		else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			returntuple = trigdata->tg_newtuple;
#ifdef TRIGGER_FIRED_BY_TRUNCATE
		else if (TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
			returntuple = trigdata->tg_trigtuple;
#endif
		else
			elog(ERROR, "unrecognized trigger action: not INSERT, DELETE, UPDATE, or TRUNCATE");
	}
	else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
	{
		/* nothing */
	}
	else /* not trigger */
	{
		for (i = 0; i < pg_proc_entry->pronargs; i++)
		{
			char * s;

			if (PG_ARGISNULL(i))
				s = "";
			else
				s = type_to_cstring(PG_GETARG_DATUM(i),
									pg_proc_entry->proargtypes.values[i]);

			elog(DEBUG2, "arg %d is \"%s\"", i, s);

			arguments[argc++] = s;
		}
	}

	/* terminate list */
	arguments[argc] = NULL;

	ret = handler_internal2(tempfile,
							arguments,
							NameStr(pg_proc_entry->proname),
							CALLED_AS_TRIGGER(fcinfo) ? (TriggerData *) fcinfo->context : NULL,
							CALLED_AS_EVENT_TRIGGER(fcinfo) ? (EventTriggerData *) fcinfo->context : NULL);


	ReleaseSysCache(proctuple);

	if (CALLED_AS_TRIGGER(fcinfo))
	{
		PG_RETURN_DATUM(PointerGetDatum(returntuple));
	}
	else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
	{
		PG_RETURN_NULL();
	}
	else
	{
		if (ret)
			PG_RETURN_DATUM(cstring_to_type(ret, pg_proc_entry->prorettype));
		else
			PG_RETURN_NULL();
	}
}



static char *
handler_internal2(const char *tempfile, char * const * arguments, const char *proname,
				  TriggerData *trigger_data, EventTriggerData *event_trigger_data)
{
	int stdout_pipe[2];
	int stderr_pipe[2];
	pid_t child_pid;
	int child_status;
	FILE * file;
	char * stdout_buffer;
	char * stderr_buffer;
	size_t len;
	bool return_null;

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

		if (trigger_data)
			set_trigger_data_envvars(trigger_data);
		if (event_trigger_data)
			set_event_trigger_data_envvars(event_trigger_data);

		set_libpq_envvars();

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
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		wait_and_cleanup(child_pid, tempfile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file stream to stdout pipe: %m")));
	}

	stdout_buffer = read_from_file(file);
	fclose(file);
	if (!stdout_buffer)
	{
		close(stderr_pipe[0]);
		wait_and_cleanup(child_pid, tempfile);
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
		close(stderr_pipe[0]);
		wait_and_cleanup(child_pid, tempfile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file stream to stderr pipe: %m")));
	}

	stderr_buffer = read_from_file(file);
	fclose(file);
	if (!stderr_buffer)
	{
		wait_and_cleanup(child_pid, tempfile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read script's stderr: %m")));
	}

	len = strlen(stderr_buffer);
	if (stderr_buffer[len - 1] == '\n')
		stderr_buffer[len - 1] = '\0';

	if (stderr_buffer[0] != '\0')
	{
		wait_and_cleanup(child_pid, tempfile);
		ereport(ERROR,
				(errmsg("%s: %s", proname, stderr_buffer)));
	}

	child_status = wait_and_cleanup(child_pid, tempfile);

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

	if (return_null)
		return NULL;
	else
		return stdout_buffer;
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
	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, PG_GETARG_OID(0)))
		PG_RETURN_VOID();
	return handler_internal(PG_GETARG_OID(0), fcinfo, false);
}



#if CATALOG_VERSION_NO >= 200909221
/*
 * Inline handler
 */
PG_FUNCTION_INFO_V1(plsh_inline_handler);

Datum
plsh_inline_handler(PG_FUNCTION_ARGS)
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));
	int argc;
	char * arguments[FUNC_MAX_ARGS + 2];
	const char *rest;
	char *tempfile;

	parse_shell_and_arguments(codeblock->source_text, &argc, arguments, &rest);
	tempfile = write_to_tempfile(rest);
	arguments[argc++] = tempfile;
	arguments[argc] = NULL;
	handler_internal2(tempfile, arguments, "inline code block", NULL, NULL);
	PG_RETURN_VOID();
}
#endif
