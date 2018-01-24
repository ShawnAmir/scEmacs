// ***********************************************************************
// Copyright © 2018 Shawn Amir
// All Rights Reserved
// ***********************************************************************
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3 or later of the License.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
// ***********************************************************************
// 1/1/2018	Created
// ***********************************************************************

// ***********************************************************************
// sc_Main.c
// Main module for scEmacs
// ***********************************************************************

#include	<locale.h>
#include	<X11/Xlib.h>
#include	<X11/Xutil.h>
#include	<X11/Xatom.h>
#include	<poll.h>
#include	<sys/time.h>

#include	<X11/Xft/Xft.h>
#include	<X11/extensions/Xrender.h>	// XRenderColor

#include	"sc_Public.h"
#include	"sc_Error.h"
#include	"sc_Editor.h"
#include	"sc_Main.h"

// ***********************************************************************

typedef struct _sc_TimerRecord {			// For BlinkTimer
    struct pollfd		PollEvent;
    struct timeval		SetTime;
} sc_TimerRecord, *sc_TimerPointer;

typedef void (*sc_EventFPointer)(XEvent *, void *);


#define	_sc_WEREGPOINTER struct _sc_WERegRecord *
    typedef struct _sc_WERegRecord {
	Uns64			Ident;			// Window Ident
	_sc_WEREGPOINTER	NextP;			// Chain
	sc_EventFPointer	EventFP;		// Event handler
	void *			DataP;			// Extra arg
    } sc_WERegRecord, *sc_WERegPointer;
#undef _sc_WEREGPOINTER

// ***********************************************************************

G_ErrorRecord  	G_GlobalErrRec;			// Stashes error info

Display *	XDispP;				// Main X Display
Int16		XScreenN;			// X Screen numb for windows
Uns16		XDispHeight, XDispWidth;	// Size of main screen

XftFont *	XftFontP;
char *		XftFontName = "Ubuntu mono-13:weight=medium:slant=roman";
XftDraw *	XftDrawP;
XIM		XIMP;
XIC		XICP;

#define		sc_WEREGCOUNT		(1 << 7)
#define		sc_WEREGMASK		(sc_WEREGCOUNT - 1)

sc_WERegPointer	sc_WERegArr[sc_WEREGCOUNT];
sc_SAStore	sc_WERegStore;

#define		sc_BLINKINTERVAL	500	// Millisec
sc_TimerRecord	sc_BlinkTimerR;
Int16		sc_BlinkerOn;
Int16		sc_DoBlink;

Int16		sc_MainContinue;

// ***********************************************************************

void		sc_SAStoreAddRack(sc_SAStorePointer StoreP);

void		sc_InitBlinkTimer(void);
void		sc_ResetBlinkTimer(void);
Uns16		sc_DoBlinkTimer(Int32 MilliSecs);

void		sc_WERegInit(void);
void		sc_WERegKill(void);
void		sc_WERegAdd(Uns64 Id, void * FP, void * DataP);
void		sc_WERegDel(Uns64 Id);
void		sc_WERegDispatch(Uns64 Id, XEvent * EventP);


// ******************************************************************************
// ******************************************************************************
// STORE (Slab) ALLOCATOR (SA)
//
// The program typically needs N data blocks (records) of type A and M data blocks
// (records) of type B....  It is easy to allocate these blocks individually,
// but that can cause fragmentation as the blocks malloc and free at different
// times.  Instead, the program creates a STORE (global) for each BLOCK type,
// and SA allocates 1 or more RACKS to hold these BLOCKS.  Each time a new
// data Block (record) is needed, it is sub-allocated from the matching
// RACK (or list of RACKS).
//
// sc_SAStoreOpen initializes the Store of the given type (different Stores are
// initialized independently).  A single Rack is initially allocated for the
// Store, more are added as needed.
//
// Everything works better if BytesPerBlock is factor of 8 (longlong aligned).

void		sc_SAStoreOpen(sc_SAStorePointer StoreP, Uns32 BytesPerBlock, Uns32 BlocksPerRack)
{
    BytesPerBlock = (BytesPerBlock + 7) & ~7L;

    StoreP->FirstRackP = NULL;
    StoreP->EmptyBlockP = NULL;
    StoreP->BytesPerBlock = BytesPerBlock;
    StoreP->BlocksPerRack = BlocksPerRack;
    StoreP->UsedBlocks = 0L;
    StoreP->FreeBlocks = 0L;
    StoreP->RackCount = 0L;
    StoreP->Flags = sc_SASTORENOFLAG;

    sc_SAStoreAddRack(StoreP);
}

