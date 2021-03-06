/*
 * 2007 January 27
 *
 * The author disclaims copyright to this source code.  In place of
 * a legal notice, here is a blessing:
 *
 *    May you do good and not evil.
 *    May you find forgiveness for yourself and forgive others.
 *    May you share freely, never taking more than you give.
 *
 ********************************************************************
 *
 * SQLite extension module for importing/exporting
 * database information from/to SQL source text and
 * export to CSV text.
 *
 * Usage:
 *
 *  SQLite function:
 *       SELECT import_sql(filename);
 *
 *  C function (STANDALONE):
 *       int impexp_import_sql(sqlite3 *db, char *filename);
 *
 *       Reads SQL commands from filename and executes them
 *       against the current database. Returns the number
 *       of changes to the current database.
 *
 *
 *  SQLite function:
 *       SELECT export_sql(filename, [mode, tablename, ...]);
 *
 *  C function (STANDALONE):
 *       int impexp_export_sql(sqlite3 *db, char *filename, int mode, ...);
 *
 *       Writes SQL to filename similar to SQLite's shell
 *       ".dump" meta command. Mode selects the output format:
 *       Mode 0 (default): dump schema and data using the
 *       optional table names following the mode argument.
 *       Mode 1: dump data only using the optional table
 *       names following the mode argument.
 *       Mode 2: dump schema and data using the optional
 *       table names following the mode argument; each
 *       table name is followed by a WHERE clause, i.e.
 *       "mode, table1, where1, table2, where2, ..."
 *       Mode 3: dump data only, same rules as in mode 2.
 *       Returns approximate number of lines written or
 *       -1 when an error occurred.
 *
 *       Bit 1 of mode:      when 1 dump data only
 *       Bits 8..9 of mode:  blob quoting mode
 *           0   default
 *         256   ORACLE
 *         512   SQL Server
 *         768   MySQL
 *
 *
 *  SQLite function:
 *       SELECT export_csv(filename, hdr, prefix1, tablename1, schema1, ...]);
 *
 *  C function (STANDALONE):
 *       int impexp_export_csv(sqlite3 *db, int hdr, char *filename,
 *                             char *prefix1, char *tablename1,
 *                             char *schema1, ...);
 *
 *       Writes entire tables as CSV to provided filename. A header
 *       row is written when the hdr parameter is true. The
 *       rows are optionally introduced with a column made up of
 *       the prefix (non-empty string) for the respective table.
 *       If "schema" is NULL, "sqlite_master" is used, otherwise
 *       specify e.g. "sqlite_temp_master" for temporary tables or 
 *       "att.sqlite_master" for the attached database "att".
 *
 *          CREATE TABLE A(a,b);
 *          INSERT INTO A VALUES(1,2);
 *          INSERT INTO A VALUES(3,'foo');
 *          CREATE TABLE B(c);
 *          INSERT INTO B VALUES('hello');
 *          SELECT export_csv('out.csv', 0, 'aa', 'A', NULL, 'bb', 'B', NULL);
 *          -- CSV output
 *          "aa",1,2
 *          "aa",3,"foo"
 *          "bb","hello"
 *          SELECT export_csv('out.csv', 1, 'aa', 'A', NULL, 'bb', 'B', NULL);
 *          -- CSV output
 *          "aa","a","b"
 *          "aa",1,2
 *          "aa",3,"foo"
 *          "bb","c"
 *          "bb","hello"
 *
 *
 *  SQLite function:
 *       SELECT export_xml(filename, appendflag, indent,
 *                         [root, item, tablename, schema]+);
 *
 *  C function (STANDALONE):
 *       int impexp_export_xml(sqlite3 *db, char *filename,
 *                             int append, int indent, char *root,
 *                             char *item, char *tablename, char *schema);
 *
 *       Writes a table as simple XML to provided filename. The
 *       rows are optionally enclosed with the "root" tag,
 *       the row data is enclosed in "item" tags. If "schema"
 *       is NULL, "sqlite_master" is used, otherwise specify
 *       e.g. "sqlite_temp_master" for temporary tables or 
 *       "att.sqlite_master" for the attached database "att".
 *          
 *          <item>
 *           <columnname TYPE="INTEGER|REAL|NULL|TEXT|BLOB">value</columnname>
 *           ...
 *          </item>
 *
 *       e.g.
 *
 *          CREATE TABLE A(a,b);
 *          INSERT INTO A VALUES(1,2.1);
 *          INSERT INTO A VALUES(3,'foo');
 *          INSERT INTO A VALUES('',NULL);
 *          INSERT INTO A VALUES(X'010203','<blob>');
 *          SELECT export_xml('out.xml', 0, 2, 'TBL_A', 'ROW', 'A');
 *          -- XML output
 *            <TBL_A>
 *             <ROW>
 *              <a TYPE="INTEGER">1</a>
 *              <b TYPE="REAL">2.1</b>
 *             </ROW>
 *             <ROW>
 *              <a TYPE="INTEGER">3</a>
 *              <b TYPE="TEXT">foo</b>
 *             </ROW>
 *             <ROW>
 *              <a TYPE="TEXT"></a>
 *              <b TYPE="NULL"></b>
 *             </ROW>
 *             <ROW>
 *              <a TYPE="BLOB">&#x01;&#x02;&x03;</a>
 *              <b TYPE="TEXT">&lt;blob&gt;</b>
 *             </ROW>
 *            </TBL_A>
 *
 *       Quoting of XML entities is performed only on the data,
 *       not on column names and root/item tags.
 *
 *
 * On Win32 the filename argument may be specified as NULL in order
 * to open a system file dialog for interactive filename selection.
 */

#ifdef STANDALONE
#include <sqlite3.h>
#else
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif

static const char space_chars[] = " \f\n\r\t\v";

#define ISSPACE(c) ((c) && strchr(space_chars, (c)) != 0)

static char *
one_input_line(FILE *fin)
{
    char *line, *tmp;
    int nline;
    int n;
    int eol;

    nline = 256;
    line = sqlite3_malloc(nline);
    if (!line) {
	return 0;
    }
    n = 0;
    eol = 0;
    while (!eol) {
	if (n + 256 > nline) {
	    nline = nline * 2 + 256;
	    tmp = sqlite3_realloc(line, nline);
	    if (!tmp) {
		sqlite3_free(line);
		return 0;
	    }
	    line = tmp;
	}
	if (!fgets(line + n, nline - n, fin)) {
	    if (n == 0) {
		sqlite3_free(line);
		return 0;
	    }
	    line[n] = 0;
	    eol = 1;
	    break;
	}
	while (line[n]) {
	    n++;
	}
	if (n > 0 && line[n-1] == '\n') {
	    n--;
	    line[n] = 0;
	    eol = 1;
	}
    }
    tmp = sqlite3_realloc(line, n + 1);
    if (!tmp) {
	sqlite3_free(line);
	return 0;
    }
    return tmp;
}

