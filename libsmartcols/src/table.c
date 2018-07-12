/*
 * table.c - functions handling the data at the table level
 *
 * Copyright (C) 2010-2014 Karel Zak <kzak@redhat.com>
 * Copyright (C) 2014 Ondrej Oprala <ooprala@redhat.com>
 * Copyright (C) 2016 Igor Gnatenko <i.gnatenko.brain@gmail.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

/**
 * SECTION: table
 * @title: Table
 * @short_description: container for rows and columns
 *
 * Table data manipulation API.
 */


#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <ctype.h>

#include "nls.h"
#include "ttyutils.h"
#include "smartcolsP.h"

#ifdef HAVE_WIDECHAR
#define UTF_V	"\342\224\202"	/* U+2502, Vertical line drawing char */
#define UTF_VR	"\342\224\234"	/* U+251C, Vertical and right */
#define UTF_H	"\342\224\200"	/* U+2500, Horizontal */
#define UTF_UR	"\342\224\224"	/* U+2514, Up and right */
#endif /* !HAVE_WIDECHAR */

#define is_last_column(_tb, _cl) \
		list_entry_is_last(&(_cl)->cl_columns, &(_tb)->tb_columns)


static void check_padding_debug(struct libscols_table *tb)
{
	const char *str;

	assert(libsmartcols_debug_mask);	/* debug has to be enabled! */

	str = getenv("LIBSMARTCOLS_DEBUG_PADDING");
	if (!str || (strcmp(str, "on") != 0 && strcmp(str, "1") != 0))
		return;

	DBG(INIT, ul_debugobj(tb, "padding debug: ENABLE"));
	tb->padding_debug = 1;
}

/**
 * scols_new_table:
 *
 * Returns: A newly allocated table.
 */
struct libscols_table *scols_new_table(void)
{
	struct libscols_table *tb;

	tb = calloc(1, sizeof(struct libscols_table));
	if (!tb)
		return NULL;

	tb->refcount = 1;
	tb->out = stdout;
	tb->termwidth = get_terminal_width(80);

	INIT_LIST_HEAD(&tb->tb_lines);
	INIT_LIST_HEAD(&tb->tb_columns);

	DBG(TAB, ul_debugobj(tb, "alloc"));
	ON_DBG(INIT, check_padding_debug(tb));

	return tb;
}

/**
 * scols_ref_table:
 * @tb: a pointer to a struct libscols_table instance
 *
 * Increases the refcount of @tb.
 */
void scols_ref_table(struct libscols_table *tb)
{
	if (tb)
		tb->refcount++;
}

/**
 * scols_unref_table:
 * @tb: a pointer to a struct libscols_table instance
 *
 * Decreases the refcount of @tb. When the count falls to zero, the instance
 * is automatically deallocated.
 */
void scols_unref_table(struct libscols_table *tb)
{
	if (tb && (--tb->refcount <= 0)) {
		DBG(TAB, ul_debugobj(tb, "dealloc"));
		scols_table_remove_lines(tb);
		scols_table_remove_columns(tb);
		scols_unref_symbols(tb->symbols);
		scols_reset_cell(&tb->title);
		free(tb->linesep);
		free(tb->colsep);
		free(tb->name);
		free(tb);
	}
}

/**
 * scols_table_set_name:
 * @tb: a pointer to a struct libscols_table instance
 * @name: a name
 *
 * The table name is used for example for JSON top level object name.
 *
 * Returns: 0, a negative number in case of an error.
 *
 * Since: 2.27
 */
int scols_table_set_name(struct libscols_table *tb, const char *name)
{
	return strdup_to_struct_member(tb, name, name);
}

/**
 * scols_table_get_name:
 * @tb: a pointer to a struct libscols_table instance
 *
 * Returns: The current name setting of the table @tb
 *
 * Since: 2.29
 */
const char *scols_table_get_name(const struct libscols_table *tb)
{
	return tb->name;
}

/**
 * scols_table_get_title:
 * @tb: a pointer to a struct libscols_table instance
 *
 * The returned pointer is possible to modify by cell functions. Note that
 * title output alignment on non-tty is hardcoded to 80 output chars. For the
 * regular terminal it's based on terminal width.
 *
 * Returns: Title of the table, or %NULL in case of blank title.
 *
 * Since: 2.28
 */
