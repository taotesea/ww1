/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Vlad Horsun
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2008 Vlad Horsun <hvlad@users.sf.net>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 *
 *  Dmitry Yemanov <dimitr@users.sf.net>
 */

#ifndef COMMON_REF_COUNTED_H
#define COMMON_REF_COUNTED_H

#include "../common/classes/fb_atomic.h"
#include "../jrd/gdsassert.h"

namespace Firebird
{
	class RefCounted
	{
	public:
		virtual int addRef()
		{
			return ++m_refCnt;
		}

		virtual int release()
		{
			fb_assert(m_refCnt.value() > 0);
			const int refCnt = --m_refCnt;
			if (!refCnt)
				delete this;
			return refCnt;
		}

	protected:
		RefCounted() : m_refCnt(0) {}

		virtual ~RefCounted()
		{
			fb_assert(m_refCnt.value() == 0);
		}

	private:
		AtomicCounter m_refCnt;
	};

	// reference counted object guard
	class Reference
	{
	public:
		explicit Reference(RefCounted& refCounted) :
			r(refCounted)
		{
			r.addRef();
		}

		~Reference()
		{
			try {
				r.release();
			}
			catch (const Exception&)
			{
				DtorException::devHalt();
			}
		}

	private:
		RefCounted& r;
	};

	// controls reference counter of the object where points
	template <typename T>
	class RefPtr
	{
	public:
		RefPtr() : ptr(0)
		{ }

		explicit RefPtr(T* p) : ptr(p)
		{
			if (ptr)
			{
				ptr->addRef();
			}
		}

		~RefPtr()
		{
			if (ptr)
			{
				ptr->release();
			}
		}

		T* operator=(T* p)
		{
			return assign(p);
		}

		T* operator=(RefPtr& p)
		{
			return assign(p.ptr);
		}

		operator T*() 
		{
			return ptr;
		}

		T* operator->() 
		{
			return ptr;
		}

		operator const T*() const
		{
			return ptr;
		}

		const T* operator->() const
		{
			return ptr;
		}

	private:
		T* assign(T* const p)
		{
			if (ptr != p)
			{
				if (ptr)
				{
					ptr->release();
				}

				ptr = p;

				if (ptr)
				{
					ptr->addRef();
				}
			}

			return ptr;
		}

		T* ptr;
	};
} // namespace

#endif // COMMON_REF_COUNTED_H
