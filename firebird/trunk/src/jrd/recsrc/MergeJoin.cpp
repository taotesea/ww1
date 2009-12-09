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

#include "firebird.h"
#include "../jrd/common.h"
#include "../jrd/jrd.h"
#include "../jrd/req.h"
#include "../jrd/rse.h"
#include "../jrd/cmp_proto.h"
#include "../jrd/evl_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/sort_proto.h"

#include "RecordSource.h"

using namespace Firebird;
using namespace Jrd;

static const char* const SCRATCH = "fb_merge_";

// -----------------------
// Data access: merge join
// -----------------------

MergeJoin::MergeJoin(CompilerScratch* csb, size_t count, SortedStream* const* args)
	: m_args(csb->csb_pool)
{
	m_impure = CMP_impure(csb, (USHORT) (sizeof(struct Impure) + count * sizeof(Impure::irsb_mrg_repeat)));

	m_args.resize(count);

	for (size_t i = 0; i < count; i++)
	{
		m_args[i] = args[i];
	}
}

void MergeJoin::open(thread_db* tdbb)
{
	jrd_req* const request = tdbb->getRequest();
	Impure* const impure = (Impure*) ((UCHAR*) request + m_impure);

	impure->irsb_flags = irsb_open;

	for (size_t i = 0; i < m_args.getCount(); i++)
	{
		Impure::irsb_mrg_repeat* const tail = &impure->irsb_mrg_rpt[i];

		// open all the substreams for the sort-merge

		m_args[i]->open(tdbb);

		// reset equality group record positions

		tail->irsb_mrg_equal = -1;
		tail->irsb_mrg_equal_end = -1;
		tail->irsb_mrg_equal_current = -1;
		tail->irsb_mrg_last_fetched = -1;
		tail->irsb_mrg_order = tail - impure->irsb_mrg_rpt;

		MergeFile* const mfb = &tail->irsb_mrg_file;
		mfb->mfb_equal_records = 0;
		mfb->mfb_current_block = 0;
		mfb->mfb_record_size = ROUNDUP(m_args[i]->getLength(), FB_ALIGNMENT);
		mfb->mfb_block_size = MAX(mfb->mfb_record_size, MERGE_BLOCK_SIZE);
		mfb->mfb_blocking_factor = mfb->mfb_block_size / mfb->mfb_record_size;
		if (!mfb->mfb_block_data)
		{
			mfb->mfb_block_data = FB_NEW(*request->req_pool) UCHAR[mfb->mfb_block_size];
		}
	}
}

void MergeJoin::close(thread_db* tdbb)
{
	jrd_req* const request = tdbb->getRequest();

	invalidateRecords(request);

	Impure* const impure = (Impure*) ((UCHAR*) request + m_impure);

	if (impure->irsb_flags & irsb_open)
	{
		impure->irsb_flags &= ~irsb_open;

		for (size_t i = 0; i < m_args.getCount(); i++)
		{
			Impure::irsb_mrg_repeat* const tail = &impure->irsb_mrg_rpt[i];

			// close all the substreams for the sort-merge

			m_args[i]->close(tdbb);

			// Release memory associated with the merge file block and the sort file block.
			// Also delete the merge file if one exists.

			MergeFile* const mfb = &tail->irsb_mrg_file;
			delete mfb->mfb_space;
			mfb->mfb_space = NULL;

			delete[] mfb->mfb_block_data;
			mfb->mfb_block_data = NULL;
		}
	}
}