struct libscols_cell *scols_table_get_title(struct libscols_table *tb)
{
	return &tb->title;
}

/**
 * scols_table_add_column:
 * @tb: a pointer to a struct libscols_table instance
 * @cl: a pointer to a struct libscols_column instance
 *
 * Adds @cl to @tb's column list. The column cannot be shared between more
 * tables.
 *
 * Returns: 0, a negative number in case of an error.
 */
int scols_table_add_column(struct libscols_table *tb, struct libscols_column *cl)
{
	if (!tb || !cl || !list_empty(&tb->tb_lines) || cl->table)
		return -EINVAL;

	if (cl->flags & SCOLS_FL_TREE)
		tb->ntreecols++;

	DBG(TAB, ul_debugobj(tb, "add column %p", cl));
	list_add_tail(&cl->cl_columns, &tb->tb_columns);
	cl->seqnum = tb->ncols++;
	cl->table = tb;
	scols_ref_column(cl);

	/* TODO:
	 *
	 * Currently it's possible to add/remove columns only if the table is
	 * empty (see list_empty(tb->tb_lines) above). It would be nice to
	 * enlarge/reduce lines cells[] always when we add/remove a new column.
	 */
	return 0;
}

/**
 * scols_table_remove_column:
 * @tb: a pointer to a struct libscols_table instance
 * @cl: a pointer to a struct libscols_column instance
 *
 * Removes @cl from @tb.
 *
 * Returns: 0, a negative number in case of an error.
 */
int scols_table_remove_column(struct libscols_table *tb,
			      struct libscols_column *cl)
{
	if (!tb || !cl || !list_empty(&tb->tb_lines))
		return -EINVAL;

	if (cl->flags & SCOLS_FL_TREE)
		tb->ntreecols--;

	DBG(TAB, ul_debugobj(tb, "remove column %p", cl));
	list_del_init(&cl->cl_columns);
	tb->ncols--;
	cl->table = NULL;
	scols_unref_column(cl);
	return 0;
}

/**
 * scols_table_remove_columns:
 * @tb: a pointer to a struct libscols_table instance
 *
 * Removes all of @tb's columns.
 *
 * Returns: 0, a negative number in case of an error.
 */
int scols_table_remove_columns(struct libscols_table *tb)
{
	if (!tb || !list_empty(&tb->tb_lines))
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "remove all columns"));
	while (!list_empty(&tb->tb_columns)) {
		struct libscols_column *cl = list_entry(tb->tb_columns.next,
					struct libscols_column, cl_columns);
		scols_table_remove_column(tb, cl);
	}
	return 0;
}

/**
 * scols_table_new_column:
 * @tb: table
 * @name: column header
 * @whint: column width hint (absolute width: N > 1; relative width: N < 1)
 * @flags: flags integer
 *
 * This is shortcut for
 *
 *   cl = scols_new_column();
 *   scols_column_set_....(cl, ...);
 *   scols_table_add_column(tb, cl);
 *
 * The column width is possible to define by:
 *
 *  @whint = 0..1    : relative width, percent of terminal width
 *
 *  @whint = 1..N    : absolute width, empty column will be truncated to
 *                     the column header width if no specified STRICTWIDTH flag
 *
 * Note that if table has disabled "maxout" flag (disabled by default) than
 * relative width is used as a hint only. It's possible that column will be
 * narrow if the specified size is too large for column data.
 *
 * The column is necessary to address by sequential number. The first defined
 * column has the colnum = 0. For example:
 *
 *	scols_table_new_column(tab, "FOO", 0.5, 0);		// colnum = 0
 *	scols_table_new_column(tab, "BAR", 0.5, 0);		// colnum = 1
 *      .
 *      .
 *	scols_line_get_cell(line, 0);				// FOO column
 *	scols_line_get_cell(line, 1);				// BAR column
 *
 * Returns: newly allocated column
 */
