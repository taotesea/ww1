/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		thd_proto.h
 *	DESCRIPTION:	Prototype header file for thd.c
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

#ifndef _JRD_THD_PROTO_H_
#define _JRD_THD_PROTO_H_

#include "../jrd/isc.h"

#ifdef __cplusplus
extern "C" {
#endif

int		API_ROUTINE gds__thread_start(FPTR_INT_VOID_PTR, void*, int, int,
										 void*);

struct thdd* DLL_EXPORT THD_get_specific(void);
void	DLL_EXPORT THD_init(void);
void	DLL_EXPORT THD_cleanup(void);
int		DLL_EXPORT THD_mutex_destroy(struct mutx_t*);
void	DLL_EXPORT THD_put_specific(struct thdd*);
int		DLL_EXPORT THD_wlck_destroy(struct wlck_t*);
int		DLL_EXPORT THD_wlck_init(struct wlck_t*);
int		DLL_EXPORT THD_wlck_lock(struct wlck_t*, USHORT);
int		DLL_EXPORT THD_wlck_unlock(struct wlck_t*);

FB_THREAD_ID THD_get_thread_id(void);
void	THD_getspecific_data(void** t_data);

int		THD_mutex_init(struct mutx_t*);
int		THD_mutex_lock(struct mutx_t*);
int		THD_mutex_unlock(struct mutx_t*);
int		THD_mutex_destroy_n(struct mutx_t*, USHORT);
int		THD_mutex_init_n(struct mutx_t*, USHORT);
int		THD_mutex_lock_global(void);
int		THD_mutex_unlock_global(void);
void	THD_putspecific_data(void* t_data);

#ifdef SUPERSERVER
int		THD_rec_mutex_destroy(struct rec_mutx_t*);
int		THD_rec_mutex_init(struct rec_mutx_t*);
int		THD_rec_mutex_lock(struct rec_mutx_t*);
int		THD_rec_mutex_unlock(struct rec_mutx_t*);
#endif

struct thdd* DLL_EXPORT THD_restore_specific(void);
int		THD_resume(THD_T);
void	THD_sleep(ULONG);
int		THD_suspend(THD_T);
void	THD_wlck_destroy_n(struct wlck_t*, USHORT);
void	THD_wlck_init_n(struct wlck_t*, USHORT);
void	THD_yield(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _JRD_THD_PROTO_H_ */