bool MergeJoin::getRecord(thread_db* tdbb)
{
	jrd_req* const request = tdbb->getRequest();
	Impure* const impure = (Impure*) ((UCHAR*) request + m_impure);

	if (!(impure->irsb_flags & irsb_open))
	{
		return false;
	}

	// If there is a record group already formed, fetch the next combination

	if (fetchRecord(tdbb, m_args.getCount() - 1))
	{
		return true;
	}

	// Assuming we are done with the current value group, advance each
	// stream one record. If any comes up dry, we're done.
	SortedStream** highest_ptr = m_args.begin();

	for (size_t i = 0; i < m_args.getCount(); i++)
	{
		SortedStream** const ptr = &m_args[i];
		SortedStream* const sort_rsb = *ptr;
		Impure::irsb_mrg_repeat* const tail = &impure->irsb_mrg_rpt[i];

		MergeFile* const mfb = &tail->irsb_mrg_file;

		// reset equality group record positions

		tail->irsb_mrg_equal = 0;
		tail->irsb_mrg_equal_current = 0;
		tail->irsb_mrg_equal_end = 0;

		// If there is a record waiting, use it. Otherwise get another.

		SLONG record = tail->irsb_mrg_last_fetched;
		if (record >= 0)
		{
			tail->irsb_mrg_last_fetched = -1;
			const UCHAR* const last_data = getData(tdbb, mfb, record);
			mfb->mfb_current_block = 0;
			UCHAR* const first_data = getData(tdbb, mfb, 0);
			if (first_data != last_data)
			{
				memcpy(first_data, last_data, sort_rsb->getLength());
			}
			mfb->mfb_equal_records = 1;
			record = 0;
		}
		else
		{
			mfb->mfb_current_block = 0;
			mfb->mfb_equal_records = 0;
			if ((record = getRecord(tdbb, i)) < 0)
			{
				return false;
			}
		}

		// map data into target records and do comparison

		sort_rsb->mapData(tdbb, request, getData(tdbb, mfb, record));
		if (ptr != highest_ptr && compare(tdbb, (jrd_nod*) highest_ptr[1], (jrd_nod*) ptr[1]) < 0)
		{
			highest_ptr = ptr;
		}
	}

	// Loop thru the streams advancing each up to the target value.
	// If any exceeds the target value, start over.

	while (true)
	{
		bool recycle = false;

		for (size_t i = 0; i < m_args.getCount(); i++)
		{
			SortedStream** const ptr = &m_args[i];
			Impure::irsb_mrg_repeat* const tail = &impure->irsb_mrg_rpt[i];

			if (highest_ptr != ptr)
			{
				int result;
				while ( (result = compare(tdbb, (jrd_nod*) highest_ptr[1], (jrd_nod*) ptr[1])) )
				{
					if (result < 0)
					{
						highest_ptr = ptr;
						recycle = true;
						break;
					}
					SortedStream* const sort_rsb = *ptr;
					MergeFile* const mfb = &tail->irsb_mrg_file;
					mfb->mfb_current_block = 0;
					mfb->mfb_equal_records = 0;

					const SLONG record = getRecord(tdbb, i);
					if (record < 0)
					{
						return false;
					}
					sort_rsb->mapData(tdbb, request, getData(tdbb, mfb, record));
				}

				if (recycle)
				{
					break;
				}
			}
		}

		if (!recycle)
		{
			break;
		}
	}

	// finally compute equality group for each stream in sort/merge

	for (size_t i = 0; i < m_args.getCount(); i++)
	{
		SortedStream* const sort_rsb = m_args[i];
		Impure::irsb_mrg_repeat* const tail = &impure->irsb_mrg_rpt[i];

		MergeFile* const mfb = &tail->irsb_mrg_file;

		HalfStaticArray<ULONG, 64> key;
		const USHORT smb_key_length = sort_rsb->getKeyLength();
		ULONG* const first_data = key.getBuffer(smb_key_length);
		const ULONG key_length = smb_key_length * sizeof(ULONG);
		memcpy(first_data, getData(tdbb, mfb, 0), key_length);

		SLONG record;
		while ((record = getRecord(tdbb, i)) >= 0)
		{
			const SLONG* p = (SLONG*) first_data;
			const SLONG* q = (SLONG*) getData(tdbb, mfb, record);
			bool equal = true;

			for (USHORT count = smb_key_length; count; p++, q++, count--)
			{
				if (*p != *q)
				{
					tail->irsb_mrg_last_fetched = record;
					equal = false;
					break;
				}
			}

			if (!equal)
			{
				break;
			}

			tail->irsb_mrg_equal_end = record;
		}

		if (mfb->mfb_current_block)
		{
			if (!mfb->mfb_space)
			{
				MemoryPool& pool = *getDefaultMemoryPool();
				mfb->mfb_space = FB_NEW(pool) TempSpace(pool, SCRATCH);
			}

			SORT_write_block(mfb->mfb_space, mfb->mfb_block_size * mfb->mfb_current_block, mfb->mfb_block_data, mfb->mfb_block_size);
		}
	}

	// Optimize cross product of equivalence groups by ordering the streams
	// from left (outermost) to right (innermost) by descending cardinality
	// of merge blocks. This ordering will vary for each set of equivalence
	// groups and cannot be statically assigned by the optimizer.

	typedef Stack<Impure::irsb_mrg_repeat*> ImrStack;
	ImrStack best_tails;

	Impure::irsb_mrg_repeat* tail = impure->irsb_mrg_rpt;
	for (const Impure::irsb_mrg_repeat* const tail_end = tail + m_args.getCount();
		 tail < tail_end; tail++)
	{
		Impure::irsb_mrg_repeat* best_tail = NULL;

		ULONG most_blocks = 0;
		for (Impure::irsb_mrg_repeat* tail2 = impure->irsb_mrg_rpt; tail2 < tail_end; tail2++)
		{
			ImrStack::iterator stack(best_tails);
			for (; stack.hasData(); ++stack)
			{
				if (stack.object() == tail2)
				{
					break;
				}
			}

			if (stack.hasData())
			{
				continue;
			}

			MergeFile* const mfb = &tail2->irsb_mrg_file;
			ULONG blocks = mfb->mfb_equal_records / mfb->mfb_blocking_factor;
			if (++blocks > most_blocks)
			{
				most_blocks = blocks;
				best_tail = tail2;
			}
		}

		best_tails.push(best_tail);
		tail->irsb_mrg_order = best_tail - impure->irsb_mrg_rpt;
	}

	return true;
}