struct libscols_column *scols_table_new_column(struct libscols_table *tb,
					       const char *name,
					       double whint,
					       int flags)
{
	struct libscols_column *cl;
	struct libscols_cell *hr;

	if (!tb)
		return NULL;

	DBG(TAB, ul_debugobj(tb, "new column name=%s, whint=%g, flags=%d",
				name, whint, flags));
	cl = scols_new_column();
	if (!cl)
		return NULL;

	/* set column name */
	hr = scols_column_get_header(cl);
	if (!hr)
		goto err;
	if (scols_cell_set_data(hr, name))
		goto err;

	scols_column_set_whint(cl, whint);
	scols_column_set_flags(cl, flags);

	if (scols_table_add_column(tb, cl))	/* this increments column ref-counter */
		goto err;

	scols_unref_column(cl);
	return cl;
err:
	scols_unref_column(cl);
	return NULL;
}

/**
 * scols_table_next_column:
 * @tb: a pointer to a struct libscols_table instance
 * @itr: a pointer to a struct libscols_iter instance
 * @cl: a pointer to a pointer to a struct libscols_column instance
 *
 * Returns the next column of @tb via @cl.
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_table_next_column(struct libscols_table *tb,
			    struct libscols_iter *itr,
			    struct libscols_column **cl)
{
	int rc = 1;

	if (!tb || !itr || !cl)
		return -EINVAL;
	*cl = NULL;

	if (!itr->head)
		SCOLS_ITER_INIT(itr, &tb->tb_columns);
	if (itr->p != itr->head) {
		SCOLS_ITER_ITERATE(itr, *cl, struct libscols_column, cl_columns);
		rc = 0;
	}

	return rc;
}

/**
 * scols_table_get_ncols:
 * @tb: table
 *
 * Returns: the ncols table member.
 */
size_t scols_table_get_ncols(const struct libscols_table *tb)
{
	return tb->ncols;
}

/**
 * scols_table_get_nlines:
 * @tb: table
 *
 * Returns: the nlines table member.
 */
size_t scols_table_get_nlines(const struct libscols_table *tb)
{
	return tb->nlines;
}

/**
 * scols_table_set_stream:
 * @tb: table
 * @stream: output stream
 *
 * Sets the output stream for table @tb.
 *
 * Returns: 0, a negative number in case of an error.
 */
int scols_table_set_stream(struct libscols_table *tb, FILE *stream)
{
	assert(tb);
	if (!tb)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "setting alternative stream"));
	tb->out = stream;
	return 0;
}

/**
 * scols_table_get_stream:
 * @tb: table
 *
 * Gets the output stream for table @tb.
 *
 * Returns: stream pointer, NULL in case of an error or an unset stream.
 */
FILE *scols_table_get_stream(const struct libscols_table *tb)
{
	return tb->out;
}

/**
 * scols_table_reduce_termwidth:
 * @tb: table
 * @reduce: width
 *
 * If necessary then libsmartcols use all terminal width, the @reduce setting
 * provides extra space (for example for borders in ncurses applications).
 *
 * The @reduce must be smaller than terminal width, otherwise it's silently
 * ignored. The reduction is not applied when STDOUT_FILENO is not terminal.
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_table_reduce_termwidth(struct libscols_table *tb, size_t reduce)
{
	if (!tb)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "reduce terminal width: %zu", reduce));
	tb->termreduce = reduce;
	return 0;
}

/**
 * scols_table_get_column:
 * @tb: table
 * @n: number of column (0..N)
 *
 * Returns: pointer to column or NULL
 */
struct libscols_column *scols_table_get_column(struct libscols_table *tb,
					       size_t n)
{
	struct libscols_iter itr;
	struct libscols_column *cl;

	if (!tb)
		return NULL;
	if (n >= tb->ncols)
		return NULL;

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_column(tb, &itr, &cl) == 0) {
		if (cl->seqnum == n)
			return cl;
	}
	return NULL;
}

