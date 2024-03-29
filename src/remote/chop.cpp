/*
 *	PROGRAM:	Misc. Function for Server and Tools
 *	MODULE:		chop.c
 *	DESCRIPTION:	Function to nicely truncate a long path/file name
 *			to a specified length.
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
#include "../jrd/y_ref.h"
#include "../jrd/ibase.h"
#include "../jrd/common.h"


extern "C" {


#define PATHSEP(c) ((c) == '\\' || (c) == '/')
char *ChopFileName( char *szName, char *szShortName, ULONG dwLen)
{
/**************************************
 *
 *	C h o p F i l e N a m e
 *
 **************************************
 *
 * Functional description
 *	Try to shorten a given file name to a specified length.  Put
 *	in '...' where anything was taken out.
 *
 *	Both szName and szShort name can be the same pointer.
 *
 *  Inputs: 
 *           szName - Null terminated input path/filename.
 *           szShortName - Output buffer for truncated path/filename.
 *           dwLen - Length to truncate to.
 *
 *  Outputs:
 *				 pointer to a buffer (szShortname).
 *
 **************************************/
	char *pchLeft, *pchRight, *pchLastRight, *pchLastLeft, *pchEnd, *pchTmp;
	BOOLEAN bLeft = TRUE, bLeftFull = FALSE, bRightFull = FALSE;

/* Set pointers to the beginning and the end */
	pchLeft = pchEnd = szName;
	while (*pchEnd)
		pchEnd++;

/* Check that the path is already short enough */
	if (((ULONG) (pchEnd - pchLeft)) <= dwLen) {
		pchTmp = szShortName;
		while (*pchTmp++ = *szName++);
		return szShortName;
	}

/* Subtract the room needed for the three dots */
	dwLen -= 3;

	pchRight = pchEnd;

	pchLastLeft = pchLeft;
	pchLastRight = pchRight;

	while (!bLeftFull || !bRightFull) {
		if (bLeft) {
			while (!bLeftFull && pchLeft++ && !PATHSEP(*pchLeft)
				   && pchLeft < pchRight);
			if ((pchLeft - szName) + ((ULONG) (pchEnd - pchRight)) > dwLen) {
				bLeftFull = TRUE;
				pchLeft = pchLastLeft;
			}
			else
				pchLastLeft = pchLeft;
		}
		else {
			while (!bRightFull && pchRight-- && !PATHSEP(*pchRight)
				   && pchLeft < pchRight);
			if ((pchLeft - szName) + ((ULONG) (pchEnd - pchRight)) > dwLen) {
				bRightFull = TRUE;
				pchRight = pchLastRight;
			}
			else
				pchLastRight = pchRight;
		}
		bLeft = bLeft ? FALSE : TRUE;
	}

	for (pchTmp = szShortName, pchLeft = szName;
		 pchLeft <= pchLastLeft; *pchTmp++ = *pchLeft++);
	*pchTmp++ = '.';
	*pchTmp++ = '.';
	*pchTmp++ = '.';
	for (; pchLastRight < pchEnd; *pchTmp++ = *pchLastRight++);
	*pchTmp++ = '\0';

	return szShortName;
}


} // extern "C"
