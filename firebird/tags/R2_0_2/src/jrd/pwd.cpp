/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		pwd.cpp
 *	DESCRIPTION:	User information database access
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
 * 2003.02.02 Dmitry Yemanov: Implemented cached security database connection
 */

#include "firebird.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "../jrd/common.h"
#include "../jrd/ibase.h"
#include "../jrd/jrd.h"
#include "../jrd/jrd_pwd.h"
#include "../jrd/enc_proto.h"
#include "../jrd/err_proto.h"
#include "../jrd/gds_proto.h"
#include "../jrd/sch_proto.h"
#include "../jrd/thd.h"
#include "../jrd/thread_proto.h"
#include "../jrd/jrd_proto.h"
#include "../common/config/config.h"
#include "../common/classes/objects_array.h"
#include "../common/classes/init.h"
#include "../common/classes/ClumpletWriter.h"

using namespace Jrd;

#ifdef SUPERSERVER
const bool SecurityDatabase::is_cached = true;
#else
const bool SecurityDatabase::is_cached = false;
#endif

// BLR to search database for user name record

const UCHAR SecurityDatabase::PWD_REQUEST[] = {
	blr_version5,
	blr_begin,
	blr_message, 1, 4, 0,
	blr_long, 0,
	blr_long, 0,
	blr_short, 0,
	blr_text, UCHAR(MAX_PASSWORD_LENGTH + 2), 0,
	blr_message, 0, 1, 0,
	blr_cstring, 129, 0,
	blr_receive, 0,
	blr_begin,
	blr_for,
	blr_rse, 1,
	blr_relation, 9, 'R', 'D', 'B', '$', 'U', 'S', 'E', 'R', 'S', 0,
	blr_first,
	blr_literal, blr_short, 0, 1, 0,
	blr_boolean,
	blr_eql,
	blr_field, 0, 13, 'R', 'D', 'B', '$', 'U', 'S', 'E', 'R', '_', 'N', 'A', 'M', 'E',
	blr_parameter, 0, 0, 0,
	blr_end,
	blr_send, 1,
	blr_begin,
	blr_assignment,
	blr_field, 0, 7, 'R', 'D', 'B', '$', 'G', 'I', 'D',
	blr_parameter, 1, 0, 0,
	blr_assignment,
	blr_field, 0, 7, 'R', 'D', 'B', '$', 'U', 'I', 'D',
	blr_parameter, 1, 1, 0,
	blr_assignment,
	blr_literal, blr_short, 0, 1, 0,
	blr_parameter, 1, 2, 0,
	blr_assignment,
	blr_field, 0, 10, 'R', 'D', 'B', '$', 'P', 'A', 'S', 'S', 'W', 'D',
	blr_parameter, 1, 3, 0,
	blr_end,
	blr_send, 1,
	blr_assignment,
	blr_literal, blr_short, 0, 0, 0,
	blr_parameter, 1, 2, 0,
	blr_end,
	blr_end,
	blr_eoc
};

// Transaction parameter buffer

const UCHAR SecurityDatabase::TPB[4] = {
	isc_tpb_version1,
	isc_tpb_read,
	isc_tpb_concurrency,
	isc_tpb_wait
};

// Static instance of the database

SecurityDatabase SecurityDatabase::instance;

#ifndef EMBEDDED
namespace {
#ifdef SUPERSERVER
// Disable attempts to brutforce logins/passwords
	class FailedLogin
	{
	public:
		Firebird::string login;
		int	failCount;
		time_t lastAttempt;
		FailedLogin(Firebird::MemoryPool& p) 
			: login(p), failCount(0), lastAttempt(time(0)) {}
		static const Firebird::string* generate(const void* sender, const FailedLogin* f)
		{
			return &(f->login);
		}
	};

	const size_t MAX_CONCURRENT_FAILURES = 16;
	const int MAX_FAILED_ATTEMPTS = 4;
	const int FAILURE_DELAY = 8; // seconds