/**
 * scols_table_add_line:
 * @tb: table
 * @ln: line
 *
 * Note that this function calls scols_line_alloc_cells() if number
 * of the cells in the line is too small for @tb.
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_table_add_line(struct libscols_table *tb, struct libscols_line *ln)
{
	if (!tb || !ln || tb->ncols == 0)
		return -EINVAL;

	if (tb->ncols > ln->ncells) {
		int rc = scols_line_alloc_cells(ln, tb->ncols);
		if (rc)
			return rc;
	}

	DBG(TAB, ul_debugobj(tb, "add line %p", ln));
	list_add_tail(&ln->ln_lines, &tb->tb_lines);
	ln->seqnum = tb->nlines++;
	scols_ref_line(ln);
	return 0;
}

/**
 * scols_table_remove_line:
 * @tb: table
 * @ln: line
 *
 * Note that this function does not destroy the parent<->child relationship between lines.
 * You have to call scols_line_remove_child()
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_table_remove_line(struct libscols_table *tb,
			    struct libscols_line *ln)
{
	if (!tb || !ln)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "remove line %p", ln));
	list_del_init(&ln->ln_lines);
	tb->nlines--;
	scols_unref_line(ln);
	return 0;
}

/**
 * scols_table_remove_lines:
 * @tb: table
 *
 * This empties the table and also destroys all the parent<->child relationships.
 */
void scols_table_remove_lines(struct libscols_table *tb)
{
	if (!tb)
		return;

	DBG(TAB, ul_debugobj(tb, "remove all lines"));
	while (!list_empty(&tb->tb_lines)) {
		struct libscols_line *ln = list_entry(tb->tb_lines.next,
						struct libscols_line, ln_lines);
		if (ln->parent)
			scols_line_remove_child(ln->parent, ln);
		scols_table_remove_line(tb, ln);
	}
}

/**
 * scols_table_next_line:
 * @tb: a pointer to a struct libscols_table instance
 * @itr: a pointer to a struct libscols_iter instance
 * @ln: a pointer to a pointer to a struct libscols_line instance
 *
 * Finds the next line and returns a pointer to it via @ln.
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_table_next_line(struct libscols_table *tb,
			  struct libscols_iter *itr,
			  struct libscols_line **ln)
{
	int rc = 1;

	if (!tb || !itr || !ln)
		return -EINVAL;
	*ln = NULL;

	if (!itr->head)
		SCOLS_ITER_INIT(itr, &tb->tb_lines);
	if (itr->p != itr->head) {
		SCOLS_ITER_ITERATE(itr, *ln, struct libscols_line, ln_lines);
		rc = 0;
	}

	return rc;
}

/**
 * scols_table_new_line:
 * @tb: table
 * @parent: parental line or NULL
 *
 * This is shortcut for
 *
 *   ln = scols_new_line();
 *   scols_table_add_line(tb, ln);
 *   scols_line_add_child(parent, ln);
 *
 *
 * Returns: newly allocate line
 */
struct libscols_line *scols_table_new_line(struct libscols_table *tb,
					   struct libscols_line *parent)
{
	struct libscols_line *ln;

	if (!tb || !tb->ncols)
		return NULL;

	ln = scols_new_line();
	if (!ln)
		return NULL;

	if (scols_table_add_line(tb, ln))
		goto err;
	if (parent)
		scols_line_add_child(parent, ln);

	scols_unref_line(ln);	/* ref-counter incremented by scols_table_add_line() */
	return ln;
err:
	scols_unref_line(ln);
	return NULL;
}

/**
 * scols_table_get_line:
 * @tb: table
 * @n: column number (0..N)
 *
 * Returns: a line or NULL
 */
struct libscols_line *scols_table_get_line(struct libscols_table *tb,
					   size_t n)
{
	struct libscols_iter itr;
	struct libscols_line *ln;

	if (!tb)
		return NULL;
	if (n >= tb->nlines)
		return NULL;

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_line(tb, &itr, &ln) == 0) {
		if (ln->seqnum == n)
			return ln;
	}
	return NULL;
}

/**
 * scols_copy_table:
 * @tb: table
 *
 * Creates a new independent table copy, except struct libscols_symbols that
 * are shared between the tables.
 *
 * Returns: a newly allocated copy of @tb
 */
struct libscols_table *scols_copy_table(struct libscols_table *tb)
{
	struct libscols_table *ret;
	struct libscols_line *ln;
	struct libscols_column *cl;
	struct libscols_iter itr;

	if (!tb)
		return NULL;
	ret = scols_new_table();
	if (!ret)
		return NULL;

	DBG(TAB, ul_debugobj(tb, "copy into %p", ret));

	if (tb->symbols)
		scols_table_set_symbols(ret, tb->symbols);

