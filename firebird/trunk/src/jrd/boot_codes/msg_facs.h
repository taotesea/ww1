/*
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
/*
 * 
 * File generated by codes.e - do not edit!
 * 
 */
/*
 *	MODULE:		msg_facs.h
 *	DESCRIPTION:	ISC message facilities
 *
 */



/******************************/
/*   ISC message facilities   */
/******************************/

typedef struct _facilities {
	int  fac_code;
	char *facility;
	};

static const struct _facilities facilities[] = {
	0,  "JRD       ",
	3,  "GFIX      ",
	7,  "DSQL      ",
	8,  "DYN       ",
	12,  "GBAK      ",
	18,  "GSEC      ",
	19,  "LICENSE   ",
	21,  "GSTAT     ",
	0, NULL};


