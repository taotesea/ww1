/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		why.cpp
 *	DESCRIPTION:	Universal Y-valve
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
 *
 * 23-Feb-2002 Dmitry Yemanov - Events wildcarding
 *
 *
 * 2002-02-23 Sean Leyne - Code Cleanup, removed old Win3.1 port (Windows_Only)
 * 2002-02-23 Sean Leyne - Code Cleanup, removed old M88K and NCR3000 port
 *
 * 2002.10.27 Sean Leyne - Completed removal of obsolete "IMP" port
 * 2002.10.27 Sean Leyne - Completed removal of obsolete "DG_X86" port
 *
 * 2002.10.28 Sean Leyne - Code cleanup, removed obsolete "MPEXL" port
 * 2002.10.28 Sean Leyne - Code cleanup, removed obsolete "DecOSF" port
 * 2002.10.28 Sean Leyne - Code cleanup, removed obsolete "SGI" port
 *
 * 2002.10.29 Sean Leyne - Removed obsolete "Netware" port
 *
 * 2002.10.30 Sean Leyne - Removed support for obsolete "PC_PLATFORM" define
 *
 * 2002.12.10 Alex Peshkoff: 1. Moved struct hndl declaration to h-file
 * 							 2. renamed all specific objects to WHY_*
 *
 */

#include "firebird.h"
#include "memory_routines.h"	// needed for get_long
#include "consts_pub.h"

#include <stdlib.h>
#include <string.h>
#include "../jrd/common.h"
#include <stdarg.h>

#include <stdio.h>
#include "../jrd/gdsassert.h"

/* includes specific for DSQL */

#include "../dsql/sqlda.h"
#include "../dsql/sqlda_pub.h"
#include "../dsql/prepa_proto.h"
#include "../dsql/utld_proto.h"

/* end DSQL-specific includes */

#include "../jrd/why_proto.h"
#include "../common/classes/alloc.h"
#include "../common/classes/array.h"
#include "../common/classes/fb_string.h"
#include "../jrd/thread_proto.h"
#include "gen/iberror.h"
#include "../jrd/msg_encode.h"
#include "gen/msg_facs.h"
#include "../jrd/acl.h"
#include "../jrd/inf_pub.h"
#include "../jrd/isc.h"
#include "../jrd/fil.h"
#include "../jrd/db_alias.h"
#include "../jrd/os/path_utils.h"
#include "../common/classes/ClumpletWriter.h"
#include "../common/utils_proto.h"

#include "../jrd/flu_proto.h"
#include "../jrd/gds_proto.h"
#include "../jrd/isc_proto.h"
#include "../jrd/isc_f_proto.h"
#include "../jrd/os/isc_i_proto.h"
#include "../jrd/isc_s_proto.h"
#include "../jrd/utl_proto.h"
#include "../common/classes/rwlock.h"
#include "../common/classes/auto.h"
#include "../common/classes/init.h"
#include "../common/classes/semaphore.h"
#include "../jrd/constants.h"
#include "../jrd/ThreadStart.h"
#ifdef SCROLLABLE_CURSORS
#include "../jrd/blr.h"
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#ifdef HAVE_FLOCK
#include <sys/file.h>			/* for flock() */
#endif

#ifdef WIN_NT
#include <windows.h>
#endif

#ifndef F_OK
#define F_OK		0
#endif

#ifndef F_TLOCK
#define F_TLOCK		2
#endif

const int IO_RETRY	= 20;

inline void init_status(ISC_STATUS* vector)
{
	vector[0] = isc_arg_gds;
	vector[1] = FB_SUCCESS;
	vector[2] = isc_arg_end;
}

inline bool is_network_error(const ISC_STATUS* vector)
{
	return vector[1] == isc_network_error || vector[1] == isc_net_write_err ||
		vector[1] == isc_net_read_err;
}

inline void bad_handle(ISC_STATUS code)
{
	Firebird::status_exception::raise(code, isc_arg_end);
}

inline void nullCheck(const FB_API_HANDLE* ptr, ISC_STATUS code)
{
	// this function is called for incoming API handles,
	// proposed to be created by the call
	if ((!ptr) || (*ptr)) 
	{
		bad_handle(code);
	}
}

#if !defined (SUPERCLIENT)
static bool shutdown_flag = false;
#endif

typedef ISC_STATUS(*PTR) (ISC_STATUS* user_status, ...);

/* Transaction element block */

struct teb
{
	FB_API_HANDLE *teb_database;
	int teb_tpb_length;
	const UCHAR *teb_tpb;
};
typedef teb TEB;

#ifdef DEBUG_GDS_ALLOC
#define alloc(x) alloc_debug(x, __FILE__, __LINE__)
static SCHAR *alloc_debug(SLONG, const char*, int);
#else
static SCHAR *alloc(SLONG);
#endif
static void free_block(void*);
static ISC_STATUS detach_or_drop_database(ISC_STATUS * user_status, FB_API_HANDLE * handle,
										  const int proc, const ISC_STATUS specCode = 0);

namespace Jrd {
	class Attachment;
	class jrd_tra;
	class jrd_req;
	class dsql_req;
}

namespace
{
	// flags
	const UCHAR HANDLE_TRANSACTION_limbo	= 0x01;
	const UCHAR HANDLE_STATEMENT_prepared	= 0x02;

	// forwards
	class Attachment;
	class Transaction;
	class Request;
	class Blob;
	class Statement;
	class Service;

	// force use of default memory pool for Y-Valve objects
	typedef Firebird::GlobalStorage DefaultMemory;

	// stored handle types
	typedef Jrd::jrd_tra StoredTra;
	typedef void StoredReq;
	typedef void StoredBlb;
	typedef Jrd::Attachment StoredAtt;
	typedef Jrd::dsql_req StoredStm;
	typedef void StoredSvc;

	template <typename CleanupRoutine, typename CleanupArg>
	class Clean : public DefaultMemory
	{
	private:
		struct st_clean
		{
			CleanupRoutine *Routine;
			void* clean_arg;
			st_clean(CleanupRoutine *r, void* a)
				: Routine(r), clean_arg(a) { }
			st_clean()
				: Routine(0), clean_arg(0) { }
		};
		Firebird::HalfStaticArray<st_clean, 1> calls;
		Firebird::Mutex mutex;

	public:
		Clean() : calls(*getDefaultMemoryPool()) { }

		void add(CleanupRoutine *r, void* a)
		{
			Firebird::MutexLockGuard guard(mutex);
			for (size_t i = 0; i < calls.getCount(); ++i)
			{
				if (calls[i].Routine == r && 
					calls[i].clean_arg == a)
				{
					return;
				}
			}
			calls.add(st_clean(r, a));
		}

		void call(CleanupArg public_handle)
		{
			Firebird::MutexLockGuard guard(mutex);
			for (size_t i = 0; i < calls.getCount(); ++i)
			{
				if (calls[i].Routine)
				{
					calls[i].Routine(public_handle, calls[i].clean_arg);
				}
			}
		}
	};

	class BaseHandle : public DefaultMemory
	{
	public:
		UCHAR			type;
		UCHAR			flags;
		USHORT			implementation;
		FB_API_HANDLE	public_handle;
		Attachment*		parent;
    	FB_API_HANDLE*	user_handle;

	protected:
		BaseHandle(UCHAR t, FB_API_HANDLE* pub, Attachment* par, USHORT imp = ~0);

	public:
		static BaseHandle* translate(FB_API_HANDLE);
		Jrd::Attachment* getAttachmentHandle();
		~BaseHandle();

		// required to put pointers to it into the tree
		static const FB_API_HANDLE& generate(const void* sender, const BaseHandle* value) 
		{
			return value->public_handle;
		}
	};

	template <typename HType>
		void toParent(Firebird::SortedArray<HType*>& members, HType* newMember, Firebird::Mutex& mutex)
	{
		Firebird::MutexLockGuard guard(mutex);
		members.add(newMember);
	}

	template <typename HType>
		void fromParent(Firebird::SortedArray<HType*>& members, HType* newMember, Firebird::Mutex& mutex)
	{
		Firebird::MutexLockGuard guard(mutex);
		size_t pos;
		if (members.find(newMember, pos))
		{
			members.remove(pos);
		}
#ifdef DEV_BUILD
		else
		{
			//Attempt to deregister not registered member
			fb_assert(false);
		}
#endif
	}

	template <typename ToHandle>
		ToHandle* translate(FB_API_HANDLE* handle)
	{
		if (handle && *handle)
		{
			BaseHandle* rc = BaseHandle::translate(*handle);
			if (rc && rc->type == ToHandle::hType())
			{
				return static_cast<ToHandle*>(rc);
			}
		}

		Firebird::status_exception::raise(ToHandle::hError(), 
										  isc_arg_end);
		// compiler warning silencer
		return 0;
	}

	class Attachment : public BaseHandle
	{
	public:
		Firebird::SortedArray<Transaction*> transactions;
		Firebird::SortedArray<Request*> requests;
		Firebird::SortedArray<Blob*> blobs;
		Firebird::SortedArray<Statement*> statements;
		// Each array can be protected with personal mutex,
		// but possibility of collision is so slow here, that's why
		// I prefer to save resources, using single mutex.
		Firebird::Mutex mutex;

		int enterCount;
		Firebird::Mutex enterMutex;

		Clean<AttachmentCleanupRoutine, FB_API_HANDLE*> cleanup;
		StoredAtt* handle;
		Firebird::PathName db_path;

		static ISC_STATUS hError()
		{
			return isc_bad_db_handle;
		}

		static UCHAR hType()
		{
			return 1;
		}

	public:
		Attachment(StoredAtt*, FB_API_HANDLE*, USHORT);
		~Attachment();
	};

	class Transaction : public BaseHandle
	{
	public:
		Clean<TransactionCleanupRoutine, FB_API_HANDLE> cleanup;
		Transaction* next;
		StoredTra* handle;

		static ISC_STATUS hError()
		{
			return isc_bad_trans_handle;
		}

		static UCHAR hType()
		{
			return 2;
		}

	public:
		Transaction(StoredTra* h, FB_API_HANDLE* pub, Attachment* par)
			: BaseHandle(hType(), pub, par), 
			  next(0), handle(h)
		{
			toParent<Transaction>(parent->transactions, this, parent->mutex);
		}

		Transaction(FB_API_HANDLE* pub, USHORT a_implementation)
			: BaseHandle(hType(), pub, 0, a_implementation), 
			  next(0), handle(0)
		{
		}

		~Transaction()
		{
			cleanup.call(public_handle);
			if (parent)
			{
				fromParent<Transaction>(parent->transactions, this, parent->mutex);
			}
		}
	};

	class Request : public BaseHandle
	{
	public:
		StoredReq* handle;

		static ISC_STATUS hError()
		{
			return isc_bad_req_handle;
		}

		static UCHAR hType()
		{
			return 3;
		}

	public:
		Request(StoredReq* h, FB_API_HANDLE* pub, Attachment* par)
			: BaseHandle(hType(), pub, par), handle(h)
		{
			toParent<Request>(parent->requests, this, parent->mutex);
		}

		~Request() 
		{ 
			fromParent<Request>(parent->requests, this, parent->mutex);
		}
	};

	class Blob : public BaseHandle
	{
	public:
		StoredBlb* handle;

		static ISC_STATUS hError()
		{
			return isc_bad_segstr_handle;
		}

		static UCHAR hType()
		{
			return 4;
		}

	public:
		Blob(StoredBlb* h, FB_API_HANDLE* pub, Attachment* par)
			: BaseHandle(hType(), pub, par), handle(h)
		{
			toParent<Blob>(parent->blobs, this, parent->mutex);
		}

		~Blob() 
		{ 
			fromParent<Blob>(parent->blobs, this, parent->mutex);
		}
	};

	class Statement : public BaseHandle
	{
	public:
		StoredStm* handle;
		struct sqlda_sup das;

		static ISC_STATUS hError()
		{
			return isc_bad_stmt_handle;
		}

		static UCHAR hType()
		{
			return 5;
		}

	public:
		Statement(StoredStm* h, FB_API_HANDLE* pub, Attachment* par)
			: BaseHandle(hType(), pub, par), handle(h)
		{
			toParent<Statement>(parent->statements, this, parent->mutex);
			memset(&das, 0, sizeof das);
		}

		void checkPrepared()
		{
			if (!(flags & HANDLE_STATEMENT_prepared))
			{
				Firebird::status_exception::raise(isc_unprepared_stmt, isc_arg_end);
			}
		}

		~Statement() 
		{ 
			fromParent<Statement>(parent->statements, this, parent->mutex);
		}
	};

	class Service : public BaseHandle
	{
	public:
		Clean<AttachmentCleanupRoutine, FB_API_HANDLE*> cleanup;
		StoredSvc* handle;

		static ISC_STATUS hError()
		{
			return isc_bad_svc_handle;
		}

		static UCHAR hType()
		{
			return 6;
		}

	public:
		Service(StoredSvc* h, FB_API_HANDLE* pub, USHORT impl)
			: BaseHandle(hType(), pub, 0, impl), handle(h)
		{
		}

		~Service() 
		{ 
			cleanup.call(&public_handle);
		}
	};

	typedef Firebird::BePlusTree<BaseHandle*, FB_API_HANDLE, MemoryPool, BaseHandle> HandleMapping;

	Firebird::GlobalPtr<HandleMapping> handleMapping;
	ULONG handle_sequence_number = 0;
	Firebird::GlobalPtr<Firebird::RWLock> handleMappingLock;

	Firebird::InitInstance<Firebird::SortedArray<Attachment*> > attachments;
	Firebird::GlobalPtr<Firebird::Mutex> attachmentsMutex, shutdownMutex;

	class ShutChain : public Firebird::GlobalStorage
	{
	private:
		ShutChain(ShutChain* link, FPTR_INT cb, const int m)
			: next(link), callBack(cb), mask(m) { }

		~ShutChain() { }

	private:
		static ShutChain* list;
		ShutChain* next;
		FPTR_INT callBack;
		int mask;

	public:
		static void add(FPTR_INT cb, const int m)
		{
			Firebird::MutexLockGuard guard(shutdownMutex);

			for (const ShutChain* chain = list; chain; chain = chain->next)
			{
				if (chain->callBack == cb && chain->mask == m)
				{
					return;
				}
			}

			list = new ShutChain(list, cb, m);
		}

		static int run(const int m)
		{
			int rc = FB_SUCCESS;
			Firebird::MutexLockGuard guard(shutdownMutex);

			for (ShutChain* chain = list; chain; chain = chain->next)
			{
				if ((chain->mask & m) && (chain->callBack() != FB_SUCCESS))
				{
					rc = FB_FAILURE;
				}
			}

			return rc;
		}
	};

	ShutChain* ShutChain::list = 0;


	BaseHandle::BaseHandle(UCHAR t, FB_API_HANDLE* pub, Attachment* par, USHORT imp)
		: type(t), flags(0), implementation(par ? par->implementation : imp), 
		  parent(par), user_handle(0)
	{
		fb_assert(par || (imp != USHORT(~0)));

		{ // scope for write lock on handleMappingLock
			Firebird::WriteLockGuard sync(handleMappingLock);
			// Loop until we find an empty handle slot.
			// This is to care of case when counter rolls over
			do {
				// Generate handle number using rolling counter.
				// This way we tend to give out unique handle numbers and closed
				// handles do not appear as valid to our clients.
				ULONG temp = ++handle_sequence_number;

				// Avoid generating NULL handle when sequence number wraps
				if (!temp)
					temp = ++handle_sequence_number;
				public_handle = (FB_API_HANDLE)(IPTR)temp;
			} while (!handleMapping->add(this));
		}

		if (pub)
		{
			*pub = public_handle;
		}
	}

	BaseHandle* BaseHandle::translate(FB_API_HANDLE handle)
	{
		Firebird::ReadLockGuard sync(handleMappingLock);

		HandleMapping::Accessor accessor(&handleMapping);
		if (accessor.locate(handle))
		{
			return accessor.current();
		}

		return 0;
	}

	Jrd::Attachment* BaseHandle::getAttachmentHandle()
	{
		return parent ? parent->handle : 0;
	}

	BaseHandle::~BaseHandle()
	{
		if (user_handle)
		{
			*user_handle = 0;
		}

		Firebird::WriteLockGuard sync(handleMappingLock);

		// Silently ignore bad handles for PROD_BUILD
		if (handleMapping->locate(public_handle)) 
		{
			handleMapping->fastRemove();
		}
#ifdef DEV_BUILD
		else
		{
			//Attempt to release bad handle
			fb_assert(false);
		}
#endif
	}

	Attachment::Attachment(StoredAtt* h, FB_API_HANDLE* pub, USHORT impl)
		: BaseHandle(hType(), pub, 0, impl), 
		  transactions(*getDefaultMemoryPool()),
		  requests(*getDefaultMemoryPool()),
		  blobs(*getDefaultMemoryPool()),
		  statements(*getDefaultMemoryPool()),
		  handle(h),
		  db_path(*getDefaultMemoryPool())
	{
		toParent<Attachment>(attachments(), this, attachmentsMutex);
		parent = this;
	}

	Attachment::~Attachment()
	{
		cleanup.call(&public_handle);
		fromParent<Attachment>(attachments(), this, attachmentsMutex);
	}

}

#ifdef DEV_BUILD
static void check_status_vector(const ISC_STATUS*);
#endif

static void event_ast(void*, USHORT, const UCHAR*);
static void exit_handler(event_t*);

static Transaction* find_transaction(Attachment*, Transaction*);

inline Transaction* findTransaction(FB_API_HANDLE* public_handle, Attachment *a)
{
	Transaction* t = find_transaction(a, translate<Transaction>(public_handle));
	if (! t)
	{
		bad_handle(isc_bad_trans_handle);
	}

	return t;
}

static int get_database_info(ISC_STATUS *, Transaction*, TEXT **);
static const PTR get_entrypoint(int, int);
static USHORT sqlda_buffer_size(USHORT, XSQLDA*, USHORT);
static ISC_STATUS get_transaction_info(ISC_STATUS *, Transaction*, TEXT **);

static void iterative_sql_info(ISC_STATUS *, FB_API_HANDLE*, SSHORT, const SCHAR *, SSHORT,
							   SCHAR *, USHORT, XSQLDA *);
