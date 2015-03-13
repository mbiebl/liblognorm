/* simple log structure analyzer (slsa)
 *
 * This is a heuristic to mine log structure.
 *
 * Copyright 2015 Rainer Gerhards
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* Learnings (mostly from things that failed)
 * ------------------------------------------
 * - if we detect IP (and similar things) too early in the process, we
 *   get wrong detections (e.g. for reverse DNS entries)
 * - if we detect IP adresses during value collapsing, we get unacceptable
 *   runtime and memory requirements, as a huge number of different actual
 *   values needs to be stored.
 * ->current solution is to detect them after tokenization but before
 *   adding the the structure tree.
 *
 * - if we split some known syntaxes (like cisco ip/port) into it's parts,
 *   they may get improperly detected in regard to subwords. This especially
 *   happens if there are few servers (or few destination ports), where parts
 *   of the IP (or the port) is considered to be a common prefix, which then
 *   may no longer be properly detected. As it looks, it pays to really have a
 *   much richer set of special parsers, and let the common pre/suffix
 *   detection only be used in rare cases where we do not have a parser.
 *   Invocation may even be treated as the need to check for a new parser.
 *
 *   Along the same lines, common "command words" (like "udp", "tcp") may
 *   also be detected by a specific parser, because otherwise they tend to have
 *   common pre/suffixes (e.g. {"ud","tc"},"p"). This could be done via a
 *   dictionary lookup (bsearch, later to become a single state machine?).
 *
 *   Along these lines, we probably need parses who return *multiple* values,
 *   e.g. the IP address and port. This requires that the field name has a
 *   common prefix. A solution is to return JSON where the node element is
 *   named like the field name, and the members have parser-specific names.
 */

#include "config.h"
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <getopt.h>

#include "liblognorm.h"
#include "internal.h"
#include "parser.h"
#include "syntaxes.h"

#define MAXLINE 32*1024

struct wordflags {
	unsigned
		isSubword : 1,
		isSpecial : 1;	/* indicates this is a special parser */
};

struct wordinfo {
	char *word;
	int occurs;
	struct wordflags flags;
};
struct logrec_node {
	struct logrec_node *parent;
	struct logrec_node *sibling;
	struct logrec_node *child; /* NULL: end of record */
	int nterm; /* number of times this was the terminal node */
	int8_t ntype;
	union {
		char *ltext; /* the literal text */
		int64_t number; /* all integer types */
	} val;
	/* the addtl value structure is dumb, and needs to
	 * be replaced with something much better when the initial
	 * experiments work out. But good enough for now.
	 * (bsearch table?)
	 */
	int nwords;	/* size of table */
	struct wordinfo **words;
};
typedef struct logrec_node logrec_node_t;

logrec_node_t *root = NULL;

/* forward definitions */
void wordDetectSyntax(struct wordinfo *const __restrict__ wi, const size_t wordlen, const int);
void treePrint(logrec_node_t *node, const int level);

/* param word may be NULL, if word is not yet known */
static struct wordinfo *
wordinfoNew(char *const word)
{
	struct wordinfo *const wi = calloc(1, sizeof(struct wordinfo));
	if(wi == NULL) {
		perror("slsa: malloc error struct wordinfo");
		exit(1);
	}
	wi->word = word;
	wi->occurs = 1;
	return wi;
}

static void
wordinfoDelete(struct wordinfo *const wi)
{
	free(wi->word);
	free(wi);
}

/* command line options */
static int displayProgress = 0; /* display progress indicators */

/* a stack to keep track of detected (sub)words that
 * need to be processed in the future.
 */
#define SIZE_WORDSTACK 8
static struct wordinfo* wordStack[SIZE_WORDSTACK];
static int wordstackPtr = -1; /* empty */

static void
wordstackPush(struct wordinfo *const wi)
{
	++wordstackPtr;
	if(wordstackPtr >= SIZE_WORDSTACK) {
		fprintf(stderr, "slsa: wordstack too small\n");
		exit(1);
	}
	wordStack[wordstackPtr] = wi;
}

/* returns wordinfo or NULL, if stack is empty */
static struct wordinfo *
wordstackPop(void)
{
	return (wordstackPtr < 0) ? NULL : wordStack[wordstackPtr--];
}

