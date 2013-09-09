/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"
#include "fileops.h"
#include "hash.h"
#include "filter.h"
#include "repository.h"
#include "git2/config.h"
#include "blob.h"
#include "attr_file.h"

struct git_filter_source {
	git_repository *repo;
	const char     *path;
	git_oid         oid;  /* zero if unknown (which is likely) */
	uint16_t        filemode; /* zero if unknown */
};

typedef struct {
	git_filter *filter;
	void *payload;
} git_filter_entry;

struct git_filter_list {
	git_array_t(git_filter_entry) filters;
	git_filter_mode_t mode;
	git_filter_source source;
	char path[GIT_FLEX_ARRAY];
};

typedef struct {
	const char *filter_name;
	git_filter *filter;
	int priority;
	size_t nattrs, nmatches;
	char *attrdata;
	const char *attrs[GIT_FLEX_ARRAY];
} git_filter_def;

static int filter_def_priority_cmp(const void *a, const void *b)
{
	int pa = ((const git_filter_def *)a)->priority;
	int pb = ((const git_filter_def *)b)->priority;
	return (pa < pb) ? -1 : (pa > pb) ? 1 : 0;
}

static git_vector git__filter_registry = {
	0, filter_def_priority_cmp, NULL, 0, 0
};

static int filter_def_scan_attrs(
	git_buf *attrs, size_t *nattr, size_t *nmatch, const char *attr_str)
{
	const char *start, *scan = attr_str;
	int has_eq;

	*nattr = *nmatch = 0;

	if (!scan)
		return 0;

	while (*scan) {
		while (git__isspace(*scan)) scan++;

		for (start = scan, has_eq = 0; *scan && !git__isspace(*scan); ++scan) {
			if (*scan == '=')
				has_eq = 1;
		}

		if (scan > start) {
			(*nattr)++;
			if (has_eq || *scan == '-' || *scan == '+' || *scan == '!')
				(*nmatch)++;

			if (has_eq)
				git_buf_putc(attrs, '=');
			git_buf_put(attrs, start, scan - start);
			git_buf_putc(attrs, '\0');
		}
	}

	return 0;
}

static void filter_def_set_attrs(git_filter_def *fdef)
{
	char *scan = fdef->attrdata;
	size_t i;

	for (i = 0; i < fdef->nattrs; ++i) {
		const char *name, *value;

		switch (*scan) {
		case '=':
			name = scan + 1;
			for (scan++; *scan != '='; scan++) /* find '=' */;
			*scan++ = '\0';
			value = scan;
			break;
		case '-':
			name = scan + 1; value = git_attr__false; break;
		case '+':
			name = scan + 1; value = git_attr__true;  break;
		case '!':
			name = scan + 1; value = git_attr__unset; break;
		default:
			name = scan;     value = NULL; break;
		}

		fdef->attrs[i] = name;
		fdef->attrs[i + fdef->nattrs] = value;

		scan += strlen(scan) + 1;
	}
}

int git_filter_register(
	const char *name, git_filter *filter, int priority)
{
	git_filter_def *fdef;
	size_t nattr = 0, nmatch = 0;
	git_buf attrs = GIT_BUF_INIT;

	if (git_filter_lookup(name) != NULL) {
		giterr_set(
			GITERR_FILTER, "Attempt to reregister existing filter '%s'", name);
		return -1;
	}

	if (filter_def_scan_attrs(&attrs, &nattr, &nmatch, filter->attributes) < 0)
		return -1;

	fdef = git__calloc(
		sizeof(git_filter_def) + 2 * nattr * sizeof(char *), 1);
	GITERR_CHECK_ALLOC(fdef);

	fdef->filter_name = name;
	fdef->filter      = filter;
	fdef->priority    = priority;
	fdef->nattrs      = nattr;
	fdef->nmatches    = nmatch;
	fdef->attrdata    = git_buf_detach(&attrs);

	filter_def_set_attrs(fdef);

	if (git_vector_insert(&git__filter_registry, fdef) < 0) {
		git__free(fdef->attrdata);
		git__free(fdef);
		return -1;
	}

	git_vector_sort(&git__filter_registry);
	return 0;
}

