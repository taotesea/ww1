//____________________________________________________________
//  
//		PROGRAM:	Gpre support
//		MODULE:		movg.cpp
//		DESCRIPTION:	Data mover and converter and comparator, etc.
//  
//  The contents of this file are subject to the Interbase Public
//  License Version 1.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy
//  of the License at http://www.Inprise.com/IPL.html
//  
//  Software distributed under the License is distributed on an
//  "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
//  or implied. See the License for the specific language governing
//  rights and limitations under the License.
//  
//  The Original Code was created by Inprise Corporation
//  and its predecessors. Portions created by Inprise Corporation are
//  Copyright (C) Inprise Corporation.
//  
//  All Rights Reserved.
//  Contributor(s): ______________________________________.
//  TMN (Mike Nordell) 11.APR.2001 - Reduce compiler warnings
//  
//
//____________________________________________________________
//
//	$Id: movg.cpp,v 1.3 2001-12-24 02:50:49 tamlin Exp $
//

#include "firebird.h"
#include "../jrd/common.h"
#include <stdarg.h>

#include "gen/codes.h"
#include "../jrd/iberr.h"
#include "../jrd/dsc.h"
#include "../gpre/movg_proto.h"
//  TMN: Unfortunately we need to include gpre.h before grep_proto.h since
//  the latter references a macro from the former, and doesn't include that
//  header file itself. We need to include gpre_proto.h since we use gpre
//  functions.
//  
#include "../gpre/gpre.h"
#include "../gpre/gpre_proto.h"
#include "../jrd/cvt_proto.h"
#include "../jrd/thd_proto.h"

static void post_error(void);


//____________________________________________________________
//  
//		Move (and possible convert) something to something else.
//  

void MOVG_move(DSC * from, DSC * to)
{

	CVT_move(from, to, (FPTR_VOID) post_error);
}


//____________________________________________________________
//  
//		A conversion error occurred.  Complain.
//  

static void post_error(void)
{

	CPR_error("conversion error: illegal string literal");
	CPR_abort();
}
