// ***********************************************************************
// Copyright © 2018 Shawn Amir
// All rights reserved
// ***********************************************************************

// ***********************************************************************
// 1/1/2018	Created
// ***********************************************************************

// ***********************************************************************
// sc_Error.h
// Error handling and debugging checks
// ***********************************************************************

#ifndef _G_Error_Header_
#define _G_Error_Header_

	#include <signal.h>

	// Always place G_MAINFILEDEBUG in the main application file.

	typedef struct _G_ErrorRecord {
	    char *	StrP;
	    Int32	Val;
	    Int32	Line;
	} G_ErrorRecord, *G_ErrorPointer;

	extern	G_ErrorRecord			G_GlobalErrRec;


#ifdef DEBUG	
	#define	G_MAINFILEERROR_INIT								\
	    do {G_GlobalErrRec.StrP = NULL;							\
		G_GlobalErrRec.Val = 0;								\
		G_GlobalErrRec.Line = 0;							\
		setbuf(stdout, NULL);								\
	    } while (0)
#else
	#define	G_MAINFILEERROR_INIT								\
	    do {G_GlobalErrRec.StrP = NULL;							\
		G_GlobalErrRec.Val = 0;								\
		G_GlobalErrRec.Line = 0;							\
	    } while (0)
#endif
	
	#define G_SETERROR(E, V)								\
	    do {G_GlobalErrRec.StrP = E;							\
	        G_GlobalErrRec.Val = V;								\
	        G_GlobalErrRec.Line = __LINE__;							\
	        fprintf(stderr, "Error -->%s:%d [%s:%d]\n", __FILE__,  __LINE__, E, V);		\
	    } while (0)

	#define G_SETERROR_S(E, S, V)								\
	    do {G_GlobalErrRec.StrP = E;							\
	        G_GlobalErrRec.Val = V;								\
	        G_GlobalErrRec.Line = __LINE__;							\
	        fprintf(stderr, "Error -->%s:%d [%s, %s:%d]\n", __FILE__,  __LINE__, E, S, V);		\
	    } while (0)
	    

	#define G_SETEXCEPTION(E, V)								\
	    do {G_GlobalErrRec.StrP = E;							\
	        G_GlobalErrRec.Val = V;								\
	        G_GlobalErrRec.Line = __LINE__;							\
	        fprintf(stderr, "Exception -->%s:%d [%s:%d]\n", __FILE__, __LINE__, E, V);	\
		raise(SIGABRT);									\
	    } while (0)

	#define G_SETEXCEPTION_L(E, V)								\
	    do {G_GlobalErrRec.StrP = E;							\
	        G_GlobalErrRec.Val = V;								\
	        G_GlobalErrRec.Line = __LINE__;							\
	        fprintf(stderr, "Exception -->%s:%d [%s:%ld]\n", __FILE__, __LINE__, E, V);	\
		raise(SIGABRT);									\
	    } while (0)

	#ifdef DEBUG

		// C is the condition, scream if false.
		// E is the error type, e.g. "NewRefCount is already 0!"
		// A1, A2 are the arguments.  The specific line number is
		// always the fourth element in the error arr.

		#define G_ASSERT(C, E, V)							\
			do {if (!(C)) {G_GlobalErrRec.StrP = E;					\
				       G_GlobalErrRec.Val = V;					\
				       G_GlobalErrRec.Line = __LINE__;				\
				       fprintf(stderr, "Assert -->%s:%d [%s:%d]\n",  __FILE__,  __LINE__, E, V);    \
				       raise(SIGABRT);						\
			   }									\
			} while (0)

		#define G_DEBUG(E, V)								\
			do {G_GlobalErrRec.StrP = E;						\
			    G_GlobalErrRec.Val = V;						\
			    G_GlobalErrRec.Line = __LINE__;					\
			    fprintf(stderr, "Debug -->%s:%d [%s:%d]\n",				\
				    __FILE__, __LINE__, E, V);				       	\
			    raise(SIGABRT);							\
			} while (0)
			
	#else	// _DEBUG
	
		#define	G_ASSERT(C, E, Val)
		#define	G_DEBUG(E, Val)
	
	#endif // _DEBUG

#endif	// _G_Error_Header_