static void
reportProgress(const char *const label)
{
	static unsigned cnt = 0;
	static const char *lastlabel = NULL;
	if(!displayProgress)
		return;
	if(lastlabel == NULL)
		lastlabel = strdup(label);
	if(label == NULL || strcmp(label, lastlabel)) {
		fprintf(stderr, "\r%s: %u - done\n", lastlabel, cnt);
		cnt = 0;
		free((void*)lastlabel);
		lastlabel = (label == NULL) ? NULL : strdup(label);
	} else {
		if(++cnt % 100 == 0)
			fprintf(stderr, "\r%s: %u", label, cnt);
	}
}

static int
qs_compmi(const void *v1, const void *v2)
{
	const struct wordinfo *const w1 =  *((struct wordinfo**) v1);
	const struct wordinfo *const w2 =  *((struct wordinfo**) v2);
	return strcmp(w1->word, w2->word);
}
#if 0 /* we don't need this at the moment, but want to preserve it
       * in case we can need it again.
       */
static int
bs_compmi(const void *key, const void *mem)
{
	const struct wordinfo *const wi =  *((struct wordinfo**) mem);
	return strcmp((char*)key, wi->word);
}
#endif

/* add an additional word to existing node */
void
logrec_addWord(logrec_node_t *const __restrict__ node,
	struct wordinfo *const __restrict__ wi)
{
	/* TODO: here we can insert at the right spot, which makes
	 * bsearch() usable [in the caller, later...]
	 */
	/* TODO: alloc in larger increments */
	int newnwords = node->nwords + 1;
	node->words = realloc(node->words, sizeof(struct wordinfo *)*newnwords);
	wi->occurs = 1; /* TODO: layer violation, do in other function */
	node->words[node->nwords] = wi;
	node->nwords = newnwords;
#if 0
	if(node->nwords > 1) {
		qsort(node->words, node->nwords, sizeof(struct wordinfo), qs_compmi); // TODO upgrade
	}
#endif
}

logrec_node_t *
logrec_newNode(struct wordinfo *const wi, struct logrec_node *const parent)
{
	logrec_node_t *const node = calloc(1, sizeof(logrec_node_t));
	node->parent = parent;
	node->child = NULL;
	node->val.ltext = NULL;
	logrec_addWord(node, wi);
	return node;
}

void
logrec_delNode(logrec_node_t *const node)
{
	for(int i = 0 ; i < node->nwords ; ++i)
		wordinfoDelete(node->words[i]);
	free(node->words);
	free(node->val.ltext);
	free(node);
}


/* returns ptr to existing struct wordinfo or NULL, if not found.
 */
static struct wordinfo *
logrec_hasWord(logrec_node_t *const __restrict__ node,
	char *const __restrict__ word)
{
//	return (struct wordinfo*) bsearch(word, node->words, node->nwords, sizeof(struct wordinfo), bs_compmi);
	/* alternative without need to have a binary array -- may make sense... */
	int i;
	for(i = 0 ; i < node->nwords ; ++i) {
		if(!strcmp(node->words[i]->word, word))
			break;
	}
	return (i < node->nwords) ? node->words[i] : NULL;
}


void
printPrefixes(logrec_node_t *const __restrict__ node,
	const int lenPrefix, const int lenSuffix)
{
	int i;
	const int maxwords = node->nwords > 5 ? 5 : node->nwords;
	printf("prefix %d, suffix %d\n", lenPrefix, lenSuffix);
	for(i = 0 ; i < maxwords ; ++i) {
		const char *const word = node->words[i]->word;
		const int lenWord = strlen(word);
		const int strtSuffix = lenWord - lenSuffix;
		int j;
		putchar('"');
		for(j = 0 ; j < lenPrefix ; ++j)
			putchar(word[j]);
		printf("\" \"");
		for( ; j < strtSuffix ; ++j)
			putchar(word[j]);
		printf("\" \"");
		for( ; j < lenWord ; ++j)
			putchar(word[j]);
		printf("\"\n");
	}
}

/* Squash duplicate values inside a tree node. This must only be run
 * after tree node values have been modified.
 */