bool MergeJoin::refetchRecord(thread_db* tdbb)
{
	return true;
}

bool MergeJoin::lockRecord(thread_db* tdbb)
{
	status_exception::raise(Arg::Gds(isc_record_lock_not_supp));
	return false; // compiler silencer
}

void MergeJoin::dump(thread_db* tdbb, UCharBuffer& buffer)
{
	buffer.add(isc_info_rsb_begin);

	buffer.add(isc_info_rsb_type);
	buffer.add(isc_info_rsb_merge);

	const size_t count = m_args.getCount();
	// This place must be reviewed if we allow more than 255 joins
	fb_assert(count <= USHORT(MAX_UCHAR));
	buffer.add((UCHAR) count);

	for (size_t i = 0; i < count; i++)
	{
		m_args[i]->dump(tdbb, buffer);
	}

	buffer.add(isc_info_rsb_end);
}

void MergeJoin::markRecursive()
{
	for (size_t i = 0; i < m_args.getCount(); i++)
	{
		m_args[i]->markRecursive();
	}
}

void MergeJoin::findUsedStreams(UCHAR* streams)
{
	for (size_t i = 0; i < m_args.getCount(); i++)
	{
		m_args[i]->findUsedStreams(streams);
	}
}

void MergeJoin::invalidateRecords(jrd_req* request)
{
	for (size_t i = 0; i < m_args.getCount(); i++)
	{
		m_args[i]->invalidateRecords(request);
	}
}

void MergeJoin::nullRecords(thread_db* tdbb)
{
	for (size_t i = 0; i < m_args.getCount(); i++)
	{
		m_args[i]->nullRecords(tdbb);
	}
}

void MergeJoin::saveRecords(thread_db* tdbb)
{
	for (size_t i = 0; i < m_args.getCount(); i++)
	{
		m_args[i]->saveRecords(tdbb);
	}
}

void MergeJoin::restoreRecords(thread_db* tdbb)
{
	for (size_t i = 0; i < m_args.getCount(); i++)
	{
		m_args[i]->restoreRecords(tdbb);
	}
}