	/* columns */
	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_column(tb, &itr, &cl) == 0) {
		cl = scols_copy_column(cl);
		if (!cl)
			goto err;
		if (scols_table_add_column(ret, cl))
			goto err;
		scols_unref_column(cl);
	}

	/* lines */
	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_line(tb, &itr, &ln) == 0) {
		struct libscols_line *newln = scols_copy_line(ln);
		if (!newln)
			goto err;
		if (scols_table_add_line(ret, newln))
			goto err;
		if (ln->parent) {
			struct libscols_line *p =
				scols_table_get_line(ret, ln->parent->seqnum);
			if (p)
				scols_line_add_child(p, newln);
		}
		scols_unref_line(newln);
	}

	/* separators */
	if (scols_table_set_column_separator(ret, tb->colsep) ||
	    scols_table_set_line_separator(ret, tb->linesep))
		goto err;

	return ret;
err:
	scols_unref_table(ret);
	return NULL;
}

/**
 * scols_table_set_default_symbols:
 * @tb: table
 *
 * The library check the current environment to select ASCII or UTF8 symbols.
 * This default behavior could be controlled by scols_table_enable_ascii().
 *
 * Use scols_table_set_symbols() to unset symbols or use your own setting.
 *
 * Returns: 0, a negative value in case of an error.
 *
 * Since: 2.29
 */
int scols_table_set_default_symbols(struct libscols_table *tb)
{
	struct libscols_symbols *sy;
	int rc;

	if (!tb)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "setting default symbols"));

	sy = scols_new_symbols();
	if (!sy)
		return -ENOMEM;

#if defined(HAVE_WIDECHAR)
	if (!scols_table_is_ascii(tb) &&
	    !strcmp(nl_langinfo(CODESET), "UTF-8")) {
		scols_symbols_set_branch(sy, UTF_VR UTF_H);
		scols_symbols_set_vertical(sy, UTF_V " ");
		scols_symbols_set_right(sy, UTF_UR UTF_H);
	} else
#endif
	{
		scols_symbols_set_branch(sy, "|-");
		scols_symbols_set_vertical(sy, "| ");
		scols_symbols_set_right(sy, "`-");
	}
	scols_symbols_set_title_padding(sy, " ");
	scols_symbols_set_cell_padding(sy, " ");

	rc = scols_table_set_symbols(tb, sy);
	scols_unref_symbols(sy);
	return rc;
}


/**
 * scols_table_set_symbols:
 * @tb: table
 * @sy: symbols or NULL
 *
 * Add a reference to @sy from the table. The symbols are used by library to
 * draw tree output. If no symbols are used for the table then library creates
 * default temporary symbols to draw output by scols_table_set_default_symbols().
 *
 * If @sy is NULL then remove reference from the currenly uses symbols.
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_table_set_symbols(struct libscols_table *tb,
			    struct libscols_symbols *sy)
{
	if (!tb)
		return -EINVAL;

	/* remove old */
	if (tb->symbols) {
		DBG(TAB, ul_debugobj(tb, "remove symbols %p refrence", tb->symbols));
		scols_unref_symbols(tb->symbols);
		tb->symbols = NULL;
	}

	/* set new */
	if (sy) {					/* ref user defined */
		DBG(TAB, ul_debugobj(tb, "set symbols so %p", sy));
		tb->symbols = sy;
		scols_ref_symbols(sy);
	}
	return 0;
}

/**
 * scols_table_get_symbols:
 * @tb: table
 *
 * Returns: pointer to symbols table.
 *
 * Since: 2.29
 */
struct libscols_symbols *scols_table_get_symbols(const struct libscols_table *tb)
{
	return tb->symbols;
}

/**
 * scols_table_enable_nolinesep:
 * @tb: table
 * @enable: 1 or 0
 *
 * Enable/disable line separator printing. This is useful if you want to
 * re-printing the same line more than once (e.g. progress bar). Don't use it
 * if you're not sure.
 *
 * Returns: 0 on success, negative number in case of an error.
 */
int scols_table_enable_nolinesep(struct libscols_table *tb, int enable)
{
	if (!tb)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "nolinesep: %s", enable ? "ENABLE" : "DISABLE"));
	tb->no_linesep = enable ? 1 : 0;
	return 0;
}

