#include "postgres.h"
#include "funcapi.h"
#include "assert.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "catalog/namespace.h"
#include "ctype.h"
#include "string.h"
#include "orafunc.h"
#include "builtins.h"

PG_FUNCTION_INFO_V1(dbms_assert_enquote_literal);
PG_FUNCTION_INFO_V1(dbms_assert_enquote_name);
PG_FUNCTION_INFO_V1(dbms_assert_noop);
PG_FUNCTION_INFO_V1(dbms_assert_qualified_sql_name);
PG_FUNCTION_INFO_V1(dbms_assert_schema_name);
PG_FUNCTION_INFO_V1(dbms_assert_simple_sql_name);
PG_FUNCTION_INFO_V1(dbms_assert_object_name);


#define CUSTOM_EXCEPTION(code, msg) \
	ereport(ERROR, \
		(errcode(ERRCODE_ORA_PACKAGES_##code), \
		 errmsg(msg)))

#define INVALID_SCHEMA_NAME_EXCEPTION()	\
	CUSTOM_EXCEPTION(INVALID_SCHEMA_NAME, "invalid schema name")

#define INVALID_OBJECT_NAME_EXCEPTION() \
	CUSTOM_EXCEPTION(INVALID_OBJECT_NAME, "invalid object name")

#define ISNOT_SIMPLE_SQL_NAME_EXCEPTION() \
	CUSTOM_EXCEPTION(ISNOT_SIMPLE_SQL_NAME, "string is not simple SQL name")

#define ISNOT_QUALIFIED_SQL_NAME_EXCEPTION() \
	CUSTOM_EXCEPTION(ISNOT_QUALIFIED_SQL_NAME, "string is not qualified SQL name")

#define EMPTY_STR(str)		((VARSIZE(str) - VARHDRSZ) == 0)


static bool check_sql_name(char *cp, int len);
static bool ParseIdentifierString(char *rawstring);

/*
 * Procedure ParseIdentifierString is based on SplitIdentifierString
 * from varlena.c. We need different behave of quote symbol evaluation.
 */
bool
ParseIdentifierString(char *rawstring)
{
	char	   *nextp = rawstring;
	bool		done = false;

	while (isspace((unsigned char) *nextp))
		nextp++;				/* skip leading whitespace */

	if (*nextp == '\0')
		return true;			/* allow empty string */

	/* At the top of the loop, we are at start of a new identifier. */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '\"')
		{
			/* Quoted name --- collapse quote-quote pairs, no downcasing */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '\"');
				if (endp == NULL)
					return false;		/* mismatched quotes */
				if (endp[1] != '\"')
					break;		/* found end of quoted name */
				/* Collapse adjacent quotes into one quote, and look again */
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			/* endp now points at the terminating quote */
			nextp = endp + 1;
		}
		else
		{
			/* Unquoted name --- extends to separator or whitespace */
			curname = nextp;
			while (*nextp && *nextp != '.' &&
				   !isspace((unsigned char) *nextp))
			{
				if (!isalnum(*nextp) && *nextp != '_')
					return false;
				nextp++;
			}
			endp = nextp;
			if (curname == nextp)
				return false;	/* empty unquoted name not allowed */
		}

		while (isspace((unsigned char) *nextp))
			nextp++;			/* skip trailing whitespace */

		if (*nextp == '.')
		{
			nextp++;
			while (isspace((unsigned char) *nextp))
				nextp++;		/* skip leading whitespace for next */
			/* we expect another name, so done remains false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* invalid syntax */

		/* Loop back if we didn't reach end of string */
	} while (!done);

	return true;
}



/****************************************************************
 * DBMS_ASSERT.ENQUOTE_LITERAL
 *
 * Syntax:
 *   FUNCTION ENQUOTE_LITERAL(str varchar) RETURNS varchar;
 *
 * Purpouse:
 *   Add leading and trailing quotes, verify that all single quotes
 *   are paired with adjacent single quotes.
 *
 ****************************************************************/

Datum
dbms_assert_enquote_literal(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall1(quote_literal, PG_GETARG_DATUM(0)));
}


/****************************************************************
 * DBMS_ASSERT.ENQUOTE_NAME
 *
 * Syntax:
 *   FUNCTION ENQUOTE_NAME(str varchar) RETURNS varchar;
 *   FUNCTION ENQUOTE_NAME(str varchar, loweralize boolean := true)
 *      RETURNS varchar;
 * Purpouse:
 *   Enclose name in double quotes.
 * Atention!:
 *   On Oracle is second parameter capitalize!
 *
 ****************************************************************/

Datum
dbms_assert_enquote_name(PG_FUNCTION_ARGS)
{
	Datum name  = PG_GETARG_DATUM(0);
    	bool loweralize = PG_GETARG_BOOL(1);

	name = DirectFunctionCall1(quote_ident, name);

	if (loweralize)
		name = DirectFunctionCall1(lower, name);

	PG_RETURN_DATUM(name);
}


/****************************************************************
 * DBMS_ASSERT.NOOP
 *
 * Syntax:
 *   FUNCTION NOOP(str varchar) RETURNS varchar;
 *
 * Purpouse:
 *   Returns value without any checking.
 *
 ****************************************************************/

Datum
dbms_assert_noop(PG_FUNCTION_ARGS)
{
	text *str = PG_GETARG_TEXT_P(0);

	PG_RETURN_TEXT_P(TextPCopy(str));
}