static int filter_def_name_key_check(const void *key, const void *fdef)
{
	const char *name =
		fdef ? ((const git_filter_def *)fdef)->filter_name : NULL;
	return name ? -1 : git__strcmp(key, name);
}

static git_filter_def *filter_find_by_name(size_t *pos, const char *name)
{
	git_filter_def *fdef = NULL;

	if (!git_vector_search2(
			pos, &git__filter_registry, filter_def_name_key_check, name))
		fdef = git_vector_get(&git__filter_registry, *pos);

	return fdef;
}

int git_filter_unregister(const char *name)
{
	size_t pos;
	git_filter_def *fdef;

	/* cannot unregister default filters */
	if (!strcmp(GIT_FILTER_CRLF, name)) {
		giterr_set(GITERR_FILTER, "Cannot unregister filter '%s'", name);
		return -1;
	}

	if ((fdef = filter_find_by_name(&pos, name)) == NULL) {
		giterr_set(GITERR_FILTER, "Cannot find filter '%s' to unregister", name);
		return GIT_ENOTFOUND;
	}

	(void)git_vector_remove(&git__filter_registry, pos);

	if (fdef->filter->shutdown)
		fdef->filter->shutdown(fdef->filter);

	git__free(fdef->attrdata);
	git__free(fdef);

	return 0;
}

git_filter *git_filter_lookup(const char *name)
{
	size_t pos;
	git_filter_def *fdef = filter_find_by_name(&pos, name);
	return fdef ? fdef->filter : NULL;
}

static int filter_load_defaults(void)
{
	if (!git_vector_length(&git__filter_registry))
		return git_filter_register(GIT_FILTER_CRLF, git_crlf_filter_new(), 0);

	return 0;
}

git_repository *git_filter_source_repo(const git_filter_source *src)
{
	return src->repo;
}

const char *git_filter_source_path(const git_filter_source *src)
{
	return src->path;
}

uint16_t git_filter_source_filemode(const git_filter_source *src)
{
	return src->filemode;
}

const git_oid *git_filter_source_id(const git_filter_source *src)
{
	return git_oid_iszero(&src->oid) ? NULL : &src->oid;
}

static int git_filter_list_new(
	git_filter_list **out, git_filter_mode_t mode, const git_filter_source *src)
{
	git_filter_list *fl = NULL;
	size_t pathlen = src->path ? strlen(src->path) : 0;

	fl = git__calloc(1, sizeof(git_filter_list) + pathlen + 1);
	GITERR_CHECK_ALLOC(fl);

	fl->mode = mode;
	if (src->path)
		memcpy(fl->path, src->path, pathlen);
	fl->source.repo = src->repo;
	fl->source.path = fl->path;

	*out = fl;
	return 0;
}

static int filter_list_check_attributes(
	const char ***out, git_filter_def *fdef, const git_filter_source *src)
{
	int error;
	size_t i;
	const char **strs = git__calloc(fdef->nattrs, sizeof(const char *));
	GITERR_CHECK_ALLOC(strs);

	error = git_attr_get_many(
		strs, src->repo, 0, src->path, fdef->nattrs, fdef->attrs);

	/* if no values were found but no matches are needed, it's okay! */
	if (error == GIT_ENOTFOUND && !fdef->nmatches) {
		giterr_clear();
		git__free(strs);
		return 0;
	}

	for (i = 0; !error && i < fdef->nattrs; ++i) {
		const char *want = fdef->attrs[fdef->nattrs + i];
		git_attr_t want_type, found_type;

		if (!want)
			continue;

		want_type  = git_attr_value(want);
		found_type = git_attr_value(strs[i]);

		if (want_type != found_type ||
			(want_type == GIT_ATTR_VALUE_T && strcmp(want, strs[i])))
			error = GIT_ENOTFOUND;
	}

	if (error)
		git__free(strs);
	else
		*out = strs;

	return error;
}

