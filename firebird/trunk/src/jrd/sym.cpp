/*
 *	PROGRAM:	JRD access method
 *	MODULE:		hsh.cpp
 *	DESCRIPTION:	Hash table and symbol manager
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include <string.h>
#include "../jrd/jrd.h"
#include "../jrd/val.h"
#include "../jrd/err_proto.h"
#include "../jrd/sym_proto.h"
#include "../jrd/thd_proto.h"



static SSHORT hash_func(const SCHAR*);


void SYM_insert(Symbol* symbol)
{
/**************************************
 *
 *	S Y M _ i n s e r t
 *
 **************************************
 *
 * Functional description
 *	Insert a symbol into the hash table.
 *
 **************************************/
	Database* dbb = GET_DBB;

	const int h = hash_func(symbol->sym_string);

	for (Symbol* old = dbb->dbb_hash_table[h]; old; old = old->sym_collision) {
		if (!strcmp(symbol->sym_string, old->sym_string)) {
			symbol->sym_homonym = old->sym_homonym;
			old->sym_homonym = symbol;
			return;
		}
	}

	symbol->sym_collision = dbb->dbb_hash_table[h];
	dbb->dbb_hash_table[h] = symbol;
}


Symbol* SYM_lookup(const TEXT* string)
{
/**************************************
 *
 *	S Y M _ l o o k u p
 *
 **************************************
 *
 * Functional description
 *	Perform a string lookup against hash table.
 *
 **************************************/
	Database* dbb = GET_DBB;

	for (Symbol* symbol = dbb->dbb_hash_table[hash_func(string)]; symbol;
		 symbol = symbol->sym_collision)
	{
		if (!strcmp(string, symbol->sym_string)) 
			return symbol;
	}

	return NULL;
}


void SYM_remove(Symbol* symbol)
{
/**************************************
 *
 *	S Y M _ r e m o v e 
 *
 **************************************
 *
 * Functional description
 *	Remove a symbol from the hash table.
 *
 **************************************/
	Database* dbb = GET_DBB;

	const int h = hash_func(symbol->sym_string);

	for (Symbol** next = &dbb->dbb_hash_table[h]; *next;
		 next = &(*next)->sym_collision)
	{
		if (symbol == *next) {
			Symbol* homonym = symbol->sym_homonym;
			if (homonym) {
				homonym->sym_collision = symbol->sym_collision;
				*next = homonym;
				return;
			}
			else {
				*next = symbol->sym_collision;
				return;
			}
		}
		else {
			for (Symbol** ptr = &(*next)->sym_homonym; *ptr;
				 ptr = &(*ptr)->sym_homonym)
			{
				if (symbol == *ptr) {
					*ptr = symbol->sym_homonym;
					return;
				}
			}
		}
	}

	BUGCHECK(164);				/* msg 164 failed to remove symbol from hash table */
}


static SSHORT hash_func(const SCHAR* string)
{
/**************************************
 *
 *	h a s h
 *
 **************************************
 *
 * Functional description
 *	Returns the hash function of a string.
 *
 **************************************/

/* It is OK to not Internationalize this function as it is for
   internal optimization of symbol lookup */

	int value = 0;

	SCHAR c;
	while ( (c = *string++) ) {
		value = (value << 1) + UPPER7(c);
	}

	return ((value >= 0) ? value : -value) % HASH_SIZE;
}