static int
ends_with_semicolon(const char *str, int n)
{
    while (n > 0 && ISSPACE(str[n - 1])) {
	n--;
    }
    return n > 0 && str[n - 1] == ';';
}

static int
all_whitespace(const char *str)
{
    for (; str[0]; str++) {
	if (ISSPACE(str[0])) {
	    continue;
	}
	if (str[0] == '/' && str[1] == '*') {
	    str += 2;
	    while (str[0] && (str[0] != '*' || str[1] != '/')) {
		str++;
	    }
	    if (!str[0]) {
		return 0;
	    }
	    str++;
	    continue;
	}
	if (str[0] == '-' && str[1] == '-') {
	    str += 2;
	    while (str[0] && str[0] != '\n') {
		str++;
	    }
	    if (!str[0]) {
		return 1;
	    }
	    continue;
	}
	return 0;
    }
    return 1;
}

static int
process_input(sqlite3 *db, FILE *fin)
{
    char *line = 0;
    char *sql = 0;
    int nsql = 0;
    int rc;
    int errors = 0;

    while (1) {
	line = one_input_line(fin);
	if (!line) {
	    break;
	}
	if ((!sql || !sql[0]) && all_whitespace(line)) {
	    continue;
	}
	if (!sql) {
	    int i;
	    for (i = 0; line[i] && ISSPACE(line[i]); i++) {
		/* empty loop body */
	    }
	    if (line[i]) {
		nsql = strlen(line);
		sql = sqlite3_malloc(nsql + 1);
		if (!sql) {
		    errors++;
		    break;
		}
		strcpy(sql, line);
	    }
	} else {
	    int len = strlen(line);
	    char *tmp;

	    tmp = sqlite3_realloc(sql, nsql + len + 2);
	    if (!tmp) {
		errors++;
		break;
	    }
	    sql = tmp;
	    strcpy(sql + nsql, "\n");
	    nsql++;
	    strcpy(sql + nsql, line);
	    nsql += len;
	}
	sqlite3_free(line);
	line = 0;
	if (sql && ends_with_semicolon(sql, nsql) && sqlite3_complete(sql)) {
	    rc = sqlite3_exec(db, sql, 0, 0, 0);
	    if (rc != SQLITE_OK) {
		errors++;
	    }
	    sqlite3_free(sql);
	    sql = 0;
	    nsql = 0;
	}
    }
    if (sql) {
	sqlite3_free(sql);
    }
    if (line) {
	sqlite3_free(line);
    }
    return errors;
}

static void
quote_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    int mode = 0;

    if (argc < 1) {
	return;
    }
    if (argc > 1) {
	mode = sqlite3_value_int(argv[1]);
    }
    switch (sqlite3_value_type(argv[0])) {
    case SQLITE_NULL: {
	sqlite3_result_text(context, "NULL", 4, SQLITE_STATIC);
	break;
    }
    case SQLITE_INTEGER:
    case SQLITE_FLOAT: {
	sqlite3_result_value(context, argv[0]);
	break;
    }
    case SQLITE_BLOB: {
	char *text = 0;
	char const *blob = sqlite3_value_blob(argv[0]);
	int nblob = sqlite3_value_bytes(argv[0]);

	if (2 * nblob + 4 > 1000000000) {
	    sqlite3_result_error(context, "value too large", -1);
	    return;
	}
	text = (char *) sqlite3_malloc((2 * nblob) + 4);
	if (!text) {
	    sqlite3_result_error(context, "out of memory", -1);
	} else {
	    int i, k = 0;
	    static const char xdigits[] = "0123456789ABCDEF";

	    if (mode == 1) {
		/* ORACLE enclosed in '' */
		text[k++] = '\'';
	    } else if (mode == 2) {
		/* SQL Server 0x prefix */
		text[k++] = '0';
		text[k++] = 'x';
	    } else if (mode == 3) {
		/* MySQL x'..' */
		text[k++] = 'x';
		text[k++] = '\'';
	    } else {
		/* default */
		text[k++] = 'X';
		text[k++] = '\'';
	    }
	    for (i = 0; i < nblob; i++) {
		text[k++] = xdigits[(blob[i] >> 4 ) & 0x0F];
		text[k++] = xdigits[blob[i] & 0x0F];
	    }
	    if (mode == 1) {
		/* ORACLE enclosed in '' */
		text[k++] = '\'';
	    } else if (mode == 2) {
		/* SQL Server 0x prefix */
	    } else if (mode == 3) {
		/* MySQL x'..' */
		text[k++] = '\'';
	    } else {
		/* default */
		text[k++] = '\'';
	    }
	    text[k] = '\0';
	    sqlite3_result_text(context, text, k, SQLITE_TRANSIENT);
	    sqlite3_free(text);
	}
	break;
    }
    case SQLITE_TEXT: {
	int i, n;
	const unsigned char *arg = sqlite3_value_text(argv[0]);
	char *p;

	if (!arg) {
	    return;
	}
	for (i = 0, n = 0; arg[i]; i++) {
	    if (arg[i] == '\'') {
		n++;
	    }
	}
	if (i + n + 3 > 1000000000) {
	    sqlite3_result_error(context, "value too large", -1);
	    return;
	}
	p = sqlite3_malloc(i + n + 3);
	if (!p) {
	    sqlite3_result_error(context, "out of memory", -1);
	}
	p[0] = '\'';
	for (i = 0, n = 1; arg[i]; i++) {
	    p[n++] = arg[i];
	    if (arg[i] == '\'') {
		p[n++] = '\'';
	    }
	}
	p[n++] = '\'';
	p[n] = 0;
	sqlite3_result_text(context, p, n, SQLITE_TRANSIENT);
	sqlite3_free(p);
	break;
    }
    }
}