int git_filter_list_load(
	git_filter_list **filters,
	git_repository *repo,
	const char *path,
	git_filter_mode_t mode)
{
	int error = 0;
	git_filter_list *fl = NULL;
	git_filter_source src = { 0 };
	git_filter_entry *fe;
	size_t idx;
	git_filter_def *fdef;

	if (filter_load_defaults() < 0)
		return -1;

	src.repo = repo;
	src.path = path;

	git_vector_foreach(&git__filter_registry, idx, fdef) {
		const char **values = NULL;
		void *payload = NULL;

		if (!fdef || !fdef->filter)
			continue;

		if (fdef->nattrs > 0) {
			error = filter_list_check_attributes(&values, fdef, &src);
			if (error == GIT_ENOTFOUND) {
				error = 0;
				continue;
			} else if (error < 0)
				break;
		}

		if (fdef->filter->check)
			error = fdef->filter->check(
				fdef->filter, &payload, mode, &src, values);

		git__free(values);

		if (error == GIT_ENOTFOUND)
			error = 0;
		else if (error < 0)
			break;
		else {
			if (!fl && (error = git_filter_list_new(&fl, mode, &src)) < 0)
				return error;

			fe = git_array_alloc(fl->filters);
			GITERR_CHECK_ALLOC(fe);
			fe->filter  = fdef->filter;
			fe->payload = payload;
		}
	}

	if (error && fl != NULL) {
		git_array_clear(fl->filters);
		git__free(fl);
		fl = NULL;
	}

	*filters = fl;
	return error;
}

void git_filter_list_free(git_filter_list *fl)
{
	uint32_t i;

	if (!fl)
		return;

	for (i = 0; i < git_array_size(fl->filters); ++i) {
		git_filter_entry *fe = git_array_get(fl->filters, i);
		if (fe->filter->cleanup)
			fe->filter->cleanup(fe->filter, fe->payload);
	}

	git_array_clear(fl->filters);
	git__free(fl);
}

int git_filter_list_apply(
	git_buf *dest,
	git_buf *source,
	git_filter_list *fl)
{
	int error = 0;
	uint32_t i;
	unsigned int src;
	git_buf *dbuffer[2];
	git_filter_entry *fe;

	if (!fl) {
		git_buf_swap(dest, source);
		return 0;
	}

	dbuffer[0] = source;
	dbuffer[1] = dest;

	src = 0;

	/* Pre-grow the destination buffer to more or less the size
	 * we expect it to have */
	if (git_buf_grow(dest, git_buf_len(source)) < 0)
		return -1;

	for (i = 0; i < git_array_size(fl->filters); ++i) {
		unsigned int dst = 1 - src;

		git_buf_clear(dbuffer[dst]);

		fe = git_array_get(
			fl->filters, (fl->mode == GIT_FILTER_TO_ODB) ?
			i : git_array_size(fl->filters) - 1 - i);

		/* Apply the filter from dbuffer[src] to the other buffer;
		 * if the filtering is canceled by the user mid-filter,
		 * we skip to the next filter without changing the source
		 * of the double buffering (so that the text goes through
		 * cleanly).
		 */
		{
			git_buffer srcb = GIT_BUFFER_FROM_BUF(dbuffer[src]);
			git_buffer dstb = GIT_BUFFER_FROM_BUF(dbuffer[dst]);

			error = fe->filter->apply(
				fe->filter, &fe->payload, fl->mode, &dstb, &srcb, &fl->source);

			if (error == GIT_ENOTFOUND)
				error = 0;
			else if (error < 0) {
				git_buf_clear(dest);
				return error;
			}
			else {
				git_buf_from_buffer(dbuffer[src], &srcb);
				git_buf_from_buffer(dbuffer[dst], &dstb);
				src = dst;
			}
		}

		if (git_buf_oom(dbuffer[dst]))
			return -1;
	}

	/* Ensure that the output ends up in dbuffer[1] (i.e. the dest) */
	if (src != 1)
		git_buf_swap(dest, source);

	return 0;
}