static void
squashDuplicateValues(logrec_node_t *node)
{
	if(node->nwords == 1)
		return;

	/* sort and the remove the easy to find dupes */
	qsort(node->words, node->nwords, sizeof(struct wordinfo*), qs_compmi);
	int iPrev = 0;
	int nsquashed = 0;
	for(int iNext = 1 ; iNext < node->nwords ; ++iNext) {
		if(!strcmp(node->words[iPrev]->word, node->words[iNext]->word)) {
			wordinfoDelete(node->words[iNext]);
			++nsquashed;
		} else { /* OK, new word */
			if(iPrev+1 == iNext)
				iPrev = iNext;
			else {
				node->words[iPrev]->occurs += iNext - iPrev - 1;
				++iPrev;
				node->words[iPrev] = node->words[iNext];
			}
		}
	}
	if(nsquashed) {
		node->nwords -= nsquashed;
		node->words = realloc(node->words,
				sizeof(struct wordinfo *)*node->nwords);
	}
}

/* Disjoin common prefixes and suffixes. This also triggers a
 * new syntax detection on the remaining variable part.
 */
static void
disjoinCommon(logrec_node_t *node,
	const size_t lenPrefix,
	const size_t lenSuffix)
{
	logrec_node_t *newnode;
	struct wordinfo *newwi;
	char *newword;
	char *word = node->words[0]->word;
	if(lenPrefix > 0) {
		/* we need to update our node in-place, because otherwise
		 * we change the structure of the tree with a couple of
		 * side effects. As we do not want this, the prefix must
		 * be placed into the current node, and new nodes be
		 * created for the other words.
		 */
		newword = malloc(lenPrefix+1);
		memcpy(newword, word, lenPrefix);
		newword[lenPrefix] = '\0';
		newwi = wordinfoNew(newword);
		newwi->flags.isSubword = 1;

		newnode = logrec_newNode(newwi, node);
		newnode->words = node->words;
		newnode->nwords = node->nwords;
		node->nwords = 0;
		node->words = NULL;
		logrec_addWord(node, newwi);
		newnode->child = node->child;
		node->child = newnode;
		node->parent = newnode;

		/* switch node */
		node = newnode;

		for(int i = 0 ; i < node->nwords ; ++i)
			memmove(node->words[i]->word, /* move includes \0 */
				node->words[i]->word+lenPrefix,
				strlen(node->words[i]->word)-lenPrefix+1);
	}
	if(lenSuffix > 0) {
		const size_t lenword = strlen(word);
		size_t iSuffix = lenword-lenSuffix;
		newword = malloc(lenSuffix+1);
		memcpy(newword, word+iSuffix, lenSuffix+1); /* includes \0 */
		//newword[lenSuffix] = '\0';
		newwi = wordinfoNew(newword);
		newwi->flags.isSubword = 1;

		newnode = logrec_newNode(newwi, node);
		newnode->child = node->child;
		newnode->child->parent = newnode;
		node->child = newnode;

		for(int i = 0 ; i < node->nwords ; ++i) {
			iSuffix = strlen(node->words[i]->word)-lenSuffix;
			node->words[i]->word[iSuffix] = '\0';
		}
	}

	for(int i = 0 ; i < node->nwords ; ++i) {
		node->words[i]->flags.isSubword = 1;
		wordDetectSyntax(node->words[i], strlen(node->words[i]->word), 0);
	}
	// TODO: squash duplicates only if syntaxes were detected!
	squashDuplicateValues(node);
}

/* find a matching terminator inside a suffix, searchs only
 * within the suffix area. If found, lenPrefix and lenSuffix
 * are update and 1 is returned. Returns 0 if not found.
 * Helper to checkPrefixes.
 */
static int
findMatchingTerm(const char *const __restrict__ word,
	const size_t lenWord,
	size_t potentialNewPrefix,
	int *const __restrict lenPrefix,
	int *const __restrict lenSuffix,
	const char term)
{
	int newSuffix = -1;
	for(int i = 0 ; i < *lenSuffix ; ++i)
		if(word[lenWord-i-1] == term) {
			newSuffix = i+1;
			break;
		}
	if(newSuffix >= 0) {
		*lenSuffix = newSuffix;
		*lenPrefix = potentialNewPrefix;
		return 1;
	}
	return 0;
}

/* check if there are common prefixes and suffixes and, if so,
 * extract them.
 * TODO: fix cases like {"end","eend"} which will lead to prefix=1, suffix=3
 * and which are obviously wrong. This happens with the current algo whenever
 * we have common chars at the edge of prefix and suffix.
 */