static void
quote_csv_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (argc < 1) {
	return;
    }
    switch (sqlite3_value_type(argv[0])) {
    case SQLITE_NULL: {
	sqlite3_result_text(context, "", 0, SQLITE_STATIC);
	break;
    }
    case SQLITE_INTEGER:
    case SQLITE_FLOAT: {
	sqlite3_result_value(context, argv[0]);
	break;
    }
    case SQLITE_BLOB: {
	char *text = 0;
	char const *blob = sqlite3_value_blob(argv[0]);
	int nblob = sqlite3_value_bytes(argv[0]);

	if (2 * nblob + 4 > 1000000000) {
	    sqlite3_result_error(context, "value too large", -1);
	    return;
	}
	text = (char *) sqlite3_malloc((2 * nblob) + 4);
	if (!text) {
	    sqlite3_result_error(context, "out of memory", -1);
	} else {
	    int i, k = 0;
	    static const char xdigits[] = "0123456789ABCDEF";

	    text[k++] = '"';
	    for (i = 0; i < nblob; i++) {
		text[k++] = xdigits[(blob[i] >> 4 ) & 0x0F];
		text[k++] = xdigits[blob[i] & 0x0F];
	    }
	    text[k++] = '"';
	    text[k] = '\0';
	    sqlite3_result_text(context, text, k, SQLITE_TRANSIENT);
	    sqlite3_free(text);
	}
	break;
    }
    case SQLITE_TEXT: {
	int i, n;
	const unsigned char *arg = sqlite3_value_text(argv[0]);
	char *p;

	if (!arg) {
	    return;
	}
	for (i = 0, n = 0; arg[i]; i++) {
	    if (arg[i] == '"') {
		n++;
	    }
	}
	if (i + n + 3 > 1000000000) {
	    sqlite3_result_error(context, "value too large", -1);
	    return;
	}
	p = sqlite3_malloc(i + n + 3);
	if (!p) {
	    sqlite3_result_error(context, "out of memory", -1);
	}
	p[0] = '"';
	for (i = 0, n = 1; arg[i]; i++) {
	    p[n++] = arg[i];
	    if (arg[i] == '"') {
		p[n++] = '"';
	    }
	}
	p[n++] = '"';
	p[n] = 0;
	sqlite3_result_text(context, p, n, SQLITE_TRANSIENT);
	sqlite3_free(p);
	break;
    }
    }
}

static void
indent_xml_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    static const char spaces[] = "                                ";
    int n = 0;

    if (argc > 0) {
	n = sqlite3_value_int(argv[0]);
	if (n > 32) {
	    n = 32;
	} else if (n < 0) {
	    n = 0;
	}
    }
    sqlite3_result_text(context, spaces, n, SQLITE_STATIC);
}

static void
quote_xml_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    static const char xdigits[] = "0123456789ABCDEF";
    int type, addtype = 0;

    if (argc < 1) {
	return;
    }
    if (argc > 1) {
	addtype = sqlite3_value_int(argv[1]);
    }
    type = sqlite3_value_type(argv[0]);
    switch (type) {
    case SQLITE_NULL: {
	if (addtype > 0) {
	    sqlite3_result_text(context, " TYPE=\"NULL\">", -1, SQLITE_STATIC);
	} else {
	    sqlite3_result_text(context, "", 0, SQLITE_STATIC);
	}
	break;
    }
    case SQLITE_INTEGER:
    case SQLITE_FLOAT: {
	if (addtype > 0) {
	    char *text = (char *) sqlite3_malloc(128);
	    int k;

	    if (!text) {
		sqlite3_result_error(context, "out of memory", -1);
		return;
	    }
	    strcpy(text, (type == SQLITE_FLOAT) ? " TYPE=\"REAL\">" :
		   " TYPE=\"INTEGER\">");
	    k = strlen(text);
	    strcpy(text + k, (char *) sqlite3_value_text(argv[0]));
	    k = strlen(text);
	    sqlite3_result_text(context, text, k, SQLITE_TRANSIENT);
	    sqlite3_free(text);
	} else {
	    sqlite3_result_value(context, argv[0]);
	}
	break;
    }
    case SQLITE_BLOB: {
	char *text = 0;
	char const *blob = sqlite3_value_blob(argv[0]);
	int nblob = sqlite3_value_bytes(argv[0]);
	int i, k = 0;

	if (6 * nblob + 34 > 1000000000) {
	    sqlite3_result_error(context, "value too large", -1);
	    return;
	}
	text = (char *) sqlite3_malloc((6 * nblob) + 34);
	if (!text) {
	    sqlite3_result_error(context, "out of memory", -1);
	    return;
	}
	if (addtype > 0) {
	    strcpy(text, " TYPE=\"BLOB\">");
	    k = strlen(text);
	}
	for (i = 0; i < nblob; i++) {
	    text[k++] = '&';
	    text[k++] = '#';
	    text[k++] = 'x';
	    text[k++] = xdigits[(blob[i] >> 4 ) & 0x0F];
	    text[k++] = xdigits[blob[i] & 0x0F];
	    text[k++] = ';';
	}
	text[k] = '\0';
	sqlite3_result_text(context, text, k, SQLITE_TRANSIENT);
	sqlite3_free(text);
	break;
    }
    case SQLITE_TEXT: {
	int i, n;
	const unsigned char *arg = sqlite3_value_text(argv[0]);
	char *p;

	if (!arg) {
	    return;
	}
	for (i = 0, n = 0; arg[i]; i++) {
	    if (arg[i] == '"' || arg[i] == '\'' ||
		arg[i] == '<' || arg[i] == '>' ||
		arg[i] == '&' || arg[i] < ' ') {
		n += 5;
	    }
	}
	if (i + n + 32 > 1000000000) {
	    sqlite3_result_error(context, "value too large", -1);
	    return;
	}
	p = sqlite3_malloc(i + n + 32);
	if (!p) {
	    sqlite3_result_error(context, "out of memory", -1);
	    return;
	}
	n = 0;
	if (addtype > 0) {
	    strcpy(p, " TYPE=\"TEXT\">");
	    n = strlen(p);
	}	    
	for (i = 0; arg[i]; i++) {
	    if (arg[i] == '"') {
		p[n++] = '&';
		p[n++] = 'q';
		p[n++] = 'u';
		p[n++] = 'o';
		p[n++] = 't';
		p[n++] = ';';
	    } else if (arg[i] == '\'') {
		p[n++] = '&';
		p[n++] = 'a';
		p[n++] = 'p';
		p[n++] = 'o';
		p[n++] = 's';
		p[n++] = ';';
	    } else if (arg[i] == '<') {
		p[n++] = '&';
		p[n++] = 'l';
		p[n++] = 't';
		p[n++] = ';';
	    } else if (arg[i] == '>') {
		p[n++] = '&';
		p[n++] = 'g';
		p[n++] = 't';
		p[n++] = ';';
	    } else if (arg[i] == '&') {
		p[n++] = '&';
		p[n++] = 'a';
		p[n++] = 'm';
		p[n++] = 'p';
		p[n++] = ';';
	    } else if (arg[i] < ' ') {
		p[n++] = '&';
		p[n++] = '#';
		p[n++] = 'x';
		p[n++] = xdigits[(arg[i] >> 4 ) & 0x0F];
		p[n++] = xdigits[arg[i] & 0x0F];
		p[n++] = ';';
	    } else if (addtype < 0 && arg[i] == ' ') {
		p[n++] = '&';
		p[n++] = '#';
		p[n++] = 'x';
		p[n++] = xdigits[(arg[i] >> 4 ) & 0x0F];
		p[n++] = xdigits[arg[i] & 0x0F];
		p[n++] = ';';
	    } else {
		p[n++] = arg[i];
	    }
	}
	p[n] = '\0';
	sqlite3_result_text(context, p, n, SQLITE_TRANSIENT);
	sqlite3_free(p);
	break;
    }
    }
}