int MergeJoin::compare(thread_db* tdbb, jrd_nod* node1, jrd_nod* node2)
{
	jrd_req* const request = tdbb->getRequest();

	jrd_nod* const* ptr1 = node1->nod_arg;
	jrd_nod* const* ptr2 = node2->nod_arg;

	for (const jrd_nod* const* const end = ptr1 + node1->nod_count; ptr1 < end; ptr1++, ptr2++)
	{
		const dsc* const desc1 = EVL_expr(tdbb, *ptr1);
		const ULONG flags = request->req_flags;
		const dsc* const desc2 = EVL_expr(tdbb, *ptr2);

		if (flags & req_null)
		{
			if (!(request->req_flags & req_null))
			{
				return -1;
			}

			// AB: When both expression evaluated NULL then
			// we return 0 ( (NULL = NULL) = true).
			//
			// Currently this (0 and higher) isn't used by the
			// MERGE procedure, but when we allow MERGE to
			// handle outer-joins we must not forget this one !!!
			return 0;
		}

		if (request->req_flags & req_null)
		{
			return 1;
		}

		// AB: MOV_compare can't handle NULL parameters
		// therefore check before passing all null flags.
		const int result = MOV_compare(desc1, desc2);
		if (result != 0)
		{
			return result;
		}
	}

	return 0;
}

UCHAR* MergeJoin::getData(thread_db* tdbb, MergeFile* mfb, SLONG record)
{
	fb_assert(record >= 0 && record < (SLONG) mfb->mfb_equal_records);

	const ULONG merge_block = record / mfb->mfb_blocking_factor;
	if (merge_block != mfb->mfb_current_block)
	{
		SORT_read_block(mfb->mfb_space, mfb->mfb_block_size * merge_block, mfb->mfb_block_data, mfb->mfb_block_size);
		mfb->mfb_current_block = merge_block;
	}

	const ULONG merge_offset = (record % mfb->mfb_blocking_factor) * mfb->mfb_record_size;
	return mfb->mfb_block_data + merge_offset;
}

SLONG MergeJoin::getRecord(thread_db* tdbb, size_t index)
{
	jrd_req* const request = tdbb->getRequest();
	Impure* const impure = (Impure*) ((UCHAR*) request + m_impure);

	SortedStream* const sort_rsb = m_args[index];
	Impure::irsb_mrg_repeat* const tail = &impure->irsb_mrg_rpt[index];

	UCHAR* sort_data = sort_rsb->getData(tdbb);
	if (!sort_data)
	{
		return -1;
	}

	MergeFile* const mfb = &tail->irsb_mrg_file;
	const SLONG record = mfb->mfb_equal_records;

	const ULONG merge_block = record / mfb->mfb_blocking_factor;
	if (merge_block != mfb->mfb_current_block)
	{
		if (!mfb->mfb_space)
		{
			MemoryPool& pool = *getDefaultMemoryPool();
			mfb->mfb_space = FB_NEW(pool) TempSpace(pool, SCRATCH);
		}

		SORT_write_block(mfb->mfb_space, mfb->mfb_block_size * mfb->mfb_current_block, mfb->mfb_block_data, mfb->mfb_block_size);
		mfb->mfb_current_block = merge_block;
	}

	const ULONG merge_offset = (record % mfb->mfb_blocking_factor) * mfb->mfb_record_size;
	UCHAR* merge_data = mfb->mfb_block_data + merge_offset;

	memcpy(merge_data, sort_data, sort_rsb->getLength());
	++mfb->mfb_equal_records;

	return record;
}

bool MergeJoin::fetchRecord(thread_db* tdbb, size_t index)
{
	jrd_req* const request = tdbb->getRequest();
	Impure* const impure = (Impure*) ((UCHAR*) request + m_impure);
	Impure::irsb_mrg_repeat* tail = &impure->irsb_mrg_rpt[index];

	const SSHORT m = tail->irsb_mrg_order;
	tail = &impure->irsb_mrg_rpt[m];
	SortedStream* const sort_rsb = m_args[m];

	SLONG record = tail->irsb_mrg_equal_current;
	++record;

	if (record > tail->irsb_mrg_equal_end)
	{
		if (index == 0 || !fetchRecord(tdbb, index - 1))
		{
			return false;
		}
		record = tail->irsb_mrg_equal;
	}

	tail->irsb_mrg_equal_current = record;

	MergeFile* const mfb = &tail->irsb_mrg_file;
	sort_rsb->mapData(tdbb, request, getData(tdbb, mfb, record));

	return true;
}