void
checkPrefixes(logrec_node_t *const __restrict__ node)
{
	if(node->nwords == 1 || node->words[0]->flags.isSubword)
		return;

	int i;
	const char *const baseword = node->words[0]->word;
	const size_t lenBaseword = strlen(baseword);
	int lenPrefix = lenBaseword;
	int lenSuffix = lenBaseword;
	for(i = 1 ; i < node->nwords ; ++i) {
		int j;
		/* check prefix */
		if(lenPrefix > 0) {
			for(j = 0 ;
			    j < lenPrefix && node->words[i]->word[j] == baseword[j] ;
			    ++j)
				; /* EMPTY - just scan */
			if(j < lenPrefix)
				lenPrefix = j;
		}
		/* check suffix */
		if(lenSuffix > 0) {
			const int lenWord = strlen(node->words[i]->word);
			const int jmax = (lenWord < lenSuffix) ? lenWord : lenSuffix;
			for(j = 0 ;
			    j < jmax &&
			      node->words[i]->word[lenWord-j-1] == baseword[lenBaseword-j-1] ;
			    ++j)
				; /* EMPTY - just scan */
			if(j < lenSuffix)
				lenSuffix = j;
		}
	}
	/* to avoid false positives, we check for some common
	 * field="xxx" syntaxes here.
	 */
	for(int j = lenPrefix-1 ; j >= 0 ; --j) {
		switch(baseword[j]) {
		case '"':
			if(findMatchingTerm(baseword, lenBaseword, j+1,
						&lenPrefix, &lenSuffix,'"'))
				goto done_prefixes;
			break;
		case '\'':
			if(findMatchingTerm(baseword, lenBaseword, j+1,
						&lenPrefix, &lenSuffix,'\''))
				goto done_prefixes;
			break;
		case '[':
			if(findMatchingTerm(baseword, lenBaseword, j+1,
						&lenPrefix, &lenSuffix,']'))
				goto done_prefixes;
			break;
		case '(':
			if(findMatchingTerm(baseword, lenBaseword, j+1,
						&lenPrefix, &lenSuffix,')'))
				goto done_prefixes;
			break;
		case '<':
			if(findMatchingTerm(baseword, lenBaseword, j+1,
						&lenPrefix, &lenSuffix,'>'))
				goto done_prefixes;
			break;
		case '=':
		case ':':
			lenPrefix = j+1;
			break;
		default:
			break;
		}
	}
done_prefixes:
	if(lenPrefix != 0 || lenSuffix != 0) {
		/* TODO: not only print here, but let user override
		 * (in upcoming "interactive" mode)
		 */
		printPrefixes(node, lenPrefix, lenSuffix);
		disjoinCommon(node, lenPrefix, lenSuffix);
	}
}

/* squash a tree, that is combine nodes that point to nodes
 * without siblings to a single node.
 */
void
treeSquash(logrec_node_t *node)
{
	if(node == NULL) return;
	reportProgress("squashing");
	const int hasSibling = node->sibling == NULL ? 0 : 1;
//printf("new iter, node %s\n", node->val.ltext);
	while(node != NULL) {
		if(!hasSibling && node->child != NULL && node->nwords == 0
		   && node->child->sibling == NULL && node->child->nwords == 0
		   && node->words[0]->word[0] != '%' /* do not combine syntaxes */
		   && node->child->words[0]->word[0] != '%') {
			char *newword;
			if(asprintf(&newword, "%s %s", node->words[0]->word,
				node->child->words[0]->word)) {}; /* silence cc warning */
			printf("squashing: %s\n", newword);
			free(node->words[0]->word);
			node->words[0]->word = newword;
			node->nterm = node->child->nterm; /* TODO: do not combine terminals! */
			logrec_node_t *toDel = node->child;
			node->child = node->child->child;
			logrec_delNode(toDel);
			continue; /* see if we can squash more */
		}
		checkPrefixes(node);
		treeSquash(node->child);
//printf("moving to next node %p -> %p\n", node, node->sibling);
		node = node->sibling;
	}
}


void
treePrintIndent(const int level, const char indicator)
{
	printf("%2d%c:", level, indicator);
	for(int i = 0 ; i < level ; ++i)
		printf("   ");
}

void
treePrintWordinfo(struct wordinfo *const __restrict__ wi)
{
	printf("%s", wi->word);
	if(wi->flags.isSubword)
		printf(" {subword}");
	if(wi->occurs > 1)
		printf(" {%d}", wi->occurs);
}