/**
 * scols_table_is_nolinesep:
 * @tb: a pointer to a struct libscols_table instance
 *
 * Returns: 1 if line separator printing is disabled.
 *
 * Since: 2.29
 */
int scols_table_is_nolinesep(const struct libscols_table *tb)
{
	return tb->no_linesep;
}

/**
 * scols_table_enable_colors:
 * @tb: table
 * @enable: 1 or 0
 *
 * Enable/disable colors.
 *
 * Returns: 0 on success, negative number in case of an error.
 */
int scols_table_enable_colors(struct libscols_table *tb, int enable)
{
	if (!tb)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "colors: %s", enable ? "ENABLE" : "DISABLE"));
	tb->colors_wanted = enable;
	return 0;
}

/**
 * scols_table_enable_raw:
 * @tb: table
 * @enable: 1 or 0
 *
 * Enable/disable raw output format. The parsable output formats
 * (export, raw, JSON, ...) are mutually exclusive.
 *
 * Returns: 0 on success, negative number in case of an error.
 */
int scols_table_enable_raw(struct libscols_table *tb, int enable)
{
	if (!tb)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "raw: %s", enable ? "ENABLE" : "DISABLE"));
	if (enable)
		tb->format = SCOLS_FMT_RAW;
	else if (tb->format == SCOLS_FMT_RAW)
		tb->format = 0;
	return 0;
}

/**
 * scols_table_enable_json:
 * @tb: table
 * @enable: 1 or 0
 *
 * Enable/disable JSON output format. The parsable output formats
 * (export, raw, JSON, ...) are mutually exclusive.
 *
 * Returns: 0 on success, negative number in case of an error.
 *
 * Since: 2.27
 */
int scols_table_enable_json(struct libscols_table *tb, int enable)
{
	if (!tb)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "json: %s", enable ? "ENABLE" : "DISABLE"));
	if (enable)
		tb->format = SCOLS_FMT_JSON;
	else if (tb->format == SCOLS_FMT_JSON)
		tb->format = 0;
	return 0;
}

/**
 * scols_table_enable_export:
 * @tb: table
 * @enable: 1 or 0
 *
 * Enable/disable export output format (COLUMNAME="value" ...).
 * The parsable output formats (export and raw) are mutually exclusive.
 *
 * Returns: 0 on success, negative number in case of an error.
 */
int scols_table_enable_export(struct libscols_table *tb, int enable)
{
	if (!tb)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "export: %s", enable ? "ENABLE" : "DISABLE"));
	if (enable)
		tb->format = SCOLS_FMT_EXPORT;
	else if (tb->format == SCOLS_FMT_EXPORT)
		tb->format = 0;
	return 0;
}

/**
 * scols_table_enable_ascii:
 * @tb: table
 * @enable: 1 or 0
 *
 * The ASCII-only output is relevant for tree-like outputs. The library
 * checks if the current environment is UTF8 compatible by default. This
 * function overrides this check and force the library to use ASCII chars
 * for the tree.
 *
 * If a custom libcols_symbols are specified (see scols_table_set_symbols()
 * then ASCII flag setting is ignored.
 *
 * Returns: 0 on success, negative number in case of an error.
 */
int scols_table_enable_ascii(struct libscols_table *tb, int enable)
{
	if (!tb)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "ascii: %s", enable ? "ENABLE" : "DISABLE"));
	tb->ascii = enable ? 1 : 0;
	return 0;
}

/**
 * scols_table_enable_noheadings:
 * @tb: table
 * @enable: 1 or 0
 *
 * Enable/disable header line.
 *
 * Returns: 0 on success, negative number in case of an error.
 */
int scols_table_enable_noheadings(struct libscols_table *tb, int enable)
{
	if (!tb)
		return -EINVAL;
	DBG(TAB, ul_debugobj(tb, "noheading: %s", enable ? "ENABLE" : "DISABLE"));
	tb->no_headings = enable ? 1 : 0;
	return 0;
}

/**
 * scols_table_enable_maxout:
 * @tb: table
 * @enable: 1 or 0
 *
 * The extra space after last column is ignored by default. The output
 * maximization use the extra space for all columns.
 *
 * Returns: 0 on success, negative number in case of an error.
 */
