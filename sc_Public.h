// ***********************************************************************
// Copyright © 2018 Shawn Amir
// All rights reserved
// ***********************************************************************

// ***********************************************************************
// 1/1/2018	Created
// ***********************************************************************

// ***********************************************************************
// sc_Public.h
// General type defs used everywhere.
// ***********************************************************************

#ifndef	_G_PublicTypes_Header_
#define	_G_PublicTypes_Header_

    #include <stdint.h>
    #include <stdlib.h>
    #include <stdio.h>
    #include <string.h>
    #include <errno.h>
    #include <time.h>


    typedef int64_t		Int64;
    typedef int32_t	        Int32;
    typedef int16_t 	        Int16;
    typedef int8_t		Int8;
    typedef int_least32_t	IntX;			// At least 32 bits, maybe more

    typedef uint64_t        	Uns64;	
    typedef uint32_t        	Uns32;
    typedef uint16_t        	Uns16;
    typedef uint8_t 		Uns8;
    typedef uint_least32_t	UnsX;			// At least 32 bits, maybe more

#endif	// _G_PublicTypes_Header_