void
treePrint(logrec_node_t *node, const int level)
{
	reportProgress("print");
	while(node != NULL) {
		treePrintIndent(level, 'l');
		treePrintWordinfo(node->words[0]);
		if(node->nterm)
			printf(" [nterm %d]", node->nterm);
		printf("\n");
		for(int i = 1 ; i < node->nwords ; ++i) {
			treePrintIndent(level, 'v');
			treePrintWordinfo(node->words[i]);
			printf("\n");
		}
		treePrint(node->child, level + 1);
		node = node->sibling;
	}
}

/* TODO: move wordlen to struct wordinfo? */
void
/* NOTE: bDetectStacked is just a development aid, it permits us to write
 * a first version which does not detect multi-node items that would
 * go to the stack and require more elaborate handling. TODO: remove that
 * restriction.
 */
wordDetectSyntax(struct wordinfo *const __restrict__ wi, const size_t wordlen,
	const int bDetectStacked)
{
	size_t nproc;
	size_t constzero = 0; /* the default lognorm parsers need this */
	if(syntax_posint(wi->word, wordlen, NULL, &nproc) &&
	   nproc == wordlen) {
		free(wi->word);
		wi->word = strdup("%posint%");
		wi->flags.isSpecial = 1;
		goto done;
	}
	if(ln_parseTime24hr(wi->word, wordlen, &constzero, NULL, &nproc, NULL) == 0 &&
	   nproc == wordlen) {
		free(wi->word);
		wi->word = strdup("%time-24hr%");
		wi->flags.isSpecial = 1;
		goto done;
	}
	/* duration needs to go after Time24hr, as duration would accept
	 * Time24hr format, whereas duration usually starts with a single
	 * digit and so Tim24hr will not pick it. Still we may get false
	 * detection for durations > 10hrs, but so is it...
	 */
	if(ln_parseDuration(wi->word, wordlen, &constzero, NULL, &nproc, NULL) == 0 &&
	   nproc == wordlen) {
		free(wi->word);
		wi->word = strdup("%duration%");
		wi->flags.isSpecial = 1;
		goto done;
	}
	if(syntax_ipv4(wi->word, wordlen, NULL, &nproc)) {
		if(nproc == wordlen) {
			free(wi->word);
			wi->word = strdup("%ipv4%");
			wi->flags.isSpecial = 1;
			goto done;
		}
		if(bDetectStacked && wi->word[nproc] == '/') {
			size_t strtnxt = nproc + 1;
			if(syntax_posint(wi->word+strtnxt, wordlen-strtnxt,
				NULL, &nproc))
				if(strtnxt+nproc == wordlen) {
					free(wi->word);
					wi->word = strdup("%ipv4%");
					wi->flags.isSubword = 1;
					wi->flags.isSpecial = 1;

					struct wordinfo *wit;

					wit = wordinfoNew("%posint%");
					wit->flags.isSubword = 1;
					wit->flags.isSpecial = 1;
					wordstackPush(wit);

					wit = wordinfoNew("/");
					wit->flags.isSubword = 1;
					wordstackPush(wit);
					goto done;
				}
		}
	}
done:
	return;
}

struct wordinfo *
getWord(char **const line)
{

	struct wordinfo *wi = wordstackPop();
	if(wi != NULL)
		return wi;
	char *ln = *line;
	if(*ln == '\0')
		return NULL;
	size_t i;
	for(i = 0 ; ln[i] && isspace(ln[i]) ; ++i)
		/* EMPTY - skip spaces */;
	const size_t begin_word = i;
	for( ; ln[i] && !isspace(ln[i]) ; ++i)
		/* EMPTY - just count */;
	if(begin_word == i) /* only trailing spaces? */
		return NULL;
	const size_t wordlen = i - begin_word;
	wi = wordinfoNew(NULL);
	wi->word = malloc(wordlen + 1);
	memcpy(wi->word, ln+begin_word, wordlen);
	wi->word[wordlen] = '\0';
	if(wi->word[0] == '%') /* assume already token [TODO: improve] */
		goto done;
	wordDetectSyntax(wi, wordlen, 1);
done:
	*line = ln+i;
	return wi;
}