int scols_table_enable_maxout(struct libscols_table *tb, int enable)
{
	if (!tb)
		return -EINVAL;
	DBG(TAB, ul_debugobj(tb, "maxout: %s", enable ? "ENABLE" : "DISABLE"));
	tb->maxout = enable ? 1 : 0;
	return 0;
}

/**
 * scols_table_enable_nowrap:
 * @tb: table
 * @enable: 1 or 0
 *
 * Never continue on next line, remove last column(s) when too large, truncate last column.
 *
 * Returns: 0 on success, negative number in case of an error.
 *
 * Since: 2.28
 */
int scols_table_enable_nowrap(struct libscols_table *tb, int enable)
{
	if (!tb)
		return -EINVAL;
	DBG(TAB, ul_debugobj(tb, "nowrap: %s", enable ? "ENABLE" : "DISABLE"));
	tb->no_wrap = enable ? 1 : 0;
	return 0;
}

/**
 * scols_table_is_nowrap:
 * @tb: a pointer to a struct libscols_table instance
 *
 * Returns: 1 if nowrap is enabled.
 *
 * Since: 2.29
 */
int scols_table_is_nowrap(const struct libscols_table *tb)
{
	return tb->no_wrap;
}

/**
 * scols_table_colors_wanted:
 * @tb: table
 *
 * Returns: 1 if colors are enabled.
 */
int scols_table_colors_wanted(const struct libscols_table *tb)
{
	return tb->colors_wanted;
}

/**
 * scols_table_is_empty:
 * @tb: table
 *
 * Returns: 1 if the table is empty.
 */
int scols_table_is_empty(const struct libscols_table *tb)
{
	return !tb->nlines;
}

/**
 * scols_table_is_ascii:
 * @tb: table
 *
 * Returns: 1 if ASCII tree is enabled.
 */
int scols_table_is_ascii(const struct libscols_table *tb)
{
	return tb->ascii;
}

/**
 * scols_table_is_noheadings:
 * @tb: table
 *
 * Returns: 1 if header output is disabled.
 */
int scols_table_is_noheadings(const struct libscols_table *tb)
{
	return tb->no_headings;
}

/**
 * scols_table_is_export:
 * @tb: table
 *
 * Returns: 1 if export output format is enabled.
 */
int scols_table_is_export(const struct libscols_table *tb)
{
	return tb->format == SCOLS_FMT_EXPORT;
}

/**
 * scols_table_is_raw:
 * @tb: table
 *
 * Returns: 1 if raw output format is enabled.
 */
int scols_table_is_raw(const struct libscols_table *tb)
{
	return tb->format == SCOLS_FMT_RAW;
}

/**
 * scols_table_is_json:
 * @tb: table
 *
 * Returns: 1 if JSON output format is enabled.
 *
 * Since: 2.27
 */
int scols_table_is_json(const struct libscols_table *tb)
{
	return tb->format == SCOLS_FMT_JSON;
}

/**
 * scols_table_is_maxout
 * @tb: table
 *
 * Returns: 1 if output maximization is enabled or 0
 */
int scols_table_is_maxout(const struct libscols_table *tb)
{
	return tb->maxout;
}

/**
 * scols_table_is_tree:
 * @tb: table
 *
 * Returns: returns 1 tree-like output is expected.
 */
int scols_table_is_tree(const struct libscols_table *tb)
{
	return tb->ntreecols > 0;
}

/**
 * scols_table_set_column_separator:
 * @tb: table
 * @sep: separator
 *
 * Sets the column separator of @tb to @sep.
 * Please note that @sep should always take up a single cell in the output.
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_table_set_column_separator(struct libscols_table *tb, const char *sep)
{
	return strdup_to_struct_member(tb, colsep, sep);
}

/**
 * scols_table_set_line_separator:
 * @tb: table
 * @sep: separator
 *
 * Sets the line separator of @tb to @sep.
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_table_set_line_separator(struct libscols_table *tb, const char *sep)
{
	return strdup_to_struct_member(tb, linesep, sep);
}

/**
 * scols_table_get_column_separator:
 * @tb: table
 *
 * Returns: @tb column separator, NULL in case of an error
 */
const char *scols_table_get_column_separator(const struct libscols_table *tb)
{
	return tb->colsep;
}

/**
 * scols_table_get_line_separator:
 * @tb: table
 *
 * Returns: @tb line separator, NULL in case of an error
 */