// ******************************************************************************
// sc_SAStoreAddRack is called to create, init, and add a new Rack to the Store.
// This function is called to initialize the store and automatically if more
// racks are needed when allocating blocks.  There is really no need to call
// this explicitly from client code.

void		sc_SAStoreAddRack(sc_SAStorePointer StoreP)
{
    sc_SARackPointer	RackP;
    void **		BPP;
    Uns32		I;

    RackP = (sc_SARack *)malloc((StoreP->BytesPerBlock * StoreP->BlocksPerRack) + sizeof(sc_SARack));
    if (RackP == NULL) G_SETEXCEPTION("SA Store AddRack failed", StoreP->RackCount + 1);

    if (StoreP->FirstRackP) StoreP->FirstRackP->PrevP = RackP;
    RackP->NextP = StoreP->FirstRackP;
    RackP->PrevP = NULL;
    StoreP->FirstRackP = RackP;

    StoreP->FreeBlocks += StoreP->BlocksPerRack;
    StoreP->RackCount += 1L;

    // First fill Rack with empty blocks, chained together.

    I = 0;
    BPP = (void *)(RackP + 1);
    while (I++ < StoreP->BlocksPerRack) {
	*BPP = (void *)((char *)BPP + StoreP->BytesPerBlock);
	BPP = *BPP;
    }
    BPP = (void *)((char *)BPP - StoreP->BytesPerBlock);	// Overshot, go back one block
    *BPP = StoreP->EmptyBlockP;					// Chain to existing list, if any
    StoreP->EmptyBlockP = (void *)(RackP + 1);			// Empty blocks on this rack come first
}


// ******************************************************************************
// sc_SAStoreClose will clear out the Store and FREE all its Racks... stored
// data Blocks will be lost forever and pointers to them will be invalid.

void		sc_SAStoreClose(sc_SAStorePointer StoreP)
{
    sc_SARackPointer	RackP, NextP;

    RackP = StoreP->FirstRackP;
    while (RackP) {
	NextP = RackP->NextP;
	free(RackP);
	RackP = NextP;
    }

    StoreP->FirstRackP = NULL;
    StoreP->EmptyBlockP = NULL;
    StoreP->UsedBlocks = 0L;
    StoreP->FreeBlocks = 0L;
    StoreP->RackCount = 0L;
    StoreP->Flags = sc_SASTORENOFLAG;
}

// ******************************************************************************
// sc_SAStoreAllocBlock suballocates and returns a data block from the Store.  It
// will create and add a new Rack if necessary.

void *		sc_SAStoreAllocBlock(sc_SAStorePointer StoreP)
{
    void *	BlockP;

    if (StoreP->EmptyBlockP == NULL)
	sc_SAStoreAddRack(StoreP);

    BlockP = StoreP->EmptyBlockP;
    StoreP->EmptyBlockP = (*(void **)BlockP);
    StoreP->UsedBlocks += 1;
    StoreP->FreeBlocks -= 1;

    return BlockP;
}

// ******************************************************************************
// sc_SAStoreFreeBlock returns the BlockP back to the free state.
//
// WARNING:	Crash and burn if BlockP did NOT come from StoreP!

void		sc_SAStoreFreeBlock(sc_SAStorePointer StoreP, void * BlockP)
{
    void **	BPP;

    BPP = BlockP;
    *BPP = StoreP->EmptyBlockP;
    StoreP->EmptyBlockP = BPP;

    StoreP->FreeBlocks += 1;
    StoreP->UsedBlocks -= 1;
}

// ******************************************************************************
// ******************************************************************************
// WIN EVENT REGISTRY (WEReg)
//
// Given an Event, the program has to find the handler function for *THAT*
// particular window as well as the main data structure associated with it.
// The WEReg associates a callback event handler and generic data pointer with
// every registered window.  (XLib can associate any data with a Win, but
// access is cumbersome, espcially in an event loop.)

// sc_WERegInit initializes everything.

void	sc_WERegInit(void)
{
    Uns16		I = 0;
    sc_WERegPointer *	P = sc_WERegArr;

    // Init the Store to sub-allocate the blocks.
    sc_SAStoreOpen(&sc_WERegStore, sizeof(sc_WERegRecord), sc_WEREGCOUNT / 2);

    // Init the Hash array
    while (I++ < sc_WEREGCOUNT) *P++ = NULL;
}

// ******************************************************************************
// sc_WERegKill disposes of the WE Registry.

void	sc_WERegKill(void)
{
    Uns16		I = 0;
    sc_WERegPointer *	P = sc_WERegArr;    

    sc_SAStoreClose(&sc_WERegStore);	// Frees the Racks holding the blocks
    while (I++ < sc_WEREGCOUNT) *P++ = NULL;
}