static void
import_func(sqlite3_context *ctx, int nargs, sqlite3_value **args)
{
    sqlite3 *db = (sqlite3 *) sqlite3_user_data(ctx);
    int changes0 = sqlite3_changes(db);
    char *filename = 0;
    FILE *fin;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    if (nargs > 0) {
	if (sqlite3_value_type(args[0]) != SQLITE_NULL) {
	    filename = (char *) sqlite3_value_text(args[0]);
	}
    }
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST |
		    OFN_EXPLORER | OFN_PATHMUSTEXIST;
	if (GetOpenFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
    fin = fopen(filename, "r");
    if (!fin) {
	goto done;
    }
    process_input(db, fin);
    fclose(fin);
done:
    sqlite3_result_int(ctx, sqlite3_changes(db) - changes0);
}

#ifdef STANDALONE
int
impexp_import_sql(sqlite3 *db, char *filename)
{
    int changes0;
    FILE *fin;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    if (!db) {
	return 0;
    }
    changes0 = sqlite3_changes(db);
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST |
		    OFN_EXPLORER | OFN_PATHMUSTEXIST;
	if (GetOpenFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
    fin = fopen(filename, "r");
    if (!fin) {
	goto done;
    }
    process_input(db, fin);
    fclose(fin);
done:
    return sqlite3_changes(db) - changes0;
}
#endif

typedef struct {
    sqlite3 *db;
    int with_schema;
    int quote_mode;
    char *where;
    int nlines;
    int indent;
    FILE *out;
} DUMP_DATA;

static void
indent(DUMP_DATA *dd)
{
    int i;

    for (i = 0; i < dd->indent; i++) {
	fputc(' ', dd->out);
    }
}

static int
table_dump(DUMP_DATA *dd, char **errp, int fmt, const char *query, ...)
{
    sqlite3_stmt *select;
    int rc;
    const char *rest, *q = query;
    va_list ap;

    if (errp && *errp) {
	sqlite3_free(*errp);
	*errp = 0;
    }
    if (fmt) {
	va_start(ap, query);
	q = sqlite3_vmprintf(query, ap);
	va_end(ap);
	if (!q) {
	    return SQLITE_NOMEM;
	}
    }
#if defined(HAVE_SQLITE3PREPAREV2) && HAVE_SQLITE3PREPAREV2
    rc = sqlite3_prepare_v2(dd->db, q, -1, &select, &rest);
#else
    rc = sqlite3_prepare(dd->db, q, -1, &select, &rest);
#endif
    if (fmt) {
	sqlite3_free((char *) q);
    }
    if (rc != SQLITE_OK || !select) {
	return rc;
    }
    rc = sqlite3_step(select);
    while (rc == SQLITE_ROW) {
	if (fputs((char *) sqlite3_column_text(select, 0), dd->out) > 0) {
	    dd->nlines++;
	}
	if (dd->quote_mode >= 0) {
	    fputc(';', dd->out);
	}
	if (dd->quote_mode == -1) {
	    fputc('\r', dd->out);
	}
	if (dd->quote_mode >= -1) {
	    fputc('\n', dd->out);
	}
	rc = sqlite3_step(select);
    }
    rc = sqlite3_finalize(select);
    if (rc != SQLITE_OK) {
	if (errp) {
	    *errp = sqlite3_mprintf("%s", sqlite3_errmsg(dd->db));
	}
    }
    return rc;
}

static char *
append(char *in, char const *append, char quote)
{
    int len, i;
    int nappend = append ? strlen(append) : 0;
    int nin = in ? strlen(in) : 0;
    char *tmp;

    len = nappend + nin + 1;
    if (quote) {
	len += 2;
	for (i = 0; i < nappend; i++) {
	    if (append[i] == quote) {
		len++;
	    }
	}
    } else if (!nappend) {
	return in;
    }
    tmp = (char *) sqlite3_realloc(in, len);
    if (!tmp) {
	sqlite3_free(in);
	return 0;
    }
    in = tmp;
    if (quote) {
	char *p = in + nin;

	*p++ = quote;
	for (i = 0; i < nappend; i++) {
	    *p++ = append[i];
	    if (append[i] == quote) {
		*p++ = quote;
	    }
	}
	*p++ = quote;
	*p++ = '\0';
    } else {
	if (nappend) {
	    memcpy(in + nin, append, nappend);
	}
	in[len - 1] = '\0';
    }
    return in;
}

static void
quote_xml_str(DUMP_DATA *dd, char *str)
{
    static const char xdigits[] = "0123456789ABCDEF";
    int i;

    if (!str) {
	return;
    }
    for (i = 0; str[i]; i++) {
	if (str[i] == '"') {
	    fputs("&quot;", dd->out);
	} else if (str[i] == '\'') {
	    fputs("&apos;", dd->out);
	} else if (str[i] == '<') {
	    fputs("&lt;", dd->out);
	} else if (str[i] == '>') {
	    fputs("&gt;", dd->out);
	} else if (str[i] == '&') {
	    fputs("&amp;", dd->out);
	} else if ((unsigned char) str[i] <= ' ') {
	    char buf[8];

	    buf[0] = '&';
	    buf[1] = '&';
	    buf[2] = '#';
	    buf[3] = 'x';
	    buf[4] = xdigits[(str[i] >> 4 ) & 0x0F];
	    buf[5] = xdigits[str[i] & 0x0F];
	    buf[6] = ';';
	    buf[7] = '\0';
	    fputs(buf, dd->out);
	} else {
	    fputc(str[i], dd->out);
	}
    }
}

static int
dump_cb(void *udata, int nargs, char **args, char **cols)
{
    int rc;
    const char *table, *type, *sql;
    DUMP_DATA *dd = (DUMP_DATA *) udata;

    if (nargs != 3) {
	return 1;
    }
    table = args[0];
    type = args[1];
    sql = args[2];
    if (strcmp(table, "sqlite_sequence") == 0) {
	if (dd->with_schema) {
	    if (fputs("DELETE FROM sqlite_sequence;\n", dd->out) >= 0) {
		dd->nlines++;
	    }
	}
    } else if (strcmp(table, "sqlite_stat1") == 0) {
	if (dd->with_schema) {
	    if (fputs("ANALYZE sqlite_master;\n", dd->out) >= 0) {
		dd->nlines++;
	    }
	}
    } else if (strncmp(table, "sqlite_", 7) == 0) {
	return 0;
    } else if (strncmp(sql, "CREATE VIRTUAL TABLE", 20) == 0) {
	if (dd->with_schema) {
	    sqlite3_stmt *stmt = 0;
	    char *creat = 0, *table_info = 0;
   
	    table_info = append(table_info, "PRAGMA table_info(", 0);
	    table_info = append(table_info, table, '"');
	    table_info = append(table_info, ");", 0);
#if defined(HAVE_SQLITE3PREPAREV2) && HAVE_SQLITE3PREPAREV2
	    rc = sqlite3_prepare_v2(dd->db, table_info, -1, &stmt, 0);
#else
	    rc = sqlite3_prepare(dd->db, table_info, -1, &stmt, 0);
#endif
	    if (table_info) {
		sqlite3_free(table_info);
		table_info = 0;
	    }
	    if (rc != SQLITE_OK || !stmt) {
bailout0:
		if (creat) {
		    sqlite3_free(creat);
		}
		return 1;
	    }
	    creat = append(creat, table, '"');
	    creat = append(creat, "(", 0);
	    rc = sqlite3_step(stmt);
	    while (rc == SQLITE_ROW) {
		const char *p;

		p = (const char *) sqlite3_column_text(stmt, 1);
		creat = append(creat, p, '"');
		creat = append(creat, " ", 0);
		p = (const char *) sqlite3_column_text(stmt, 2);
		if (p && p[0]) {
		    creat = append(creat, p, 0);
		}
		if (sqlite3_column_int(stmt, 5)) {
		    creat = append(creat, " PRIMARY KEY", 0);
		}
		if (sqlite3_column_int(stmt, 3)) {
		    creat = append(creat, " NOT NULL", 0);
		}
		p = (const char *) sqlite3_column_text(stmt, 4);
		if (p && p[0]) {
		    creat = append(creat, " DEFAULT ", 0);
		    creat = append(creat, p, 0);
		}
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
		    creat = append(creat, ",", 0);
		}
	    }
	    rc = sqlite3_finalize(stmt);
	    if (rc != SQLITE_OK) {
		goto bailout0;
	    }
	    creat = append(creat, ")", 0);
	    if (creat && fprintf(dd->out, "CREATE TABLE %s;\n", creat) > 0) {
		dd->nlines++;
	    }
	    if (creat) {
		sqlite3_free(creat);
	    }
	}
    } else {
	if (dd->with_schema) {
	    if (fprintf(dd->out, "%s;\n", sql) > 0) {
		dd->nlines++;
	    }
	}
    }
    if (strcmp(type, "table") == 0 ||
	(dd->quote_mode < 0 && strcmp(type, "view") == 0)) {
	sqlite3_stmt *stmt = 0;
	char *select = 0, *hdr = 0, *table_info = 0, *tmp = 0;
	char buffer[256];
   
	table_info = append(table_info, "PRAGMA table_info(", 0);
	table_info = append(table_info, table, '"');
	table_info = append(table_info, ");", 0);
#if defined(HAVE_SQLITE3PREPAREV2) && HAVE_SQLITE3PREPAREV2
	rc = sqlite3_prepare_v2(dd->db, table_info, -1, &stmt, 0);
#else
	rc = sqlite3_prepare(dd->db, table_info, -1, &stmt, 0);
#endif
	if (rc != SQLITE_OK || !stmt) {
bailout1:
	    if (hdr) {
		sqlite3_free(hdr);
	    }
	    if (select) {
		sqlite3_free(select);
	    }
	    if (table_info) {
		sqlite3_free(table_info);
	    }
	    return 1;
	}
	if (dd->quote_mode < -1) {
	    if (dd->where) {
		select = append(select, "SELECT ", 0);
		sprintf(buffer, "indent_xml(%d)", dd->indent);
		select = append(select, buffer, 0);
		select = append(select, " || '<' || quote_xml(", 0);
		select = append(select, dd->where, '"');
		select = append(select, ",-1) || '>\n' || ", 0);
	    } else {
		select = append(select, "SELECT ", 0);
	    }
	} else if (dd->quote_mode < 0) {
	    if (dd->where) {
		select = append(select, "SELECT quote_csv(", 0);
		select = append(select, dd->where, '"');
		select = append(select, ") || ',' || ", 0);
	    } else {
		select = append(select, "SELECT ", 0);
	    }
	    if (dd->indent) {
		hdr = append(hdr, select, 0);
	    }
	} else {
	    if (dd->with_schema) {
		select = append(select, "SELECT 'INSERT INTO ' || ", 0);
	    } else {
		select = append(select, "SELECT 'INSERT OR REPLACE INTO ' || ",
				0);
	    }
	    tmp = append(tmp, table, '"');
	    if (tmp) {
		select = append(select, tmp, '\'');
		sqlite3_free(tmp);
		tmp = 0;
	    }
	}
	if (dd->quote_mode >= 0 && !dd->with_schema) {
	    select = append(select, " || ' (' || ", 0);
	    rc = sqlite3_step(stmt);
	    while (rc == SQLITE_ROW) {
		const char *text = (const char *) sqlite3_column_text(stmt, 1);

		tmp = append(tmp, text, '"');
		if (tmp) {
		    select = append(select, tmp, '\'');
		    sqlite3_free(tmp);
		    tmp = 0;
		}
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
		    select = append(select, " || ',' || ", 0);
		}
	    }
	    rc = sqlite3_reset(stmt);
	    if (rc != SQLITE_OK) {
		goto bailout1;
	    }
	    select = append(select, "|| ')'", 0);
	}
	if (dd->quote_mode == -1 && dd->indent) {
	    rc = sqlite3_step(stmt);
	    while (rc == SQLITE_ROW) {
		const char *text = (const char *) sqlite3_column_text(stmt, 1);

		hdr = append(hdr, "quote_csv(", 0);
		hdr = append(hdr, text, '"');
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
		    hdr = append(hdr, ") || ',' || ", 0);
		} else {
		    hdr = append(hdr, ")", 0);
		}
	    }
	    rc = sqlite3_reset(stmt);
	    if (rc != SQLITE_OK) {
		goto bailout1;
	    }
	}
	if (dd->quote_mode >= 0) {
	    select = append(select, " || ' VALUES(' || ", 0);
	}
	rc = sqlite3_step(stmt);
	while (rc == SQLITE_ROW) {
	    const char *text = (const char *) sqlite3_column_text(stmt, 1);
	    const char *type = (const char *) sqlite3_column_text(stmt, 2);
	    int tlen = strlen(type ? type : "");

	    if (dd->quote_mode < -1) {
		sprintf(buffer, "indent_xml(%d)", dd->indent + 1);
		select = append(select, buffer, 0);
		select = append(select, "|| '<' || quote_xml(", 0);
		select = append(select, text, '\'');
		select = append(select, ",-1) || quote_xml(", 0);
		select = append(select, text, '"');
		select = append(select, ",1) || '</' || quote_xml(", 0);
		select = append(select, text, '\'');
		select = append(select, ",-1) || '>\n'", 0);
	    } else if (dd->quote_mode < 0) {
		/* leave out BLOB columns */
		if ((tlen >= 4 && strncasecmp(type, "BLOB", 4) == 0) ||
		    (tlen >= 6 && strncasecmp(type, "BINARY", 6) == 0)) {
		    rc = sqlite3_step(stmt);
		    if (rc != SQLITE_ROW) {
			tlen = strlen(select);
			if (tlen > 10) {
			    select[tlen - 10] = '\0';
			}
		    }
		    continue;
		}
		select = append(select, "quote_csv(", 0);
		select = append(select, text, '"');
	    } else {
		select = append(select, "quote_sql(", 0);
		select = append(select, text, '"');
		if (dd->quote_mode) {
		    char mbuf[32];

		    sprintf(mbuf, ",%d", dd->quote_mode);
		    select = append(select, mbuf, 0);
		}
	    }
	    rc = sqlite3_step(stmt);
	    if (rc == SQLITE_ROW) {
		if (dd->quote_mode >= -1) {
		    select = append(select, ") || ',' || ", 0);
		} else {
		    select = append(select, " || ", 0);
		}
	    } else {
		if (dd->quote_mode >= -1) {
		    select = append(select, ") ", 0);
		} else {
		    select = append(select, " ", 0);
		}
	    }
	}
	rc = sqlite3_finalize(stmt);
	if (rc != SQLITE_OK) {
	    goto bailout1;
	}
	if (dd->quote_mode >= 0) {
	    select = append(select, "|| ')' FROM ", 0);
	} else {
	    if (dd->quote_mode < -1 && dd->where) {
		sprintf(buffer, " || indent_xml(%d)", dd->indent);
		select = append(select, buffer, 0);
		select = append(select, " || '</' || quote_xml(", 0);
		select = append(select, dd->where, '"');
		select = append(select, ",-1) || '>\n' FROM ", 0);
	    } else {
		select = append(select, "FROM ", 0);
	    }
	}
	select = append(select, table, '"');
	if (dd->quote_mode >= 0 && dd->where) {
	    select = append(select, " ", 0);
	    select = append(select, dd->where, 0);
	}
	if (table_info) {
	    sqlite3_free(table_info);
	    table_info = 0;
	}
	if (hdr) {
	    rc = table_dump(dd, 0, 0, hdr);
	    sqlite3_free(hdr);
	    hdr = 0;
	}
	rc = table_dump(dd, 0, 0, select);
	if (rc == SQLITE_CORRUPT) {
	    select = append(select, " ORDER BY rowid DESC", 0);
	    rc = table_dump(dd, 0, 0, select);
	}
	if (select) {
	    sqlite3_free(select);
	    select = 0;
	}
    }
    return 0;
}