static ISC_STATUS open_blob(ISC_STATUS*, FB_API_HANDLE*, FB_API_HANDLE*, FB_API_HANDLE*, SLONG*,
						USHORT, const UCHAR*, SSHORT, SSHORT);
static ISC_STATUS prepare(ISC_STATUS *, Transaction*);
static void release_dsql_support(sqlda_sup&);
static void save_error_string(ISC_STATUS *);
static bool set_path(const Firebird::PathName&, Firebird::PathName&);
static void subsystem_enter(void) throw();
static void subsystem_exit(void) throw();

static event_t why_event[1];
static bool why_initialized = false;

/* subsystem_usage is used to count how many active ATTACHMENTs are 
 * running though the why valve.  For the first active attachment
 * request we reset the Firebird FPE handler.
 * This counter is incremented for each ATTACH DATABASE, ATTACH SERVER,
 * or CREATE DATABASE.  This counter is decremented for each 
 * DETACH DATABASE, DETACH SERVER, or DROP DATABASE.
 *
 * A client-only API call, isc_reset_fpe() also controls the re-setting of
 * the FPE handler.
 *	isc_reset_fpe (0);		(default)
 *		Initialize the FPE handler the first time the gds
 *		library is made active.
 *	isc_reset_fpe (1);
 *		Initialize the FPE handler the NEXT time an API call is
 *		invoked.
 *	isc_reset_fpe (2);
 *		Revert to InterBase pre-V4.1.0 behavior, reset the FPE
 *		handler on every API call.
 */

static const USHORT FPE_RESET_INIT_ONLY			= 0x0;	/* Don't reset FPE after init */
static const USHORT FPE_RESET_NEXT_API_CALL		= 0x1;	/* Reset FPE on next gds call */
static const USHORT FPE_RESET_ALL_API_CALL		= 0x2;	/* Reset FPE on all gds call */

#if !(defined SUPERCLIENT || defined SUPERSERVER)
extern ULONG isc_enter_count;
static ULONG subsystem_usage = 0;
static USHORT subsystem_FPE_reset = FPE_RESET_INIT_ONLY;
#define SUBSYSTEM_USAGE_INCR	subsystem_usage++
#define SUBSYSTEM_USAGE_DECR	subsystem_usage--
#else
#define SUBSYSTEM_USAGE_INCR	/* nothing */
#define SUBSYSTEM_USAGE_DECR	/* nothing */
#endif

/* 
 * Global array to store string from the status vector in 
 * save_error_string.
 */

const int MAXERRORSTRINGLENGTH	= 250;
static TEXT glbstr1[MAXERRORSTRINGLENGTH];
static const TEXT glbunknown[10] = "<unknown>";

const USHORT PREPARE_BUFFER_SIZE	= 32768;	// size of buffer used in isc_dsql_prepare call
const USHORT DESCRIBE_BUFFER_SIZE	= 1024;		// size of buffer used in isc_dsql_describe_xxx call

namespace 
{
	// Status:	Provides correct status vector for operation and init() it.
	class Status
	{
	public:
		explicit Status(ISC_STATUS* v) throw()
			: local_vector(v ? v : local_status)
		{
			init_status(local_vector);
		}

		operator ISC_STATUS*() const
		{
			return local_vector;
		}

		~Status()
		{
#ifdef DEV_BUILD
			check_status_vector(local_vector);
#endif
		}

	private:
		ISC_STATUS_ARRAY local_status;
		ISC_STATUS* local_vector;
	};

#ifdef UNIX
	int killed;
	bool procInt, procTerm;

	const int SHUTDOWN_TIMEOUT = 5000;	// 5 sec

	void atExitShutdown()
	{
		fb_shutdown(SHUTDOWN_TIMEOUT);
	}

	Firebird::GlobalPtr<Firebird::SignalSafeSemaphore> shutdownSemaphore;

	THREAD_ENTRY_DECLARE shutdownThread(THREAD_ENTRY_PARAM)
	{
		for (;;)
		{
			killed = 0;
			try {
				shutdownSemaphore->enter();
			}
			catch (Firebird::status_exception& e)
			{
				TEXT buffer[BUFFER_LARGE];
				const ISC_STATUS* vector = e.value();
				if (! (vector && fb_interpret(buffer, sizeof(buffer), &vector)))
				{
					strcpy(buffer, "Unknown failure in shutdown thread in shutSem:enter()");
				}
				gds__log(buffer, 0);
				exit(0);
			}

			if (! killed)
			{
				break;
			}

			// perform shutdown
			if (fb_shutdown(SHUTDOWN_TIMEOUT) == FB_SUCCESS)
			{
				Firebird::InstanceControl::registerShutdown(0);
				exit(0);
			}
		}

		return 0;
	}

	void handler(int sig)
	{
		if (killed)
		{
			return;
		}
		killed = sig;
#if !defined (SUPERCLIENT)
		shutdown_flag = true;
#endif
		shutdownSemaphore->release();		
	}

	void handlerInt(void*)
	{
		handler(SIGINT);
	}

	void handlerTerm(void*)
	{
		handler(SIGTERM);
	}

	class CtrlCHandler
	{
	public:
		explicit CtrlCHandler(Firebird::MemoryPool&)
		{
			Firebird::InstanceControl::registerShutdown(atExitShutdown);

			gds__thread_start(shutdownThread, 0, 0, 0, 0);

			procInt = ISC_signal(SIGINT, handlerInt, 0);
			procTerm = ISC_signal(SIGTERM, handlerTerm, 0);
		}

		~CtrlCHandler()
		{
			ISC_signal_cancel(SIGINT, handlerInt, 0);
			ISC_signal_cancel(SIGTERM, handlerTerm, 0);

			if (! killed)
			{
				// must be done to let shutdownThread close
				shutdownSemaphore->release();
				THREAD_YIELD();
			}
		}
	};
#endif //UNIX

	// YEntry:	Tracks subsystem_enter/exit() calls.
	//			Accounts activity per different attachments.
	class YEntry : public Status
	{
	public:
		explicit YEntry(ISC_STATUS* v) throw()
			: Status(v), att(0)
		{
			subsystem_enter();
#ifdef UNIX
			static Firebird::GlobalPtr<CtrlCHandler> ctrlCHandler;
#endif //UNIX
		}

		void setPrimaryHandle(BaseHandle* primary)
		{
			if (primary && primary->parent && (!att))
			{
				att = primary->parent;
				Firebird::MutexLockGuard guard(att->enterMutex);
				att->enterCount++;
			}
		}

		~YEntry()
		{
			if (att)
			{
				Firebird::MutexLockGuard guard(att->enterMutex);
				att->enterCount--;
			}
			subsystem_exit();
		}

	private:
		YEntry(const YEntry&);	// prohibit copy constructor
		Attachment* att;
	};

} // anonymous namespace


#define CALL(proc, handle)	(get_entrypoint(proc, handle))


#define GDS_ATTACH_DATABASE		isc_attach_database
#define GDS_BLOB_INFO			isc_blob_info
#define GDS_CANCEL_BLOB			isc_cancel_blob
#define GDS_CANCEL_EVENTS		isc_cancel_events
#define GDS_CANCEL_OPERATION	gds__cancel_operation
#define GDS_CLOSE_BLOB			isc_close_blob
#define GDS_COMMIT				isc_commit_transaction
#define GDS_COMMIT_RETAINING	isc_commit_retaining
#define GDS_COMPILE				isc_compile_request
#define GDS_COMPILE2			isc_compile_request2
#define GDS_CREATE_BLOB			isc_create_blob
#define GDS_CREATE_BLOB2		isc_create_blob2
#define GDS_CREATE_DATABASE		isc_create_database
#define GDS_DATABASE_INFO		isc_database_info
#define GDS_DDL					isc_ddl
#define GDS_DETACH				isc_detach_database
#define GDS_DROP_DATABASE		isc_drop_database
//#define GDS_EVENT_WAIT			gds__event_wait
#define GDS_GET_SEGMENT			isc_get_segment
#define GDS_GET_SLICE			isc_get_slice
#define GDS_OPEN_BLOB			isc_open_blob
#define GDS_OPEN_BLOB2			isc_open_blob2
#define GDS_PREPARE				isc_prepare_transaction
#define GDS_PREPARE2			isc_prepare_transaction2
#define GDS_PUT_SEGMENT			isc_put_segment
#define GDS_PUT_SLICE			isc_put_slice
#define GDS_QUE_EVENTS			isc_que_events
#define GDS_RECONNECT			isc_reconnect_transaction
#define GDS_RECEIVE				isc_receive

#ifdef SCROLLABLE_CURSORS
#define GDS_RECEIVE2			isc_receive2
#endif

#define GDS_RELEASE_REQUEST		isc_release_request
#define GDS_REQUEST_INFO		isc_request_info
#define GDS_ROLLBACK			isc_rollback_transaction
#define GDS_ROLLBACK_RETAINING	isc_rollback_retaining
#define GDS_SEEK_BLOB			isc_seek_blob
#define GDS_SEND				isc_send
#define GDS_START_AND_SEND		isc_start_and_send
#define GDS_START				isc_start_request
#define GDS_START_MULTIPLE		isc_start_multiple
#define GDS_START_TRANSACTION	isc_start_transaction
#define GDS_TRANSACT_REQUEST	isc_transact_request
#define GDS_TRANSACTION_INFO	isc_transaction_info
#define GDS_UNWIND				isc_unwind_request

#define GDS_DSQL_ALLOCATE		isc_dsql_allocate_statement
#define GDS_DSQL_ALLOC			isc_dsql_alloc_statement
#define GDS_DSQL_ALLOC2			isc_dsql_alloc_statement2
#define GDS_DSQL_EXECUTE		isc_dsql_execute
#define GDS_DSQL_EXECUTE2		isc_dsql_execute2
#define GDS_DSQL_EXECUTE_M		isc_dsql_execute_m
#define GDS_DSQL_EXECUTE2_M		isc_dsql_execute2_m
#define GDS_DSQL_EXECUTE_IMMED	isc_dsql_execute_immediate
#define GDS_DSQL_EXECUTE_IMM_M	isc_dsql_execute_immediate_m
#define GDS_DSQL_EXEC_IMMED		isc_dsql_exec_immediate
#define GDS_DSQL_EXEC_IMMED2	isc_dsql_exec_immed2
#define GDS_DSQL_EXEC_IMM_M		isc_dsql_exec_immediate_m
#define GDS_DSQL_EXEC_IMM2_M	isc_dsql_exec_immed2_m
#define GDS_DSQL_EXEC_IMM3_M    isc_dsql_exec_immed3_m
#define GDS_DSQL_FETCH			isc_dsql_fetch
#define GDS_DSQL_FETCH2			isc_dsql_fetch2
#define GDS_DSQL_FETCH_M		isc_dsql_fetch_m
#define GDS_DSQL_FETCH2_M		isc_dsql_fetch2_m
#define GDS_DSQL_FREE			isc_dsql_free_statement
#define GDS_DSQL_INSERT			isc_dsql_insert
#define GDS_DSQL_INSERT_M		isc_dsql_insert_m
#define GDS_DSQL_PREPARE		isc_dsql_prepare
#define GDS_DSQL_PREPARE_M		isc_dsql_prepare_m
#define GDS_DSQL_SET_CURSOR		isc_dsql_set_cursor_name
#define GDS_DSQL_SQL_INFO		isc_dsql_sql_info

#define GDS_SERVICE_ATTACH		isc_service_attach
#define GDS_SERVICE_DETACH		isc_service_detach
#define GDS_SERVICE_QUERY		isc_service_query
#define GDS_SERVICE_START		isc_service_start

/*****************************************************
 *  IMPORTANT IMPORTANT IMPORTANT IMPORTANT IMPORTANT
 *
 *  The order in which these defines appear MUST match
 *  the order in which the entrypoint appears in
 *  source/jrd/entry.h.  Failure to do so will result in
 *  much frustration
 ******************************************************/

const int PROC_ATTACH_DATABASE	= 0;
const int PROC_BLOB_INFO		= 1;
const int PROC_CANCEL_BLOB		= 2;
const int PROC_CLOSE_BLOB		= 3;
const int PROC_COMMIT			= 4;
const int PROC_COMPILE			= 5;
const int PROC_CREATE_BLOB		= 6;
const int PROC_CREATE_DATABASE	= 7;
const int PROC_DATABASE_INFO	= 8;
const int PROC_DETACH			= 9;
const int PROC_GET_SEGMENT		= 10;
const int PROC_OPEN_BLOB		= 11;
const int PROC_PREPARE			= 12;
const int PROC_PUT_SEGMENT		= 13;
const int PROC_RECONNECT		= 14;
const int PROC_RECEIVE			= 15;
const int PROC_RELEASE_REQUEST	= 16;
const int PROC_REQUEST_INFO		= 17;
const int PROC_ROLLBACK			= 18;
const int PROC_SEND				= 19;
const int PROC_START_AND_SEND	= 20;
const int PROC_START			= 21;
const int PROC_START_MULTIPLE	= 22;
const int PROC_START_TRANSACTION= 23;
const int PROC_TRANSACTION_INFO	= 24;
const int PROC_UNWIND			= 25;
const int PROC_COMMIT_RETAINING	= 26;
const int PROC_QUE_EVENTS		= 27;
const int PROC_CANCEL_EVENTS	= 28;
const int PROC_DDL				= 29;
const int PROC_OPEN_BLOB2		= 30;
const int PROC_CREATE_BLOB2		= 31;
const int PROC_GET_SLICE		= 32;
const int PROC_PUT_SLICE		= 33;
const int PROC_SEEK_BLOB		= 34;
const int PROC_TRANSACT_REQUEST	= 35;
const int PROC_DROP_DATABASE	= 36;

const int PROC_DSQL_ALLOCATE	= 37;
const int PROC_DSQL_EXECUTE		= 38;
const int PROC_DSQL_EXECUTE2	= 39;
const int PROC_DSQL_EXEC_IMMED	= 40;
const int PROC_DSQL_EXEC_IMMED2	= 41;
const int PROC_DSQL_FETCH		= 42;
const int PROC_DSQL_FREE		= 43;
const int PROC_DSQL_INSERT		= 44;
const int PROC_DSQL_PREPARE		= 45;
const int PROC_DSQL_SET_CURSOR	= 46;
const int PROC_DSQL_SQL_INFO	= 47;

const int PROC_SERVICE_ATTACH	= 48;
const int PROC_SERVICE_DETACH	= 49;
const int PROC_SERVICE_QUERY	= 50;
const int PROC_SERVICE_START	= 51;

const int PROC_ROLLBACK_RETAINING	= 52;
const int PROC_CANCEL_OPERATION	= 53;

const int PROC_SHUTDOWN			= 54;

const int PROC_count			= 55;

/* Define complicated table for multi-subsystem world */

extern "C" {

static ISC_STATUS no_entrypoint(ISC_STATUS * user_status, ...);

#ifdef SUPERCLIENT
#define ENTRYPOINT(cur, rem)	ISC_STATUS rem(ISC_STATUS* user_status, ...);
#else
#define ENTRYPOINT(cur, rem)	ISC_STATUS rem(ISC_STATUS* user_status, ...), cur(ISC_STATUS* user_status, ...);
#endif

#include "../jrd/entry.h"

#define SUBSYSTEMS 2

static PTR entrypoints[PROC_count * SUBSYSTEMS] =
{
#define ENTRYPOINT(cur, rem)	rem,
#include "../jrd/entry.h"

#if !defined(SUPERCLIENT)
#define ENTRYPOINT(cur, rem)	cur,
#include "../jrd/entry.h"
#endif
};

} // extern "C"

/* Information items for two phase commit */

static const UCHAR prepare_tr_info[] =
{
	isc_info_tra_id,
	isc_info_end
};

/* Information items for DSQL prepare */

static const SCHAR sql_prepare_info[] =
{
	isc_info_sql_select,
	isc_info_sql_describe_vars,
	isc_info_sql_sqlda_seq,
	isc_info_sql_type,
	isc_info_sql_sub_type,
	isc_info_sql_scale,
	isc_info_sql_length,
	isc_info_sql_field,
	isc_info_sql_relation,
	isc_info_sql_owner,
	isc_info_sql_alias,
	isc_info_sql_describe_end
};

/* Information items for SQL info */

static const SCHAR describe_select_info[] =
{
	isc_info_sql_select,
	isc_info_sql_describe_vars,
	isc_info_sql_sqlda_seq,
	isc_info_sql_type,
	isc_info_sql_sub_type,
	isc_info_sql_scale,
	isc_info_sql_length,
	isc_info_sql_field,
	isc_info_sql_relation,
	isc_info_sql_owner,
	isc_info_sql_alias,
	isc_info_sql_describe_end
};

static const SCHAR describe_bind_info[] =
{
	isc_info_sql_bind,
	isc_info_sql_describe_vars,
	isc_info_sql_sqlda_seq,
	isc_info_sql_type,
	isc_info_sql_sub_type,
	isc_info_sql_scale,
	isc_info_sql_length,
	isc_info_sql_field,
	isc_info_sql_relation,
	isc_info_sql_owner,
	isc_info_sql_alias,
	isc_info_sql_describe_end
};

static const SCHAR sql_prepare_info2[] = 
{
	isc_info_sql_stmt_type,

	// describe_select_info
	isc_info_sql_select,
	isc_info_sql_describe_vars,
	isc_info_sql_sqlda_seq,
	isc_info_sql_type,
	isc_info_sql_sub_type,
	isc_info_sql_scale,
	isc_info_sql_length,
	isc_info_sql_field,
	isc_info_sql_relation,
	isc_info_sql_owner,
	isc_info_sql_alias,
	isc_info_sql_describe_end,

	// describe_bind_info
	isc_info_sql_bind,
	isc_info_sql_describe_vars,
	isc_info_sql_sqlda_seq,
	isc_info_sql_type,
	isc_info_sql_sub_type,
	isc_info_sql_scale,
	isc_info_sql_length,
	isc_info_sql_field,
	isc_info_sql_relation,
	isc_info_sql_owner,
	isc_info_sql_alias,
	isc_info_sql_describe_end
};