// ******************************************************************************
// sc_WERegAdd creates an entry for each new Win Id.

void	sc_WERegAdd(Uns64 Id, void * FP, void * DataP)
{
    Uns16		I = (Uns16)(Id && sc_WEREGMASK);
    sc_WERegPointer	NewP = sc_SAStoreAllocBlock(&sc_WERegStore);

    NewP->Ident = Id;
    NewP->EventFP = (sc_EventFPointer)FP;
    NewP->DataP = DataP;
    NewP->NextP = sc_WERegArr[I];
    sc_WERegArr[I] = NewP;
}

// ******************************************************************************
// sc_WERegDel removes the entry for the given Win Id.

void	sc_WERegDel(Uns64 Id)
{
    Uns16		I = (Uns16)(Id && sc_WEREGMASK);
    sc_WERegPointer	CurP = sc_WERegArr[I];
    sc_WERegPointer	LastP = NULL;

    while (CurP && CurP->Ident != Id) {
	LastP = CurP;
	CurP = CurP->NextP;
    }
    if (CurP == NULL) return;			// Did not find Id

    if (LastP) LastP->NextP = CurP->NextP;
    else sc_WERegArr[I] = CurP->NextP;

    sc_SAStoreFreeBlock(&sc_WERegStore, CurP);
}

// ******************************************************************************
// sc_WERegDispatch finds the entry for a given Win Id and
// invokes the callback.

void	sc_WERegDispatch(Uns64 Id, XEvent * EventP)
{
    Uns16		I = (Uns16)(Id && sc_WEREGMASK);
    sc_WERegPointer	CurP = sc_WERegArr[I];

    while (CurP && CurP->Ident != Id)
	CurP = CurP->NextP;

    if (CurP) (*CurP->EventFP)(EventP, CurP->DataP);
}

// ******************************************************************************
// ******************************************************************************
// Blink Timer
//
// The cursor/blinker in an active window has to wink on/off.  XLib does not
// support asynch alarm interrups, so must used a Polled approach. So XNextEvent
// is called only if there are pending events, will never sit and wait in XNextEvent.
// Poll is called to wait on a "file descriptor" representing the connection to the
// XServer--it will kick out when the connection gets a new event or allocated time
// expires... the allocated time is whatever is left from the desired Blink interval
// since the last SetTime.

// sc_BlinkTimerInit gets the FD and establishes first SetTime.

void	sc_BlinkTimerInit()
{
    sc_BlinkTimerR.PollEvent.fd = ConnectionNumber(XDispP);
    sc_BlinkTimerR.PollEvent.events = POLLIN;
    gettimeofday(&sc_BlinkTimerR.SetTime, NULL);
}

// sc_BlinkTimerReset rests the SetTime--Blinker should stay on
// for N millisecs each time user types a character, switches
// windows, panes, etc.

void	sc_BlinkTimerReset()
{
    gettimeofday(&sc_BlinkTimerR.SetTime, NULL);
}

// sc_BlinkTimerDo is called after XLib runs out of window events.  It
// spins (in POLL function) until interval passes, resets the SetTime
// for another cycle and returns 1.  If the FD registers more data, i.e.
// XLib has a new event, the function just returns 0.
//
// Time computation is in MilliSec... using 1024 instead of 1000 for
// conversion factor--cannot help myself!

Uns16	sc_BlinkTimerDo(Int32 MilliSecs)
{
    struct timeval	CurTime;
    Int32		DeltaT;

    gettimeofday(&CurTime, NULL);
    DeltaT = (1024 * (CurTime.tv_sec - sc_BlinkTimerR.SetTime.tv_sec)) +
	     ((CurTime.tv_usec - sc_BlinkTimerR.SetTime.tv_usec) / 1024);
    if (DeltaT >= MilliSecs || 0 == poll(&sc_BlinkTimerR.PollEvent, 1, MilliSecs - DeltaT)) {
	gettimeofday(&sc_BlinkTimerR.SetTime, NULL);
	return 1;
    }

    return 0;
}

// ******************************************************************************
// ******************************************************************************
// ******************************************************************************
// Main loop

void	sc_MainExit(void)
{
    sc_MainContinue = 0;
}


