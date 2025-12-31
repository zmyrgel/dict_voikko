/*-------------------------------------------------------------------------
 *
 * dict_voikko.c
 *	  Text search dictionary for Finnish using Voikko
 *
 * Copyright (c) 2014, Houston Inc.
 *
 * Author: Petter Ljungqvist (petter.ljungqvist@houston-inc.com)
 *
 * IDENTIFICATION
 *	  contrib/dict_voikko/dict_voikko.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <string.h>
#include <sys/types.h>
#include <regex.h>

#include "catalog/pg_collation_d.h"
#include "commands/defrem.h"
#include "tsearch/ts_utils.h"
#include "utils/formatting.h"

#include <libvoikko/voikko.h>


PG_MODULE_MAGIC;

typedef struct {
	struct VoikkoHandle *voikko;
	regex_t *regex_stem;
	regex_t *regex_suff;
	StopList stoplist;
} DictVoikko;

PG_FUNCTION_INFO_V1(dvoikko_init);
Datum		dvoikko_init(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(dvoikko_lexize);
Datum		dvoikko_lexize(PG_FUNCTION_ARGS);

Datum dvoikko_init(PG_FUNCTION_ARGS)
{
	List		*dictoptions = (List *) PG_GETARG_POINTER(0);
	DictVoikko	*d = (DictVoikko *) palloc0(sizeof(DictVoikko));
	bool		stoploaded = false;
	ListCell	*l;
	const char	*voikko_error;

	foreach(l, dictoptions) {
		DefElem    *defel = (DefElem *) lfirst(l);

		if (pg_strcasecmp("StopWords", defel->defname) == 0) {
			if (stoploaded)
				ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("multiple StopWords parameters")));
			readstoplist(defGetString(defel), &d->stoplist, str_tolower);
			stoploaded = true;
		} else {
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("unrecognized voikko dictionary parameter: \"%s\"",
				defel->defname)));
		}
	}

	d->voikko = voikkoInit(&voikko_error, "fi-x-morpho", 0);
	if (!d->voikko) {
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("voikko error: \"%s\"", voikko_error)));
	}

	d->regex_stem = palloc0(sizeof(regex_t));
	if (regcomp(d->regex_stem, "\\(([a-zåäö]+)\\)", REG_ICASE|REG_EXTENDED))
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("could not compile regex")));

	d->regex_suff = palloc0(sizeof(regex_t));
	if (regcomp(d->regex_suff, "\\+(([a-zåäö])+)\\(([a-zåäö])+\\)\\+([a-zåäö])+\\(\\+(([a-zåäö])+)\\)", REG_ICASE|REG_EXTENDED))
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("could not compile regex")));

	PG_RETURN_POINTER(d);
}

static TSLexeme *
add_lexeme(TSLexeme *res, int *lex_n, const char *in_lexeme, const int num)
{
	char * lexeme = str_tolower(in_lexeme, num, DEFAULT_COLLATION_OID);
	int i;
	int n = (*lex_n) + 1;
	char * lexeme_ = palloc0(sizeof(char) * (num + 1));

	for (i = 0; i < *lex_n; ++i) {
		if ('\0' == res[i].lexeme[num + 1] && !memcmp(res[i].lexeme, lexeme, num)) {
			return res;
		}
	}
	res = repalloc(res, sizeof(TSLexeme) * (n + 1));
	res[n].lexeme = NULL;
	res[n].nvariant = 0;
	res[n].flags = 0;
	res[n - 1].lexeme = memcpy(lexeme_, lexeme, num);
	lexeme_[num] = '\0';
	*lex_n = n;
	pfree(lexeme);
	return res;
}

static char *
removeEqualSign(const char * in)
{
	char *out = palloc0(sizeof(char));
	int i_in = 0, i_out = 0;
	while (in[i_in] != '\0') {
		if ('=' != in[i_in]) {
			out[i_out] = in[i_in];
			++i_out;
			out = repalloc(out, sizeof(char) * (i_out + 1));
		}
		++i_in;
	}
	out[i_out] = '\0';
	return out;
}

Datum dvoikko_lexize(PG_FUNCTION_ARGS)
{
	TSLexeme	*res = NULL;
	DictVoikko	*d = (DictVoikko *) PG_GETARG_POINTER(0);
	char		*in = (char *) PG_GETARG_POINTER(1);
	int		lex_n = 0;
	char		*txt = str_tolower(in, PG_GETARG_INT32(2), DEFAULT_COLLATION_OID);
	char		*base, *p, *match;
	size_t		nmatch = 20;
	regmatch_t	*matchptr = palloc0(nmatch * sizeof(regmatch_t));
	regmatch_t	matchp;

	if (*txt == '\0' || searchstoplist(&(d->stoplist), txt)) {
		res = palloc0(sizeof(TSLexeme) * 2);
	} else {
		struct voikko_mor_analysis **analysis_arr = voikkoAnalyzeWordCstr(d->voikko, txt);
		if (analysis_arr) {
			struct voikko_mor_analysis *analysis = analysis_arr[0];
			if (analysis) {
				char *base_ = voikko_mor_analysis_value_cstr(analysis, "WORDBASES");
				if (base_) {
					res = palloc0(sizeof(TSLexeme));
					res[0].lexeme = NULL;
					base = removeEqualSign(base_);
					voikko_free_mor_analysis_value_cstr(base_);
					p = base;
					while (!regexec(d->regex_stem, p, nmatch, matchptr, 0)) {
						matchp = matchptr[1];
						res = add_lexeme(res, &lex_n, p + matchp.rm_so, matchp.rm_eo - matchp.rm_so);
						p += matchptr[0].rm_eo;
					}

					p = base;
					while (!regexec(d->regex_suff, p, nmatch, matchptr, 0)) {
						regmatch_t match_b, match_s;
						int len_b, len_s;
						if (d->regex_suff->re_nsub < 5) {
							ereport(ERROR, (errmsg("regex_suff does not provide expected capture groups")));
						}

						match_b = matchptr[1];
						match_s = matchptr[5];
						len_b = match_b.rm_eo - match_b.rm_so;
						len_s = match_s.rm_eo - match_s.rm_so;
						match = palloc0(len_b + len_s + 1);
						match[len_b + len_s] = '\0';
						memcpy(match, p + match_b.rm_so, len_b);
						memcpy(match + len_b, p + match_s.rm_so, len_s);
						res = add_lexeme(res, &lex_n, match, len_b + len_s);
						pfree(match);
						p += matchptr[0].rm_eo;
					}

					pfree(base);
				}
			}

			voikko_free_mor_analysis(analysis_arr);
		}
	}

	pfree(matchptr);
	pfree(txt);
	PG_RETURN_POINTER(res);
}