	class FailedLogins : private Firebird::SortedObjectsArray<FailedLogin, 
		Firebird::InlineStorage<FailedLogin*, MAX_CONCURRENT_FAILURES>, 
		const Firebird::string, FailedLogin> 
	{
	private:
		// as long as we have voluntary threads scheduler,
		// this mutex should be entered AFTER that scheduler entered!
		Firebird::Mutex fullAccess;

		typedef Firebird::SortedObjectsArray<FailedLogin, 
			Firebird::InlineStorage<FailedLogin*, MAX_CONCURRENT_FAILURES>, 
			const Firebird::string, FailedLogin> inherited;

	public:
		FailedLogins(MemoryPool& p) : inherited(p) {}

		void loginFail(const Firebird::string& login)
		{
			//Firebird::MutexLockGuard(fullAccess);
			fullAccess.enter();
			FailedLogin& l = get(login);
			if (++l.failCount >= MAX_FAILED_ATTEMPTS)
			{
				sleepThread();
				l.failCount = 0;
			}
			fullAccess.leave();
		}

		void loginSuccess(const Firebird::string& login)
		{
			//Firebird::MutexLockGuard(fullAccess);
			fullAccess.enter();
			size_t pos;
			if (find(login, pos))
			{
				remove(pos);
			}
			fullAccess.leave();
		}

	private:
		FailedLogin& get(const Firebird::string& login)
		{
			size_t pos;
			if (find(login, pos))
			{
				(*this)[pos].lastAttempt = time(0);
				return (*this)[pos];
			}

checkForFreeSpace:
			if (getCount() >= MAX_CONCURRENT_FAILURES)
			{
				// try to perform old entries collection
				const time_t t = time(0);
				for (iterator i = begin(); i != end(); )
				{
					if (t - i->lastAttempt >= FAILURE_DELAY)
					{
						remove(i);
					}
					else
					{
						++i;
					}
				}
			}
			if (getCount() >= MAX_CONCURRENT_FAILURES)
			{
				// it seems we are under attack - too many wrong logins !!!
				// therefore sleep for a while and clear failures cache
				sleepThread();
				goto checkForFreeSpace;
			}

			FailedLogin& rc = add();
			rc.login = login;
			return rc;
		}

		void sleepThread()
		{
			THREAD_EXIT();
			fullAccess.leave();
			THREAD_SLEEP(1000 * FAILURE_DELAY);
			THREAD_ENTER();
			fullAccess.enter();
		}
	};
#else //SUPERSERVER
	// Unfortunately, in case of multi-process architectire, this doesn't work.
	class FailedLogins
	{
	public:
		FailedLogins(MemoryPool& p) {}
		void loginFail(const Firebird::string& login) { }
		void loginSuccess(const Firebird::string& login) {}
	};
#endif //SUPERSERVER

	Firebird::InitInstance<FailedLogins> usernameFailedLogins;
	Firebird::InitInstance<FailedLogins> remoteFailedLogins;
}
#endif //EMBEDDED

/******************************************************************************
 *
 *	Private interface
 */

void SecurityDatabase::fini()
{
	counter -= (is_cached) ? 1 : 0;
#ifndef EMBEDDED
	if (counter == 1 && lookup_db)
	{
		THREAD_EXIT();
		isc_detach_database(status, &lookup_db);
		THREAD_ENTER();
	}
#endif
}

void SecurityDatabase::init()
{
	counter += (is_cached) ? 1 : 0;
}

bool SecurityDatabase::lookup_user(TEXT * user_name, int *uid, int *gid, TEXT * pwd)
{
	bool found = false;		// user found flag
	TEXT uname[129];		// user name buffer
	user_record user;		// user record

	// Start by clearing the output data

	if (uid)
		*uid = 0;
	if (gid)
		*gid = 0;
	if (pwd)
		*pwd = '\0';

	strncpy(uname, user_name, sizeof uname);
	uname[sizeof uname - 1] = 0;

	THREAD_EXIT();
	mutex.acquire();
	THREAD_ENTER();

	// Attach database and compile request

	if (!prepare())
	{
		if (lookup_db)
		{
			isc_db_handle tmp = lookup_db;
			lookup_db = 0;
			isc_detach_database(status, &tmp);
		}
		THREAD_ENTER();
		mutex.release();
		ERR_post(isc_psw_attach, 0);
	}

	// Lookup

	isc_tr_handle lookup_trans = 0;

	if (isc_start_transaction(status, &lookup_trans, 1, &lookup_db, sizeof(TPB), TPB))
	{
		THREAD_ENTER();
		mutex.release();
		ERR_post(isc_psw_start_trans, 0);
	}

	if (!isc_start_and_send(status, &lookup_req, &lookup_trans, 0, sizeof(uname), uname, 0))
	{
		while (true)
		{
			isc_receive(status, &lookup_req, 1, sizeof(user), &user, 0);
			if (!user.flag || status[1])
				break;
			found = true;
			if (uid)
				*uid = user.uid;
			if (gid)
				*gid = user.gid;
			if (pwd) 
			{
				strncpy(pwd, user.password, MAX_PASSWORD_LENGTH);
				pwd[MAX_PASSWORD_LENGTH] = 0;
			}
		}
	}

	isc_rollback_transaction(status, &lookup_trans);

	if (!is_cached)
	{
		isc_detach_database(status, &lookup_db);
	}
	THREAD_ENTER();
	mutex.release();

	return found;
}