const char *scols_table_get_line_separator(const struct libscols_table *tb)
{
	return tb->linesep;
}
/* for lines in the struct libscols_line->ln_lines list */
static int cells_cmp_wrapper_lines(struct list_head *a, struct list_head *b, void *data)
{
	struct libscols_column *cl = (struct libscols_column *) data;
	struct libscols_line *ra, *rb;
	struct libscols_cell *ca, *cb;

	assert(a);
	assert(b);
	assert(cl);

	ra = list_entry(a, struct libscols_line, ln_lines);
	rb = list_entry(b, struct libscols_line, ln_lines);
	ca = scols_line_get_cell(ra, cl->seqnum);
	cb = scols_line_get_cell(rb, cl->seqnum);

	return cl->cmpfunc(ca, cb, cl->cmpfunc_data);
}

/* for lines in the struct libscols_line->ln_children list */
static int cells_cmp_wrapper_children(struct list_head *a, struct list_head *b, void *data)
{
	struct libscols_column *cl = (struct libscols_column *) data;
	struct libscols_line *ra, *rb;
	struct libscols_cell *ca, *cb;

	assert(a);
	assert(b);
	assert(cl);

	ra = list_entry(a, struct libscols_line, ln_children);
	rb = list_entry(b, struct libscols_line, ln_children);
	ca = scols_line_get_cell(ra, cl->seqnum);
	cb = scols_line_get_cell(rb, cl->seqnum);

	return cl->cmpfunc(ca, cb, cl->cmpfunc_data);
}


static int sort_line_children(struct libscols_line *ln, struct libscols_column *cl)
{
	struct list_head *p;

	if (list_empty(&ln->ln_branch))
		return 0;

	list_for_each(p, &ln->ln_branch) {
		struct libscols_line *chld =
				list_entry(p, struct libscols_line, ln_children);
		sort_line_children(chld, cl);
	}

	list_sort(&ln->ln_branch, cells_cmp_wrapper_children, cl);
	return 0;
}

/**
 * scols_sort_table:
 * @tb: table
 * @cl: order by this column
 *
 * Orders the table by the column. See also scols_column_set_cmpfunc().
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_sort_table(struct libscols_table *tb, struct libscols_column *cl)
{
	if (!tb || !cl || !cl->cmpfunc)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "sorting table"));
	list_sort(&tb->tb_lines, cells_cmp_wrapper_lines, cl);

	if (scols_table_is_tree(tb)) {
		struct libscols_line *ln;
		struct libscols_iter itr;

		scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
		while (scols_table_next_line(tb, &itr, &ln) == 0)
			sort_line_children(ln, cl);
	}

	return 0;
}

/**
 * scols_table_set_termforce:
 * @tb: table
 * @force: SCOLS_TERMFORCE_{NEVER,ALWAYS,AUTO}
 *
 * Forces library to use stdout as terminal, non-terminal or use automatic
 * detection (default).
 *
 * Returns: 0, a negative value in case of an error.
 *
 * Since: 2.29
 */
int scols_table_set_termforce(struct libscols_table *tb, int force)
{
	if (!tb)
		return -EINVAL;
	tb->termforce = force;
	return 0;
}

/**
 * scols_table_get_termforce:
 * @tb: table
 *
 * Returns: SCOLS_TERMFORCE_{NEVER,ALWAYS,AUTO} or a negative value in case of an error.
 *
 * Since: 2.29
 */
int scols_table_get_termforce(const struct libscols_table *tb)
{
	return tb->termforce;
}

/**
 * scols_table_set_termwidth
 * @tb: table
 * @width: terminal width
 *
 * The library automatically detects terminal width or defaults to 80 chars if
 * detections is unsuccessful. This function override this behaviour.
 *
 * Returns: 0, a negative value in case of an error.
 *
 * Since: 2.29
 */
int scols_table_set_termwidth(struct libscols_table *tb, size_t width)
{
	tb->termwidth = width;
	return 0;
}

/**
 * scols_table_get_termwidth
 * @tb: table
 *
 * Returns: terminal width or a negative value in case of an error.
 */
size_t scols_table_get_termwidth(const struct libscols_table *tb)
{
	return tb->termwidth;
}