ISC_STATUS API_ROUTINE GDS_ATTACH_DATABASE(ISC_STATUS*	user_status,
										   SSHORT	file_length,
										   const TEXT*	file_name,
										   FB_API_HANDLE*	public_handle,
										   SSHORT	dpb_length,
										   const SCHAR*	dpb)
{
/**************************************
 *
 *	g d s _ $ a t t a c h _ d a t a b a s e
 *
 **************************************
 *
 * Functional description
 *	Attach a database through the first subsystem
 *	that recognizes it.
 *
 **************************************/
	ISC_STATUS *ptr;
	ISC_STATUS_ARRAY temp;
	StoredAtt* handle = 0;
	Attachment* attachment = 0;
	USHORT n;

	YEntry status(user_status);

	try 
	{
		nullCheck(public_handle, isc_bad_db_handle);

		if (!file_name)
		{
			Firebird::status_exception::raise(isc_bad_db_format,
											  isc_arg_string,
											  "",
											  isc_arg_end);
		}

		if (dpb_length > 0 && !dpb)
		{
			Firebird::status_exception::raise(isc_bad_dpb_form,
											  isc_arg_end);
		}

#if !defined (SUPERCLIENT)
		if (shutdown_flag)
		{
			Firebird::status_exception::raise(isc_shutwarn,
											  isc_arg_end);
		}
#endif /* !SUPERCLIENT */

		SUBSYSTEM_USAGE_INCR;

		ptr = status;

/* copy the file name to a temp buffer, since some of the following
   utilities can modify it */

		Firebird::PathName temp_filename(file_name, 
			file_length ? file_length : strlen(file_name));
		temp_filename.rtrim();
		file_length = temp_filename.length();
		Firebird::PathName expanded_filename;

		if (!ISC_check_if_remote(temp_filename, true))
		{
			Firebird::PathName database_filename;
			if (ResolveDatabaseAlias(temp_filename, database_filename))
			{
				ISC_expand_filename(database_filename, false);
				expanded_filename = database_filename;
			}
			else if (set_path(temp_filename, expanded_filename))
			{
				temp_filename = expanded_filename;
				file_length = temp_filename.length();
			}
			else
			{
				expanded_filename = temp_filename;
				ISC_expand_filename(expanded_filename, true);
			}
		}
		else
		{
			expanded_filename = temp_filename;
			ISC_expand_filename(expanded_filename, true);
		}

		Firebird::ClumpletWriter newDpb(Firebird::ClumpletReader::Tagged, MAX_DPB_SIZE, 
			reinterpret_cast<const UCHAR*>(dpb), dpb_length, isc_dpb_version1);

		setLogin(newDpb);

		for (n = 0; n < SUBSYSTEMS; n++)
		{
			if (!CALL(PROC_ATTACH_DATABASE, n) (ptr, temp_filename.length(), 
												temp_filename.c_str(),
												&handle, newDpb.getBufferLength(), 
												reinterpret_cast<const char*>(newDpb.getBuffer()),
												expanded_filename.c_str()))
			{
				attachment = new Attachment(handle, public_handle, n);
				attachment->db_path = expanded_filename;

				status[0] = isc_arg_gds;
				status[1] = 0;

				/* Check to make sure that status[2] is not a warning.  If it is, then
				 * the rest of the vector should be untouched.  If it is not, then make
				 * this element isc_arg_end
				 *
				 * This cleanup is done to remove any erroneous errors from trying multiple
				 * protocols for a database attach
				 */
				if (status[2] != isc_arg_warning) {
					status[2] = isc_arg_end;
				}

				return status[1];
			}
			if (ptr[1] != isc_unavailable) 
			{
				ptr = temp;
			}
		}
	}
	catch(const Firebird::Exception& e)
	{
		if (handle)
		{
			CALL(PROC_DETACH, n) (temp, handle);
		}
		if (attachment)
		{
			delete attachment;
		}

  		e.stuff_exception(status);
	}

	SUBSYSTEM_USAGE_DECR;
	return status[1];
}