logrec_node_t *
treeAddToLevel(logrec_node_t *const level, /* current node */
	struct wordinfo *const wi,
	struct wordinfo *const nextwi
	)
{
	logrec_node_t *existing, *prev = NULL;

	for(existing = level->child ; existing != NULL ; existing = existing->sibling) {
		struct wordinfo *wi_val;
		if((wi_val = logrec_hasWord(existing, wi->word)) != NULL) {
			wi_val->occurs++;
			break;
		}
		prev = existing;
	}

	if(existing == NULL && nextwi != NULL) {
		/* we check if the next word is the same, if so, we can
		 * just add this as a different value.
		 */
		logrec_node_t *child;
		for(child = level->child ; child != NULL ; child = child->sibling) {
			if(child->child != NULL
			   && !strcmp(child->child->words[0]->word, nextwi->word))
			   	break;
		}
		if(child != NULL) {
			logrec_addWord(child, wi);
			existing = child;
		}
	}

	if(existing == NULL) {
		existing = logrec_newNode(wi, level);
		if(prev == NULL) {
			/* first child of parent node */
			level->child = existing;
		} else {
			/* new sibling */
			prev->sibling = existing;
		}
	}

	return existing;
}

void
treeAddLine(char *ln)
{
	struct wordinfo *wi;
	struct wordinfo *nextwi; /* we need one-word lookahead for structure tree */
	logrec_node_t *level = root;
	nextwi = getWord(&ln);
	while(1) {
		wi = nextwi;
		if(wi == NULL) {
			++level->nterm;
			break;
		}
		nextwi = getWord(&ln);
		level = treeAddToLevel(level, wi, nextwi);
	}
}

void
preprocessLine(const char *const __restrict__ buf,
	const size_t buflen,
	char *const bufout)
{
	static int lnCnt = 1;
	size_t nproc;
	char *tocopy;
	size_t tocopylen;
	size_t iout;

	iout = 0;
	for(size_t i = 0 ; i < buflen ; ) {
		/* in this stage, we must only detect syntaxes that we are
		 * very sure to correctly detect AND that *spawn multiple
		 * words*. Otherwise, it is safer to detect them on a
		 * word basis.
		 */
		if(ln_parseRFC3164Date(buf, buflen, &i, NULL, &nproc, NULL) == 0) {
			tocopy = "%date-rfc3164%";
		} else {
			tocopy = NULL;
			nproc = 1;
		}
		if(ln_parseRFC5424Date(buf, buflen, &i, NULL, &nproc, NULL) == 0) {
			tocopy = "%date-rfc5424%";
		} else {
			tocopy = NULL;
			nproc = 1;
		}

		/* copy to output buffer */
		if(tocopy == NULL) {
			bufout[iout++] = buf[i];
		} else {
			tocopylen = strlen(tocopy); // do this in lower lever
			memcpy(bufout+iout, tocopy, tocopylen);
			iout += tocopylen;
		}
		i += nproc;
	}
	bufout[iout] = '\0';
	++lnCnt;
}
int
processFile(FILE *fp)
{
	char lnbuf[MAXLINE];
	char lnpreproc[MAXLINE];

	while(!feof(fp)) {
		reportProgress("reading");
		size_t i;
		for(i = 0 ; i < sizeof(lnbuf)-1 ; ++i) {
			const int c = fgetc(fp);
			if(c == EOF || c == '\n')
				break;
			lnbuf[i] = c;
		}
		lnbuf[i] = '\0';
		if(i > 0) {
			//processLine(lnbuf, i, &logrec);
			//logrecPrint(logrec);
			preprocessLine(lnbuf, i, lnpreproc);
			treeAddLine(lnpreproc);
		}
	}

	treePrint(root, 0);
	treeSquash(root);
	treePrint(root, 0);
	reportProgress(NULL);
	return 0;
}


int
main(int argc, char *argv[])
{
	int r;
	int ch;
	static const struct option longopts[] = {
		{ "report-progress",	no_argument,	  0, 'p' },
		{ NULL,		0, 0, 0 }
	};

	while ((ch = getopt_long(argc, argv, "p", longopts, NULL)) != -1) {
		switch (ch) {
		case 'p':		/* file to log */
			displayProgress = 1;
			break;
		case '?':
		default:
		//	usage(stderr);
			fprintf(stderr, "invalid option");
			break;
		}
	}

	root = logrec_newNode(wordinfoNew(strdup("[ROOT]")), NULL);
	r = processFile(stdin);
	return r;
}