static int
schema_dump(DUMP_DATA *dd, char **errp, const char *query, ...)
{
    int rc;
    char *q;
    va_list ap;

    if (errp) {
	sqlite3_free(*errp);
	*errp = 0;
    }
    va_start(ap, query);
    q = sqlite3_vmprintf(query, ap);
    va_end(ap);
    if (!q) {
	return SQLITE_NOMEM;
    }
    rc = sqlite3_exec(dd->db, q, dump_cb, dd, errp);
    if (rc == SQLITE_CORRUPT) {
	char *tmp;

	tmp = sqlite3_mprintf("%s ORDER BY rowid DESC", q);
	sqlite3_free(q);
	if (!tmp) {
	    return rc;
	}
	q = tmp;
	if (errp) {
	    sqlite3_free(*errp);
	    *errp = 0;
	}
	rc = sqlite3_exec(dd->db, q, dump_cb, dd, errp);
    }
    sqlite3_free(q);
    return rc;
}

static void
export_func(sqlite3_context *ctx, int nargs, sqlite3_value **args)
{
    DUMP_DATA dd0, *dd = &dd0;
    sqlite3 *db = (sqlite3 *) sqlite3_user_data(ctx);
    int i, mode = 0;
    char *filename = 0;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    dd->db = db;
    dd->where = 0;
    dd->nlines = -1;
    dd->indent = 0;
    if (nargs > 0) {
	if (sqlite3_value_type(args[0]) != SQLITE_NULL) {
	    filename = (char *) sqlite3_value_text(args[0]);
	}
    }
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER |
		    OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (GetSaveFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
    dd->out = fopen(filename, "w");
    if (!dd->out) {
	goto done;
    }
    if (nargs > 1) {
	mode = sqlite3_value_int(args[1]);
    }
    dd->with_schema = !(mode & 1);
    dd->quote_mode = (mode >> 8) & 3;
    dd->nlines = 0;
    if (fputs("BEGIN TRANSACTION;\n", dd->out) >= 0) {
	dd->nlines++;
    }
    if (nargs <= 2) {
	schema_dump(dd, 0,
		    "SELECT name, type, sql FROM sqlite_master"
		    " WHERE sql NOT NULL AND type = 'table'");
	if (dd->with_schema) {
	    table_dump(dd, 0, 0,
		       "SELECT sql FROM sqlite_master WHERE"
		       " sql NOT NULL AND type IN ('index','trigger','view')");
	}
    } else {
	for (i = 2; i < nargs; i += (mode & 2) ? 2 : 1) {
	    dd->where = 0;
	    if ((mode & 2) && i + 1 < nargs) {
		dd->where = (char *) sqlite3_value_text(args[i + 1]);
	    }
	    schema_dump(dd, 0,
			"SELECT name, type, sql FROM sqlite_master"
			" WHERE tbl_name LIKE %Q AND type = 'table'"
			" AND sql NOT NULL",
			sqlite3_value_text(args[i]));
	    if (dd->with_schema) {
		table_dump(dd, 0, 1,
			   "SELECT sql FROM sqlite_master"
			   " WHERE sql NOT NULL"
			   " AND type IN ('index','trigger','view')"
			   " AND tbl_name LIKE %Q",
			   sqlite3_value_text(args[i]));
	    }
	}
    }
    if (fputs("COMMIT;\n", dd->out) >= 0) {
	dd->nlines++;
    }
    fclose(dd->out);
done:
    sqlite3_result_int(ctx, dd->nlines);
}

static void
export_csv_func(sqlite3_context *ctx, int nargs, sqlite3_value **args)
{
    DUMP_DATA dd0, *dd = &dd0;
    sqlite3 *db = (sqlite3 *) sqlite3_user_data(ctx);
    int i;
    char *filename = 0;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    dd->db = db;
    dd->where = 0;
    dd->nlines = -1;
    dd->indent = 0;
    dd->with_schema = 0;
    dd->quote_mode = -1;
    if (nargs > 0) {
	if (sqlite3_value_type(args[0]) != SQLITE_NULL) {
	    filename = (char *) sqlite3_value_text(args[0]);
	}
    }
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER |
		    OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (GetSaveFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
#ifdef _WIN32
    dd->out = fopen(filename, "wb");
#else
    dd->out = fopen(filename, "w");
#endif
    if (!dd->out) {
	goto done;
    }
    dd->nlines = 0;
    if (nargs > 1) {
	if (sqlite3_value_type(args[1]) != SQLITE_NULL) {
	    if (sqlite3_value_int(args[1])) {
		dd->indent = 1;
	    }
	}
    }
    for (i = 2; i <= nargs - 3; i += 3) {
	char *schema = 0, *sql;

	dd->where = 0;
	if (sqlite3_value_type(args[i]) != SQLITE_NULL) {
	    dd->where = (char *) sqlite3_value_text(args[i]);
	    if (dd->where && !dd->where[0]) {
		dd->where = 0;
	    }
	}
	if (sqlite3_value_type(args[i + 2]) != SQLITE_NULL) {
	    schema = (char *) sqlite3_value_text(args[i + 2]);
	}
	if (!schema || schema[0] == '\0') {
	    schema = "sqlite_master";
	}
	sql = sqlite3_mprintf("SELECT name, type, sql FROM %s"
			      " WHERE tbl_name LIKE %%Q AND "
			      " (type = 'table' OR type = 'view')"
			      " AND sql NOT NULL", schema);
	if (sql) {
	    schema_dump(dd, 0, sql, sqlite3_value_text(args[i + 1]));
	    sqlite3_free(sql);
	}
    }
    fclose(dd->out);
done:
    sqlite3_result_int(ctx, dd->nlines);
}

static void
export_xml_func(sqlite3_context *ctx, int nargs, sqlite3_value **args)
{
    DUMP_DATA dd0, *dd = &dd0;
    sqlite3 *db = (sqlite3 *) sqlite3_user_data(ctx);
    int i;
    char *filename = 0;
    char *openmode = "w";
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    dd->db = db;
    dd->where = 0;
    dd->nlines = -1;
    dd->indent = 0;
    dd->with_schema = 0;
    dd->quote_mode = -2;
    if (nargs > 0) {
	if (sqlite3_value_type(args[0]) != SQLITE_NULL) {
	    filename = (char *) sqlite3_value_text(args[0]);
	}
    }
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER |
		    OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (GetSaveFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
    if (nargs > 1) {
	if (sqlite3_value_type(args[1]) != SQLITE_NULL) {
	    if (sqlite3_value_int(args[1])) {
		openmode = "a";
	    }
	}
    }
    if (nargs > 2) {
	if (sqlite3_value_type(args[2]) != SQLITE_NULL) {
	    dd->indent = sqlite3_value_int(args[2]);
	    if (dd->indent < 0) {
		dd->indent = 0;
	    }
	}
    }
    dd->out = fopen(filename, openmode);
    if (!dd->out) {
	goto done;
    }
    dd->nlines = 0;
    for (i = 3; i <= nargs - 4; i += 4) {
	char *root = 0, *schema = 0, *sql;

	if (sqlite3_value_type(args[i]) != SQLITE_NULL) {
	    root = (char *) sqlite3_value_text(args[i]);
	    if (root && !root[0]) {
		root = 0;
	    }
	}
	dd->where = 0;
	if (sqlite3_value_type(args[i + 1]) != SQLITE_NULL) {
	    dd->where = (char *) sqlite3_value_text(args[i + 1]);
	    if (dd->where && !dd->where[0]) {
		dd->where = 0;
	    }
	}
	if (root) {
	    indent(dd);
	    dd->indent++;
	    fputs("<", dd->out);
	    quote_xml_str(dd, root);
	    fputs(">\n", dd->out);
	}
	if (sqlite3_value_type(args[i + 3]) != SQLITE_NULL) {
	    schema = (char *) sqlite3_value_text(args[i + 3]);
	}
	if (!schema || schema[0] == '\0') {
	    schema = "sqlite_master";
	}
	sql = sqlite3_mprintf("SELECT name, type, sql FROM %s"
			      " WHERE tbl_name LIKE %%Q AND"
			      " (type = 'table' OR type = 'view')"
			      " AND sql NOT NULL", schema);
	if (sql) {
	    schema_dump(dd, 0, sql, sqlite3_value_text(args[i + 2]));
	    sqlite3_free(sql);
	}
	if (root) {
	    dd->indent--;
	    indent(dd);
	    fputs("</", dd->out);
	    quote_xml_str(dd, root);
	    fputs(">\n", dd->out);
	}
    }
    fclose(dd->out);
done:
    sqlite3_result_int(ctx, dd->nlines);
}


#ifdef STANDALONE
int
impexp_export_sql(sqlite3 *db, char *filename, int mode, ...)
{
    DUMP_DATA dd0, *dd = &dd0;
    va_list ap;
    char *table;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    if (!db) {
	return 0;
    }
    dd->db = db;
    dd->where = 0;
    dd->nlines = -1;
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER |
		    OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (GetSaveFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
    dd->out = fopen(filename, "w");
    if (!dd->out) {
	goto done;
    }
    dd->with_schema = !(mode & 1);
    dd->nlines = 0;
    if (fputs("BEGIN TRANSACTION;\n", dd->out) >= 0) {
	dd->nlines++;
    }
    va_start(ap, mode);
    table = va_arg(ap, char *);
    if (!table) {
	schema_dump(dd, 0,
		    "SELECT name, type, sql FROM sqlite_master"
		    " WHERE sql NOT NULL AND type = 'table'");
	if (dd->with_schema) {
	    table_dump(dd, 0, 0,
		       "SELECT sql FROM sqlite_master WHERE"
		       " sql NOT NULL AND type IN ('index','trigger','view')");
	}
    } else {
	while (table) {
	    dd->where = 0;
	    if ((mode & 2)) {
		dd->where = va_arg(ap, char *);
	    }
	    schema_dump(dd, 0,
			"SELECT name, type, sql FROM sqlite_master"
			" WHERE tbl_name LIKE %Q AND type = 'table'"
			" AND sql NOT NULL", table);
	    if (dd->with_schema) {
		table_dump(dd, 0, 1,
			   "SELECT sql FROM sqlite_master"
			   " WHERE sql NOT NULL"
			   " AND type IN ('index','trigger','view')"
			   " AND tbl_name LIKE %Q", table);
	    }
	    table = va_arg(ap, char *);
	}
    }
    va_end(ap);
    if (fputs("COMMIT;\n", dd->out) >= 0) {
	dd->nlines++;
    }
    fclose(dd->out);
done:
    return dd->nlines;
}
#endif

#ifdef STANDALONE
int
impexp_export_csv(sqlite3 *db, char *filename, int hdr, ...)
{
    DUMP_DATA dd0, *dd = &dd0;
    va_list ap;
    char *prefix, *table, *schema;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    if (!db) {
	return 0;
    }
    dd->db = db;
    dd->where = 0;
    dd->nlines = -1;
    dd->indent = 0;
    dd->with_schema = 0;
    dd->quote_mode = -1;
    dd->indent = hdr != 0;
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER |
		    OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (GetSaveFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
#ifdef _WIN32
    dd->out = fopen(filename, "wb");
#else
    dd->out = fopen(filename, "w");
#endif
    if (!dd->out) {
	goto done;
    }
    dd->nlines = 0;
    va_start(ap, hdr);
    prefix = va_arg(ap, char *);
    table = va_arg(ap, char *);
    schema = va_arg(ap, char *);
    while (table != NULL) {
	char *sql;

	dd->where = (prefix && prefix[0]) ? prefix : 0;
	if (!schema || schema[0] == '\0') {
	    schema = "sqlite_master";
	}
	sql = sqlite3_mprintf("SELECT name, type, sql FROM %s"
			      " WHERE tbl_name LIKE %%Q AND"
			      " (type = 'table' OR type = 'view')"
			      " AND sql NOT NULL", schema);
	if (sql) {
	    schema_dump(dd, 0, sql, table);
	    sqlite3_free(sql);
	}
	prefix = va_arg(ap, char *);
	table = va_arg(ap, char *);
	schema = va_arg(ap, char *);
    }
    va_end(ap);
    fclose(dd->out);
done:
    return dd->nlines;
}
#endif

#ifdef STANDALONE
int
impexp_export_xml(sqlite3 *db, char *filename, int append, int indnt,
		  char *root, char *item, char *tablename, char *schema)
{
    DUMP_DATA dd0, *dd = &dd0;
    char *sql;
#ifdef _WIN32
    char fnbuf[MAX_PATH];
#endif

    if (!db) {
	return 0;
    }
    dd->db = db;
    dd->where = item;
    dd->nlines = -1;
    dd->indent = indnt > 0 ? indnt : 0;
    dd->with_schema = 0;
    dd->quote_mode = -2;
#ifdef _WIN32
    if (!filename) {
	OPENFILENAME ofn;

	memset(&ofn, 0, sizeof (ofn));
	memset(fnbuf, 0, sizeof (fnbuf));
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFile = fnbuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER |
		    OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (GetSaveFileName(&ofn)) {
	    filename = fnbuf;
	}
    }
#endif
    if (!filename) {
	goto done;
    }
    dd->out = fopen(filename, append ? "a" : "w");
    if (!dd->out) {
	goto done;
    }
    dd->nlines = 0;
    if (root) {
	indent(dd);
	dd->indent++;
	fputs("<", dd->out);
	quote_xml_str(dd, root);
	fputs(">\n", dd->out);
    }
    if (!schema || schema[0] == '\0') {
	schema = "sqlite_master";
    }
    sql = sqlite3_mprintf("SELECT name, type, sql FROM %s"
			  " WHERE tbl_name LIKE %%Q AND"
			  " (type = 'table' OR type = 'view')"
			  " AND sql NOT NULL", schema);
    if (sql) {
	schema_dump(dd, 0, sql, tablename);
	sqlite3_free(sql);
    }
    if (root) {
	dd->indent--;
	indent(dd);
	fputs("</", dd->out);
	quote_xml_str(dd, root);
	fputs(">\n", dd->out);
    }
    fclose(dd->out);
done:
    return dd->nlines;
}
#endif

#ifdef STANDALONE
int
impexp_init(sqlite3 *db)
#else
int
sqlite3_extension_init(sqlite3 *db, char **errmsg, 
		       const sqlite3_api_routines *api)
#endif
{
    int rc, i;
    static const struct {
	const char *name;
	void (*func)(sqlite3_context *, int, sqlite3_value **);
	int nargs;
	int textrep;
    } ftab[] = {
	{ "quote_sql",	quote_func,      -1, SQLITE_UTF8 },
	{ "import_sql",	import_func,     -1, SQLITE_UTF8 },
	{ "export_sql",	export_func,     -1, SQLITE_UTF8 },
	{ "quote_csv",	quote_csv_func,  -1, SQLITE_UTF8 },
	{ "export_csv",	export_csv_func, -1, SQLITE_UTF8 },
	{ "indent_xml",	indent_xml_func,  1, SQLITE_UTF8 },
	{ "quote_xml",	quote_xml_func,  -1, SQLITE_UTF8 },
	{ "export_xml",	export_xml_func, -1, SQLITE_UTF8 }
    };

#ifndef STANDALONE
    SQLITE_EXTENSION_INIT2(api);
#endif

    for (i = 0; i < sizeof (ftab) / sizeof (ftab[0]); i++) {
	rc = sqlite3_create_function(db, ftab[i].name, ftab[i].nargs,
				     ftab[i].textrep, db, ftab[i].func, 0, 0);
	if (rc != SQLITE_OK) {
	    for (--i; i >= 0; --i) {
		sqlite3_create_function(db, ftab[i].name, ftab[i].nargs,
					ftab[i].textrep, 0, 0, 0, 0);
	    }
	    break;
	}
    }
    return rc;
}

