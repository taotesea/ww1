/*
 *	PROGRAM:	Preprocessor
 *	MODULE:		hsh_proto.h
 *	DESCRIPTION:	Prototype header file for hsh.c
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

#ifndef _GPRE_HSH_PROTO_H_
#define _GPRE_HSH_PROTO_H_

#ifdef __cplusplus
extern "C" {
#endif

extern void HSH_fini(void);
extern void HSH_init(void);
extern void HSH_insert(SYM);
extern SYM HSH_lookup(SCHAR *);
extern SYM HSH_lookup2(SCHAR *);
extern void HSH_remove(SYM);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _GPRE_HSH_PROTO_H_ */
