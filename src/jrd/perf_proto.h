/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		perf_proto.h
 *	DESCRIPTION:	Prototype header file for perf.c
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

#ifndef _JRD_PERF_PROTO_H_
#define _JRD_PERF_PROTO_H_

#include "../jrd/y_ref.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int API_ROUTINE perf_format(struct perf *, struct perf *, SCHAR *,
								   SCHAR *, SSHORT *);
extern void API_ROUTINE perf_get_info(FRBRD **, struct perf *);
extern void API_ROUTINE perf_report(struct perf *, struct perf *, SCHAR *,
									SSHORT *);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _JRD_PERF_PROTO_H_ */