ISC_STATUS API_ROUTINE GDS_BLOB_INFO(ISC_STATUS*	user_status,
									 FB_API_HANDLE*		blob_handle,
									 SSHORT		item_length,
									 const SCHAR*		items,
									 SSHORT		buffer_length,
									 SCHAR*		buffer)
{
/**************************************
 *
 *	g d s _ $ b l o b _ i n f o
 *
 **************************************
 *
 * Functional description
 *	Provide information on blob object.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Blob* blob = translate<Blob>(blob_handle);
		status.setPrimaryHandle(blob);
		CALL(PROC_BLOB_INFO, blob->implementation) (status, &blob->handle, 
													item_length, items,
													buffer_length, buffer);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_CANCEL_BLOB(ISC_STATUS * user_status,
									   FB_API_HANDLE * blob_handle)
{
/**************************************
 *
 *	g d s _ $ c a n c e l _ b l o b
 *
 **************************************
 *
 * Functional description
 *	Abort a partially completed blob.
 *
 **************************************/
	if (!*blob_handle) 
	{
		if (user_status) 
		{
			user_status[0] = isc_arg_gds;
			user_status[1] = 0;
			user_status[2] = isc_arg_end;
		}
		return FB_SUCCESS;
	}

	YEntry status(user_status);

	try 
	{
		Blob* blob = translate<Blob>(blob_handle);
		status.setPrimaryHandle(blob);

		if (! CALL(PROC_CANCEL_BLOB, blob->implementation) (status, &blob->handle))
		{
			status.setPrimaryHandle(0);
			delete blob;
			*blob_handle = 0;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_CANCEL_EVENTS(ISC_STATUS * user_status,
										 FB_API_HANDLE * handle,
										 SLONG * id)
{
/**************************************
 *
 *	g d s _ $ c a n c e l _ e v e n t s
 *
 **************************************
 *
 * Functional description
 *	Try to cancel an event.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Attachment* attachment = translate<Attachment>(handle);
		status.setPrimaryHandle(attachment);
		CALL(PROC_CANCEL_EVENTS, attachment->implementation) (status,
															&attachment->handle,
															id);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_CANCEL_OPERATION(ISC_STATUS * user_status,
											FB_API_HANDLE * handle,
											USHORT option)
{
/**************************************
 *
 *	g d s _ $ c a n c e l _ o p e r a t i o n
 *
 **************************************
 *
 * Functional description
 *	Try to cancel an operation.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Attachment* attachment = translate<Attachment>(handle);
		// mutex will be locked here for a really long time
		Firebird::MutexLockGuard guard(attachment->enterMutex);	
		if (attachment->enterCount)
		{
			CALL(PROC_CANCEL_OPERATION, attachment->implementation) (status,
																	 &attachment->handle,
																	 option);
		}
		else
		{
			Firebird::status_exception::raise(isc_random, isc_arg_string, "Nothing to cancel", 0);
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_CLOSE_BLOB(ISC_STATUS * user_status,
									  FB_API_HANDLE * blob_handle)
{
/**************************************
 *
 *	g d s _ $ c l o s e _ b l o b
 *
 **************************************
 *
 * Functional description
 *	Abort a partially completed blob.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Blob* blob = translate<Blob>(blob_handle);
		status.setPrimaryHandle(blob);

		CALL(PROC_CLOSE_BLOB, blob->implementation) (status, &blob->handle);
		status.setPrimaryHandle(0);
		delete blob;
		*blob_handle = 0;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_COMMIT(ISC_STATUS * user_status,
								  FB_API_HANDLE * tra_handle)
{
/**************************************
 *
 *	g d s _ $ c o m m i t
 *
 **************************************
 *
 * Functional description
 *	Commit a transaction.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Transaction* transaction = translate<Transaction>(tra_handle);
		Transaction* sub;
		status.setPrimaryHandle(transaction);

		if (transaction->implementation != SUBSYSTEMS) {
			// Handle single transaction case
			if (CALL(PROC_COMMIT, transaction->implementation) (status,
															&transaction->
															handle))
			{
				return status[1];
			}
		}
		else {
			// Handle two phase commit.  Start by putting everybody into
			// limbo.  If anybody fails, punt
			if (!(transaction->flags & HANDLE_TRANSACTION_limbo))
			{
				if (prepare(status, transaction))
				{
					return status[1];
				}
			}

			// Everybody is in limbo, now commit everybody.
			// In theory, this can't fail

			for (sub = transaction->next; sub; sub = sub->next)
			{
				if (CALL(PROC_COMMIT, sub->implementation) (status, &sub->handle))
				{
					return status[1];
				}
			}
		}

		status.setPrimaryHandle(0);
		while (sub = transaction) {
			transaction = sub->next;
			delete sub;
		}
		*tra_handle = 0;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_COMMIT_RETAINING(ISC_STATUS * user_status,
											FB_API_HANDLE * tra_handle)
{
/**************************************
 *
 *	g d s _ $ c o m m i t _ r e t a i n i n g
 *
 **************************************
 *
 * Functional description
 *	Do a commit retaining.
 *
 * N.B., the transaction cleanup handlers are NOT
 * called here since, conceptually, the transaction
 * is still running.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Transaction* transaction = translate<Transaction>(tra_handle);
		status.setPrimaryHandle(transaction);

		for (Transaction* sub = transaction; sub; sub = sub->next)
		{
			if (sub->implementation != SUBSYSTEMS &&
				CALL(PROC_COMMIT_RETAINING, sub->implementation) (status,
																  &sub->handle))
			{
				return status[1];
			}
		}

		transaction->flags |= HANDLE_TRANSACTION_limbo;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_COMPILE(ISC_STATUS* user_status,
								   FB_API_HANDLE* db_handle,
								   FB_API_HANDLE* req_handle,
								   USHORT blr_length,
								   const SCHAR* blr)
{
/**************************************
 *
 *	g d s _ $ c o m p i l e
 *
 **************************************
 *
 * Functional description
 *
 **************************************/
	YEntry status(user_status);
	Attachment* dbb = NULL;
	StoredReq* rq = NULL;
	try 
	{
		dbb = translate<Attachment>(db_handle);
		status.setPrimaryHandle(dbb);
		nullCheck(req_handle, isc_bad_req_handle);

		if (CALL(PROC_COMPILE, dbb->implementation) (status, &dbb->handle,
													 &rq, blr_length,
													 blr))
		{
			return status[1];
		}

		new Request(rq, req_handle, dbb);
	}
	catch (const Firebird::Exception& e)
	{
		if (dbb && rq)
		{
			*req_handle = 0;
			CALL(PROC_RELEASE_REQUEST, dbb->implementation) (status, rq);
		}
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_COMPILE2(ISC_STATUS* user_status,
									FB_API_HANDLE* db_handle,
									FB_API_HANDLE* req_handle,
									USHORT blr_length,
									const SCHAR* blr)
{
/**************************************
 *
 *	g d s _ $ c o m p i l e 2
 *
 **************************************
 *
 * Functional description
 *
 **************************************/
	Status status(user_status);

	try 
	{
		if (GDS_COMPILE(status, db_handle, req_handle, blr_length, blr))
		{
			return status[1];
		}

		Request *request = translate<Request>(req_handle);
		request->user_handle = req_handle;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_CREATE_BLOB(ISC_STATUS* user_status,
									   FB_API_HANDLE* db_handle,
									   FB_API_HANDLE* tra_handle,
									   FB_API_HANDLE* blob_handle,
									   SLONG* blob_id)
{
/**************************************
 *
 *	g d s _ $ c r e a t e _ b l o b
 *
 **************************************
 *
 * Functional description
 *	Open an existing blob.
 *
 **************************************/

	return open_blob(user_status, db_handle, tra_handle, blob_handle, blob_id,
					 0, 0, PROC_CREATE_BLOB, PROC_CREATE_BLOB2);
}


ISC_STATUS API_ROUTINE GDS_CREATE_BLOB2(ISC_STATUS* user_status,
										FB_API_HANDLE* db_handle,
										FB_API_HANDLE* tra_handle,
										FB_API_HANDLE* blob_handle,
										SLONG* blob_id,
										SSHORT bpb_length,
										const UCHAR* bpb)
{
/**************************************
 *
 *	g d s _ $ c r e a t e _ b l o b 2
 *
 **************************************
 *
 * Functional description
 *	Open an existing blob.
 *
 **************************************/

	return open_blob(user_status, db_handle, tra_handle, blob_handle, blob_id,
					 bpb_length, bpb, PROC_CREATE_BLOB,
					 PROC_CREATE_BLOB2);
}



ISC_STATUS API_ROUTINE GDS_CREATE_DATABASE(ISC_STATUS* user_status,
										   USHORT file_length,
										   const TEXT* file_name,
										   FB_API_HANDLE* public_handle,
										   SSHORT dpb_length,
										   const UCHAR* dpb,
										   USHORT db_type)
{
/**************************************
 *
 *	g d s _ $ c r e a t e _ d a t a b a s e
 *
 **************************************
 *
 * Functional description
 *	Create a nice, squeaky clean database, uncorrupted by user data.
 *
 **************************************/
	ISC_STATUS *ptr;
	ISC_STATUS_ARRAY temp;
	StoredAtt* handle = 0;
	Attachment* attachment = 0;
	USHORT n;

	YEntry status(user_status);

	try 
	{
		nullCheck(public_handle, isc_bad_db_handle);

		if (!file_name)
		{
			Firebird::status_exception::raise(isc_bad_db_format,
											  isc_arg_string,
											  "",
											  isc_arg_end);
		}

		if (dpb_length > 0 && !dpb)
		{
			Firebird::status_exception::raise(isc_bad_dpb_form,
											  isc_arg_end);
		}

#if !defined (SUPERCLIENT)
		if (shutdown_flag)
		{
			Firebird::status_exception::raise(isc_shutwarn,
											  isc_arg_end);
		}
#endif /* !SUPERCLIENT */

		SUBSYSTEM_USAGE_INCR;

		ptr = status;

/* copy the file name to a temp buffer, since some of the following
   utilities can modify it */

		Firebird::PathName temp_filename(file_name, 
			file_length ? file_length : strlen(file_name));
		temp_filename.rtrim();
		file_length = temp_filename.length();
		Firebird::PathName expanded_filename;

		if (!ISC_check_if_remote(temp_filename, true))
		{
			Firebird::PathName database_filename;
			if (ResolveDatabaseAlias(temp_filename, database_filename))
			{
				ISC_expand_filename(database_filename, false);
				expanded_filename = database_filename;
			}
			else if (set_path(temp_filename, expanded_filename))
			{
				temp_filename = expanded_filename;
			}
			else
			{
				expanded_filename = temp_filename;
				ISC_expand_filename(expanded_filename, true);
			}
		}
		else
		{
			expanded_filename = temp_filename;
			ISC_expand_filename(expanded_filename, true);
		}

		Firebird::ClumpletWriter newDpb(Firebird::ClumpletReader::Tagged, MAX_DPB_SIZE, 
				reinterpret_cast<const UCHAR*>(dpb), dpb_length, isc_dpb_version1);

		setLogin(newDpb);

		for (n = 0; n < SUBSYSTEMS; n++)
		{
			if (!CALL(PROC_CREATE_DATABASE, n) (ptr, temp_filename.length(), 
												temp_filename.c_str(),
												&handle, newDpb.getBufferLength(), 
												reinterpret_cast<const char*>(newDpb.getBuffer()), 
												0, expanded_filename.c_str()))
			{
#ifdef WIN_NT
            	/* Now we can expand, the file exists. */
				expanded_filename = temp_filename;
	            ISC_expand_filename (expanded_filename, true);
#endif

				attachment = new Attachment(handle, public_handle, n);
#ifdef WIN_NT
				attachment->db_path = expanded_filename;
#else
				attachment->db_path = temp_filename;
#endif

				status[0] = isc_arg_gds;
				status[1] = 0;
				if (status[2] != isc_arg_warning)
					status[2] = isc_arg_end;

				return status[1];
			}
			if (ptr[1] != isc_unavailable)
				ptr = temp;
		}
	}
	catch(const Firebird::Exception& e)
	{
  		e.stuff_exception(status);
		if (handle)
		{
			CALL(PROC_DROP_DATABASE, n) (temp, handle);
		}
		if (attachment)
		{
			delete attachment;
		}
	}

	SUBSYSTEM_USAGE_DECR;
	return status[1];
}


ISC_STATUS API_ROUTINE isc_database_cleanup(ISC_STATUS * user_status,
											 FB_API_HANDLE * handle,
											 AttachmentCleanupRoutine * routine,
											 void* arg)
{
/**************************************
 *
 *	g d s _ $ d a t a b a s e _ c l e a n u p
 *
 **************************************
 *
 * Functional description
 *	Register an attachment specific cleanup handler.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Attachment* attachment = translate<Attachment>(handle);
		status.setPrimaryHandle(attachment);

		attachment->cleanup.add(routine, arg);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DATABASE_INFO(ISC_STATUS* user_status,
										 FB_API_HANDLE* handle,
										 SSHORT item_length,
										 const SCHAR* items,
										 SSHORT buffer_length,
										 SCHAR* buffer)
{
/**************************************
 *
 *	g d s _ $ d a t a b a s e _ i n f o
 *
 **************************************
 *
 * Functional description
 *	Provide information on database object.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Attachment* attachment = translate<Attachment>(handle);
		status.setPrimaryHandle(attachment);
		CALL(PROC_DATABASE_INFO, attachment->implementation) (status,
															&attachment->handle,
															item_length,
															items,
															buffer_length,
															buffer);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DDL(ISC_STATUS* user_status,
							   FB_API_HANDLE* db_handle,
							   FB_API_HANDLE* tra_handle,
							   SSHORT length,
							   const UCHAR* ddl)
{
/**************************************
 *
 *	g d s _ $ d d l
 *
 **************************************
 *
 * Functional description
 *	Do meta-data update.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Attachment* attachment = translate<Attachment>(db_handle);
		status.setPrimaryHandle(attachment);
		Transaction* transaction = findTransaction(tra_handle, attachment);

		CALL(PROC_DDL, attachment->implementation) (status,
												  &attachment->handle,
												  &transaction->handle,
												  length, ddl);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DETACH(ISC_STATUS * user_status,
								  FB_API_HANDLE * handle)
{
/**************************************
 *
 *	g d s _ d e t a c h
 *
 **************************************
 *
 * Functional description
 *	Close down an attachment.
 *
 **************************************/
	return detach_or_drop_database(user_status, handle, PROC_DETACH);
}


static ISC_STATUS detach_or_drop_database(ISC_STATUS * user_status, FB_API_HANDLE * handle,
										  const int proc, const ISC_STATUS specCode)
{
/**************************************
 *
 *	d e t a c h _ o r _ d r o p _ d a t a b a s e 
 *
 **************************************
 *
 * Functional description
 *	Common code for that calls.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Attachment* dbb = translate<Attachment>(handle);
		{ // guard scope
			Firebird::MutexLockGuard guard(dbb->mutex);
			size_t i;

#if defined(SUPERSERVER) && !defined(EMBEDDED)

			// Drop all DSQL statements to reclaim DSQL memory pools.
			while ((i = dbb->statements.getCount()))
			{
				FB_API_HANDLE temp_handle;
				Statement* statement = dbb->statements[i - 1];
				if (!statement->user_handle) {
					temp_handle = statement->public_handle;
					statement->user_handle = &temp_handle;
				}

				subsystem_exit();
				ISC_STATUS rc = GDS_DSQL_FREE(status, statement->user_handle, DSQL_drop);
				subsystem_enter();

				if (rc)
				{
					return status[1];
				}
			}
#endif

			if (CALL(proc, dbb->implementation) (status, &dbb->handle) && 
			    status[1] != specCode)
			{
				return status[1];
			}

			// Release associated handles

			while ((i = dbb->requests.getCount()))
			{
				delete dbb->requests[i - 1];
			}

#ifndef SUPERSERVER
			while ((i = dbb->statements.getCount()))
			{
				release_dsql_support(dbb->statements[i - 1]->das);
				delete dbb->statements[i - 1];
			}
#endif

			while ((i = dbb->blobs.getCount()))
			{
				delete dbb->blobs[i - 1];
			}

			SUBSYSTEM_USAGE_DECR;
		}

		delete dbb;
		*handle = 0;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}

int API_ROUTINE gds__disable_subsystem(TEXT * subsystem)
{
/**************************************
 *
 *	g d s _ $ d i s a b l e _ s u b s y s t e m 
 *
 **************************************
 *
 * Functional description
 *	Disable access to a specific subsystem.  If no subsystem
 *	has been explicitly disabled, all are available.
 *
 **************************************/
	return FALSE;
}


ISC_STATUS API_ROUTINE GDS_DROP_DATABASE(ISC_STATUS * user_status,
										 FB_API_HANDLE * handle)
{
/**************************************
 *
 *	i s c _ d r o p _ d a t a b a s e
 *
 **************************************
 *
 * Functional description
 *	Close down a database and then purge it.
 *
 **************************************/
	return detach_or_drop_database(user_status, handle, PROC_DROP_DATABASE, isc_drdb_completed_with_errs);
}


ISC_STATUS API_ROUTINE GDS_DSQL_ALLOC(ISC_STATUS * user_status,
									  FB_API_HANDLE * db_handle,
									  FB_API_HANDLE * stmt_handle)
{
/**************************************
 *
 *	i s c _ d s q l _ a l l o c _ s t a t e m e n t
 *
 **************************************/

	return GDS_DSQL_ALLOCATE(user_status, db_handle, stmt_handle);
}


ISC_STATUS API_ROUTINE GDS_DSQL_ALLOC2(ISC_STATUS * user_status,
									   FB_API_HANDLE * db_handle,
									   FB_API_HANDLE * stmt_handle)
{
/**************************************
 *
 *	i s c _ d s q l _ a l l o c _ s t a t e m e n t 2
 *
 **************************************
 *
 * Functional description
 *	Allocate a statement handle.
 *
 **************************************/
	Status status(user_status);

	try 
	{
		if (GDS_DSQL_ALLOCATE(status, db_handle, stmt_handle))
		{
			return status[1];
		}

		Statement *statement = translate<Statement>(stmt_handle);
		statement->user_handle = stmt_handle;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_ALLOCATE(ISC_STATUS * user_status,
										 FB_API_HANDLE * db_handle,
										 FB_API_HANDLE * public_stmt_handle)
{
/**************************************
 *
 *	i s c _ d s q l _ a l l o c a t e _ s t a t e m e n t
 *
 **************************************
 *
 * Functional description
 *	Allocate a statement handle.
 *
 **************************************/
	YEntry status(user_status);
	Attachment* dbb = NULL;
	StoredStm* stmt_handle = NULL;

	try 
	{
		dbb = translate<Attachment>(db_handle);
		status.setPrimaryHandle(dbb);
		// check the statement handle to make sure it's NULL and then initialize it.
		nullCheck(public_stmt_handle, isc_bad_stmt_handle);

		if (CALL(PROC_DSQL_ALLOCATE, dbb->implementation) (status, &dbb->handle, &stmt_handle))
		{
			return status[1];
		}

		Statement* statement = new Statement(stmt_handle, public_stmt_handle, dbb);
	}
	catch (const Firebird::Exception& e)
	{
		if (dbb && stmt_handle)
		{
			*public_stmt_handle = 0;
			CALL(PROC_DSQL_FREE, dbb->implementation) (status, &stmt_handle, DSQL_drop);
		}
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE isc_dsql_describe(ISC_STATUS * user_status,
										 FB_API_HANDLE * stmt_handle,
										 USHORT dialect,
										 XSQLDA * sqlda)
{
/**************************************
 *
 *	i s c _ d s q l _ d e s c r i b e
 *
 **************************************
 *
 * Functional description
 *	Describe output parameters for a prepared statement.
 *
 **************************************/
	Status status(user_status);

	try 
	{
		Statement* statement = translate<Statement>(stmt_handle);

		statement->checkPrepared();
		sqlda_sup::dasup_clause& clause = 
			statement->das.dasup_clauses[DASUP_CLAUSE_select];

		if (clause.dasup_info_len && clause.dasup_info_buf)
		{
			iterative_sql_info(	status,
								stmt_handle,
								sizeof(describe_select_info),
								describe_select_info,
								clause.dasup_info_len,
								clause.dasup_info_buf,
								dialect,
								sqlda);
		}
		else
		{
			Firebird::HalfStaticArray<SCHAR, DESCRIBE_BUFFER_SIZE> local_buffer;
			USHORT buffer_len = sqlda_buffer_size(DESCRIBE_BUFFER_SIZE, sqlda, dialect);
			SCHAR *buffer = local_buffer.getBuffer(buffer_len);

			if (!GDS_DSQL_SQL_INFO(	status,
									stmt_handle,
									sizeof(describe_select_info),
									describe_select_info,
									buffer_len,
									buffer))
			{
				iterative_sql_info(	status,
									stmt_handle,
									sizeof(describe_select_info),
									describe_select_info,
									buffer_len,
									buffer,
									dialect,
									sqlda);
			}
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE isc_dsql_describe_bind(ISC_STATUS * user_status,
											  FB_API_HANDLE * stmt_handle,
											  USHORT dialect,
											  XSQLDA * sqlda)
{
/**************************************
 *
 *	i s c _ d s q l _ d e s c r i b e _ b i n d
 *
 **************************************
 *
 * Functional description
 *	Describe input parameters for a prepared statement.
 *
 **************************************/
	Status status(user_status);

	try 
	{
		Statement* statement = translate<Statement>(stmt_handle);

		sqlda_sup::dasup_clause& clause = 
			statement->das.dasup_clauses[DASUP_CLAUSE_bind];

		if (clause.dasup_info_len && clause.dasup_info_buf)
		{
			iterative_sql_info(	status,
								stmt_handle,
								sizeof(describe_bind_info),
								describe_bind_info,
								clause.dasup_info_len,
								clause.dasup_info_buf,
								dialect,
								sqlda);
		}
		else
		{
			Firebird::HalfStaticArray<SCHAR, DESCRIBE_BUFFER_SIZE> local_buffer;
			USHORT buffer_len = sqlda_buffer_size(DESCRIBE_BUFFER_SIZE, sqlda, dialect);
			SCHAR *buffer = local_buffer.getBuffer(buffer_len);

			if (!GDS_DSQL_SQL_INFO(	status,
									stmt_handle,
									sizeof(describe_bind_info),
									describe_bind_info,
									buffer_len,
									buffer))
			{
				iterative_sql_info(	status,
									stmt_handle,
									sizeof(describe_bind_info),
									describe_bind_info,
									buffer_len,
									buffer,
									dialect,
									sqlda);
			}
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXECUTE(ISC_STATUS* user_status,
										FB_API_HANDLE* tra_handle,
										FB_API_HANDLE* stmt_handle,
										USHORT dialect,
										XSQLDA* sqlda)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c u t e
 *
 **************************************
 *
 * Functional description
 *	Execute a non-SELECT dynamic SQL statement.
 *
 **************************************/

	return GDS_DSQL_EXECUTE2(user_status, tra_handle, stmt_handle, dialect,
							 sqlda, NULL);
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXECUTE2(ISC_STATUS* user_status,
										 FB_API_HANDLE* tra_handle,
										 FB_API_HANDLE* stmt_handle,
										 USHORT dialect,
										 XSQLDA* in_sqlda,
										 XSQLDA* out_sqlda)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c u t e 2
 *
 **************************************
 *
 * Functional description
 *	Execute a non-SELECT dynamic SQL statement.
 *
 **************************************/
	Status status(user_status);

	try 
	{
		USHORT in_blr_length, in_msg_type, in_msg_length,
			out_blr_length, out_msg_type, out_msg_length;

		Statement* statement = translate<Statement>(stmt_handle);

		sqlda_sup* dasup = &(statement->das);
		statement->checkPrepared();

		if (UTLD_parse_sqlda(status, dasup, &in_blr_length, &in_msg_type,
							 &in_msg_length, dialect, in_sqlda,
							 DASUP_CLAUSE_bind))
		{
			return status[1];
		}
	
		if (UTLD_parse_sqlda
			(status, dasup, &out_blr_length, &out_msg_type, &out_msg_length,
			dialect, out_sqlda, DASUP_CLAUSE_select))
		{
			return status[1];
		}

		if (GDS_DSQL_EXECUTE2_M(status, tra_handle, stmt_handle,
								in_blr_length,
								dasup->dasup_clauses[DASUP_CLAUSE_bind].dasup_blr,
								in_msg_type, in_msg_length,
								dasup->dasup_clauses[DASUP_CLAUSE_bind].dasup_msg,
								out_blr_length,
								dasup->dasup_clauses[DASUP_CLAUSE_select].
								dasup_blr, out_msg_type, out_msg_length,
								dasup->dasup_clauses[DASUP_CLAUSE_select].
								dasup_msg))
		{
			return status[1];
		}

		if (UTLD_parse_sqlda(status, dasup, NULL, NULL, NULL,
							 dialect, out_sqlda, DASUP_CLAUSE_select))
		{
			return status[1];
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXECUTE_M(ISC_STATUS* user_status,
										  FB_API_HANDLE* tra_handle,
										  FB_API_HANDLE* stmt_handle,
										  USHORT blr_length,
										  const SCHAR* blr,
										  USHORT msg_type,
										  USHORT msg_length,
										  SCHAR* msg)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c u t e _ m
 *
 **************************************
 *
 * Functional description
 *	Execute a non-SELECT dynamic SQL statement.
 *
 **************************************/

	return GDS_DSQL_EXECUTE2_M(user_status, tra_handle, stmt_handle,
							   blr_length, blr, msg_type, msg_length, msg,
							   0, NULL, 0, 0, NULL);
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXECUTE2_M(ISC_STATUS* user_status,
										   FB_API_HANDLE* tra_handle,
										   FB_API_HANDLE* stmt_handle,
										   USHORT in_blr_length,
										   const SCHAR* in_blr,
										   USHORT in_msg_type,
										   USHORT in_msg_length,
										   SCHAR* in_msg,
										   USHORT out_blr_length,
										   SCHAR* out_blr,
										   USHORT out_msg_type,
										   USHORT out_msg_length, 
										   SCHAR* out_msg)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c u t e 2 _ m
 *
 **************************************
 *
 * Functional description
 *	Execute a non-SELECT dynamic SQL statement.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Statement* statement = translate<Statement>(stmt_handle);
		status.setPrimaryHandle(statement);

		Transaction* transaction = NULL;
		StoredTra* handle = NULL;

		if (tra_handle && *tra_handle)
		{
			transaction = translate<Transaction>(tra_handle);
			Transaction* t = find_transaction(statement->parent, transaction);
			if (!t)
			{
				bad_handle(isc_bad_trans_handle);
			}
			handle = t->handle;
		}

		CALL(PROC_DSQL_EXECUTE2, statement->implementation) (status,
														     &handle,
														     &statement->handle,
														     in_blr_length, in_blr,
														     in_msg_type, in_msg_length, in_msg,
														     out_blr_length, out_blr,
														     out_msg_type, out_msg_length, out_msg);

		if (!status[1])
		{
			if (transaction && !handle)
			{
				delete transaction;
				*tra_handle = 0;
			}
			else if (!transaction && handle)
			{
				transaction = new Transaction(handle, tra_handle, statement->parent);
			}
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXEC_IMMED(ISC_STATUS* user_status,
										   FB_API_HANDLE* db_handle,
										   FB_API_HANDLE* tra_handle,
										   USHORT length,
										   const SCHAR* string,
										   USHORT dialect,
										   XSQLDA* sqlda)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c _ i m m e d
 *
 **************************************
 *
 * Functional description
 *
 **************************************/

	return GDS_DSQL_EXECUTE_IMMED(user_status,
								  db_handle, tra_handle, length, string,
								  dialect, sqlda);
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXECUTE_IMMED(ISC_STATUS* user_status,
											  FB_API_HANDLE* db_handle,
											  FB_API_HANDLE* tra_handle,
											  USHORT length,
											  const SCHAR* string,
											  USHORT dialect, XSQLDA* sqlda)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c u t e _ i m m e d i a t e
 *
 **************************************
 *
 * Functional description
 *	Prepare a statement for execution.
 *
 **************************************/

	return GDS_DSQL_EXEC_IMMED2(user_status,
								db_handle, tra_handle, length, string,
								dialect, sqlda, NULL);
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXEC_IMMED2(ISC_STATUS* user_status,
											FB_API_HANDLE* db_handle,
											FB_API_HANDLE* tra_handle,
											USHORT length,
											const SCHAR* string,
											USHORT dialect,
											XSQLDA* in_sqlda,
											XSQLDA* out_sqlda)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c _ i m m e d 2
 *
 **************************************
 *
 * Functional description
 *	Prepare a statement for execution.
 *
 **************************************/
	Status status(user_status);
	ISC_STATUS s = 0;
	sqlda_sup dasup;
	memset(&dasup, 0, sizeof(sqlda_sup));

	try
	{
		USHORT in_blr_length, in_msg_type, in_msg_length,
			out_blr_length, out_msg_type, out_msg_length;

		if (UTLD_parse_sqlda(status, &dasup, &in_blr_length, &in_msg_type,
							 &in_msg_length, dialect, in_sqlda,
							 DASUP_CLAUSE_bind))
		{
			return status[1];
		}
	
		if (UTLD_parse_sqlda
			(status, &dasup, &out_blr_length, &out_msg_type, &out_msg_length,
			 dialect, out_sqlda, DASUP_CLAUSE_select))
		{
			return status[1];
		}

		s = GDS_DSQL_EXEC_IMM2_M(status, db_handle, tra_handle,
								 length, string, dialect,
								 in_blr_length,
								 dasup.dasup_clauses[DASUP_CLAUSE_bind].dasup_blr,
								 in_msg_type, in_msg_length,
								 dasup.dasup_clauses[DASUP_CLAUSE_bind].dasup_msg,
								 out_blr_length,
								 dasup.dasup_clauses[DASUP_CLAUSE_select].dasup_blr,
								 out_msg_type, out_msg_length,
								 dasup.dasup_clauses[DASUP_CLAUSE_select].dasup_msg);
		if (!s)
		{
			s =	UTLD_parse_sqlda(status, &dasup, NULL, NULL, NULL, dialect,
							 out_sqlda, DASUP_CLAUSE_select);
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
		s = status[1];
	}

	release_dsql_support(dasup);
	return s;
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXEC_IMM_M(ISC_STATUS* user_status,
										   FB_API_HANDLE* db_handle,
										   FB_API_HANDLE* tra_handle,
										   USHORT length,
										   const SCHAR* string,
										   USHORT dialect,
										   USHORT blr_length,
										   USHORT msg_type,
										   USHORT msg_length,
										   const SCHAR* blr,
										   SCHAR* msg)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c _ i m m e d _ m
 *
 **************************************
 *
 * Functional description
 *
 **************************************/

	return GDS_DSQL_EXECUTE_IMM_M(user_status, db_handle, tra_handle,
								  length, string, dialect, blr_length, blr,
								  msg_type, msg_length, msg);
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXECUTE_IMM_M(ISC_STATUS* user_status,
											  FB_API_HANDLE* db_handle,
											  FB_API_HANDLE* tra_handle,
											  USHORT length,
											  const SCHAR* string,
											  USHORT dialect,
											  USHORT blr_length,
											  const SCHAR* blr,
											  USHORT msg_type,
											  USHORT msg_length,
											  SCHAR* msg)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c u t e _ i m m e d i a t e _ m
 *
 **************************************
 *
 * Functional description
 *	Prepare a statement for execution.
 *
 **************************************/

	return GDS_DSQL_EXEC_IMM2_M(user_status, db_handle, tra_handle,
								length, string, dialect, blr_length, blr,
								msg_type, msg_length, msg, 0, NULL, 0, 0,
								NULL);
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXEC_IMM2_M(ISC_STATUS* user_status,
											FB_API_HANDLE* db_handle,
											FB_API_HANDLE* tra_handle,
											USHORT length,
											const SCHAR* string,
											USHORT dialect,
											USHORT in_blr_length,
											const SCHAR* in_blr,
											USHORT in_msg_type,
											USHORT in_msg_length,
											SCHAR* in_msg,
											USHORT out_blr_length,
											SCHAR* out_blr,
											USHORT out_msg_type,
											USHORT out_msg_length,
											SCHAR* out_msg)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c _ i m m 2 _ m
 *
 **************************************
 *
 * Functional description
 *	Prepare a statement for execution.
 *
 **************************************/
	Status status(user_status);

	bool stmt_eaten;
	if (PREPARSE_execute(	status,
							db_handle,
							tra_handle,
							length,
							string,
							&stmt_eaten,
							dialect))
	{
		if (status[1])
		{
			return status[1];
		}

		ISC_STATUS_ARRAY temp_status;
		FB_API_HANDLE crdb_trans_handle = 0;
		if (GDS_START_TRANSACTION(status, &crdb_trans_handle, 1,
								  db_handle, 0, 0))
		{
			save_error_string(status);
			GDS_DROP_DATABASE(temp_status, db_handle);
			*db_handle = 0;
			return status[1];
		}

		BOOLEAN ret_v3_error = FALSE;
		if (!stmt_eaten) {
			/* Check if against < 4.0 database */

			const SCHAR ch = isc_info_base_level;
			SCHAR buffer[16];
			if (!GDS_DATABASE_INFO(status, db_handle, 1, &ch, sizeof(buffer),
								   buffer))
			{
				if ((buffer[0] != isc_info_base_level) || (buffer[4] > 3))
					GDS_DSQL_EXEC_IMM3_M(status, db_handle,
										 &crdb_trans_handle, length, string,
										 dialect, in_blr_length, in_blr,
										 in_msg_type, in_msg_length, in_msg,
										 out_blr_length, out_blr,
										 out_msg_type, out_msg_length,
										 out_msg);
				else
					ret_v3_error = TRUE;
			}
		}

		if (status[1]) {
			GDS_ROLLBACK(temp_status, &crdb_trans_handle);
			save_error_string(status);
			GDS_DROP_DATABASE(temp_status, db_handle);
			*db_handle = 0;
			return status[1];
		}

		if (GDS_COMMIT(status, &crdb_trans_handle)) {
			GDS_ROLLBACK(temp_status, &crdb_trans_handle);
			save_error_string(status);
			GDS_DROP_DATABASE(temp_status, db_handle);
			*db_handle = 0;
			return status[1];
		}

		if (ret_v3_error) {
			status[0] = isc_arg_gds;
			status[1] = isc_srvr_version_too_old;
			status[2] = isc_arg_end;
			return status[1];
		}

		return status[1];
	}

	return GDS_DSQL_EXEC_IMM3_M(user_status, db_handle, tra_handle,
								length, string, dialect,
								in_blr_length, in_blr, in_msg_type,
								in_msg_length, in_msg, out_blr_length,
								out_blr, out_msg_type, out_msg_length,
								out_msg);
}


ISC_STATUS API_ROUTINE GDS_DSQL_EXEC_IMM3_M(ISC_STATUS* user_status,
											FB_API_HANDLE* db_handle,
											FB_API_HANDLE* tra_handle,
											USHORT length,
											const SCHAR* string,
											USHORT dialect,
											USHORT in_blr_length,
											const SCHAR* in_blr,
											USHORT in_msg_type,
											USHORT in_msg_length,
											SCHAR* in_msg,
											USHORT out_blr_length,
											SCHAR* out_blr,
											USHORT out_msg_type,
											USHORT out_msg_length,
											SCHAR* out_msg)
{
/**************************************
 *
 *	i s c _ d s q l _ e x e c _ i m m 3 _ m
 *
 **************************************
 *
 * Functional description
 *	Prepare a statement for execution.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Attachment* dbb = translate<Attachment>(db_handle);
		status.setPrimaryHandle(dbb);

		Transaction* transaction = NULL;
		StoredTra* handle = NULL;

		if (tra_handle && *tra_handle)
		{
			transaction = translate<Transaction>(tra_handle);
			Transaction* t = find_transaction(dbb, transaction);
			if (!t)
			{
				bad_handle(isc_bad_trans_handle);
			}
			handle = t->handle;
		}

		CALL(PROC_DSQL_EXEC_IMMED2, dbb->implementation) (status,
														  &dbb->handle,
														  &handle,
														  length, string, dialect,
														  in_blr_length, in_blr,
														  in_msg_type, in_msg_length, in_msg,
														  out_blr_length, out_blr, 
														  out_msg_type, out_msg_length, out_msg);

		if (!status[1])
		{
			if (transaction && !handle)
			{
				delete transaction;
				*tra_handle = 0;
			}
			else if (!transaction && handle)
			{
				transaction = new Transaction(handle, tra_handle, dbb);
			}
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_FETCH(ISC_STATUS* user_status,
									  FB_API_HANDLE* stmt_handle,
									  USHORT dialect,
									  XSQLDA* sqlda)
{
/**************************************
 *
 *	i s c _ d s q l _ f e t c h
 *
 **************************************
 *
 * Functional description
 *	Fetch next record from a dynamic SQL cursor
 *
 **************************************/
	Status status(user_status);

	try
	{
		if (!sqlda) 
		{
			Firebird::status_exception::raise(isc_dsql_sqlda_err, isc_arg_end);
		}

		Statement* statement = translate<Statement>(stmt_handle);

		statement->checkPrepared();
		sqlda_sup& dasup = statement->das;

		USHORT blr_length, msg_type, msg_length;
		if (UTLD_parse_sqlda(status, &dasup, &blr_length, &msg_type, &msg_length,
							 dialect, sqlda, DASUP_CLAUSE_select))
		{
			return status[1];
		}

		ISC_STATUS s = GDS_DSQL_FETCH_M(status, stmt_handle, blr_length,
								dasup.dasup_clauses[DASUP_CLAUSE_select].dasup_blr,
								0, msg_length,
								dasup.dasup_clauses[DASUP_CLAUSE_select].dasup_msg);
		if (s && s != 101)
		{
			return s;
		}

		if (UTLD_parse_sqlda(status, &dasup, NULL, NULL, NULL,
							 dialect, sqlda, DASUP_CLAUSE_select))
		{
			return status[1];
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


#ifdef SCROLLABLE_CURSORS
ISC_STATUS API_ROUTINE GDS_DSQL_FETCH2(ISC_STATUS* user_status,
									   FB_API_HANDLE* stmt_handle,
									   USHORT dialect,
									   XSQLDA* sqlda,
									   USHORT direction,
									   SLONG offset)
{
/**************************************
 *
 *	i s c _ d s q l _ f e t c h 2
 *
 **************************************
 *
 * Functional description
 *	Fetch next record from a dynamic SQL cursor
 *
 **************************************/
	Status status(user_status);

	try
	{
		if (!sqlda) 
		{
			Firebird::status_exception::raise(isc_dsql_sqlda_err, isc_arg_end);
		}

		Statement* statement = translate<Statement>(stmt_handle);

		statement->checkPrepared();
		sqlda_sup& dasup = statement->das;

		USHORT blr_length, msg_type, msg_length;

		if (UTLD_parse_sqlda(status, &dasup, &blr_length, &msg_type, &msg_length,
						 dialect, sqlda, DASUP_CLAUSE_select))
		{
			return status[1];
		}

		ISC_STATUS s = GDS_DSQL_FETCH2_M(status, stmt_handle, blr_length,
							   dasup.dasup_clauses[DASUP_CLAUSE_select].dasup_blr,
							   0, msg_length,
							   dasup.dasup_clauses[DASUP_CLAUSE_select].dasup_msg,
							   direction, offset);
		if (s && s != 101)
		{
			return s;
		}

		if (UTLD_parse_sqlda(status, &dasup, NULL, NULL, NULL,
							 dialect, sqlda, DASUP_CLAUSE_select))
		{
			return status[1];
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}
#endif


ISC_STATUS API_ROUTINE GDS_DSQL_FETCH_M(ISC_STATUS* user_status,
										FB_API_HANDLE* stmt_handle,
										USHORT blr_length,
										const SCHAR* blr,
										USHORT msg_type,
										USHORT msg_length,
										SCHAR* msg)
{
/**************************************
 *
 *	i s c _ d s q l _ f e t c h _ m
 *
 **************************************
 *
 * Functional description
 *	Fetch next record from a dynamic SQL cursor
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Statement* statement = translate<Statement>(stmt_handle);
		status.setPrimaryHandle(statement);

		ISC_STATUS s =
			CALL(PROC_DSQL_FETCH, statement->implementation) (status,
															  &statement->handle,
															  blr_length, blr,
															  msg_type,
															  msg_length, msg
#ifdef SCROLLABLE_CURSORS
															  ,
															  (USHORT) 0,
															  (ULONG) 1
#endif // SCROLLABLE_CURSORS
															  );

		if (s == 100 || s == 101)
		{
			return s;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


#ifdef SCROLLABLE_CURSORS
ISC_STATUS API_ROUTINE GDS_DSQL_FETCH2_M(ISC_STATUS* user_status,
										 FB_API_HANDLE* stmt_handle,
										 USHORT blr_length,
										 const SCHAR* blr,
										 USHORT msg_type,
										 USHORT msg_length,
										 SCHAR* msg,
										 USHORT direction,
										 SLONG offset)
{
/**************************************
 *
 *	i s c _ d s q l _ f e t c h 2 _ m
 *
 **************************************
 *
 * Functional description
 *	Fetch next record from a dynamic SQL cursor
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Statement* statement = translate<Statement>(stmt_handle);
		status.setPrimaryHandle(statement);

		ISC_STATUS s =
			CALL(PROC_DSQL_FETCH, statement->implementation) (status,
															  &statement->
															  handle,
															  blr_length, blr,
															  msg_type,
															  msg_length, msg,
															  direction,
															  offset);

		if (s == 100 || s == 101)
		{
			return s;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}
#endif


ISC_STATUS API_ROUTINE GDS_DSQL_FREE(ISC_STATUS * user_status,
									 FB_API_HANDLE * stmt_handle,
									 USHORT option)
{
/*****************************************
 *
 *	i s c _ d s q l _ f r e e _ s t a t e m e n t
 *
 *****************************************
 *
 * Functional Description
 *	release request for an sql statement
 *
 *****************************************/
	YEntry status(user_status);

	try
	{
		Statement* statement = translate<Statement>(stmt_handle);

		if (CALL(PROC_DSQL_FREE, statement->implementation) (status,
															 &statement->handle,
															 option))
		{
			return status[1];
		}

		if (option & DSQL_drop) 
		{
			release_dsql_support(statement->das);
			delete statement;
			*stmt_handle = 0;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_INSERT(ISC_STATUS* user_status,
									   FB_API_HANDLE* stmt_handle,
									   USHORT dialect,
									   XSQLDA* sqlda)
{
/**************************************
 *
 *	i s c _ d s q l _ i n s e r t
 *
 **************************************
 *
 * Functional description
 *	Insert next record into a dynamic SQL cursor
 *
 **************************************/
	Status status(user_status);

	try
	{
		Statement* statement = translate<Statement>(stmt_handle);

		statement->checkPrepared();
		sqlda_sup& dasup = statement->das;
		USHORT blr_length, msg_type, msg_length;
		if (UTLD_parse_sqlda(status, &dasup, &blr_length, &msg_type, &msg_length,
							 dialect, sqlda, DASUP_CLAUSE_bind))
		{
			return status[1];
		}

		return GDS_DSQL_INSERT_M(status, stmt_handle, blr_length,
								 dasup.dasup_clauses[DASUP_CLAUSE_bind].
								 dasup_blr, 0, msg_length,
								 dasup.dasup_clauses[DASUP_CLAUSE_bind].
								 dasup_msg);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_INSERT_M(ISC_STATUS* user_status,
										 FB_API_HANDLE* stmt_handle,
										 USHORT blr_length,
										 const SCHAR* blr,
										 USHORT msg_type,
										 USHORT msg_length,
										 const SCHAR* msg)
{
/**************************************
 *
 *	i s c _ d s q l _ i n s e r t _ m
 *
 **************************************
 *
 * Functional description
 *	Insert next record into a dynamic SQL cursor
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Statement* statement = translate<Statement>(stmt_handle);
		status.setPrimaryHandle(statement);

		statement->checkPrepared();
		sqlda_sup& dasup = statement->das;

		CALL(PROC_DSQL_INSERT, statement->implementation) (status,
														   &statement->handle,
														   blr_length, blr, 
														   msg_type, msg_length, msg);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_PREPARE(ISC_STATUS* user_status,
										FB_API_HANDLE* tra_handle,
										FB_API_HANDLE* stmt_handle,
										USHORT length,
										const SCHAR* string,
										USHORT dialect,
										XSQLDA* sqlda)
{
/**************************************
 *
 *	i s c _ d s q l _ p r e p a r e
 *
 **************************************
 *
 * Functional description
 *	Prepare a statement for execution.
 *
 **************************************/
	Status status(user_status);

	try
	{
		Statement* statement = translate<Statement>(stmt_handle);
		sqlda_sup& dasup = statement->das;

		USHORT buffer_len = sqlda_buffer_size(PREPARE_BUFFER_SIZE, sqlda, dialect);
		Attachment*	attachment = statement->parent;
		Firebird::Array<SCHAR> db_prepare_buffer;
		SCHAR* const buffer = db_prepare_buffer.getBuffer(buffer_len);

		if (!GDS_DSQL_PREPARE_M(status,
								tra_handle,
								stmt_handle,
								length,
								string,
								dialect,
								sizeof(sql_prepare_info2),
								sql_prepare_info2,
								buffer_len,
								buffer))
		{
			statement->flags &= ~HANDLE_STATEMENT_prepared;
			release_dsql_support(dasup);
			memset(&dasup, 0, sizeof(dasup));

			dasup.dasup_dialect = dialect;

			SCHAR* p = buffer;

			dasup.dasup_stmt_type = 0;
			if (*p == isc_info_sql_stmt_type)
			{
				const USHORT len = gds__vax_integer((UCHAR*)p + 1, 2);
				dasup.dasup_stmt_type = gds__vax_integer((UCHAR*)p + 3, len);
				p += 3 + len;
			}

			sqlda_sup::dasup_clause &das_select = dasup.dasup_clauses[DASUP_CLAUSE_select];
			sqlda_sup::dasup_clause &das_bind = dasup.dasup_clauses[DASUP_CLAUSE_bind];
			das_select.dasup_info_buf = das_bind.dasup_info_buf = 0;
			das_select.dasup_info_len = das_bind.dasup_info_len = 0;

			SCHAR* buf_select = 0; // pointer in the buffer where isc_info_sql_select starts
			USHORT len_select = 0; // length of isc_info_sql_select part
			if (*p == isc_info_sql_select)
			{
				das_select.dasup_info_buf = p;
				buf_select = p;
				len_select = buffer_len - (buf_select - buffer);
			}

			das_bind.dasup_info_buf = UTLD_skip_sql_info(p);

			p = das_select.dasup_info_buf;
			if (p)
			{
				SCHAR* p2 = das_bind.dasup_info_buf;
				if (p2)
				{
					const SLONG len =  p2 - p;

					p2 = alloc(len + 1);
					memmove(p2, p, len);
					p2[len] = isc_info_end;
					das_select.dasup_info_buf = p2;
					das_select.dasup_info_len = len + 1;
					
					buf_select = das_select.dasup_info_buf;
					len_select = das_select.dasup_info_len;
				}
				else 
				{
					das_select.dasup_info_buf = 0;
					das_select.dasup_info_len = 0;
				}
			}

			p = das_bind.dasup_info_buf;
			if (p)
			{
				SCHAR* p2 = UTLD_skip_sql_info(p);
				if (p2)
				{
					const SLONG len =  p2 - p;

					p2 = alloc(len + 1);
					memmove(p2, p, len);
					p2[len] = isc_info_end;
					das_bind.dasup_info_buf = p2;
					das_bind.dasup_info_len = len + 1;
				}
				else 
				{
					das_bind.dasup_info_buf = 0;
					das_bind.dasup_info_len = 0;
				}
			}

			iterative_sql_info(status, stmt_handle, sizeof(sql_prepare_info),
				sql_prepare_info, len_select, buf_select, dialect, sqlda);

			// statement prepared OK
			statement->flags |= HANDLE_STATEMENT_prepared;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_PREPARE_M(ISC_STATUS* user_status,
										  FB_API_HANDLE* tra_handle,
										  FB_API_HANDLE* stmt_handle,
										  USHORT length,
										  const SCHAR* string,
										  USHORT dialect,
										  USHORT item_length,
										  const SCHAR* items,
										  USHORT buffer_length,
										  SCHAR* buffer)
{
/**************************************
 *
 *	i s c _ d s q l _ p r e p a r e _ m
 *
 **************************************
 *
 * Functional description
 *	Prepare a statement for execution.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Statement* statement = translate<Statement>(stmt_handle);
		status.setPrimaryHandle(statement);

		StoredTra* handle = NULL;

		if (tra_handle && *tra_handle)
		{
			Transaction* transaction = translate<Transaction>(tra_handle);
			transaction = find_transaction(statement->parent, transaction);
			if (!transaction)
			{
				bad_handle (isc_bad_trans_handle);
			}
			handle = transaction->handle;
		}

		CALL(PROC_DSQL_PREPARE, statement->implementation) (status,
															&handle,
															&statement->handle, 
															length,
															string, dialect,
															item_length,
															items,
															buffer_length,
															buffer);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_SET_CURSOR(ISC_STATUS* user_status,
										   FB_API_HANDLE* stmt_handle,
										   const SCHAR* cursor,
										   USHORT type)
{
/**************************************
 *
 *	i s c _ d s q l _ s e t _ c u r s o r
 *
 **************************************
 *
 * Functional description
 *	Set a cursor name for a dynamic request.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Statement* statement = translate<Statement>(stmt_handle);
		status.setPrimaryHandle(statement);

		CALL(PROC_DSQL_SET_CURSOR, statement->implementation) (status,
															   &statement->handle,
															   cursor, type);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_DSQL_SQL_INFO(ISC_STATUS* user_status,
										 FB_API_HANDLE* stmt_handle,
										 SSHORT item_length,
										 const SCHAR* items,
										 SSHORT buffer_length,
										 SCHAR* buffer)
{
/**************************************
 *
 *	i s c _ d s q l _ s q l _ i n f o
 *
 **************************************
 *
 * Functional description
 *	Provide information on sql statement.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Statement* statement = translate<Statement>(stmt_handle);
		status.setPrimaryHandle(statement);

		if (( (item_length == 1) && (items[0] == isc_info_sql_stmt_type) ||
			  (item_length == 2) && (items[0] == isc_info_sql_stmt_type) && 
		      (items[1] == isc_info_end || items[1] == 0) ) &&
			(statement->flags & HANDLE_STATEMENT_prepared) &&
			statement->das.dasup_stmt_type)
		{
			if (buffer_length >= 8)
			{
				*buffer++ = isc_info_sql_stmt_type;
				put_short((UCHAR*) buffer, 4);
				buffer += 2;
				put_long((UCHAR*) buffer, statement->das.dasup_stmt_type);
				buffer += 4;
				*buffer = isc_info_end;
			}
			else 
			{
				*buffer = isc_info_truncated;
			}
		}
		else 
		{
			CALL(PROC_DSQL_SQL_INFO, statement->implementation) (status,
																 &statement->handle,
																 item_length, items,
																 buffer_length, buffer);
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


int API_ROUTINE gds__enable_subsystem(TEXT * subsystem)
{
/**************************************
 *
 *	g d s _ $ e n a b l e _ s u b s y s t e m 
 *
 **************************************
 *
 * Functional description
 *	Enable access to a specific subsystem.  If no subsystem
 *	has been explicitly enabled, all are available.
 *
 **************************************/
	return FALSE;
}


ISC_STATUS API_ROUTINE isc_wait_for_event(ISC_STATUS * user_status,
									  FB_API_HANDLE * handle,
									  USHORT length,
									  const UCHAR* events,
									  UCHAR* buffer)
{
/**************************************
 *
 *	g d s _ $ e v e n t _ w a i t
 *
 **************************************
 *
 * Functional description
 *	Que request for event notification.
 *
 **************************************/
	Status status(user_status);

	try
	{
		if (!why_initialized) 
		{
			gds__register_cleanup((FPTR_VOID_PTR) exit_handler, why_event);
			why_initialized = true;
			ISC_event_init(why_event, 0, 0);
		}

		const SLONG value = ISC_event_clear(why_event);
		SLONG id;

		if (GDS_QUE_EVENTS
			(status, handle, &id, length, events, event_ast, buffer))
		{
			return status[1];
		}

		event_t* event_ptr = why_event;
		ISC_event_wait(1, &event_ptr, &value, -1);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_GET_SEGMENT(ISC_STATUS * user_status,
									   FB_API_HANDLE * blob_handle,
									   USHORT * length,
									   USHORT buffer_length,
									   UCHAR * buffer)
{
/**************************************
 *
 *	g d s _ $ g e t _ s e g m e n t
 *
 **************************************
 *
 * Functional description
 *	Abort a partially completed blob.
 *
 **************************************/
	YEntry status(user_status);

	try 
	{
		Blob* blob = translate<Blob>(blob_handle);
		status.setPrimaryHandle(blob);

		ISC_STATUS code = 
			CALL(PROC_GET_SEGMENT, blob->implementation) (status, &blob->handle,
														  length,
														  buffer_length, buffer);

		if (code == isc_segstr_eof || code == isc_segment) 
		{
			return code;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_GET_SLICE(ISC_STATUS* user_status,
									 FB_API_HANDLE* db_handle,
									 FB_API_HANDLE* tra_handle,
									 ISC_QUAD* array_id,
									 USHORT sdl_length,
									 const UCHAR* sdl,
									 USHORT param_length,
									 const UCHAR* param,
									 SLONG slice_length,
									 UCHAR* slice,
									 SLONG* return_length)
{
/**************************************
 *
 *	g d s _ $ g e t _ s l i c e
 *
 **************************************
 *
 * Functional description
 *	Snatch a slice of an array.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Attachment* dbb = translate<Attachment>(db_handle);
		status.setPrimaryHandle(dbb);
		Transaction* transaction = findTransaction(tra_handle, dbb);

		CALL(PROC_GET_SLICE, dbb->implementation) (status,
												   &dbb->handle,
												   &transaction->handle,
												   array_id,
												   sdl_length, sdl,
												   param_length, param,
												   slice_length, slice,
												   return_length);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE fb_disconnect_transaction(ISC_STATUS* user_status,
												 FB_API_HANDLE* user_handle)
{
/**************************************
 *
 *	g d s _ $ h a n d l e _ c l e a n u p
 *
 **************************************
 *
 * Functional description
 *	Clean up a dangling transaction handle.
 *
 **************************************/
	Status status(user_status);

	try
	{
		Transaction* transaction = translate<Transaction>(user_handle);

		if (!(transaction->flags & HANDLE_TRANSACTION_limbo))
		{
			Firebird::status_exception::raise(isc_no_recon, isc_arg_end);
		}

		while (transaction) 
		{
			Transaction* sub = transaction;
			transaction = sub->next;
			delete sub;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_OPEN_BLOB(ISC_STATUS* user_status,
									 FB_API_HANDLE* db_handle,
									 FB_API_HANDLE* tra_handle,
									 FB_API_HANDLE* blob_handle,
									 SLONG* blob_id)
{
/**************************************
 *
 *	g d s _ $ o p e n _ b l o b
 *
 **************************************
 *
 * Functional description
 *	Open an existing blob.
 *
 **************************************/

	return open_blob(user_status, db_handle, tra_handle, blob_handle, blob_id,
					 0, 0, PROC_OPEN_BLOB, PROC_OPEN_BLOB2);
}


ISC_STATUS API_ROUTINE GDS_OPEN_BLOB2(ISC_STATUS* user_status,
									  FB_API_HANDLE* db_handle,
									  FB_API_HANDLE* tra_handle,
									  FB_API_HANDLE* blob_handle,
									  SLONG* blob_id,
									  SSHORT bpb_length,
									  const UCHAR* bpb)
{
/**************************************
 *
 *	g d s _ $ o p e n _ b l o b 2
 *
 **************************************
 *
 * Functional description
 *	Open an existing blob (extended edition).
 *
 **************************************/

	return open_blob(user_status, db_handle, tra_handle, blob_handle, blob_id,
					 bpb_length, bpb, PROC_OPEN_BLOB,
					 PROC_OPEN_BLOB2);
}


ISC_STATUS API_ROUTINE GDS_PREPARE(ISC_STATUS* user_status,
								   FB_API_HANDLE* tra_handle)
{
/**************************************
 *
 *	g d s _ $ p r e p a r e
 *
 **************************************
 *
 * Functional description
 *	Prepare a transaction for commit.  First phase of a two
 *	phase commit.
 *
 **************************************/
	return GDS_PREPARE2(user_status, tra_handle, 0, 0);
}


ISC_STATUS API_ROUTINE GDS_PREPARE2(ISC_STATUS* user_status,
									FB_API_HANDLE* tra_handle,
									USHORT msg_length,
									const UCHAR* msg)
{
/**************************************
 *
 *	g d s _ $ p r e p a r e 2
 *
 **************************************
 *
 * Functional description
 *	Prepare a transaction for commit.  First phase of a two
 *	phase commit.
 *
 **************************************/
	YEntry status(user_status);
	try 
	{
		Transaction* transaction = translate<Transaction>(tra_handle);
		status.setPrimaryHandle(transaction);

		for (Transaction* sub = transaction; sub; sub = sub->next)
		{
			if (sub->implementation != SUBSYSTEMS &&
				CALL(PROC_PREPARE, sub->implementation) (status, &sub->handle,
														 msg_length, msg))
			{
				return status[1];
			}
		}

		transaction->flags |= HANDLE_TRANSACTION_limbo;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_PUT_SEGMENT(ISC_STATUS* user_status,
									   FB_API_HANDLE* blob_handle,
									   USHORT buffer_length,
									   const UCHAR* buffer)
{
/**************************************
 *
 *	g d s _ $ p u t _ s e g m e n t
 *
 **************************************
 *
 * Functional description
 *	Abort a partially completed blob.
 *
 **************************************/
	YEntry status(user_status);
	try 
	{
		Blob* blob = translate<Blob>(blob_handle);
		status.setPrimaryHandle(blob);

		CALL(PROC_PUT_SEGMENT, blob->implementation) (status,
													  &blob->handle,
													  buffer_length,
													  buffer);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_PUT_SLICE(ISC_STATUS* user_status,
									 FB_API_HANDLE* db_handle,
									 FB_API_HANDLE* tra_handle,
									 ISC_QUAD* array_id,
									 USHORT sdl_length,
									 const UCHAR* sdl,
									 USHORT param_length,
									 const SLONG* param,
									 SLONG slice_length,
									 UCHAR* slice)
{
/**************************************
 *
 *	g d s _ $ p u t _ s l i c e
 *
 **************************************
 *
 * Functional description
 *	Snatch a slice of an array.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Attachment* dbb = translate<Attachment>(db_handle);
		status.setPrimaryHandle(dbb);
		Transaction* transaction = findTransaction(tra_handle, dbb);

		CALL(PROC_PUT_SLICE, dbb->implementation) (status,
												   &dbb->handle,
												   &transaction->handle,
												   array_id,
												   sdl_length,
												   sdl,
												   param_length,
												   param,
												   slice_length,
												   slice);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_QUE_EVENTS(ISC_STATUS* user_status,
									  FB_API_HANDLE* handle,
									  SLONG* id,
									  USHORT length,
									  const UCHAR* events,
									  FPTR_EVENT_CALLBACK ast,
									  void* arg)
{
/**************************************
 *
 *	g d s _ $ q u e _ e v e n t s
 *
 **************************************
 *
 * Functional description
 *	Que request for event notification.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Attachment* dbb = translate<Attachment>(handle);
		status.setPrimaryHandle(dbb);

		CALL(PROC_QUE_EVENTS, dbb->implementation) (status, &dbb->handle,
													id, length, events,
													ast, arg);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_RECEIVE(ISC_STATUS* user_status,
								   FB_API_HANDLE* req_handle,
								   USHORT msg_type,
								   USHORT msg_length,
								   SCHAR* msg,
								   SSHORT level)
{
/**************************************
 *
 *	g d s _ $ r e c e i v e
 *
 **************************************
 *
 * Functional description
 *	Get a record from the host program.
 *
 **************************************/

#ifdef SCROLLABLE_CURSORS
	return GDS_RECEIVE2(user_status, req_handle, msg_type, msg_length,
						msg, level, (USHORT) blr_continue,	/* means continue in same direction as before */
						(ULONG) 1);
#else
	YEntry status(user_status);

	try
	{
		Request* request = translate<Request>(req_handle);
		status.setPrimaryHandle(request);

		CALL(PROC_RECEIVE, request->implementation) (status,
													 &request->handle,
													 msg_type,
													 msg_length,
													 msg,
													 level);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
#endif
}


#ifdef SCROLLABLE_CURSORS
ISC_STATUS API_ROUTINE GDS_RECEIVE2(ISC_STATUS* user_status,
									FB_API_HANDLE* req_handle,
									USHORT msg_type,
									USHORT msg_length,
									SCHAR* msg,
									SSHORT level,
									USHORT direction,
									ULONG offset)
{
/**************************************
 *
 *	i s c _ r e c e i v e 2
 *
 **************************************
 *
 * Functional description
 *	Scroll through the request output stream, 
 *	then get a record from the host program.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Request* request = translate<Request>(req_handle);
		status.setPrimaryHandle(request);

		CALL(PROC_RECEIVE, request->implementation) (status,
													 &request->handle,
													 msg_type,
													 msg_length,
													 msg,
													 level,
													 direction,
													 offset);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}
#endif


ISC_STATUS API_ROUTINE GDS_RECONNECT(ISC_STATUS* user_status,
									 FB_API_HANDLE* db_handle,
									 FB_API_HANDLE* tra_handle,
									 SSHORT length,
									 const UCHAR* id)
{
/**************************************
 *
 *	g d s _ $ r e c o n n e c t
 *
 **************************************
 *
 * Functional description
 *	Connect to a transaction in limbo.
 *
 **************************************/
	YEntry status(user_status);
	StoredTra* handle = 0;

	try
	{
		nullCheck(tra_handle, isc_bad_trans_handle);
		Attachment* dbb = translate<Attachment>(db_handle);
		status.setPrimaryHandle(dbb);

		if (CALL(PROC_RECONNECT, dbb->implementation) (status, &dbb->handle,
													   &handle,
													   length, id))
		{
			return status[1];
		}

		Transaction* transaction = new Transaction(handle, tra_handle, dbb);
		transaction->flags |= HANDLE_TRANSACTION_limbo;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
		if (handle)
		{
			*tra_handle = 0;
		}
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_RELEASE_REQUEST(ISC_STATUS * user_status,
										   FB_API_HANDLE * req_handle)
{
/**************************************
 *
 *	g d s _ $ r e l e a s e _ r e q u e s t
 *
 **************************************
 *
 * Functional description
 *	Release a request.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Request* request = translate<Request>(req_handle);
		status.setPrimaryHandle(request);

		if (!CALL(PROC_RELEASE_REQUEST, request->implementation) (status,
																  &request->handle))
		{
			delete request;
			*req_handle = 0;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_REQUEST_INFO(ISC_STATUS* user_status,
										FB_API_HANDLE* req_handle,
										SSHORT level,
										SSHORT item_length,
										const SCHAR* items,
										SSHORT buffer_length,
										SCHAR* buffer)
{
/**************************************
 *
 *	g d s _ $ r e q u e s t _ i n f o
 *
 **************************************
 *
 * Functional description
 *	Provide information on blob object.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Request* request = translate<Request>(req_handle);
		status.setPrimaryHandle(request);

		CALL(PROC_REQUEST_INFO, request->implementation) (status,
														  &request->handle,
														  level,
														  item_length,
														  items,
														  buffer_length,
														  buffer);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}

#if defined (SOLARIS) || defined (WIN_NT)
extern "C" 
#endif

SLONG API_ROUTINE isc_reset_fpe(USHORT fpe_status)
{
/**************************************
 *
 *	i s c _ r e s e t _ f p e
 *
 **************************************
 *
 * Functional description
 *	API to be used to tell Firebird to reset it's
 *	FPE handler - eg: client has an FPE of it's own
 *	and just changed it.
 *
 * Returns
 *	Prior setting of the FPE reset flag
 *
 **************************************/
#if !(defined SUPERCLIENT || defined SUPERSERVER)
	SLONG prior;
	prior = (SLONG) subsystem_FPE_reset;
	switch (fpe_status) {
	case FPE_RESET_INIT_ONLY:
		subsystem_FPE_reset = fpe_status;
		break;
	case FPE_RESET_NEXT_API_CALL:
		subsystem_FPE_reset = fpe_status;
		break;
	case FPE_RESET_ALL_API_CALL:
		subsystem_FPE_reset = fpe_status;
		break;
	default:
		break;
	}
	return prior;
#else
	return FPE_RESET_INIT_ONLY;
#endif
}


ISC_STATUS API_ROUTINE GDS_ROLLBACK_RETAINING(ISC_STATUS * user_status,
											  FB_API_HANDLE * tra_handle)
{
/**************************************
 *
 *	i s c _ r o l l b a c k _ r e t a i n i n g
 *
 **************************************
 *
 * Functional description
 *	Abort a transaction, but keep all cursors open.
 *
 **************************************/
	YEntry status(user_status);
	try 
	{
		Transaction* transaction = translate<Transaction>(tra_handle);
		status.setPrimaryHandle(transaction);

		for (Transaction* sub = transaction; sub; sub = sub->next)
		{
			if (sub->implementation != SUBSYSTEMS &&
				CALL(PROC_ROLLBACK_RETAINING, sub->implementation) (status,
																	&sub->handle))
			{
				return status[1];
			}
		}

		transaction->flags |= HANDLE_TRANSACTION_limbo;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_ROLLBACK(ISC_STATUS * user_status,
									FB_API_HANDLE * tra_handle)
{
/**************************************
 *
 *	g d s _ $ r o l l b a c k
 *
 **************************************
 *
 * Functional description
 *	Abort a transaction.
 *
 **************************************/
	YEntry status(user_status);
	try 
	{
		Transaction* transaction = translate<Transaction>(tra_handle);
		status.setPrimaryHandle(transaction);

		for (Transaction* sub = transaction; sub; sub = sub->next)
			if (sub->implementation != SUBSYSTEMS &&
				CALL(PROC_ROLLBACK, sub->implementation) (status, &sub->handle))
			{
				if (!is_network_error(status) ||
					(transaction->flags & HANDLE_TRANSACTION_limbo) )
				{
					return status[1];
				}
			}

		if (is_network_error(status))
		{
			init_status(status);
		}

		while (transaction) 
		{
			Transaction* sub = transaction;
			transaction = sub->next;
			delete sub;
		}
		*tra_handle = 0;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_SEEK_BLOB(ISC_STATUS * user_status,
									 FB_API_HANDLE * blob_handle,
									 SSHORT mode,
									 SLONG offset,
									 SLONG * result)
{
/**************************************
 *
 *	g d s _ $ s e e k _ b l o b
 *
 **************************************
 *
 * Functional description
 *	Seek a blob.
 *
 **************************************/
	YEntry status(user_status);
	try 
	{
		Blob* blob = translate<Blob>(blob_handle);
		status.setPrimaryHandle(blob);

		CALL(PROC_SEEK_BLOB, blob->implementation) (status,
													&blob->handle,
													mode,
													offset, result);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_SEND(ISC_STATUS* user_status,
								FB_API_HANDLE* req_handle,
								USHORT msg_type,
								USHORT msg_length,
								const SCHAR* msg,
								SSHORT level)
{
/**************************************
 *
 *	g d s _ $ s e n d
 *
 **************************************
 *
 * Functional description
 *	Get a record from the host program.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Request* request = translate<Request>(req_handle);
		status.setPrimaryHandle(request);

		CALL(PROC_SEND, request->implementation) (status, &request->handle,
												  msg_type, msg_length, msg,
												  level);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_SERVICE_ATTACH(ISC_STATUS* user_status,
										  USHORT service_length,
										  const TEXT* service_name,
										  FB_API_HANDLE* public_handle,
										  USHORT spb_length,
										  const SCHAR* spb)
{
/**************************************
 *
 *	i s c _ s e r v i c e _ a t t a c h
 *
 **************************************
 *
 * Functional description
 *	Attach a service through the first subsystem
 *	that recognizes it.
 *
 **************************************/
	StoredSvc* handle = 0;
	Service* service = 0;
	ISC_STATUS_ARRAY temp;
	USHORT n;
	YEntry status(user_status);

	try
	{
		nullCheck(public_handle, isc_bad_svc_handle);

		if (!service_name) 
		{
			Firebird::status_exception::raise(isc_service_att_err,
						isc_arg_gds, isc_svc_name_missing, isc_arg_end);
		}

		if (spb_length > 0 && !spb) 
		{
			Firebird::status_exception::raise(isc_bad_spb_form, isc_arg_end);
		}

#if !defined (SUPERCLIENT)
		if (shutdown_flag) 
		{
			Firebird::status_exception::raise(isc_shutwarn, isc_arg_end);
		}
#endif /* !SUPERCLIENT */

		SUBSYSTEM_USAGE_INCR;

		USHORT org_length = service_length;
		if (org_length) {
			const TEXT* p = service_name + org_length - 1;
			while (*p == ' ')
				p--;
			org_length = p - service_name + 1;
		}

		ISC_STATUS* ptr = status;
		for (n = 0; n < SUBSYSTEMS; n++) 
		{
			if (!CALL(PROC_SERVICE_ATTACH, n) (ptr,
											   org_length, service_name,
											   &handle, 
											   spb_length, spb))
			{
				service = new Service(handle, public_handle, n);
				status[0] = isc_arg_gds;
				status[1] = 0;
				if (status[2] != isc_arg_warning)
				{
					status[2] = isc_arg_end;
				}
				return status[1];
			}
			if (ptr[1] != isc_unavailable)
			{
				ptr = temp;
			}
		}

		SUBSYSTEM_USAGE_DECR;
		if (status[1] == isc_unavailable)
		{
			status[1] = isc_service_att_err;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
		if (handle)
		{
			CALL(PROC_SERVICE_DETACH, n) (temp, handle);
			*public_handle = 0;
			delete service;
		}
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_SERVICE_DETACH(ISC_STATUS * user_status,
										  FB_API_HANDLE * handle)
{
/**************************************
 *
 *	i s c _ s e r v i c e _ d e t a c h
 *
 **************************************
 *
 * Functional description
 *	Close down a service.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Service* service = translate<Service>(handle);

		if (CALL(PROC_SERVICE_DETACH, service->implementation) (status,
																&service->handle))
		{
			return status[1];
		}

		SUBSYSTEM_USAGE_DECR;

		delete service;
		*handle = 0;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_SERVICE_QUERY(ISC_STATUS* user_status,
										 FB_API_HANDLE* handle,
										 ULONG* reserved,
										 USHORT send_item_length,
										 const SCHAR* send_items,
										 USHORT recv_item_length,
										 const SCHAR* recv_items,
										 USHORT buffer_length,
										 SCHAR* buffer)
{
/**************************************
 *
 *	i s c _ s e r v i c e _ q u e r y
 *
 **************************************
 *
 * Functional description
 *	Provide information on service object.
 *
 *	NOTE: The parameter RESERVED must not be used
 *	for any purpose as there are networking issues
 *	involved (as with any handle that goes over the
 *	network).  This parameter will be implemented at 
 *	a later date.
 **************************************/
	YEntry status(user_status);

	try
	{
		Service* service = translate<Service>(handle);

		CALL(PROC_SERVICE_QUERY, service->implementation) (status, 
														   &service->handle, 
														   0,	/* reserved */
														   send_item_length,
														   send_items,
														   recv_item_length,
														   recv_items,
														   buffer_length,
														   buffer);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_SERVICE_START(ISC_STATUS* user_status,
										 FB_API_HANDLE* handle,
										 ULONG* reserved,
										 USHORT spb_length,
										 const SCHAR* spb)
{
/**************************************
 *
 *	i s c _ s e r v i c e _ s t a r t
 *
 **************************************
 *
 * Functional description
 *	Starts a service thread
 * 
 *	NOTE: The parameter RESERVED must not be used
 *	for any purpose as there are networking issues
 *	involved (as with any handle that goes over the
 *	network).  This parameter will be implemented at 
 *	a later date.
 **************************************/
	YEntry status(user_status);

	try
	{
		Service* service = translate<Service>(handle);

		CALL(PROC_SERVICE_START, service->implementation) (status,
														   &service->handle,
														   NULL,
														   spb_length, spb);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_START_AND_SEND(ISC_STATUS* user_status,
										  FB_API_HANDLE* req_handle,
										  FB_API_HANDLE* tra_handle,
										  USHORT msg_type,
										  USHORT msg_length,
										  const SCHAR* msg,
										  SSHORT level)
{
/**************************************
 *
 *	g d s _ $ s t a r t _ a n d _ s e n d
 *
 **************************************
 *
 * Functional description
 *	Get a record from the host program.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Request* request = translate<Request>(req_handle);
		status.setPrimaryHandle(request);
		Transaction* transaction = findTransaction(tra_handle, request->parent);

		CALL(PROC_START_AND_SEND, request->implementation) (status,
															&request->handle,
															&transaction->
															handle, msg_type,
															msg_length, msg,
															level);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_START(ISC_STATUS * user_status,
								 FB_API_HANDLE * req_handle,
								 FB_API_HANDLE * tra_handle,
								 SSHORT level)
{
/**************************************
 *
 *	g d s _ $ s t a r t
 *
 **************************************
 *
 * Functional description
 *	Get a record from the host program.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Request* request = translate<Request>(req_handle);
		status.setPrimaryHandle(request);
		Transaction* transaction = findTransaction(tra_handle, request->parent);

		CALL(PROC_START, request->implementation) (status,
												   &request->handle,
												   &transaction->handle,
												   level);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_START_MULTIPLE(ISC_STATUS* user_status,
										  FB_API_HANDLE* tra_handle,
										  SSHORT count,
//										  TEB* vector)
										  void* vec)
{
/**************************************
 *
 *	g d s _ $ s t a r t _ m u l t i p l e
 *
 **************************************
 *
 * Functional description
 *	Start a transaction.
 *
 **************************************/
	TEB* vector = (TEB*) vec;
	ISC_STATUS_ARRAY temp;
	Transaction* transaction = 0;
	Attachment* dbb = 0;
	StoredTra* handle = 0;
	
	YEntry status(user_status);

	try
	{
		nullCheck(tra_handle, isc_bad_trans_handle);

		if (count <= 0 || !vector)
		{
			Firebird::status_exception::raise(isc_bad_teb_form, isc_arg_end);
		}

		Transaction** ptr;
		USHORT n;
		for (n = 0, ptr = &transaction; n < count;
			n++, ptr = &(*ptr)->next, vector++)
		{
			if (vector->teb_tpb_length < 0 ||
				(vector->teb_tpb_length > 0 && !vector->teb_tpb))
			{
				Firebird::status_exception::raise(isc_bad_tpb_form, isc_arg_end);
			}

			dbb = translate<Attachment>(vector->teb_database);

			if (CALL(PROC_START_TRANSACTION, dbb->implementation) (status,
																   &handle,
																   1,
																   &dbb->handle,
																   vector->teb_tpb_length,
																   vector->teb_tpb))
			{
				Firebird::status_exception::raise(status);
			}

			*ptr = new Transaction(handle, 0, dbb);
			handle = 0;
		}

		if (transaction->next)
		{
			Transaction *sub = new Transaction(tra_handle, SUBSYSTEMS);
			sub->next = transaction;
		}
		else {
			*tra_handle = transaction->public_handle;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);

		if (handle || transaction)
		{
			*tra_handle = 0;
		}
		while (transaction) 
		{
			Transaction *sub = transaction;
			transaction = sub->next;
			if (sub->handle)
			{
				CALL(PROC_ROLLBACK, sub->implementation) (temp, &sub->handle);
			}
			delete sub;
		}
		if (handle && dbb)
		{
			CALL(PROC_ROLLBACK, dbb->implementation) (temp, handle);
		}
	}

	return status[1];
}


ISC_STATUS API_ROUTINE_VARARG GDS_START_TRANSACTION(ISC_STATUS * user_status,
													FB_API_HANDLE * tra_handle,
													SSHORT count, ...)
{
/**************************************
 *
 *	g d s _ $ s t a r t _ t r a n s a c t i o n
 *
 **************************************
 *
 * Functional description
 *	Start a transaction.
 *
 **************************************/
	Status status(user_status);
	try 
	{
		Firebird::HalfStaticArray<TEB, 16> tebs;
		TEB* teb = tebs.getBuffer(count);

		const TEB* const end = teb + count;
		va_list ptr;
		va_start(ptr, count);

		for (TEB* teb_iter = teb; teb_iter < end; teb_iter++) {
			teb_iter->teb_database = va_arg(ptr, FB_API_HANDLE*);
			teb_iter->teb_tpb_length = va_arg(ptr, int);
			teb_iter->teb_tpb = va_arg(ptr, UCHAR *);
		}
		va_end(ptr);

		GDS_START_MULTIPLE(status, tra_handle, count, teb);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_TRANSACT_REQUEST(ISC_STATUS* user_status,
											FB_API_HANDLE* db_handle,
											FB_API_HANDLE* tra_handle,
											USHORT blr_length,
											const SCHAR* blr,
											USHORT in_msg_length,
											SCHAR* in_msg,
											USHORT out_msg_length,
											SCHAR* out_msg)
{
/**************************************
 *
 *	i s c _ t r a n s a c t _ r e q u e s t
 *
 **************************************
 *
 * Functional description
 *	Execute a procedure.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Attachment* dbb = translate<Attachment>(db_handle);
		status.setPrimaryHandle(dbb);
		Transaction* transaction = findTransaction(tra_handle, dbb);

		CALL(PROC_TRANSACT_REQUEST, dbb->implementation) (status,
														  &dbb->handle,
														  &transaction->
														  handle, blr_length,
														  blr, in_msg_length,
														  in_msg,
														  out_msg_length,
														  out_msg);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE gds__transaction_cleanup(ISC_STATUS * user_status,
												FB_API_HANDLE * tra_handle,
												TransactionCleanupRoutine *routine,
												void* arg)
{
/**************************************
 *
 *	g d s _ $ t r a n s a c t i o n _ c l e a n u p
 *
 **************************************
 *
 * Functional description
 *	Register a transaction specific cleanup handler.
 *
 **************************************/
	Status status(user_status);
	try 
	{
		translate<Transaction>(tra_handle)->cleanup.add(routine, arg);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_TRANSACTION_INFO(ISC_STATUS* user_status,
											FB_API_HANDLE* tra_handle,
											SSHORT item_length,
											const SCHAR* items,
											SSHORT buffer_length,
											UCHAR* buffer)
{
/**************************************
 *
 *	g d s _ $ t r a n s a c t i o n _ i n f o
 *
 **************************************
 *
 * Functional description
 *	Provide information on transaction object.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Transaction* transaction = translate<Transaction>(tra_handle);
		status.setPrimaryHandle(transaction);

		if (transaction->implementation != SUBSYSTEMS) {
			CALL(PROC_TRANSACTION_INFO, transaction->implementation) (status,
																	  &transaction->
																	  handle,
																	  item_length,
																	  items,
																	  buffer_length,
																	  buffer);
		}
		else {
			SSHORT item_len = item_length;
			SSHORT buffer_len = buffer_length;
			for (Transaction* sub = transaction->next; sub; sub = sub->next) {
				if (CALL(PROC_TRANSACTION_INFO, sub->implementation) (status,
																	  &sub->
																	  handle,
																	  item_len,
																	  items,
																	  buffer_len,
																	  buffer))
				{
					return status[1];
				}

				UCHAR* ptr = buffer;
				const UCHAR* const end = buffer + buffer_len;
				while (ptr < end && *ptr == isc_info_tra_id)
				{
					ptr += 3 + gds__vax_integer(ptr + 1, 2);
				}

				if (ptr >= end || *ptr != isc_info_end) 
				{
					return status[1];
				}

				buffer_len = end - ptr;
				buffer = ptr;
			}
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


ISC_STATUS API_ROUTINE GDS_UNWIND(ISC_STATUS * user_status,
								  FB_API_HANDLE * req_handle,
								  SSHORT level)
{
/**************************************
 *
 *	g d s _ $ u n w i n d
 *
 **************************************
 *
 * Functional description
 *	Unwind a running request.  This is potentially nasty since it can be called
 *	asynchronously.
 *
 **************************************/
	YEntry status(user_status);

	try
	{
		Request* request = translate<Request>(req_handle);
		status.setPrimaryHandle(request);

		CALL(PROC_UNWIND, request->implementation) (status,
													&request->handle,
													level);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}

#ifdef DEBUG_GDS_ALLOC
static SCHAR *alloc_debug(SLONG length, const char* file, int line)
#else
static SCHAR *alloc(SLONG length)
#endif
{
/**************************************
 *
 *	a l l o c
 *
 **************************************
 *
 * Functional description
 *	Allocate some memory.
 *
 **************************************/
	SCHAR *block;

#ifdef DEBUG_GDS_ALLOC
	if (block = reinterpret_cast<SCHAR *>(gds__alloc_debug((SLONG) (sizeof(SCHAR) * length), file, line)))
#else
	if (block = reinterpret_cast<SCHAR *>(gds__alloc((SLONG) (sizeof(SCHAR) * length))))
#endif
		memset(block, 0, length);
	else
		Firebird::BadAlloc::raise();
	return block;
}


#ifdef DEV_BUILD
static void check_status_vector(const ISC_STATUS* status)
 {
/**************************************
 *
 *	c h e c k _ s t a t u s _ v e c t o r
 *
 **************************************
 *
 * Functional description
 *	Validate that a status vector looks valid.
 *
 **************************************/

#define SV_MSG(x)	{ gds__log ("%s %d check_status_vector: %s", __FILE__, __LINE__, (x)); BREAKPOINT (__LINE__); }

	const ISC_STATUS* s = status;
	if (!s) {
		SV_MSG("Invalid status vector");
		return;
	}

	if (*s != isc_arg_gds) {
		SV_MSG("Must start with isc_arg_gds");
		return;
	}

/* Vector [2] could either end the vector, or start a warning 
   in either case the status vector is a success */
	if ((s[1] == FB_SUCCESS) && 
		(s[2] != isc_arg_end) && 
		(s[2] != isc_arg_gds) && 
		(s[2] != isc_arg_warning))
	{
		SV_MSG("Bad success vector format");
	}

	ULONG length;

	while (*s != isc_arg_end) {
		const ISC_STATUS code = *s++;
		switch (code) {
		case isc_arg_warning:
		case isc_arg_gds:
			/* The next element must either be 0 (indicating no error) or a
			 * valid isc error message, let's check */
			if (*s && (*s & ISC_MASK) != ISC_MASK) {
				if (code == isc_arg_warning) {
					SV_MSG("warning code not a valid ISC message");
				}
				else {
					SV_MSG("error code not a valid ISC message");
				}
			}

			/* If the error code is valid, then I better be able to retrieve a
			 * proper facility code from it ... let's find out */
			if (*s && (*s & ISC_MASK) == ISC_MASK) {
				bool found = false;

				const struct _facilities* facs = facilities;
				const int fac_code = GET_FACILITY(*s);
				while (facs->facility) {
					if (facs->fac_code == fac_code) {
						found = true;
						break;
					}
					facs++;
				}
				if (!found)
					if (code == isc_arg_warning) {
						SV_MSG
							("warning code does not contain a valid facility");
					}
					else {
						SV_MSG
							("error code does not contain a valid facility");
					}
			}
			s++;
			break;

		case isc_arg_interpreted:
		case isc_arg_string:
		case isc_arg_sql_state:
			length = strlen((const char*) *s);
			/* This check is heuristic, not deterministic */
			if (length > 1024 - 1)
				SV_MSG("suspect length value");
			if (*((const UCHAR *) * s) == 0xCB)
				SV_MSG("string in freed memory");
			s++;
			break;

		case isc_arg_cstring:
			length = (ULONG) * s;
			s++;
			/* This check is heuristic, not deterministic */
			/* Note: This can never happen anyway, as the length is coming
			   from a byte value */
			if (length > 1024 - 1)
				SV_MSG("suspect length value");
			if (*((const UCHAR *) * s) == 0xCB)
				SV_MSG("string in freed memory");
			s++;
			break;

		case isc_arg_number:
		case isc_arg_vms:
		case isc_arg_unix:
		case isc_arg_win32:
			s++;
			break;

		default:
			SV_MSG("invalid status code");
			return;
		}
		if ((s - status) >= ISC_STATUS_LENGTH)
			SV_MSG("vector too long");
	}

#undef SV_MSG

}
#endif


// Make this repetitive block a function.
// Call all cleanup routines registered with the transaction.
/*void WHY_cleanup_transaction(Transaction* transaction)
{
	for (clean* cln = transaction->cleanup; cln; cln = transaction->cleanup)
	{
		transaction->cleanup = cln->clean_next;
		cln->TransactionRoutine(transaction->public_handle, cln->clean_arg);
		free_block(cln);
	}
}*/

static void event_ast(void* buffer_void,
					  USHORT length,
					  const UCHAR* items)
{
/**************************************
 *
 *	e v e n t _ a s t
 *
 **************************************
 *
 * Functional description
 *	We're had an event complete.
 *
 **************************************/
	memcpy(buffer_void, items, length);
	ISC_event_post(why_event);
}


static void exit_handler(event_t* why_eventL)
{
/**************************************
 *
 *	e x i t _ h a n d l e r
 *
 **************************************
 *
 * Functional description
 *	Cleanup shared image.
 *
 **************************************/

#ifdef WIN_NT
	CloseHandle((void *) why_eventL->event_handle);
#endif

	why_initialized = false;
#if !(defined SUPERCLIENT || defined SUPERSERVER)
	isc_enter_count = 0;
	subsystem_usage = 0;
	subsystem_FPE_reset = FPE_RESET_INIT_ONLY;
#endif
}


static Transaction* find_transaction(Attachment* dbb,
								Transaction* transaction)
{
/**************************************
 *
 *	f i n d _ t r a n s a c t i o n
 *
 **************************************
 *
 * Functional description
 *	Find the element of a possible multiple database transaction
 *	that corresponds to the current attachment.
 *
 **************************************/

	for (; transaction; transaction = transaction->next)
	{
		if (transaction->parent == dbb)
			return transaction;
	}

	return NULL;
}


static void free_block(void* block)
{
/**************************************
 *
 *	f r e e _ b l o c k
 *
 **************************************
 *
 * Functional description
 *	Release some memory.
 *
 **************************************/

	gds__free((SLONG *) block);
}


static int get_database_info(ISC_STATUS * status,
							 Transaction* transaction,
							 TEXT ** ptr)
{
/**************************************
 *
 *	g e t _ d a t a b a s e _ i n f o
 *
 **************************************
 *
 * Functional description
 *	Get the full database pathname
 *	and put it in the transaction
 *	description record.
 *
 **************************************/

	// Look at the changed code: we don't support here more than 254 bytes in the path
	// so it's better to truncate or we'll have corrupt data in the trans desc record:
	// the length in one byte would wrap and we would copy more bytes that expected.
	// Our caller (prepare) assumed each call consumes at most 256 bytes (item, len, data)
	// hence if we don't check here, we have a B.O.
	TEXT* p = *ptr;
	Attachment* attachment = transaction->parent;
	*p++ = TDR_DATABASE_PATH;
	const TEXT* q = attachment->db_path.c_str();
	size_t len = strlen(q);
	if (len > 254)
		len = 254;
		
	*p++ = (TEXT) len;
	memcpy(p, q, len);
	*ptr = p + len;

	return FB_SUCCESS;
}


static const PTR get_entrypoint(int proc, int implementation)
{
/**************************************
 *
 *	g e t _ e n t r y p o i n t
 *
 **************************************
 *
 * Functional description
 *	Lookup entrypoint for procedure.
 *
 **************************************/

	const PTR* const entry = entrypoints + implementation * PROC_count + proc;
	return *entry ? *entry : &no_entrypoint;
}


static USHORT sqlda_buffer_size(USHORT min_buffer_size, XSQLDA * sqlda,
								USHORT dialect)
{
/**************************************
 *
 *	s q l d a _ b u f f e r _ s i z e
 *
 **************************************
 *
 * Functional description
 *	Calculate size of a buffer that is large enough
 *	to store the info items relating to an SQLDA.
 *
 **************************************/
	USHORT n_variables;
	USHORT sql_dialect;

/* If dialect / 10 == 0, then it has not been combined with the
   parser version for a prepare statement.  If it has been combined, then
   the dialect needs to be pulled out to compare to DIALECT_xsqlda
*/

	if ((sql_dialect = dialect / 10) == 0)
		sql_dialect = dialect;

	if (!sqlda)
		n_variables = 0;
	else if (sql_dialect >= DIALECT_xsqlda)
		n_variables = sqlda->sqln;
	else
		n_variables = ((SQLDA *) sqlda)->sqln;

	ULONG length = 32 + n_variables * 172;
	if (length < min_buffer_size)
		length = min_buffer_size;

	return (USHORT)((length > 65500L) ? 65500L : length);
}


static ISC_STATUS get_transaction_info(ISC_STATUS* user_status,
									   Transaction* transaction,
									   TEXT** ptr)
{
/**************************************
 *
 *	g e t _ t r a n s a c t i o n _ i n f o
 *
 **************************************
 *
 * Functional description
 *	Put a transaction's id into the transaction
 *	description record.
 *
 **************************************/
	Status status(user_status);

	try
	{
		TEXT buffer[16];
		TEXT* p = *ptr;

		if (CALL(PROC_TRANSACTION_INFO, transaction->implementation) (status,
																	  &transaction->
																	  handle,
																	  sizeof
																	  (prepare_tr_info),
																	  prepare_tr_info,
																	  sizeof
																	  (buffer),
																	  buffer)) 
		{
			return status[1];
		}

		const TEXT* q = buffer + 3;
		*p++ = TDR_TRANSACTION_ID;

		USHORT length = (USHORT)gds__vax_integer(reinterpret_cast<UCHAR*>(buffer + 1), 2);
				
		// Prevent information out of sync.
		if (length > MAX_UCHAR)
			length = MAX_UCHAR;
			
		*p++ = length;
		memcpy(p, q, length);
		*ptr = p + length;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


static void iterative_sql_info(ISC_STATUS * user_status,
							   FB_API_HANDLE* stmt_handle,
							   SSHORT item_length,
							   const SCHAR * items,
							   SSHORT buffer_length,
							   SCHAR* buffer,
							   USHORT dialect,
							   XSQLDA * sqlda)
{
/**************************************
 *
 *	i t e r a t i v e _ s q l _ i n f o
 *
 **************************************
 *
 * Functional description
 *	Turn an sql info buffer into an SQLDA.  If the info
 *	buffer was incomplete, make another request, beginning
 *	where the previous info call left off.
 *
 **************************************/
	USHORT last_index;
	SCHAR new_items[32];

	while (UTLD_parse_sql_info(	user_status,
								dialect,
								buffer,
								sqlda,
								&last_index) && last_index)
	{
		char* p = new_items;
		*p++ = isc_info_sql_sqlda_start;
		*p++ = 2;
		*p++ = last_index;
		*p++ = last_index >> 8;
		memcpy(p, items, (int) item_length);
		p += item_length;
		if (GDS_DSQL_SQL_INFO(	user_status,
								stmt_handle,
								(SSHORT) (p - new_items),
								new_items,
								buffer_length,
								buffer))
		{
			break;
		}
	}
}


static ISC_STATUS open_blob(ISC_STATUS* user_status,
							FB_API_HANDLE* db_handle,
							FB_API_HANDLE* tra_handle,
							FB_API_HANDLE* public_blob_handle,
							SLONG* blob_id,
							USHORT bpb_length,
							const UCHAR* bpb,
							SSHORT proc,
							SSHORT proc2)
{
/**************************************
 *
 *	o p e n _ b l o b
 *
 **************************************
 *
 * Functional description
 *	Open an existing blob (extended edition).
 *
 **************************************/
	YEntry status(user_status);
	StoredBlb* blob_handle = 0;
	try 
	{
		nullCheck(public_blob_handle, isc_bad_segstr_handle);

		Attachment* dbb = translate<Attachment>(db_handle);
		status.setPrimaryHandle(dbb);
		Transaction* transaction = findTransaction(tra_handle, dbb);

		USHORT flags = 0;
		USHORT from, to;
		gds__parse_bpb(bpb_length, bpb, &from, &to);

		if (get_entrypoint(proc2, dbb->implementation) != no_entrypoint &&
			CALL(proc2, dbb->implementation) (status,
											  &dbb->handle,
											  &transaction->handle,
											  &blob_handle,
											  blob_id,
											  bpb_length,
											  bpb) != isc_unavailable)
		{
			flags = 0;
		}
		else if (!to || from == to)
		{
			CALL(proc, dbb->implementation) (status,
											 &dbb->handle,
											 &transaction->handle,
											 &blob_handle, blob_id);
		}

		if (status[1]) {
			return status[1];
		}

		Blob* blob = new Blob(blob_handle, public_blob_handle, dbb);
		blob->flags |= flags;
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}


extern "C" {

static ISC_STATUS no_entrypoint(ISC_STATUS * user_status, ...)
{
/**************************************
 *
 *	n o _ e n t r y p o i n t
 *
 **************************************
 *
 * Functional description
 *	No_entrypoint is called if there is not entrypoint for a given routine.
 *
 **************************************/

	*user_status++ = isc_arg_gds;
	*user_status++ = isc_unavailable;
	*user_status = isc_arg_end;

	return isc_unavailable;
}

} // extern "C"

static ISC_STATUS prepare(ISC_STATUS* user_status,
						  Transaction* transaction)
{
/**************************************
 *
 *	p r e p a r e
 *
 **************************************
 *
 * Functional description
 *	Perform the first phase of a two-phase commit 
 *	for a multi-database transaction.
 *
 **************************************/
	Status status(user_status);

	Transaction* sub;
	TEXT tdr_buffer[1024];
	size_t length = 0;
	int transcount = 0;

	for (sub = transaction->next; sub; sub = sub->next)
	{
		length += 256;
		++transcount;
	}
	// To do: use transcount to check the maximum allowed dbs in a two phase commit.

	TEXT host[64];
	ISC_get_host(host, sizeof(host));
	const size_t hostlen = strlen(host);
	length += hostlen + 3; // TDR_version + TDR_host_site + UCHAR(strlen(host))
	
	TEXT* const description = (length > sizeof(tdr_buffer)) ?
		(TEXT *) gds__alloc(length) : tdr_buffer;

/* build a transaction description record containing 
   the host site and database/transaction
   information for the target databases. */

	TEXT* p = description;
	if (!p) 
	{
		status[0] = isc_arg_gds;
		status[1] = isc_virmemexh;
		status[2] = isc_arg_end;
		return status[1];
	}
	
	*p++ = TDR_VERSION;
	*p++ = TDR_HOST_SITE;
	*p++ = UCHAR(hostlen);
	memcpy(p, host, hostlen);
	p += hostlen;

/* Get database and transaction stuff for each sub-transaction */

	for (sub = transaction->next; sub; sub = sub->next) {
		get_database_info(status, sub, &p);
		get_transaction_info(status, sub, &p);
	}

/* So far so good -- prepare each sub-transaction */

	length = p - description;

	for (sub = transaction->next; sub; sub = sub->next)
	{
		if (CALL(PROC_PREPARE, sub->implementation) (status,
													 &sub->handle,
													 length, description))
		{
			if (description != tdr_buffer) {
				free_block(description);
			}
			return status[1];
		}
	}

	if (description != tdr_buffer)
		free_block(description);

	return FB_SUCCESS;
}


inline void why_priv_gds__free_if_set(SCHAR** pMem)
{
	if (*pMem) 
	{
		gds__free(*pMem);
		*pMem = 0;
	}
}

static void release_dsql_support(sqlda_sup& dasup)
{
/**************************************
 *
 *	r e l e a s e _ d s q l _ s u p p o r t
 *
 **************************************
 *
 * Functional description
 *	Release some memory.
 *
 **************************************/
	sqlda_sup::dasup_clause* pClauses = dasup.dasup_clauses;

	why_priv_gds__free_if_set(&pClauses[DASUP_CLAUSE_bind].dasup_blr);
	why_priv_gds__free_if_set(&pClauses[DASUP_CLAUSE_select].dasup_blr);
	why_priv_gds__free_if_set(&pClauses[DASUP_CLAUSE_bind].dasup_msg);
	why_priv_gds__free_if_set(&pClauses[DASUP_CLAUSE_select].dasup_msg);

	why_priv_gds__free_if_set(&pClauses[DASUP_CLAUSE_bind].dasup_info_buf);
	why_priv_gds__free_if_set(&pClauses[DASUP_CLAUSE_select].dasup_info_buf);
}


static void save_error_string(ISC_STATUS * status)
{
/**************************************
 *
 *	s a v e _ e r r o r _ s t r i n g  
 *
 **************************************
 *
 * Functional description
 *	This is need because there are cases
 *	where the memory allocated for strings 
 *	in the status vector is freed prior to 
 *	surfacing them to the user.  This is an
 *	attempt to save off 1 string to surface to
 *  	the user.  Any other strings will be set to
 *	a standard <Unknown> string.
 *
 **************************************/
	fb_assert(status != NULL);

	TEXT* p = glbstr1;
	ULONG len = sizeof(glbstr1) - 1;

	while (*status != isc_arg_end)
	{
		ULONG l;
		switch (*status++)
		{
		case isc_arg_cstring:
			l = (ULONG) * status;
			if (l < len)
			{
				status++;		/* Length is unchanged */
				/* 
				 * This strncpy should really be a memcpy
				 */
				strncpy(p, reinterpret_cast<char*>(*status), l);
				*status++ = (ISC_STATUS) p;	/* string in static memory */
				p += l;
				len -= l;
			}
			else {
				*status++ = (ISC_STATUS) strlen(glbunknown);
				*status++ = (ISC_STATUS) glbunknown;
			}
			break;

		case isc_arg_interpreted:
		case isc_arg_string:
		case isc_arg_sql_state:
			l = (ULONG) strlen(reinterpret_cast<char*>(*status)) + 1;
			if (l < len)
			{
				strncpy(p, reinterpret_cast<char*>(*status), l);
				*status++ = (ISC_STATUS) p;	/* string in static memory */
				p += l;
				len -= l;
			}
			else
			{
				*status++ = (ISC_STATUS) glbunknown;
			}
			break;

		default:
			fb_assert(FALSE);
		case isc_arg_gds:
		case isc_arg_number:
		case isc_arg_vms:
		case isc_arg_unix:
		case isc_arg_win32:
			status++;			/* Skip parameter */
			break;
		}
	}
}


static bool set_path(const Firebird::PathName& file_name, Firebird::PathName& expanded_name)
{
/**************************************
 *
 *	s e t _ p a t h
 *
 **************************************
 *
 * Functional description
 *	Set a prefix to a filename based on 
 *	the ISC_PATH user variable.
 *
 **************************************/

	// look for the environment variables to tack 
	// onto the beginning of the database path
	Firebird::PathName pathname;
	if (!fb_utils::readenv("ISC_PATH", pathname))
		return false;

	// if the file already contains a remote node
	// or any path at all forget it
	for (const TEXT* p = file_name.c_str(); *p; p++)
	{
		if (*p == ':' || *p == '/' || *p == '\\')
			return false;
	}

	// concatenate the strings

	expanded_name = pathname;

	// CVC: Make the concatenation work if no slash is present.
	char lastChar = expanded_name[expanded_name.length() - 1];
	if (lastChar != ':' && lastChar != '/' && lastChar != '\\') {
		expanded_name.append(1, PathUtils::dir_sep);
	}

	expanded_name.append(file_name);

	return true;
}


static void subsystem_enter(void) throw()
{
/**************************************
 *
 *	s u b s y s t e m _ e n t e r
 *
 **************************************
 *
 * Functional description
 *	Enter subsystem.
 *
 **************************************/

	try 
	{
#if !(defined SUPERCLIENT || defined SUPERSERVER)
		isc_enter_count++;
		if (subsystem_usage == 0 ||
			(subsystem_FPE_reset &
			(FPE_RESET_NEXT_API_CALL | FPE_RESET_ALL_API_CALL)))
		{
			ISC_enter();
			subsystem_FPE_reset &= ~FPE_RESET_NEXT_API_CALL;
		}
#endif

#ifdef DEBUG_FPE_HANDLING
		{
/* It's difficult to make a FPE to occur inside the engine - for debugging
 * just force one to occur every-so-often. */
			static ULONG counter = 0;
			if (((counter++) % 10) == 0)
			{
				fprintf(stderr, "Forcing FPE to occur within engine\n");
				kill(getpid(), SIGFPE);
			}
		}
#endif /* DEBUG_FPE_HANDLING */
	}
	catch(const Firebird::Exception&)
	{
		// ToDo: show full exception message here
		gds__log("Unexpected exception in subsystem_enter()");
	}
}


static void subsystem_exit(void) throw()
{
/**************************************
 *
 *	s u b s y s t e m _ e x i t
 *
 **************************************
 *
 * Functional description
 *	Exit subsystem.
 *
 **************************************/

	try
	{
#if !(defined SUPERCLIENT || defined SUPERSERVER)
		if (subsystem_usage == 0 ||
			(subsystem_FPE_reset &
			 (FPE_RESET_NEXT_API_CALL | FPE_RESET_ALL_API_CALL)))
		{
			ISC_exit();
		}
		isc_enter_count--;
#endif
	}
	catch(const Firebird::Exception&)
	{
		// ToDo: show full exception message here
		gds__log("Unexpected exception in subsystem_exit()");
	}
}


#if !defined (SUPERCLIENT)
bool WHY_set_shutdown(bool flag)
{
/**************************************
 *
 *	W H Y _ s e t _ s h u t d o w n
 *
 **************************************
 *
 * Functional description
 *	Set shutdown_flag to either TRUE or FALSE.
 *		TRUE = accept new connections
 *		FALSE= refuse new connections
 *	Returns the prior state of the flag (server).
 *
 **************************************/

	const bool old_flag = shutdown_flag;
	shutdown_flag = flag;
	return old_flag;
}

bool WHY_get_shutdown()
{
/**************************************
 *
 *	W H Y _ g e t _ s h u t d o w n
 *
 **************************************
 *
 * Functional description
 *	Returns the current value of shutdown_flag.
 *
 **************************************/

	return shutdown_flag;
}
#endif // !SUPERCLIENT


int API_ROUTINE fb_shutdown(unsigned int timeout)
{
/**************************************
 *
 *	f b _ s h u t d o w n
 *
 **************************************
 *
 * Functional description
 *	Shutdown firebird.
 *
 **************************************/
	YEntry status(NULL);
	int rc = FB_SUCCESS;

	try
	{
		// Shutdown clients before providers
		if (ShutChain::run(fb_shut_preproviders) != FB_SUCCESS)
		{
			return FB_FAILURE;	// Do not perform former shutdown
		}

		// Shutdown providers
		for (int n = 0; n < SUBSYSTEMS; ++n)
		{
			typedef int ShutType(unsigned int);
			PTR entry = get_entrypoint(PROC_SHUTDOWN, n);
			if (entry != no_entrypoint) 
			{
				// this awful cast will be gone as soon as we will have 
				// pure virtual functions based provider interface
				if (((ShutType*) entry)(timeout) != FB_SUCCESS)
				{
					rc = FB_FAILURE;
				}
			}
		}

		// Shutdown clients after providers
		if (ShutChain::run(fb_shut_postproviders) != FB_SUCCESS)
		{
			rc = FB_FAILURE;
		}
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
		gds__log_status(0, status);
		return FB_SUCCESS;	// This seems to be a strange logic, but we should better 
							// let it exit() when unexpected errors happen
	}

	return rc;
}


ISC_STATUS API_ROUTINE fb_shutdown_callback(ISC_STATUS* user_status, FPTR_INT callBack, const int mask)
{
/**************************************
 *
 *	f b _ s h u t d o w n _ c a l l b a c k
 *
 **************************************
 *
 * Functional description
 *	Register client callback to be called when FB is going down.
 *
 **************************************/
	YEntry status(user_status);
	try 
	{
		ShutChain::add(callBack, mask);
	}
	catch (const Firebird::Exception& e)
	{
		e.stuff_exception(status);
	}

	return status[1];
}