bool SecurityDatabase::prepare()
{
	TEXT user_info_name[MAXPATHLEN];

	if (lookup_db)
	{
		THREAD_EXIT();
		return true;
	}

	THREAD_EXIT();

	lookup_db = lookup_req = 0;

	// Initialize the database name
	getPath(user_info_name);

	// Perhaps build up a dpb
	Firebird::ClumpletWriter dpb(Firebird::ClumpletReader::Tagged, MAX_DPB_SIZE, isc_dpb_version1);

	// Insert username
	const char* szAuthenticator = "authenticator";
	dpb.insertString(isc_dpb_user_name, 
		szAuthenticator, strlen(szAuthenticator));

	// Insert password
	const char* szPassword = "none";
	dpb.insertString(isc_dpb_password, 
		szPassword, strlen(szPassword));

	// Attachment is for the security database
	dpb.insertByte(isc_dpb_sec_attach, TRUE);

	// Temporarily disable security checks for this thread
	JRD_thread_security_disable(true);

	isc_attach_database(status, 0, user_info_name, &lookup_db, 
		dpb.getBufferLength(), 
		reinterpret_cast<const char*>(dpb.getBuffer()));

	if (status[1])
	{
		JRD_thread_security_disable(false);
		char buffer[1024];
		const ISC_STATUS *s = status;
		if (fb_interpret(buffer, sizeof buffer, &s))
		{
			gds__log(buffer);
			while (fb_interpret(buffer, sizeof buffer, &s))
				gds__log(buffer);
		}
		return false;
	}

	isc_compile_request(status, &lookup_db, &lookup_req, sizeof(PWD_REQUEST),
						reinterpret_cast<const char*>(PWD_REQUEST));

	JRD_thread_security_disable(false);

	if (status[1])
	{
		char buffer[1024];
		const ISC_STATUS *s = status;
		if (fb_interpret(buffer, sizeof buffer, &s))
		{
			gds__log(buffer);
			while (fb_interpret(buffer, sizeof buffer, &s))
				gds__log(buffer);
		}
		return false;
	}

	return true;
}

/******************************************************************************
 *
 *	Public interface
 */

void SecurityDatabase::initialize()
{
	instance.init();
}

void SecurityDatabase::shutdown()
{
	instance.fini();
}

void SecurityDatabase::verifyUser(TEXT* name,
								  const TEXT* user_name,
								  const TEXT* password,
								  const TEXT* password_enc,
								  int* uid,
								  int* gid,
								  int* node_id, 
								  const Firebird::string& remoteId)
{
	if (user_name)
	{
		TEXT* p = name;
		for (const TEXT* q = user_name; *q; ++q, ++p)
		{
			*p = UPPER7(*q);
		}
		*p = 0;
	}

#ifndef EMBEDDED

	// Look up the user name in the userinfo database and use the parameters
	// found there. This means that another database must be accessed, and
	// that means the current context must be saved and restored.

	TEXT pw1[MAX_PASSWORD_LENGTH + 1];
	const bool found = instance.lookup_user(name, uid, gid, pw1);
	pw1[MAX_PASSWORD_LENGTH] = 0;
	Firebird::string storedHash(pw1, MAX_PASSWORD_LENGTH);
	storedHash.rtrim();

	// Punt if the user has specified neither a raw nor an encrypted password,
	// or if the user has specified both a raw and an encrypted password, 
	// or if the user name specified was not in the password database
	// (or if there was no password database - it's still not found)

	if ((!password && !password_enc) || (password && password_enc) || !found)
	{
		usernameFailedLogins().loginFail(name);
		remoteFailedLogins().loginFail(remoteId);
		ERR_post(isc_login, 0);
	}

	TEXT pwt[MAX_PASSWORD_LENGTH + 2];
	if (password) 
	{
		ENC_crypt(pwt, sizeof pwt, password, PASSWORD_SALT);
		password_enc = pwt + 2;
	}

	Firebird::string newHash;
	hash(newHash, name, password_enc, storedHash);
	if (newHash != storedHash)
	{
		bool legacyHash = Config::getLegacyHash();
		if (legacyHash)
		{
			newHash.resize(MAX_PASSWORD_LENGTH + 2);
			ENC_crypt(newHash.begin(), newHash.length(), password_enc, PASSWORD_SALT);
			newHash.recalculate_length();
			newHash.erase(0, 2);
			legacyHash = newHash == storedHash;
		}
		if (! legacyHash) 
		{
			usernameFailedLogins().loginFail(name);
			remoteFailedLogins().loginFail(remoteId);
			ERR_post(isc_login, 0);
		}
	}

	usernameFailedLogins().loginSuccess(name);
	remoteFailedLogins().loginSuccess(remoteId);
#endif

	*node_id = 0;
}