void	sc_MainEventLoop(void)
{
    XEvent	XWinEvent;
	
    while (sc_MainContinue && XPending(XDispP)) {

	XNextEvent(XDispP, &XWinEvent);
	switch (XWinEvent.type) {
	    case PropertyNotify:
		// printf("Prop Notify!  Win:%ld Atom:%ld State:%d\n",
		//        XWinEvent.xproperty.window, XWinEvent.xproperty.atom, XWinEvent.xproperty.state);
		break;

	    case MotionNotify:
		// printf("Motion\n");
		sc_WERegDispatch(XWinEvent.xmotion.window, &XWinEvent);
		break;

	    case EnterNotify:
	    case LeaveNotify:
		 sc_WERegDispatch(XWinEvent.xcrossing.window, &XWinEvent);
		 break;

	    case Expose:
		// printf("Expose\n");
		sc_WERegDispatch(XWinEvent.xexpose.window, &XWinEvent);
		break;

	    case ConfigureNotify:
		// printf("Configure\n");
		sc_WERegDispatch(XWinEvent.xconfigure.window, &XWinEvent);
		break;

	    case ButtonPress:
		// printf("ButtonPress\n");
		sc_WERegDispatch(XWinEvent.xbutton.window, &XWinEvent);
		break;

	    case ButtonRelease:
		sc_WERegDispatch(XWinEvent.xbutton.window, &XWinEvent);
		break;

	    case MappingNotify:
		// printf("Mapping\n");
		if ((XWinEvent.xmapping.request == MappingModifier) ||
		    (XWinEvent.xmapping.request == MappingKeyboard))
		    XRefreshKeyboardMapping(&XWinEvent.xmapping);
		break;

	    case KeyPress:
		// printf("KeyPress\n");
		sc_WERegDispatch(XWinEvent.xkey.window, &XWinEvent);
		break;

	    case FocusIn:
	    case FocusOut:
		// printf("Focus\n");
		sc_WERegDispatch(XWinEvent.xfocus.window, &XWinEvent);
		break;

	    case ClientMessage:
		// printf("Client\n");
		sc_WERegDispatch(XWinEvent.xclient.window, &XWinEvent);
		break;

	    case DestroyNotify:
		// printf("Destroy\n");
		sc_WERegDispatch(XWinEvent.xdestroywindow.window, &XWinEvent);
		break;

	    case SelectionClear:
		// printf("SelectionClear\n");
		sc_WERegDispatch(XWinEvent.xselectionclear.window, &XWinEvent);
		break;

	    case SelectionNotify:
		// printf("SelectionNotify\n");
		sc_WERegDispatch(XWinEvent.xselection.requestor, &XWinEvent);
		break;

	    case SelectionRequest:
		// printf("SelectionRequest\n");
		sc_WERegDispatch(XWinEvent.xselectionrequest.owner, &XWinEvent);
		break;

	    default:
		break;

	} // Switch
    } // while (Event)
}

int	main(int ArgC, char* ArgV[])
{
    Int16	OpenInitCount;
    char	*CP;
    
    G_MAINFILEERROR_INIT;			// For error reporting/debugging
    sc_WERegInit();				// Init window handler registry

    if ((NULL == setlocale(LC_ALL, "")) ||
	(! XSupportsLocale()) ||
	(NULL == XSetLocaleModifiers("@im=none")))
	G_SETEXCEPTION("Failed Locale Init", 0);


    XDispP = XOpenDisplay(getenv("DISPLAY"));
    if (! XDispP) G_SETEXCEPTION("Cannot connext to X server", 0);

    // XSynchronize(XDispP, True);		// Debugging AID !!

    XScreenN = DefaultScreen(XDispP);
    XDispHeight = DisplayHeight(XDispP, XScreenN);
    XDispWidth = DisplayWidth(XDispP, XScreenN);

    XftFontP = XftFontOpenName(XDispP, XScreenN, XftFontName);
    if (! XftFontP) G_SETEXCEPTION("Xft failed to get FontP", 0);

    XIMP = XOpenIM(XDispP, NULL, NULL, NULL);
    if (XIMP == NULL) G_SETEXCEPTION("XOpenIM Failed", 0);

    ED_EditorInit(XDispP, XftFontP, XIMP, XDispWidth / 3, XDispHeight / 2);

    OpenInitCount = 1;
    while (OpenInitCount < ArgC) {
	CP = ArgV[OpenInitCount];
	// printf("Arg%d -> [%s]\n", OpenInitCount, CP);
	if (*CP && (*CP != '-'))
	    ED_EditorOpenFile(CP, OpenInitCount > 1);	// NewFrame after first
	OpenInitCount += 1;
    }

    sc_MainContinue = 1;
    sc_BlinkTimerInit();					// Blinker
    while (sc_MainContinue) {					// Timer
    
	sc_MainEventLoop();
	if (sc_MainContinue && sc_BlinkTimerDo(sc_BLINKINTERVAL))
	    ED_BlinkHandler();
	    
    }

    sc_WERegKill();
    XftFontClose(XDispP, XftFontP);
    XCloseIM(XIMP);
    XCloseDisplay(XDispP);
    exit(0);
}



