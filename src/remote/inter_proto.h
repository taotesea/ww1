/*
 *	PROGRAM:	JRD Remote Interface/Server
 *	MODULE:		inter_proto.h
 *	DESCRIPTION:	Prototype Header file for interface.c
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

#ifndef REMOTE_INTER_PROTO_H
#define REMOTE_INTER_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

extern ISC_STATUS	REM_attach_database (ISC_STATUS *, SSHORT, SCHAR *, struct rdb **, SSHORT, SCHAR *, UCHAR *);
extern ISC_STATUS	REM_attach_service (ISC_STATUS *, USHORT, TEXT *, struct rdb **, USHORT, SCHAR *);
extern ISC_STATUS	REM_blob_info (ISC_STATUS *, struct rbl **, SSHORT, SCHAR *, SSHORT, SCHAR *);
extern ISC_STATUS	REM_cancel_blob (ISC_STATUS *, struct rbl **);
extern ISC_STATUS	REM_cancel_events (ISC_STATUS *, struct rdb **, SLONG *);
extern ISC_STATUS	REM_close_blob (ISC_STATUS *, struct rbl **);
extern ISC_STATUS	REM_commit_transaction (ISC_STATUS *, struct rtr **);
extern ISC_STATUS	REM_commit_retaining (ISC_STATUS *, struct rtr **);
extern ISC_STATUS	REM_compile_request (ISC_STATUS *, struct rdb **, struct rrq **, USHORT, UCHAR *);
extern ISC_STATUS	REM_create_blob2 (ISC_STATUS *, struct rdb **, struct rtr **, struct rbl **, BID, USHORT, UCHAR *);
extern ISC_STATUS	REM_create_database (ISC_STATUS *, SSHORT, SCHAR *, struct rdb **, SSHORT, SCHAR *, SSHORT, UCHAR *);
extern ISC_STATUS	REM_database_info (ISC_STATUS *, struct rdb **, SSHORT, SCHAR *, SSHORT, SCHAR *);
extern ISC_STATUS	REM_ddl (ISC_STATUS *, struct rdb **, struct rtr **, USHORT, UCHAR *);
extern ISC_STATUS	REM_detach_database (ISC_STATUS *, struct rdb **);
extern ISC_STATUS	REM_detach_service (ISC_STATUS *, struct rdb **);
extern ISC_STATUS	REM_drop_database (ISC_STATUS *, struct rdb **);
extern ISC_STATUS	REM_allocate_statement (ISC_STATUS *, struct rdb **, struct rsr **);
extern ISC_STATUS	REM_execute (ISC_STATUS *, struct rtr **, struct rsr **, USHORT, UCHAR *, USHORT, USHORT, UCHAR *);
extern ISC_STATUS	REM_execute2 (ISC_STATUS *, struct rtr **, struct rsr **, USHORT, UCHAR *, USHORT, USHORT, UCHAR *, USHORT, UCHAR *, USHORT, USHORT, UCHAR *);
extern ISC_STATUS	REM_execute_immediate (ISC_STATUS *, struct rdb **, struct rtr **, USHORT, TEXT *, USHORT, USHORT, UCHAR *, USHORT, USHORT, UCHAR *);
extern ISC_STATUS	REM_execute_immediate2 (ISC_STATUS *, struct rdb **, struct rtr **, USHORT, TEXT *, USHORT, USHORT, UCHAR *, USHORT, USHORT, UCHAR *, USHORT, UCHAR *, USHORT, USHORT, UCHAR *);
extern ISC_STATUS	REM_fetch (ISC_STATUS *, struct rsr **, USHORT, UCHAR *, USHORT, USHORT, UCHAR *);
extern ISC_STATUS	REM_free_statement (ISC_STATUS *, struct rsr **, USHORT);
extern ISC_STATUS	REM_insert (ISC_STATUS *, struct rsr **, USHORT, UCHAR *, USHORT, USHORT, UCHAR *);
extern ISC_STATUS	REM_prepare (ISC_STATUS *, struct rtr **, struct rsr **, USHORT, TEXT *, USHORT, USHORT, SCHAR *, USHORT, SCHAR *);
extern ISC_STATUS	REM_set_cursor_name (ISC_STATUS *, struct rsr **, TEXT *, USHORT);
extern ISC_STATUS	REM_sql_info (ISC_STATUS *, struct rsr **, SSHORT, SCHAR *, SSHORT, SCHAR *);
extern ISC_STATUS	REM_get_segment (ISC_STATUS *, struct rbl **, USHORT *, USHORT, UCHAR *);
extern ISC_STATUS	REM_get_slice (ISC_STATUS *, struct rdb **, struct rtr **, BID, USHORT, UCHAR *, USHORT, UCHAR *, SLONG, UCHAR *, SLONG *);
extern ISC_STATUS	REM_open_blob2 (ISC_STATUS *, struct rdb **, struct rtr **, struct rbl **, BID, USHORT, UCHAR *);
extern ISC_STATUS	REM_prepare_transaction (ISC_STATUS *, struct rtr **, USHORT, UCHAR *);
extern ISC_STATUS	REM_put_segment (ISC_STATUS *, struct rbl **, USHORT, UCHAR *);
extern ISC_STATUS	REM_put_slice (ISC_STATUS *, struct rdb **, struct rtr **, BID, USHORT, UCHAR *, USHORT, UCHAR *, SLONG, UCHAR *);
extern ISC_STATUS	REM_que_events (ISC_STATUS *, struct rdb **, SLONG *, SSHORT, UCHAR *, void (*)(void*, USHORT, UCHAR*), void *);
extern ISC_STATUS	REM_query_service (ISC_STATUS *, struct rdb **, USHORT, SCHAR *, USHORT, SCHAR *, USHORT, SCHAR *);
extern ISC_STATUS	REM_receive (ISC_STATUS *, struct rrq **, USHORT, USHORT, UCHAR *, SSHORT);
extern ISC_STATUS	REM_reconnect_transaction (ISC_STATUS *, struct rdb **, struct rtr **, USHORT, UCHAR *);
extern ISC_STATUS	REM_release_request (ISC_STATUS *, struct rrq **);
extern ISC_STATUS	REM_request_info (ISC_STATUS *, struct rrq **, SSHORT, SSHORT, UCHAR *, SSHORT, UCHAR *);
extern ISC_STATUS	REM_rollback_transaction (ISC_STATUS *, struct rtr **);
extern ISC_STATUS	REM_seek_blob (ISC_STATUS *, struct rbl **, SSHORT, SLONG, SLONG *);
extern ISC_STATUS	REM_send (ISC_STATUS *, struct rrq **, USHORT, USHORT, UCHAR *, SSHORT);
extern ISC_STATUS	REM_start_and_send (ISC_STATUS *, struct rrq **, struct rtr **, USHORT, USHORT, UCHAR *, SSHORT);
extern ISC_STATUS	REM_start_request (ISC_STATUS *, struct rrq **, struct rtr **, USHORT);
extern ISC_STATUS	REM_start_transaction (ISC_STATUS *, struct rtr **, SSHORT, struct rdb **, SSHORT, UCHAR *);
extern ISC_STATUS	REM_transact_request (ISC_STATUS *, struct rdb **, struct rtr **, USHORT, UCHAR *, USHORT, UCHAR *, USHORT, UCHAR *);
extern ISC_STATUS	REM_transaction_info (ISC_STATUS *, struct rtr **, SSHORT, UCHAR *, SSHORT, UCHAR *);
extern ISC_STATUS	REM_unwind_request (ISC_STATUS *, struct rrq **, USHORT);

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif	/* REMOTE_INTER_PROTO_H */