/****************************************************************
 * DBMS_ASSERT.QUALIFIED_SQL_NAME
 *
 * Syntax:
 *   FUNCTION QUALIFIED_SQL_NAME(str varchar) RETURNS varchar;
 *
 * Purpouse:
 *   This function verifies that the input string is qualified SQL
 *   name.
 * Exception: 44004 string is not a qualified SQL name
 *
 ****************************************************************/

Datum
dbms_assert_qualified_sql_name(PG_FUNCTION_ARGS)
{
	text *qname;

	if (PG_ARGISNULL(0))
		ISNOT_QUALIFIED_SQL_NAME_EXCEPTION();

	qname = PG_GETARG_TEXT_P(0);
	if (EMPTY_STR(qname))
		ISNOT_QUALIFIED_SQL_NAME_EXCEPTION();

	if (!ParseIdentifierString(text_to_cstring(qname)))
		ISNOT_QUALIFIED_SQL_NAME_EXCEPTION();

	PG_RETURN_TEXT_P(qname);
}


/****************************************************************
 * DBMS_ASSERT.SCHEMA_NAME
 *
 * Syntax:
 *   FUNCTION SCHEMA_NAME(str varchar) RETURNS varchar;
 *
 * Purpouse:
 *   Function verifies that input string is an existing schema
 *   name.
 * Exception: 44001 Invalid schema name
 *
 ****************************************************************/

Datum
dbms_assert_schema_name(PG_FUNCTION_ARGS)
{
	Oid			namespaceId;
	AclResult	aclresult;
	text *sname;
	char *nspname;
	List	*names;

	if (PG_ARGISNULL(0))
		INVALID_SCHEMA_NAME_EXCEPTION();

	sname = PG_GETARG_TEXT_P(0);
	if (EMPTY_STR(sname))
		INVALID_SCHEMA_NAME_EXCEPTION();

	nspname = text_to_cstring(sname);
	names = stringToQualifiedNameList(nspname);
	if (list_length(names) != 1)
		INVALID_SCHEMA_NAME_EXCEPTION();

	namespaceId = GetSysCacheOid(NAMESPACENAME,
							CStringGetDatum(strVal(linitial(names))),
							0, 0, 0);
	if (!OidIsValid(namespaceId))
		INVALID_SCHEMA_NAME_EXCEPTION();

	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		INVALID_SCHEMA_NAME_EXCEPTION();

	PG_RETURN_TEXT_P(sname);
}


/****************************************************************
 * DBMS_ASSERT.SIMPLE_SQL_NAME
 *
 * Syntax:
 *   FUNCTION SIMPLE_SQL_NAME(str varchar) RETURNS varchar;
 *
 * Purpouse:
 *   This function verifies that the input string is simple SQL
 *   name.
 * Exception: 44003 String is not a simple SQL name
 *
 ****************************************************************/

static bool
check_sql_name(char *cp, int len)
{
	if (*cp == '"')
    	{
		for (cp++, len -= 2; len-- > 0; cp++)
		{
			/* all double quotes have to be paired */
			if (*cp == '"')
			{
				if (len-- == 0)
					return false;
				/* next char has to be quote */
				if (*cp != '"')
					return false;
			}

		}
		if (*cp != '"')
			return false;
	}
	else
	{
		/* Doesn't allow national characters in sql name :( */
		for (; len-- > 0; cp++)
			if (!isalnum(*cp) && *cp != '_')
				return false;
	}

	return true;
}

Datum
dbms_assert_simple_sql_name(PG_FUNCTION_ARGS)
{
	text  *sname;
	int		len;
	char *cp;

	if (PG_ARGISNULL(0))
		ISNOT_SIMPLE_SQL_NAME_EXCEPTION();

	sname = PG_GETARG_TEXT_P(0);
	if (EMPTY_STR(sname))
		ISNOT_SIMPLE_SQL_NAME_EXCEPTION();

	len = VARSIZE(sname) - VARHDRSZ;
	cp = VARDATA(sname);

	if (!check_sql_name(cp, len))
		ISNOT_SIMPLE_SQL_NAME_EXCEPTION();

	PG_RETURN_TEXT_P(sname);
}


/****************************************************************
 * DBMS_ASSERT.OBJECT_NAME
 *
 * Syntax:
 *   FUNCTION OBJECT_NAME(str varchar) RETURNS varchar;
 *
 * Purpouse:
 *   Verifies that input string is qualified SQL identifier of
 *   an existing SQL object.
 * Exception: 44002 Invalid object name
 *
 ****************************************************************/

Datum
dbms_assert_object_name(PG_FUNCTION_ARGS)
{
	List	*names;
	text	*str;
	char	*object_name;
	Oid 		classId;

	if (PG_ARGISNULL(0))
		INVALID_OBJECT_NAME_EXCEPTION();

	str = PG_GETARG_TEXT_P(0);
	if (EMPTY_STR(str))
		INVALID_OBJECT_NAME_EXCEPTION();

	object_name = text_to_cstring(str);

	names = stringToQualifiedNameList(object_name);

	classId = RangeVarGetRelid(makeRangeVarFromNameList(names), true);
	if (!OidIsValid(classId))
		INVALID_OBJECT_NAME_EXCEPTION();

	PG_RETURN_TEXT_P(str);
}
