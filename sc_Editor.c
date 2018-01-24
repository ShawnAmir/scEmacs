// ***********************************************************************
// Copyright (C) 2018 Shawn Amir
// All rights reserved
//
// scEmacs
//	... simple c Emacs
//	... small component Emacs
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
// sc_Editor.c
// Editor module for scEmacs
// ***********************************************************************

#include	<locale.h>
#include	<X11/Xlib.h>
#include	<X11/Xutil.h>
#include	<X11/Xatom.h>  
#include	<X11/cursorfont.h>
#include	<poll.h>
#include	<unistd.h>
#include	<pwd.h>
#include	<dirent.h>
#include	<sys/time.h>
#include	<fcntl.h>

#include	<X11/Xft/Xft.h>

#include	"sc_Public.h"
#include	"sc_Error.h"
#include	"sc_Editor.h"
#include	"sc_Main.h"

// NOTE:	On Ubuntu, be sure to re-assign Alt/Meta as the window manager
//		usurps it for Dash/menu thingies.  Even then, it does a few
//		strange things that need workarounds.  (Look for "????");

// ***********************************************************************
// LATER	Tab mutli-line selection, aligns to prev line!
//		Implement C-X Tab--move selected region (L/R Arrow + Shift-Arrow)
//		Implement C-M-f/b over matched paren (sexp)
//		Pop-up for QR--if window too small
//		Expand FReg... Add Flags
//		    1) Can register Func as NEEDS WRITE ACCESS--not for RO
//		    2) Can register Func for special buffers/modes

// ***********************************************************************
// EXTERNAL DEPENDENCIES
//
// XLib for drawing/windows/etc.
// Xft for text rendering
//	... otherwise simple C is all there is.

// ***********************************************************************
// LIMITATIONS
//
// RealEmacs does not allow multiple instances to access the very same disk
// file--uses special lockfiles.  scEmacs is smart enough to simply
// go to the existing buffer if attempting to open the same disk file
// multiple times.  But does *NOT* use lockfiles so cannot prevent this across
// multiple running applications.  scEmacs does NOT currently do auto-save!
//
// The QueryResponse (QR)  mechanism is far more limited than a MiniBuffer...
// It is only a single line and does not allow mark/selection range.  But it
// can be scrolled horizontally, allows for Kill/Yank, and supports basic mouse
// movements. It does not support mouse-clicking, selection ranges,
// XSel copy/paste, or other more advanced operations.
//
// RealEmacs has a beautiful and elaborate (occasionally slow/hanging) contextual
// coloring mechanism.  scEmacs currently draws only in black.
//
// RealEmacs is incredible.  scEmacs, while (hopefully) useful, is not RealEmacs
// and (like its author) is far from incredible.

// ***********************************************************************
// Design Priorities, in rough order:
//
// 1.		Useable, capable, and useful
// 2.		Easy to embed and incorporate
// 3.		Clear + easy to maintain
// 4.		Minimal external dependence
// 5.		Easy to expand, alter, and customize
// 6.		Small footprint and resource efficient
// 7.		Fast enough

// ***********************************************************************
// Design + Definitions
//
// Buffer	One and only 1 per open file, contains char data.
//		There will be a BufferRecord that contain info about the
//		conceptual buffer, and there will be a BufMem which is
//		a large hunk of memory (from Malloc) that contains the
//		actual char data (and a Gap).  No meta-data is stored
//		in the buffer memory--lexical role, color, etc.
//
// Window	Refers to traditional X Windows, not the emacs pane.
//
// Frame	Each (main) X Window contains a single Emacs Frame.
//
// Pane		Each Frame contains 1 or more Panes.  traditionally emacs
//		called these windows, back in the VT100 terminal days.
//		Each Pane displays a single buffer, but a given buffer
//		may have 0 or more Panes displaying it--each potentially
//		showing a different location.  Only a single Frame and
//		a single Pane in that Frame is ever Active (has focus)
//		and shows an Active (blinking) cursor.
//
// Cursor	Each Pane has a single Cursor.  It maintains the location
//		of this cursor on the screen (Col, Row) and its indexed
//		Position in the memory Buffer.  Every Pane will always
//		show its cursor, and the cursor is never allowed to wander
//		out of the displayed range.  Inactive Panes always
//		show an inactive cursor Box while active Panes have a
//		blinking solid cursor.  If a Buffer is no longer shown
//		in any Pane, it stashes the location of the Cursor in the
//		BufferRecord, just so it can go to the "last" place if and
//		when it is displayed again.
//
// Gap		Each buffer represents the character data as a contiguous
//		array along with a Gap that is moved around to allow text
//		insertion.  Moving the memory around to move the gap is
//		easier than having a complex data structure.  The Buffer
//		memory is indexed by Pos or accessed directly by a Pointer.
//
// Pos		Sees the whole buffer as conceptually contiguous, so
//		Buffer[Pos==N] is always the N'th data location, regardless
//		of gap location or size.
//
// Pointer	Is an actual pointer to the buffer, may fall within gap.
//		Worse, the Pointer may be completely invalid if the Buffer
//		memory is re-allocated (expanded) and moved to a new
//		location.
//
// Rather than constantly worrying about the position of the gap, a Pos
// is generally used, manipulated, and passed around instead of a pointer.
// Each time a character is to be read from the buffer, the 'Pos' has to
// be converted to a real Pointer to access the memory location.  The
// conversion process will take care to skip over the gap.
//
// Characters in the buffer are stored in UTF8 format.  While most will
// take a single byte of memory, some may take up to 4 bytes.  Therefore,
// there is NOT a 1-to-1 correspondance between characters in the file and
// bytes (indexed) in the buffer.  A Utf8 character (up to 4 bytes) will
// NEVER straddle the Gap.
//
// Every glyph is drawn as 1 mono-spaced character wide.  Double-spaced
// wide (typically Asian) glyphs are *NOT SUPPORTED* here.  Tab characters
// (variable width) are also *NOT SUPPORTED* and are converted into space
// charactes as part of the initial "filtering" when a new file is read.
// (Filtering also handles any necessary CR/LF -> '\n' conversions.)
//
// Line		A string of characters starting after a '\n' (or beginning
//		of the buffer) and ending in a '\n' (or end of buffer).
//		A long line may occupy multiple display Rows if the Frame
//		window is fairly narrow.
//
// Row		One horizontal sweep of the screen.
//
// Operations that move up or down by one screen row must care about
// Rows and not Lines.  But while the beginning and end of a Line
// is fairly obvious (begin/end of buffer or '\n' character) the Row
// does not have an algorithmically obviously terminus.  So the trick is
// to first isolate the line origin, then move over one pane-width (Row)
// at a time, thus finding all the Rows.

// ***********************************************************************
// Feature set
//
// SelRange	A contiguous range of characters, ranging from 1 char,
//		to many rows can be selected.  This range is limited
//		by the SelMark on one end (start or end) and the Cursor
//		on the other.  While the SelMark does not move with
//		scrolling, the Cursor *MAY*... so the SelRange will
//		shrink/expand with scrolling--as the cursor will be
//		moved to keep it on the visible screen.
//
//		The SelRange is conceptually dangerous as any typed
//		character will replace the entire range.  Therefore, the
//		range is deselected at the earliest opportunity.
//
// MarkRing	Each Buffer maintains a "MarkRing" array that keeps track
//		of the last 16 Marks in a circular array.  Each time a
//		new mark is created, it is pushed onto the ring.  The
//		area between the Cursor and the last Mark is the Region,
//		and many commands operate on this Region.  The Region
//		is *NOT* highlighted like the SelRange.  But the SelMark
//		used by SelRange is itself just a Mark.
//
// KillRing	There is a single KillRing (KR) and it stores "killed"
//		text from all Buffers.  The KillRing is not a ring at all,
//		but a stack.  A new Kill() will set YankP to the top of
//		the stack and Push the kill data there.  Yank() will
//		retrieve the kill data *AT* the YankP while PopYank() will
//		move the YankP down the stack to previous kills... until
//		a new Kill() resets it at the top again.  The KillRing
//		stack is NEVER re-ordered... but the stack does have
//		a limit on the number of elements and will wrap around,
//		hence the "ring" appellation.
//
// QueryResponse	The bottom Echo line of the Frame can be used as
//		a simple 1-line (QR) editor.  A Query, such as "Save File?"
//		is posted on the left and the user is prompted to type in
//		a response--which can be in single-letter (from multiple
//		choices) form or complex text, like a pathname.  This 1
//		line buffer shares the KillRing with the rest of the
//		program and supports simple cursor movements, deletion,
//		yank, yankpop, kill, etc.  It does NOT support a selection
//		range or marks, but will auto-scroll horizontally for long
//		responses on narrow frames.  It supports auto-completion
//		for File/Dir names, command names, etc.  The QR facility
//		is in no way as powerful or complex as the MiniBuffer in Emacs,
//		but is quite useable.
//
// Pop-up List	A floating (pop-up) window is used to display lists of items.
//		The user can view/scroll or select a single item.  This in
//		effect replaces some of the read-only buffers and special
//		behaviors of real emacs.  The pop-up window can be re-positioned
//		on the screen and stores its location (for the next
//		iteration) inside the relevent FrameRecord.
//
// I-Search	Incremental search is very similar to RealEmacs.  It Can search
//		forward and backward, can use C-w to read into the search
//		string, and shows alt matches along with the main (current) one.
//		There are some minor (expert level) differences with ReaEmacs,
//		but most would not notice.
//
// QueryReplace	Works very similar to RealEmacs for basic search and replace.
//		scEmacs does NOT provide the more esoteric features of recursive
//		editing, multi-buffer replacement, replacement history, etc.
//		But it does allow for y/n to each replacement, '!' to replace
//		all remaining, '.' to replace current and exit, and <Enter> to
//		exit replacement mode.  The current match is highlighted, but
//		alternative (next) matches are also shown.
//
// Undo		Works very similar to RealEmacs for basic functionality.
//		User can 'disable-undo' for a given Buffer or "reset-undo'
//		to turn it back on--will get rid of all previous Undo
//		memory if it was not previously disabled.  Simple GC keeps
//		a limit on the amount of Undo memory kept around.  Selective
//		Undo of SelRegion is NOT supported--only global Undo.  scEmacs
//		is smart about marking the Buffer as Mod/Unmod when undoing
//		and will happily Undo what was undone, ad infinitum.  The
//		Algorithm does NOT checksum the Buffer, so can show false
//		negative (MOD), but no false positive (will not claim UNMOD
//		when it isn't).  A special "C-u C-x =" command will print out
//		(to stdout) the	Undo memory Slabs--DEBUG mode only.
//
// Copy/Paste	PRIMARY and CLIPBOARD XSel (Xlib Selection not SelRange
//		selection) are supported.  Data in UTF8_STRING and simple
//		STRING formats can be imported/exported.  (Although no distinction
//		is made between UTF8 and simple STRING.)  INCR transmission is
//		also supported. XSel logic appears similar to RealEmacs.
//		
// ***********************************************************************

// ***********************************************************************
// EXTENSIONS/ADDITIONS/DELETIONS:
//
// International char sets:	I have done very little testing for these,
// basically limited to opening a few files with UTF8 glyphs in them.
//
// Asian double-wide glyphs:	These are NOT currently supported as all
// chars (UTF8 too) are limited to one column width.  Even Tabs are initially
// converted to multiple <space> chars because of this.  While support for
// these double-wide glyphs *can* be hacked in, it would be better to go all
// the way and support variable-spaced fonts in general.  This would require
// a complete (albeit simple) re-write to change all horizontal notions of
// Column (Col) to pixel-based X-Loc.
//
// Syntax coloring text:	scEmacs currently draws in black/gray.  Drawing
// in color is easy.  Having a localized syntax parser (for various modes)
// is also not too hard, as we draw one Pane at a time, starting at the
// beginning of a Pane.  But proper syntax-marking generally requires starting
// at the beginning of the Buffer (e.g. it can start with an open quote or
// or comment-start marker).  The syntax-markings for the whole Buffer will
// have to be recorded and stored in a parallel data-structure which is
// kept (more-or-less) in phase with the Buffer memory.  Ideally, a syntax
// parser marks the "coloring memory" for the Buffer in a separate thread.
//
// Specialized Buffers:		A ReadOnly (Info) Buffer is currently provided
// for Help (C-h).  It would be an easy matter to use such machinery for
// other uses, list of marks, list of Buffers, etc.  If a <return>-to-select
// mechanism was implemented, the whole Pop-Up window/menu machinery could
// be eliminated.
//
// Mini-buffers:		The QR (Query-Response) machinery uses the
// echo-line of each Frame as a very specialized mini-buffer.  It also involves
// a lot of code.  This code could be eliminated if a real Buffer was used as
// the MiniBuffer instead of the QR hack.
//
// Other commands:		I have included those commands that I normally
// use.  Everything from Word-transpose to keyboard macros, dired, and even
// GDB interface *can* be added.  But scEmacs is only meant to provide
// emacs-like functionality for a text-edit or script-controller window in
// some app.  It will never be RealEmacs.
//
// Testing:			An editor is a combinatorial nightmare to
// test.  I have done my best in a short span.  Beat on it and let me know
// when you find problems... better yet, see if you can fix it!
// ***********************************************************************

#define	ED_VERSIONMAJOR		0x0001
#define ED_VERSIONMINOR		0x0001

#define	ED_MAXMEMSIZE		0x7FFFFFFF
#define ED_BUFINITLEN		8192L		// Set 256 for Testing
#define ED_GAPEXTRAEXPAND	512		// Set 256 for Testing
#define ED_NAMESTRING		"scEmacs"
#define ED_UNDOINITLEN		8192		// Initial UndoSlab size
#define ED_UNDOEXTRALEN		8192		// Extra UndoSlab size
#define ED_UNDOGCSLABCOUNT	16		// Max number of UndoSlabs (L0 GC)
#define ED_UNDOGCL0SLABCOUNT	12		// Max number of UndoSlabs to trim to in L0 GC
#define ED_UNDOGCL1SLABCOUNT	8		// Max number of UndoSlabs for L1 GC
#define ED_UNDOGCL2MEMMAX	(120 * 1024)	// Max total UndoSlabsize for L2 GC

#define ED_EXTCOMMANDCODE	0x15		// Designates special keys
#define	ED_CTLCOMMANDCODE	0x16		// Control
#define ED_ALTCOMMANDCODE	0x17		// Alt/Meta
#define ED_SUPCOMMANDCODE	0x18		// Super
#define ED_HYPCOMMANDCODE	0x19		// Hyper
#define ED_COMMANDMODSMASK	(ControlMask | Mod1Mask | Mod4Mask)

#define	ED_FRAMEMINROWCHARS	30
#define ED_FRAMEMINROWCOUNT	8

#define	ED_FRAMELMARGIN		6		// Left of drawing area
#define	ED_FRAMERMARGIN		10		// Right of drawing area

#define ED_PANEMINROWCOUNT	5		// Minimum size of a display Pane
#define	ED_SCROLLINACTIVEWIDTH	3		// How much to show when inactive
#define ED_SCROLLTHUMBMINLEN	30		// Min Scroll bar height
#define ED_SCROLLWHEELSTEP	5		// Rows to scroll with each click

#define ED_BUFFERPATHLEN	511		// Leave 1 for terminating NULL (4K is a silly waste)
#define ED_FULLPATHLEN		(NAME_MAX + ED_BUFFERPATHLEN + 3)

#define	ED_FREGALLOCCOUNT	100		// Size of Store Rack (in entries)
#define ED_FBINDALLOCCOUNT	100
#define ED_FRAMEALLOCCOUNT	8
#define ED_PANEALLOCCOUNT	16
#define ED_BUFFERALLOCCOUNT	6

#define ED_BUFHELPIDENT		0xABC0		// Special code for special HELP Info buffers

#define	ED_FBINDKEYSTRLEN	(8 * 2)		// Multiple of 8
#define ED_CMDKEYSTRLEN		(ED_FBINDKEYSTRLEN * 2)

#define ED_MARKRINGCOUNT	(1 << 4)	// Power of 2--16 is plenty
#define ED_MARKRINGMASK		(ED_MARKRINGCOUNT - 1)

#define	ED_KILLRINGCOUNT	(1 << 4)	// Power of 2--16 is plenty
#define	ED_KILLRINGMASK		(ED_KILLRINGCOUNT - 1)
#define	ED_KRTOPINITLEN		128		// 1024		Testing
#define	ED_KRTOPEXTRA		128		// 512		Testing
#define	ED_KRRESTINITLEN	128		// 8 * 1024	Testing
#define	ED_KRRESTEXTRA		128		// 1024		Testing

#define	ED_MSGSTRLEN		256
#define ED_RESPSTRLEN		256
#define	ED_ISSTRLEN		128
#define	ED_TABSTOP		8		// Every N spaces

#define ED_DOUBLECLICKINTERVAL	500		// mSec, standard value.
#define ED_EXTRACLICKINTERVAL	100		// mSec, Extra time for triple (and more) clicks

#define ED_PU_XMARGIN		6		// Pop-Up (PU) parameters
#define ED_PU_YMARGIN		4
#define ED_PU_MINCHARS		30 + 5
#define ED_PU_MAXCHARS		80 + 5
#define ED_PU_MINROWS		8
#define ED_PU_MAXROWS		32
#define	ED_PU_STRLEN		128

#define ED_PUML_LEFTCOLCHARS	4		// PU for MarkList (ML)
#define	ED_PUML_ENTRYROWS	3

#define	ED_PUKL_LEFTCOLCHARS	5		// PU for KillList (KL)
#define ED_PUKL_ENTRYROWS	5

#define	ED_PUCL_LEFTCOLCHARS	5		// PU for CommandList (CL)
#define ED_PUCL_ENTRYROWS	2

#define	ED_PUBL_LEFTCOLCHARS	4		// PU for BufferList (BL)
#define ED_PUBL_ENTRYROWS	3

// ***********************************************************************

#define _ED_USLABPOINTER	struct _ED_USlabRecord *
#define _ED_UBLOCKPOINTER	struct _ED_UBlockRecord *

    typedef struct _ED_USlabRecord {
	Uns32			Tag;		// "UNDO"
	Uns32			Flags;
	Int32			SlabSize;	// Size of this USlab (May vary!)
	Int32			LastUBlock;	// Offset to Last USED UBlock on this Slab
	_ED_USLABPOINTER	NextUSP;	// Next USlab
	_ED_USLABPOINTER	PrevUSP;	// Prev USlab
	// UBlock		Block[];
    } ED_USlabRecord, *ED_USlabPointer;

    typedef struct _ED_UBlockRecord {
	Int32			PrevUBlock;	// Prev Offset, this Slab or Prev if First block
	Int32			DataPos;	// Start Pos in Buffer
	Int32			DataLen;	// DataLen in Buffer, Store here if Del
	Uns8			Flags;		// Add + Del (replace if both) + Chain/Free
	char			Data[3];	// Only stored if Del/Replace
    } ED_UBlockRecord, *ED_UBlockPointer;

    typedef enum {
	ED_UB_ADD		= 0x01,		// UndoBlock (UB) Text was added
	ED_UB_DEL		= 0x02,		// Text was deleted
	ED_UB_CHAIN		= 0x08,		// Chain this UB to previous

	ED_UB_CHUNK		= 0x10,		// Text was yanked, copied, etc... must stand alone
	ED_UB_FIRSTMOD		= 0x20,		// First Modification of an UNMODIFIED buffer--new or after save!
	ED_UB_SAVE		= 0x40,		// Buffer was saved... So ignore FirsMod *BEFORE* this!
    } ED_UB_Flags;

    
#undef _ED_UBLOCKPOINTER
#undef _ED_USLABPOINTER


#define _ED_FRAMEPOINTER	struct _ED_FrameRecord *
#define _ED_PANEPOINTER		struct _ED_PaneRecord *
#define _ED_BUFFERPOINTER	struct _ED_BufferRecord *

    typedef struct _ED_FrameRecord {
	Uns32			Tag;			// "FRAM"... Cleans your oil too!
	Uns32			Flags;
	_ED_FRAMEPOINTER	NextFrameP;
	_ED_FRAMEPOINTER	PrevFrameP;
	_ED_PANEPOINTER		FirstPaneP;		// For this Frame
	_ED_PANEPOINTER		CurPaneP;		// Active if Frame is active, Only NULL at creation
	XftDraw *		XftDP;			// Draw with Xft
	XIC			XICP;
	Window			XWin;			// XWindow containing this Frame
	GC			BlinkerGC;		// XWin GC for Blinker
	GC			HiliteGC;		// For high-lighting Search
	GC			HLBlinkerGC;		// GC for Blinker *ON* hilite color (Sel, Match, etc.)
	Uns32			Number;			// Frame number
	Int32			PaneCount;		// Number of Panes
	Int32			WinWidth;
	Int32			WinHeight;
	Int32			FrameX;			// TopLeft of Frame in Window
	Int32			FrameY;
	Int32			FrameWidth;		// Width/Height of Frame in Window
	Int32			FrameHeight;
	Int32			RowChars;		// Chars in 1 Row of Frame (save 1 for EOL cursor!)
	Int32			RowCount;		// Rows in Frame
	Int32			PUWinX;			// Coordinates for PopUp list windows
	Int32			PUWinY;
    } ED_FrameRecord, *ED_FramePointer;

    typedef struct _ED_PaneRecord {
	Uns32			Tag;			// "PANE"... No pane no gain!
	Uns32			Flags;
	_ED_PANEPOINTER		NextPaneP;		// Panes on this frame, top to bot
	_ED_PANEPOINTER		PrevPaneP;
	_ED_FRAMEPOINTER	FrameP;
	_ED_BUFFERPOINTER	BufP;			// Every Pane *MUST* have a Buffer
	Int64			FracRowCount;		// 32.32 Row count + fractions for scaling
	Int64			ScrollScale;		// 32.32 scale for scrolling
	Window			XModeWin;		// Window for modeline--not for bottom Pane
	Window			XScrollWin;		// Window for VScroll--every Pane gets one
	Int32			CursorPos;		// But MarkPos belongs in Buffer
	Int32			PanePos;		// Start of displayed page, first visible Pos

	Int32			CursorRow;		// First row of Pane is 0
	Int32			CursorCol;		// First col of Pane is 0
	Int32			StartRowCount;		// Rows before PanePos
	Int32			BufRowCount;		// Total rows of text
	Int32			TopRow;			// First row of FRAME is 0
	Int32			RowCount;		// Includes ModeLine at bot of Pane
	Int32			ScrollTop;		// Height of area above bar
	Int32			ScrollThumb;		// Height of Thumb area (visible bar)
	Int32			ScrollHeight;		// Height of entire scrol bar
    } ED_PaneRecord, *ED_PanePointer;

    typedef struct _ED_BufferRecord {
	Uns32			Tag;			// "BUFF"... Not just for nudists!
	Uns32			Flags;
	_ED_BUFFERPOINTER	NextBufP;		// Chain from ED_FirstBufP
	_ED_BUFFERPOINTER	PrevBufP;	
	
	char *			BufStartP;		// Start of BufMem;
	char *			BufEndP;		// First byte *AFTER* Buf mem (including Gap)
	char *			GapStartP;		// First byte of Gap
	char *			GapEndP;		// First byte *AFTER* Gap

	ED_USlabPointer		FirstUSP;		// First UndoSlab
	ED_USlabPointer		LastUSP;		// Last UndoSlab
	Int32			USCount;		// Number of UndoSlabs
	Int32			USTotalSize;		// Total size of UndoSlabs in bytes

	Int32			MarkPos;		// MarkPos is in buffer, CursorPos in Pane
	Int32			CursorPos;		// Stashed here for new Pane getting this Buf
	Int32			PanePos;		// Stashed here for new Pane getting this Buf
	Int32			LastPos;		// 1 more than last valid Pos
	Int32			PaneRefCount;		// How many are showing this Buffer

	Int32			MarkRingIndex;		// Limit with ED_MARKRINGMASK
	Int32			MarkRingArr[ED_MARKRINGCOUNT];

	Int32			Ident;			// Special Ident codes for special buffers
	Int32			DirNameOffset;		// Offset to *LAST* DirName in PathName
	char			FileName[NAME_MAX + 1];	// Posix constant (Waay too big!)
	char			PathName[ED_BUFFERPATHLEN + 1];
    } ED_BufferRecord, *ED_BufferPointer;


#undef _ED_BUFFERPOINTER
#undef _ED_PANEPOINTER
#undef _ED_FRAMEPOINTER

typedef enum {
    ED_Black = 0,
    ED_White, ED_Touch, ED_TouchPlus, ED_LightGray, ED_Gray, ED_TitleGray,
    ED_Red, ED_Orange,
    ED_Brown, ED_Green, ED_Blue,
    ED_SelOrange, ED_Turquoise, ED_Purple,

    ED_ColorCount
} ED_Color;

typedef enum {
    ED_ECHOMSGMODE		= 0x0000,		// Usual msg
    ED_ECHOERRORMODE,					// System error
    ED_ECHOPROMPTMODE,					// Prompt for action/selection
} ED_EchoMode;

typedef enum {
    ED_FRAMENOFLAG		= 0x00000000,
    ED_FRAMEFOCUSFLAG		= 0x00000001,		// Frame has focus
    ED_FRAMENOWINFLAG		= 0x80000000,		// Do NOT draw/write
    ED_FRAMEINITFILTERFLAG	= 0x40000000,
    ED_FRAMEQRSOLIDCURSORFLAG	= 0x08000000,
    ED_FRAMEQRBOXCURSORFLAG	= 0x04000000,

    ED_PANENOFLAG		= 0x00000000,
    ED_PANESOLIDCURSORFLAG	= 0x80000000,
    ED_PANEBOXCURSORFLAG	= 0x40000000,
    ED_PANESCROLLINGFLAG	= 0x00000001,
    
    ED_BUFNOFLAG		= 0x00000000,
    ED_BUFNOFILEFLAG		= 0x00000001,		// Has no associated disk file
    ED_BUFINFOONLYFLAG		= 0x00000010,		// InfoOnly buffer, will kill when PaneRefCount == 0
    ED_BUFREADONLYFLAG		= 0x00000020,		// ReadONly buffer, will not allow WRITE
    ED_BUFMODFLAG		= 0x80000000,		// Buffer was modified
    ED_BUFCLEANUNDOFLAG		= 0x40000000,		// Buffer is Unmodified--reset by Undo system!
    ED_BUFFILTERFLAG		= 0x20000000,		// Buffer was filtered (Tab+CR removed)
    ED_BUFNAMECOLFLAG		= 0x10000000,		// Buffer Filename collides with other buffers

    ED_US_NOFLAG		= 0x00000000,		// UndoSlab (US)

} ED_Flag;

typedef enum _ED_QRespType {
    ED_QRLetterType		= 0x0001,		// 1 letter response, so no COMMANDS
    ED_QRStringType		= 0x0002,		// Longer responses, filename, command name, etc.

    ED_QRSomeCmd		= 0,
    ED_QRBadCmd			= -1,
    ED_QRKillCmd		= 99,
    ED_QRYankCmd		= 100,
    
} ED_QRespType;


#define _ED_FREGPOINTER struct _ED_FRegRecord *
#define _ED_FBINDPOINTER struct _ED_FBindRecord *

    typedef enum {
	ED_StartId		= 1,			// Starting up
	ED_SelectId,					// Frame/Pane selection
	ED_ResizeId,					// Frame/Window re-size
	ED_ScrollId,					// Scrolling
	ED_CursorPosId,					// Selecting Cursor Pos
	ED_InsertId,					// Text insertion
	ED_ReadId,					// Read a file
	ED_KillId,					// Text killed
	ED_YankId,					// Yanked Killed text
	ED_ExecId,					// Identifies M-x
	ED_UndoId,
    } ED_CmdId;

    typedef void (*ED_FRegFuncP)(ED_PanePointer PaneP);

    typedef struct _ED_FRegRecord {
	_ED_FREGPOINTER		SortNextP;			// Next in Name sort order
	_ED_FREGPOINTER		SortPrevP;			// Prev in Name sort order
	char *			NameP;				// Name string
	ED_FRegFuncP		FuncP;				// Actual function
	_ED_FBINDPOINTER	FirstBindP;			// Bindings (to KeyStr) if any
    } ED_FRegRecord, *ED_FRegPointer;

    typedef struct _ED_FBindRecord {
	_ED_FBINDPOINTER	SortNextP;			// Next in KeyStr sort order
	_ED_FBINDPOINTER	SortPrevP;			// Prev in KeyStr sort order
	ED_FRegPointer		FRegP;				// FReg entry (has Func and Name)
	_ED_FBINDPOINTER	FuncNextP;			// Next Binding of SAME function, entry order
	_ED_FBINDPOINTER	FuncPrevP;			// Prev Binding of SAME function
	char			KeyStr[ED_FBINDKEYSTRLEN];	// Actual key binding--Null delimited
    } ED_FBindRecord, *ED_FBindPointer;

#undef _ED_FBINDPOINTER
#undef _ED_FREGPOINTER

typedef Int16 (*ED_QRRespFuncP)(void);				// QueryResponse (QR) Response function
typedef Int16 (*ED_QRAutoCompFuncP)(void);			// QR AutoCompletion function
typedef void (*ED_QRExitFuncP)(void);				// QR Exit (cleanup) function

#define _ED_KRPOINTER struct _ED_KRRecord *
#define _ED_KEPOINTER struct _ED_KERecord *

    typedef enum {
	ED_KRNOFLAG		= 0x00000000,

	ED_KENOFLAG		= 0x00000000,
	ED_KETOPFLAG		= 0x80000000,
    } ED_KFlags;

    typedef struct _ED_KERecord {
	Int32			Flags;
	Int32			Pos;				// Offset in buffer
	Int32			Len;				// Length of data
    } ED_KERecord, *ED_KEPointer;


    typedef struct _ED_KRRecord {
	Uns32			Flags;
	char *			TopP;				// Data for CUR kill
	char *			RestP;				// Data for prev kills
	Int32			TopLen;				// Len for TopP
	Int32			RestLen;			// Len for RestP
	Int32			RestDataLen;			// How much Data in RestP

	Int16			TopI;				// Top--Index into Elts
	Int16			YankI;				// Doodle dandy, Index to Yank (can be TopI)

	ED_KERecord		EltArr[ED_KILLRINGCOUNT];	// Index++ to Push, Index-- to Pop
    } ED_KRRecord, *ED_KRPointer;

#undef _ED_KEPOINTER
#undef _ED_KRPOINTER

#define _ED_PUPOINTER struct _ED_PURecord *

    typedef enum _ED_PUEnums {
	ED_PUNOFLAGS			= 0x00000000,
	ED_PULISTTYPE			= 0x0001,
	ED_PUNOSTATE			= 0x0000,

	ED_PU_DRAWRSCROLLFLAG		= 0x0001,
	ED_PU_DRAWLSCROLLFLAG		= 0x0002,
	ED_PU_DRAWMORELINESFLAG		= 0x0004,
	
    } ED_PUEnums;

    typedef struct _ED_PURecord _ED_PURecord;		// Forward decl for FuncP def

    typedef void	(*ED_PUReturnFuncP)(_ED_PURecord * PUP);
    typedef Int16	(*ED_PUDrawFuncP)(_ED_PURecord * PUP, Int32 CurEntry, Int32 StartX, Int32 CharLimit, Int32 StartY);

    typedef struct _ED_PURecord {
	ED_PanePointer		PaneP;			// Pane associated with PU window
	ED_PUReturnFuncP	ReturnFP;		// Execute on successful return.
	ED_PUDrawFuncP		DrawFP;			// Draw contents of one entry line
	ED_FRegFuncP		CLCmdFP;		// Command-List Cmd to execute, if any
	void *			DataP;			// Data for caller/creator

	Window			XWin;
	XftDraw *		XftDP;

	Uns32			Flags;
	Uns16			Type;
	Uns16			State;
	Int32			Config1;
	Int32			Config2;
	Int32			EntryCount;		// Visible entry slots
	Int32			EntryTotal;		// Total number of entries, not all shown
	Int32			EntryTop;		// Top Entry if can scroll, 0 otherwise
	Int32			EntrySel;		// Selected Entry, MUST BE on screen!
	Int32			EntryRows;		// Rows per each entry
	Int32			EntryCharMax;		// Max width of entry data
	Int32			EntryCharScroll;	// How much to scroll (Horiz) left
	Int32			RowCount;		// Actual Rows in Window
	Int32			RowChars;		// Chars in each Row
	Int32			WinX;			// Top-left X position
	Int32			WinY;			// Top-left Y position
	Int32			WinWidth;
	Int32			WinHeight;
	Int32			TitleLen;
	char			TitleStr[ED_PU_STRLEN];
    } ED_PURecord, *ED_PUPointer;

#undef _ED_PUPOINTER


// ******************************************************************************
XftFont *			ED_XFP;				// Xft Font
Display *			ED_XDP;				// X11 Display
XIM				ED_XIMP;			// Input method
Atom				ED_XWMDelAtom;			// Client window Delete
Atom				ED_PUExecAtom;			// Exec a cmd--from PU
Atom				ED_ClipAtom;			// CLIPBOARD
Atom				ED_TargetsAtom;			// TARGETS
Atom				ED_TextAtom;			// TEXT
Atom				ED_UTF8Atom;			// UTF8_STRING
Atom				ED_IncrAtom;			// INCR

XftColor			ED_XCArr[ED_ColorCount];	// Colors
Cursor				ED_TextCursor;			// Text insertion
Cursor				ED_SizeCursor;			// Pane re-size (double arrow)
Cursor				ED_ArrowCursor;			// Left Arrow
Int16				ED_Ascent;			// XFP metrics
Int16				ED_Descent;
Int16				ED_Height;
Int16				ED_Row;
Int16				ED_Advance;

sc_SAStore			ED_FrameStore;			// Store to allocate Frames
ED_FramePointer			ED_FirstFrameP = NULL;		// Head of linked list
ED_FramePointer			ED_CurFrameP = NULL;		// Frontmost
Uns32				ED_FrameN = 0;			// Counts up, used to generate frame names

Int16				ED_CmdLen = 0;			// Length of FrameCommand, so far
ED_FBindPointer			ED_CmdBindP = NULL;		// FBindP for command search/match
char				ED_CmdStr[ED_FBINDKEYSTRLEN] = "";	// keyStr for command
Int16				ED_CmdEchoDelay;		// Echo commands when Delay goes to 0;
Int16				ED_CmdShift = 0;		// 0 Unless Shift or ShiftLock
Int32				ED_CmdMult = 1;			// Default is 1, do everthing once
Int16				ED_CmdMultNeg = 0;		// Negative CommandMult!
Int16				ED_CmdMultCmd = 0;		// Default is 0, 1 if C-u actually given
Int16				ED_CmdMultDigit = 0;		// Counts digits in C-u command, 0 otherwise
Int16				ED_CmdMultIn = 0;		// IN middle of reading numeric Arg!
Int16				ED_CmdMultLen = 0;		// Length of Mult arg, 0 if none
Int32				ED_CmdTargetCol;		// Desired Target Col for Cursor Up/Down
Int64				ED_CmdThisId;			// So a function can set its own ID!
Int64				ED_CmdLastId;			// Id/FuncP of last executed Cmd

ED_PanePointer			ED_ISPaneP = NULL;		// IS mode (IncSearch) if not NULL
Int16				ED_ISDir = 0;			// 0 == NO search, 1 == Forward, -1 == Backward
char				ED_ISStr[ED_ISSTRLEN] = "";	// Current search string--may get aborted
Int32				ED_ISStrLen = 0;		// Length of ISStr
Int16				ED_ISCaseSen = 0;		// 1 if CaseSensitive (ISStr includes Uppercase)
char				ED_ISPrevStr[ED_ISSTRLEN] = "";	// Previous successful search
Int32				ED_ISMatchPos = -1;		// Current Match Pos
Int32				ED_ISSearchPos = -1;		// Where to start searching NEXT
Int32				ED_ISAltPos = -1;		// Only if Alternate comes RIGHT AFTER MatchPos!
Int32				ED_ISOriginPos = 0;		// Origin of the search
Int16				ED_ISDoWrap = 0;		// Wrap around again

ED_PanePointer			ED_QREPPaneP = NULL;			// QREP mode (QueryReplace) if not NULL
char				ED_QREPFromStr[ED_ISSTRLEN] = "";	// Text should match this
char				ED_QREPToStr[ED_ISSTRLEN] = "";		// Text will be replaced by this
Int32				ED_QREPFromLen = 0;
Int32				ED_QREPToLen = 0;
char				ED_QREPPrevFromStr[ED_ISSTRLEN] = "";	// Prev invocation
char				ED_QREPPrevToStr[ED_ISSTRLEN] = "";	// Prev invocation
Int32				ED_QREPCount = 0;			// How many were replaced
Int16				ED_QREPDoAll = 0;			// Quietly replace all


char				ED_Path[ED_BUFFERPATHLEN + 1];	// Temp global, during file operations
char				ED_Name[NAME_MAX + 1];		// Temp global, during file operations
char				ED_FullPath[ED_FULLPATHLEN + 1]; // Temp global, during file operations
Int32				ED_FullPathLen;			// Temp global, during file operations
Int32				ED_FD;				// Temp global, during file operations

ED_PanePointer			ED_QRPaneP = NULL;		// PaneP or NULL--THIS IS HOW WE KNOW QR MODE!!
ED_QRRespFuncP			ED_QRRespFP = NULL;		// Callback to process response
ED_QRAutoCompFuncP		ED_QRAutoCompFP = NULL;		// Callback for auto-complete!
ED_QRExitFuncP			ED_QRExitFP = NULL;		// Callback for cleanup on exit/abort
ED_BufferPointer		ED_QRTempBufP = NULL;		// Used for file handling and Buf selection
char				ED_QRPromptS[ED_MSGSTRLEN];	// String contains prompt (may have UTF8) (0 delimited)
Int16				ED_QRPromptLen = 0;		// Length in bytes
Int16				ED_QRPromptX = 0;		// Length in # of chars
char				ED_QRRespS[ED_RESPSTRLEN];	// String contains typed response (0 delimited)
Int16				ED_QRRespLen = 0;		// Length in bytes
Int16				ED_QRRespX = 0;			// Length in # of chars
Int16				ED_QRType = 0;			// Query type
Int16				ED_QRRes = 0;			// Result of processing Response
Int16				ED_QRCursorPos = 0;		// Location of cursor in bytes of Resp
Int16				ED_QRCursorX = 0;		// Location of cursor in # of chars
Int16				ED_QRStartPos = 0;		// Bytes Resp had to be scrolled left to fit
Int16				ED_QRStartX = 0;		// # of Chars Resp had to be scrolled left to fit
Int16				ED_QRThisCmd = 0;		// So a function can set it for LastCmd!
Int16				ED_QRLastCmd = 0;		// Last Mods in hi word, SymChar in low word
Int16				ED_QRYankPos = 0;		// Records Pos of last Yank, to allow YankPop
Int16				ED_QRYankX = 0;			// # of chars that was yanked the last time
Int16				ED_QRStrikeOut = 0;		// NO ROOM for a Response!

char				ED_FrameEchoS[ED_MSGSTRLEN];	// Text for Echo line
Int16				ED_FrameEchoLen = 0;
Int16				ED_FrameEchoMode = ED_ECHOMSGMODE;

char				ED_TabStr[ED_TABSTOP + 1];	// String of spaces to replace Tab

sc_SAStore			ED_PaneStore;			// Store to allocate Panes

ED_PanePointer			ED_SelPaneP = NULL;		// Pane with Sel range, or NULL
Int32				ED_SelMarkPos;			// SelRange is between Mark and Cursor
Int32				ED_SelMarkCol;			// Mark (Col, Row) can be OFF screen!
Int32				ED_SelMarkRow;
Int32				ED_SelLastRow;			// Used for drag-selecting region
Int32				ED_SelLastCol;

ED_PanePointer			ED_LastSelPaneP	= NULL;		// Stash ED_SelPaneP when Frame loses focus

sc_SAStore			ED_BufferStore;			// Store to allocate Buffer records
ED_BufferPointer		ED_FirstBufP = NULL;		// Head of linked list

ED_KRRecord			ED_KillRing;			// One KillRing for everything!

char				ED_FrameTag[] = "FRAM";
char				ED_PaneTag[] = "PANE";
char				ED_BufTag[] = "BUFF";
char				ED_UndoTag[] = "UNDO";
char				ED_TempBufName[32];		// Stash temp name here
Int32				ED_TempBufCount = 1;		// Increment each time!

sc_SAStore			ED_FRegStore;
ED_FRegPointer			ED_FirstFRegP = NULL;

sc_SAStore			ED_FBindStore;
ED_FBindPointer			ED_FirstBindP = NULL;

ED_PURecord			ED_PUR;				// Just one for now



// ******************************************************************************
// ******************************************************************************

char *				ED_STR_QuerySaveExit		= "There are modified buffers!  Exit anyway? [y or n]: ";
char *				ED_STR_QuerySaveClose		= "There are modified buffers!  Close anyway? [y or n]: ";
char *				ED_STR_QueryWriteFile		= "Write File: ";
char *				ED_STR_QueryFindFile		= "Find File ";
char *				ED_STR_QueryInsertFile		= "Insert File ";
char *				ED_STR_QueryOverwriteFile	= "File already exists!  Overwrite it? [y or n]: ";
char *				ED_STR_QueryFilterFile		= "File has <Tab> and <CR>!  Correct in buffer? [y or n]: ";
char *				ED_STR_QuerySaveFilter		= "File was filtered!  Overwrite original? [y or n]: ";
char *				ED_STR_QuerySaveFilterNamed	= "%s was filtered!  Overwrite original? [y or n]: ";

char *				ED_STR_QueryReplace		= "Query replace (default %.*s -> %.*s): ";
char *				ED_STR_QueryReplaceNoDef	= "Query replace: ";
char *				ED_STR_QueryReplaceWith		= "Query replace %.*s with: ";

char *				ED_STR_TempBufName		= "*Temp_%d*";
char *				ED_STR_Filtered			= "<Filtered>";
char *				ED_STR_All			= "All";
char *				ED_STR_Top			= "Top";
char *				ED_STR_Bot			= "Bot";

char *				ED_STR_PUEmptyList		= "[Empty]";
char *				ED_STR_PUMarkList		= "Go to Mark -- %.*s";
char *				ED_STR_PUKillList		= "Yank from List";
char *				ED_STR_PUCmdList		= "Execute from List";
char *				ED_STR_PUListSelect		= "<Enter> to select, <Space> to cancel";
char *				ED_STR_PUCmdNoBind		= "Not bound--use 'M-x %s'";
char *				ED_STR_PUCmdFunc		= "Function: %s";
char *				ED_STR_PUCmdFirstBind		= "Binding:  [%s]";
char *				ED_STR_PUCmdOtherBind		= " or [%s]";
char *				ED_STR_PUBufList		= "Select Buffer from List";
char *				ED_STR_Filter			= "Filtering:";
char *				ED_STR_SwitchBuffer		= "Switch to buffer (default %.*s): ";
char *				ED_STR_KillBuffer		= "Kill buffer (default %.*s): ";
char *				ED_STR_ISearch			= " ISearch";
char *				ED_STR_QReplace			= " QReplace";
char *				ED_STR_BufInfoPath		= "Temp read-only info buffer!";
char *				ED_STR_BufMemDisp		= "-:%c%c %d Data (%ld Memory) %d Undo Memory (%d Bytes)";


char *				ED_STR_EchoErrorTemplate	= "%s  (Errno:%d)";
char *				ED_STR_EchoSelCancel		= "SelRange cancelled";
char *				ED_STR_EchoMarkSet		= "Mark set";
char *				ED_STR_EchoMarkPop		= "Mark popped";
char *				ED_STR_EchoQuit			= "Quit";
char *				ED_STR_EchoAbort		= "Aborted";
char *				ED_STR_EchoCursorInfo		= "Char: %s %s Pos=%d of %d (%%%d) Loc:(%d,%d)";
char *				ED_STR_EchoEOBCursorInfo	= "E-O-B Pos=%d of %d Loc:(%d,%d)";
char *				ED_STR_EchoCmdUndef		= "%s%s is not a command!";
char *				ED_STR_EchoCmdMatchFail		= "%sis undefined!";
char *				ED_STR_EchoCmdBadNum		= "%sis too much!";
char *				ED_STR_EchoBegBuf		= "Beginning of buffer";
char *				ED_STR_EchoEndBuf		= "End of buffer";
char *				ED_STR_EchoWrotePath		= "Wrote: %.*s";
char *				ED_STR_EchoWroteName		= "Wrote: .../%.*s";
char *				ED_STR_EchoOpenFailed		= "Could not open file.";
char *				ED_STR_EchoWriteFailed		= "Could not write file.";
char *				ED_STR_EchoReadFailed		= "Could not read file.";
char *				ED_STR_EchoNoChangeSave		= "(No changes need to be saved.)";
char *				ED_STR_EchoModFlagSet		= "Modification-flag set";
char *				ED_STR_EchoModFlagClear		= "Modification-flag cleared";
char *				ED_STR_EchoOutOfMem		= "Not enough memory!";
char *				ED_STR_EchoExecSelf		= "Cannot execute self!";
char *				ED_STR_EchoDir			= "Directory %.*s";
char *				ED_STR_EchoAborted		= "Operation aborted!";
char *				ED_STR_EchoOnlyFrame		= "Only one frame!";
char *				ED_STR_EchoDelOnlyFrame		= "Cannot delete the only frame!";
char *				ED_STR_EchoParenMismatch	= "Mismatched parenthesis";
char *				ED_STR_EchoParenMatch		= "Matches: %.*s";
char *				ED_STR_EchoIS			= "Search: %.*s";
char *				ED_STR_EchoISFail		= "Failing search: %.*s";
char *				ED_STR_EchoISBack		= "Search backward: %.*s";
char *				ED_STR_EchoISBackFail		= "Failing search backward: %.*s";
char *				ED_STR_EchoQueryReplace		= "Query replacing %.*s with %.*s: [y n ! . <Ret>]";
char *				ED_STR_EchoQueryReplaceDone	= "Replaced %d occurrences";
char *				ED_STR_EchoUndoMemFreed		= "Cleared out some old Undo memory";
char *				ED_STR_EchoUndoLost		= "Severe memory shortage, disabled Undo command";
char *				ED_STR_EchoBufNoUndo		= "Undo has been disabled, 'M-x reset-undo' to turn back on!";
char *				ED_STR_EchoUndoNoMore		= "Reached limit of Undo history";
char *				ED_STR_EchoUndoReset		= "Undo memory has been reset, will record from here";
char *				ED_STR_EchoReadOnly		= "Buffer is read only!";
char *				ED_STR_QueryLineNumber		= "Line number: ";
char *				ED_STR_QueryCharNumber		= "Char number: ";
char *				ED_STR_EchoSavedNFiles		= "Buffers saved: %d";
char *				ED_STR_EchoSearchOriginMark	= "Mark saved where search started";


// ******************************************************************************

#define		ED_BUFFERISMIDUTF8(C)		(((Uns8)C & 0xC0) == 0x80)
#define		ED_RANGELIMIT(X, Low, Hi)	do {if (X < (Low)) X = (Low);		   \
						    else if (X > (Hi)) X = (Hi); } while (0);
						    
// ******************************************************************************

void		ED_FRegInit(void);
void		ED_FRegKill(void);
ED_FRegPointer	ED_FRegNewFunction(ED_FRegFuncP FuncP, char * NameP);
ED_FBindPointer	ED_FRegNewBinding(ED_FRegPointer FRegP, char * KeyStrP);
Int16		ED_FRegFindBinding(ED_FBindPointer *StartBPP, char * KeyStrP);
Int16		ED_FRegBindCompare(register char * EntryP, register char * KeyStrP);

Int16		ED_KeyStrPrintExtKey(char * DestP, char Key);
Int16		ED_KeyStrPrint(char * KeyStrP, char * DestP);

void		ED_XSelInit(void);
void		ED_XSelKill(void);
Int16		ED_XSelInsertData(ED_PanePointer PaneP, Atom SelAtom);
void		ED_XSelHandleEvent(XEvent * EventP, void * DataP);
void		ED_XSelSetPrimary(ED_PanePointer PaneP);
void		ED_XSelReleasePrimary(ED_BufferPointer BufP);
void		ED_XSelAlterPrimary(ED_BufferPointer BufP, Int32 Pos, Int32 Len);
void		ED_XSelSetClip(void);
void		ED_XSelLostPrimary(void);
void		ED_XSelLostClip(void);

void		ED_KillRingInit(void);
void		ED_KillRingFree(void);
void		ED_KillRingAdd(char * DataP, Int32 DataLen, Int16 OpForward);
void		ED_KillRingYank(Int32 PopCount, char **PP, Int32 *LenP, Int16 LoopForData);

void		ED_FrameHandleEvent(XEvent * EventP, ED_FramePointer FP);
void		ED_FrameHandleClick(ED_FramePointer FrameP, XEvent * EventP);
void		ED_FrameHandleRelease(ED_FramePointer FrameP, XEvent * EventP);
void		ED_FrameHandleMotion(ED_FramePointer FrameP, XEvent * EventP);
void		ED_FrameHandleKey(ED_FramePointer FrameP, KeySym * SymP, Uns32 Mods, Int16 Count, char * StrP);

void		ED_FrameCommandEcho(ED_FramePointer FrameP, Int16 Waited);
void		ED_FrameCommandBadEcho(ED_FramePointer FrameP, Uns16 SymChar, Int16 Error);
Int16		ED_FrameCommandMatchAbort(ED_FramePointer FrameP, Uns16 SymChar);
Int16		ED_FrameCommandMatch(ED_FramePointer FrameP, Uns16 SymChar, Uns32 Mods);

void		ED_FrameDrawBlinker(ED_FramePointer FrameP);
void		ED_FrameDrawScrollBar(ED_FramePointer FrameP);
void		ED_FrameGetWinSize(ED_FramePointer FrameP, Int32 * WidthP, Int32 * HeightP);
void		ED_FrameResetWinMinSize(ED_FramePointer FrameP);
void		ED_FrameSetSize(ED_FramePointer FrameP, Int16 DoPanes);
void		ED_FrameUpdateTextRows(ED_FramePointer FrameP, Int32 OldRowChars);
void		ED_FrameNew(Int32 WinWidth, Int32 WinHeight, ED_BufferPointer BufP);
void		ED_FrameWinDestroyed(ED_FramePointer FrameP);
void		ED_FrameKill(ED_FramePointer FrameP);
void		ED_FrameKillAsk(ED_FramePointer FrameP);
void		ED_FrameDrawAll(ED_FramePointer FrameP);
void		ED_FrameDrawEchoProgressBar(ED_FramePointer FrameP, char * LabelP, Int16 Percent);
void		ED_FrameDrawEchoLine(ED_FramePointer FrameP);
void		ED_FrameSetEchoS(Int16 Mode, char * SourceP);
void		ED_FrameSPrintEchoS(Int16 Mode, char * FormatP, ...);
void		ED_FrameSetEchoError(char * SourceP, Int32 Err);
void		ED_FrameFlashError(ED_FramePointer FrameP);
void		ED_FrameExecPUCmd();

void		ED_PaneNew(ED_FramePointer FrameP, ED_BufferPointer BufP);
void		ED_PaneSplit(ED_PanePointer PaneP);
void		ED_PaneGetNewBuf(ED_PanePointer PaneP, ED_BufferPointer NewBufP);
void		ED_PaneKill(ED_PanePointer PaneP);
Int16		ED_PaneResize(ED_PanePointer PaneP, Int32 Delta);
void		ED_PaneUpdateStartPos(ED_PanePointer PaneP, Int32 OldRowChars);
void		ED_PaneUpdateAllPos(ED_PanePointer PaneP, Int32 MoveCursor);
void		ED_PaneDrawBlinker(ED_PanePointer PaneP);
void		ED_PaneEraseCursor(ED_PanePointer FrameP);
void		ED_PaneDrawCursor(ED_PanePointer PaneP, Int16 Box);
char *		ED_PaneGetDrawRow(ED_PanePointer PaneP, Int32 *PosP, Int32 *ColP, Int32 *CountP, Int16 * WrapP, Int16 * PartialP);
void		ED_PaneDrawText(ED_PanePointer PaneP);
void		ED_PaneDrawBackground(ED_PanePointer PaneP, Int32 Row, Int32 LineY);
void		ED_PaneDrawModeLine(ED_PanePointer PaneP);
void		ED_PaneMakeModeWin(ED_PanePointer PaneP);
void		ED_PaneKillModeWin(ED_PanePointer PaneP);
void		ED_PanePositionModeWin(ED_PanePointer PaneP);
void		ED_PaneModeWinHandleEvent(XEvent * EventP, ED_PanePointer PaneP);
void		ED_PaneMakeScrollWin(ED_PanePointer PaneP);
void		ED_PaneKillScrollWin(ED_PanePointer PaneP);
void		ED_PanePositionScrollBar(ED_PanePointer PaneP);
void		ED_PaneSetScrollBar(ED_PanePointer PaneP);
void		ED_PaneDrawScrollBar(ED_PanePointer PaneP, Int16 Grabbed);
void		ED_PaneScrollWinHandleEvent(XEvent * EventP, ED_PanePointer PaneP);
Int32		ED_PaneScrollByThumb(ED_PanePointer PaneP, Int32 Delta);
void		ED_PaneScrollByRow(ED_PanePointer PaneP, XEvent * EventP, Int32 DeltaRow);
void		ED_PaneMoveCursorAfterScroll(ED_PanePointer PaneP, Int32 DeltaRow);
void		ED_PaneMoveAfterCursorMove(ED_PanePointer PaneP, Int32 CursorRowStartPos, Int32 DeltaRow, Int16 ForceSetScroll);
void		ED_PaneMoveForCursor(ED_PanePointer PaneP, Int32 CursorRowStartPos, Int32 NewCursorRow);

void		ED_PaneHandleClick(ED_PanePointer PaneP, Int32 Row, Int32 Col, Int16 Shift, Int16 Count);
void		ED_PaneHandleDrag(ED_PanePointer PaneP);
void		ED_PaneHandleRelease(ED_PanePointer PaneP);
void		ED_PaneInsertPrimary(ED_PanePointer PaneP);
void		ED_PaneHandleDragMotion(ED_PanePointer PaneP, Int32 Row, Int32 Col);
Int32	ED_PaneFindLoc(ED_PanePointer PaneP, Int32 Pos, Int32 *RowP, Int32 *ColP, Int16 FromZero, Int16 PaneLimit);
Int32	ED_PaneFindPos(ED_PanePointer PaneP, Int32 *PosP, Int32 *RowP, Int32 *ColP, Int16 FromPane, Int16 FixOffBottom);
Int32		ED_PaneDelSelRange(ED_PanePointer PaneP, Int16 DoUpdate);
void		ED_PaneShowOpenParen(ED_PanePointer PaneP, char CloseC, Int32 ParenPos);
void		ED_PaneInsertChars(ED_PanePointer PaneP, Int32 Count, char * StrP, Int16 Typed);
void		ED_PaneInsertBufferChars(ED_PanePointer PaneP, ED_BufferPointer TempBufP, Int16 IsPaste);
void		ED_PaneUpdateOtherPanes(ED_PanePointer PaneP, Int32 InsertPos, Int32 InsertCount);
void			ED_PaneUpdateOtherPanesIncrBasic(ED_PanePointer PaneP, Int32 InsertPos, Int32 InsertCount);
void			ED_PaneUpdateOtherPanesIncrRest(ED_PanePointer PaneP);

void		ED_BufferPushMark(ED_BufferPointer BufP, Int32 Pos);
Int32		ED_BufferGetMark(ED_BufferPointer BufP, Int16 DoPop);
Int32		ED_BufferGetDiffMark(ED_BufferPointer BufP, Int32 NotPos);
Int32		ED_BufferSwapMark(ED_BufferPointer BufP, Int32 NewPos);
void		ED_BufferUpdateMark(ED_BufferPointer BufP, Int32 Pos, Int32 Delta);
ED_BufferPointer	ED_BufferNew(Int32 InitSize, char * FileNameP, char * PathNameP, Int16 InfoOnly);
ED_BufferPointer	ED_BufferReadFile(Int32 FD, char * NameP, char * PathP);
void		ED_BufferKill(ED_BufferPointer BufP);

void		ED_BufferDidSave(ED_BufferPointer BufP, ED_FramePointer ThisFrameP);
void		ED_BufferDidWrite(ED_BufferPointer BufP, ED_FramePointer ThisFrameP);
void		ED_BufferPlaceGap(ED_BufferPointer BufP, Int32 Offset, Int32 Len);
Int16		ED_BufferGetUTF8Len(char C);
Int16		ED_BufferCIsAlpha(register char C);
char *		ED_BufferPosToPtr(ED_BufferPointer BufP, Int32 Pos);
Int32	ED_BufferGetPosPlusRows(ED_BufferPointer BufP, Int32 Pos, Int32 *RowsP, Int32 ColLimit);	
Int32	ED_BufferGetPosMinusRows(ED_BufferPointer BufP, Int32 Pos, Int32 *RowsP, Int32 ColLimit);
Int32		ED_BufferFillStr(ED_BufferPointer BufP, char * StrP, Int32 StartPos, Int32 MaxLen, Int16 NoNL);
Int32		ED_BufferGetRowStartPos(ED_BufferPointer BufP, Int32 OldPos, Int32 Col);
Int32		ED_BufferGetLineStartPos(ED_BufferPointer BufP, Int32 OldPos);
Int32		ED_BufferGetLineEndPos(ED_BufferPointer BufP, Int32 OldPos);
Int32		ED_BufferGetLineLen(ED_BufferPointer BufP, Int32 Pos, Int16 BeforeToo);
void		ED_BufferGetLineCount(ED_BufferPointer BufP, Int32 *PosP, Int32 *CountP);
void		ED_BufferCheckNameCol(ED_BufferPointer BufP);
ED_BufferPointer	ED_BufferFindByName(char * NameP, char * PathP);
Int16		ED_BufferWriteFile(ED_BufferPointer BufP, Int32 FD);
Int16		ED_BufferNeedsFilter(ED_BufferPointer BufP);
void		ED_BufferDoFilter(ED_BufferPointer BufP, void (*UpdateFuncP)(Int16, void *), void * DataP);
void			EDCB_FilterEchoUpdate(Int16 Percent, void * DataP);
Int32		ED_BufferInsertLine(ED_BufferPointer BufP, Int32 Pos, char * StrP);
Int16		ED_BufferReadOnly(ED_BufferPointer BufP);

void		ED_CmdSplitPane(ED_PanePointer PaneP);
void		ED_CmdKillPane(ED_PanePointer PaneP);
void		ED_CmdKillOtherPanes(ED_PanePointer PaneP);
void		ED_CmdGoNextPane(ED_PanePointer PaneP);
void		ED_CmdExit(ED_PanePointer PaneP);
void		ED_CmdHelp(ED_PanePointer PaneP);
void		ED_CmdQuitAction(ED_PanePointer PaneP);
void		ED_CmdSetMark(ED_PanePointer PaneP);
void		ED_CmdGetCursorInfo(ED_PanePointer PaneP);
void		ED_CmdGoNextChar(ED_PanePointer PaneP);
void		ED_CmdGoNextWord(ED_PanePointer PaneP);
void		ED_CmdGoPrevChar(ED_PanePointer PaneP);
void		ED_CmdGoPrevWord(ED_PanePointer PaneP);
void		ED_CmdGoNextRow(ED_PanePointer PaneP);
void		ED_CmdGoPrevRow(ED_PanePointer PaneP);
void		ED_CmdGoNextPage(ED_PanePointer PaneP);
void		ED_CmdGoPrevPage(ED_PanePointer PaneP);
void		ED_CmdGoLineEnd(ED_PanePointer PaneP);
void		ED_CmdGoLineStart(ED_PanePointer PaneP);
void		ED_CmdGoBufEnd(ED_PanePointer PaneP);
void		ED_CmdGoBufStart(ED_PanePointer PaneP);
void		ED_CmdRecenterPage(ED_PanePointer PaneP);
void		ED_CmdSelectLine(ED_PanePointer PaneP);
void		ED_CmdSelectAll(ED_PanePointer PaneP);
void		ED_CmdSelectArea(ED_PanePointer PaneP);
void		ED_CmdExchMark(ED_PanePointer PaneP);
void		ED_CmdInsertTab(ED_PanePointer PaneP);
void		ED_CmdDelNextChar(ED_PanePointer PaneP);
void		ED_CmdDelNextWord(ED_PanePointer PaneP);
void		ED_CmdDelPrevChar(ED_PanePointer PaneP);
void		ED_CmdDelPrevWord(ED_PanePointer PaneP);
void		ED_CmdDelHSpace(ED_PanePointer PaneP);
void		ED_CmdJoinLines(ED_PanePointer PaneP);
void		ED_CmdDowncaseWord(ED_PanePointer PaneP);
void		ED_CmdUpcaseWord(ED_PanePointer PaneP);
void		ED_CmdCapitalizeWord(ED_PanePointer PaneP);
void		ED_CmdGotoLine(ED_PanePointer PaneP);
void		ED_CmdGotoChar(ED_PanePointer PaneP);
void		ED_CmdYank(ED_PanePointer PaneP);
void		ED_CmdYankPop(ED_PanePointer PaneP);
void		ED_CmdKillLine(ED_PanePointer PaneP);
void		ED_CmdKillRegion(ED_PanePointer PaneP);
void		ED_CmdCopyRegion(ED_PanePointer PaneP);
void		ED_CmdModBuffer(ED_PanePointer PaneP);
void		ED_CmdPopUpMarkList(ED_PanePointer PaneP);
void		ED_CmdPopUpKillList(ED_PanePointer PaneP);
void		ED_CmdPopUpCmdList(ED_PanePointer PaneP);
void		ED_CmdPopUpBufferList(ED_PanePointer PaneP);

void		ED_CmdWriteFile(ED_PanePointer PaneP);
void		ED_CmdSaveFile(ED_PanePointer PaneP);
void		ED_CmdSaveSomeFiles(ED_PanePointer PaneP);
void		ED_CmdFindFile(ED_PanePointer PaneP);
void		ED_CmdInsertFile(ED_PanePointer PaneP);
void		ED_CmdSwitchBuffer(ED_PanePointer PaneP);
void		ED_CmdKillBuffer(ED_PanePointer PaneP);
void		ED_CmdGetWorkingDir(ED_PanePointer PaneP);
void		ED_CmdExecNamedCmd(ED_PanePointer PaneP);
void		ED_CmdNewFrame(ED_PanePointer PaneP);
void		ED_CmdOtherFrame(ED_PanePointer PaneP);
void		ED_CmdDeleteFrame(ED_PanePointer PaneP);
void		ED_CmdDeleteOtherFrames(ED_PanePointer PaneP);
void		ED_CmdISearch(ED_PanePointer PaneP);
void		ED_CmdISearchBack(ED_PanePointer PaneP);
Int16			ED_ISHandleChars(ED_FramePointer FrameP, Uns16 SymChar, Int16 Mods, Int32 StrLen, char *StrP);
Int16			ED_ISCheckMatch(ED_BufferPointer BufP, Int32 Pos);
void			ED_ISAbortOut(char * MsgP);
void		ED_CmdQueryReplace(ED_PanePointer PaneP);
Int16			ED_QREPHandleChars(ED_FramePointer FrameP, Uns16 SymChar, Int16 Mods);
void			ED_QREPAbortOut(char * MsgP);
void		ED_CmdUndo(ED_PanePointer PaneP);
void		ED_CmdResetUndo(ED_PanePointer PaneP);
void		ED_CmdDisableUndo(ED_PanePointer PaneP);
void			ED_BufferInitUndo(ED_BufferPointer BufP);
void			ED_BufferKillUndo(ED_BufferPointer BufP);
void			ED_BufferAddUndoBlock(ED_BufferPointer BufP, Int32 Pos, Int32 Len, Uns8 Mode, char * DataP);

void		ED_XWinCreate(ED_FramePointer FP);
void		ED_ColorArrCreate(void);
void		ED_ColorArrDestroy(void);

void		ED_FrameQRAsk(ED_FramePointer FrameP, char * QueryP, char * RespP, Int16 QRType,
			      ED_QRRespFuncP RespFP, ED_QRAutoCompFuncP AutoCompFP);
void		ED_FrameQRSetResp(char * RespP);			      
void		ED_FrameQRDraw(ED_FramePointer FrameP);
void		ED_FrameQRDrawBlinker(ED_FramePointer FrameP);
void		ED_FrameQRDrawCursor(ED_FramePointer FrameP, Int16 Box);
void		ED_FrameQRAbortOut(char * MsgP);
void		ED_FrameQRHandleChars(ED_FramePointer FrameP, Uns16 SymChar, Int16 Mods, Int32 StrLen, char * StrP);
Int16		EDCB_QRAutoCompPathFunc(void);

void		ED_PUListConfigure(ED_PUPointer PUP, char * TitleP, Int32 TitleLen,
				   Int32 EntryTotal, Int32 EntryRows, Int32 LeftColChars);
void		ED_PUListCreate(ED_PUPointer PUP, ED_PanePointer PaneP, ED_PUReturnFuncP ReturnFP,
				ED_PUDrawFuncP DrawFP, void * DataP);


#ifdef DEBUG
    void	TEST_PrintUndoMemory(ED_BufferPointer BufP);
    void	TEST_StuffText(void);

    void	TEST_FillGap(ED_BufferPointer BufP)
    {
	char *	P = BufP->GapStartP;
	while (P < BufP->GapEndP) *P++ = 'X';
    }
#endif



// ******************************************************************************
// UTILITIES

// Returns ptr to last slash in Str
char *		ED_UtilGetLastStrSlash(char * Str)
{
    char *	P = Str;
    char *	LastP = P;

    while (*P) {
	if(*P == '/') LastP = P;
	P++;
    }

    return LastP;
}

// returns byte count to end of line--in KillRing NOT Buffer!
Int32	ED_UtilGetLineEnd(char * TextP, Int32 TextLen, Int32 * CharCountP) {
    char *	CurP = TextP;
    char *	EndP = TextP + TextLen;
    Int32	Count = 0;

    while (CurP < EndP) {
	Count += 1;
	if (*CurP == '\n') {
	    break;
	}
	CurP += ED_BufferGetUTF8Len(*CurP);
    }

    *CharCountP = Count;
    return (CurP - TextP);
}

// NewSP will be intersected with MainSP.  Result accumulates in MainSP.
// (So result can NEVER be longer than MainSP already is!  But MainSP
// can become shorter!)
void	ED_UtilIntersectStr(char * MainSP, char * NewSP)
{
    char	*SP = MainSP;
    char	*DP = NewSP;

    while (*SP && *DP) {
	if (*SP != *DP) break;
	SP++, DP++;
    }
    *SP = 0;		// In case it got shorter!
}

// Normalize InputP and write to PathP + NameP (FileName)
// Handles Tilde, Dot, and DotDot in leading position.
// Return 1 if Error, 0 if good!
Int16	ED_UtilNormalizePath(char * InputP, char * PathP, Int32 MaxPath, char * NameP, Int32 MaxName)
{
    char	*P, *StartP, *LastP;
    Int32	PathLen, DirLen, HalfPath = (MaxPath / 2) - 1;
    char	Str[ED_BUFFERPATHLEN + 1];
    Int16	DoTilde, DoDot, DoDotDot, NoPath;

    DoTilde = DoDot = DoDotDot = NoPath = 0;
    // First, eliminate leading blanks
    P = InputP;

    while (*P == ' ') P++;
    StartP = P;

    // Separate Path and filename--after last '/';
    LastP = ED_UtilGetLastStrSlash(P);

    // Possible to have just a filename, no path at all!
    NoPath = (LastP == StartP);

    if (*LastP == '/')
	strncpy(NameP, LastP + 1, MaxName);
    else
	strncpy(NameP, LastP, MaxName);		// Just a filename, no dir at all!
    NameP[MaxName - 1] = 0;			// just in case.

    PathLen = LastP - StartP;

    if ((StartP[0] == '~') && (StartP[1] == '/'))
	DoTilde = 1;
    else if (StartP[0] == '.') {
	if (StartP[1] == '/')
	    DoDot = 1;
	else if ((StartP[1] == '.') && (StartP[2] == '/'))
	    DoDotDot = 1;
    }

    if (DoTilde) {
	P = getenv("HOME");
	if (P == NULL)
	    P = getpwuid(geteuid())->pw_dir;
	if (P == NULL) return 1;
    } else if (NoPath || DoDot || DoDotDot) {
	P = getenv("PWD");
	if (P == NULL)
	    P = getcwd(Str, ED_BUFFERPATHLEN + 1);
	if (P == NULL) return 1;

	LastP = ED_UtilGetLastStrSlash(P);
	if (LastP == P) return 1;
    }

    if (NoPath) {
	sprintf(PathP, "%.*s", HalfPath, P);

    } else if (DoTilde) {
	if (PathLen > HalfPath) PathLen = HalfPath;
	sprintf(PathP, "%.*s%.*s", HalfPath, P, PathLen - 1, StartP + 1);

    } else if (DoDot) {
	if (PathLen > HalfPath) PathLen = HalfPath;
	sprintf(PathP, "%.*s%.*s", HalfPath, P, PathLen - 1, StartP + 1);

    } else if (DoDotDot) {
	DirLen = LastP - P;
	if (DirLen > HalfPath) DirLen = HalfPath;
	if (PathLen > HalfPath) PathLen = HalfPath;
	sprintf(PathP, "%.*s%.*s", DirLen, P, PathLen - 2, StartP + 2);

    } else {
	if (PathLen > MaxPath) PathLen = MaxPath;
	strncpy(PathP, StartP, PathLen);
	PathP[PathLen] = 0;
    }

    return 0;
}



// ******************************************************************************
// ED_BlinkHandler is called from MAIN event loop... Blinker is drawn once to show,
// again to erase--GXxor.  Only concerned with the Frame if it has focus, and then
// only with the current (active) Pane.  If showing a Box cursor, erase it.  Otherwise
// Draw/Erase solid cursor and toggle its flag bit.

void	ED_BlinkHandler(void)
{
    if (ED_CurFrameP && (ED_CurFrameP->Flags & ED_FRAMEFOCUSFLAG)) {
	ED_PanePointer		PaneP;

	if (ED_QRPaneP && (ED_QRPaneP->FrameP == ED_CurFrameP)) {
	    if (ED_CurFrameP->Flags & ED_FRAMEQRBOXCURSORFLAG)
		ED_FrameQRDrawCursor(ED_CurFrameP, 1);
	    ED_FrameQRDrawCursor(ED_CurFrameP, 0);
	} else {

	    PaneP = ED_CurFrameP->CurPaneP;
	    if (PaneP->Flags & ED_PANEBOXCURSORFLAG)	// Erase Box
		ED_PaneDrawCursor(PaneP, 1);

	    ED_PaneDrawCursor(PaneP, 0);		// Draw solid
	    ED_FrameCommandEcho(ED_CurFrameP, 1);
	}
    }
}

// ******************************************************************************
// FReg and FBind:  Command functions in the editor are registered, so they can
// be invoked by name or bound to a control (or meta) key sequence.  Each new
// function is initially registered to get an FRegPointer.  Once registered,
// the FRegPointer can be given any number of FBind key-sequence bindings.  But a
// given key-sequence can only bind to a single function... so a FRegNewBind
// will overwrite an existing key-seq binding.

// ******************************************************************************
// ED_FRegInit will initialize the function and binding registery.  Allocation is
// off the SAStore mechanism, so the stores are Opened.

void	ED_FRegInit(void)
{
    sc_SAStoreOpen(&ED_FRegStore, sizeof(ED_FRegRecord), ED_FREGALLOCCOUNT);
    ED_FirstFRegP = NULL;

    sc_SAStoreOpen(&ED_FBindStore, sizeof(ED_FBindRecord), ED_FBINDALLOCCOUNT);
    ED_FirstBindP = NULL;
}

// ******************************************************************************
// ED_FRegKill will clear out the Function and Binding registery and close
// their allocation stores.

void	ED_FRegKill(void)
{
    ED_FirstFRegP = NULL;
    sc_SAStoreClose(&ED_FRegStore);

    ED_FirstBindP = NULL;
    sc_SAStoreClose(&ED_FBindStore);
}

// ******************************************************************************
// ED_FRegNewFunction will register a new command function and establish its name.

ED_FRegPointer		ED_FRegNewFunction(ED_FRegFuncP FuncP, char * NameP)
{
    ED_FRegPointer	NewFRP, PrevFRP, NextFRP;

    NewFRP = sc_SAStoreAllocBlock(&ED_FRegStore);
    NewFRP->SortNextP = NewFRP->SortPrevP = NULL;
    NewFRP->NameP = NameP;
    NewFRP->FuncP = FuncP;
    NewFRP->FirstBindP = NULL;

    PrevFRP = NULL;
    NextFRP = ED_FirstFRegP;
    while (NextFRP) {

	if (strcmp(NameP, NextFRP->NameP) <= 0) break;

	PrevFRP = NextFRP;
	NextFRP = NextFRP->SortNextP;
    }

    NewFRP->SortPrevP = PrevFRP;
    if (PrevFRP) PrevFRP->SortNextP = NewFRP;
    else ED_FirstFRegP = NewFRP;
    NewFRP->SortNextP = NextFRP;
    if (NextFRP) NextFRP->SortPrevP = NewFRP;
     
    return NewFRP;
}

// ******************************************************************************
// ED_FregNewBinding will record a new Binding for the registered function.  The
// FRegP can have many different bindings, but a given KeyStr can only be bound
// once--previous bindings will be overwritten.

ED_FBindPointer	ED_FRegNewBinding(ED_FRegPointer FRegP, char * KeyStrP)
{
    ED_FBindPointer	NewFBP, PrevFBP, NextFBP;
    Int16		Res;

    if (strlen(KeyStrP) >= ED_FBINDKEYSTRLEN)
	G_SETEXCEPTION("FRegNewBinding too long", 0);

    Res = 1;			// Not 0 if FirstBindP is NULL!!
    PrevFBP = NULL;
    NextFBP = ED_FirstBindP;
    while (NextFBP) {
	Res = strcmp(KeyStrP, NextFBP->KeyStr);
	if (Res <= 0) break;

	PrevFBP = NextFBP;
	NextFBP = NextFBP->SortNextP;
    }

    if (Res == 0) {
	// Binding already exits, remove from old FRegP chain, it will be recycled!
	// No need to alter SortNextP/SortPrevP chain, it is already linked correctly.
	
	NewFBP = NextFBP;
	if (NewFBP->FuncNextP) NewFBP->FuncNextP->FuncPrevP = NewFBP->FuncNextP;
	if (NewFBP->FuncPrevP) NewFBP->FuncPrevP->FuncNextP = NewFBP->FuncNextP;
	if (NewFBP->FRegP->FirstBindP == NewFBP) NewFBP->FRegP->FirstBindP = NewFBP->FuncNextP;
	NewFBP->FRegP = FRegP;

    } else {
	// Binding does not exit, create and use.

	NewFBP = sc_SAStoreAllocBlock(&ED_FBindStore);
	NewFBP->SortNextP = NewFBP->SortPrevP = NULL;
	NewFBP->FRegP = FRegP;
	strcpy(NewFBP->KeyStr, KeyStrP);

	NewFBP->SortPrevP = PrevFBP;
	if (PrevFBP) PrevFBP->SortNextP = NewFBP;
	else ED_FirstBindP = NewFBP;
	NewFBP->SortNextP = NextFBP;
	if (NextFBP) NextFBP->SortPrevP = NewFBP;
    }
     
    NewFBP->FuncNextP = FRegP->FirstBindP;
    NewFBP->FuncPrevP = NULL;
    if (FRegP->FirstBindP) FRegP->FirstBindP->FuncPrevP = NewFBP;
    FRegP->FirstBindP = NewFBP;

    return NewFBP;
}

// ******************************************************************************
// ED_FRegFindBinding is called to get a binding for a given KeyStr (key sequence).
// This KeyStr will be matched incrementally, as the user is typing it, so it
// is important to recognize partial matches, as opposed to obvious mismatches.
// In the case of a partial match, the *StartBPP value will refer to the
// ED_FBindPointer that should be used to re-start the match once another
// char in the KeyStr has been entered.  In the case of a perfect match, this
// field will indicate the matching FBindPointer... otherwise, throw away value.
//
// Return Values:
// -1	There are no matching bindings, adding more Chars will not help.
// 1	Partial match, keep typing
// 99	Found perfect match, get the function from *StartBPP

Int16	ED_FRegFindBinding(ED_FBindPointer *StartBPP, char * KeyStrP)
{
    Int16		Res;
    
    while (*StartBPP) {
	Res = ED_FRegBindCompare(KeyStrP, (*StartBPP)->KeyStr);
	if (Res) return Res;

        *StartBPP = (*StartBPP)->SortNextP;
    }

    return -1;
}

// ******************************************************************************
// ED_FRegBindCompare is a special incremental match comparing the entered
// key-sequence (EntryP) and binding KeyStr.  It is assumed that
// FBindRecords will be fed to it in strict ascending alphabetic order.
//
// Return values:
// -1	Mismatch: give up, no match will be found.
// 0	Get next BindP: Look in the next FBindRecord.
// 1	Partial match: Looks promising, type more chars
// 99	Full match: Bingo!

Int16	ED_FRegBindCompare(register char * EntryP, register char * KeyStrP)
{
#ifdef DEBUG
    if (0) {
	char	EStr[64], KStr[64];

	ED_KeyStrPrint(EntryP, EStr);
	ED_KeyStrPrint(KeyStrP, KStr);
	printf("BindCompare Entry:[%s] <%x %x %x> --> Binding:[%s]\n", EStr, EntryP[0], EntryP[1], EntryP[2], KStr);
    }
#endif

    // Must cast to (Uns8 *) otherwise <DELETE> which is 0xFF would NOT match!!
    while (*EntryP && *KeyStrP) {
	if (*(Uns8 *)EntryP < *(Uns8 *)KeyStrP) return -1;	// Already too late! MISMATCH
	if (*(Uns8 *)EntryP > *(Uns8 *)KeyStrP) return 0;	// Get next binding
	EntryP++, KeyStrP++;			// Obviously ==, get next
    }

    if (*EntryP) return -1;			// BindP ran out, so MISMATCH
    if (*KeyStrP) return 1;			// EntryP ran out, so PARTIAL
    return 99;					// Both ran out, FULL MATCH			
}

// ******************************************************************************
// ED_KeyStrPrintExtKey will write the name of the special Key (within < and > to DestP.
// It will get the name from XLib, but use the Hex KeySym code if the name is NULL.
//
// Not particularly fast or efficient, but no need.

Int16	ED_KeyStrPrintExtKey(char * DestP, char Key)
{
    Uns32	KS = 0x0000ff00 | (Uns8)Key;	// DO NOT sign-extend!
    char *	XStrP;
    Int16	Len;

    XStrP = XKeysymToString(KS);

    if ((XStrP == NULL) || (0 == strcmp(XStrP, "(null)")))
	Len = sprintf(DestP, "<%x>", KS);
    else
	Len = sprintf(DestP, "<%s>", XStrP);

    return Len;
}

// ******************************************************************************
// ED_KeyStrPrint will write the KeyStr (from a Binding) to DestP and returns
// its length--also inserts terminating NULL to be safe.

Int16	ED_KeyStrPrint(char * KeyStrP, char * DestP)
{
    char	*KP = KeyStrP;
    char	*DP = DestP;
    char	*XStrP;

    while (*KP) {
	switch (*KP) {
	    case ED_CTLCOMMANDCODE:
		*DP++ = 'C'; *DP++ = '-';
		break;

	    case ED_ALTCOMMANDCODE:
		*DP++ = 'M'; *DP++ = '-';
		break;

	    case ED_SUPCOMMANDCODE:
		*DP++ = 'S'; *DP++ = '-';
		break;

	    case ED_HYPCOMMANDCODE:
		*DP++ = 'H'; *DP++ = '-';
		break;

	    case ED_EXTCOMMANDCODE:
		KP++;
		DP += ED_KeyStrPrintExtKey(DP, *KP); *DP++ = ' ';
		break;

	    case ' ': // handle <Space>, hard to read
		XStrP = XKeysymToString((Uns32)*KP);
		DP += sprintf(DP, "<%s>", XStrP);
		*DP++ = ' ';
		break;

	    default:
		*DP++ = *KP; *DP++ = ' ';
		break;
	}
	KP++;
    }

     *DP = 0;			// Terminating Null to be safe
     return (DP - DestP);
}



// ******************************************************************************
// ******************************************************************************
// Copy/Paste
// Supports PRIMARY and CLIPBOARD XSel
//
// NOTE:	XLib calls these 'selection", but we call them 'XSel' because
//		selection refers to selecting a range of text in the editor--and
//		getting Orange hilite.
//
// Internal Primary data is either the top of the KillRing (PrimaryBufP == NULL) or
// on PrimaryBufP, starting at PrimaryPos and going PrimaryLen bytes.  External
// is of course obtained from XConvertSelection.  Primary XSel is relinquished
// (volunatarily) when the Buffer is closed or the XSel range is altered in
// some way--i.e. test is deleted/inserted in it.
//
// Internal Clip data is *ALWAYS* the top of the KillRing... so no need for a
// Buffer spec.  As with Primary, external CLIPBOARD data must be obtained with
// XConvertSelection.  Clipboard XSel is never relinquished, unless explicitly
// requested by server... as in when another application does a Copy.
//
// A new (internal) Kill/Copy sets Primary *AND* Clip.
// A new selection range sets Primary XSel (only).
//
// A special unmapped ED_XSelWin is used for INCR data import.

Int32			ED_XSelSizeLimit;		// Max size of each INCR data block
Window			ED_XSelWin = 0;			// Hidden XWin for XSelection

Int16			ED_ClipOwn;			// We OWN the CLIPBOARD selection

Int16			ED_PrimaryOwn;			// We OWN the PRIMARY selection
ED_BufferPointer	ED_PrimaryBufP;
Int32			ED_PrimaryPos;
Int32			ED_PrimaryLen;

struct	pollfd		ED_XSelPollR;			// So can limit to N mSec searching for an event
struct	timeval		ED_XSelSetTimeR;		// To see how much time is left.


// Create XSelWin.  It is never mapped, just used for XSelection handling.
// MUST XSelectInput, otherwise NO events, EVER!!
void    ED_XSelInit(void)
{
    Int16	XScreenN = DefaultScreen(ED_XDP);

    ED_XSelSizeLimit = XMaxRequestSize(ED_XDP);			// Value is for N longs, we go 1/4 for Bytes!
    ED_XSelWin = XCreateSimpleWindow(ED_XDP, RootWindow(ED_XDP, XScreenN), 0, 0, 10, 10, 0, 0, 0);
    XSelectInput(ED_XDP, ED_XSelWin, PropertyChangeMask);
    
    sc_WERegAdd(ED_XSelWin, &ED_XSelHandleEvent, NULL);		// Register with main loop
}

void	ED_XSelKill(void)
{
    XDestroyWindow(ED_XDP, ED_XSelWin);
    ED_XSelWin = 0;
}

// Initialize the timeout-timer for XSel.
void	ED_AuxInitXSelTimer(void)
{
    ED_XSelPollR.fd = ConnectionNumber(ED_XDP);
    ED_XSelPollR.events = POLLIN;
}

// Set the timeout-timer to measure from NOW!
void	ED_AuxResetXSelTimer(void)
{
    gettimeofday(&ED_XSelSetTimeR, NULL);
}

// Can *ONLY* use in a do{}while loop, since poll will **NOT** kick out
// if the Event has already been queued.  Be sure to process multiple
// events in the loop... as there could be extraneous PropertyNotify
// events that could gum-up the works!
Int16	ED_AuxWaitXSelTimer(Int32 MSecs)
{
    struct timeval	CurTime;
    Int32		DeltaT;

    gettimeofday(&CurTime, NULL);
    DeltaT = (1024 * (CurTime.tv_sec - ED_XSelSetTimeR.tv_sec)) +
	     ((CurTime.tv_usec - ED_XSelSetTimeR.tv_usec) / 1024);

    if (DeltaT >= MSecs || 0 == poll(&ED_XSelPollR, 1, MSecs - DeltaT))
	return 0;

    return 1;
}

// Send requested Data... whether PRIMARY or CLIPBOARD, whether from BufP or KillRing.
// Will process (involves complex hand-shaking + wait) locally rather than asynch to
// eliminate possibility of buffer closing, gap moving, KillRing changing, another Sel....
// Return 1 == Success
// Return 0 == Fail
//
// NOTE:	Had ProgressBar display originally... but works toooo fast, even
//		for multi-meg sized files, so no need.  Sigh.
Int16	ED_XSelSendINCRData(XSelectionEvent * SEP, Atom Type, char * DataP, Int32 DataLen)
{
    XEvent		XE;
    Int16		StepDone;
    Int32		CurStep, Len, LenLeft;

    // Get the number of Steps + add one for the INCR step.
    // StepCount = 1 + ((ED_PrimaryLen + ED_XSelSizeLimit - 1) / ED_XSelSizeLimit);
    CurStep = 0;

    // Announce INCR protocol
    XChangeProperty(ED_XDP, SEP->requestor, SEP->property, ED_IncrAtom, 32, PropModeReplace,
		    (unsigned char *)&DataLen, 1);
    XSendEvent(ED_XDP, SEP->requestor, 0, 0, (XEvent *)SEP);

    // Look at requestor Win, ready for PropertyChange events.
    // Stop looking at requestor Win when done... don't want events FOREVER.
    // **MUST** Flush here... otherwise will NOT get events while we wait!
    XSelectInput(ED_XDP, SEP->requestor, PropertyChangeMask);
    XFlush(ED_XDP);

    // Initialize timer mechanism... so can timeout if no Event!
    ED_AuxInitXSelTimer();

    // Allow 2 Full seconds for accept/delete of INCR prop
    StepDone = 0;
    ED_AuxResetXSelTimer();
    do {
	// May have spurious PropertyNewValue before the PropertyDelete... so loop here.
	while (XCheckWindowEvent(ED_XDP, SEP->requestor, PropertyChangeMask, &XE)) {
	    if ((XE.xproperty.state == PropertyDelete) &&
	        (XE.xproperty.atom == SEP->property)) {

		StepDone = 1;
		CurStep += 1;
		break;
	    }
	}

	if (StepDone) break;
    } while (ED_AuxWaitXSelTimer(2048));
    
    if (! StepDone) goto Failed;
    
    // Now transmit all N parts, 1 chunk at a time... allow 2 sec timeout
    LenLeft = DataLen;
    while (LenLeft) {
	Len = ED_XSelSizeLimit;
	if (Len > LenLeft) Len = LenLeft;

	// printf("Sending %d/%d  Len:%d LenLeft:%d\n ", CurStep, StepCount - 1, Len, LenLeft);
	XChangeProperty(ED_XDP, SEP->requestor, SEP->property, Type, 8, PropModeReplace,
			(unsigned char *)DataP + (DataLen - LenLeft), Len);
	XFlush(ED_XDP);

	// Wait until PropertyDelete, then repeat with next chunk
	StepDone = 0;
	ED_AuxResetXSelTimer();
	do {
	    // May have >1 PropertyNotify events... one Delete and one NewValue!
	    while (XCheckWindowEvent(ED_XDP, SEP->requestor, PropertyChangeMask, &XE)) {
		if ((XE.xproperty.state == PropertyDelete) &&
		    (XE.xproperty.atom == SEP->property)) {

		    StepDone = 1;
		    CurStep += 1;
		    break;
		}
	    }

	    if (StepDone) break;
	} while (ED_AuxWaitXSelTimer(2014));

	if (! StepDone) goto Failed;
	LenLeft -= Len;
    }

    // Finally, write a zero-len data to end it all--and stop looking at requestor win events.
    XChangeProperty(ED_XDP, SEP->requestor, SEP->property, Type, 8, PropModeReplace, 0, 0);
    XSelectInput(ED_XDP, SEP->requestor, NoEventMask);
    XFlush(ED_XDP);    
    return 1;

Failed:
    XSelectInput(ED_XDP, SEP->requestor, None);
    return 0;
}

// Send the data to the requestor window.  SEP is the SelectionNotify event
// that should be sent--in response to a SelectionRequest.  All the fields
// in it are already set by the caller.
void	ED_XSelSendData(XSelectionEvent * SEP, Atom Type)
{
    char	* DataP = NULL;
    Int32	DataLen = 0;

    if (SEP->property == None) SEP->property = Type;	// Ancient clients?

    // PRIMARY can come from Buffer or KillRing... but check that we own it.
    // CLIPBOARD can only come from KillRing... but check that we own it.
    
    if (SEP->selection == XA_PRIMARY) {
	if (ED_PrimaryOwn == 0) goto RejectRequest;

	if (ED_PrimaryBufP) {			// Source == Buffer
	    ED_BufferPlaceGap(ED_PrimaryBufP, ED_PrimaryPos, 0);
	    DataP = ED_PrimaryBufP->GapEndP;
	    DataLen = ED_PrimaryLen;
	} else					// Source == KillRing
	    ED_KillRingYank(0, &DataP, &DataLen, 1);
	
    } else if (SEP->selection == ED_ClipAtom) {
	if (ED_ClipOwn == 0) goto RejectRequest;

	ED_KillRingYank(0, &DataP, &DataLen, 1);
    }

    // Wrong SEP->selection or other problems...
    if (DataLen == 0) goto RejectRequest;

    // Send in 1 step, or go INCRemental
    if (DataLen <= ED_XSelSizeLimit) {
	XChangeProperty(ED_XDP, SEP->requestor, SEP->property, Type, 8, PropModeReplace,
			(unsigned char *)DataP, DataLen);
	XSendEvent(ED_XDP, SEP->requestor, 0, 0, (XEvent *)SEP);

    } else {
	if (! ED_XSelSendINCRData(SEP, Type, DataP, DataLen));
	    goto RejectRequest;
    }
    return;
    
RejectRequest:
    SEP->property = None;
    XSendEvent(ED_XDP, SEP->requestor, 0, 0, (XEvent *)SEP);
}

// Get incoming XSel data using the (ICCCM) INCR protocol.  Accumulate the data in
// TempBufP, as it comes in.  When all is done, filter the data if ncessary, and
// insert into PaneP.
//
// NOTE:	Beware of spurious PropNotify events... must get rid of PropertyDelete
//		ones if looking for PropertyNewValue ones.  Must call XCheckWindowEvent
//		in a loop.
//
// NOTE:	Protocol asks for initiating INCR property to have DataLen (actually
//		a lower bound on DataLen) as value.  But code defensively ignores DataLen,
//		in case the XSel owner does not send it (or sends a strange value).
//		Instead, simply takes all the blocks and stops when the NULL one shows up.
Int16	ED_XSelInsertINCRData(ED_PanePointer PaneP, Atom SelAtom)
{
    ED_BufferPointer	TempBufP;
    XEvent		XE;
    Int16		Done, GotBlock;
    Atom		ResType;
    char		*DataP;
    Uns64		DataLen, LenLeft;
    Int32		ResFormat;
    
    TempBufP = ED_BufferNew(ED_BUFINITLEN, NULL, NULL, 0);
    
    Done = 0;
    while (! Done) {
	GotBlock = 0;
	ED_AuxResetXSelTimer();
	do {
	    // PropertyChange events will come in pairs, one NewValue and one Delete!
	    while (XCheckWindowEvent(ED_XDP, ED_XSelWin, PropertyChangeMask, &XE)) {
		if ((XE.xproperty.state == PropertyNewValue) &&
		    (XE.xproperty.atom == SelAtom)) {
		    GotBlock = 1;
		    break;
		}
	    }

	    if (GotBlock) break;
	} while (ED_AuxWaitXSelTimer(2048));

	if (! GotBlock) goto Fail;
	if (Success == XGetWindowProperty(ED_XDP, ED_XSelWin, SelAtom, 0L, LONG_MAX, True, AnyPropertyType,
					  &ResType, &ResFormat, &DataLen, &LenLeft, (unsigned char **)&DataP)) {
	    if (DataLen) {
		ED_BufferPlaceGap(TempBufP, TempBufP->LastPos, DataLen);	// May expand it!
		memcpy(TempBufP->GapStartP, DataP, DataLen);
		TempBufP->GapStartP += DataLen;
		TempBufP->LastPos += DataLen;
	    } else
		Done = 1;
	    XFree(DataP);
	} else
	    goto Fail;
    }

    if (TempBufP->LastPos) {
	if (ED_BufferNeedsFilter(TempBufP))
	    ED_BufferDoFilter(TempBufP, NULL, NULL);
	ED_BufferPushMark(PaneP->BufP, PaneP->CursorPos);
	ED_PaneInsertBufferChars(PaneP, TempBufP, 1);
	ED_FrameDrawAll(PaneP->FrameP);
    }
    ED_BufferKill(TempBufP);
    return 1;

Fail:
    ED_BufferKill(TempBufP);
    return 0;
}

// Insert XSel data (paste it) on PaneP.  Asks for UTF8_STRING first, then simply
// STRING if that fails.  Whatever SelAtom is (PRIMARY or CLIPBOARD), uses it as
// the transfer property on ED_XSelWin!  Calls ED_XSelInsetINCRData if the owner
// (of the XSel) responds with an INCR protocol.
//
// NOTE:	A Mark is pushed at the beginning of the insertion point... just
//		as when Yanking locally!
//
// NOTE:	INCR protocol relies on PropNotify events.  *MUST* process/dispose
//		of 2 such events for every block that is transmitted--one DELETE
//		and one NEWVALUE!!  Otherwise, it will gum-up the transmission.
//
// NOTE:	Incoming data may have Tabs/CRLF... so need filtering.
//		Best to read it into a TempBufP, then filter if necessary, and
//		finally insert into PaneP!
Int16	ED_XSelInsertData(ED_PanePointer PaneP, Atom SelAtom)
{
    ED_BufferPointer	TempBufP;
    XEvent		XE;
    Atom		TypeAtom, ResType;
    char		*DataP;
    Uns64		DataLen, LenLeft;
    Int32		ResFormat;

    ED_AuxInitXSelTimer();
    TypeAtom = ED_UTF8Atom;
    
TryAgain:

    XConvertSelection(ED_XDP, SelAtom, TypeAtom, SelAtom, ED_XSelWin, CurrentTime);
    ED_AuxResetXSelTimer();
    XFlush(ED_XDP);
    do {
	if (XCheckTypedWindowEvent(ED_XDP, ED_XSelWin, SelectionNotify, &XE)) {
	    
	    if (XE.xselection.property == None) {
		if (TypeAtom == ED_UTF8Atom) {
		    TypeAtom = XA_STRING;
		    goto TryAgain;
		} else
		    goto FailReturn;

	    } else if ((XE.xselection.target == ED_UTF8Atom) ||
		       (XE.xselection.target == ED_TextAtom) ||
		       (XE.xselection.target == XA_STRING)) {
		XEvent		IgnoreE;

		// XSel Owner responded with a Target type we know and like.
		// Get rid of previous PropertyChange events... may have a few!
		while (XCheckWindowEvent(ED_XDP, ED_XSelWin, PropertyChangeMask, &IgnoreE));
		
		if (Success == XGetWindowProperty(ED_XDP, ED_XSelWin, SelAtom, 0L, LONG_MAX, True, AnyPropertyType,
						  &ResType, &ResFormat, &DataLen, &LenLeft, (unsigned char **)&DataP)) {
		    // If ResType is INCR, switch to INCRemental transfer protocol.
		    // Otherwise, we got the data...!!
		    if (ResType == ED_IncrAtom) {
			//printf("INCR -> Length %d\n", *(Uns32 *)DataP);
			XFree(DataP);
			if (ED_XSelInsertINCRData(PaneP, SelAtom))
			    return 1;
			else
			    goto FailReturn;
		    } else {

			if (DataLen) {
			    TempBufP = ED_BufferNew(DataLen + ED_GAPEXTRAEXPAND, NULL, NULL, 0);
			    memcpy(TempBufP->GapStartP, DataP, DataLen);
			    TempBufP->GapStartP += DataLen;
			    TempBufP->LastPos = DataLen;

			    if (ED_BufferNeedsFilter(TempBufP))
				ED_BufferDoFilter(TempBufP, NULL, NULL);

			    ED_BufferPushMark(PaneP->BufP, PaneP->CursorPos);
			    ED_PaneInsertBufferChars(PaneP, TempBufP, 1);
			    ED_BufferKill(TempBufP);
			    ED_FrameDrawAll(PaneP->FrameP);
			}
			XFree(DataP);			
			return 1;
		    }
		} else
		    goto FailReturn;

	    }
	}
    } while (ED_AuxWaitXSelTimer(2048));

FailReturn:
    ED_FrameFlashError(PaneP->FrameP);
    return 0;
}

// Event handler (for ED_XSelWin) registered with the main event loop.
// Incoming SelectionRequest will trigger XSelSendData.  Will also get
// incoming SelectionClear requests--as other apps claim the XSel (PRIMARY
// or CLIPBOARD).  SelectionNotify is generally ignored here as XSelInsertData
// will look for it locally.  INCR protocol is handled locally, rather than
// through main event loop.
void	ED_XSelHandleEvent(XEvent * EventP, void * DataP)
{
    switch (EventP->type) {
	case SelectionClear:
	    // printf("XSel Clear %ld\n", EventP->xselectionclear.selection);
	    if (EventP->xselectionclear.selection == ED_ClipAtom)
		ED_XSelLostClip();
	    else if (EventP->xselectionclear.selection == XA_PRIMARY)
		ED_XSelLostPrimary();
	    break;

	case SelectionRequest: {
	    XSelectionRequestEvent	*SREP = &EventP->xselectionrequest;
	    XSelectionEvent		SE;
	    Atom			TargetsArr[] = {ED_UTF8Atom, ED_TextAtom, XA_STRING};

	    SE.type = SelectionNotify;
	    SE.display = ED_XDP;
	    SE.requestor = SREP->requestor;
	    SE.selection = SREP->selection;
	    SE.target = SREP->target;
	    SE.property = SREP->property;	// 'None' for no response
	    SE.time = SREP->time;

	    // printf("XSel Request Req:%ld, Sel: %ld  Target:%ld  Property:%ld\n",
	    // 	    SREP->requestor, SREP->selection, SREP->target, SREP->property);
	    
	    if (SREP->target == ED_TargetsAtom) {
		// Always reply with UTF8_STRING... without checking if we are still the owner
		XChangeProperty(ED_XDP, SE.requestor, SE.property, XA_ATOM, 32,
				      PropModeReplace, (unsigned char *)TargetsArr, sizeof(TargetsArr)/sizeof(Atom));
		XSendEvent(ED_XDP, SE.requestor, 0, 0, (XEvent *)&SE);

	    } else if ((SREP->target == XA_STRING) || (SREP->target == ED_TextAtom)) {
		ED_XSelSendData(&SE, XA_STRING);
		
	    } else if (SREP->target == ED_UTF8Atom) {
		ED_XSelSendData(&SE, ED_UTF8Atom);
		
	    } else {
		// Unkown target
		SE.property = None;
		XSendEvent(ED_XDP, SE.requestor, 0, 0, (XEvent *)&SE);
	    }
	    
	    break;
	}
	
	case SelectionNotify:
	    // printf("XSel Notify\n");
	default:
	    break;
    }
}

// Call whenever a region is selected--hilited.  Holds on to the Primary
// unless forced to relinquish, so may *already* own the Primary even
// as the Sel changes.  In that case, simply change the vars, no need
// to involve Xlib.
//
// NOTE:	ED_XSelWin is used to minimize disruptions and calls
//		as Frames/win come and go.
//
// NOTE:	ED_XSelSetClip will *ALSO* set ED_PrimaryOwn!!
//		ED_PrimaryBufP == NULL if Primary is KillRing!
void	ED_XSelSetPrimary(ED_PanePointer PaneP)
{
    if (ED_PrimaryOwn == 0) {
	XSetSelectionOwner(ED_XDP, XA_PRIMARY, ED_XSelWin, CurrentTime);
	if (XGetSelectionOwner(ED_XDP, XA_PRIMARY) != ED_XSelWin)
	    return;
    }

    ED_PrimaryOwn = 1;
    ED_PrimaryBufP = PaneP->BufP;
    if (PaneP->CursorPos < ED_SelMarkPos) {
	ED_PrimaryPos = PaneP->CursorPos;
	ED_PrimaryLen = ED_SelMarkPos - PaneP->CursorPos;

    } else {
	ED_PrimaryPos = ED_SelMarkPos;
	ED_PrimaryLen = PaneP->CursorPos - ED_SelMarkPos;
    }
}

// Call if PrimaryBufP is killed... or region (PrimaryPos -> PrimaryLen)
// is altered!
void	ED_XSelReleasePrimary(ED_BufferPointer BufP)
{
    if (ED_PrimaryBufP) {
	ED_PrimaryBufP = NULL;
	
	if (ED_PrimaryOwn) {
	    XSetSelectionOwner(ED_XDP, XA_PRIMARY, None, CurrentTime);
	    ED_PrimaryOwn = 0;
	}
    }
}

// PRIMARY XSel (when not referring to the KillRing) consists of a Pos
// and a Len to specify a range.  Pos is just an offset relative to the
// beginning of the Buffer, so it must be updated when any data is added
// or deleted BEFORE the specified Pos.  If data in the Pos-Len range is
// altered, then it is best to relinquish the PRIMARY XSel--user can
// re-select later...
void	ED_XSelAlterPrimary(ED_BufferPointer BufP, Int32 Pos, Int32 Len)
{
    if ((BufP != ED_PrimaryBufP) || (Len == 0))
	return;

    if (Pos <= ED_PrimaryPos) {
	ED_PrimaryPos += Len;
	if (ED_PrimaryPos < Pos)
	    ED_XSelReleasePrimary(ED_PrimaryBufP);
    } else if (Pos < (ED_PrimaryPos + ED_PrimaryLen))
	ED_XSelReleasePrimary(ED_PrimaryBufP);
}

// Call after a Kill/Copy... Top of KillRing is *ON* clipboard!
// Will *ALSO* set primary... but ED_PrimaryBufP == NULL means look to
// get it from the KillRing--just like the clipboard!
void	ED_XSelSetClip(void)
{
    if (ED_ClipOwn == 0) {
	XSetSelectionOwner(ED_XDP, ED_ClipAtom, ED_XSelWin, CurrentTime);
	if (XGetSelectionOwner(ED_XDP, ED_ClipAtom) == ED_XSelWin) {
	    ED_ClipOwn = 1;
	}
    }

    if (ED_PrimaryOwn == 0) {
	XSetSelectionOwner(ED_XDP, XA_PRIMARY, ED_XSelWin, CurrentTime);
	if (XGetSelectionOwner(ED_XDP, XA_PRIMARY) == ED_XSelWin) {
	    ED_PrimaryOwn = 1;
	    ED_PrimaryBufP = NULL;
	}
    }
}

// Called when system claims PRIMARY for another app.
void	ED_XSelLostPrimary(void)
{
    ED_PrimaryOwn = 0;
}

// Called when system claims CLIPBOARD for another app.
void	ED_XSelLostClip(void)
{
    ED_ClipOwn = 0;
}

// ******************************************************************************
// ******************************************************************************
// ED_KillRing is a single statically allocated structure that manages two memory
// buffers, one for the current (Top) kill, and one for the accumulated previous
// (Rest) kills.  If the user kills a number of words/lines in sequences, they are
// all added and combined in the Top buffer.  But if another command is executed,
// the Top buffer is frozen.  If the user kills another line, the frozen Top will
// be transfered to the Rest buffer, and the new kill data will be placed in Top.
//
// The Elts array in ED_KillRing points to the buffers and implements a circular
// stack... Indices wrap around using ED_KILLRINGMASK.  

void	ED_KillRingInit(void)
{
    char		*TP, *RP;
    Int16		I;
    ED_KEPointer	KEP;

    TP = malloc(ED_KRTOPINITLEN);
    if (! TP) G_SETEXCEPTION("Malloc KillRing Top Failed", 0);

    RP = malloc(ED_KRRESTINITLEN);
    if (! RP) G_SETEXCEPTION("Malloc KillRing Rest Failed", 0);

    ED_KillRing.Flags = ED_KRNOFLAG;
    ED_KillRing.TopP = TP;
    ED_KillRing.TopLen = ED_KRTOPINITLEN;
    ED_KillRing.RestP = RP;
    ED_KillRing.RestLen = ED_KRRESTINITLEN;
    ED_KillRing.RestDataLen = 0;
    ED_KillRing.TopI = 0;
    ED_KillRing.YankI = (ED_KillRing.TopI - 1) & ED_KILLRINGMASK;

    KEP = ED_KillRing.EltArr;
    for (I = 0; I < ED_KILLRINGCOUNT; KEP++, I++) {
	KEP->Flags = ED_KENOFLAG;
	KEP->Pos = 0;
	KEP->Len = 0;
    }
    ED_KillRing.EltArr[0].Flags |= ED_KETOPFLAG;
}

void	ED_KillRingFree(void)
{
    free(ED_KillRing.TopP);
    free(ED_KillRing.RestP);
    ED_KillRing.TopP = NULL;
    ED_KillRing.RestP = NULL;
    // Assume everything is being zapped, no reason to cleanup.
}

// ******************************************************************************
// ED_KillRingAppendTop is called to append new data to the TOP element of the
// KillRing.  This happens only if the user KILLS again, with a routine that goes
// in the forward direction.  The idea is to combine multiple kills into a single
// one that can be yanked back.
//
// NOTE:	The whole reason to have a separate TopP buffer (instead of RestP)
//		is to allow for these appends as the user makes multiple kills
//		in sequence.

void	ED_KillRingAppendTop(char * DataP, Int32 DataLen)
{
    ED_KEPointer	KEP = ED_KillRing.EltArr + ED_KillRing.TopI;
    char *		MemP = ED_KillRing.TopP;
    Int32		Len;

    // Enough room?  Slide contents Up first, then expand!
    while (KEP->Pos + KEP->Len + DataLen > ED_KillRing.TopLen) {
	if (KEP->Pos) {
	    memmove(MemP, MemP + KEP->Pos, KEP->Len);
	    KEP->Pos = 0;
	    continue;
	}

	// Pos is already 0 if we get here.
	Len = KEP->Len + DataLen + ED_KRTOPEXTRA;
	MemP = realloc(MemP, Len);
	if (! MemP) G_SETEXCEPTION("Append: Realloc KillRing Top Failed", 0);
	ED_KillRing.TopP = MemP;
	ED_KillRing.TopLen = Len;
	break;
    }

    memcpy(MemP + KEP->Pos + KEP->Len, DataP, DataLen);
    KEP->Len += DataLen;
}

// ******************************************************************************
// ED_KillRingPrependTop is just like ED_KillRingAppendTop, only the kill function
// that was used deletes in the reverse/backward direction.  Therefore, the killed
// data should go BEFORE the existing data in Top.

void	ED_KillRingPrependTop(char * DataP, Int32 DataLen)
{
    ED_KEPointer	KEP = ED_KillRing.EltArr + ED_KillRing.TopI;
    char *		MemP = ED_KillRing.TopP;
    Int32		Len;

    // Enough room?  Expand first, then slide contents down
    if (KEP->Pos < DataLen) {
	if (DataLen + KEP->Len > ED_KillRing.TopLen) {
	    Len = DataLen + KEP->Len + ED_KRTOPEXTRA;
	    MemP = realloc(MemP, Len);
	    if (! MemP) G_SETEXCEPTION("Prepend: Realloc KillRing Top Failed", 0);
	    ED_KillRing.TopP = MemP;
	    ED_KillRing.TopLen = Len;
	}

	// Slide data all the way to bottom... likely to do another Prepend later!
	Len = ED_KillRing.TopLen;
	memmove(MemP + Len - KEP->Len, MemP + KEP->Pos, KEP->Len);
	KEP->Pos = Len - KEP->Len;
    }

    KEP->Pos -= DataLen;
    memcpy(MemP + KEP->Pos, DataP, DataLen);
    KEP->Len += DataLen;
}

// ******************************************************************************
// ED_KillRingAdvanceTop is called prior to pushing new data onto Top.

void	ED_KillRingAdvanceTop(void)
{
    ED_KEPointer	TopKEP, LastKEP;
    Int32		Len;
    Int16		LastI;
    char		*MemP;

    TopKEP = ED_KillRing.EltArr + ED_KillRing.TopI;
    
    ED_KillRing.YankI = ED_KillRing.TopI;		// Must Reset
    if (TopKEP->Len == 0) return;			// Nothing to stash

    LastI = (ED_KillRing.TopI + 1) & ED_KILLRINGMASK;
    LastKEP = ED_KillRing.EltArr + LastI;		// Get the last KEP, 
    if (LastKEP->Len) {					// Zap its data, if any!
	ED_KillRing.RestDataLen -= LastKEP->Len;	// Will be at BOTTOM of RestP
	LastKEP->Pos = LastKEP->Len = 0;		// Clean out KEP record
    }

    MemP = ED_KillRing.RestP;
    Len = ED_KillRing.RestDataLen + TopKEP->Len;	// Total data len for RestP
    
    // Now there is a gap at the end of RestP.  Enough for TopP or Expand!
    if (ED_KillRing.RestLen < Len) {
	Len += ED_KRRESTEXTRA;
	MemP = realloc(MemP, Len);
	if (! MemP) G_SETEXCEPTION("Realloc KillRing Rest Failed", 0);
	ED_KillRing.RestP = MemP;
	ED_KillRing.RestLen = Len;
    }

    memmove(MemP + TopKEP->Len, MemP, ED_KillRing.RestDataLen);	// Slide RestP down just enough
    memcpy(MemP, ED_KillRing.TopP + TopKEP->Pos, TopKEP->Len);	// Copy TopP to the top of RestP
    ED_KillRing.RestDataLen += TopKEP->Len;		// RestP gets the former Top data
    TopKEP->Pos = 0;					// At very top of RestP
    TopKEP->Flags &= ~ ED_KETOPFLAG;			// First of the Rest, no longer Top.

    ED_KillRing.TopI = LastI;				// New Top!
    LastKEP->Flags = ED_KETOPFLAG;
    LastKEP->Pos = LastKEP->Len = 0;			// Top is Empty!

    // Since we wrote the top of RestP, all other Pos offsets are displaced
    // down... skip the new TopKEP and the old LastKEP.

    LastKEP = ED_KillRing.EltArr;			// Recycle LastI/TopKEP vars
    for (LastI = 0; LastI < ED_KILLRINGCOUNT; LastI++, LastKEP++)
	if (LastKEP->Len && (LastKEP != TopKEP))	// Old TopKEP is current TOP of RestP
	    LastKEP->Pos += TopKEP->Len;
}

// ******************************************************************************
// ED_KillRingWriteTop is called to store recently killed chars in the KillRing.
//
// NOTE:	*MUST* call StashTop before calling this function.

void	ED_KillRingWriteTop(char * DataP, Int32 DataLen)
{
    ED_KEPointer	KEP = ED_KillRing.EltArr + ED_KillRing.TopI;
    char *		MemP = ED_KillRing.TopP;
    Int32		Len = ED_KillRing.TopLen;

    ED_KillRing.YankI = ED_KillRing.TopI;
    if (Len < DataLen) {				// Enough room?
	Len = DataLen + ED_KRTOPEXTRA;			// Expand!
	MemP = realloc(MemP, Len);			
	if (! MemP) G_SETEXCEPTION("NewTop: Realloc KillRing Top Failed", 0);
	ED_KillRing.TopP = MemP;
	ED_KillRing.TopLen = Len;
    }

    memcpy(MemP, DataP, DataLen);
    KEP->Pos = 0;
    KEP->Len = DataLen;
}

// ******************************************************************************
// ED_KillRingAdd places DataLen bytes of DataP on the Top KR buffer.  If the last
// command was also a Kill, (including QR commands) then it will Append or Prepend
// existing TopP, depending on Forward/Reverse direction.  In this way multiple
// sequential kills will be grouped as 1 big mass homicide--emacs lingo!!

void	ED_KillRingAdd(char * DataP, Int32 DataLen, Int16 OpForward)
{
    if ((ED_QRPaneP && ED_QRLastCmd == ED_QRKillCmd) ||
	(ED_CmdLastId == ED_KillId)) {
	if (OpForward)
	    ED_KillRingAppendTop(DataP, DataLen);
	else
	    ED_KillRingPrependTop(DataP, DataLen);
    } else {
	ED_KillRingAdvanceTop();		// Store in Rest
	ED_KillRingWriteTop(DataP, DataLen);
    }
    ED_XSelSetClip();        
}

// ******************************************************************************
// ED_KillRingYank recovers the LAST kill by setting *PP to and *LenP to indicate
// the Char * and Length respectively.  If PopCount is non-zero, it will pop
// the indicated number of times--can be Pos or Neg direction.  Given the
// conceptually circular KillRing, if LoopForData (usually 1) it will keep going
// around until it finds a nonzero entry--really only an issue when first starting
// up, as the KR soon fills up with entries.

void	ED_KillRingYank(Int32 PopCount, char **PP, Int32 *LenP, Int16 LoopForData)
{
    ED_KEPointer	KEP;
    Int16		Count, Inc;

    // Set YankI according to PopCount, pos or neg direction.
    ED_KillRing.YankI = (ED_KillRing.YankI - PopCount) & ED_KILLRINGMASK;
    KEP = ED_KillRing.EltArr + ED_KillRing.YankI;

    // Keep going in the indicated dir until wrap-around or find KEP with data.
    if (LoopForData) {
	Count = 1;
	Inc = (PopCount < 0) ? -1 : 1;
	while ((Count < ED_KILLRINGCOUNT) && (KEP->Len == 0)) {
	    Count += 1;
	    ED_KillRing.YankI = (ED_KillRing.YankI - Inc) & ED_KILLRINGMASK;
	    KEP = ED_KillRing.EltArr + ED_KillRing.YankI;
	}
    }

    // Get from TopP or RestP, depending on KEP.
    if (ED_KillRing.YankI == ED_KillRing.TopI)
	*PP = ED_KillRing.TopP + KEP->Pos;
    else
	*PP = ED_KillRing.RestP + KEP->Pos;

    *LenP = KEP->Len;
}

// ******************************************************************************
// ******************************************************************************
// ED_FrameHandleEvent dispatched from WEReg in Main event loop.
//
// NOTE:	Under Ubuntu 16.4 "Alt/Meta" key will cause TWO FocusOut events
//		sent to the Frame, followed by ONE FocusIn.  Must Stash SelPaneP
//		so selection does not get cancelled for any M-* commands.
//
//		*LATER* added "fix" to ignore the FocusOut/In messages.
//		As they were creating too much work (and aborting QREP/IS).
//		Look for "????".
//
// NOTE:	PU (PopUp) Window will have strange background IFF the Frame window
//		does not have Focus!!  At one point, a simple ED_FrameHasPUCmd
//		var was used to tell the main loop there was a PU-selected command
//		to execute... this was serviced when XWin got FocusIn.  So now
//		there is an explicit client message (which does NOT interfer with
//		FocusIn).

void	ED_FrameHandleEvent(XEvent * EventP, ED_FramePointer FP)
{
    if (XFilterEvent(EventP, FP->XWin))
	return;

    switch (EventP->type) {    
	case Expose:
	    if ((FP->Flags & ED_FRAMENOWINFLAG) ||
		(EventP->xexpose.count != 0))		// Not the last
		break;

	    if (FP->Flags & ED_FRAMEINITFILTERFLAG) {	// Needs filtering!
		FP->Flags &= ~ ED_FRAMEINITFILTERFLAG;

		// Ubuntu bug ????
		// A new window gets 2 expose messages--one for all the window, the second for the bottom
		// 38 pixels.  Worse, the bottom 38 pixels cannot be drawn to UNLESS there is a little
		// (10 msec on my machine) sleep here (obviously a race condition)!
		usleep(10000);
		ED_BufferDoFilter(FP->FirstPaneP->BufP, EDCB_FilterEchoUpdate, FP->FirstPaneP);
	    }

	    ED_FrameDrawAll(FP);			// Resets blinker
	    break;

        case ConfigureNotify:
	    if (FP->Flags & ED_FRAMENOWINFLAG) break;
	    if (FP->WinWidth != EventP->xconfigure.width || FP->WinHeight != EventP->xconfigure.height) {
		FP->WinWidth = EventP->xconfigure.width;
		FP->WinHeight = EventP->xconfigure.height;
		ED_FrameSetSize(FP, 1);
	    }
	    sc_BlinkTimerReset();
	    break;

        case ButtonPress:
	    if (FP->Flags & ED_FRAMENOWINFLAG) break;
	    if (! FP->Flags & ED_FRAMEFOCUSFLAG) break;
	    ED_FrameHandleClick(FP, EventP);
	    break;

	case ButtonRelease:
	    if (FP->Flags & ED_FRAMENOWINFLAG) break;
	    if (! FP->Flags & ED_FRAMEFOCUSFLAG) break;
	    ED_FrameHandleRelease(FP, EventP);
	    sc_BlinkTimerReset();
	    break;

	case KeyPress:
	    if (FP->Flags & ED_FRAMENOWINFLAG) break;
	    {
		Int16		Count;
		char		KeyBuf[32];
		KeySym		KS;
		Status		XStat = 0;
		
		Count = Xutf8LookupString(FP->XICP, &EventP->xkey, KeyBuf, 31, &KS, &XStat);
		if (XStat == XBufferOverflow) G_SETEXCEPTION("Utf8 LookupSring Buffer Overflow", Count);
		// Count reflects bytes in buffer, not multi-byte chars.
		if (Count > -1) {
		    KeyBuf[Count] = 0;		// Xutf8LookupString does not Null terminate
		    ED_FrameHandleKey(FP, &KS, EventP->xkey.state, Count, KeyBuf);
		}
	    }

	    // ED_FrameHandleKey *CAN* kill the Frame, thus NO WIN flag !!
	    if (! (FP->Flags & ED_FRAMENOWINFLAG))
		ED_FrameDrawBlinker(FP);
		
	    sc_BlinkTimerReset();
	    break;

	case FocusIn:
	    if (FP->Flags & ED_FRAMENOWINFLAG) break;

	    // Ubuntu does 2 FocusOut and 1 FocusIn(Mode==NotifyUngrab) for each Alt key!!
	    // But check whether this "fix" causes problems for other system. ????
	    if (EventP->xfocus.mode == NotifyUngrab) break;
	    
	    ED_CurFrameP = FP;				// This frame has focus
	    FP->Flags |= ED_FRAMEFOCUSFLAG;
	    if (ED_LastSelPaneP == FP->CurPaneP) {
		ED_SelPaneP = ED_LastSelPaneP;
		ED_PaneDrawText(ED_SelPaneP);
	    }
	    ED_LastSelPaneP = NULL;			// Reset it
	    
	    ED_PaneDrawModeLine(FP->CurPaneP);
	    ED_FrameDrawBlinker(FP);
	    ED_FrameDrawScrollBar(FP);
	    sc_BlinkTimerReset();
	    XSetICFocus(FP->XICP);
	    XSync(ED_XDP, False);			// Necessary if PU creates another PU window!
	    break;

	case FocusOut:
	    if (FP->Flags & ED_FRAMENOWINFLAG) break;		// Definitely needed
	    if (! (FP->Flags & ED_FRAMEFOCUSFLAG)) break;	// Already out!

	    // Ubuntu does 2 focusOut (NotifyGrab and NotifyUngrab) for each Alt key!!
	    // But check whether this "fix" causes problems for other system. ????
	    if ((EventP->xfocus.mode == NotifyGrab) ||
		(EventP->xfocus.mode == NotifyUngrab))
		break;

	    {   Int16	DidAbort = 0;

		// QREP involves IS, so always check IS after QREP!
		if (ED_QRPaneP) ED_FrameQRAbortOut(ED_STR_EchoAbort), DidAbort = 1;
		if (ED_QREPPaneP) ED_QREPAbortOut(ED_STR_EchoAbort), DidAbort = 1;
		if (ED_ISPaneP) ED_ISAbortOut(ED_STR_EchoAbort), DidAbort = 1;

		if (! DidAbort) {
		    ED_FrameEchoLen = 0;
		    ED_FrameDrawEchoLine(FP);
		}
	    }

	    FP->Flags &= ~ED_FRAMEFOCUSFLAG;
	    ED_LastSelPaneP = ED_SelPaneP;		// Stash in LastSelPaneP
	    if (ED_SelPaneP) {				// Can only be FP->CurPaneP
		ED_SelPaneP = NULL;			// Deselect
		ED_PaneDrawText(FP->CurPaneP);		// Update to erase Sel hilite
	    }
	    ED_PaneDrawModeLine(FP->CurPaneP);
	    ED_FrameDrawBlinker(FP);
	    ED_FrameDrawScrollBar(FP);
	    sc_BlinkTimerReset();
	    XUnsetICFocus(FP->XICP);
	    XSync(ED_XDP, False);
	    break;

	case ClientMessage:				
	    if (EventP->xclient.data.l[0] == ED_XWMDelAtom) {		// Close box in window title!!
	    
		// QREP involves IS, so always check IS after QREP!
		if (ED_QRPaneP) ED_FrameQRAbortOut(ED_STR_EchoAbort);
		if (ED_QREPPaneP) ED_QREPAbortOut(ED_STR_EchoAbort);
		if (ED_ISPaneP) ED_ISAbortOut(ED_STR_EchoAbort);
	    
		ED_FrameKillAsk(FP);
	    }else if (EventP->xclient.data.l[0] == ED_PUExecAtom) {	// Exec PU command

		// Defer executing cmd until Frame has focus!  Especially important if
		// the PU cmd will launch ANOTHER PU window.
		if (! (FP->Flags & ED_FRAMEFOCUSFLAG)) {
		    struct pollfd	PollR;
		    
		    // Wait a little for Frame focus--otherwise will XSendEvent MANY times.
		    PollR.fd = ConnectionNumber(ED_XDP);
		    PollR.events = POLLIN;
		    poll(&PollR, 1, 25);
		    XFlush(ED_XDP);
		    XSendEvent(ED_XDP, EventP->xclient.window, 0, 0, EventP);
		} else
		    ED_FrameExecPUCmd();
	    }
	    break;
	    
	case DestroyNotify:
	    ED_FrameWinDestroyed(FP);
	    
	    if (ED_FirstFrameP == NULL)			// No more windows
		ED_EditorKill();
	    break;
	    
	default:
	    break;

    }
}

// ******************************************************************************
// ED_FrameHandleClick is called when the mouse button is pressed in the Frame
// window.  In the case of Button1 it dispatches to PaneHandleClick, but if
// the mouse wheel (Button4 and Button5) were pressed, it will dispatch to
// PaneScrollWinEventHandler instead.

#define	ED_EVENTGETROW(FP, YArg)		((YArg - FP->FrameY) / ED_Row)
#define	ED_EVENTGETCOL(FP, XArg)		(((XArg + ED_Advance - 2) - (FP->FrameX + ED_FRAMELMARGIN)) / ED_Advance)
#define ED_EVENTISDOUBLE(R, C, B, T)		((B == Button1) && (R == ED_LastClickRow) && (C == ED_LastClickCol) &&	 \
						 (T < ED_LastClickTime + ED_DOUBLECLICKINTERVAL))
#define ED_INRANGE(A, B, R)			(((B - R) <= A) && (A <= (B + R)))

Int32	ED_LastClickRow, ED_LastClickCol;
Int16	ED_LastClickCount = 0;
Time	ED_LastClickTime = 0;

void	ED_FrameHandleClick(ED_FramePointer FrameP, XEvent * EventP)
{
    Int16	DidAbort = 0;
    Int32	X = EventP->xbutton.x;
    Int32	Y = EventP->xbutton.y;

    // Turnoff Query Replace (QREP) and Incremental Search (IS) regardless of which Frame or Pane.
    if (ED_QREPPaneP) ED_QREPAbortOut(ED_STR_EchoAbort), DidAbort = 1;
    if (ED_ISPaneP) ED_ISAbortOut(ED_STR_EchoAbort), DidAbort = 1;

    if (((FrameP->FrameX < X) && (X < FrameP->FrameX + FrameP->FrameWidth)) &&
	((FrameP->FrameY < Y) && (Y < FrameP->FrameY + FrameP->FrameHeight))) {

	ED_PanePointer	PaneP = FrameP->FirstPaneP;
	Int32		Row =	ED_EVENTGETROW(FrameP, Y);
	Int32		Col =	ED_EVENTGETCOL(FrameP, X);
	Int32		Button = EventP->xbutton.button;
	Int16		Shift = (EventP->xbutton.state & ShiftMask);

	if ((Button == Button1) &&
	    (EventP->xbutton.time < ED_LastClickTime + ED_DOUBLECLICKINTERVAL) &&
	    (ED_INRANGE(Row, ED_LastClickRow, 2)) &&
	    (ED_INRANGE(Col, ED_LastClickCol, 2))) {

	    ED_LastClickCount += 1;
	    ED_LastClickTime += ED_EXTRACLICKINTERVAL;
	} else {
	    ED_LastClickRow = Row;
	    ED_LastClickCol = Col;
	    ED_LastClickTime = EventP->xbutton.time;
	    ED_LastClickCount = 1;
	}
	
	while (PaneP) {
	    if ((PaneP->TopRow + PaneP->RowCount) > Row) {
		if (Button == Button1) {
		    ED_PaneHandleClick(PaneP, Row - PaneP->TopRow, Col, Shift, ED_LastClickCount);
		} else if (Button == Button4 || Button == Button5)
		    ED_PaneScrollWinHandleEvent(EventP, PaneP);

		break;
	    }
	    PaneP = PaneP->NextPaneP;
	}
	
	ED_FrameDrawBlinker(FrameP);
	sc_BlinkTimerReset();
    }

    // AbortOut functions will draw the EchoLine on their own...
    if (! DidAbort) {
	ED_FrameEchoLen = 0;
	ED_FrameDrawEchoLine(FrameP);
   }
}

void	ED_FrameHandleRelease(ED_FramePointer FrameP, XEvent * EventP)
{
    if (EventP->xbutton.button == Button1)
	ED_PaneHandleRelease(FrameP->CurPaneP);
    else if (EventP->xbutton.button == Button2)
	ED_PaneInsertPrimary(FrameP->CurPaneP);
    else
	return;
}



// ******************************************************************************
// ED_FrameHandleKey is called when a key is typed and FrameP has focus.
//
// Mods indicate Ctrl (ControlMask 0x0004) and Alt (Mod1Mask 0x0008).
// Alt-a is Alt + a=61 Str[0x61]
// Ctl-a is Ctl + a=61 Str[0x01]
//
// If Ctl/Meta/Alt/Super/Hyper -> Just look at KeySym, this is a command
//
// Sym 0000 - 001f	>> IGNORE IGNORE
// Sym 0020 - 007e	>> 0x20 - 0x7e, Insert
// Sym 007f - 00ff	>> Ignore Sym, Insert StrP
// Sym 01**		>> Ignore Sym, Insert StrP
// Sym 02**		>> Ignore Sym, Insert StrP
// ....
// Sym fc**		>> Ignore Sym, Insert StrP
// --------		------------------------------------------
// Sym fd**		IGNORE IGNORE, IBM terminal keys
// Sym fe**		IGNORE IGNORE, Extended keys and modifiers
// Sym ffcf - 0xffef	IGNORE IGNORE Aux Function and Mod Codes
// --------		------------------------------------------
// Sym ff00 - 0xff4f	Bind & Exec, not for insertion
// Sym ff50 - 0xffce	>> 0x80 - 0xfe Map and Insert
// --------		------------------------------------------
//
// KeySym	Function Binding KeyStr
//
// ff01 - ff14	>> 0x01 - 0x14
// Ext(*)	>> 0x15
// Ctl		>> 0x16			ControlMask
// Alt		>> 0x17			Mod1Mask  (Meta in Emacs)
// Sup		>> 0x18			Mod4Mask
// Hyp		>> 0x19
// Digit(+)	>> 0x1a			Wants digits!
// ff1b		>> 0x1b	ESC
// ffff		>> 0x7f DEL
//
// Sym ffcf - ffe0	IGNORE Sym, Aux Function codes
// Sym ffe1 - ffef	IGNORE Sym, Modifier codes
//
// (*)	Ext is a special Key to denote special commands, delete, F1, UpArrow, etc. in KeyStr
// (+)	Digit is a special escape Key to denote acceptance of numeric digits.
//
//	XK_*** Keypad SymChars are simply mapped to their equivalent regulr XK chars:
//
//	Keypad			Regular
//	------			------
//	ff80		-->	0020			Space
//	ff89		-->	ff09			Tab
//	ff8d		-->	ff0d			Enter

//	ff91 - ff94	-->	ffbe - ffc1		F1 - F4		+ x2d
//	ff95 - ff9d	-->	ff50 - ff58		Home - Begin	- x45
//	ff9e		-->	ff63			Insert
//	ff9f		-->	ffff			Delete

//	ffbd		-->	003d			Equal		- x80
//	ffaa		-->	002a			mult		- x80
//	ffab		-->	002b			add		- x80
//	ffac		-->	002c			separator/comma - x80
//	ffad		-->	002d			subtract	- x80
//	ffae		-->	002e			Decimal point	- x80
//	ffaf		-->	002f			Divide		- x80
//	ffb0 - ffb9	-->	0030 - 0039		0 - 9		- x80


void	ED_FrameHandleKey(ED_FramePointer FrameP, KeySym * SymP, Uns32 Mods, Int16 Count, char * StrP)
{
    Uns16		SymChar;
    
    SymChar = (Uns16)*SymP;

#ifdef DEBUG
    if (0) {
	Int16 I;
	printf("Key %X+%X ->  [%d:%s] = <", SymChar, Mods, Count, StrP);
	I = 0;
	while (1) {
	    printf("%x", StrP[I]);
	    if (I++ < Count) printf(" ");
	    else break;
	}
	printf(">\n");
    }
#endif

    // Do KeyPad mapping first... a 128 byte "ff" map array would go *MUCH* faster....
    // Especially if it dispatched what Command/Insert functions to call!
    if ((0xffaa <= SymChar) && (SymChar <= 0xffbd))
	SymChar = (0x00ff & SymChar) - 0x0080;
    else if ((0xff95 <= SymChar) && (SymChar <= 0xff9d))
	SymChar -= 0x0045;
    else if ((0xff91 <= SymChar) && (SymChar <= 0xff94))
	SymChar += 0x002d;
    else if (SymChar == 0xff80) SymChar = 0x0020;		// Space
    else if (SymChar == 0xff89) SymChar = 0xff09;		// Tab
    else if (SymChar == 0xff8d) SymChar = 0xff0d;		// Enter
    else if (SymChar == 0xff9e) SymChar = 0xff63;		// Insert
    else if (SymChar == 0xff9f) SymChar = 0xffff;		// Delete

   // We are in FrameQR mode, handle incoming chars! (+ Enter and Delete)
    if (ED_QRPaneP) {
	if (((0x0020 <= SymChar) && (SymChar <= 0xfcff)) ||	// Text chars
	    ((0xff50 <= SymChar) && (SymChar <= 0xff58)) ||	// Arrows + Home keys
	    ((0xff08 <= SymChar) && (SymChar <= 0xff0d)) ||	// Backspace... Enter
	    (SymChar == 0xffff))				// Delete

	    ED_FrameQRHandleChars(FrameP, SymChar, Mods, Count, StrP);
	return;
    }

    // SymChar in these ranges should be completely ignored, never happened!
    if ((SymChar <= 0x001f) ||
	((0xfd00 <= SymChar) && (SymChar <= 0xfeff)) ||
    	((0xffcf <= SymChar) && (SymChar <= 0xffef)))
	return;    

    // QREP (Query Replace) takes over search... otherwise, if in ISearch, let it
    // handle the chars.

    if (ED_QREPPaneP) {
	if (ED_QREPHandleChars(FrameP, SymChar, Mods))
	    return;
    } else if (ED_ISPaneP)
	if (ED_ISHandleChars(FrameP, SymChar, Mods, Count, StrP))
	    return;

    if ((0xff50 <= SymChar) && (SymChar <= 0xffce)) {
	ED_FrameCommandMatch(FrameP, SymChar, Mods);		// Nothing to insert!

    } else if (((0x0020 <= SymChar) && (SymChar <= 0x007e)) ||
		(SymChar == 0xff0d)) {				// SymChar Insert, do InCommand
	if (ED_CmdLen || (Mods & ED_COMMANDMODSMASK)) {
	    if (ED_FrameCommandMatch(FrameP, SymChar, Mods))
		ED_PaneInsertChars(FrameP->CurPaneP, Count, StrP, 1);
	} else
	    ED_PaneInsertChars(FrameP->CurPaneP, Count, StrP, 1);

    } else if ((0x007f <= SymChar) && (SymChar <= 0xfcff)) {	// StrP Insert, Ignore InCommand
	if  (ED_FrameCommandMatchAbort(FrameP, SymChar))
	    ED_PaneInsertChars(FrameP->CurPaneP, Count, StrP, 1);

    } else if (((0xff00 <= SymChar) && (SymChar <= 0xff4f)) ||	// Do/initiate InCommand
	       (0xffff == SymChar))
	ED_FrameCommandMatch(FrameP, SymChar, Mods);		// Nothing to insert!

    return;
}

// ******************************************************************************
// ED_FrameCommandEcho *MAY* write the command KeyStr on the Frame's EchoLine,
// depending on the FrameCommandEchoDelay.
//
// If "Waited" is set, then decrement FrameCommandEchoDelay.
// Write the command *JUST ONCE* when the FrameCommandEchoDelay becomes 0.
//
// NOTE:	This function is called from the BlinkHandler.  DOES NOT
//		writing the same Msg every Blink cycle.  Just once when
//		FrameCommandEchoDelay transitions to 0.
//
// The function is called from ED_FrameCommandMatch (Waited == 0) as the
// user types more of the command.  IFF delay is already 0, then just
// write these out as the user types them.

void	ED_FrameCommandEcho(ED_FramePointer FrameP, Int16 Waited)
{
    if (ED_CmdLen == 0) return;			// Nothing to echo

    if (Waited) {				// Logic is serial, to catch 0-transit
	if (! ED_CmdEchoDelay) return;		// Don't repeat
	if (--ED_CmdEchoDelay) return;		// Decrement, but DO not return on 0-transit
    } else if (ED_CmdEchoDelay) return;		// Wait more
    
    ED_FrameEchoLen = ED_KeyStrPrint(ED_CmdStr, ED_FrameEchoS);
    ED_FrameDrawEchoLine(FrameP);
}

// ******************************************************************************
// ED_FrameCommandBadEcho is called when
// 0) command does not match,
// 1) strange character (never in a command sequence) aborts the match, or
// 2) Command has too many digits in numeric arg

void	ED_FrameCommandBadEcho(ED_FramePointer FrameP, Uns16 SymChar, Int16 Error)
{
    char	CommandStr[ED_MSGSTRLEN];
    Uns32	KS = 0x0000FFFF & SymChar;
    char *	XStrP;

    ED_KeyStrPrint(ED_CmdStr, CommandStr);	// Already has trailing <space>
    switch (Error) {
	case 0:
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoCmdMatchFail, CommandStr);
	    break;

	case 1:
	    XStrP = XKeysymToString(KS);
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoCmdUndef, CommandStr, XStrP);
	    break;

	case 2:
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoCmdBadNum, CommandStr);
	    break;

        default:
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoCmdUndef, CommandStr, "");
    }

    ED_FrameDrawEchoLine(FrameP);
}

// ******************************************************************************
// ED_FrameCommandMatchAbort is called when the ongoing command match is being
// aborted.  The resulting bad match is shown in the ModeLine.
//
// If there *WAS NO* match, the function return 1 to signal SymChar should be
// inserted into buffer.

Int16	ED_FrameCommandMatchAbort(ED_FramePointer FrameP, Uns16 SymChar)
{
    if (ED_CmdLen) {
	ED_FrameCommandBadEcho(FrameP, SymChar, 1);
        ED_CmdLen = 0;
	return 0;			// SymChar was used to Abort
    }

    return 1;				// SymChar was not used, process it
}

// ******************************************************************************
// ED_FrameCommandMatch is called to initiate a new command match or *CONTINUE*
// an on-going partial match--as new command chars are typed in.
//
// When initiating, ED_CmdBindP is set to the first binding record.
// In a partial match, ED_CmdBindP limits the number of bindings to be
// searched.
//
// Return 1 means SymChar should be inserted... check on ED_CmdMult !
// Return 0 means did my thing, success or fail
//
//			C-n	C-u C-n		C-u 9 9 C-n
// CommandMult		1	4		99
// CommandMultCmd	0	1		1
// CommandMultNumb	0	0		1
// CommandMultDigit	0	0		2

Int16	ED_FrameCommandMatch(ED_FramePointer FrameP, Uns16 SymChar, Uns32 Mods)
{
    Int16	Res;
    char	C;
    Int16	NewI;

    if (ED_CmdLen == 0) {
	ED_CmdBindP = ED_FirstBindP;
	ED_CmdEchoDelay = 2;				// 2 blinker cycles
	ED_FrameEchoLen = 0;				// Clear out Echo line
	ED_CmdMult = 1;					// Default
	ED_CmdMultCmd = 0;
	ED_CmdMultNeg = 0;
	ED_CmdMultDigit = 0;
	ED_CmdMultIn = 0;
	ED_CmdMultLen = 0;
    }

    NewI = ED_CmdLen;					// Everything after was just added!

    if (Mods & ControlMask) ED_CmdStr[ED_CmdLen++] = ED_CTLCOMMANDCODE;
    if (Mods & Mod1Mask) ED_CmdStr[ED_CmdLen++] = ED_ALTCOMMANDCODE;
    if (Mods & Mod4Mask) ED_CmdStr[ED_CmdLen++] = ED_SUPCOMMANDCODE;

    if (SymChar == 0xff0d)				// CR (will be turned to LF eventually!)
	C = 0x0d;
    else if (SymChar & 0xff00) {			// Ext code
	ED_CmdStr[ED_CmdLen++] = ED_EXTCOMMANDCODE;
	C = (char)(SymChar & 0x00ff);
    } else {						// Regular char
	C = (char)SymChar;
	if (('A' <= C) && (C <= 'Z')) C += ('a' - 'A');	// Force lowercase
    }

    ED_CmdStr[ED_CmdLen++] = C;
    ED_CmdStr[ED_CmdLen] = 0;

    // Entering CommandMult?
    if (NewI == 0) {
	// Have Command U ?
	if ((ED_CmdStr[0] == ED_CTLCOMMANDCODE) && (ED_CmdStr[1] == 'u')) {
	    ED_CmdMult = 0;
	    ED_CmdMultCmd = 1;
	    ED_CmdMultIn = 1;
	} else if (ED_CmdStr[0] == ED_ALTCOMMANDCODE) {
	    // Have Meta + Minus sign?
	    if (ED_CmdStr[1] == '-') {
		ED_CmdMult = 0;
		ED_CmdMultCmd = 1;
		ED_CmdMultNeg = 1;
		ED_CmdMultIn = 1;

	    // Have Meta + Digit?
	    } else if (('0' <= ED_CmdStr[1]) && (ED_CmdStr[1] <= '9')) {
		ED_CmdMult = ED_CmdStr[1] - '0';
		ED_CmdMultCmd = 1;
		ED_CmdMultDigit = 1;
		ED_CmdMultIn = 1;
	    }
	}
    }

    // Already in CommandMult!
    if (ED_CmdMultIn && (NewI > 0)) {

	if (ED_CmdStr[NewI] == '-') {				// Minus sign
	    if ((ED_CmdMultDigit == 0) && (! ED_CmdMultNeg))
		ED_CmdMultNeg = 1;
	    else {
		// The minus sign is just a Char, insert it!
		ED_FrameCommandEcho(FrameP, 0);
		ED_CmdLen = 0;
		return 1;
	    }

	} else if (('0' <= C) && (C <= '9')) {			// Digit
	    // Overwrite any Alt/Cmd modifiers here!
	    ED_CmdStr[NewI] = C;
	    ED_CmdStr[NewI + 1] = 0;
	    ED_CmdLen = NewI + 1;
	    
	    ED_CmdMultDigit += 1;
	    ED_CmdMult = (ED_CmdMult * 10) + (C - '0');
	    if (ED_CmdMultDigit > 5) {
		ED_FrameCommandBadEcho(FrameP, 0, 2);
		ED_CmdLen = 0;
		return 0;
	    }
	} else {						// Done with numbers!
	    ED_CmdMultIn = 0;

	    if (ED_CmdMultDigit == 0) {				// No number!
		if (ED_CmdMultNeg) {				// Negative zero?
		    ED_CmdMult = 1;				// Make it Neg 1
		} else
		    ED_CmdMult = 4;				// Default value for C-u with no digits
	    }

	    if ((ED_HYPCOMMANDCODE < ED_CmdStr[NewI]) || (ED_CmdStr[NewI] < ED_EXTCOMMANDCODE)) {
		// New char is for insertion, not a command!
		ED_FrameCommandEcho(FrameP, 0);
		ED_CmdLen = 0;
		return 1;
	    }
	}
    }

    if (ED_CmdMultIn) {
	ED_FrameCommandEcho(FrameP, 0);
	ED_CmdMultLen = ED_CmdLen;				// Still in Numeric args
	return 0;						// Keep going with Numb args!
    }

    // So handle next command!
    Res = ED_FRegFindBinding(&ED_CmdBindP, ED_CmdStr + ED_CmdMultLen);
    if (Res == 1) {
	// Partial Match... Update in EchoLine.
	ED_FrameCommandEcho(FrameP, 0);
	return 0;
    }

    if (Res < 0) {
	// Failed Match... Update in EchoLine.
	ED_FrameCommandBadEcho(FrameP, 0, 0);
	ED_CmdLen = 0;
	return 0;
    }

    // Found a perfect match.  Execute!  Echo the command in case
    // it had a numeric arg... but then reset in case command
    // itself has msg to echo.
    ED_FrameCommandEcho(FrameP, 0);
    ED_CmdLen = 0;
    ED_CmdShift = (Mods & (ShiftMask | LockMask));
    ED_CmdThisId = (Uns64)ED_CmdBindP->FRegP->FuncP;
	(*ED_CmdBindP->FRegP->FuncP)(FrameP->CurPaneP);
    ED_CmdLastId = ED_CmdThisId;

    // Reset the count for everyone EXCEPT M-X... it has not
    // executed the target command yet, needs the CmdMult for it.
    if (ED_CmdLastId != ED_ExecId) {
	ED_CmdMult = 1;
	ED_CmdMultNeg = ED_CmdMultCmd = 0;
    }
    return 0;
}

// ******************************************************************************
// ED_FrameDrawBlinker draws the blinker for all the Panes in the Frame

void	ED_FrameDrawBlinker(ED_FramePointer FrameP)
{
    ED_PanePointer	PaneP;

    PaneP = FrameP->FirstPaneP;
    while (PaneP) {
	ED_PaneDrawBlinker(PaneP);
	PaneP = PaneP->NextPaneP;
    }

    if (ED_QRPaneP && ED_QRPaneP->FrameP == FrameP)
	ED_FrameQRDrawBlinker(FrameP);
}

// ******************************************************************************
// ED_FrameDrawScrollBar draws the scroll bars for every Pane in the Frame

void	ED_FrameDrawScrollBar(ED_FramePointer FrameP)
{
    ED_PanePointer PaneP;

    PaneP = FrameP->FirstPaneP;
    while (PaneP) {
	ED_PaneDrawScrollBar(PaneP, 0);
	PaneP = PaneP->NextPaneP;
    }	
}

// ******************************************************************************
// ED_FrameGetWinSize returns the proper window size for the given frame.

void	ED_FrameGetWinSize(ED_FramePointer FrameP, Int32 * WidthP, Int32 * HeightP)
{
    *WidthP = ((FrameP->RowChars + 1) * ED_Advance) + ED_FRAMELMARGIN + ED_FRAMERMARGIN;
    *HeightP = FrameP->RowCount * ED_Row;
}

// ******************************************************************************
// ED_FrameWinMinSizeReset is called to reset the min size limits for the Frame
// XWin when panes are added or removed... because each pane has a min row count.

void	ED_FrameResetWinMinSize(ED_FramePointer FrameP)
{
    XSizeHints		Hints;
    ED_FrameRecord	FakeF;
    Int32		W, H;
    Int64		UserHints;

    FakeF.RowChars = ED_FRAMEMINROWCHARS;
    FakeF.RowCount = (FrameP->PaneCount * ED_PANEMINROWCOUNT) + 1;
    if (FakeF.RowCount < ED_FRAMEMINROWCOUNT)
	FakeF.RowCount = ED_FRAMEMINROWCOUNT;
	
    ED_FrameGetWinSize(&FakeF, &W, &H);

    // XSetWMNormalHints will completely over-write all hints,
    // so must get the existing hints first.

    XGetWMNormalHints(ED_XDP, FrameP->XWin, &Hints, &UserHints);
    
    Hints.min_width = W;
    Hints.min_height = H;
    Hints.flags = PMinSize | PPosition |PSize | PResizeInc;
    XSetWMNormalHints(ED_XDP, FrameP->XWin, &Hints);
}

// ******************************************************************************
// ED_FrameSetSize will set the Frame parameters (based on mono-spaced Font)
// for the WinWidth+WinHeight stashed in the Frame.  The more difficult part
// is setting the Pane sizes (since the Frame may have N) as the Frame window
// is resized, bigger or smaller.
//
// PaneP->FracRowCount is a 32.32 number to track the fractional RowCount
// when the Pane size is scaled up and down.  So the Frame can be shrunk,
// and expanded back and the Panes will have the exact same size as before.
//
// When scaling fixed-point, better to divide before multiplying, to avoid overflow.
// 1/2 is 0x80000000 in 32.32, used for rounding.
//
// Panes will not shrink below ED_PANEMINROWCOUNT rows, and this introduces
// a non-linearity in the scaling algorithm... worse this makes things
// assymetrical for expansion vs. compression.
//
// When expanding, goal is to reach a total of 't' rows... but small Panes may
// (or may not) stay at the min RowCount, so it is not clear how much to scale
// the Pane sizes.  Instead, the algorithm shoots for a giant T, and then compresses
// the panes to the desired size 't'.
//
// Compression is easier, although iterative, with Order of N^2 if N Panes.
// First, a Scale factor is found and applied based on total size.  This will
// usually not work perfectly, since some Panes will reach Min, and not
// compress.  Then a new Scale factor is found (and applied) based on how
// much the non-min size (Big) Panes have to compress.  This process is
// repeated as more Panes hit their min limit--the Scale factors simply
// compound in the FracRowCount fields.  The process will terminate
// quickly if the min-size window limits are set correctly.

#define		ED_GIANTROWCOUNT	(8 * 1024)

void	ED_FrameSetSize(ED_FramePointer FrameP, Int16 DoPanes)
{
    Int32		RowChars, RowCount, OldRowCount, RowsUsed, OldRowChars;
    Int16		HChanged;

    RowChars = (FrameP->WinWidth - (ED_FRAMELMARGIN + ED_FRAMERMARGIN)) / ED_Advance;
    RowCount = FrameP->WinHeight / ED_Row;
    OldRowCount = FrameP->RowCount;

    FrameP->FrameX = ED_FRAMELMARGIN;
    FrameP->FrameY = 0;
    FrameP->FrameWidth = RowChars * ED_Advance;
    FrameP->FrameHeight = RowCount * ED_Row;
    FrameP->RowCount = RowCount;

    RowChars -= 1;
    OldRowChars = FrameP->RowChars;
    HChanged = (OldRowChars != RowChars);
    FrameP->RowChars = RowChars;

    if (DoPanes && (RowCount != OldRowCount)) {

	ED_PanePointer	PaneP, LastPaneP;
	Int64		DivCount, MulCount;		// For scaling

	RowCount -= 1;		// Last row of Frame is EchoLine, not for Panes	
	OldRowCount -= 1;
	RowsUsed = 0;

	// Expanding, make it really huge, then compress down to size

	if (RowCount > OldRowCount) {
	    PaneP = FrameP->FirstPaneP;
	    while (PaneP) {
		PaneP->FracRowCount = (PaneP->FracRowCount / (Int32)OldRowCount) * ED_GIANTROWCOUNT;
		PaneP->RowCount = (PaneP->FracRowCount + 0x80000000LL) >> 32;
		if (PaneP->RowCount < ED_PANEMINROWCOUNT)
		    PaneP->RowCount = ED_PANEMINROWCOUNT;
		RowsUsed += PaneP->RowCount;
		PaneP = PaneP->NextPaneP;
	    }
	    
	    OldRowCount = RowsUsed;	// Now compress
	}

	// Compressing, Panes cannot go below PANEMINROWCOUNT, so compress in passes.
	// Pass 1-N:	Compress all Panes, separate MIN sized from Big Panes.
	//		Repeat the compression, since some pane might reach min.

	DivCount = (Int32)OldRowCount;
	MulCount = (Int32)RowCount;
	while (MulCount < DivCount) {
	    Int32	MinPaneRowsUsed = 0;
	    Int32	BigPaneRowsUsed = 0;

	    PaneP = FrameP->FirstPaneP;
	    while (PaneP) {
		PaneP->FracRowCount = (PaneP->FracRowCount / DivCount) * MulCount;
		PaneP->RowCount = (PaneP->FracRowCount + 0x80000000LL) >> 32;
		if (PaneP->RowCount <= ED_PANEMINROWCOUNT) {
		    PaneP->RowCount = ED_PANEMINROWCOUNT;
		    MinPaneRowsUsed += ED_PANEMINROWCOUNT;
		} else
		    BigPaneRowsUsed += PaneP->RowCount;
		
		PaneP = PaneP->NextPaneP;
	    }

	    DivCount = (Int64)BigPaneRowsUsed;
	    MulCount = (Int64)RowCount - MinPaneRowsUsed;
	}

	// Final Pass:     	Assign the Tops, Position the XModeWin for all panes.
	//			Last pane WILL NOT have XModeWin,
	    
	RowsUsed = 0;
	PaneP = FrameP->FirstPaneP;
	while (PaneP) {
	    PaneP->TopRow = RowsUsed;
	    RowsUsed += PaneP->RowCount;
	    LastPaneP = PaneP;
	    PaneP = PaneP->NextPaneP;
	}

	LastPaneP->RowCount += RowCount - RowsUsed;	// Final adjust, usually +1

    }	// DoPanes

    // Update Pane size information, even if only width changes!

    if (DoPanes) {
	ED_PanePointer	PaneP = FrameP->FirstPaneP;
	Int32		RowStartPos;

	if (HChanged) ED_FrameUpdateTextRows(FrameP, OldRowChars);
	
	while (PaneP) {
	    if (PaneP->XModeWin) ED_PanePositionModeWin(PaneP);
	    ED_PanePositionScrollBar(PaneP);
	    if (PaneP->CursorPos) {
		RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
		ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, 2, 1);	// Force SetScrollBar
	    } else
		ED_PaneSetScrollBar(PaneP);

	    if (ED_SelPaneP == PaneP)
		ED_PaneFindLoc(PaneP, ED_SelMarkPos, &ED_SelMarkRow, &ED_SelMarkCol, 0, 0);

	    PaneP = PaneP->NextPaneP;
	}

	// Clear out echo line if re-sizing... but only if not really using it.
	if (! (ED_QRPaneP || ED_QREPPaneP || ED_ISPaneP)) ED_FrameEchoLen = 0;
    }
}


// ******************************************************************************
// ED_FrameUpdateTextRows updates BufRowCount (needed for Scroll bars) for all
// Panes in the Frame.  The value is computed only once for each Pane Buffer, and
// stashed in all the Panes of the same Buffer.
//
// StartRowCount is also computed for all Panes, but these have to be done
// individually as PanePos will be different for each Pane.
//
// NOTE:	The first line(s) of a Pane can be continuations of a long
//		wrapped line.  If so, as the Pane width changes, the StartPos
//		for the Pane has to change, since the long line will wrap
//		differently!!  (Initial Wrap-FLow problem.)

void	ED_FrameUpdateTextRows(ED_FramePointer FrameP, Int32 OldRowChars)
{
    ED_PanePointer	PaneP, PP;
    Int32	        Col;

    // Clear out all BufRowCount on Panes in this Frame + compute StartRowCount
    PaneP = FrameP->FirstPaneP;
    while (PaneP) {
	ED_PaneUpdateStartPos(PaneP, OldRowChars);
	PaneP->BufRowCount = -1;
	ED_PaneFindLoc(PaneP, PaneP->PanePos, &PaneP->StartRowCount, &Col, 1, 0);
	PaneP = PaneP->NextPaneP;
    }

    // Now compute BufRowCount for all Panes on this Frame
    PaneP = FrameP->FirstPaneP;
    while (PaneP) {
	if (PaneP->BufRowCount == -1) {
	    ED_PaneFindLoc(PaneP, PaneP->BufP->LastPos, &PaneP->BufRowCount, &Col, 1, 0);

	    // Stash value in all Panes, same Frame, same Buf
	    PP = FrameP->FirstPaneP;
	    while (PP) {
		if (PP->BufP == PaneP->BufP)
		    PP->BufRowCount = PaneP->BufRowCount;

		PP = PP->NextPaneP;
	    }
	}

	PaneP = PaneP->NextPaneP;
    }
}

// ******************************************************************************
// ED_FrameNew will allocate and init a new FrameRecord, will stash in ED_FirstFrameP.
//
// NOTE:	A new Frame creates a new XWin, which will get Expose Event before
//		FocusIn Event.  So ED_FrameNew *MUST* set ED_CurFrameP explicitly.

void	ED_FrameNew(Int32 WinWidth, Int32 WinHeight, ED_BufferPointer BufP)
{
    ED_FramePointer	FrameP;

    ED_FrameN += 1;			// Increases only
    FrameP = sc_SAStoreAllocBlock(&ED_FrameStore);

    FrameP->Tag = *(Uns32 *)ED_FrameTag;
    FrameP->Flags = ED_FRAMENOFLAG;
    FrameP->FirstPaneP = NULL;
    FrameP->CurPaneP = NULL;
    FrameP->XWin = 0;
    FrameP->BlinkerGC = 0;
    FrameP->HiliteGC = 0;
    FrameP->HLBlinkerGC = 0;
    FrameP->Number = ED_FrameN;
    FrameP->PaneCount = 0;
    FrameP->WinWidth = WinWidth;
    FrameP->WinHeight = WinHeight;
    FrameP->PUWinX = FrameP->PUWinY = -1;
    
    FrameP->PrevFrameP = NULL;
    FrameP->NextFrameP = ED_FirstFrameP;
    if (ED_FirstFrameP) ED_FirstFrameP->PrevFrameP = FrameP;
    ED_FirstFrameP = FrameP;
    ED_CurFrameP = FrameP;

    ED_FrameSetSize(FrameP, 0);		// Get char-based sizes computed
    ED_XWinCreate(FrameP);		// Create the window, before Pane
    ED_PaneNew(FrameP, BufP);		// Must have at least 1 pane
}

// ******************************************************************************
// ED_FrameWinDestroyed is called because the XWin for the frame got a
// DestroyNotify event.  In response, this function kills the frame and its
// data structures--Buffers can survive.
//
// NOTE:	The FrameP->XWin has *ALREADY* been destroyed, and that
//		has already destroyed all of its sub windows, especially
//		PaneP->XModeWin and PaneP->XScrollWin.

void	ED_FrameWinDestroyed(ED_FramePointer FrameP)
{
    ED_PanePointer	ThisPP, NextPP;

    // Go through and kill the Panes first
    ThisPP = FrameP->FirstPaneP;
    while (ThisPP) {
	NextPP = ThisPP->NextPaneP;
	// Do NOT kill Info Buffers here, FrameKill has already done that!
	sc_SAStoreFreeBlock(&ED_PaneStore, ThisPP);
	ThisPP = NextPP;
    }
    
    // CurFrameP will be whichever gets focus next, set to NULL now.
    if (ED_CurFrameP == FrameP) ED_CurFrameP = NULL;

    if (ED_FirstFrameP == FrameP) ED_FirstFrameP = FrameP->NextFrameP;
    if (FrameP->PrevFrameP) FrameP->PrevFrameP->NextFrameP = FrameP->NextFrameP;
    if (FrameP->NextFrameP) FrameP->NextFrameP->PrevFrameP = FrameP->PrevFrameP;

    sc_WERegDel(FrameP->XWin);			// Un register dead window
    sc_SAStoreFreeBlock(&ED_FrameStore, FrameP);
}

// ******************************************************************************
// ED_FrameKill is called to kill a frame.  Since every Frame has an XWin, the
// function simply destroys the XWin, which will cause XLib to send a DestroyNotify
// even to the window which causes ED_FrameHandleEvent to call ED_FrameWinDestroyed.
//
// NOTE:	We cannot set FrameP->XWin to 0 here... X11 will send other events
//		that must be processed, we need the XWin for that.  But program
//		should not draw/write on it--expcially to update the blinker.  So
//		set FRAMENOWINFLAG.  (X11 calls FocusOut on destroyed windows!)
//
// NOTE:	If the Frame has multiple Panes, all but the bottom one will have
//		XModeWin InputOnly windows.  When FrameP->XWin is destroyed, these
//		sub windows will be destroyed with it!  But call ED_PaneKillModeWin
//		to clean up any other data structures.

void	ED_FrameKill(ED_FramePointer FrameP)
{
    ED_PanePointer	PaneP;

    PaneP = FrameP->FirstPaneP;
    while (PaneP) {
	PaneP->BufP->PaneRefCount -= 1;
	PaneP->BufP->CursorPos = PaneP->CursorPos;
	PaneP->BufP->PanePos = PaneP->PanePos;

	if (PaneP->BufP->Flags & ED_BUFINFOONLYFLAG)
	    ED_BufferKill(PaneP->BufP);		// Only kills if PaneRefCount is 0 !!
    
	ED_PaneKillModeWin(PaneP);		// Cleans up if it has ModeWin
	ED_PaneKillScrollWin(PaneP);
	PaneP = PaneP->NextPaneP;
    }

    XftDrawDestroy(FrameP->XftDP);		// Kill Xft Draw
    FrameP->XftDP = NULL;
    XFreeGC(ED_XDP, FrameP->BlinkerGC);		// Free the GC
    FrameP->BlinkerGC = 0;
    XFreeGC(ED_XDP, FrameP->HiliteGC);
    FrameP->HiliteGC = 0;
    XFreeGC(ED_XDP, FrameP->HLBlinkerGC);	// Free the GC
    FrameP->HLBlinkerGC = 0;
    
    XDestroyIC(FrameP->XICP);
    FrameP->XICP = NULL;
    
    XDestroyWindow(ED_XDP, FrameP->XWin);
    FrameP->Flags |= ED_FRAMENOWINFLAG;
}

// Return 0 == Good Response
// Return 1 == Bad response, try again
Int16	ED_AuxQRKillFunc(void)
{
    char		C;
    ED_FramePointer	FrameP = ED_QRPaneP->FrameP;

    if (ED_QRRespLen == 1) {
	C = ED_QRRespS[0];
	if ((C == 'n') || (C == 'N')) {
	    ED_QRPaneP = NULL;
	    return 0;
	}

	if ((C == 'y') || (C == 'Y')) {
	    ED_QRPaneP = NULL;
	    ED_FrameKill(FrameP);
	    return 0;
	}
    }

    // Bad response if we get here!
    return 1;
}

// Only ask *IFF* this is the last Frame *AND* buffers needs saving!
// Only buffers that HAVE A FILE need saving!!

void	ED_FrameKillAsk(ED_FramePointer FrameP)
{
    ED_PanePointer	PaneP = FrameP->FirstPaneP;
    Int16		NeedSave = 0;

    // Check buffers in these Panes first!
    if ((FrameP->NextFrameP == NULL) && (FrameP->PrevFrameP == NULL)) {
	while (PaneP) {
	    if ((PaneP->BufP->Flags & ED_BUFMODFLAG) &&		// Modified
		(! PaneP->BufP->Flags & ED_BUFNOFILEFLAG))	// And has a File
	    {
		NeedSave = 1;
		break;
	    }
	    PaneP = PaneP->NextPaneP;
	}
    }
    
    if (NeedSave)
	ED_FrameQRAsk(FrameP, ED_STR_QuerySaveClose, NULL, ED_QRLetterType, ED_AuxQRKillFunc, NULL);
    else
	ED_FrameKill(FrameP);
}


// ******************************************************************************
// ED_FrameDrawAll draws every Pane on the Frame, followed by the EchoLine.
// Drawing the text will erase the blinker, so it too must be drawn.
//
// Drawing the whole frame is usually the last thing and takes time...
// So reset BlinkTimer too!

void    ED_FrameDrawAll(ED_FramePointer FrameP)
{
    ED_PanePointer	PaneP;

    // Draw left and right margins first

    PaneP = FrameP->FirstPaneP;			// Draw all the panes first
    while (PaneP) {
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);
	PaneP = PaneP->NextPaneP;
    }

    ED_FrameDrawEchoLine(FrameP);
    ED_FrameDrawBlinker(FrameP);

    sc_BlinkTimerReset();
}

// ******************************************************************************
// ED_FrameDrawEchoProgressBar will draw a progress bar in the Echo area of the Frame
// LabelP is a regular string, *NOT* UTF8.
//
// NOTE:	Call initially with (Percent == -1) to clear the area.
//		LabelP must be provided for -1%, otherwise ignored.
//
// NOTE:	Recommend call to usleep after invoking this function, in order to
//		make the update/progress bar visible.  Most operations would otherwise
//		just go too fast.  If updating every 10%, 50,000 usec is good interval.
//		(This would only add 1/2 sec to total time.)

void	ED_FrameDrawEchoProgressBar(ED_FramePointer FrameP, char * LabelP, Int16 Percent)
{
    static Int32	LabelLen = 0;
    Int32		W, LineY = (FrameP->RowCount - 1) * ED_Row;

    if (Percent == -1) {				// Erase all of it
	XftDrawRect(FrameP->XftDP, &ED_XCArr[ED_White], 0, LineY, FrameP->FrameWidth + ED_Advance + ED_FRAMELMARGIN, ED_Height + 1);

	LabelLen = strlen(LabelP);
	XftDrawString8(FrameP->XftDP, &ED_XCArr[ED_Black], ED_XFP, ED_FRAMELMARGIN, LineY + ED_Ascent, (XftChar8 *)LabelP, LabelLen);

	LabelLen = ((LabelLen + 1) * ED_Advance) + ED_FRAMELMARGIN;
	XftDrawRect(FrameP->XftDP, &ED_XCArr[ED_Gray], LabelLen, LineY + 8,
		    FrameP->FrameWidth - LabelLen, 4);
    } else {
	W = ((Int32)Percent * (FrameP->FrameWidth - LabelLen)) / 100;
	XftDrawRect(FrameP->XftDP, &ED_XCArr[ED_SelOrange], LabelLen, LineY + 8, W, 4);
    }
    
    // Synch/Flush here, caller is usually in a tight loop that will not give any time to XLib.
    XSync(ED_XDP, False);
}

// ******************************************************************************
// ED_FrameDrawEchoLine draws the EchoLine at the bottom of the Frame.
//
// NOTE:	FrameWidth reflects RowChars... but there is 1 extra "extension" row.

void	ED_FrameDrawEchoLine(ED_FramePointer FrameP)
{
    Int32	LineY;
    Int16	Color;

    if (ED_QRPaneP && ED_QRPaneP->FrameP == FrameP) {
	ED_FrameQRDraw(FrameP);
	return;
    }

    // Clear first
    LineY = (FrameP->RowCount - 1) * ED_Row;
    XftDrawRect(FrameP->XftDP, &ED_XCArr[ED_White], 0, LineY, ED_FRAMELMARGIN + FrameP->FrameWidth + ED_Advance, ED_Height + 1);

    // Draw if there is something
    if (ED_FrameEchoLen) {
	Color = ED_Black;
	if (ED_FrameEchoMode == ED_ECHOERRORMODE)
	    Color = ED_Red;
	else if (ED_FrameEchoMode == ED_ECHOPROMPTMODE)
	    Color = ED_Blue;

	XftDrawStringUtf8(FrameP->XftDP, &ED_XCArr[Color], ED_XFP, ED_FRAMELMARGIN, LineY + ED_Ascent,
		       (XftChar8 *)ED_FrameEchoS, ED_FrameEchoLen);
    }
}

// ******************************************************************************
// ED_FrameSetEchoS copes the SourceP string into ED_FrameEchoS and sets its Len.
//
// NOTE:	This function waits for ED_FrameDrawEchoLine to draw it later.

void	ED_FrameSetEchoS(Int16 Mode, char * SourceP)
{
    Int16	Len = 0;
    char *	DP = ED_FrameEchoS;
    
    while ( (*DP++ = *SourceP++) ) Len++;
    ED_FrameEchoLen = Len;
    ED_FrameEchoMode = Mode;
}

void	ED_FrameSPrintEchoS(Int16 Mode, char * FormatP, ...)
{
    va_list	Args;

    va_start(Args, FormatP);
	ED_FrameEchoLen = vsnprintf(ED_FrameEchoS, ED_MSGSTRLEN - 1, FormatP, Args);
    va_end(Args);
    ED_FrameEchoMode = Mode;
}

void	ED_FrameSetEchoError(char * SourceP, Int32 Err)
{
    ED_FrameEchoLen = sprintf(ED_FrameEchoS, ED_STR_EchoErrorTemplate, SourceP, Err);
    ED_FrameEchoMode = ED_ECHOERRORMODE;
}

// ******************************************************************************
// ED_FrameFlashError blips the Echo line to indicate problem with command,
// running out of space, etc.  Takes the place of missing XBell.

void	ED_FrameFlashError(ED_FramePointer FrameP)
{
    Int32		LineY = (FrameP->RowCount - 1) * ED_Row;
    struct pollfd	PollR;
    XEvent		Event;

    PollR.fd = ConnectionNumber(ED_XDP);	// Negate to ignore all events
    PollR.events = POLLIN;
    XFillRectangle(ED_XDP, FrameP->XWin, FrameP->BlinkerGC, 0, LineY, FrameP->WinWidth, ED_Height + 1);
    XFlush(ED_XDP);
    poll(&PollR, 1, 100);
    XFillRectangle(ED_XDP, FrameP->XWin, FrameP->BlinkerGC, 0, LineY, FrameP->WinWidth, ED_Height + 1);
    XFlush(ED_XDP);

    while (XCheckMaskEvent(ED_XDP, KeyPressMask, &Event));
}


// ******************************************************************************
// ED_PaneNew creates a new Pane for the (new) FrameP.  This function is called
// only to initialize NEW Frames... otherwise ED_PaneSplit is normally called.

void	ED_PaneNew(ED_FramePointer FrameP, ED_BufferPointer NewBufP)
{
    ED_PanePointer	PaneP;

    PaneP = sc_SAStoreAllocBlock(&ED_PaneStore);
    PaneP->Tag = *(Uns32 *)ED_PaneTag;
    PaneP->Flags = ED_PANENOFLAG;
    PaneP->NextPaneP = NULL;			// First by definition
    PaneP->PrevPaneP = NULL;
    PaneP->FrameP = FrameP;
    PaneP->BufP = NULL;				// Must be set NULL here
    PaneP->XModeWin = 0;
    PaneP->XScrollWin = 0;
    PaneP->CursorPos = 0;
    PaneP->PanePos = 0;
    PaneP->CursorRow = 0;
    PaneP->CursorCol = 0;
    PaneP->TopRow = 0;
    PaneP->RowCount = FrameP->RowCount - 1;	// Pane includes its ModeLine
    PaneP->FracRowCount = (Int64)PaneP->RowCount << 32;	// 32.32
    PaneP->BufRowCount = 0;
    PaneP->StartRowCount = 0;
    FrameP->FirstPaneP = PaneP;
    FrameP->CurPaneP = PaneP;
    
    ED_PaneSetScrollBar(PaneP); 		// Must be set + created before PaneGetNewBuf  
    ED_PaneMakeScrollWin(PaneP);
    
    FrameP->PaneCount += 1;			// Will definitely get one!
    if (NewBufP == NULL) {
	PaneP->BufP = ED_FirstBufP;
	PaneP->BufP->PaneRefCount += 1;
    } else
	ED_PaneGetNewBuf(PaneP, NewBufP);
}

// ******************************************************************************
// ED_PaneSplit will split an existing pane into two, but only if big enough.
// Existing pane is top half, shrink it and reposition contents if necessary to
// keep Cursor visible.  New pane is bottom half.

void	ED_PaneSplit(ED_PanePointer PaneP)
{
    ED_PanePointer	NewPaneP;
    Int32		TotalRows;
    Int64		TotalFracRows;

    if (PaneP->RowCount < (2 * ED_PANEMINROWCOUNT)) return;

    NewPaneP = sc_SAStoreAllocBlock(&ED_PaneStore);
    NewPaneP->Tag = *(Uns32 *)ED_PaneTag;
    NewPaneP->Flags = ED_PANENOFLAG;
    
    NewPaneP->NextPaneP = PaneP->NextPaneP;
    NewPaneP->PrevPaneP = PaneP;
    if (PaneP->NextPaneP) PaneP->NextPaneP->PrevPaneP = NewPaneP;
    PaneP->NextPaneP = NewPaneP;

    NewPaneP->FrameP = PaneP->FrameP;
    NewPaneP->BufP = PaneP->BufP;
    NewPaneP->BufP->PaneRefCount += 1;
    NewPaneP->XModeWin = 0;
    NewPaneP->XScrollWin = 0;
    NewPaneP->CursorPos = PaneP->CursorPos;
    NewPaneP->PanePos = PaneP->PanePos;
    NewPaneP->CursorRow = PaneP->CursorRow;
    NewPaneP->CursorCol = PaneP->CursorCol;

    // Divide the pane, but account properly for fractional size.
    // If the pane is really 27.3 lines, then top gets 14 (13.65)
    // and bottom gets remaining 13 (13.65).  But we track the
    // 32.32 fractional parts for when we go to stretch the window
    // and scale up the Panes.
    
    TotalRows = PaneP->RowCount;
    TotalFracRows = PaneP->FracRowCount;

    PaneP->FracRowCount = TotalFracRows / 2;
    PaneP->RowCount = (PaneP->FracRowCount + 0x80000000LL) >> 32;	// Round
    
    NewPaneP->TopRow = PaneP->TopRow + PaneP->RowCount;
    NewPaneP->RowCount = TotalRows - PaneP->RowCount;
    NewPaneP->FracRowCount = TotalFracRows - PaneP->FracRowCount;
    NewPaneP->BufRowCount = PaneP->BufRowCount;			// Same Buf, same Frame
    NewPaneP->StartRowCount = PaneP->StartRowCount;		// Same PanePos

    // Create a ModeWin for new Pane.  But this gets tricky.
    // First time around, upper Pane gets XModeWin.  But if split again,
    // the upper Pane keeps the window (must be repositioned) and new,
    // bottom, pane must get a new one!

    if (PaneP->XModeWin == 0)
	ED_PaneMakeModeWin(PaneP);
    else {
	ED_PaneMakeModeWin(NewPaneP);
	ED_PanePositionModeWin(PaneP);
    }

    // The Cursor may have fallen off bottom of the smaller Panes... ScrollUp if necessary.

    ED_PanePositionScrollBar(PaneP);
    ED_PaneMoveAfterCursorMove(PaneP, -1, 0, 1);		// Force SetScrollBar
    ED_PaneMakeScrollWin(NewPaneP);
    ED_PaneMoveAfterCursorMove(NewPaneP, -1, 0, 1);		// Force SetScrollBar
    
    // Set minimum window size to accomodate all panes

    NewPaneP->FrameP->PaneCount += 1;
    ED_FrameResetWinMinSize(NewPaneP->FrameP);
}

// ******************************************************************************
// ED_PaneGetNewBuffer detaches the existing BufP from PaneP and replaces it with
// NewBufP.  It then computes all the necessary parameters, PanePos, StartRowCount,
// ScrollBar etc.
//
// NOTE:	New Panes are normally created by splitting an existing Pane.
//		In that case, the new Pane inherits the pre-existing setup from
//		the parent.  This function is called *ONLY* when reading a new
//		file/buffer into a Pane.

void	ED_PaneGetNewBuf(ED_PanePointer PaneP, ED_BufferPointer NewBufP)
{
    ED_BufferPointer	OldBufP = PaneP->BufP;
    
    // OldBuf gets divorced!  Stash Pane position info in it.
    // Brand new Panes (on new Frames) DO NOT have an OldBufP!
    if (OldBufP) {
	OldBufP->PaneRefCount -= 1;
	OldBufP->CursorPos = PaneP->CursorPos;
	OldBufP->PanePos = PaneP->PanePos;

	if (PaneP->BufP->Flags & ED_BUFINFOONLYFLAG)
	    ED_BufferKill(PaneP->BufP);		// Only kills if PaneRefCount is 0 !!
    }

    // Pane gets NewBuf
    NewBufP->PaneRefCount += 1;
    PaneP->BufP = NewBufP;

    // Transfer PanePos and CursorPos from Buf... but compute + verify as old
    // values could be from different Frame with different width!
    PaneP->PanePos = NewBufP->PanePos;
    PaneP->CursorPos = NewBufP->CursorPos;
    ED_PaneUpdateAllPos(PaneP, 1);
}

// ******************************************************************************
// ED_PaneKill will kill the Pane, but only if the Frame already has another.
// Assume a FrameDraw after this function.
//
// Top [-][X][-] -->  [--][-]	PaneP loses XModeWin, reposition Prev XModeWin
// Top [-][X]	 -->  [--]	PrevPaneP loses XModeWin
// Top [X][-]	 -->  [--]	PaneP loses XModeWin

void	ED_PaneKill(ED_PanePointer PaneP)
{
    if (PaneP->FrameP->PaneCount == 1)
	return;

    if (PaneP->PrevPaneP) {
	// Give space to previous Pane, it will become CurPaneP
	PaneP->FrameP->CurPaneP = PaneP->PrevPaneP;

	PaneP->PrevPaneP->FracRowCount += PaneP->FracRowCount;
	PaneP->PrevPaneP->RowCount += PaneP->RowCount;

	ED_PanePositionScrollBar(PaneP->PrevPaneP);
	ED_PaneSetScrollBar(PaneP->PrevPaneP);

	PaneP->PrevPaneP->NextPaneP = PaneP->NextPaneP;
	if (PaneP->NextPaneP) {
	    PaneP->NextPaneP->PrevPaneP = PaneP->PrevPaneP;
	    ED_PanePositionModeWin(PaneP->PrevPaneP);
	} else
	    ED_PaneKillModeWin(PaneP->PrevPaneP);
	
    } else if (PaneP->NextPaneP) {	// PaneP has no Prev if we get here
	ED_PanePointer		NP = PaneP->NextPaneP;
	Int32			Rows = PaneP->RowCount;
    
	// Give space to next Pane, Prev is NULL--so this was first
	PaneP->FrameP->FirstPaneP = NP;
	PaneP->FrameP->CurPaneP = NP;
    
	NP->FracRowCount += PaneP->FracRowCount;
	NP->RowCount += PaneP->RowCount;
	NP->TopRow -= PaneP->RowCount;

	// Try to leave NP's buffer unmoved, start the Pane N Rows sooner!  (But may hit Pos 0!)
	NP->PanePos = ED_BufferGetPosMinusRows(NP->BufP, NP->PanePos, &Rows, PaneP->FrameP->RowChars);
	NP->StartRowCount -= Rows;
	NP->CursorRow += Rows;

	ED_PanePositionScrollBar(NP);
	ED_PaneSetScrollBar(NP);

	NP->PrevPaneP = NULL;		// PaneP had no Prev!
    }

    ED_PaneKillModeWin(PaneP);
    ED_PaneKillScrollWin(PaneP);
    PaneP->FrameP->PaneCount -= 1;
    PaneP->BufP->PaneRefCount -= 1;
    if (PaneP->BufP->Flags & ED_BUFINFOONLYFLAG)
	ED_BufferKill(PaneP->BufP);		// Only kills if PaneRefCount is 0 !!
    
    PaneP->BufP->CursorPos = PaneP->CursorPos;	// Stash in Buffer, for next visit to Buffer
    PaneP->BufP->PanePos = PaneP->PanePos;
    ED_FrameResetWinMinSize(PaneP->FrameP);	// Lost a pane, so WinMinSize is less
    sc_SAStoreFreeBlock(&ED_PaneStore, PaneP);
    
}

// ******************************************************************************
// ED_PaneResize is called to re-size a Pane--and by extension the very next Pane.
// Return 1 if the size was changed, 0 otherwise.
//
// PaneResize is *ONLY* Y axis, moving ModeLine up and down.  (No impact on BufRowCount)
//
// The last Pane in a Frame cannot be resized.
//
// Panes cannot be resized below the PANEMINROWCOUNT threshold--applies to NextPaneP
// if PaneP is being enlarged.

Int16	ED_PaneResize(ED_PanePointer PaneP, Int32 Delta)
{
    ED_PanePointer	NextP;
    Int32		NewRowCount, RowStart;

    if (!PaneP || !PaneP->NextPaneP) return 0;

    NextP = PaneP->NextPaneP;
    if (Delta > 0) {			// Make Pane *BIGGER*

	NewRowCount = NextP->RowCount - Delta;
	if (NewRowCount < ED_PANEMINROWCOUNT) NewRowCount = ED_PANEMINROWCOUNT;
	Delta = NextP->RowCount - NewRowCount;
    } else {				// Make Pane *SMALLER* (Delta is NEG)
	NewRowCount = PaneP->RowCount + Delta;
	if (NewRowCount < ED_PANEMINROWCOUNT) NewRowCount = ED_PANEMINROWCOUNT;
	Delta = NewRowCount - PaneP->RowCount;
    }

    if (Delta) {
	PaneP->RowCount += Delta;
	NextP->TopRow += Delta;
	NextP->RowCount -= Delta;

	PaneP->FracRowCount = (Int64)PaneP->RowCount << 32;
	NextP->FracRowCount = (Int64)NextP->RowCount << 32;

	RowStart = -1;
	if ((Delta < 0) && PaneP->CursorPos) 			// PaneP shrank
	    RowStart = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);

	ED_PanePositionScrollBar(PaneP);
	ED_PaneMoveAfterCursorMove(PaneP, RowStart, 0, 1);	// Force SetScrollBar
	ED_PanePositionModeWin(PaneP);
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);
	ED_PaneDrawBlinker(PaneP);

	RowStart = -1;
	if ((Delta > 0) && NextP->CursorPos) 			// NextP shrank!
	    RowStart = ED_PaneFindLoc(NextP, NextP->CursorPos, &NextP->CursorRow, &NextP->CursorCol, 0, 0);

	ED_PanePositionScrollBar(NextP);
	ED_PaneMoveAfterCursorMove(NextP, RowStart, 0, 1);	// Force SetScrollBar
	ED_PaneDrawText(NextP);
	ED_PaneDrawScrollBar(NextP, 0);
	ED_PaneDrawBlinker(NextP);
	
	sc_BlinkTimerReset();
	return 1;
    }

    return 0;
}

// ******************************************************************************
// ED_PaneUpdateStartPos deals with the "Initial Wrap-Flow" problem in a Pane,
// where the first line is the continuation of a wrapped line, so can lengthen
// and shorten (even disappear) as the Frame width is shrunk or expanded.
//
// The proper approach is (1) find the hard beginning of the wrapped line,
// (2) count the number of wrapped rows, and (3) re-wrap based on the new frame
// width.  If the wrap overflow into the Pane falls to zero, then the previous
// row should be displayed--the cursor could have been on the overflow and
// should not be allowed to fall off the Pane.
//
// This function is also called to update PanePos (with OldRowChars = 0)
// when the buffer for this Pane is changed by another.  Char insertion in
// the buffer may effect this Pane if it starts on a wrapped row.

void	ED_PaneUpdateStartPos(ED_PanePointer PaneP, Int32 OldRowChars)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		Pos, Count, PrevRowPos, RowCount, SubCount, NewRowChars;
    char		*CurP;

    NewRowChars = PaneP->FrameP->RowChars;
    Pos = PaneP->PanePos;
    if ((Pos == 0) || (OldRowChars == NewRowChars)) return;

    CurP = ED_BufferPosToPtr(BufP, --Pos);		// Look at prev char
    if (*CurP == '\n') return;				// Starts on full line!!

    RowCount = 0;
    Count = 0;
    if (OldRowChars == 0) OldRowChars = NewRowChars;	// Not Changed!
    while (1) {
	while (ED_BUFFERISMIDUTF8(*CurP)) CurP--, Pos--;

	Count++;					// Count is 1-based
	if (Pos == 0) {
	    RowCount += (Count / OldRowChars);
	    break;
	}

	if (*CurP == '\n') {				// Hit hard NewLine
	    Count--;					// Go forward 1
	    CurP = ED_BufferPosToPtr(BufP, ++Pos);
	    RowCount += (Count / OldRowChars);
	    break;
	}

	CurP = ED_BufferPosToPtr(BufP, --Pos);
    }

    PrevRowPos = Pos;				// Start of line for now
    Count = RowCount * NewRowChars;
    SubCount = 0;
    while (Count--) {
	Pos += ED_BufferGetUTF8Len(*CurP);
	CurP = ED_BufferPosToPtr(BufP, Pos);
	if (Pos == BufP->LastPos) break;	// Hit the end!

	if (*CurP == '\n') {			// No overflow left
	    Pos = PrevRowPos;			// Show from LAST wrap/line start
	    break;
	}

	SubCount++;				// Look for previous line wraps
	if (SubCount == NewRowChars) {		// Line will wrap here
	    PrevRowPos = Pos;			// Stash the position
	    SubCount = 0;			// Now look for another
	}
	    
    } // While

    PaneP->PanePos = Pos;
}

// ******************************************************************************
// ED_PaneUpdateAllPos is called after major changes were made to the Pane's buffer.
// It will go through and re-do all relevent display parameters, including setting
// the ScrollBar.

void	ED_PaneUpdateAllPos(ED_PanePointer PaneP, Int32 MoveCursor)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		RowStartPos, Col;
    
    // Set PanePos and StartRowCount first
    if (PaneP->PanePos >= BufP->LastPos) PaneP->PanePos = 0;
    PaneP->PanePos = ED_PaneFindLoc(PaneP, PaneP->PanePos, &PaneP->StartRowCount, &Col, 1, 0);
    
    // Get BufRowCount--find it Pane-relative, then add in rows before PanePos.
    ED_PaneFindLoc(PaneP, BufP->LastPos, &PaneP->BufRowCount, &Col, 0, 0);
    PaneP->BufRowCount += PaneP->StartRowCount;
    
    // Now setup the cursor
    if (MoveCursor && (PaneP->CursorPos < PaneP->PanePos))
	    PaneP->CursorPos = PaneP->PanePos;
   
    RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    if ((PaneP->CursorRow < 0) || (PaneP->CursorRow > PaneP->RowCount / 2))
	ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, PaneP->RowCount / 2, 0);

    // Set scroll bar and update other panes (showing same buffer).
    ED_PaneSetScrollBar(PaneP);
}


// ******************************************************************************
// ED_PaneDrawBlinker draws the blinker for the given Pane.
// The concept of a Blinker gets implemented as a Solid or Box Cursor.
//
// The Blinker in a pane that has keyboard Focus is a solid rect, blinks on and
// off.  The Blinker in a pane WITHOUT focus is a hollow box, does not blink.

void	ED_PaneDrawBlinker(ED_PanePointer PaneP)
{
    ED_FramePointer	FrameP = PaneP->FrameP;

   if ((FrameP->Flags & ED_FRAMEFOCUSFLAG) &&
	(PaneP == FrameP->CurPaneP) &&
	(ED_QRPaneP != PaneP)) {
	// Gets solid cursor.  Erase box cursor if it had it.
	// Draw solid cursor if not there.

	if (PaneP->Flags & ED_PANEBOXCURSORFLAG) ED_PaneDrawCursor(PaneP, 1);
	if (! (PaneP->Flags & ED_PANESOLIDCURSORFLAG)) ED_PaneDrawCursor(PaneP, 0);

    } else {

	// Gets a box cursor.  Erase Solid cursor if it had it.
	// Draw box cursor if not there.

	if (PaneP->Flags & ED_PANESOLIDCURSORFLAG) ED_PaneDrawCursor(PaneP, 0);
	if (! (PaneP->Flags & ED_PANEBOXCURSORFLAG)) ED_PaneDrawCursor(PaneP, 1);
    }
}

// ******************************************************************************
// ED_PaneEraseCursor will erase the cursor (if visible) from the Pane.

void	ED_PaneEraseCursor(ED_PanePointer PaneP)
{
    if (PaneP->Flags & ED_PANEBOXCURSORFLAG) ED_PaneDrawCursor(PaneP, 1);
    if (PaneP->Flags & ED_PANESOLIDCURSORFLAG) ED_PaneDrawCursor(PaneP, 0);
}

// ******************************************************************************
// ED_PaneDrawCursor will draw a cursor of the indicated type in the Pane.
// The cursor flag is toggled to reflect the GXxor drawing.
//
// If a selection (Sel hilite) region is shown, the Cursor will be at the beginning
// or the end.  If the former, the cursor will be physically drawn ON TOP of the
// colored rect, and will be the WRONG color, unless HLBlinkerGC is used.
// (Same issue with Incremental Search (IS) Match.)
//
// NOTE:	RealEmacs will keep a Sel region if another application
//		gets focus, so it *CAN* draw a Box cursor on the Sel region.
//		This program will *NOT* draw the Sel region without focus.

void	ED_PaneDrawCursor(ED_PanePointer PaneP, Int16 Box)
{
    ED_FramePointer	FP = PaneP->FrameP;
    Int32		X, Y;
    GC			BGC;

    if (FP->Flags & ED_FRAMENOWINFLAG) return;
    
    X = FP->FrameX + (PaneP->CursorCol * ED_Advance);
    Y = FP->FrameY + ((PaneP->TopRow + PaneP->CursorRow) * ED_Row);
    if (Box) {
	XDrawRectangle(ED_XDP, FP->XWin, FP->BlinkerGC, X, Y, ED_Advance-1, ED_Height-1);
	PaneP->Flags ^= ED_PANEBOXCURSORFLAG;
	return;
    }

    // Drawing solid cursor here!
    // First check for the two ISearch conditions MatchPos and ISAltPos
    if (PaneP == ED_ISPaneP) {
	BGC = FP->HLBlinkerGC;
	if (PaneP->CursorPos == ED_ISMatchPos) {
	    XSetForeground(ED_XDP, BGC, ED_XCArr[ED_Purple].pixel);
	    goto DrawSolid;
	}

	if (PaneP->CursorPos == ED_ISAltPos) {
	    XSetForeground(ED_XDP, BGC, ED_XCArr[ED_Turquoise].pixel);
	    goto DrawSolid;
	}
    }

    // If not on search, then perhaps in SelqRange
    if ((PaneP == ED_SelPaneP) && (PaneP->CursorPos < ED_SelMarkPos)) {
	BGC = FP->HLBlinkerGC;
	XSetForeground(ED_XDP, BGC, ED_XCArr[ED_SelOrange].pixel);
	goto DrawSolid;
    }

    // Just a regular cursor!
    BGC = FP->BlinkerGC;

DrawSolid:	    
    XFillRectangle(ED_XDP, FP->XWin, BGC, X, Y, ED_Advance, ED_Height);	
    PaneP->Flags ^= ED_PANESOLIDCURSORFLAG;
}

// ******************************************************************************
// ED_PaneGetDrawRow is called to obtain a Row of chars from the Buffer for drawing.
// In most cases, the Row is terminated by a '\n'.  But when dealing with a long
// line, it is possible to have a few rows before the newline char is encountered.
// In these cases, *WrapP is set for all but the last Row.
//
// The Row of chars can start before the Gap, or after it.  If the former, the Row
// can run into the Gap, i.e. get 'cut', so *RowCutP is set and the rest of the
// Row can be obtained (and drawn) with a subsequent call.
//
// *CountP is the number of Chars/glyphs in the Row (or current segment), NOT BYTES.
// *PosP is the Position to start with--conceptually skips over the Gap.
//
// NOTE:	DO NOT CALL with *PosP greater than BufP->LastPos.

char *	ED_PaneGetDrawRow(ED_PanePointer PaneP, Int32 *PosP, Int32 *ColP, Int32 *CountP, Int16 * WrapP, Int16 * PartialP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    char		*StartP, *EndP, *CurP;
    Int32		ColLimit;
    Int16		L;

    ColLimit = PaneP->FrameP->RowChars;
    StartP = BufP->BufStartP + *PosP;

    if (StartP >= BufP->GapStartP) {			// Hit Gap already
	StartP += (BufP->GapEndP - BufP->GapStartP);	// Jump over whole gap
	EndP = BufP->BufEndP;				// Can go until BufEnd
	*PartialP = 0;					// Can never be True
    } else {
	EndP = BufP->GapStartP;				// Can go until GapStart
	*PartialP = 1;					// Can have SegLeft
    }

    CurP = StartP;
    *CountP = 0;
    *WrapP = 0;						// Assume no wrap!
    while (CurP < EndP) {

	*ColP += 1;
	if (*CurP == '\n') {				// Hard EOL
	    *PartialP = 0;				// Definitely full line
	    *PosP += 1;					// Skip this \n
	    break;
	}

	if (*ColP > ColLimit) {				// Wrap around
	    *WrapP = 1;
	    break;
	}
    
	L = ED_BufferGetUTF8Len(*CurP);			// Can be 1-4 bytes
	*PosP += L, CurP += L;				// Advance to next char
    }

    *CountP =  CurP - StartP;
    return StartP;
}

// ******************************************************************************
// ED_PaneDrawText draws the contents of the given Pane, followed by its mode line.
// If a Row is 'Partial', must call ED_PaneGetDrawRow again to get and draw the rest of it.
//
// NOTE:	XftDrawString8 will occasionally draw slightly outside its glyph envelope.
//		(Saw this with Underscore char!)  So erase using (ED_Height + 1).
//
// NOTE:	This is an excellent place to STASH the CurPane state (Pane + Cursor Pos)
//		into its Buffer...  So if another Pane ever opens the same Buffer, it will
//		end up in this state.  No point in doing this for other Panes, as only the
//		CurPaneP of the FrameP with focus ever changes!!  (And every change is
//		followed by a PaneDrawText!)


// Variables used for PaneDrawText (PDT) showing Incremental Search (IS) matches.
Int16	ED_PDT_InMatch = 0;		// PaneDrawText: IN a match to ISStr (may be Alt)
Int16	ED_PDT_IsMain = 0;		// PaneDrawText: This match is the MAIN one (not Alt)
Int32	ED_PDT_StartPos = 0;		// PaneDrawText: StartPos for last match (Main or Alt)

// Find any matches that start *BEFORE* top of pane, but may extend INTO pane.
// (If in QueryReplace (QREP), not interested in alternative BEFORE the main match,
// so no point in checking here--the main match CANNOT start before the Pane top!)
void	ED_PDTFindHangingMatch(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		CurPos;

    ED_PDT_InMatch = 0;
    ED_PDT_StartPos = 0;
    if (ED_QREPPaneP) return;
    
    if (PaneP->PanePos) {
	CurPos = PaneP->PanePos - ED_ISStrLen;
	if (CurPos < 0) CurPos = 0;
	while (CurPos < PaneP->PanePos) {
	    if (ED_ISCheckMatch(BufP, CurPos)) {
		ED_PDT_InMatch = 1;
		ED_PDT_StartPos = CurPos;
		ED_PDT_IsMain = (CurPos == ED_ISMatchPos);
		break;
	    }
	    CurPos += 1;
	}
    }
}

// Hilite IS match--Purple for Main, Turquoise for Alt.
void	ED_PDTHiliteMatch(ED_PanePointer PaneP, Int16 IsMain, Int32 StartCol, Int32 EndCol, Int32 Row)
{
    ED_FramePointer	FP = PaneP->FrameP;
    Int32		StartX = ED_FRAMELMARGIN + (StartCol * ED_Advance);
    Int32		Width = (EndCol - StartCol) * ED_Advance;
    Int32		LineY = (PaneP->TopRow + Row) * ED_Row;
    Int16		Color = (IsMain) ? ED_Purple : ED_Turquoise;

    XftDrawRect(FP->XftDP, &ED_XCArr[Color], StartX, LineY, Width, ED_Row);
}

// Finds and draws IS matches (Main or Alt) *AS* PaneDrawText gets text one row at a time.
// On occasion, PDT will hit the gap in mid-row... so a match may be interruped and span two of these
// calls.  In that case, it will be hilited with two rects!
//
// If in QREP (QuerySearch) mode, only draw alternatives AFTER the current main match.
void	ED_PDTDrawMatch(ED_PanePointer PaneP, Int32 StartPos, char *CurP, Int32 Count, Int32 Row, Int32 Col)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		L, CurPos, StartCol;
    char		*EndP;

    StartCol = Col;
    CurPos = StartPos;
    EndP = CurP + Count;

    // If QREP do NOT search before the main match!  If the main Match
    // is at hand, just count the Cols to it, then start searching.
    // This does the checks up front, without slowing the main loop down
    // for regular IS--without QREP.
    if (ED_QREPPaneP && (ED_ISMatchPos >= 0)) {
	if (CurPos + Count < ED_ISMatchPos)
	    return;
	else while (CurPos < ED_ISMatchPos) {
	    L = ED_BufferGetUTF8Len(*CurP);
	    CurP += L; CurPos += L; Col += 1;
	}
    }
    
    while (CurP < EndP) {
	// Cannot go into overflow column
	if (Col >= PaneP->FrameP->RowChars)
	    break;

	// Could be a left-over from previous rows!
	if (ED_PDT_InMatch && (CurPos - ED_PDT_StartPos) >= ED_ISStrLen) {
	    ED_PDTHiliteMatch(PaneP, ED_PDT_IsMain, StartCol, Col, Row);
	    ED_PDT_InMatch = 0;
	    ED_PDT_IsMain = 0;
	}

	// Logic must allow for back-to-back matches! (An Alt following Main leads to ED_ISAltPos)
	if ((! ED_PDT_InMatch) && (ED_ISCheckMatch(BufP, CurPos))) {
	    ED_PDT_InMatch = 1;
	    ED_PDT_StartPos = CurPos;
	    ED_PDT_IsMain = (CurPos == ED_ISMatchPos);
	    StartCol = Col;
	    if (CurPos == (ED_ISMatchPos + ED_ISStrLen))
		ED_ISAltPos = CurPos;
	}

	L = ED_BufferGetUTF8Len(*CurP);
	CurP += L;
	CurPos += L;
	Col += 1;
    }

    // Reached the end of the row or row-segment... If still InMatch, hilite it.
    if (ED_PDT_InMatch) {
	ED_PDTHiliteMatch(PaneP, ED_PDT_IsMain, StartCol, Col, Row);
    }
}

void	ED_PaneDrawText(ED_PanePointer PaneP)
{
    ED_FramePointer	FP = PaneP->FrameP;
    Int32		StartPos, Pos, LineCount, LineX, LineY, StartCol, Col, Count;
    Int16		LineWrap, Partial;
    char *		CharP;

    // If this is *the* CurPane, stash Pane state into its Buf... 
    if ((PaneP == FP->CurPaneP) && (FP->Flags & ED_FRAMEFOCUSFLAG)) {
	PaneP->BufP->PanePos = PaneP->PanePos;
	PaneP->BufP->CursorPos = PaneP->CursorPos;
    }

    Pos = PaneP->PanePos;
    LineCount = 0;
    LineY = PaneP->TopRow * ED_Row;

    // Decide whether very first line was wrapped!
    LineWrap = 0;
    if (Pos) {
	CharP = ED_BufferPosToPtr(PaneP->BufP, Pos - 1);
	LineWrap = ! (*CharP == '\n');
    }

    // Draw left and right edge band
    XftDrawRect(FP->XftDP, &ED_XCArr[ED_Touch], 0, (PaneP->TopRow * ED_Row),
		ED_FRAMELMARGIN, (PaneP->RowCount - 1) * ED_Row);
    XftDrawRect(FP->XftDP, &ED_XCArr[ED_Touch],
		ED_FRAMELMARGIN + (FP->RowChars * ED_Advance), (PaneP->TopRow * ED_Row),
		ED_Advance, (PaneP->RowCount - 1) * ED_Row);

    // Catch matches that start *above* the pane, but end in the pane!
    if (PaneP == ED_ISPaneP)
	ED_PDTFindHangingMatch(PaneP);

    while (++LineCount < PaneP->RowCount) {
	// Draw background for row... normally just white, unless hilited for selection, search, etc.
	ED_PaneDrawBackground(PaneP, LineCount - 1, LineY);

	// Draw left bar if line was wrapped
	if (LineWrap) XftDrawRect(FP->XftDP, &ED_XCArr[ED_Gray], 1, LineY, ED_FRAMELMARGIN - 1, ED_Height);

	// Draw the row, but it may intersect the Gap, so need to draw TWO segments of 1 row.
	LineX = ED_FRAMELMARGIN;
	Col = 0;
	do {
	    StartPos = Pos;
	    StartCol = Col;
	    CharP = ED_PaneGetDrawRow(PaneP, &Pos, &Col, &Count, &LineWrap, &Partial);
	    if ((PaneP == ED_ISPaneP) && (ED_ISMatchPos >= 0))
		ED_PDTDrawMatch(PaneP, StartPos, CharP, Count, LineCount - 1, StartCol);
	    
	    if (Count) {
		XftDrawStringUtf8(FP->XftDP, &ED_XCArr[ED_Black], ED_XFP, LineX, LineY + ED_Ascent, (XftChar8 *)CharP, Count);
		LineX = ED_FRAMELMARGIN + (Col * ED_Advance);
		if (LineWrap) break;
	    }
	} while (Partial);

	// Draw right bar if line was wrapped
	if (LineWrap) XftDrawRect(FP->XftDP, &ED_XCArr[ED_Gray],
				  ED_FRAMELMARGIN + (FP->RowChars * ED_Advance) + 1, LineY,
				  ED_FRAMELMARGIN - 1, ED_Height);

	LineY += ED_Row;
	if (Pos >= PaneP->BufP->LastPos) break;		// Termination!
    }

    // Draw blanks in case any rows remain on the Pane

    while (++LineCount < PaneP->RowCount) {
	XftDrawRect(FP->XftDP, &ED_XCArr[ED_White], ED_FRAMELMARGIN, LineY, FP->RowChars * ED_Advance, ED_Height + 1);
	LineY += ED_Row;
    }

    ED_PaneDrawModeLine(PaneP);

    // Drawing erases blinkers, so reset the flags.
    PaneP->Flags &= ~(ED_PANESOLIDCURSORFLAG | ED_PANEBOXCURSORFLAG);
}

// ******************************************************************************
// ED_PaneDrawBackground is called from within ED_PaneDrawText to "erase" the
// background before text is drawn on top of it.  This function also provides
// the highlighting for Selection--Xft draws text glyphs in grayscale
// so the hilite must be drawn *UNDER* the text, not over it!
//
// NOTE:	The screen will flash, especially when scrolling, if the
//		row is fully painted white, then highlighted for Sel.
//		Better to paint each section of the Row just once.
//
// The Cursor/Blinker must use SelBlinkerGC if drawn on top of Sel highlights.
// (That only happens if range is from Cursor to SelMark, as the Cursor will
// come directly after the range when Sel is from SelMark to Cursor.)
// (Likewise must use ISBlinkerGC if drawn on top of IS highlights.)

void	ED_PaneDrawBackground(ED_PanePointer PaneP, Int32 Row, Int32 LineY)
{
    ED_FramePointer	FP = PaneP->FrameP;
    Int32		StartCol, EndCol;
    Int16		DrawSel = 0;

    if (ED_SelPaneP == PaneP) {						// Can have Sel !
	if (ED_SelMarkPos < PaneP->CursorPos) {				// From SelPos to CursorPos
	    if ((ED_SelMarkRow <= Row) && (Row <= PaneP->CursorRow)) {	// This row has Sel!
		StartCol = (ED_SelMarkRow == Row) ? ED_SelMarkCol : 0;
		EndCol = (PaneP->CursorRow == Row) ? PaneP->CursorCol : FP->RowChars;
		DrawSel = 1;
	    }
	    
	} else if (PaneP->CursorPos < ED_SelMarkPos) {			// From CursorPos to SelPos
	    if ((PaneP->CursorRow <= Row) && (Row <= ED_SelMarkRow)) {	// This row has Sel!
		StartCol = (PaneP->CursorRow == Row) ? PaneP->CursorCol : 0;
		EndCol = (ED_SelMarkRow == Row) ? ED_SelMarkCol : FP->RowChars;
		DrawSel = 1;
	    }
	}
    }

    // If the row has Sel, it can have 3 sections:  Before (in white), Sel (orange), After (in white again).
    if (DrawSel) {	
	if (StartCol)
	    XftDrawRect(FP->XftDP, &ED_XCArr[ED_White], ED_FRAMELMARGIN, LineY, StartCol * ED_Advance, ED_Height + 1);

	XftDrawRect(FP->XftDP, &ED_XCArr[ED_SelOrange],
		    ED_FRAMELMARGIN + (StartCol * ED_Advance), LineY,
		    (EndCol - StartCol) * ED_Advance, ED_Height + 1);

	if (EndCol < FP->RowChars)
	    XftDrawRect(FP->XftDP, &ED_XCArr[ED_White], ED_FRAMELMARGIN + (EndCol * ED_Advance), LineY,
			(FP->RowChars - EndCol) * ED_Advance, ED_Height + 1);
    } else
	// No selection on this row, just paint it white!
	XftDrawRect(FP->XftDP, &ED_XCArr[ED_White], ED_FRAMELMARGIN, LineY, FP->RowChars * ED_Advance, ED_Height + 1);
}

// ******************************************************************************
// ED_PaneDrawModeLine draws the modeline at the bottom of the Pane.
//
// NOTE:	A large amount of info is displayed in DEBUG mode.
//		These numbers are 0-based to help debugging.
//		User-level Character position and line numbers are 1-based.

void	ED_PaneDrawModeLine(ED_PanePointer PaneP)
{
    ED_FramePointer	FrameP = PaneP->FrameP;
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		LineY;
    char		ModeString[ED_MSGSTRLEN];
    char		DirName[64];
    char		PercentString[8], *PStringP;
    char		*FilterP;
    char		NullString[] = "";
    char	        ModMark;
    char		*ModeP;
    Int16		ModeStringLen, Color;

    if (PaneP->StartRowCount == 0)
	PStringP = (PaneP->BufRowCount <= PaneP->RowCount) ? ED_STR_All : ED_STR_Top;
    else if ((PaneP->StartRowCount + PaneP->RowCount) > PaneP->BufRowCount)
	PStringP = ED_STR_Bot;
    else {
	Int32  	PerCent = ((Int32)PaneP->StartRowCount * 100) / PaneP->BufRowCount;

	snprintf(PercentString, 8, "%d%%", PerCent);
	PStringP = PercentString;
    }

    FilterP = (BufP->Flags & ED_BUFFILTERFLAG) ? ED_STR_Filtered : NullString;
    ModMark = (BufP->Flags & ED_BUFMODFLAG) ? '*' : '-';
    if (BufP->Flags & ED_BUFREADONLYFLAG)
	ModMark = '%';
    if (BufP->Flags & ED_BUFNAMECOLFLAG)
	sprintf(DirName, "<%.*s>", 62, BufP->PathName + BufP->DirNameOffset);
    else
	DirName[0] = 0;

    ModeP = NullString;
    if (PaneP == ED_QREPPaneP) ModeP = ED_STR_QReplace;
    else if (PaneP == ED_ISPaneP) ModeP = ED_STR_ISearch;

#ifdef DEBUG
    snprintf(ModeString, ED_MSGSTRLEN - 1, "U:%c%c %.*s %.*s %s  %s R%d (Text%s) (R %d-%d) (P %d-%d) (S %d-%d-%d) (C %d,%d:%d)",
	     ModMark, ModMark, 64, BufP->FileName, 64, DirName, FilterP, PStringP,
	     PaneP->CursorRow + PaneP->StartRowCount + 1, ModeP,
	     PaneP->StartRowCount, PaneP->BufRowCount,
	     PaneP->PanePos, BufP->LastPos,
	     PaneP->ScrollTop, PaneP->ScrollThumb, PaneP->ScrollHeight,
	     PaneP->CursorCol, PaneP->CursorRow, PaneP->CursorPos);
#else
    snprintf(ModeString, ED_MSGSTRLEN - 1, "U:%c%c %.*s %.*s %s  %s R%d (Text%s)",
	     ModMark, ModMark, 64, BufP->FileName, 64, DirName, FilterP, PStringP,
	     PaneP->CursorRow + PaneP->StartRowCount + 1, ModeP);
#endif

    ModeStringLen = strlen(ModeString);
    

    Color = ((FrameP->Flags & ED_FRAMEFOCUSFLAG) && (PaneP == FrameP->CurPaneP)) ? ED_Gray : ED_LightGray;
    
    LineY = ED_Ascent + (ED_Row * (PaneP->TopRow + PaneP->RowCount - 1));
    XftDrawRect(FrameP->XftDP, &ED_XCArr[Color], 0, LineY - ED_Ascent,
		((FrameP->RowChars + 1) * ED_Advance) + ED_FRAMELMARGIN + ED_FRAMERMARGIN, ED_Height + 1);
		
    XftDrawStringUtf8(FrameP->XftDP, &ED_XCArr[ED_Black], ED_XFP,
		      ED_FRAMELMARGIN, LineY,
		      (XftChar8 *)ModeString, ModeStringLen);
}

// ******************************************************************************
// ED_PaneMakeModeWin is called to create an InputOnly window for the ModeLine--
// for all but the bottom Pane of the window.  ModeWins are registered with
// the main event loop... will get own events.  Must un-register when destroyed.

void	ED_PaneMakeModeWin(ED_PanePointer PaneP)
{
    Window			Win;
    Int16			XScreenN = DefaultScreen(ED_XDP);
    Uns64			Mask;
    XSetWindowAttributes	AttrRec;

    Mask = CWCursor | CWEventMask;
    AttrRec.cursor = ED_SizeCursor;
    AttrRec.event_mask = ButtonPressMask | ButtonReleaseMask | Button1MotionMask;
    
    Win = XCreateWindow(ED_XDP, PaneP->FrameP->XWin, 0, 0, 1, 1, 0, 0,
			InputOnly, DefaultVisual(ED_XDP, XScreenN), Mask, &AttrRec);

    PaneP->XModeWin = Win;
    ED_PanePositionModeWin(PaneP);
    
    sc_WERegAdd(Win, &ED_PaneModeWinHandleEvent, PaneP);	// Reg with main loop.
    XMapWindow(ED_XDP, Win);
}

// ******************************************************************************
// ED_PaneKillModeWin will destroy the XModeWin, but only if the Pane has it.

void	ED_PaneKillModeWin(ED_PanePointer PaneP)
{
    if (PaneP->XModeWin) {
	XDestroyWindow(ED_XDP, PaneP->XModeWin);
	PaneP->XModeWin = 0;
    }
}

// ******************************************************************************
// ED_PanePositionModeWin is called to postion (both initially and after a resize)
// the XModeWin window for the Pane.  The only Pane in a Frame will NOT have an
// XModeWin, since the pane cannot be resized (independent of the Frame).
//
// MUST check PaneP->XModeWin is present before calling.

void	ED_PanePositionModeWin(ED_PanePointer PaneP)
{
    Int32	LineY;

    LineY = ED_Row * (PaneP->TopRow + PaneP->RowCount - 1);
    XMoveResizeWindow(ED_XDP, PaneP->XModeWin, ED_FRAMELMARGIN, LineY,
		      ((PaneP->FrameP->RowChars + 1) * ED_Advance) + ED_FRAMERMARGIN, ED_Height);
}

// ******************************************************************************
// ED_PaneModeWinHandleEvent is registered as the event handler for the
// XModeWin of PaneP... it is dispatched from Main.c.
//
// Only Button1 is used to re-size the Pane... other buttons are ungrabbed.
//
// Note:	This function *can* be built with XQueryPointer in response
//		to a Button1Press... But that would require special handling
//		for the blinker (and the polling is just as inefficient).

void	ED_PaneModeWinHandleEvent(XEvent * EventP, ED_PanePointer PaneP)
{
    Int32	DeltaRow;

    switch (EventP->type) {
	case MotionNotify:
	    // Flush extra ButtonMotion events, just look at last.
	    while (XCheckMaskEvent(ED_XDP, ButtonMotionMask, EventP));
	    
	    DeltaRow = (EventP->xmotion.y - (ED_Row / 2)) / ED_Row;
	    if (DeltaRow) ED_PaneResize(PaneP, DeltaRow);
	    break;

	case ButtonPress: {
	    ED_FramePointer	FrameP = PaneP->FrameP;
	    
	    if (EventP->xbutton.button != Button1)	// Another button!
		XUngrabPointer(ED_XDP, CurrentTime);	// Undo automatic grab

	    if ((ED_QREPPaneP == NULL) && (ED_ISPaneP == NULL) && (ED_QRPaneP == NULL)) {
		ED_FrameEchoLen = 0;		// Clear it out!
		ED_FrameDrawEchoLine(FrameP);
	    }
		
	    if (PaneP != FrameP->CurPaneP) {
		FrameP->CurPaneP = PaneP;
		ED_FrameDrawAll(FrameP);
	    } break;
	}

	default: break;
    }
}

// ******************************************************************************
// ******************************************************************************
// Each pane gets a vertical scroll bar.  These are NOT implemented as widgets,
// but simply use an input-only window to update the cursor.  The contents are
// drawn as rects... the thumb is orange if the Frame window has focus and gray
// otherwise.

// ******************************************************************************
// ED_PaneMakeScrollWin create the actual XScrollWin as an InputOnly XWin for
// the Pane and register the event handler for it.

void	ED_PaneMakeScrollWin(ED_PanePointer PaneP)
{
    Window			Win;
    Int16			XScreenN = DefaultScreen(ED_XDP);
    Uns64			Mask;
    XSetWindowAttributes	AttrRec;

    Mask = CWCursor | CWEventMask;
    AttrRec.cursor = ED_ArrowCursor;
    AttrRec.event_mask = ButtonPressMask | ButtonReleaseMask | Button1MotionMask | EnterWindowMask | LeaveWindowMask;

    Win = XCreateWindow(ED_XDP, PaneP->FrameP->XWin, 0, 0, 1, 1, 0, 0,
			InputOnly, DefaultVisual(ED_XDP, XScreenN), Mask, &AttrRec);
    PaneP->XScrollWin = Win;
    ED_PanePositionScrollBar(PaneP);
    sc_WERegAdd(Win, &ED_PaneScrollWinHandleEvent, PaneP);
    XMapWindow(ED_XDP, Win);
}

// ******************************************************************************
// ED_PaneKillScrollWin destroys the XScrollWin and un-registers its event handler.

void	ED_PaneKillScrollWin(ED_PanePointer PaneP)
{
    sc_WERegDel(PaneP->XScrollWin);
    XDestroyWindow(ED_XDP, PaneP->XScrollWin);
    PaneP->XScrollWin = 0;
}

// ******************************************************************************
// ED_PanePositionScrollBar is called to size and position the Pane's XScrollWin.
// It is called when the scroll bar is first created, but also if the Frame or
// Pane is resized (or split).

void	ED_PanePositionScrollBar(ED_PanePointer PaneP)
{
    XMoveResizeWindow(ED_XDP, PaneP->XScrollWin,
		      ED_FRAMELMARGIN + ((PaneP->FrameP->RowChars + 1) * ED_Advance), ED_Row * (PaneP->TopRow),
		      ED_FRAMERMARGIN, ED_Row * (PaneP->RowCount - 1));
}

// ******************************************************************************
// ED_PaneSetScrollBar is called to properly set the operating parameters for the
// the Scroll Bar.  It should be called if the RowCount or BufRowCount changes--as
// well as the Pane width which impact line wrap-around.  The Scroll bar has a Top
// area, a Thumb, and a Bottom area, but only the Top, Thumb, and total Height
// values are recorded.
//
// Scale and IScale are 32.32 fixed point numbers, good for multiply, NOT divide.
// Scale is (Pixels/Rows) and used for positioning the Thumb given the Row numbers.
// IScale is (Rows/Pixels) and good for getting Row numbers given Thumb positions.
// IScale is also stored in the Pane to avoid computation on each draw.
//
// The length of the Thumb shrinks and expands to reflect the visible RowCount
// versus the total BufRowCount.  However, the thumb does NOT shrink any smaller
// than ED_SCROLLTHUMBMINLEN.
//
// No thumb is drawn for an empty Pane.

void	ED_PaneSetScrollBar(ED_PanePointer PaneP)
{
    Int32	Height, Span;
    Int64	Scale, IScale;

    Height = ED_Row * (PaneP->RowCount - 1);
    Span = PaneP->BufRowCount + PaneP->RowCount;
    if (Span == 0) {
	PaneP->ScrollScale = 0LL;
	PaneP->ScrollTop = Height;
	PaneP->ScrollThumb = 0;
	PaneP->ScrollHeight = Height;
	return;
    }
    
    Scale = ((Uns64)Height << 32) / Span;
    IScale = ((Uns64)Span << 32) / Height;

    // If ScrollThumb is too small, floor its size, re-scale Top + Bot space
    PaneP->ScrollThumb = (Scale * PaneP->RowCount) >> 32;
    if (PaneP->ScrollThumb < ED_SCROLLTHUMBMINLEN) {
	PaneP->ScrollThumb = ED_SCROLLTHUMBMINLEN;
	Scale = ((Uns64)(Height - ED_SCROLLTHUMBMINLEN) << 32) / (Span - PaneP->RowCount);
	IScale = ((Uns64)(Span - PaneP->RowCount) << 32) / (Height - ED_SCROLLTHUMBMINLEN);
    }

    PaneP->ScrollScale = IScale;
    PaneP->ScrollTop = (Scale * PaneP->StartRowCount) >> 32;
    PaneP->ScrollHeight = Height;
}

// ******************************************************************************
// ED_PaneDrawScrollBar draws on the right side of its Pane.  It draws the
// background first, then the thumb--which is drawn full-width if Grabbed.

void	ED_PaneDrawScrollBar(ED_PanePointer PaneP, Int16 Grabbed)
{
    Int32		X, Y;
    Int16		Color;
    ED_FramePointer	FrameP;

    FrameP = PaneP->FrameP;

    X = ED_FRAMELMARGIN + ((FrameP->RowChars + 1) * ED_Advance);
    Y = PaneP->TopRow * ED_Row;

    // Paint the area first.
    XftDrawRect(FrameP->XftDP, &ED_XCArr[ED_TouchPlus], X, Y, ED_FRAMERMARGIN, ED_Row * (PaneP->RowCount - 1));

    // Draw the bar it NOT an empty Pane.
    if (PaneP->ScrollScale) {
	Y += PaneP->ScrollTop;
	Color = (FrameP->Flags & ED_FRAMEFOCUSFLAG) ? ED_Orange : ED_Gray;
	if (Grabbed)
	    XftDrawRect(FrameP->XftDP,  &ED_XCArr[Color], X, Y, ED_FRAMERMARGIN, PaneP->ScrollThumb);
	else
	    XftDrawRect(FrameP->XftDP,  &ED_XCArr[Color], X + ED_FRAMERMARGIN - ED_SCROLLINACTIVEWIDTH,
			Y, ED_SCROLLINACTIVEWIDTH, PaneP->ScrollThumb);
    }     
}

// ******************************************************************************
// ED_PaneScrollWinHandleEvent is the callback function for the XWinScroll window.
// In addition to the obvious, it handles a number of tricks unique to Scroll bars.
//
// In the case of mouse wheel activation (XLib maps them to Button4 and Button5),
// this function is called DIRECTLY by FrameHandleClick IFF the mouse was on
// the Pane proper.  But this function is called (dispatched) in the usual way if
// the mouse was actually on the Scroll bar window.  In the former case, the
// xbutton.x and y coordinates will be relative to the Pane's main window.
//
// The user can click on the actual Scroll Thumb to drag it around, but can also
// click above/below it to have it jump to those locations before being grabbed.
//
// GrabY is a static, stashes the last (virtual) mouse/thumb position, so the thumb
// re-latches on the mouse pointer as the user moves too far up and down.  When
// clicking on Top/Bot areas (instead of the Thumb) GrabY is set to a virtual Mid-Thumb
// location to handle the initial jump.

void	ED_PaneScrollWinHandleEvent(XEvent * EventP, ED_PanePointer PaneP)
{
    static Int32	GrabY;

    ED_CmdLastId = ED_ScrollId;
    switch (EventP->type) {
	case MotionNotify:
	    // Flush extra ButtonMotion events, just look at last.
	    while (XCheckMaskEvent(ED_XDP, ButtonMotionMask, EventP));
	    if (PaneP->Flags & ED_PANESCROLLINGFLAG) {
		if (EventP->xmotion.y != GrabY)
		    GrabY += ED_PaneScrollByThumb(PaneP, EventP->xmotion.y - GrabY);
	    }
	    break;

	case EnterNotify:
	    if (PaneP->Flags & ED_PANESCROLLINGFLAG) break;
	    ED_PaneDrawScrollBar(PaneP, 1);
	    break;
	    
	case LeaveNotify:
	    if (PaneP->Flags & ED_PANESCROLLINGFLAG) break;
	    ED_PaneDrawScrollBar(PaneP, 0);
	    break;

	case ButtonPress:

	    // Turn off QREP/ISearch if active... but NOT QR!
	    {	Int16		ClearML = 1;
	    
		if (ED_QREPPaneP) ED_QREPAbortOut(ED_STR_EchoAbort), ClearML = 0;
		if (ED_ISPaneP) ED_ISAbortOut(ED_STR_EchoAbort), ClearML = 0;
		if (ED_QRPaneP) ClearML = 0;

		if (ClearML) {
		    ED_FrameEchoLen = 0;
		    ED_FrameDrawEchoLine(PaneP->FrameP);
		}
	    }
	
	    if (EventP->xbutton.button == Button4)
		ED_PaneScrollByRow(PaneP, EventP, - ED_SCROLLWHEELSTEP);
	    else if (EventP->xbutton.button == Button5)
		ED_PaneScrollByRow(PaneP, EventP, ED_SCROLLWHEELSTEP);

	    if (EventP->xbutton.button != Button1) {	// Another Button!
		XUngrabPointer(ED_XDP, CurrentTime);	// Undo automatic grab
		break;
	    }

	    // Grabbing the ScrollThumb...
	    // 1) Clicked on the thumb:	GrabY saves start location, Motion Notify handles movement
	    PaneP->Flags |= ED_PANESCROLLINGFLAG;
	    GrabY = EventP->xbutton.y;

	    // 2) Clicked ABOVE thumb:	Pretend clicked mid-thumb, then moved to loc with one motion.
	    // 3) Clicked BELOW thumb:	Ditto.	    
	    if ((GrabY < PaneP->ScrollTop) || (GrabY > PaneP->ScrollTop + PaneP->ScrollThumb)) {

		GrabY = PaneP->ScrollTop + (PaneP->ScrollThumb / 2);
		GrabY += ED_PaneScrollByThumb(PaneP, EventP->xbutton.y - GrabY);
	    } 
	    break;

	case ButtonRelease:
	    if (EventP->xbutton.button == Button1)
		PaneP->Flags &= ~ (ED_PANESCROLLINGFLAG);
	    break;

	default: break;	    
    }
}

// ******************************************************************************
// ED_PaneScrollByThumb handles scrolling when the Thumb is moved.  First handle
// the pixel position of the Thumb, then map this onto Pane Row positions.
//
// Given an input Delta +/- (mouse) movement of the Thumb, the function returns
// the proper output Delta for the Thumb--limits the range.
//
// (Delta > 0) ->	Text should scroll UP, so page should start on NEXT rows.
//			Cursor may end up ABOVE the Pane, bring back to top.
// (Delta < 0) -->	Text should scroll Down, so page should start on PREV rows.
//			Cursor may end up BELOW the Pane, bring back to bottom.

Int32	ED_PaneScrollByThumb(ED_PanePointer PaneP, Int32 Delta)
{
    Int32	MaxLen, StartRow, DeltaRow;

    if (Delta < 0) {
	if ((PaneP->ScrollTop + Delta) < 0)
	    Delta = - PaneP->ScrollTop;
    } else {
	if ((PaneP->ScrollTop + PaneP->ScrollThumb + Delta) > PaneP->ScrollHeight)
	    Delta = PaneP->ScrollHeight - (PaneP->ScrollTop + PaneP->ScrollThumb);
    }

    if (Delta == 0) return 0;
    PaneP->ScrollTop += Delta;
    
    // When ScrollTop is 0, StartRow will always be 0.  But 32.32 multiplication
    // can miss the max value by 1... so adjust.
    StartRow = (PaneP->ScrollScale * PaneP->ScrollTop) >> 32;
    if (PaneP->ScrollTop + PaneP->ScrollThumb == PaneP->ScrollHeight)
	StartRow = PaneP->BufRowCount;

    MaxLen = PaneP->FrameP->RowChars;
    DeltaRow = StartRow - PaneP->StartRowCount;
    PaneP->StartRowCount = StartRow;
    if (DeltaRow < 0) {
	DeltaRow = -DeltaRow;
	    PaneP->PanePos = ED_BufferGetPosMinusRows(PaneP->BufP, PaneP->PanePos, &DeltaRow, MaxLen);
	DeltaRow = -DeltaRow;
	ED_PaneMoveCursorAfterScroll(PaneP, DeltaRow);

    } else if (DeltaRow > 0) {
	PaneP->PanePos = ED_BufferGetPosPlusRows(PaneP->BufP, PaneP->PanePos, &DeltaRow, MaxLen);
	ED_PaneMoveCursorAfterScroll(PaneP, DeltaRow);
    }

    ED_PaneDrawScrollBar(PaneP, 1);
    ED_PaneDrawText(PaneP);
    ED_PaneDrawBlinker(PaneP);
    sc_BlinkTimerReset();
    return Delta;
}

// ******************************************************************************
// ED_PaneScrollByRow handles non-thumb scrolling, as in the mouse wheel.  Where
// ED_PaneScrollByThumb specifies a pixel-based Delta, this function uses a row-
// based DeltaRow.
//
// The EventP arg could target the Pane window *or* the XScrollWin.  In the former
// case, the XEvent originally went to the Pane, but was re-directed here.  So
// the X,Y coords of xbutton could be relative to FrameP->XWin or PaneP->XScrollWin.
// Must differentiate the origin (OnScrollBar) so can draw full or thin scroll thumb.

void	ED_PaneScrollByRow(ED_PanePointer PaneP, XEvent * EventP, Int32 DeltaRow)
{
    Int32	MaxLen;
    Int16	OnScrollBar;

    MaxLen = PaneP->FrameP->RowChars;
    OnScrollBar = (EventP->xbutton.window == PaneP->XScrollWin) && (EventP->xbutton.x <= ED_FRAMERMARGIN);

    if (DeltaRow < 0) {
	if (PaneP->StartRowCount + DeltaRow < 0)
	    DeltaRow = - PaneP->StartRowCount;
		
	DeltaRow = -DeltaRow;
	    PaneP->PanePos = ED_BufferGetPosMinusRows(PaneP->BufP, PaneP->PanePos, &DeltaRow, MaxLen);
	DeltaRow = -DeltaRow;
	ED_PaneMoveCursorAfterScroll(PaneP, DeltaRow);

    } else {
	if (PaneP->StartRowCount + DeltaRow > PaneP->BufRowCount)
	    DeltaRow = PaneP->BufRowCount - PaneP->StartRowCount;
	if (DeltaRow) {
	    PaneP->PanePos = ED_BufferGetPosPlusRows(PaneP->BufP, PaneP->PanePos, &DeltaRow, MaxLen);
	    ED_PaneMoveCursorAfterScroll(PaneP, DeltaRow);
        }
    }
    
    PaneP->StartRowCount += DeltaRow;
    
    ED_PaneSetScrollBar(PaneP);
    ED_PaneDrawText(PaneP);
    ED_PaneDrawScrollBar(PaneP, OnScrollBar);		// Draw full thumb if OnScrollBar
    
    ED_PaneDrawBlinker(PaneP);
    sc_BlinkTimerReset();
}

// ******************************************************************************
// ED_PaneMoveCursorAfterScroll moves the cursor up/down to keep the blinker
// on the exact character it used to be on before the scrolling.  It will move
// the Cursor to the very first position in the Pane if it moves off the top or
// to the beginning of the last line if it moves off the bottom.
//
// The begining of the first line in the Pane has a known Buffer Pos.  But the
// beginning of the last line does NOT... so ED_PaneFindLoc returns that Pos
// when PaneLimit is 1 and CursorRow will be greater than the Pane limit.
//
// This function will also move the SelMark to keep it on the same relative
// position when the Pane scrolls.  But no attempt is made to keep it visible.
//
// NOTE:	Sel range is de-selected *IFF* the cursor has to be moved
//		to keep it on the Pane.  This movement will alter the selection
//		range, so better to remove the range than change it without the
//		user being explicitly aware.

void	ED_PaneMoveCursorAfterScroll(ED_PanePointer PaneP, Int32 DeltaRow)
{
    Int16	CancelSel = 0;
    Int32	LastPos;

    if (ED_SelPaneP == PaneP)
	ED_SelMarkRow -= DeltaRow;	// Adjust, ok if off pane now!

    PaneP->CursorRow -= DeltaRow;	// Adjust, but maybe off Pane now!
    if (DeltaRow < 0) {
	if (PaneP->CursorPos && (PaneP->CursorRow > PaneP->RowCount - 2)) {
	    LastPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 1);
	    if (PaneP->CursorRow == -1) {
		PaneP->CursorPos = LastPos;
		PaneP->CursorRow = PaneP->RowCount - 2;
		PaneP->CursorCol = 0;
	    }

	    if (ED_SelPaneP == PaneP) CancelSel = 1;
	}
    } else if (DeltaRow > 0) {

	if (PaneP->CursorRow < 0) {
	    PaneP->CursorPos = PaneP->PanePos;
	    PaneP->CursorRow = 0;
	    PaneP->CursorCol = 0;
	    if (ED_SelPaneP == PaneP) CancelSel = 1;
	}
    }

    if (CancelSel) {
	ED_SelPaneP = NULL;
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoSelCancel);
	ED_FrameDrawEchoLine(PaneP->FrameP);
    }
}

// ******************************************************************************
// ED_PaneMoveAfterCursorMove is called to re-position the Pane (scroll it) when
// the Cursor is moved to a new location that is *possibly* off the Pane.  The
// caller first calls ED_PaneFindLoc to get the (Col,Row) for the cursor (along
// with CursorRowStartPos), then calls this function to re-position the pane so
// the Cursor is DeltaRow away from the edge (top or bottom).
//
// NOTE:	MUST check that Cursor is OFF pane before re-positioning.
// NOTE:	CursorRowStartPos < 0, means "compute it youself".
// NOTE:	ForceSetScroll *ONLY* if the actual buffer contents has changed,
//		otherwise, the ScrollBars will get updated IFF PaneMoveForCursor
//		is called.

void	ED_PaneMoveAfterCursorMove(ED_PanePointer PaneP, Int32 CursorRowStartPos, Int32 DeltaRow, Int16 ForceSetScroll)
{
    if (PaneP->CursorRow > (PaneP->RowCount - 2))
	DeltaRow = (PaneP->RowCount - 2) - DeltaRow;
    else if (PaneP->CursorRow >= 0) {
	if (ForceSetScroll) ED_PaneSetScrollBar(PaneP);		// *MUST* set ScrollBar
	return;							// Nothing else to do!
    }
    // Cursor is OFF the bottom or top if it gets here.
    ED_PaneMoveForCursor(PaneP, CursorRowStartPos, DeltaRow);
}

// ******************************************************************************
// ED_PaneMoveForCursor will move/reposition the Pane so the Cursor appears on
// NewCursorRow of the Pane.  It does not matter to this function whether the
// Cursor was originally On or Off the Pane.
//
// This function definitely MOVES the Pane--unless PaneP->CursorRow was == NewCursorRow.

void	ED_PaneMoveForCursor(ED_PanePointer PaneP, Int32 CursorRowStartPos, Int32 NewCursorRow)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		MaxLen = PaneP->FrameP->RowChars;

    if (CursorRowStartPos < 0)
	CursorRowStartPos = ED_BufferGetRowStartPos(BufP, PaneP->CursorPos, PaneP->CursorCol);

    PaneP->PanePos = CursorRowStartPos;			// Position Pane at Cursor
    PaneP->StartRowCount += PaneP->CursorRow;
    PaneP->PanePos = ED_BufferGetPosMinusRows(BufP, PaneP->PanePos, &NewCursorRow, MaxLen);
    PaneP->StartRowCount -= NewCursorRow;
    
    if (ED_SelPaneP == PaneP) ED_SelMarkRow -= PaneP->CursorRow - NewCursorRow;
    PaneP->CursorRow -= PaneP->CursorRow - NewCursorRow;
    
    ED_PaneSetScrollBar(PaneP);
}

// ******************************************************************************
// ED_PaneHandleClick is called when the user clicks in the given Pane.  In most
// cases, this click will a) select a new Pane or b) re-position the Cursor.
// The user may also "drag" the cursor to select a region, and this becomes a
// complicated process.
//
// NOTE:	If Shift+Click, SelMark is previous Cursor position... and cursor
//		can be dragged around to establish new Cursor position and end of
//		SelRange.  If normal click, SelMark is the location of the click,
//		and the cursor can be dragged around to also establish new Cursor
//		position and end of SelRange.  Both may end up with NO SELrange,
//		if the cursor is dragged to its old position!  Only ED_PaneHandleRelease
//		will know...  If there *IS* a real Sel, then SelMark is recorded
//		on the MarkRing.

void	ED_PaneHandleClick(ED_PanePointer PaneP, Int32 Row, Int32 Col, Int16 Shift, Int16 Count)
{
    ED_FramePointer	FP = PaneP->FrameP;

    ED_CmdLastId = ED_SelectId;	// Assume this for now

    // Make CurPane--can be over ModeLine of bottom Pane--and get rid of Sel hilites.
    if (PaneP != FP->CurPaneP) {
	ED_PanePointer	OldPaneP = FP->CurPaneP;
	if (ED_SelPaneP) {			// Could only be FP->CurPaneP
	    ED_SelPaneP = NULL;			// Deselect
	    ED_PaneDrawText(FP->CurPaneP);	// Redraw to erase Sel hilite
	}
	FP->CurPaneP = PaneP;
	ED_PaneDrawText(OldPaneP);
	ED_PaneDrawBlinker(OldPaneP);
    }

    if ((Count > 1) && (Row < PaneP->RowCount - 1)) {
	if (Count == 2) {			// Double click
	    ED_CmdSelectArea(PaneP);		// Select Word/Blank/Identical
	    return;
	} else if (Count == 3) {		// Triple click
	    // The preceding DOUBLE click, may move the Cursor!!  So figure out
	    // where it should be before CmdSelectLine.
	    ED_PaneFindPos(PaneP, &PaneP->CursorPos, &Row, &Col, 1, 1);
	    PaneP->CursorRow = Row;
	    PaneP->CursorCol = Col;
	    ED_CmdSelectLine(PaneP);		// Select line
	    return;
	}
    }
    
    // Set Cursor position if NOT over
    if (Row < PaneP->RowCount - 1) {
	ED_CmdLastId = ED_CursorPosId;
	if (Shift) {
	    // SelRange is from old CursorPos to the new one... 
	    ED_SelMarkPos = PaneP->CursorPos;
	    ED_SelMarkRow = PaneP->CursorRow;
	    ED_SelMarkCol = PaneP->CursorCol;
	}

	// Find the Buffer Pos, Row and Col are adjusted if clicked over
	// empty space--past end of line or end of buffer.
	ED_PaneFindPos(PaneP, &PaneP->CursorPos, &Row, &Col, 1, 1);
	PaneP->CursorRow = Row;
	PaneP->CursorCol = Col;

	ED_SelPaneP = PaneP;				// Selecting!
	ED_SelLastRow = ED_SelLastCol = 0x7FFFFFFF;	// Reset
	if (! Shift) {
	    // May end up as a Drag, or not... Treat SelMark as new CursorPos
	    ED_SelMarkPos = PaneP->CursorPos;
	    ED_SelMarkRow = Row;
	    ED_SelMarkCol = Col;
	}
    }

    ED_PaneDrawText(PaneP);
    ED_PaneDrawBlinker(PaneP);

    // Done, except for dragging to select a region...
    ED_PaneHandleDrag(PaneP);
}

// ******************************************************************************
// ED_PaneHandleDrag deals with the user dragging the mouse, while Button1 is
// pressed, to select a region (Sel).  In the simple case, the user drags within
// the displayed Pane.  But can also drag *OUTSIDE* the Pane to auto-scroll, in
// order to select additional rows off the top/bottom of the Pane.
//
// NOTE:	Auto-scrolling should *CONTINUE* as the mouse is held stationary
//		outside the Pane, expanding the Sel area... this is in contrast
//		with selecting inside the Pane.
//
// NOTE:	Even if PointerMotionHint mask is set in window, each call to
//		XQueryPointer generates a Motion event for the window.  The only
//		way around it is to simply NOT have ButtonMotion or PointerMotion
//		events for the window.  Since auto-scrolling must continue if
//		the mouse is stationary, the program *HAS* to use XQueryPointer.
//		So easier to use XQueryPointer and manually track the mouse and
//		forego Motion events.
//
// NOTE:	Row & Col extraction *MUST* match algorithm in ED_FrameHandleClick.
//		So use same Macros!

void	ED_PaneHandleDrag(ED_PanePointer PaneP)
{
    ED_FramePointer	FP = PaneP->FrameP;
    Window		RootW, ChildW;
    Int32		Row, Col, RootX, RootY, X, Y;
    Uns32		MouseMask;

    while (1) {
	XQueryPointer(ED_XDP, FP->XWin, &RootW, &ChildW, &RootX, &RootY, &X, &Y, &MouseMask);
	if (! (MouseMask & Button1Mask)) break;

	Row = ED_EVENTGETROW(FP, Y) - PaneP->TopRow;
	Col = ED_EVENTGETCOL(FP, X);
	ED_PaneHandleDragMotion(PaneP, Row, Col);
    }
}

// ******************************************************************************
// ED_PaneHandleRelease is called when Mouse1 button is released.  Its only job
// is to reset ED_SelPaneP if no region was selected.

void	ED_PaneHandleRelease(ED_PanePointer PaneP)
{
    if (ED_SelPaneP) {
	if (ED_SelMarkPos == PaneP->CursorPos)
	    ED_SelPaneP = NULL;
	else {
	    ED_BufferPushMark(PaneP->BufP, ED_SelMarkPos);
	    ED_XSelSetPrimary(PaneP);	    
	}
    }	    
}

// ******************************************************************************
// ED_PaneInsertPrimary is called to insert the PRIMARY selection at CursorPos.

void	ED_PaneInsertPrimary(ED_PanePointer PaneP)
{
    char	*MemP;
    Int32	Len;

    if (ED_BufferReadOnly(PaneP->BufP)) return;

    if (ED_PrimaryOwn) {
	if (ED_PrimaryBufP) {

	    // PRIMARY is in a Buffer.  Set Gap to get it out of the way.
	    // It is possible (and probable) that Insertion position + PrimaryPos
	    // are disjoint... but defensive programming to copy to a malloc
	    // buffer before insertion--in case it is same buffer, something moves, etc.

	    MemP = malloc(ED_PrimaryLen);
	    if (MemP == NULL) {
		ED_FrameSetEchoError(ED_STR_EchoOutOfMem, errno);
		ED_FrameDrawEchoLine(PaneP->FrameP);
		return;
	    }
	    ED_BufferPlaceGap(ED_PrimaryBufP, ED_PrimaryPos, 0);
	    memcpy(MemP, ED_PrimaryBufP->GapEndP, ED_PrimaryLen);
		ED_PaneInsertChars(PaneP, ED_PrimaryLen, MemP, 0);
	    free(MemP);

	} else {
	    ED_KillRingYank(0, &MemP, &Len, 0);
	    ED_PaneInsertChars(PaneP, Len, MemP, 0);
	}

    } else 
	ED_XSelInsertData(PaneP, XA_PRIMARY);
}

// ******************************************************************************
// ED_PaneHandleDragMotion is called as the user drags the mouse while Button1 is pressed.
// The main tasks are: a) select the dragged region and b) auto-scroll if outside
// the Pane.
//
// ED_PaneHandleDrag will call this function in a tight loop, again and again:
// 1)	IFF dragging inside the Pane, DO NOTHING if (Col,Row) is the same as last
//	call (stash them in (ED_SelLastCol, ED_SelLastRow).
// 2)	IFF dragging Outside the Pane, i) Auto-scroll, even if the mouse is
//	stationary and ii) take a pause before returning to slow down the scrolling.


void	ED_PaneHandleDragMotion(ED_PanePointer PaneP, Int32 Row, Int32 Col)
{
    Int32		R;
    struct pollfd	PollR;
    Int16		OffPane = 0;
    
    if (ED_SelPaneP != PaneP) return;
    if ((Row == ED_SelLastRow) && (Col == ED_SelLastCol)) return;
    
    if ((PaneP->CursorRow != Row) || (PaneP->CursorCol != Col)) {
    	ED_SelLastRow = Row;
	ED_SelLastCol = Col;
	
	if (Row < 0) {					// Auto-scroll UP
	    OffPane = 1; R = 1;
	    if (PaneP->PanePos) {			// Can still scroll some more
		PaneP->PanePos = ED_BufferGetPosMinusRows(PaneP->BufP, PaneP->PanePos, &R, PaneP->FrameP->RowChars);
		PaneP->StartRowCount -= R;
		ED_SelMarkRow += R;
	    } else					// Cannot scroll, just go to very beginning
		Col = 0;			
	    Row = 0;
	} else if (Row >= (PaneP->RowCount - 1)) {	// Auto-scroll DOWN
	    OffPane = 1; R = 1;
	    if (PaneP->CursorPos < (PaneP->BufP->LastPos)) {
		PaneP->PanePos = ED_BufferGetPosPlusRows(PaneP->BufP, PaneP->PanePos, &R, PaneP->FrameP->RowChars);
		PaneP->StartRowCount += R;
		ED_SelMarkRow -= R;
	    } else					// Already at bottom
		Col = PaneP->FrameP->RowChars - 1;	// Rightmost Col--until PaneSetCursorLoc
	    Row = PaneP->RowCount - 2;			// Just above the ModeLine
	}

	// Find the real cursor position and location.. has to be on text, not off on blank space
	PaneP->CursorCol = Col;
	PaneP->CursorRow = Row;

	ED_RANGELIMIT(PaneP->CursorRow, 0, PaneP->RowCount - 2);
	ED_PaneFindPos(PaneP, &PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 1, 0);

	// Scroll bars change IFF auto-scrolling
	if (OffPane) {
	    ED_PaneSetScrollBar(PaneP);
	    ED_PaneDrawScrollBar(PaneP, 0);
	}
	ED_PaneDrawText(PaneP);
	ED_PaneDrawBlinker(PaneP);
	sc_BlinkTimerReset();

	// If OffPane, then AutoScrolling, so slow down before returning.
	// Use Poll, so ButtonUp or other events kick out immediately.
	// Set ED_SelLastRow/Col to impossible values, so we process the
	// next call, even if mouse is stationary OFF PANE.
	if (OffPane) {
	    PollR.fd = ConnectionNumber(ED_XDP);
	    PollR.events = POLLIN;
	    poll(&PollR, 1, 50);
	    ED_SelLastRow = ED_SelLastCol = 0x7FFFFFFF;
	}
    }

    return;
}

// ******************************************************************************
// ED_PaneFindLoc is the workhorse function for locating an arbitrary point,
// wheather a CursorPos, an old Mark or some other stashed Pos.  Given Pos, it will
// find the Row and Col values.  The return value is the char position for
// the beginning of the line that contains the Pos arg.
//
// NOTE:	If FromZero, it will always start from the very first byte
//		of the Buffer... otherwise, it *might* start at PanePos.
//		*RowP will get an ABSOLUTE count if FromZero is set--otherwise
//		row count is Pane-relative.
//
// NOTE:	If PaneLimit, it *WILL NOT* look for Pos past the last row of
//		the Pane.  Instead, it will simply set *Row/*Col to -1 and
//		return the POS for the beginning of the last row of the Pane.
//		(This is used to re-locate the Cursor when scrolling it off
//		the pane... in must circumstances, it is best to Scroll the Pane
//		to make the Cursor visible, not *MOVE* the cursor.)
//
// NOTE:	If using "break" instead of 'goto FoundIt', cannot differentiate
//		boundary case when hit Gap, Pos was incremented, but CurCol was not!
//
// NOTE:	Extra "CorCol += 1" will handle case where target is not found and
//		there is no /n at the end of buffer.

Int32	ED_PaneFindLoc(ED_PanePointer PaneP, Int32 Pos, Int32 *RowP, Int32 *ColP, Int16 FromZero, Int16 PaneLimit)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    char		*CurP, *EndP;
    Int32		CurRow, CurCol, ColLimit, CurPos, RowStartPos, FinalPos, LastRow;
    Int16		L;

    // Internally, CurRow is *ABSOLUTE*, start from Pos 0 or PaneP->PanePos?
    CurPos = PaneP->PanePos;
    CurRow = PaneP->StartRowCount;
    CurCol = 0;
    if (FromZero || (Pos < PaneP->PanePos)) {
	CurPos = 0;
	CurRow = 0;
    } 

    ColLimit = PaneP->FrameP->RowChars;
    RowStartPos = CurPos;					// 0 or PaneStart, either way, new Row!
    LastRow = PaneP->StartRowCount + PaneP->RowCount  - 2;	// ABSOLUTE value for it
    
    EndP = BufP->GapStartP;
    CurP = BufP->BufStartP + CurPos;
    if (CurP >= BufP->GapStartP) {
	CurP += (BufP->GapEndP - BufP->GapStartP);
	EndP = BufP->BufEndP;
    }

DoRest:
    while (CurP < EndP) {
    
	CurCol++;					// CurCol is **1-Based**
	if (*CurP == '\n') {				// Hard end of line, *CAN* be in OverFlow col
	    if (CurPos == Pos) goto FoundIt;		// Found it, done!!
	    CurRow++, CurCol = 0;			// (**) May exit loop with 0 CurCol !!
	    FinalPos = RowStartPos;			// Lags 1 line behind.
	    RowStartPos = CurPos + 1;			// Next char will be beginning of line
	}
	
        if (CurCol > ColLimit) {			// Wrap around, will be 1 on next row
	    CurRow++, CurCol = 1;
	    FinalPos = RowStartPos;
	    RowStartPos = CurPos;			// THIS char will be beginning of line
	}

	if (PaneLimit && (CurRow > LastRow)) {		// 1 Past last row, so exit if PaneLimit
	    *RowP = -1, *ColP = -1;
	    return FinalPos;				// RowStartPos for LAST row!
	}
	
	if (CurPos == Pos) goto FoundIt;		// Found it, *NOT* in OverFlow col

	L = ED_BufferGetUTF8Len(*CurP);			// Advance to next char
	CurP += L, CurPos += L;
     }

     if (EndP < BufP->BufEndP) {
	EndP = BufP->BufEndP;
	CurP = BufP->GapEndP;
	goto DoRest;
     }
     CurCol += 1;					// End of Buffer and still NOT FoundIt

FoundIt:
     if (CurCol) CurCol--;				// Make it 0-based
     if (! FromZero) CurRow -= PaneP->StartRowCount;	// Make it Pane-relative
     *RowP = CurRow;
     *ColP = CurCol;
     return RowStartPos;				// Returns matching Row start!
}

// ******************************************************************************
// ED_PaneSetCursorLoc is called to position the Cursor based on a mouse click
// somewhere on the Pane.  Only PaneP->PanePos is known (on Row 0, Col 0), so
// the function goes through the entire visible portion of the Pane to find where
// the linebreaks/linewraps are, up to the row that was clicked.
//
// On entry, *RowP and *ColP indicate the location of the initiating mouse click.
// On exit, *PosP, *RowP, and *ColP indicate where the Cursor should be.
//
// The user can click on empty space, the Cursor is placed on the logically
// closest valid postion (on Text)--generally the same Row:
// Case 1:	Exact hit on Text	->	set Cursor there.
// Case 2:	Row hit, Col too short	->	Select end of Row instead.
// Case 3:	Not enough Rows		->	Select end of last Row.
//
// NOTE:	There is a Cursor overflow column on the right of the Pane.  The
//		Cursor *CAN* go there IFF the line ends (/n) at the very edge
//		of the Pane with no wrap-around--since /n chars are NOT visible.
//
//		OFF-BOTTOM case:  If the line wraps (it is long), a click on
//		the Overflow column will place the Cursor on the first position
//		of the FOLLOWING line.  If the user does this on the bottom
//		row of the Pane, the cursor will want to fall off the bottom.
//		Instead, it is placed on the last position of the current Row.
//
// NOTE:	The column count (CurCol) is base-1 internal to this (and other
//		related) functions... this makes the code simpler.  The column
//		count is always 0-based externally (*ColP).
//
// NOTE:	ED_PaneSetCusorLoc and ED_PaneGetCursorLoc (as well as other drawing
//		and scrolling functions) ***MUST*** be in logical synchrony.  Do
//		not alter one without the others.


Int32	ED_PaneFindPos(ED_PanePointer PaneP, Int32 *PosP, Int32 *RowP, Int32 *ColP, Int16 FromPane, Int16 FixOffBottom)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    char		*CurP, *EndP;
    Int32		CurRow, CurCol, ColLimit, CurPos, RowStartPos;
    Int16		L;

    *RowP += PaneP->StartRowCount;			// Internally in ABSOLUTE count
    
    ColLimit = PaneP->FrameP->RowChars;
    CurRow = CurCol = CurPos = 0;
    if (FromPane || (*RowP >= PaneP->StartRowCount)) {
	CurRow = PaneP->StartRowCount;
	CurPos = PaneP->PanePos;
    }
    RowStartPos = CurPos;

    EndP = BufP->GapStartP;
    CurP = BufP->BufStartP + CurPos;
    if (CurP >= BufP->GapStartP) {
	CurP += (BufP->GapEndP - BufP->GapStartP);
	EndP = BufP->BufEndP;
    }
    *ColP += 1;						// Make 1-based for here

    if (FixOffBottom &&					// Avoid OFF-BOTTOM problem!
	(*ColP > ColLimit) &&
	(*RowP == (PaneP->RowCount + PaneP->StartRowCount - 2)))
	*ColP = ColLimit;

DoRest:
    while (CurP < EndP) {

	CurCol++;					// CurCol is **1-Based**
	if (*CurP == '\n') {
	    if (CurRow == *RowP) goto FoundIt;		// Hard EOL, *CAN* be in Overflow COl
	    CurRow++, CurCol = 0;			// Next char will be Col 0
	    RowStartPos = CurPos + 1;			// Next char will be Begin of Line
	}

	if (CurCol > ColLimit) {			// Wrap around
	    CurRow++, CurCol = 1;
	    RowStartPos = CurPos;			// THIS char is begin of line
	}

	if ((CurRow > *RowP) ||				// Just went one Row past it! OR
	    ((CurRow == *RowP) && (CurCol == *ColP)))	// EXACT HIT!!
	    goto FoundIt;			

        L = ED_BufferGetUTF8Len(*CurP);			// Advance
	CurP += L, CurPos += L;
	if (CurPos > BufP->LastPos) goto FoundIt;	// No more chars (Gap can be at end!) (>= before)
    }

    if ((CurPos <= BufP->LastPos) &&			// Iterate for buffer AFTER gap.  (< before)
	(EndP < BufP->BufEndP)) {
	EndP = BufP->BufEndP;
	CurP = BufP->GapEndP;
	goto DoRest;
    }
    CurCol += 1;					// LastPos reached and still NOT FoundIt.

    // Just having a "break" in the loop above is not sufficient.  Must differentiate
    // exit between "found it" and "CurP hit EndP"... the latter requiring more iteration.

FoundIt:
    *PosP = CurPos;
    if (CurCol) CurCol--;				// Make it 0-based for export
    *RowP = CurRow - PaneP->StartRowCount;		// Make it Pane-relative to return.
    *ColP = CurCol;
    return RowStartPos;
}

// ******************************************************************************
// ED_PaneDelSelRangeis called to delete text within the Sel range... if there
// is a SelRange.
//
// Return 0 if no SelRange and N if N characters were zapped.
// DoUpdate is optional, since caller may wish to do additional work, i.e.
// add in replacement chars, before updating the screen.
//
// This function *WILL* return all Pane/Buffer data structures to consistent
// and correct state before returning--even if no DoUpdate.
//
// NOTE:	If not DoUpdate, Cursor MAY BE OFF SCREEN if zapping a
//		SelRange stat started above the Pane.
//
// NOTE:	SelMarkPos *MAY* be far above PanePos... so *MUST* use
//		FromZero when calling PaneFindLoc!
//
// NOTE:	IFF ED_CmdThisId is ED_KillId, then the data to
//		be deleted will be added to the KillRing.  This function is
//		usually called by routines that simply delete + discard text,
//		instead	of "kill" routines that save to the KillRing.

Int32	ED_PaneDelSelRange(ED_PanePointer PaneP, Int16 DoUpdate)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		StartPos, EndPos, Total, Col;

    if ((ED_SelPaneP != PaneP) || (ED_SelMarkPos == PaneP->CursorPos))
	return 0;

    BufP->Flags |= ED_BUFMODFLAG;
    if (PaneP->CursorPos < ED_SelMarkPos) {
	StartPos = PaneP->CursorPos;
	EndPos = ED_SelMarkPos;
    } else {
	StartPos = ED_SelMarkPos;
	EndPos = PaneP->CursorPos;
	PaneP->CursorPos = ED_SelMarkPos;
	PaneP->CursorRow = ED_SelMarkRow;
	PaneP->CursorCol = ED_SelMarkCol;
    }

    ED_SelPaneP = NULL;
    Total = EndPos - StartPos;
    ED_BufferPlaceGap(BufP, StartPos, 0);
    
    ED_BufferAddUndoBlock(BufP, StartPos, Total, ED_UB_DEL | ED_UB_CHUNK, BufP->GapEndP);
    if (ED_CmdThisId == ED_KillId) ED_KillRingAdd(BufP->GapEndP, Total, 1);
    
    BufP->GapEndP += Total;
    BufP->LastPos -= Total;

#ifdef DEBUG
	// TEST_FillGap(BufP);
#endif

    ED_PaneFindLoc(PaneP, BufP->LastPos, &PaneP->BufRowCount, &Col, 1, 0);
    ED_BufferUpdateMark(BufP, StartPos, -Total);

    if (DoUpdate) {
	if (PaneP->CursorPos == ED_SelMarkPos)			// Can only be if Cursor was MOVED
	    ED_PaneMoveAfterCursorMove(PaneP, -1, 2, 1);	// Force PaneSetScrollBar
	else
	    ED_PaneSetScrollBar(PaneP);
	    
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);
	ED_FrameDrawBlinker(PaneP->FrameP);
	ED_PaneUpdateOtherPanes(PaneP, PaneP->CursorPos, - Total);
	
	ED_FrameDrawEchoLine(PaneP->FrameP);
	sc_BlinkTimerReset();
    }
    
    return Total;
}

// ******************************************************************************
// ED_PaneShowOpenParen will hilite the matching open paren (paren/bracket/curly).
// If the match is off the top, simply show the relevent row in the Echo area.

void	ED_AuxOpenParenHilite(ED_PanePointer PaneP, char C, Int32 Row, Int32 Col)
{
    ED_FramePointer	FP = PaneP->FrameP;
    Int32		X, Y;
    struct pollfd	PollR;

    X = FP->FrameX + (Col * ED_Advance);
    Y = FP->FrameY + ((PaneP->TopRow + Row) * ED_Row);

    PollR.fd = ConnectionNumber(ED_XDP);	// Negate to ignore all events
    PollR.events = POLLIN;
    XftDrawRect(FP->XftDP, &ED_XCArr[ED_Turquoise], X, Y, ED_Advance, ED_Height + 1);
    XftDrawString8(FP->XftDP, &ED_XCArr[ED_Black], ED_XFP, X, Y + ED_Ascent, (XftChar8 *)&C, 1);
    XFlush(ED_XDP);
    poll(&PollR, 1, 1000);			// 1000 mSec !!

    // ED_PaneDrawText will erase the hiliting, but easier to just do it in place.
    XftDrawRect(FP->XftDP, &ED_XCArr[ED_White], X, Y, ED_Advance, ED_Height + 1);
    XftDrawString8(FP->XftDP, &ED_XCArr[ED_Black], ED_XFP, X, Y + ED_Ascent, (XftChar8 *)&C, 1);
}

void	ED_PaneShowOpenParen(ED_PanePointer PaneP, char CloseC, Int32 ParenPos)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    char		OpenC, *CurP;
    char		Msg[ED_MSGSTRLEN];
    Int32		CurPos, PCount, Row, Col, MsgLen;

    if (CloseC == ')')
	OpenC = CloseC - 1;	// '(' + 1 == ')'
    else
	OpenC = CloseC - 2;	// '[' + 2 == ']' and '{' + 2 == '}'

    CurPos = ParenPos;
    PCount = 1;

    while (CurPos) {
	CurP = ED_BufferPosToPtr(BufP, --CurPos);
	while (ED_BUFFERISMIDUTF8(*CurP)) CurP--, CurPos--;

	if (*CurP == CloseC) PCount++;
	else if (*CurP == OpenC) PCount--;

	if (PCount == 0) break;
    }

    // Fount the matching Paren at Pos?  Row is Pane-relative!
    if (PCount == 0) {
	ED_PaneFindLoc(PaneP, CurPos, &Row, &Col, 0, 0);

	if (Row < 0) {
	    // Off the screen, just Echo the line.
	    // Not so simple, because a single paren on a line conveys very
	    // little information.  So must show data on next line... but
	    // have only 1 Echo line, so must convert '\n' to ' ' instead!

	    MsgLen = ED_MSGSTRLEN - strlen(ED_STR_EchoParenMatch);
	    // Possible (but unlikely) that (Col > MsgLen)... so ignore!
	    CurPos = ED_BufferGetRowStartPos(BufP, CurPos, Col);
	    ED_BufferFillStr(BufP, Msg, CurPos, MsgLen, 1);
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoParenMatch, MsgLen, Msg);

	} else {
	    // The matching paren is visible!  Highlight it!
	    // Overdrawing does NOT work well with anti-aliased fonts.
	    // Better to simply draw a background color, then draw the
	    // OpenC on top of it... to erase, just redraw as normal.
	    //
	    // Unlike RealEmacs, we only ParenHilite *THIS* PaneP.
	    // Easy to do it for all Panes that show BufP, but not useful!

	    ED_AuxOpenParenHilite(PaneP, OpenC, Row, Col);
	}
    
    } else {
	// mismatched!
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoParenMismatch);

    }
}

// ******************************************************************************
// ED_PaneInsertChars will insert the StrP (length is Count) at the current Cursor
// position.
//
// The difficult part is keeping track of the BufRowCount for the scroll bar.
// When the insertion point is in the middle of the line, everything after that
// point continues to be pushed forward, perhaps causing a linewrap if not already
// wrapping--may also break into a new line if inserting \n.
//
// LenPushing counts the number of Chars on the line, to the right of insertion
// point--they get pushed forward.  LenBefore counts the *GROWING* number of Chars
// on the line, before the insertion point.
//
// NOTE:	LenPushing counts to the hard line break.  But LenBefore just counts
//		back to Col-0 initially... does not look back for a hard line break.
//		These numbers are reflected in Old/NewRowCount.
//
// NOTE:	As far as UNDO, PaneDelSelRange may DEL some chars... but then
//		PaneInsertChars will use SelZap boolean to CHAIN the ADD block
//		that it creates.

void    ED_PaneInsertChars(ED_PanePointer PaneP, Int32 StrLen, char * StrP, Int16 Typed)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    char		*SP, *DP, ParenC;
    Uns8		UndoMode;
    Int16		L;
    Int32		I, InsertPos, CurPos, ParenPos;
    Int32		LenPushing, LenBefore;
    Int32		OldRowCount, NewRowCount;
    Int32		TotalCount, SelZap;

    if (ED_BufferReadOnly(BufP)) return;

    ED_CmdLastId = ED_InsertId;
    ED_FrameEchoLen = 0;

    // If there is a SelRange... May move Cursor off the top of Pane!
    SelZap = ED_PaneDelSelRange(PaneP, 0);

    TotalCount = StrLen * ED_CmdMult;
    InsertPos = PaneP->CursorPos;
    ED_BufferUpdateMark(BufP, InsertPos, TotalCount);

    LenPushing = ED_BufferGetLineLen(BufP, InsertPos, 0);
    LenBefore = PaneP->CursorCol;
    OldRowCount = (LenBefore + LenPushing - 1) / PaneP->FrameP->RowChars;
    NewRowCount = 0;
     
    // Possible to prefix with "C-u 0" to insert NOTHING!
    ParenC = 0;
    ParenPos = -1;
    if (ED_CmdMult > 0) {		

        BufP->Flags |= ED_BUFMODFLAG;
	ED_BufferPlaceGap(BufP, InsertPos, TotalCount);
	DP = BufP->GapStartP;
	CurPos = InsertPos;
	while (ED_CmdMult-- > 0) {
	    SP = StrP;
	    I = 0;
	    while (I < StrLen) {
		if (*SP == 0x0d) *SP = '\n';	// XLib gives us CR instead of LF!!
    
		if (*SP == '\n') {
		    NewRowCount += 1 + (LenBefore / (PaneP->FrameP->RowChars + 1));
		    LenBefore = 0;
		    PaneP->CursorRow += 1;
		    PaneP->CursorCol = 0;
		} else {
		    LenBefore += 1;			// Just added another char
		    PaneP->CursorCol += 1;
		    // Cursor wrapped around? (Cursor has 1 more col than Chars!)
		    if (PaneP->CursorCol >= (PaneP->FrameP->RowChars + 1)) {
			PaneP->CursorRow += 1;
			PaneP->CursorCol = 1;		// Goes *AFTER* new char
		    }

		    if ((*SP == ')') || (*SP == ']') || (*SP == '}')) {
			ParenC = *SP;
			ParenPos = CurPos;
		    }
		}

		L = ED_BufferGetUTF8Len(*SP);
		I += L, CurPos += L;
		while (L--) *DP++ = *SP++;
	    }
	}
	ED_CmdMult = 1;			// Ready for next

	// Already counted rows that were added with \n, just add what is left over.
	NewRowCount += (LenBefore + LenPushing - 1) / PaneP->FrameP->RowChars;

	if (TotalCount) {
	    UndoMode = (SelZap) ? ED_UB_ADD | ED_UB_CHAIN : ED_UB_ADD;
	    if (Typed == 0) UndoMode |= ED_UB_CHUNK;

	    ED_BufferAddUndoBlock(BufP, InsertPos, TotalCount, UndoMode, NULL);
	    BufP->GapStartP += TotalCount;
	    BufP->LastPos += TotalCount;

	    PaneP->BufRowCount += NewRowCount - OldRowCount;
	    PaneP->CursorPos += TotalCount;
	}
    }
    ED_PaneMoveAfterCursorMove(PaneP, -1, 2, SelZap || (NewRowCount != OldRowCount));
    ED_PaneDrawText(PaneP);
    ED_PaneDrawScrollBar(PaneP, 0);
    ED_PaneDrawBlinker(PaneP);
    ED_PaneUpdateOtherPanes(PaneP, InsertPos, TotalCount);
    
    if (ParenC)
	ED_PaneShowOpenParen(PaneP, ParenC, ParenPos);
    
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();
}

// ******************************************************************************
// ED_PaneInsertBufferChars will insert all the chars in TemBufP at the current
// Cursor position.  It will also get rid of the Sel region, if there was one.
// But will ignore CmdMult.
//
// This function comes after some "slow" file I/O calls, so no attempt is made
// to optimize for speed--as opposed to PaneInsertChars that is called for every
// key click!
//
// This function is called by XSelInsert (Sets IsPaste == 1).
//
// Caller should kill the TempBufP after calling this function.

void	ED_PaneInsertBufferChars(ED_PanePointer PaneP, ED_BufferPointer TempBufP, Int16 IsPaste)
{
    ED_BufferPointer		BufP = PaneP->BufP;
    Int32			Size, SelZap, Count;
    Uns8			UndoMode;
    
    // Zap the sel-range, if there was one!  (Don't update, more work ahead)
    SelZap = ED_PaneDelSelRange(PaneP, 0);
    if (SelZap) ED_PaneMoveAfterCursorMove(PaneP, -1, 2, 0);

    BufP->Flags |= ED_BUFMODFLAG;	// Even if no Sel range to zap

    // Place + expand buffer to fit--add in extra Gap for later edits.
    Size = TempBufP->LastPos;
    ED_BufferPlaceGap(BufP, PaneP->CursorPos, Size + ED_GAPEXTRAEXPAND);

    // Now insert the chars from the TempBufP.  TempBufP *ITSELF* has a
    // Gap, so must insert in (possibly) two stages, before the Gap first,
    // then after the Gap.  All of these will go into THE GAP in BufP,
    // which is big enough to absorb it all.
    Count = TempBufP->GapStartP - TempBufP->BufStartP;
    memcpy(BufP->GapStartP, TempBufP->BufStartP, Count);
    // Now after the Gap in TempBufP
    if (Count < Size)
	memcpy(BufP->GapStartP + Count, TempBufP->GapEndP, Size - Count);

    // Create an Undo block for all this--no Data for ADD--Chain to SelZap DEL if any!
    UndoMode = (SelZap) ? ED_UB_ADD | ED_UB_CHAIN : ED_UB_ADD;
    UndoMode |= ED_UB_CHUNK;
    ED_BufferAddUndoBlock(BufP, PaneP->CursorPos, Size, UndoMode, NULL);
	
    BufP->GapStartP += Size;
    BufP->LastPos += Size;

    // Normal insertion does NOT move the Cursor, but Paste will!
    if (IsPaste) PaneP->CursorPos += TempBufP->LastPos;

    ED_PaneUpdateAllPos(PaneP, 0);
    ED_PaneUpdateOtherPanes(PaneP, PaneP->CursorPos, Size - SelZap);
    
    // PaneDelSelRange has handled the NegMarkDelta, so handle the additions here.
    // Then add a Mark for the end of the inserted text... but only if NOT IsPaste
    ED_BufferUpdateMark(BufP, PaneP->CursorPos, Size);
    if (! IsPaste) ED_BufferPushMark(BufP, PaneP->CursorPos + Size);
}

// ******************************************************************************
// ED_PaneUpdateOtherPanes is called after character insertion in Pane, to update
// and redraw any other visible Panes that shows the same buffer.  It goes through
// all the Frames and checks out all of their Panes.
//
// InsertPos is the point where text was inserted in the original Pane.
// InsertCount is how many characters were inserted:
//	0:	Chars were altered!  (Transpose-word will alter chars BEFORE InsertPos!)
//	Pos:	Chars were added.
//	Neg:	Chars were deleted.
//
// FPane is the OTHER Pane being updated/redrawn.
//
// If FPane starts BEFORE insertion point, the Pane itself does not change--just assume
// new chars are visible, so draw the Pane. (The End Pos of the FPane is not obvious, so
// it is easier to just draw the FPane, even if it ends well above the insertion point.
// Also *SAFER* since something like Transpose-Word might alter chars BEFORE insertion point.)
//
// If FPane starts AFTER insertion point, just bump its PanePos to keep it where
// it was--since InsertCount chars were inserted before it.  Cursor gets bumped too, but can
// handle Cursor *AFTER* PanePos has been updated--ED_PaneGetCursorLoc uses it.
//
// BUT if FPane starts on a wrapped line 'pushed' by the insertion point, just let it
// stay where it was, with the new text flowing through it.  The Cursor will move
// lower in the Pane, maybe off bottom, as more text gets drawn above it.  PaneUpdateStartPos
// handles these cases.
//
// NOTE:	When deleting a SelRange, if Cursor on FPane is stuck in the Range,
//		it should *ONLY* move back to the InsertPos, and NOT go back behind it.
//		(Same issue for FPaneP->PanePos.)
//		Logic is obvious when seen, but was difficult to anticipate!
//
// NOTE:	Cannot rely on Original Pane to have BufRowCount updated!
//

void	ED_PaneUpdateOtherPanes(ED_PanePointer PaneP, Int32 InsertPos, Int32 InsertCount)
{
    ED_FramePointer	FrameP;
    ED_PanePointer	FPaneP;
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		LastRowChar = 0;		// Cache of one
    Int32		LastRowCount = 0;
    Int32		RowStart, Ignore;
    Int16		DoScrollBar;

    FrameP = ED_FirstFrameP;
    while (FrameP) {

	FPaneP = FrameP->FirstPaneP;
	while (FPaneP) {
	    if ((FPaneP != PaneP) && (FPaneP->BufP == BufP)) {

		if (InsertCount) {

		    // Set BufRowCount for FPane... Cannot rely on Original Pane being updated!
		    DoScrollBar = 1;
		    if (FrameP->RowChars == LastRowChar)		// Found in Cache
			FPaneP->BufRowCount = LastRowCount;
		    else {						// Compute and Cache
			LastRowChar = FrameP->RowChars;
			ED_PaneFindLoc(FPaneP, BufP->LastPos, &LastRowCount, &Ignore, 1, 0);
			FPaneP->BufRowCount = LastRowCount;
		    }

		    // Figure out PanePos for FPane
		    if (FPaneP->PanePos > InsertPos) {
			FPaneP->PanePos += InsertCount;
			if (FPaneP->PanePos < InsertPos)		// Same issue as Cursor for neg InsertCount!
			    FPaneP->PanePos = InsertPos;
			ED_PaneUpdateStartPos(FPaneP, 0);
			ED_PaneFindLoc(FPaneP, FPaneP->PanePos, &FPaneP->StartRowCount, &Ignore, 1, 0);
		    }

		    // Figure out CursorPos for FPane... if InsertCount < 0, DO NOT move cursor before InsertPos!
		    if (FPaneP->CursorPos > InsertPos) {
			    FPaneP->CursorPos += InsertCount;
			    if (FPaneP->CursorPos < InsertPos)
				FPaneP->CursorPos = InsertPos;
			RowStart = ED_PaneFindLoc(FPaneP, FPaneP->CursorPos, &FPaneP->CursorRow, &FPaneP->CursorCol, 0, 0);
			ED_PaneMoveAfterCursorMove(FPaneP, RowStart, 2, 1);	// Will set Scrollbar
			DoScrollBar = 0;
		    }

		    if (DoScrollBar) ED_PaneSetScrollBar(FPaneP);

		    ED_PaneDrawScrollBar(FPaneP, 0);
	       }

	       ED_PaneDrawText(FPaneP);
	       ED_PaneDrawBlinker(FPaneP);
	    }

	    FPaneP = FPaneP->NextPaneP;
	} // FPaneP

	FrameP = FrameP->NextFrameP;
    } // FrameP
}

// ED_PaneUpdateOtherPanesIncrBasic is the INCREMENTAL version of the above.  This function
// simply stashes the most basic PanePos and CursorPos values... but DOES NOT compute locations.
// It is called during a fast REPLACE...  Must call ED_PaneUpdateOtherPanesIncrRest afterwards
// to compute locations and update the other panes.
//
// NOTE:	For a one-off change, calling the main PaneUpdateOtherPanes is more efficient.

void	ED_PaneUpdateOtherPanesIncrBasic(ED_PanePointer PaneP, Int32 InsertPos, Int32 InsertCount)
{
    ED_FramePointer	FrameP;
    ED_PanePointer	FPaneP;

    FrameP = ED_FirstFrameP;
    while (FrameP) {

	FPaneP = FrameP->FirstPaneP;
	while (FPaneP) {
	    if ((FPaneP != PaneP) && (FPaneP->BufP == PaneP->BufP)) {
		if (InsertCount) {
		    if (FPaneP->PanePos > InsertPos) {
			FPaneP->PanePos += InsertCount;
			if (FPaneP->PanePos < InsertPos)	// For NEG InsertCount;
			    FPaneP->PanePos = InsertPos;
		    }
		    if (FPaneP->CursorPos > InsertPos) {
			FPaneP->CursorPos += InsertCount;
			if (FPaneP->CursorPos < InsertPos)
			    FPaneP->CursorPos = InsertPos;
		    }
		}
	    }
	    FPaneP = FPaneP->NextPaneP;
	}
	FrameP = FrameP->NextFrameP;
    }
}

void	ED_PaneUpdateOtherPanesIncrRest(ED_PanePointer PaneP)
{
    ED_FramePointer	FrameP;
    ED_PanePointer	FPaneP;
    Int32		LastRowChar = 0;	// Cache of 1
    Int32		LastRowCount = 0;
    Int32		RowStart, Ignore;

    FrameP = ED_FirstFrameP;
    while (FrameP) {

	FPaneP = FrameP->FirstPaneP;
	while (FPaneP) {
	    if ((FPaneP != PaneP) && (FPaneP->BufP == PaneP->BufP)) {

		// Set the RowCount for FPane... use Cache of one!
		if (FrameP->RowChars == LastRowChar)
		    FPaneP->BufRowCount = LastRowCount;
		else {
		    LastRowChar = FrameP->RowChars;
		    ED_PaneFindLoc(FPaneP, FPaneP->BufP->LastPos, &LastRowCount, &Ignore, 1, 0);
		    FPaneP->BufRowCount = LastRowCount;
		}

		// Set PanePos for FPane
		ED_PaneUpdateStartPos(FPaneP, 0);
		ED_PaneFindLoc(FPaneP, FPaneP->PanePos, &FPaneP->StartRowCount, &Ignore, 1, 0);

		// Set CursorPos for FPane
		RowStart = ED_PaneFindLoc(FPaneP, FPaneP->CursorPos, &FPaneP->CursorRow, &FPaneP->CursorCol, 0, 0);
		ED_PaneMoveAfterCursorMove(FPaneP, RowStart, 2, 1);	// Will *SET* ScrollBar

		// Now draw everything
		ED_PaneDrawText(FPaneP);
		ED_PaneDrawBlinker(FPaneP);
		ED_PaneDrawScrollBar(FPaneP, 0);
	    }
	    FPaneP = FPaneP->NextPaneP;
	}
	FrameP = FrameP->NextFrameP;
    }
}

// ******************************************************************************
// ******************************************************************************
// MARK RING
//
// Each Buffer maintains its own MarkRing.  Marks are stored as Pos values, so
// they need to be updated if chars are inserted/delted before the Mark location.
//
// When N chars are deleted, all Marks after deletion Pos get decremented by N **BUT**
// a Mark that is positioned LESS THAN N from the deletion point should be moved to
// the deletion Pos, NOT BEFORE IT.
//
// Early versions accumulated a DeltaPos rather than update all the marks
// with each insertion/deletion.  But the non-linear behavior of deletion made
// this approach too cumbersome.

// ******************************************************************************
// ED_PanePushmark simply creates a new Mark on the Ring, with the given Pos.

void	ED_BufferPushMark(ED_BufferPointer BufP, Int32 Pos)
{
    if (BufP->MarkRingArr[BufP->MarkRingIndex] != Pos) {			// Already there?
	BufP->MarkRingIndex = (BufP->MarkRingIndex + 1) & ED_MARKRINGMASK;	// Inc and wrap
	BufP->MarkRingArr[BufP->MarkRingIndex] = Pos;				// Then write
    }

    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoMarkSet);
}

// ******************************************************************************
// ED_PaneGetMark returns the current (top) Mark.  It will advance the Index to
// the next Mark if DoPop is 1.

Int32	ED_BufferGetMark(ED_BufferPointer BufP, Int16 DoPop)
{
    Int32	MarkPos;

    MarkPos = BufP->MarkRingArr[BufP->MarkRingIndex];
    if (DoPop) {
	BufP->MarkRingIndex = (BufP->MarkRingIndex - 1) & ED_MARKRINGMASK;
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoMarkPop);	
    }
    return MarkPos;
}

// ******************************************************************************
// ED_BufferGetDiffMark will pop as many times as necessary to get MarkPos different
// from NotPos.  It *MAY* fail and return NotPos if there are no different Marks.
//
// This function is essential when 1) Go to Begin of Buffer, sets Mark then (2) Go
// to End of Buffer, also automatically sets Mark.  Now try to Kill the whole rgn,
// would be impossible without this function.  (Otherwise, cursor will be directly
// on last Mark, so no rgn to Kill.)

Int32	ED_BufferGetDiffMark(ED_BufferPointer BufP, Int32 NotPos)
{
    Int32	MarkPos;
    Int16	I;

    // Pop 0..MarkRingCount times, as necessary.
    for (I = 0; I < ED_MARKRINGCOUNT; I++) {
	BufP->MarkRingIndex = (BufP->MarkRingIndex - I) & ED_MARKRINGMASK;
	MarkPos = BufP->MarkRingArr[BufP->MarkRingIndex];
	if (MarkPos != NotPos)
	    return MarkPos;
    }

    return MarkPos;
}

// ******************************************************************************
// ED_BufferSwapMark will replace the top Mark with NewPos and return the old value.
//
// NOTE:	*IFF* NewPos == Old value, then the top Mark is POPPED.
//		This is done ONLY once to make PaneExchMarkCmd work well when
//		going to start + end of a buffer.

Int32	ED_BufferSwapMark(ED_BufferPointer BufP, Int32 NewPos)
{
    Int32			MarkPos;
    
    MarkPos = BufP->MarkRingArr[BufP->MarkRingIndex];		// Read
    if (MarkPos == NewPos) {
	BufP->MarkRingIndex = (BufP->MarkRingIndex - 1) & ED_MARKRINGMASK;
	MarkPos = BufP->MarkRingArr[BufP->MarkRingIndex];
    }
    BufP->MarkRingArr[BufP->MarkRingIndex] = NewPos;		// Write new

    return MarkPos;
}


// ******************************************************************************
// ED_BufferUpdateMark will incorporate the Pos Delta (+ or -) to move the
// existing Marks.  With a negative Delta, a Mark occuring just after Pos can
// only move back to Pos, cannot jump behind it.
//
// NOTE:	For deletion: Delta is negative, Pos is the *LEFT* edge of
//		range being deleted... i.e. assume DELETE FORWARD.

void	ED_BufferUpdateMark(ED_BufferPointer BufP, Int32 Pos, Int32 Delta)
{
    Int16		I;
    Int32		*RAP;

    RAP = BufP->MarkRingArr;
    for (I = 0; I < ED_MARKRINGCOUNT; RAP++, I++) {
	if (*RAP >= Pos) {
	    *RAP += Delta;
	    // Can only happen if Delta is negative!
	    if (*RAP < Pos) *RAP = Pos;
	}
    }
}

// ******************************************************************************
// ******************************************************************************
// ED_BufferNew creates a new Buffer and allocates initial memory for it.
// The new buffer is stashed in ED_FirstBufP and chained at the head of the list.
// 
// The ED_BufferRecord (just the record) is sub-allocated from ED_BufferStore.
// But the actual BufMem, pointed to by BufP->BufStartP, uses malloc.
//
// NOTE:	FileName and PathNameP can be NULL.
//
//		BUFNOFILEFLAG is set if either Name or Path is NULL.
//		Both are provided when reading an actual file.
//
//		If a PathNameP is NOT given, a new one is extracted first
//		using getenv, and failing that with getcwd.  The latter
//		being a less readable absolute pathname (rather than
//		just refering to /home).  An alternative would be to use
//		the same Pathname as previous FirstBufP if one existed.
//
//		NO PROVISIONS are made for "(UNREACHABLE)" pathnames, can
//		be returned by getcwd.
//
// NOTE:	InitSize of 0 means allocate the default size!

ED_BufferPointer	ED_BufferNew(Int32 InitSize, char * FileNameP, char * PathNameP, Int16 InfoOnly)
{
    ED_BufferPointer	BufP;
    char *		MemP;
    Int16		I;

    BufP = sc_SAStoreAllocBlock(&ED_BufferStore);
    BufP->Tag = *(Uns32 *)ED_BufTag;
    BufP->Flags = ED_BUFNOFLAG | ED_BUFCLEANUNDOFLAG;
    BufP->MarkPos = -1;			// No Mark yet
    BufP->CursorPos = 0;
    BufP->PanePos = 0;
    BufP->LastPos = 0;
    BufP->PaneRefCount = 0;		// No Panes yet.
    BufP->Ident = 0;			// No Ident for now

    if (FileNameP == NULL) {
	sprintf(ED_TempBufName, ED_STR_TempBufName, ED_TempBufCount++);
	FileNameP = ED_TempBufName;
	BufP->Flags |= ED_BUFNOFILEFLAG;
    }
    strncpy(BufP->FileName, FileNameP, NAME_MAX + 1);	// Stash the FileName
    BufP->FileName[NAME_MAX] = 0;			// Just in case!!

    if (InfoOnly) {
	BufP->Flags |= ED_BUFNOFILEFLAG | ED_BUFINFOONLYFLAG | ED_BUFREADONLYFLAG;
	BufP->PathName[0] = 0;
	BufP->DirNameOffset = 0;
    } else {
	if (PathNameP == NULL) {
	    PathNameP = getenv("PWD");
	    BufP->Flags |= ED_BUFNOFILEFLAG;
	}
	if (PathNameP) {
	    strncpy(BufP->PathName, PathNameP, ED_BUFFERPATHLEN + 1);
	    if (BufP->PathName[ED_BUFFERPATHLEN] != 0)	// String overrun!
		G_SETEXCEPTION("Pathname is too long", 0);
	} else {
	    PathNameP = getcwd(BufP->PathName, ED_BUFFERPATHLEN + 1);
	    if (PathNameP == NULL)
		G_SETEXCEPTION("Pathname is too long", 1);
	}
	BufP->DirNameOffset = (ED_UtilGetLastStrSlash(PathNameP) + 1) - PathNameP;
    }


    if (InitSize == 0) InitSize = ED_BUFINITLEN;
    MemP = (char *)malloc(InitSize);
    if (! MemP) G_SETEXCEPTION("Malloc Buffer Mem Failed", 0);

    if (InfoOnly) {
	BufP->FirstUSP = BufP->LastUSP = NULL;
	BufP->USCount = 0;
	BufP->USTotalSize = 0;
    } else
	ED_BufferInitUndo(BufP);		// Initialize undo buffer

    // New buffer is one big Gap!
    BufP->BufStartP = MemP;
    BufP->BufEndP = MemP + InitSize;		// Same as GapEnd, for now
    BufP->GapStartP = MemP;
    BufP->GapEndP = MemP + InitSize;		// 1 Passed end

    BufP->PrevBufP = NULL;
    BufP->NextBufP = ED_FirstBufP;
    if (ED_FirstBufP) ED_FirstBufP->PrevBufP = BufP;
    ED_FirstBufP = BufP;

    BufP->MarkRingIndex = 0;
    for (I = 0; I < ED_MARKRINGCOUNT; I++)
	BufP->MarkRingArr[I] = 0;

    return BufP;
}

// ******************************************************************************
// ED_BufferReadFile creates a new Buf and reads the FD file into it.
// It will return NULL if the read fails, in which case errno should have the
// appropriate error code!
//
// NOTE:	Assumes FD is open for READ only.
//		It will lseek to the end/beginning to get the filesize.
//		NULL NameP and PathP means make a temp buffer

ED_BufferPointer	ED_BufferReadFile(Int32 FD, char * NameP, char * PathP)
{
    Int32		Size;
    ED_BufferPointer	BufP;
    Int32		Count, SysError;

    // Find the length of the file by seeking the end.
    Size = lseek(FD, 0, SEEK_END);
    if (Size == -1) return NULL;
    lseek(FD, 0, SEEK_SET);

    // Create a new buffer!  Add a little Gap space to file size.
    BufP = ED_BufferNew(Size + ED_GAPEXTRAEXPAND, NameP, PathP, 0);

    // Read it all in!
    Count = read(FD, BufP->GapStartP, Size);
    if (Count != Size) {
	SysError = errno;		// Save
	    ED_BufferKill(BufP);
	errno = SysError;		// Restore for caller!
	return NULL;
    }
    BufP->GapStartP += Size;
    BufP->LastPos = Size;

    // Check that Filename has no collisions with other buffers!
    if (NameP) ED_BufferCheckNameCol(BufP);

    return BufP;
}

// ******************************************************************************
// ED_BufferKill gets rid of BufP *IF AND ONLY IF* PaneRefCount is 0 !!

void		ED_BufferKill(ED_BufferPointer BufP)
{
    if (BufP->PaneRefCount != 0) return;

    ED_XSelReleasePrimary(BufP);	// In case this BufP held the Primary!

    // Unchain it first
    if (BufP->PrevBufP) BufP->PrevBufP->NextBufP = BufP->NextBufP;
    if (BufP->NextBufP) BufP->NextBufP->PrevBufP = BufP->PrevBufP;
    if (ED_FirstBufP == BufP) ED_FirstBufP = BufP->NextBufP;

    ED_BufferKillUndo(BufP);
    free(BufP->BufStartP);
    sc_SAStoreFreeBlock(&ED_BufferStore, BufP);
}

// ******************************************************************************
void	ED_BufferUpdateModeLines(ED_BufferPointer TheBufP)
{
    ED_FramePointer	FP;
    ED_PanePointer	PP;

    FP = ED_FirstFrameP;
    while (FP) {
	PP = FP->FirstPaneP;
	while (PP) {
	    if (PP->BufP == TheBufP)
		ED_PaneDrawModeLine(PP);
	    PP = PP->NextPaneP;
	}
	FP = FP->NextFrameP;
    }
}

// ******************************************************************************
// ED_BufferDidSave does the housekeeping after a Buffer was saved.
// Skip drawing ModeLine on ThisFrameP (if not NULL)--as it will be updated later.

void	ED_BufferDidSave(ED_BufferPointer BufP, ED_FramePointer ThisFrameP)
{
    Int32		SLen;

    // No more Mods... they were saved--Set BUFCLEAN flag for Undo system.
    BufP->Flags &= ~(ED_BUFMODFLAG | ED_BUFFILTERFLAG);
    BufP->Flags |= ED_BUFCLEANUNDOFLAG;

    // Record SAVE... so Undo can tell when Buf becomes UN-modified!
    ED_BufferAddUndoBlock(BufP, 0, 0, ED_UB_ADD | ED_UB_SAVE, NULL);
    
    // EchoBuff is only MSGSTRLEN long.
    SLen = strlen(ED_STR_EchoWrotePath);
    if (ED_FullPathLen + SLen >= ED_MSGSTRLEN)
	ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoWroteName, ED_MSGSTRLEN - strlen(ED_STR_EchoWroteName), ED_Name);
    else
	ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoWrotePath, ED_MSGSTRLEN - SLen, ED_FullPath);

    // Saving a buffer will change the ModeLine of any Pane displaying it.
    // These must be redrawn!

    if (BufP->PaneRefCount) ED_BufferUpdateModeLines(BufP);
}


// ******************************************************************************
// ED_BufferDidWrite updates the Buffer information after a "Write File" command.
// It relies on global ED_Path, ED_Name, ED_FullPath, and ED_FullPathLen vars.
//
// If this *WAS* a special RO/Info-Only Buffer, writing it out makes it a normal buffer!
// Resets these flags and resets Undo buffers for it.
//
// NOTE:	This function calls ED_BufferDidSave to handle BUFMOD + BUFCLEANUNDO flags.

void	ED_BufferDidWrite(ED_BufferPointer BufP, ED_FramePointer ThisFrameP)
{
    char *		MemP;

    // Stash Path + Name in BufP!
    strcpy(BufP->PathName, ED_Path);
    strcpy(BufP->FileName, ED_Name);
    MemP = ED_UtilGetLastStrSlash(ED_Path);
    BufP->DirNameOffset = MemP - ED_Path;
    
    if (BufP->Flags & ED_BUFREADONLYFLAG) {
        ED_BufferKillUndo(BufP);		// Kill the old (just to be safe!)
	ED_BufferInitUndo(BufP);		// Create 1 new USlab.
    }
    
    BufP->Flags &= ~ (ED_BUFNOFILEFLAG | ED_BUFINFOONLYFLAG | ED_BUFREADONLYFLAG);
    BufP->Ident = 0;		// Reset IDENT... maybe TOO AGRESSIVE for future

    // Check if buffer's new "name" collides with any other buffers!
    // (If flag set, name will be shown with <DirName>)
    ED_BufferCheckNameCol(BufP);

    // The rest is just as if the Pane/Buffer was saved!
    ED_BufferDidSave(BufP, ThisFrameP);
}


// ******************************************************************************
// ED_BufferPlaceGap repositions the Gap at the indicated Offset and makes sure
// there is room for at least Len bytes in it.  If the existing Gap size is
// less than Len, then the entire BufMem is grown, so the gap is Len plus an
// extra ED_GAPEXTRAEXPAND bytes--to minimize calls to realloc.
//
// NOTE:	BufP->GapEndP is first byte AFTER Gap.
//
// BufMem is grown by "realloc" which (a) tries to grow in place or failing that
// (b) creates a new memory block and copies the old to the new.  So if realloc
// fails, an explicit alloc and copy will also fail.
//
// There are 5 conceptual blocks of interest when moving the gap around.
// 1:	(T) Top block, stays the same.
// 2:	(M) gapMove block, if before the gap, move to just after... or vice versa.
// 3:	(-) The existing Gap itself, moves up or down, and can be expanded
// 4:	(B) Bottom block, stays at the bottom, but needs to slide if Mem expands.
// 5:	(e) Expanded space, always starts at the bottom, add to gap.
//
// (Top)   Simple gap move up		Gap move up + expand
// Before: [TTTTTMM---BBBBBB]		[TTTTTMM---BBBBBBeee] 
// After:  [TTTTT---MMBBBBBB]		[TTTTT------MMBBBBBB]
//
//	   Simple gap move down		Gap move down + expand
// Before: [TTTTTTT---MMBBBB]		[TTTTT---MMBBBBeeeee]
// After:  [TTTTTTTMM---BBBB]		[TTTTTMM------BBBBBB]

#define		ED_SLIDEDOWN(P, Len, Delta)	memmove(P + Delta, P, Len)

void	ED_BufferPlaceGap(ED_BufferPointer BufP, Int32 Offset, Int32 Len)
{
    Int32	OldGapLen, GapMove;
    Int32	NewGapLen, NewMemLen;
    char *	NewMemP;
    char *	NewGapStartP;

    if (Len < 0) Len = 0;

    OldGapLen = BufP->GapEndP - BufP->GapStartP;    
    if (OldGapLen < Len) { // Expand Gap
	NewGapLen = Len + ED_GAPEXTRAEXPAND;
	NewMemLen = (BufP->BufEndP - BufP->BufStartP) + (NewGapLen - OldGapLen);

	NewMemP = realloc(BufP->BufStartP, NewMemLen);
	if (NewMemP == NULL) G_SETEXCEPTION("Could not realloc Buf", NewMemLen);
	
	// If the block moved, transpose pointers to new locations--adjust BufEndP later.
	if (NewMemP != BufP->BufStartP) {
	    BufP->BufEndP += (NewMemP - BufP->BufStartP);	// NOT real end
	    BufP->GapStartP += (NewMemP - BufP->BufStartP);
	    BufP->GapEndP += (NewMemP - BufP->BufStartP);
	    BufP->BufStartP = NewMemP;
	}

	NewGapStartP = BufP->BufStartP + Offset;
	GapMove = NewGapStartP - BufP->GapStartP;
	
	// We are expanding, so even if GapMove is 0, blocks have to move
	if (GapMove == 0) {
	    // Gap stays, just move bottom block further down to expand Gap.
	    ED_SLIDEDOWN(BufP->GapEndP, BufP->BufEndP - BufP->GapEndP, NewGapLen - OldGapLen);

	} else if (GapMove < 0) {
	    // Moving Gap *UP*, first Move bottom block, then GapMove block down.
	    ED_SLIDEDOWN(BufP->GapEndP, BufP->BufEndP - BufP->GapEndP, NewGapLen - OldGapLen);
	    ED_SLIDEDOWN(NewGapStartP, -GapMove, NewGapLen);

	} else {
	    // Moving Gap *DOWN*, first move bottom block, then GapMove block up.
	    ED_SLIDEDOWN(BufP->GapEndP + GapMove, BufP->BufEndP - (BufP->GapEndP + GapMove), NewGapLen - OldGapLen);
	    ED_SLIDEDOWN(BufP->GapEndP, GapMove, -OldGapLen);
	}
	
	BufP->GapStartP = NewGapStartP;
	BufP->GapEndP = NewGapStartP + NewGapLen;
	BufP->BufEndP = BufP->BufStartP + NewMemLen;
	
    } else {	// Do NOT expand gap

	NewGapStartP = BufP->BufStartP + Offset;
	if (NewGapStartP != BufP->GapStartP) {
	
	    GapMove = NewGapStartP - BufP->GapStartP;
	    if (GapMove < 0)
		ED_SLIDEDOWN(NewGapStartP, -GapMove, OldGapLen);	// Slide GapMove block down
	    else
		ED_SLIDEDOWN(BufP->GapEndP, GapMove, -OldGapLen);	// Slide GapMove block up
		
	    BufP->GapStartP = NewGapStartP;
	    BufP->GapEndP = NewGapStartP + OldGapLen;
	}
    }
	    
#ifdef DEBUG    
    // TEST_FillGap(BufP);
#endif
}


// ******************************************************************************
// ED_BufferGetUTF8Len returns the length of a UTF8 char sequence--most are
// just 1 byte.  (128 to 191 are continuing bytes in a multi-byte seq)
//
// "char" acts like Int8, but is its own unique type.
// Alas, Int8 will sign-extend and fail below unless cast as Uns8.

Int16	ED_BufferGetUTF8Len(register char C)
{
    if ((Uns8)C <= 127) return 1;
    if (192 <= (Uns8)C && (Uns8)C < 224) return 2;
    if (224 <= (Uns8)C && (Uns8)C < 240) return 3;
    if (240 <= (Uns8)C && (Uns8)C < 248) return 4;
    
    return 1;			// Cannot get here!
}

// ******************************************************************************
// ED_BufferCIsAlpha return 1 for alphanumeric characters, 0 otherwise--considered
// whitespace, word breaks, etc.
//
// [0-9, A-Z, a-z, UTF8] are Alphanumeric, everything else is Non-alphanumeric.
// Word commands only care about alphanumerics and jump over everything else!
//
// NOTE:	All UTF8 bytes, (first + continuation) have the leftmost bit set,
//		so are numerically 128 or higher.

Int16	ED_BufferCIsAlpha(register char C)
{
    if ((Uns8)C < '0') return 0;
    if ((Uns8)C < '9' + 1 ) return 1;
    if ((Uns8)C < 'A') return 0;
    if ((Uns8)C < 'Z' + 1) return 1;
    if ((Uns8)C < 'a') return 0;
    if ((Uns8)C < 'z' + 1) return 1;
    if ((Uns8)C < 128) return 0;
    return 1;			// Includes *ALL* UTF8
}


// ******************************************************************************
// ED_BufferPosToPtr will map from Pos(ition) to a Pointer in the Buffer memory.
//
// NOTE:	The Gap will *NEVER* straddle a UTF8 multi-byte sequence.

char *	ED_BufferPosToPtr(ED_BufferPointer BufP, Int32 Pos)
{
    char	*P;

    P = BufP->BufStartP + Pos;
    if (P >= BufP->GapStartP)
	P += (BufP->GapEndP - BufP->GapStartP);

    return P;
}

// ******************************************************************************
// ED_BufferGetPosPlusRows (and its sister, ED_BufferGetPosMinusRows) do the heavy
// lifting when scrolling the Pane.  These find where the PanePos should go.
//
// Pos is the buffer position (independent of Gap) of the first char in a Row,
// usually the first Row of the Pane display.  This function will find the beginning
// of the Row occuring 'Rows' later, further down.
//
// A Row can end in a \n character, or wrap around multiple times as it reaches the
// Pane edge.  There is a Gap in the Buffer, perhaps somewhere in the region being
// looked at... additionally, each UTF8 Char may take multiple bytes.
//
// NOTE:	Legacy function, can be replaced by call tp ED_PaneFindPos.
//		More runtime processing, but less code.

Int32	ED_BufferGetPosPlusRows(ED_BufferPointer BufP, Int32 Pos, Int32 *RowsP, Int32 ColLimit)
{
    char		*EndP, *CurP;
    Int32		CurRow, CurCol;
    Int16		L;

    CurP = ED_BufferPosToPtr(BufP, Pos);
    EndP = (CurP < BufP->GapStartP) ? BufP->GapStartP : BufP->BufEndP;
    CurRow = CurCol = 0;

DoRest:

    while (CurP < EndP) {
	if ((*RowsP == CurRow) || (Pos == BufP->LastPos)) {
	    *RowsP = CurRow;
	    return Pos;
	}

	CurCol++;
	if (*CurP == '\n')			// Hit \n, New row AFTER it
	    CurRow++, CurCol = 0;
	    
	if (CurCol > ColLimit) {		// Hit wrap around, 
	    CurRow++, CurCol = 0;		// New row starts RIGHT here!
	    continue;				// Loop back to check against Row count
	}					// ...Will increment CurCol then too.

	L = ED_BufferGetUTF8Len(*CurP);		// Get length of UTF8 Char (1-4)
	CurP += L, Pos += L;
    }

    if (EndP < BufP->BufEndP) {			// Went before the Gap, now
	EndP = BufP->BufEndP;			// go look after the Gap.
	CurP = BufP->GapEndP;
	goto DoRest;
    }

    *RowsP = CurRow;
    return BufP->LastPos;			// Should never get here.
}

// ******************************************************************************
// ED_BufferGetPosMinusRows (and its sister, ED_BufferGetPosPlusRows) do the heavy
// lifting when scrolling the Pane.  These find where the PanePos should go.
//
// Pos is the buffer position (independent of Gap) of the *FIRST* char in a Row,
// usually the first Row of the Pane display.  This function will find the beginning
// of the Row occuring 'Rows' sooner, higher up.
//
// Searching UP is more difficult, linebreaks (and wrap arounds) can only be seen at
// the end of the previous Line, which may wrap-around multiple Rows before ending.
// So in order to move up 1 single row, the function may have to go up 17 full Rows
// to find an actual /n, then move back down 16 Rows.  There is also a Gap somewhere
// in the Buffer and UTF8 Chars may take 1-4 bytes.  (UTF8 lengths are not obvious
// when reverse traversing, but non-initial UTF chars are all 10xx-xxxx)
//
// NOTE:	((ColCount - 1) / ColLimit) == counts wrap-around rows.
//		The "-1" is so a full row by itself does NOT count as wrap-around.
//
// NOTE:	The algorithm that moves forward from the hard linebreak requires *RowsP >= 1;
//
// NOTE:	Legacy function, can be replaced by call to ED_PaneFindPos for less code
//		but more runtime processing.

Int32	ED_BufferGetPosMinusRows(ED_BufferPointer BufP, Int32 Pos, Int32 *RowsP, Int32 ColLimit)
{
    char	*EndP, *CurP;
    Int32	RowCount, ColCount;
    Int16	Loop;

    if (*RowsP == 0) return Pos;			// Will do nothing
    if (Pos == 0) {					// Cannot go backwards!
	*RowsP = 0;
	return 0;
    }

    RowCount = ColCount = 0;
    CurP = ED_BufferPosToPtr(BufP, --Pos);		// Look backwards
    if (Pos == 0) {					// Ran out
	*RowsP = (*CurP == '\n') ? 1 : 0;
	return 0;
    }
    if (*CurP == '\n')					// \n of last line...
	CurP = ED_BufferPosToPtr(BufP, --Pos);		// ... skip it.

    Loop = 0;
    if (CurP < BufP->GapStartP)				// Already before gap
	EndP = BufP->BufStartP;				// Go to very beginning
    else {
	EndP = BufP->GapEndP;				// After gap...
	Loop = 1;					// ... Loop again for before gap
    }
    
DoRest:
    while (CurP >= EndP) {
    
	while (ED_BUFFERISMIDUTF8(*CurP))		// Hit UTF8 Char, from below!
	    CurP--, Pos--;				// Go until UTF8 is over

	ColCount++;					// 1-based !
	if (*CurP == '\n') {				// Hit a hard line break
	    ColCount -= 2;				// -1 for \n, and -1 for wrap count
	    RowCount += 1 + (ColCount / ColLimit);	// This row + wrap arounds
	    ColCount = 0;				// Start again.
	    if (RowCount >= *RowsP) {			// This last /n went too far
		Pos++;					// ... will go forward, over the /n
		Loop = 0;				// Done
		break;
	    }
	}

	if (Pos == 0) {					// Can not go any more, Pretend /n is prev
	    RowCount += 1 + (--ColCount / ColLimit);	// May have been 1 or more rows
	    break;					// Done (Loop is already 0 if Pos is before Gap)
	}
	
	CurP--, Pos--;					// May hit UTF8 Char!!  See above!
    }   

    if (Loop) {						// Not done, and was looking after the gap
	EndP = BufP->BufStartP;				// Now look before Gap
	CurP = BufP->GapStartP - 1;			// GapStart is *IN* gap, so minus 1
	Loop = 0;					// 1 time max
	goto DoRest;
    }

    if (RowCount > *RowsP) {				// Gone too far, so move forward!
	Int32	Count = (RowCount - *RowsP) * ColLimit; // Get the extra rows back
							// DOES NOT work for *RowsP == 0 !!
	RowCount = *RowsP;
	while (Count--) {
	    CurP = ED_BufferPosToPtr(BufP, Pos);
	    Pos += ED_BufferGetUTF8Len(*CurP);
	}
    }

    *RowsP = RowCount;
    return Pos;
}

// ******************************************************************************
// ED_BufferFillStr will fill StrP with chars from BufP, starting at StartPos.
// Up to MaxLen bytes are placed into StrP and it is null-terminated.  If NoNL,
// any '\n' chars encountered will be replaced by ' ';
//
// Return value is strlen of StrP.

Int32	ED_BufferFillStr(ED_BufferPointer BufP, char * StrP, Int32 StartPos, Int32 MaxLen, Int16 NoNL)
{
    char	*CurP, *DP;
    Int32	CurPos, Len, L;

    CurPos = StartPos;
    DP = StrP;
    Len = 1;		// Leave room for terminating 0

    while (CurPos < BufP->LastPos) {
	CurP = ED_BufferPosToPtr(BufP, CurPos);
	L = ED_BufferGetUTF8Len(*CurP);

	if (Len + L < MaxLen) {
	    Len += L;
	    CurPos += L;
	    if (L == 1) {
		if (NoNL && (*CurP == '\n'))
		    *DP++ = ' ', CurP++;
		else
		    *DP++ = *CurP++;
	    } else while (L--) *DP++ = *CurP++;
	} else
	    break;
    }
    *DP = 0;

    return (Int32)(DP - StrP);
}

// ******************************************************************************
// ED_BufferGetRowStartPos returns the Pos that starts the Row containing OldPos
// on the given Col.  This is most useful because various functions end up with
// a Pos (Marks, etc.) that we locate on a given (Col,Row).  *BUT* BufferGetPos+/-Row
// functions really only work when the Pos begins a Row--NOT mid-span.

Int32	ED_BufferGetRowStartPos(ED_BufferPointer BufP, Int32 OldPos, Int32 Col)
{
    Int32	I, CurPos;
    char	*CurP;

    I = Col;
    CurPos = OldPos;
    while (I--) {
	CurP = ED_BufferPosToPtr(BufP, --CurPos);		// Prev Char
	while (ED_BUFFERISMIDUTF8(*CurP)) CurP--, CurPos--;
    }

    return CurPos;
}

// ******************************************************************************
// ED_BufferGetLineStartPos returns the Pos for the hard start of the line,
// regardless of how many times (if any) it wraps around.

Int32	ED_BufferGetLineStartPos(ED_BufferPointer BufP, Int32 OldPos)
{
    Int32	CurPos;
    char	*CurP;

    if (OldPos == 0) return 0;
    CurPos = OldPos;

    do {
	CurP = ED_BufferPosToPtr(BufP, --CurPos);		// Prev char!
	while (ED_BUFFERISMIDUTF8(*CurP)) CurP--, CurPos--;
	if (*CurP == '\n') return CurPos + 1;			// Newline!
    } while (CurPos);

    return CurPos;
}

// ******************************************************************************
// ED_BufferGetLineEndPos finds the Pos for the \n at the end of the Line,
// regardless of any wrap-arounds for long lines.

Int32	ED_BufferGetLineEndPos(ED_BufferPointer BufP, Int32 OldPos)
{
    Int32	CurPos;
    char	*CurP;

    if (OldPos == BufP->LastPos) return BufP->LastPos;
    CurPos = OldPos;

    while (CurPos < BufP->LastPos) {
	CurP = ED_BufferPosToPtr(BufP, CurPos);
	if (*CurP == '\n') return CurPos;
	CurPos += ED_BufferGetUTF8Len(*CurP);
    }

    return CurPos;
}

// ******************************************************************************
// ED_BufferGetLineLen returns how many Chars (not Bytes) are in the Line containing
// Pos (can be anywhere on the Line).  If BeforeToo, it will count up to the beginning
// of the line (can be many rows) as well as to the end (can also be many rows).
//
// NOTE:	DOES NOT count final \n.

Int32	ED_BufferGetLineLen(ED_BufferPointer BufP, Int32 Pos, Int16 BeforeToo)
{
    char	*CurP;
    Int32	CurPos, Count;

    Count = 0;

    // Count Chars (not bytes) to hard beginning of line
    if (BeforeToo) {
	CurPos = Pos;
	while (CurPos--) {
	    CurP = ED_BufferPosToPtr(BufP, CurPos);
	    while (ED_BUFFERISMIDUTF8(*CurP)) CurP--, CurPos--;
	
	    if (*CurP == '\n') break;		// Hard line break
	    Count++;				// Count Chars on line BEFORE Pos.
	}
    }

    // Now count Chars (not bytes) to hard end of line
    CurPos = Pos;
    while (CurPos < BufP->LastPos) {		
	CurP = ED_BufferPosToPtr(BufP, CurPos);
	if (*CurP == '\n') break;
	
	Count++;
	CurPos += ED_BufferGetUTF8Len(*CurP);
    }
    
    return Count;
}

// ******************************************************************************
// ED_BufferGetLineCount will go through the buffer and count Newline chars--hard
// line ends.
// Input:	*PosP says were to start... usually at 0.
//		*CountP says how many lines to count, -1 for all.
//
// Output:	*PosP says were it stops.
//		*CountP says how many were counted.
//
// Typical call:  Start at 0, Count N lines, then report Pos.
//
// NOTE:	For these, and all the other functions that deal with the Gap,
//		only the Gap is empty, everything else in the buffer is valid
//		data--otherwise, it would be IN the Gap.  So there is no reason
//		to check against BufP->LastPos, as it would fall on BufEndP or
//		GapStartP (if the Gap was at the end).

void		ED_BufferGetLineCount(ED_BufferPointer BufP, Int32 *PosP, Int32 *CountP)
{
    char	*CurP, *EndP;
    Int16	LoopAgain;
    Int32	CurPos, LineCount;

    if (*CountP <= 0) *CountP = 0x7FFFFFFF;

    CurPos = *PosP;
    LineCount = 0;
    LoopAgain = 1;
    EndP = BufP->GapStartP;
    CurP = BufP->BufStartP + *PosP;
    if (CurP >= BufP->GapStartP) {
	CurP += (BufP->GapEndP - BufP->GapStartP);
	EndP = BufP->BufEndP;
	LoopAgain = 0;
    }

DoRest:

    while (CurP < EndP) {
	if (*CurP == '\n') {
	    LineCount += 1;
	    if (LineCount == *CountP) {
		*PosP = CurPos;
		return;
	    }
        }
	CurP++, CurPos++;
    }

    if (LoopAgain) {
	LoopAgain = 0;
	EndP = BufP->BufEndP;
	CurP = BufP->GapEndP;
	goto DoRest;
    }

    *CountP = LineCount;
    *PosP = CurPos;
    return;
}

// ******************************************************************************
// ED_BufferCheckNameCol checks to see if any other Buffers have the same
// FileName.  If so, it sets the BUFNAMECOLFLAG so the ModeLine in the Pane
// would also show the DirName containing the File.
//
// NOTE:	Currently no provisions to keep user opening/reading/writing
//		the same file from MULTIPLE Panes/Buffers.  realEmacs uses a
//		special temporary file to lock access, scEmacs DOES NOT.

void	ED_BufferCheckNameCol(ED_BufferPointer BufP)
{
    ED_BufferPointer	BP = ED_FirstBufP;

    while (BP) {
	if ((BP != BufP) && (0 == strcmp(BP->FileName, BufP->FileName))) {
	    BufP->Flags |= ED_BUFNAMECOLFLAG;
	    break;
	}

	BP = BP->NextBufP;
    }
}

// ******************************************************************************
// ED_BufferFindByName looks for a buffer with the matching Name and/or Path.
// NameP or PathP can be NULL.

ED_BufferPointer	ED_BufferFindByName(char * NameP, char * PathP)
{
    ED_BufferPointer	BufP = ED_FirstBufP;

    while (BufP) {
	if (((NameP == NULL) || (0 == strcmp(BufP->FileName, NameP))) &&
	    ((PathP == NULL) || (0 == strcmp(BufP->PathName, PathP))))
	    return BufP;

	BufP = BufP->NextBufP;
    }

    return NULL;
}

// ******************************************************************************
// ED_BufferWriteFile write the entire buffer (excluding Gap) to the open file
// specified with FD--assumes Write head is properly placed.
//
// Return 0 for success, 1 for failure.
// If failure, check errno for details.

Int16	ED_BufferWriteFile(ED_BufferPointer BufP, Int32 FD)
{
    Int32	Count;

    // write up to Gap first
    if (BufP->GapStartP > BufP->BufStartP) {
	Count = write(FD, BufP->BufStartP, BufP->GapStartP - BufP->BufStartP);
	if (Count < (BufP->GapStartP - BufP->BufStartP)) {
	    return 1;
	}
    }

    if (BufP->GapEndP < BufP->BufEndP) {
	Count = write(FD, BufP->GapEndP, BufP->BufEndP - BufP->GapEndP);
	if (Count < (BufP->BufEndP - BufP->GapEndP)) {
	    return 1;
	}
    }

    fsync(FD);		// Flush write buffer + update
    return 0;
}

// ******************************************************************************
// ED_BufferNeedsFilter returns 1 *IFF* BufP contains any CR (0x0d) or Tab (0x09)
// characters.
//
// This function ignores UTF8 as those characters have their high bits set and
// WILL not give false positive against CR and Tab.

Int16	ED_BufferNeedsFilter(ED_BufferPointer BufP)
{
    char	*CurP, *EndP;

    CurP = BufP->BufStartP;
    EndP = BufP->GapStartP;

LoopAgain:
    while (CurP < EndP) {
	if ((*CurP == 0x09) || (*CurP == 0x0d))
	    return 1;
        CurP++;
    }

    if (CurP < BufP->BufEndP) {
	CurP = BufP->GapEndP;
	EndP = BufP->BufEndP;
	goto LoopAgain;
    }

    return 0;
}

// ******************************************************************************
// ED_BufferDoFilter -- 
// CR -> Replace with LF (or remove if already followed by LF).
// Tab -> Replace by N spaces--enough to align with next tabstop.
// (This requires tracking Col position wrt. hard newlines, not wraparounds!)

// if CRLF, just zap the CR.  If just CR, change it in place.
Int32	ED_AuxReplaceCR(ED_BufferPointer BufP, Int32 CurPos)
{
    char	*CurP, *NextP;

    CurP = ED_BufferPosToPtr(BufP, CurPos);
    NextP = ED_BufferPosToPtr(BufP, CurPos + 1);

    if (*NextP == '\n') {		// Just zap the CR
	ED_BufferPlaceGap(BufP, CurPos, 0);
	BufP->GapEndP += 1;
	BufP->LastPos -= 1;
    } else
	*CurP = '\n';			// Change in situ

    // Either way, we end up at CurPos + 1
    return (CurPos + 1);
}

// Replace the Tab char with 1..ED_TABSTOP spaces, inorder to align
// with ED_TABSTOP Col increments.
Int32	ED_AuxReplaceTab(ED_BufferPointer BufP, Int32 CurPos, Int32 *ColP)
{
    char	*CurP;
    Int32	I, Len;

    // How many spaces are needed?
    Len = (((*ColP / ED_TABSTOP) + 1) * ED_TABSTOP) - *ColP;
    if (Len == 0) Len = ED_TABSTOP;

    // If just 1 space, simply replace the Tab char in situ.
    if (Len == 1) {
	CurP = ED_BufferPosToPtr(BufP, CurPos);
	*CurP = ' ';
    } else {
	// Place the gap just after it, replace the Tab and add in the rest.
	ED_BufferPlaceGap(BufP, CurPos + 1, Len - 1);
	CurP = BufP->GapStartP - 1;
	for (I = 0; I < Len; I++)
	    *CurP++ = ' ';
        BufP->GapStartP += Len - 1;
	BufP->LastPos += Len - 1;
    }

    *ColP += Len;
    return (CurPos + Len - 1);
}

void	ED_BufferDoFilter(ED_BufferPointer BufP, void (*UpdateFuncP)(Int16, void *), void * DataP)
{
    Int32	L, CurPos, Col;
    Int32	NextUpdate;
    Int16	Percent;
    char	*CurP;

    // NOTE:	Easy to strcat a ".sc" to the FileName, since it is being
    //		filtered and we don't want to accidentally overwrite the
    //		original.  (Would also have to BufferCheckNameCol here.)
    //		*BUT*, a) Cannot SaveFile since it needs to be written first.
    //		and b) cannot avoid opening the original file again, and
    //		again, since we changed the name!  So best, not to go down
    //		the ".sc" path....  Instead, simply set a BUFFILTERFLAG, so
    //		a warning is generated at Save time!

    BufP->Flags |= ED_BUFFILTERFLAG;
    if (UpdateFuncP) (*UpdateFuncP)(-1, DataP);		// Initialize
    
    Percent = 10;
    NextUpdate = ((Int64)Percent * (Int64)BufP->LastPos) / 100;
    
    Col = CurPos = 0;
    while (CurPos < BufP->LastPos) {			// LastPos will change during loop!
	CurP = ED_BufferPosToPtr(BufP, CurPos);

	if (*CurP == '\n')
	    Col = 0;
	else if (*CurP == 0x0d) {
	    CurPos = ED_AuxReplaceCR(BufP, CurPos);
	    CurP = ED_BufferPosToPtr(BufP, CurPos);
	    Col = 0;
	} else if (*CurP == 0x09) {
	    CurPos = ED_AuxReplaceTab(BufP, CurPos, &Col);
	    CurP = ED_BufferPosToPtr(BufP, CurPos);
	} else
	    Col += 1;

	if (CurPos > NextUpdate) {
	    if (UpdateFuncP) {
		(*UpdateFuncP)(Percent, DataP);
		usleep(50000);
	    }
	    Percent += 10;				// Re-calc NextUpdate, LastPos changes!
	    NextUpdate = ((Int64)Percent * (Int64)BufP->LastPos) / 100;
	}

	L = ED_BufferGetUTF8Len(*CurP);
	CurPos += L;
    }

    BufP->Flags |= ED_BUFMODFLAG;			// Changed!		
}

// ******************************************************************************
// Inserts a string into the Buffer at Pos.  Ignores, Undo, Mark, CmdMult, SelRange,
// etc.  This function is basically called to create RO information buffers, list
// of commands, etc.

Int32	ED_BufferInsertLine(ED_BufferPointer BufP, Int32 Pos, char * StrP)
{
    Int16	StrLen;

    StrLen = strlen(StrP);
    ED_BufferPlaceGap(BufP, Pos, StrLen + 1);

    memmove(BufP->GapStartP, StrP, StrLen);
    BufP->GapStartP += StrLen;
    *(BufP->GapStartP++) = '\n';
    
    BufP->LastPos += StrLen + 1;
    return Pos + StrLen + 1;
}

Int16	ED_BufferReadOnly(ED_BufferPointer BufP)
{
    if (BufP->Flags & ED_BUFREADONLYFLAG) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoReadOnly);
	ED_FrameDrawEchoLine(ED_CurFrameP);
	ED_FrameFlashError(ED_CurFrameP);
	return 1;
    }

    return 0;
}


// ******************************************************************************
// ******************************************************************************
// Registered Command Functions--can be invoked interactively by user.
//
// Most commands have to do with the CursorPos, Row+Col is already known.
// But, because of UTF8, The next char over maybe 1..4 bytes!  So must use
// ED_PaneFindLoc and ED_PaneFindPos.
//
// The Buffer has a Gap which is where all modifications (insert/delete) takes
// place.  It must be considered whenever traversing the Buffer.
//
// The cursor must always be visible.  so if it is moved off the screen, the Pane
// must be scrolled to bring the cursor back into view.  The ScrollBar parameters
// must also be set whenever text is added or deleted from the buffer.
//
// The Frame (and its Panes) can always be re-sized horizontally, this may impact
// wrap-around for long lines.  Some commands deal with Lines, but most with Rows.
//
// Many commands accept numeric CmdMult.  Some ignore the CmdMultNeg (indicating
// a negative polarity) while others reverse logic for it.  Some can even ignore
// the actual number and act differently if there are *any* numeric args.
//
// Many commands send status and messages to the EchoLine.
// Many commands set or use Marks.
// Many commands interact with Selection range, if there is one.
// Many commands kill text, so interact with the KillRing machinery.
// Some commands act differently depending on the preceeding command.
// Many commands modify a Buffer, so must set the ED_BUFMODFLAG.
// Many commands can potentially alter the PRIMARY selection text.
//
// There may be multiple Frames, each with multiple Panes.  The same Buffer
// may be displayed by a number of Panes.  Changes in one Pane must therefore
// be displayed in all other Panes showing the same Buffer.
//
// Many commands require user confirmation or input.  The QR (Query/Response)
// mechanism is generally used for that.  But that is intrinsically asynch,
// the program DOES NOT hang until the user responds.  Instead, there is a
// CallBack function (EDCB_* function names)  that handles the response,
// can accept (process) or reject it--flashing the EchoLine instead of ringing a bell.
//
// There is also a Pop-Up (PU) list mechanism that can show a list of items for
// selection--e.g. mark position, kill ring entries, command list.  In the latter
// case, the PU can invoke a command that re-launches that (or another) PU window.
// XLib makes this re-launch complicated.



// AUX -- Handles shift-selection for commands.
void	ED_AuxCmdDoShiftSel(ED_PanePointer PaneP)
{
    if (ED_CmdShift) {
	if (ED_SelPaneP != PaneP) {
	    ED_SelPaneP = PaneP;
	    ED_SelMarkPos = PaneP->CursorPos;
	    ED_SelMarkCol = PaneP->CursorCol;
	    ED_SelMarkRow = PaneP->CursorRow;
	    ED_BufferPushMark(PaneP->BufP, PaneP->CursorPos);
	    ED_XSelSetPrimary(PaneP);	    
	}
    } else
	ED_SelPaneP = NULL;
}

// AUX -- Advanced CursorPos over Word or non-word spaces--depending on InWord arg.
// Dir == 1 for forward, -1 for reverse.
//
// NOTE:	All parts of a UTF8 assembly (from First to last) have the high-bit set,
//		so are numerically >= 128.  Therefore count as IsAlpha!
Int16	ED_AuxCmdInWordScan(ED_PanePointer PaneP, Int16 InWord, Int32 Dir, Int32 PosLimit)
{
    Int16	In;
    char	*CurP;

    In = InWord;
    while (In == InWord) {
	PaneP->CursorPos += Dir;
	if (PaneP->CursorPos == PosLimit) return 1;
	CurP = ED_BufferPosToPtr(PaneP->BufP, PaneP->CursorPos);
	In = ED_BufferCIsAlpha(*CurP);
    }
    return 0;
}


// ******************************************************************************
void	ED_CmdSplitPane(ED_PanePointer PaneP)
{
    ED_PaneSplit(PaneP);
    ED_FrameDrawAll(PaneP->FrameP);    
}

// ******************************************************************************
void	ED_CmdKillPane(ED_PanePointer PaneP)
{
    ED_PaneKill(PaneP);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
void	ED_CmdKillOtherPanes(ED_PanePointer PaneP)
{
    ED_FramePointer	FrameP = PaneP->FrameP;
    ED_PanePointer	PP;

    if (FrameP->PaneCount > 1) {
	PP = FrameP->FirstPaneP;
	while (PP) {
	    if (PP != PaneP) ED_PaneKill(PP);
	    PP = PP->NextPaneP;
	}
    }
    ED_FrameDrawAll(PaneP->FrameP);    
}

// ******************************************************************************
void	ED_CmdGoNextPane(ED_PanePointer PaneP)
{
    ED_FramePointer	FrameP = PaneP->FrameP;

    if (FrameP->PaneCount > 1) {
	PaneP = PaneP->NextPaneP;
	if (PaneP == NULL) PaneP = FrameP->FirstPaneP;
	FrameP->CurPaneP = PaneP;
    }
    ED_FrameDrawAll(FrameP);
}

// ******************************************************************************
// ED_CmdExit

// Could call ED_FrameKill... but that would immediately
// kill the XWin and caller of this function will draw on air!
// Instead, just use the main loop... send termination signals
// as if user clicks CLOSE button on each Frame window!
void	ED_AuxDoExit(void)
{
    ED_FramePointer	FP;
    XEvent		XE = {0};	// No Valhalla if no Init--Valgrind!
    ED_BufferPointer	BufP;

    // Reset all the MOD flags, so will close without asking!!
    BufP = ED_FirstBufP;
    while (BufP) {
	BufP->Flags &= ~ ED_BUFMODFLAG;
	BufP = BufP->NextBufP;
    }
    
    FP = ED_FirstFrameP;
    while (FP) {
	XE.type = ClientMessage;
	XE.xclient.display = ED_XDP;
	XE.xclient.window = FP->XWin;
	XE.xclient.message_type = 0;
	XE.xclient.format = 32;
	XE.xclient.data.l[0] = ED_XWMDelAtom;

	XSendEvent(ED_XDP, FP->XWin, 0, 0, &XE);
	 
	FP = FP->NextFrameP;
    }
}

// CALLBACK (ED_FrameQRAsk)
// Return 0 == Good response
// Return 1 == Bad response
Int16	EDCB_ExitFunc(void)
{
    char		C;

    if (ED_QRRespLen == 1) {
    
	C = ED_QRRespS[0];
	if ((C == 'n') || (C == 'N')) {
	    ED_QRPaneP = NULL;
	    return 0;
	}

	if ((C == 'y') || (C == 'Y')) {
	    // Close EVERYTHING
	    ED_AuxDoExit();
	    ED_QRPaneP = NULL;
	    return 0;
	}
    }

    // Response was bad... get new answer!
    return 1;
}

// Ask to save only IFF a Buf is Modified and HAS FILE.
void	ED_CmdExit(ED_PanePointer PaneP)
{
    ED_BufferPointer	CurBufP;
    Int16		SaveFirst = 0;

    // Check buffers!
    CurBufP = ED_FirstBufP;
    while (CurBufP) {
	if ((CurBufP->Flags & ED_BUFMODFLAG) &&
	    ! (CurBufP->Flags & ED_BUFNOFILEFLAG)) {
	    SaveFirst = 1;
	    break;
	}
	CurBufP = CurBufP->NextBufP;
    }

    if (SaveFirst)
	ED_FrameQRAsk(PaneP->FrameP, ED_STR_QuerySaveExit, NULL, ED_QRLetterType, EDCB_ExitFunc, NULL);
    else
	ED_AuxDoExit();
}

// ******************************************************************************
// Possible to call CmdHelp FROM a Pane already displaying help!

void	ED_AuxFillHelpBuf(ED_BufferPointer BufP)
{
    ED_FRegPointer	FRP;
    ED_FBindPointer	FBP;
    Int16		Count;
    Int32		Pos, L, Len;
    char		Str[512];
    char		KeyS[128];

    // Write Name, version, and copyright header
    Pos = 0;
    sprintf(Str, "scEmacs V%d.%d", ED_VERSIONMAJOR, ED_VERSIONMINOR);
    Pos = ED_BufferInsertLine(BufP, Pos, "");    
    Pos = ED_BufferInsertLine(BufP, Pos, Str);
    Pos = ED_BufferInsertLine(BufP, Pos, "   ...A Small C Emacs!");
    Pos = ED_BufferInsertLine(BufP, Pos, "");
    Pos = ED_BufferInsertLine(BufP, Pos, "Copyright (C) 2018 Shawn Amir");
    Pos = ED_BufferInsertLine(BufP, Pos, "All Rights Reserved.");
    Pos = ED_BufferInsertLine(BufP, Pos, "");
    Pos = ED_BufferInsertLine(BufP, Pos, "scEmacs is free software: you can redistribute it and/or modify");
    Pos = ED_BufferInsertLine(BufP, Pos, "it under the terms of the GNU General Public License as published by");
    Pos = ED_BufferInsertLine(BufP, Pos, "the Free Software Foundation, version 3 or later of the License.");
    Pos = ED_BufferInsertLine(BufP, Pos, "");
    Pos = ED_BufferInsertLine(BufP, Pos, "scEmacs comes with ABSOLUTELY NO WARRANTY, NO implied warranty of");
    Pos = ED_BufferInsertLine(BufP, Pos, "merchantibility or fitness for a particular purpose.");
    Pos = ED_BufferInsertLine(BufP, Pos, "");

    // Write out all the commands + bindings.
    sprintf(Str, "  %24s --> Bindings", "Commands");
    Pos = ED_BufferInsertLine(BufP, Pos, Str);
    sprintf(Str, "  %24s     --------", "--------");
    Pos = ED_BufferInsertLine(BufP, Pos, Str);
    {

	FRP = ED_FirstFRegP;
	while (FRP) {
	    Len = sprintf(Str, "  %24s --> ", FRP->NameP);

	    FBP = FRP->FirstBindP;
	    if (FBP == NULL) {
		strcat(Str, "Not bound, use M-x ");
		strcat(Str, FRP->NameP);
	    }
	    else {
		Count = 0;
		while (FBP) {
		    L = ED_KeyStrPrint(FBP->KeyStr, KeyS);
		    // Leaving plenty of room...
		    if (Len + L < 500) {			
			if (Count) strcat(Str, "or ");
			strcat(Str, KeyS);
		    } else {
			strcat(Str, "...");
			break;
		    }

		    Count += 1;
		    FBP = FBP->FuncNextP;
		}
	    }

	    Pos = ED_BufferInsertLine(BufP, Pos, Str);
	    FRP = FRP->SortNextP;
	}
    }
}


void	ED_CmdHelp(ED_PanePointer PaneP)
{
    ED_FramePointer	FP = PaneP->FrameP;
    ED_BufferPointer	NewBufP;
    ED_PanePointer	PP;

    // See if we already have a HELP buffer sitting around!
    NewBufP = ED_FirstBufP;
    while (NewBufP) {
	if (NewBufP->Ident == ED_BUFHELPIDENT)	// Got it!!
	    break;
	NewBufP = NewBufP->NextBufP;
    }

    // If none were found, create a new Buffer and fill it.
    if (NewBufP == NULL) {
	NewBufP = ED_BufferNew(0, NULL, NULL, 1);
	NewBufP->Ident = ED_BUFHELPIDENT;
	ED_AuxFillHelpBuf(NewBufP);
    }

    // Find the bottom Pane on this Frame... if
    // only 1 Pane, then split it!
    if (FP->PaneCount == 1) {
	ED_PaneSplit(PaneP);
	PP = PaneP->NextPaneP;
    } else {
	PP = FP->FirstPaneP;
	while (PP->NextPaneP) PP = PP->NextPaneP;
    }

    // Now put the NewBuf in the bottom Pane.
    if (PP->BufP->Ident != ED_BUFHELPIDENT)
	ED_PaneGetNewBuf(PP, NewBufP);

    ED_FrameDrawAll(FP);    
}

// ******************************************************************************
void	ED_CmdQuitAction(ED_PanePointer PaneP)		// C-g (more like cancel/abort)
{
    // Turn off Selection
    ED_SelPaneP = NULL;

    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoQuit);
    ED_FrameDrawAll(PaneP->FrameP);    
}

// ******************************************************************************
void	ED_CmdSetMark(ED_PanePointer PaneP)
{
    if (ED_CmdMult == 4) {				// "C-u C-spc" or "C-u 4 C-spc"
	Int32		RowStartPos;

	PaneP->CursorPos = ED_BufferGetMark(PaneP->BufP, 1);	// Pop the last Mark
	RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
	ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, 2, 0);
    } else
	ED_BufferPushMark(PaneP->BufP, PaneP->CursorPos);

    ED_FrameDrawAll(PaneP->FrameP);	
}

// ******************************************************************************

void	ED_CmdGetCursorInfo(ED_PanePointer PaneP)
{
    char	TheChar[8] = {0};
    char	* CharName;
    char	CharNum[64];
    char	* CurP;
    Int16	L, I;

#ifdef DEBUG
    if (ED_CmdMult == 4)
	TEST_PrintUndoMemory(PaneP->BufP);
#endif

    if (PaneP->CursorPos == PaneP->BufP->LastPos) {		// EOB case
	ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoEOBCursorInfo, 
		 PaneP->CursorPos + 1, PaneP->BufP->LastPos,
		 PaneP->CursorCol, PaneP->CursorRow);
    } else {							// NOT EOB

	CurP = ED_BufferPosToPtr(PaneP->BufP, PaneP->CursorPos);
	L = ED_BufferGetUTF8Len(*CurP);

	for (I = 0; I < L; I++)
	    TheChar[I] = CurP[I];

	if (L == 1) {
	    if (*CurP < 0x20)
		CharName = XKeysymToString(0x0000ff00 | *CurP);
	    else
		CharName = XKeysymToString((Uns32)*CurP);
	    snprintf(CharNum, 64, "(%1$d #o%1$o 0x%1$02x)", *CurP);
	} else
	    snprintf(CharNum, 64, "[UTF8:%#x-%#x-%#x-%#x]",
		     (Uns8)TheChar[0], (Uns8)TheChar[1], (Uns8)TheChar[2], (Uns8)TheChar[3]);

        ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoCursorInfo, (L == 1) ? CharName : TheChar, CharNum,
		 PaneP->CursorPos + 1, PaneP->BufP->LastPos,
		 (PaneP->CursorPos * 100) / (PaneP->BufP->LastPos - 1),
		 PaneP->CursorCol, PaneP->CursorRow);
    }
    
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();
}

// ******************************************************************************
void	ED_CmdGoNextChar(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    char		*CurP;
    Int32		RowStartPos = -1;

    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdGoPrevChar(PaneP);
	return;
    }    

    if (PaneP->CursorPos >= BufP->LastPos) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	return;
    }
    ED_AuxCmdDoShiftSel(PaneP);

    while (ED_CmdMult-- && (PaneP->CursorPos < BufP->LastPos)) {
	CurP = ED_BufferPosToPtr(BufP, PaneP->CursorPos);
	PaneP->CursorPos += ED_BufferGetUTF8Len(*CurP);		// usually just 1
    }
    
    RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, (PaneP->RowCount - 2) / 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);    
}

// ******************************************************************************
void	ED_CmdGoNextWord(ED_PanePointer PaneP)
{
    Int32		LastPos = PaneP->BufP->LastPos;
    Int32		RowStartPos = -1;
    char		*CurP;

    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdGoPrevWord(PaneP);
	return;
    }    
    
    if (PaneP->CursorPos >= LastPos) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	return;
    }
    ED_AuxCmdDoShiftSel(PaneP);

    while (ED_CmdMult--) {
	CurP = ED_BufferPosToPtr(PaneP->BufP, PaneP->CursorPos);
	if (! ED_BufferCIsAlpha(*CurP))
	    if (ED_AuxCmdInWordScan(PaneP, 0, 1, LastPos)) goto DoneDone;
	if (ED_AuxCmdInWordScan(PaneP, 1, 1, LastPos)) goto DoneDone;
    }
    
DoneDone:
    RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, (PaneP->RowCount - 2) / 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
void	ED_CmdGoPrevChar(ED_PanePointer PaneP)
{
    char		*CurP;
    Int32		RowStartPos = -1;

    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdGoNextChar(PaneP);
	return;
    }    

    if (PaneP->CursorPos == 0) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBegBuf);
	return;
    }
    ED_AuxCmdDoShiftSel(PaneP);

    while (ED_CmdMult-- && PaneP->CursorPos) {
	CurP = ED_BufferPosToPtr(PaneP->BufP, --PaneP->CursorPos);
	while (ED_BUFFERISMIDUTF8(*CurP)) CurP--, PaneP->CursorPos--;
    }

    RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, (PaneP->RowCount - 2) / 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);            
}

// ******************************************************************************
// BackwardWord *IS* not completely symmetrical with ForwardWord, because the
// Cursor really indicates the letter *BEFORE* it.  So going forward, the Cursor
// jumps over the word and lands ON the first white/breakspace.  Going backward,
// the cursor jumps over the word, but lands on the FIRST letter of the word.
void	ED_CmdGoPrevWord(ED_PanePointer PaneP)
{
    char		*CurP;
    Int32		RowStartPos = -1;

    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdGoNextWord(PaneP);
	return;
    }    
     
    if (PaneP->CursorPos == 0) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBegBuf);
	return;
    }
    ED_AuxCmdDoShiftSel(PaneP);

    while (ED_CmdMult--) {
	// Initially, take 1 step BACK!
	PaneP->CursorPos -= 1;
	if (PaneP->CursorPos == 0) goto DoneDone;
	CurP = ED_BufferPosToPtr(PaneP->BufP, PaneP->CursorPos);
	while (ED_BUFFERISMIDUTF8(*CurP)) CurP--, PaneP->CursorPos--;
	
	if (! ED_BufferCIsAlpha(*CurP))
	    if (ED_AuxCmdInWordScan(PaneP, 0, -1, 0)) goto DoneDone;
	if (ED_AuxCmdInWordScan(PaneP, 1, -1, 0)) goto DoneDone;

	// Now move the Cursor 1 step FORWARD;
	CurP = ED_BufferPosToPtr(PaneP->BufP, PaneP->CursorPos);
	PaneP->CursorPos += ED_BufferGetUTF8Len(*CurP);
    }
    
DoneDone:
    RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, (PaneP->RowCount - 2) / 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);            
}

// ******************************************************************************
// GoPrev and GoNext use ED_CmdTargetCol, so you can start from the end
// of a long line, cross a bunch of short ones, and end up at the end
// of another long line.  Any intervening other command will re-set this.
void	ED_CmdGoNextRow(ED_PanePointer PaneP)
{
    Int32	RowStartPos = -1;

    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdGoPrevRow(PaneP);
	return;
    }        
    
    if (PaneP->CursorPos == PaneP->BufP->LastPos) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	return;
    }
    ED_AuxCmdDoShiftSel(PaneP);

    if ((ED_CmdLastId != (Uns64)ED_CmdGoNextRow) &&
	(ED_CmdLastId != (Uns64)ED_CmdGoPrevRow))
	ED_CmdTargetCol = PaneP->CursorCol;

    PaneP->CursorCol = ED_CmdTargetCol;
    PaneP->CursorRow += ED_CmdMult;
    
    RowStartPos = ED_PaneFindPos(PaneP, &PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 1, 0);	// FromPane
    ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, (PaneP->RowCount - 2) / 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// GoPrev and GoNext use ED_CmdTargetCol, so you can start from the end
// of a long line, cross a bunch of short ones, and end up at the end
// of another long line.  Any intervening other command will re-set this.
void	ED_CmdGoPrevRow(ED_PanePointer PaneP)
{
    Int32	RowStartPos = -1;

    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdGoNextRow(PaneP);
	return;
    }        

    if (PaneP->CursorPos == 0) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBegBuf);
	return;
    }
    ED_AuxCmdDoShiftSel(PaneP);    

    if ((ED_CmdLastId != (Uns64)ED_CmdGoPrevRow) &&
	(ED_CmdLastId != (Uns64)ED_CmdGoNextRow))
	ED_CmdTargetCol = PaneP->CursorCol;

    PaneP->CursorCol = ED_CmdTargetCol;
    PaneP->CursorRow -= ED_CmdMult;
    // If Cursor tries to move ABOVE absolute Row 0, ED_PaneFindPos places it at (0,0).
    // But we want it to stay at (CursorCol, 0)... so limit CursorRow.
    if ((PaneP->CursorRow + PaneP->StartRowCount) < 0)
	PaneP->CursorRow = - PaneP->StartRowCount;

    RowStartPos = ED_PaneFindPos(PaneP, &PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);	// NOT FromPane!
    ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, (PaneP->RowCount - 2) / 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// C-u --> Move PaneStart up N lines
// Place cursor on same "relative" row as before.
void	ED_CmdGoNextPage(ED_PanePointer PaneP)
{
    Int32	DeltaRow, Col;

    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdGoPrevPage(PaneP);
	return;
    }        

    // Nothing to do if already at bottom
    if ((PaneP->StartRowCount + PaneP->RowCount - 2) >= PaneP->BufRowCount) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	ED_FrameDrawEchoLine(PaneP->FrameP);		// Since PaneDoInteract is not called
	return;
    }

    // First move the Pane down.
    Col = 0;
    DeltaRow = (ED_CmdMultCmd) ? ED_CmdMult : PaneP->RowCount - 3;
    ED_PaneFindPos(PaneP, &PaneP->PanePos, &DeltaRow, &Col, 0, 0);
    PaneP->StartRowCount += DeltaRow;

    // Re-position the cursor.
    ED_PaneMoveCursorAfterScroll(PaneP, DeltaRow);
    ED_PaneSetScrollBar(PaneP);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// C-u --> Move PaneStart up N lines
// Place cursor on same "relative" row as before.
void	ED_CmdGoPrevPage(ED_PanePointer PaneP)
{
    Int32	DeltaRow, Col;

    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdGoNextPage(PaneP);
	return;
    }    

    // Nothing to do if already at top
    if (PaneP->PanePos == 0) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBegBuf);
	ED_FrameDrawEchoLine(PaneP->FrameP);		// Since PaneDoInteract is not called
	return;
    }

    // First move the Pane up.
    Col = 0;
    DeltaRow = (ED_CmdMultCmd) ? - ED_CmdMult : - (PaneP->RowCount - 3);
    ED_PaneFindPos(PaneP, &PaneP->PanePos, &DeltaRow, &Col, 0, 0);
    PaneP->StartRowCount += DeltaRow;

    // RE-position the cursor.
    ED_PaneMoveCursorAfterScroll(PaneP, DeltaRow);
    ED_PaneSetScrollBar(PaneP);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
void	ED_CmdGoLineEnd(ED_PanePointer PaneP)
{
    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdGoLineStart(PaneP);
	return;
    }        

    if (PaneP->CursorPos == PaneP->BufP->LastPos) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	return;
    }
    ED_AuxCmdDoShiftSel(PaneP);
    
DoItAgain:

    PaneP->CursorPos = ED_BufferGetLineEndPos(PaneP->BufP, PaneP->CursorPos);
    ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    
    if ((ED_CmdMult-- > 1) && (PaneP->CursorPos < PaneP->BufP->LastPos)) {
	PaneP->CursorPos += 1;
	goto DoItAgain;
    }

    ED_PaneMoveAfterCursorMove(PaneP, -1, 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
void	ED_CmdGoLineStart(ED_PanePointer PaneP)
{
    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdGoLineEnd(PaneP);
	return;
    }        

    if (PaneP->CursorPos == 0) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBegBuf);
	return;
    }
    ED_AuxCmdDoShiftSel(PaneP);    

DoItAgain:

    PaneP->CursorPos = ED_BufferGetLineStartPos(PaneP->BufP, PaneP->CursorPos);
    ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);

    if ((ED_CmdMult-- > 1) && PaneP->CursorPos) {
	PaneP->CursorPos -= 1;		// Go back one, end of PREV line now.
	goto DoItAgain;
    }

    ED_PaneMoveAfterCursorMove(PaneP, -1, 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
void	ED_CmdGoBufEnd(ED_PanePointer PaneP)
{
    Int32	RowStartPos;
    
    if (ED_SelPaneP) ED_SelPaneP = NULL;

    PaneP->CursorPos = PaneP->BufP->LastPos;
    RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, 2, 0);		// 2 above bottom of Pane

    ED_BufferPushMark(PaneP->BufP, PaneP->CursorPos);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
void	ED_CmdGoBufStart(ED_PanePointer PaneP)
{
    ED_SelPaneP = NULL;

    PaneP->CursorPos = 0;
    PaneP->CursorRow = PaneP->CursorCol = 0;
    PaneP->PanePos = 0;
    PaneP->StartRowCount = 0;

    ED_PaneSetScrollBar(PaneP);
    
    ED_BufferPushMark(PaneP->BufP, PaneP->CursorPos);    
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// Normally, just repositions the Pane, so the Cursor appears on the middle line.
// If invoked immediately again, repositions the Pane so the Cursor appears at the
// Top.  And if invoked once again, repositions the Pane so the Cursor appears at
// the bottom.  This cycle of Middle-Top-Bottom repeats with repeated invocations.
//
// If called with C-u N it repositions the Pane so the Cursor appears on the Nth
// line from the top (or Nth line from the bottom if Neg).
void	ED_CmdRecenterPage(ED_PanePointer PaneP)
{
    static Int16	InvokeCount = 0;
    Int32		Delta;

    // Move the cursor to the beginning of its row
    PaneP->CursorPos = ED_BufferGetRowStartPos(PaneP->BufP, PaneP->CursorPos, PaneP->CursorCol);
    PaneP->CursorCol = 0;

    if (ED_SelPaneP) ED_SelPaneP = NULL;

    if (ED_CmdMultCmd) {		// ED_CmdMult -> Line from top to go to!
	InvokeCount = 2;		// Back to Middle, if invoked again

	Delta = ED_CmdMult;
	if (ED_CmdMultNeg)
	    Delta = (PaneP->RowCount - 2) - Delta;

	if ((Delta < 0) || (Delta > (PaneP->RowCount - 2)))
	    Delta = (PaneP->RowCount - 2) / 2;
	
	ED_PaneMoveForCursor(PaneP, PaneP->CursorPos, Delta);
	
    } else {
	if (ED_CmdLastId == (Uns64)ED_CmdRecenterPage)
	    InvokeCount += 1;
	else
	    InvokeCount = 0;

	switch (InvokeCount % 3) {
	    case 0:		Delta = (PaneP->RowCount - 2) / 2; break;
	    case 1:		Delta = 0; break;
	    default:		Delta = PaneP->RowCount - 2; break;
	}
	ED_PaneMoveForCursor(PaneP, PaneP->CursorPos, Delta);
    }

    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// Currently not BOUND to a key, invoked with a triple mouse click.
void	ED_CmdSelectLine(ED_PanePointer PaneP)
{
    Int32		Pos;

    ED_SelPaneP = PaneP;
    if (PaneP->CursorPos) {
	ED_SelMarkPos = ED_BufferGetLineStartPos(PaneP->BufP, PaneP->CursorPos);
    } else
	ED_SelMarkPos = 0;

    Pos = ED_BufferGetLineEndPos(PaneP->BufP, PaneP->CursorPos);
    if (Pos < PaneP->BufP->LastPos)
	Pos += 1;				// Start of next line
    PaneP->CursorPos = Pos;

    ED_PaneFindLoc(PaneP, ED_SelMarkPos, &ED_SelMarkRow, &ED_SelMarkCol, 0, 0);
    Pos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    ED_PaneMoveAfterCursorMove(PaneP, Pos, 2, 0);
    
    ED_XSelSetPrimary(PaneP);
    ED_BufferPushMark(PaneP->BufP, ED_SelMarkPos);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
void	ED_CmdSelectAll(ED_PanePointer PaneP)
{
    Int32	Pos;

    Pos = PaneP->BufP->LastPos;

    ED_SelPaneP = PaneP;
    ED_SelMarkPos = Pos;
    ED_PaneFindLoc(PaneP, Pos, &ED_SelMarkRow, &ED_SelMarkCol, 0, 0);

    PaneP->CursorPos = 0;
    PaneP->CursorRow = 0;
    PaneP->CursorCol = 0;

    PaneP->PanePos = 0;
    PaneP->StartRowCount = 0;

    ED_PaneSetScrollBar(PaneP);
    
    ED_XSelSetPrimary(PaneP);
    ED_BufferPushMark(PaneP->BufP, Pos);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// ED_CmdSelectArea
//
// Acts differently if clicked on Alphanumeric char, white space, or random char.
// Therefore, use three different Predicate callback functions to perform task

typedef Int16 (*ED_SLPredP)(char A, char B);

// AUX -- Selects a range, depending on the selection predicate function
void	ED_AuxCmdSelectLike(ED_PanePointer PaneP, ED_SLPredP PredP, Int32 *StartPosP, Int32 *EndPosP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		S, E;
    char		*CurP, C;

    S = PaneP->CursorPos;
    CurP = ED_BufferPosToPtr(BufP, PaneP->CursorPos);
    C = *CurP;					// Match more of these
    while (S > 0) {
	CurP = ED_BufferPosToPtr(BufP, --S);
	while (ED_BUFFERISMIDUTF8(*CurP)) CurP--, S--;
	if (! (*PredP)(*CurP, C)) {
	    S += ED_BufferGetUTF8Len(*CurP);	// All UTF8 are Alpha, so this will be 1 !!
	    break;
	}
    }

    E = PaneP->CursorPos;
    CurP = ED_BufferPosToPtr(BufP, E);
    while (E < BufP->LastPos) {
	E += ED_BufferGetUTF8Len(*CurP);
	if (E == BufP->LastPos) break;
	CurP = ED_BufferPosToPtr(BufP, E);
	if (! (*PredP)(*CurP, C)) break;
    }

    *StartPosP = S;
    *EndPosP = E;
}

Int16	EDCB_BlankPred(char A, char B)		// Selects Space and/or Newline
{
    return (A == ' ' || A == '\n');
}

Int16	EDCB_WordPred(char A, char B)		// Must be Alpha/Numb/UTF8
{
    return ED_BufferCIsAlpha(A);
}

Int16	EDCB_IdentPred(char A, char B)		// Must match first char!
{
    return (A == B);
}

void	ED_CmdSelectArea(ED_PanePointer PaneP)
{
    Int32	StartPos, EndPos, RowStart;
    char	*CurP;

    ED_SelPaneP = NULL;
    
    CurP = ED_BufferPosToPtr(PaneP->BufP, PaneP->CursorPos);

    if (*CurP == ' ' || *CurP == '\n')
	ED_AuxCmdSelectLike(PaneP, EDCB_BlankPred, &StartPos, &EndPos);
    else if (ED_BufferCIsAlpha(*CurP))
	ED_AuxCmdSelectLike(PaneP, EDCB_WordPred, &StartPos, &EndPos);
    else
	ED_AuxCmdSelectLike(PaneP, EDCB_IdentPred, &StartPos, &EndPos);

    ED_SelPaneP = PaneP;
    ED_SelMarkPos = StartPos;
    ED_PaneFindLoc(PaneP, StartPos, &ED_SelMarkRow, &ED_SelMarkCol, 0, 0);

    PaneP->CursorPos = EndPos;
    RowStart = ED_PaneFindLoc(PaneP, EndPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    ED_PaneMoveAfterCursorMove(PaneP, RowStart, 0, 0);

    ED_BufferPushMark(PaneP->BufP, StartPos);
    ED_XSelSetPrimary(PaneP);
    
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// Set SelMark at Cursor, then pop the last Mark and move the cursor there.
void	ED_CmdExchMark(ED_PanePointer PaneP)
{
    Int32	RowStartPos;

    if (ED_SelPaneP == PaneP) {
	// Already have a selection going, switch Mark+Cursor

	Int32	OldMarkPos = ED_SelMarkPos;
	Int32	OldMarkRow = ED_SelMarkRow;
	Int32	OldMarkCol = ED_SelMarkCol;

	ED_SelMarkPos = PaneP->CursorPos;
	ED_SelMarkRow = PaneP->CursorRow;
	ED_SelMarkCol = PaneP->CursorCol;
	ED_BufferSwapMark(PaneP->BufP, PaneP->CursorPos);	// Swap on Ring too!

	PaneP->CursorPos = OldMarkPos;
	PaneP->CursorRow = OldMarkRow;
	PaneP->CursorCol = OldMarkCol;

	ED_PaneMoveAfterCursorMove(PaneP, -1, 2, 0);

    } else {
    
	ED_SelPaneP = PaneP;
	ED_SelMarkPos = PaneP->CursorPos;
	ED_SelMarkRow = PaneP->CursorRow;
	ED_SelMarkCol = PaneP->CursorCol;

	// Pop old mark to Cursor, push SelMark the find (Col,Row) Loc for Cursor.
	PaneP->CursorPos = ED_BufferSwapMark(PaneP->BufP, ED_SelMarkPos);
	RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
	ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, 2, 0);
	ED_XSelSetPrimary(PaneP);
    }

    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// Each tab is immediately converted into N spaces... where N is the number
// required to reach the next ED_TABSTOP increment.  PaneInsertChars and
// PaneDelSelRange will set BUFMODFLAG
void	ED_CmdInsertTab(ED_PanePointer PaneP)
{
    Int32	Len;
    Int32	Mult;

    if (ED_BufferReadOnly(PaneP->BufP)) return;

    // If there is a sel range, zap it first as it throws off col count!
    ED_PaneDelSelRange(PaneP, 0);

    Len = (((PaneP->CursorCol / ED_TABSTOP) + 1) * ED_TABSTOP) - PaneP->CursorCol;
    if (Len == 0) Len = ED_TABSTOP;

    Mult = ED_CmdMult;
    if (Mult) {
	ED_CmdMult = 1;
	ED_PaneInsertChars(PaneP, Len, ED_TabStr, 0);		// Do this just once
	Mult -= 1;
    }

    if (Mult > 0) {
	ED_CmdMult = Mult;
	ED_PaneInsertChars(PaneP, ED_TABSTOP, ED_TabStr, 0);	// Do this Mult-1 times
    }

    ED_CmdLastId = (Uns64)ED_CmdInsertTab;		// PaneInsertChars overwrites it!!
    ED_CmdMult = 1;

    // PaneInsertChars does all the screen updating!
}

// ******************************************************************************
// Deleting forward, CursorPos does NOT change! (Unless there is a SelRange, and
// PaneDelSelRange takes care of that!)
void	ED_CmdDelNextChar(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		Col, Total = 0;
    Int16		L;
    char		*CurP;

    if (ED_BufferReadOnly(BufP)) return;    
    if (ED_PaneDelSelRange(PaneP, 1)) return;

    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdDelPrevChar(PaneP);
	return;
    }

    ED_BufferPlaceGap(BufP, PaneP->CursorPos, 0);
    while (ED_CmdMult--) {
	if (PaneP->CursorPos == BufP->LastPos) {	// Cursor stays, LastPos changes!
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	    break;					// NO MORE!
	}
	CurP = BufP->GapEndP;
	L = ED_BufferGetUTF8Len(*CurP);
	Total += L;
	BufP->GapEndP += L;
	BufP->LastPos -= L;
    }

    if (Total) {
	ED_BufferAddUndoBlock(BufP, PaneP->CursorPos, Total, ED_UB_DEL, BufP->GapEndP - Total);
	BufP->Flags |= ED_BUFMODFLAG;

	ED_PaneFindLoc(PaneP, BufP->LastPos, &PaneP->BufRowCount, &Col, 0, 0);
	PaneP->BufRowCount += PaneP->StartRowCount;		// Need total, not Pane-Rel
	ED_BufferUpdateMark(PaneP->BufP, PaneP->CursorPos, - Total);

#ifdef DEBUG
    // TEST_FillGap(BufP);
#endif    

	ED_PaneSetScrollBar(PaneP);
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);
    
	ED_PaneUpdateOtherPanes(PaneP, PaneP->CursorPos, - Total);
    }
      
    ED_FrameDrawBlinker(PaneP->FrameP);
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();
}

// ******************************************************************************
// Delete word commands simply ignore/discard SelRange... they just go from where
// the Cursor is!  As a result, the Cursor position does not change.
// Word delete does honor CommandMult!
void	ED_CmdDelNextWord(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		StartPos, Col, Total = 0;
    char		*CurP;

    if (ED_BufferReadOnly(BufP)) return;
    
    ED_SelPaneP = NULL;					// Ignores selection
    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdDelPrevWord(PaneP);
	return;
    }

    StartPos = PaneP->CursorPos;
    if (StartPos == BufP->LastPos) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	return;
    }

    ED_CmdThisId = ED_KillId;				// For KillRing
    ED_BufferPlaceGap(BufP, StartPos, 0);
    while (ED_CmdMult--) {
	if (PaneP->CursorPos == BufP->LastPos) {
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	    break;
	}
	CurP = ED_BufferPosToPtr(PaneP->BufP, PaneP->CursorPos);
	if (! ED_BufferCIsAlpha(*CurP))
	    if (ED_AuxCmdInWordScan(PaneP, 0, 1, BufP->LastPos)) goto DoneDone;
	if (ED_AuxCmdInWordScan(PaneP, 1, 1, BufP->LastPos)) goto DoneDone;
    }
DoneDone:

    Total = PaneP->CursorPos - StartPos;		// This many bytes
    if (Total) {
	ED_BufferAddUndoBlock(BufP, StartPos, Total, ED_UB_DEL | ED_UB_CHUNK, BufP->GapEndP);
    
	BufP->Flags |= ED_BUFMODFLAG;    
	ED_KillRingAdd(BufP->GapEndP, Total, 1);	// Store in KillRing

	PaneP->CursorPos = StartPos;			// The cursor never moves!!
	BufP->GapEndP += Total;
	BufP->LastPos -= Total;
	ED_PaneFindLoc(PaneP, BufP->LastPos, &PaneP->BufRowCount, &Col, 0, 0);
	PaneP->BufRowCount += PaneP->StartRowCount;	// Total count, not Pane-Rel
	ED_BufferUpdateMark(BufP, StartPos, - Total);

#ifdef DEBUG
    // TEST_FillGap(BufP);
#endif

	ED_PaneSetScrollBar(PaneP);
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);

	ED_PaneUpdateOtherPanes(PaneP, PaneP->CursorPos, - Total);
    }
    
    ED_FrameDrawBlinker(PaneP->FrameP);
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();
}

// ******************************************************************************
void	ED_CmdDelPrevChar(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		Col, RowStartPos, Total = 0;
    Int16		L;
    char		*CurP;

    if (ED_BufferReadOnly(BufP)) return;    
    if (ED_PaneDelSelRange(PaneP, 1)) return;

    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdDelNextChar(PaneP);
	return;
    }

    ED_BufferPlaceGap(BufP, PaneP->CursorPos, 0);
    while (ED_CmdMult--) {
	if (PaneP->CursorPos == 0) {
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBegBuf);
	    break;		// NO MORE!
	}
	CurP = BufP->GapStartP - 1, L = 1;
	while (ED_BUFFERISMIDUTF8(*CurP)) CurP--, L++;

	PaneP->CursorPos -= L;
	BufP->GapStartP -= L;				// March GapStartP back to delete!
	BufP->LastPos -= L;
	Total += L;
    }

   
    if (Total) {
	ED_BufferAddUndoBlock(BufP, PaneP->CursorPos, Total, ED_UB_DEL, BufP->GapStartP);

#ifdef DEBUG
	// TEST_FillGap(BufP);
#endif    
 	
	BufP->Flags |= ED_BUFMODFLAG;
	RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 1, 0);
	if (PaneP->PanePos >= PaneP->CursorPos) {
	    PaneP->PanePos = RowStartPos;
	    PaneP->StartRowCount = PaneP->CursorRow;	// Absolute value
	    PaneP->CursorRow = 0;
	} else
	    PaneP->CursorRow -= PaneP->StartRowCount;	// Make it Pane-relative

	ED_PaneFindLoc(PaneP, BufP->LastPos, &PaneP->BufRowCount, &Col, 0, 0);
	PaneP->BufRowCount += PaneP->StartRowCount;		// Need total, not Pane-Rel
	ED_BufferUpdateMark(BufP, PaneP->CursorPos, - Total);

	ED_PaneSetScrollBar(PaneP);
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);
    
	ED_PaneUpdateOtherPanes(PaneP, PaneP->CursorPos, - Total);
    }
    
    ED_FrameDrawBlinker(PaneP->FrameP);
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();    

}

// ******************************************************************************
void	ED_CmdDelPrevWord(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		StartPos, Col, RowStartPos, Total = 0;
    char		*CurP;

    if (ED_BufferReadOnly(BufP)) return;    
    ED_SelPaneP = NULL;					// Ignores selection
    if (ED_CmdMultNeg) {
	ED_CmdMultNeg = 0;
	ED_CmdDelNextWord(PaneP);
	return;
    }
    
    StartPos = PaneP->CursorPos;
    if (StartPos == 0) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBegBuf);
	return;
    }

    ED_CmdThisId = ED_KillId;					// For KillRing    
    ED_BufferPlaceGap(BufP, StartPos, 0);
    while (ED_CmdMult--) {
	// Initially take 1 step BACK!
	PaneP->CursorPos -= 1;
	CurP = BufP->BufStartP + PaneP->CursorPos;		// Always before the gap
	while (ED_BUFFERISMIDUTF8(*CurP)) CurP--, PaneP->CursorPos--;
	if (PaneP->CursorPos == 0) goto DoneDone;

	if (! ED_BufferCIsAlpha(*CurP))
	    if (ED_AuxCmdInWordScan(PaneP, 0, -1, 0)) goto DoneDone;
	if (ED_AuxCmdInWordScan(PaneP, 1, -1, 0)) goto DoneDone;

	// Now move 1 step FORWARD;
	CurP = BufP->BufStartP + PaneP->CursorPos;
	PaneP->CursorPos += ED_BufferGetUTF8Len(*CurP);
    }
DoneDone:
    Total = StartPos - PaneP->CursorPos;
    if (Total) {
	BufP->GapStartP -= Total;				// Incorporate into Gap
	BufP->LastPos -= Total;

	ED_BufferAddUndoBlock(BufP, PaneP->CursorPos, Total, ED_UB_DEL | ED_UB_CHUNK, BufP->GapStartP);    	
	ED_KillRingAdd(BufP->GapStartP, Total, 0);		// Copy to KillRing
	
	BufP->Flags |= ED_BUFMODFLAG;
	RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 1, 0);
	if (PaneP->PanePos >= PaneP->CursorPos) {
	    PaneP->PanePos = RowStartPos;
	    PaneP->StartRowCount = PaneP->CursorRow;		// Absolute value
	    PaneP->CursorRow = 0;				// Pane-relative now
	} else
	    PaneP->CursorRow -= PaneP->StartRowCount;		// Make it Pane-relative


	ED_PaneFindLoc(PaneP, BufP->LastPos, &PaneP->BufRowCount, &Col, 0, 0);
	PaneP->BufRowCount += PaneP->StartRowCount;		// Need total, not Pane-rel
	ED_BufferUpdateMark(BufP, PaneP->CursorPos, - Total);

#ifdef DEBUG
    // TEST_FillGap(BufP);
#endif

	ED_PaneSetScrollBar(PaneP);
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);
    
	ED_PaneUpdateOtherPanes(PaneP, PaneP->CursorPos, - Total);
    }
    
    ED_FrameDrawBlinker(PaneP->FrameP);
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();
}

// ******************************************************************************
// Deletes all space before + after point.
// If CmdMult, just delete space before point.
void	ED_CmdDelHSpace(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		StartPos, EndPos, Len, RowStartPos, Col;
    char		*CurP;

    if (ED_BufferReadOnly(BufP)) return;	
    ED_SelPaneP = NULL;				// Ignore selection

    // Go backwards first!  In the tight loop, CurP runs 1 in advance of StartPos!
    StartPos = PaneP->CursorPos;
    while (StartPos > 0) {
	CurP = ED_BufferPosToPtr(BufP, StartPos - 1);
	if (*CurP != ' ') break;
	StartPos -= 1;
    }

    EndPos = PaneP->CursorPos;
    if (! ED_CmdMultCmd) {
	while (EndPos < BufP->LastPos) {
	    CurP = ED_BufferPosToPtr(BufP, EndPos);
	    if (*CurP != ' ') break;
	    EndPos += 1;
	}
    }

    if (StartPos < EndPos) {
	ED_BufferPlaceGap(BufP, EndPos, 0);
	Len = (EndPos - StartPos);
	BufP->GapStartP -= Len;
	BufP->LastPos -= Len;
	ED_BufferAddUndoBlock(BufP, StartPos, Len, ED_UB_DEL, BufP->GapStartP);
	BufP->Flags |= ED_BUFMODFLAG;

	PaneP->CursorPos = StartPos;
	RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 1, 0);
	if (PaneP->PanePos >= PaneP->CursorPos) {
	    PaneP->PanePos = RowStartPos;
	    PaneP->StartRowCount = PaneP->CursorRow;		// Absolute value
	    PaneP->CursorRow = 0;				// Pane-relative now
	} else
	    PaneP->CursorRow -= PaneP->StartRowCount;		// Make it Pane-relative


	ED_PaneFindLoc(PaneP, BufP->LastPos, &PaneP->BufRowCount, &Col, 0, 0);
	PaneP->BufRowCount += PaneP->StartRowCount;		// Need total, not Pane-rel
	ED_BufferUpdateMark(BufP, StartPos, - Len);
	
#ifdef DEBUG
    // TEST_FillGap(BufP);
#endif

	ED_PaneSetScrollBar(PaneP);
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);
    
	ED_PaneUpdateOtherPanes(PaneP, StartPos, - Len);
    }

    ED_FrameDrawBlinker(PaneP->FrameP);
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();    
}

// ******************************************************************************
// Delete-Indentation intervening Newline + spaces... leaves 1 space.
// RealEmacs calls this function "remove-indentation" which makes no sense!!
void	ED_CmdJoinLines(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		StartPos, EndPos, Len, RowStartPos, Col;
    char		*CurP;

    if (ED_BufferReadOnly(BufP)) return;	
    ED_SelPaneP = NULL;				// Ignore selection

    EndPos = StartPos = ED_BufferGetLineStartPos(BufP, PaneP->CursorPos);
    if (StartPos > 0)
	StartPos -= 1;				// Go back over the Newline

    // Go backwards, eat any spaces at end of prev line!
    while (StartPos > 0) {
	CurP = ED_BufferPosToPtr(BufP, StartPos - 1);
	if (*CurP != ' ') break;
	StartPos -= 1;
    }

    while (EndPos < BufP->LastPos) {
	CurP = ED_BufferPosToPtr(BufP, EndPos);
	if (*CurP != ' ') break;
	EndPos += 1;
    }

    if (StartPos < EndPos) {
	ED_BufferPlaceGap(BufP, EndPos, 1);
	Len = (EndPos - StartPos);
	BufP->GapStartP -= Len;
	ED_BufferAddUndoBlock(BufP, StartPos, Len, ED_UB_DEL, BufP->GapStartP);
	
	*BufP->GapStartP++ = ' ';
	BufP->LastPos -= Len - 1;
	ED_BufferAddUndoBlock(BufP, StartPos, 1, ED_UB_ADD | ED_UB_CHAIN, NULL);
	
	BufP->Flags |= ED_BUFMODFLAG;

	PaneP->CursorPos = StartPos;
	RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 1, 0);
	if (PaneP->PanePos >= PaneP->CursorPos) {
	    PaneP->PanePos = RowStartPos;
	    PaneP->StartRowCount = PaneP->CursorRow;		// Absolute value
	    PaneP->CursorRow = 0;				// Pane-relative now
	} else
	    PaneP->CursorRow -= PaneP->StartRowCount;		// Make it Pane-relative


	ED_PaneFindLoc(PaneP, BufP->LastPos, &PaneP->BufRowCount, &Col, 0, 0);
	PaneP->BufRowCount += PaneP->StartRowCount;		// Need total, not Pane-rel
	ED_BufferUpdateMark(BufP, StartPos, 1 - Len);		// Added 1 space!
	
#ifdef DEBUG
    // TEST_FillGap(BufP);
#endif

	ED_PaneSetScrollBar(PaneP);
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);
    
	ED_PaneUpdateOtherPanes(PaneP, StartPos, - Len);
    }

    ED_FrameDrawBlinker(PaneP->FrameP);
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();    
}

// ******************************************************************************
// ED_CmdDowncaseWord, ED_CmdUpcaseword, and ED_CmdCapitalizeWord
// All take mult arg, but ignore neg sign if any.

void	ED_AuxDoCaseConvert(char *StartP, char *EndP, char Case)
{
    Int16	Enter, InWord;
    char	*CurP;

    CurP = StartP;
    Enter = InWord = 0;
    while (CurP < EndP) {
	if (ED_BufferCIsAlpha(*CurP)) {
	    if (InWord == 0) Enter = 1;
	    InWord = 1;

	    if (((Case == 'l') || (! Enter && (Case == 'c'))) &&
		(('A' <= *CurP) && (*CurP <= 'Z')))
		*CurP += ('a' - 'A');
	    else if (((Case == 'u') || (Enter && (Case == 'c'))) &&
		     (('a' <= *CurP) && (*CurP <= 'z')))
		*CurP -= ('a' - 'A');
	    
	    Enter = 0;
	} else
	    InWord = 0;

	CurP++;
    }
}

void	ED_AuxCaseConvertWord(ED_PanePointer PaneP, char Case)
{
    ED_BufferPointer		BufP = PaneP->BufP;
    Int32			StartPos, EndPos, Total, RowStartPos;
    char			*CurP;

    if (ED_BufferReadOnly(BufP)) return;
    ED_SelPaneP = NULL;

    StartPos = PaneP->CursorPos;
    if (StartPos == BufP->LastPos) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	return;
    }

    while (ED_CmdMult--) {
	if (PaneP->CursorPos == BufP->LastPos) {
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	    break;
	}
	CurP = ED_BufferPosToPtr(PaneP->BufP, PaneP->CursorPos);
	if (! ED_BufferCIsAlpha(*CurP))
	    if (ED_AuxCmdInWordScan(PaneP, 0, 1, BufP->LastPos)) goto DoneDone;
	if (ED_AuxCmdInWordScan(PaneP, 1, 1, BufP->LastPos)) goto DoneDone;
    }

DoneDone:
    EndPos = PaneP->CursorPos;
    Total = EndPos - StartPos;
    if (Total) {
	// Place Gap for convenience, will not actually use it!
	ED_BufferPlaceGap(BufP, StartPos, 0);	
	// First declare old text as DEL block to Undo
	ED_BufferAddUndoBlock(BufP, StartPos, Total, ED_UB_DEL | ED_UB_CHUNK, BufP->GapEndP);
	// Actually convert the case!
	ED_AuxDoCaseConvert(BufP->GapEndP, BufP->GapEndP + Total, Case);
	// Now declare new text as ADD block to Undo
	ED_BufferAddUndoBlock(BufP, StartPos, Total, ED_UB_ADD | ED_UB_CHAIN | ED_UB_CHUNK, BufP->GapEndP);	

	// Cursor moved, so find its new loc and make sure it is ON the Pane!
	RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
	ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, (PaneP->RowCount - 2) / 2, 0);

	// Scrollbars may be changed by PaneMoveAfterCursorMove, if the pane was scrolled.
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);
	// No new text was added, so just a matter of redrawing
	ED_PaneUpdateOtherPanes(PaneP, StartPos, 0);
    }
    
    ED_FrameDrawBlinker(PaneP->FrameP);
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();
}


void	ED_CmdDowncaseWord(ED_PanePointer PaneP)
{
    ED_AuxCaseConvertWord(PaneP, 'l');
}

void	ED_CmdUpcaseWord(ED_PanePointer PaneP)
{
    ED_AuxCaseConvertWord(PaneP, 'u');
}

void	ED_CmdCapitalizeWord(ED_PanePointer PaneP)
{
    ED_AuxCaseConvertWord(PaneP, 'c');
}

// ******************************************************************************
// ED_CmdGotoLine--prompts for a number but will show a default number if the cursor
// is already on (or just after) a number in the buffer.  Will also accept CmdMult.

#define		ED_GOTOMAXLEN	9

char	ED_AuxCharIsDigit(ED_BufferPointer BufP, Int32 Pos)
{
    char	*P;
    
    P = ED_BufferPosToPtr(BufP, Pos);
    if (('0' <= *P) && (*P <= '9'))
	return *P;

    return 0;
}

// Return 1 if done, 0 if failed.
// Will NULL terminate StrP !
Int16	ED_AuxCollectNumber(ED_BufferPointer BufP, Int32 Pos, char *StrP, Int32 MaxLen)
{
    Int32	Len, CurPos;

    if (ED_AuxCharIsDigit(BufP, Pos))
	CurPos = Pos;
    else if ((Pos > 0) && ED_AuxCharIsDigit(BufP, Pos - 1)) {
	CurPos = Pos - 1;
    } else
	return 0;

    // Found a digit, go back to the very first.
    while (--CurPos >= 0)
	if (! ED_AuxCharIsDigit(BufP, CurPos))
	    break;

    // Now go to the end.
    // CurPos is 1 BEFORE the beginning, but PreInc will take care of that.
    Len = 0;
    while (Len < MaxLen) {
	// Will auto NULL-terminate, except if we hit MaxLen
	if (! (StrP[Len++] = ED_AuxCharIsDigit(BufP, ++CurPos)) ) break;
    }
    StrP[Len] = 0;

    return 1;
}

void	ED_AuxGotoLine(ED_PanePointer PaneP, Int32 Numb)
{
    Int32	Pos;

    Pos = 0;
    if (Numb > 1) {
	Numb -= 1;		// Convert to 0-based
	ED_BufferGetLineCount(PaneP->BufP, &Pos, &Numb);
	if (Pos < PaneP->BufP->LastPos)
	    Pos += 1;
    }

    ED_BufferPushMark(PaneP->BufP, Pos);
    PaneP->CursorPos = Pos;
    Pos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    ED_PaneMoveAfterCursorMove(PaneP, Pos, (PaneP->RowCount - 2) / 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);
}

// Limit to ED_GOTOMAXLEN digits... plenty for any document!!
// Return 0 == Good response
// Return 1 == Bad response, try again
Int16	EDCB_GotoLineFunc(void)
{
    ED_PanePointer	PP = ED_QRPaneP;
    char		C;
    Int32		I, Numb;
    
    if ((ED_QRRespLen == 0) || (ED_QRRespLen > ED_GOTOMAXLEN))
	return 1;

    Numb = 0;
    for (I = 0; I < ED_QRRespLen; I++) {
	C = ED_QRRespS[I];
	if ((C < '0') || ('9' < C))
	    return 1;

	Numb = (Numb * 10) + (C - '0');
    }

    ED_QRPaneP = NULL;
    ED_AuxGotoLine(PP, Numb);
    return 0;
}

void	ED_CmdGotoLine(ED_PanePointer PaneP)
{
    char	NumbS[ED_GOTOMAXLEN + 1];
    char	*NP;
    
    // If a number is given, just go there!
    if (ED_CmdMultCmd)
	ED_AuxGotoLine(PaneP, ED_CmdMult);
    else {
	// Check whether Cursor is ON a number!! If so collect and use as default entry!
	NP = (ED_AuxCollectNumber(PaneP->BufP, PaneP->CursorPos, NumbS, ED_GOTOMAXLEN)) ? NumbS : NULL;
	ED_FrameQRAsk(PaneP->FrameP, ED_STR_QueryLineNumber, NP, ED_QRStringType, EDCB_GotoLineFunc, NULL);
    }

}


// ******************************************************************************
// ED_CmdGotoChar--prompts for a number but will show a default number if the cursor
// is already on (or just after) a number in the buffer.  Will also accept CmdMult.

void	ED_AuxGotoChar(ED_PanePointer PaneP, Int32 Numb)
{
    Int32	Pos;

    Pos = Numb - 1;		// 1-based for used, 0-based internally!
    if (Pos < 0) Pos = 0;
    else if (Pos > PaneP->BufP->LastPos) Pos = PaneP->BufP->LastPos;

    ED_BufferPushMark(PaneP->BufP, Pos);
    PaneP->CursorPos = Pos;
    Pos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    ED_PaneMoveAfterCursorMove(PaneP, Pos, (PaneP->RowCount - 2) / 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);
}

// Limit to ED_GOTOMAXLEN digits... plenty for any document!!
// Return 0 == Good response
// Return 1 == Bad response, try again
Int16	EDCB_GotoCharFunc(void)
{
    ED_PanePointer	PP = ED_QRPaneP;
    char		C;
    Int32		I, Numb;
    
    if ((ED_QRRespLen == 0) || (ED_QRRespLen > ED_GOTOMAXLEN))
	return 1;

    Numb = 0;
    for (I = 0; I < ED_QRRespLen; I++) {
	C = ED_QRRespS[I];
	if ((C < '0') || ('9' < C))
	    return 1;

	Numb = (Numb * 10) + (C - '0');
    }

    ED_QRPaneP = NULL;
    ED_AuxGotoChar(PP, Numb);
    return 0;
}


void	ED_CmdGotoChar(ED_PanePointer PaneP)
{
    char	NumbS[ED_GOTOMAXLEN + 1];
    char	*NP;
    
    // If a number is given, just go there!
    if (ED_CmdMultCmd)
	ED_AuxGotoChar(PaneP, ED_CmdMult);
    else {
	// Check whether Cursor is ON a number!! If so collect and use as default entry!
	NP = (ED_AuxCollectNumber(PaneP->BufP, PaneP->CursorPos, NumbS, ED_GOTOMAXLEN)) ? NumbS : NULL;
	ED_FrameQRAsk(PaneP->FrameP, ED_STR_QueryCharNumber, NP, ED_QRStringType, EDCB_GotoCharFunc, NULL);
    }
}

// ******************************************************************************
// PaneInsertChars will take care of setting ED_BUFMODFLAG on these cmds.
//
// Calls on XSelInsertData to 'yank' from system if another owns CLIPBOARD XSel!
// Even then, if numeric command, so expect POP yank, just pretend the external
// XSel is on the KillRing and Pop around it.

Int16	ED_YankIsExternal;

void	ED_CmdYank(ED_PanePointer PaneP)
{
    char	*DataP;
    Int32	DataLen;
    Int32	StartPos;
    Int32	PopCount;

    if (ED_BufferReadOnly(PaneP->BufP)) return;
    ED_YankIsExternal = 0;
    
    if (ED_ClipOwn == 0) {
	if (ED_CmdMultCmd == 0) {
	    ED_XSelInsertData(PaneP, ED_ClipAtom);
	    ED_YankIsExternal = 1;			// Comes from outside... add to KillRing later!
	    ED_CmdThisId = ED_YankId;			// To allow for YankPop as next Cmd!
	    return;
	}
    
	PopCount = (ED_CmdMultNeg) ? - ED_CmdMult : ED_CmdMult;
    } else
	PopCount = (ED_CmdMultNeg) ? - ED_CmdMult : ED_CmdMult - 1;
	
    ED_KillRingYank(PopCount, &DataP, &DataLen, 1);

    StartPos = PaneP->CursorPos;			// Start of insertion range?
    if ((ED_SelPaneP == PaneP) && (ED_SelMarkPos < StartPos))
	StartPos = ED_SelMarkPos;

    ED_CmdMult = 1;					// For PaneInsertChars!

    ED_PaneInsertChars(PaneP, DataLen, DataP, 0);
    ED_BufferPushMark(PaneP->BufP, StartPos);
    ED_CmdThisId = ED_YankId;
}

// ******************************************************************************
void	ED_CmdYankPop(ED_PanePointer PaneP)
{
    char	*DataP;
    Int32	DataLen;
    Int32	PopCount;

    if (ED_BufferReadOnly(PaneP->BufP)) return;    
    if (ED_CmdLastId != ED_YankId) return;
    ED_CmdThisId = ED_YankId;

    // Use the Mark pushed by the last Yank, to create a SelRange--Cursor
    // is at the end...
    ED_SelPaneP = PaneP;
    ED_SelMarkPos = ED_BufferGetMark(PaneP->BufP, 0);
    ED_PaneFindLoc(PaneP, ED_SelMarkPos, &ED_SelMarkRow, &ED_SelMarkCol, 0, 0);

    // This last yank *MAY* have come from an XSel CLIPBOARD insertion--i.e. from
    // outside the system.  So it should be added to the KillRing before proceeding!
    if (ED_YankIsExternal && (ED_SelMarkPos < PaneP->CursorPos)) {
	ED_BufferPlaceGap(PaneP->BufP, ED_SelMarkPos, 0);
	ED_KillRingAdd(PaneP->BufP->GapEndP, PaneP->CursorPos - ED_SelMarkPos, 1);
    }
    ED_YankIsExternal = 0;

    PopCount = (ED_CmdMultNeg) ? - ED_CmdMult : ED_CmdMult;
    ED_KillRingYank(PopCount, &DataP, &DataLen, 1);
    ED_CmdMult = 1;
    ED_PaneInsertChars(PaneP, DataLen, DataP, 0);
}

// ******************************************************************************
// No Count, Kill to end of line, NOT NewLine
// Explicit 0 Count, Kill to beginning of line
// Explicit N Count, Kill N Lines+NewLines
// Explicit -N Count, Kill N PREVIOUS Lines+NewLines
void	ED_CmdKillLine(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		StartPos, EndPos, Total, RowStartPos;
    char		*CurP;
    Int16		Forward;

    if (ED_BufferReadOnly(BufP)) return;    
    EndPos = StartPos = PaneP->CursorPos;
    Forward = 1;

    if (! ED_CmdMultCmd) {
	// Kill to end of this line, not NewLine, unless ON NewLine
	if (StartPos == BufP->LastPos) {
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	    return;
	}
	CurP = ED_BufferPosToPtr(BufP, StartPos);
	if (*CurP == '\n')
	    EndPos = StartPos + 1;
	else
	    EndPos = ED_BufferGetLineEndPos(BufP, StartPos);
	
    } else if (ED_CmdMult == 0) {
	// Kill backwards to beginning of this line
	if (StartPos == 0) {
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBegBuf);
	    return;
	}
	StartPos = ED_BufferGetLineStartPos(BufP, EndPos);
	Forward = 0;			// For KillRing!
	
    } else if (ED_CmdMultNeg) {
	// Kill to beginning of THIS line, plus N previous lines!
	if (StartPos == 0) {
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBegBuf);
	    return;
	}

	StartPos = ED_BufferGetLineStartPos(BufP, EndPos);
	while (ED_CmdMult--) {
	    if (StartPos > 0)
		StartPos = ED_BufferGetLineStartPos(BufP, StartPos - 1);
	}

	Forward = 0;
    } else {
	// Kill N Lines + NewLine
	if (StartPos == BufP->LastPos) {
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoEndBuf);
	    return;
	}

	while (ED_CmdMult--) {
	    EndPos = ED_BufferGetLineEndPos(BufP, EndPos);
	    if (EndPos < BufP->LastPos)
		EndPos += 1;		// Get over the /Newline
	    else
		break;
	}
    }

    Total = EndPos - StartPos;
    if (Total) {
    	BufP->Flags |= ED_BUFMODFLAG;
	ED_CmdThisId = ED_KillId;				// For KillRing!
	
	if (! Forward) {
	    PaneP->CursorPos = StartPos;
	    RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 1, 0);
	    if (PaneP->PanePos >= PaneP->CursorPos) {		// Move Pane up!
		PaneP->PanePos = RowStartPos;
		PaneP->StartRowCount = PaneP->CursorRow;
		PaneP->CursorRow = 0;				// Pane-relative now
	    } else
		PaneP->CursorRow -= PaneP->StartRowCount;	// Pane-relative
	}

	ED_BufferPlaceGap(BufP, StartPos, 0);
	
	ED_BufferAddUndoBlock(BufP, StartPos, Total, ED_UB_DEL | ED_UB_CHUNK, BufP->GapEndP);
	ED_KillRingAdd(BufP->GapEndP, Total, Forward);

	BufP->GapEndP += Total;
	BufP->LastPos -= Total;
	ED_PaneFindLoc(PaneP, BufP->LastPos, &PaneP->BufRowCount, &RowStartPos, 0, 0);	// Recycled RowStartPos
	PaneP->BufRowCount += PaneP->StartRowCount;		// Need Abs not Pane-rel
	ED_BufferUpdateMark(BufP, StartPos, - Total);

#ifdef DEBUG
    // TEST_FillGap(BufP);
#endif

	ED_PaneSetScrollBar(PaneP);
	ED_PaneDrawText(PaneP);
	ED_PaneDrawScrollBar(PaneP, 0);

	ED_PaneUpdateOtherPanes(PaneP, PaneP->CursorPos, - Total);
    }
    
    ED_FrameDrawBlinker(PaneP->FrameP);
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();
}

// ******************************************************************************
// If ED_SelPaneP, kill the sel rgn
// Else, go from Cursor to last Mark
// DelSelRange will set BUFMODFLAG
void	ED_CmdKillRegion(ED_PanePointer PaneP)
{
    if (ED_BufferReadOnly(PaneP->BufP)) return;
    
    // Set ThisId to ED_KillId early on, so PaneDelSelRange will save to KillRing.
    ED_CmdThisId = ED_KillId;

    // If there is a SelRange, kill it, save it, update and done.
    if (ED_PaneDelSelRange(PaneP, 1)) return;

    // Use the last Mark to create and then Kil a SelRange
    ED_SelPaneP = PaneP;
    ED_SelMarkPos = ED_BufferGetDiffMark(PaneP->BufP, PaneP->CursorPos);
    ED_PaneFindLoc(PaneP, ED_SelMarkPos, &ED_SelMarkRow, &ED_SelMarkCol, 0, 0);
    ED_PaneDelSelRange(PaneP, 1);
}

// ******************************************************************************
// ED_CmdCopyRegion

Int32	ED_AuxCopySelRange(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		StartPos, EndPos, Total;

    if ((ED_SelPaneP != PaneP) || (ED_SelMarkPos == PaneP->CursorPos))
	return 0;

    if (PaneP->CursorPos < ED_SelMarkPos) {
	StartPos = PaneP->CursorPos;
	EndPos = ED_SelMarkPos;
    } else {
	StartPos = ED_SelMarkPos;
	EndPos = PaneP->CursorPos;
    }

    // BufferPlaceGap to get it out of the way, nothing is being changed!
    ED_BufferPlaceGap(BufP, StartPos, 0);
    Total = EndPos - StartPos;
    ED_KillRingAdd(BufP->GapEndP, Total, 1);

    return Total;
}

void	ED_CmdCopyRegion(ED_PanePointer PaneP)
{
    // Set so KillRingAdd does the right thing
    ED_CmdThisId = ED_KillId;

    // If there is a SelRange, add it to KillRing, update and done.
    if (ED_AuxCopySelRange(PaneP)) return;

    // Use the Mark to create, then copy a SelRange
    ED_SelPaneP = PaneP;
    ED_SelMarkPos = ED_BufferGetDiffMark(PaneP->BufP, PaneP->CursorPos);
    ED_PaneFindLoc(PaneP, ED_SelMarkPos, &ED_SelMarkRow, &ED_SelMarkCol, 0, 0);
    ED_AuxCopySelRange(PaneP);

    // Now turn off the SelRange!
    ED_SelPaneP = NULL;
}

// ******************************************************************************
// Clear the ModFlag with NO numeric arg
// Set the ModFlag with ANY numeric arg
void	ED_CmdModBuffer(ED_PanePointer PaneP)
{
    if (ED_BufferReadOnly(PaneP->BufP)) return;
    if (ED_CmdMultCmd) {
	// Set the buffer as MODIFIED--do-the-opposite day!
	// No UBlock if it is already MOD!
	if (! (PaneP->BufP->Flags & ED_BUFMODFLAG))
	    ED_BufferAddUndoBlock(PaneP->BufP, PaneP->CursorPos, 0, ED_UB_ADD, NULL);
	    
	PaneP->BufP->Flags |= ED_BUFMODFLAG;
	PaneP->BufP->Flags &= ~ED_BUFCLEANUNDOFLAG;
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoModFlagSet);
    } else {
	PaneP->BufP->Flags &= ~ ED_BUFMODFLAG;
	PaneP->BufP->Flags |= ED_BUFCLEANUNDOFLAG;
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoModFlagClear);
	
	ED_BufferAddUndoBlock(PaneP->BufP, 0, 0, ED_UB_ADD | ED_UB_SAVE, NULL);
    }

    ED_BufferUpdateModeLines(PaneP->BufP);
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// ED_CmdPopUpMarkList -- Pop-up list of Marks.  Use callbacks for Draw and Select.

Int16	EDCB_PUMLDraw(ED_PUPointer PUP, Int32 CurEntry, Int32 StartX, Int32 CharLimit, Int32 StartY)
{
    ED_BufferPointer	BufP = PUP->PaneP->BufP;
    Int32		Pos, Col, DeltaRow, MarkRow, MarkCol, MarkPos, MarkIndex;
    Int32		LineX, LineY, CharCount, LineCount;
    Int16		DrawFlags, LineWrap, Partial, Color;
    char		* CharP;

    DrawFlags = 0;
    if (PUP->EntryCharScroll) DrawFlags |= ED_PU_DRAWLSCROLLFLAG;
    
    MarkIndex = (BufP->MarkRingIndex - CurEntry) & ED_MARKRINGMASK;
    MarkPos = BufP->MarkRingArr[MarkIndex];
    Color = (CurEntry == PUP->EntrySel) ? ED_SelOrange : ED_Gray;

    Pos = 0;
    DeltaRow = MarkRow = MarkCol = 0;
    if (MarkPos) {
	DeltaRow = PUP->EntryRows / 2;
	ED_PaneFindLoc(PUP->PaneP, MarkPos, &MarkRow, &MarkCol, 1, 0);
	Pos = ED_BufferGetRowStartPos(BufP, MarkPos, MarkCol);
	Pos = ED_BufferGetPosMinusRows(BufP, Pos, &DeltaRow, PUP->PaneP->FrameP->RowChars);
    }

    // Indicate the MarkPos by coloring its background... can be off ClipRect!
    LineX = StartX + ((MarkCol - PUP->EntryCharScroll) * ED_Advance);
    LineY = StartY + (DeltaRow * ED_Row);
    XftDrawRect(PUP->XftDP, &ED_XCArr[Color], LineX, LineY + 1, ED_Advance, ED_Height);
    
    // Draw all lines in the entry, starting at Pos!
    LineCount = 0;
    LineY = StartY;
    while (LineCount < PUP->EntryRows) {
	LineX = StartX - (PUP->EntryCharScroll * ED_Advance);
	Col = 0;
	do {
	    CharP = ED_PaneGetDrawRow(PUP->PaneP, &Pos, &Col, &CharCount, &LineWrap, &Partial);
	    // Cursor/Mark can be after last char on line, so add an extension col!
	    if (Col + 1 > PUP->EntryCharMax) PUP->EntryCharMax = Col + 1;
	    if ((Col + 1 - PUP->EntryCharScroll) > CharLimit) DrawFlags |= ED_PU_DRAWRSCROLLFLAG;
	    if (CharCount) {
		XftDrawStringUtf8(PUP->XftDP, &ED_XCArr[ED_Black], ED_XFP, LineX, LineY + ED_Ascent, (XftChar8 *)CharP, CharCount);
		LineX += (Col * ED_Advance);
		if (LineWrap || (Col > (CharLimit + PUP->EntryCharScroll))) break;
	    }

	    if (Pos >= BufP->LastPos) break;

	} while (Partial);

	LineCount += 1;
	LineY += ED_Row;
    }

    return DrawFlags;
}

void	EDCB_PUMLSelect(ED_PUPointer PUP)
{
    ED_PanePointer		PaneP = PUP->PaneP;
    ED_BufferPointer		BufP = PaneP->BufP;
    Int32			MarkPos, RowStartPos;

    BufP->MarkRingIndex = (BufP->MarkRingIndex - PUP->EntrySel) & ED_MARKRINGMASK;    
    MarkPos = BufP->MarkRingArr[BufP->MarkRingIndex];
    if (PUP->EntrySel) ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoMarkPop);

    PaneP->CursorPos = MarkPos;
    RowStartPos = ED_PaneFindLoc(PaneP, PaneP->CursorPos, &PaneP->CursorRow, &PaneP->CursorCol, 0, 0);
    ED_PaneMoveAfterCursorMove(PaneP, RowStartPos, 2, 0);
    ED_FrameDrawAll(PaneP->FrameP);
}

// Ignores ED_SelPaneP... if there is one, it will re-adjust to new CursorPos,
// if a Mark is selected--or be restored if no Marks are selected.
void	ED_CmdPopUpMarkList(ED_PanePointer PaneP)
{
    char	Str[128];
    Int32	StrLen;

    StrLen = sprintf(Str, ED_STR_PUMarkList, 100, PaneP->BufP->FileName);

    ED_PUListConfigure(&ED_PUR, Str, StrLen, ED_MARKRINGCOUNT, ED_PUML_ENTRYROWS, ED_PUML_LEFTCOLCHARS);
    ED_PUListCreate(&ED_PUR, PaneP, EDCB_PUMLSelect, EDCB_PUMLDraw, NULL);
}

// ******************************************************************************
// ED_CmdPopUpKillList -- Pop-up KillRing entries.  CB to Draw and Select.

Int16	EDCB_PUKLDraw(ED_PUPointer PUP, Int32 CurEntry, Int32 StartX, Int32 CharLimit, Int32 StartY)
{
    Int16		CurI;
    char		*CurKillP, *TextP;
    Int32		TextLen, ByteCount, CharCount, RowCount;
    Int32		EmptyLen = strlen(ED_STR_PUEmptyList);
    Int16		DrawFlags;

    DrawFlags = 0;
    CurI = (ED_KillRing.TopI - CurEntry) & ED_KILLRINGMASK;
    CurKillP = (CurI == ED_KillRing.TopI) ? ED_KillRing.TopP : ED_KillRing.RestP;

    StartY += ED_Ascent;
    StartX -= (PUP->EntryCharScroll * ED_Advance);
    TextLen = ED_KillRing.EltArr[CurI].Len;
    if (TextLen == 0)
	XftDrawString8(PUP->XftDP, &ED_XCArr[ED_Gray], ED_XFP, StartX, StartY, (XftChar8 *)ED_STR_PUEmptyList, EmptyLen);
    else {
	if (PUP->EntryCharScroll) DrawFlags |= ED_PU_DRAWLSCROLLFLAG;
	RowCount = 0;
	TextP = CurKillP + ED_KillRing.EltArr[CurI].Pos;
	
	while ( (ByteCount = ED_UtilGetLineEnd(TextP, TextLen, &CharCount)) ) {
	    if (CharCount > PUP->EntryCharMax) PUP->EntryCharMax = CharCount;
	    if ((CharCount - PUP->EntryCharScroll) > CharLimit) DrawFlags |= ED_PU_DRAWRSCROLLFLAG;
	    // Relies on the ClipRect, can start/stop OFF screen!
	    if (CharCount)
		XftDrawStringUtf8(PUP->XftDP, &ED_XCArr[ED_Black], ED_XFP, StartX, StartY, (XftChar8 *)TextP, ByteCount);

	    ByteCount += 1;		// To skip over the \n char!
	    TextLen -= ByteCount;
	    TextP += ByteCount;
	    RowCount += 1;
	    StartY += ED_Row;
	    if ((TextLen <= 0) || (RowCount == PUP->EntryRows)) break;
	}
	if (TextLen > 0) DrawFlags |= ED_PU_DRAWMORELINESFLAG;
    }

    return DrawFlags;
}

// Selection basically does an ED_CmdYank!
// The list display always starts from TopI, so this command will first restore
// the YankI to the TopI, then YankPop to the EntrySel.
//
// NOTE:	This is a callback from the PU window... so underlying Frame/Pane
//		have had a FocusOut and lost the SelPaneP--stashed in LastSelPaneP.
//		Restore SelPaneP before calling PaneInsertChars (so it does not
//		ignore the SelectionRange if there was one).
//
// NOTE:	This function will *NOT* return to FrameHandleKey or FrameCommandMatch!
//		CmdPopUpKillList was called from FrameCommandMatch... and returned
//		to it already... It created a window which had a life of its own.
//		There is NO ONE else to set ED_CmdLastId, and must be set here.
//
void	EDCB_PUKLSelect(ED_PUPointer PUP)
{
    ED_PanePointer	PaneP = PUP->PaneP;
    char		*DataP;
    Int32		DataLen;
    Int32		StartPos;

    if (ED_BufferReadOnly(PaneP->BufP)) return;
    
    ED_KillRing.YankI = ED_KillRing.TopI;
    ED_KillRingYank(PUP->EntrySel, &DataP, &DataLen, 0);

    ED_CmdMult = 1;

    ED_SelPaneP = ED_LastSelPaneP;			// RESTORE (Frame lost focus)
	StartPos = PaneP->CursorPos;			// Start of insertion range?
	if ((ED_SelPaneP == PaneP) && (ED_SelMarkPos < StartPos))
	    StartPos = ED_SelMarkPos;
    
	ED_PaneInsertChars(PaneP, DataLen, DataP, 0);
	ED_BufferPushMark(PaneP->BufP, StartPos);
    ED_LastSelPaneP = ED_SelPaneP;			// STORE (for when Frame gets focus)
    ED_CmdThisId = ED_CmdLastId = ED_YankId;		// No one else will set it!
}

// Will ignore SelPaneP... if there is one, selection from the PU will replace it.
void	ED_CmdPopUpKillList(ED_PanePointer PaneP)
{
    ED_PUListConfigure(&ED_PUR, ED_STR_PUKillList, strlen(ED_STR_PUKillList), ED_KILLRINGCOUNT, ED_PUKL_ENTRYROWS, ED_PUKL_LEFTCOLCHARS);
    ED_PUListCreate(&ED_PUR, PaneP, EDCB_PUKLSelect, EDCB_PUKLDraw, NULL);
}

// ******************************************************************************
// ED_CmdPopUpCmdList -- Pop-up list of all commands.  CB to Draw and Select.
//
// Display has to go through FReg structure to find functions, so uses CDP as a
// Cache/stash of sorts, otherwise it would have to constantly count from the
// first FReg.
//
// If the Cmd selected itself requires another PopUp window (e.g. chosing to
// execute this very same command from the PopUp!) then we have to wait until
// this PopUp is killed and Focus returns to the Frame window before launching
// another PopUp.  (XLib complicates things by NOT sending a FocusOut to the
// Frame window!)  So the PUP pointer is saved in ED_CLPUP... and the main
// event loop will call ED_FrameExecPUCmd when the Frame gets focus!!

typedef struct {
    ED_FRegPointer		FRP;
    Int32			Count;
} ED_CmdDispRecord, * ED_CmdDispPointer;

ED_PUPointer	ED_CLPUP;		// PUP is saved here.

// When done, CDP will have the right FRP!
void	ED_AuxPUCLFindNth(ED_CmdDispPointer CDP, Int32 N)
{
    if (N == CDP->Count) return;	// Already have it
    
    if (N > CDP->Count)			// Go Delta steps farther
	N -= CDP->Count;
    else {				// Overshot, start at beginning
	CDP->Count = 0;
	CDP->FRP = ED_FirstFRegP;
    }

    while (N && CDP->FRP) {
	CDP->FRP = CDP->FRP->SortNextP;
	CDP->Count += 1;
	N -= 1;
    }
}

// ClipRect is in effect... helps DrawString!
// Return DrawFlags.
Int16	EDCB_PUCLDraw(ED_PUPointer PUP, Int32 CurEntry, Int32 StartX, Int32 CharLimit, Int32 StartY)
{
    Int16		DrawFlags = 0;
    ED_FBindPointer	CurBP;
    ED_CmdDispPointer	CDP = PUP->DataP;
    char		BindStr[64], PrintStr[128];
    Int32		Len, TotalLen, BindCount;

    StartY += ED_Ascent;
    StartX -= (PUP->EntryCharScroll * ED_Advance);
    if (PUP->EntryCharScroll) DrawFlags |= ED_PU_DRAWLSCROLLFLAG;

    ED_AuxPUCLFindNth(CDP, CurEntry);
    Len = sprintf(PrintStr, ED_STR_PUCmdFunc, CDP->FRP->NameP);
    if (Len > PUP->EntryCharMax) PUP->EntryCharMax = Len;
    if ((Len - PUP->EntryCharScroll) > CharLimit) DrawFlags |= ED_PU_DRAWRSCROLLFLAG;
    XftDrawString8(PUP->XftDP, &ED_XCArr[ED_Black], ED_XFP, StartX, StartY, (XftChar8 *)PrintStr, Len);

    // Now do the Bindings!!

    StartY += ED_Row;
    CurBP = CDP->FRP->FirstBindP;
    BindCount = 0, TotalLen = 0;
    while (CurBP) {
	// Strip trailing SPACE from BindStr.
	Len = ED_KeyStrPrint(CurBP->KeyStr, BindStr);
	if (Len) BindStr[Len - 1] = 0;

	if (BindCount == 0)
	    Len = sprintf(PrintStr, ED_STR_PUCmdFirstBind, BindStr);
	else
	    Len = sprintf(PrintStr, ED_STR_PUCmdOtherBind, BindStr);

	XftDrawString8(PUP->XftDP, &ED_XCArr[ED_Green], ED_XFP, StartX, StartY, (XftChar8 *)PrintStr, Len);
	TotalLen += Len;
	BindCount += 1;
	StartX += (Len * ED_Advance);
	CurBP = CurBP->FuncNextP;
    }

    if (BindCount == 0) {
	TotalLen = sprintf(PrintStr, ED_STR_PUCmdNoBind, CDP->FRP->NameP);
	XftDrawString8(PUP->XftDP, &ED_XCArr[ED_Black], ED_XFP, StartX, StartY, (XftChar8 *)PrintStr, TotalLen);
    }

    if (TotalLen > PUP->EntryCharMax) PUP->EntryCharMax = Len;
    if ((Len - PUP->EntryCharScroll) > CharLimit) DrawFlags |= ED_PU_DRAWRSCROLLFLAG;

    return DrawFlags;
}

// NOTE:	When this function is executing, the PU window has just been
//		destroyed, but the underlying FrameP will NOT have focus yet!
//		So restore ED_SelPaneP before executing any commands, in case
//		there was a SelRange.
//
// NOTE:	Select simply stashes the selected command, and we exec on exit!
//		Cannot launch ANOTHER PU command while in the midst of one!

void	EDCB_PUCLSelect(ED_PUPointer PUP)
{
    ED_CmdDispPointer	CDP = PUP->DataP;
    ED_FramePointer	FP = PUP->PaneP->FrameP;
    XEvent		XE = {0};		// No Valhalla if no Init--Valgrind!

    ED_AuxPUCLFindNth(CDP, PUP->EntrySel);
    PUP->CLCmdFP = CDP->FRP->FuncP;
    ED_FrameSetEchoS(ED_ECHOMSGMODE, CDP->FRP->NameP);
    ED_FrameDrawEchoLine(FP);

    // Used to just set ED_FrameHasPUCmd... but better to send explicit
    // client message--XLib was doing strange things when ED_FrameHasPUCmd was
    // being serviced (creating another PopUp window) during FocusIn event.
    XE.type = ClientMessage;
    XE.xclient.display = ED_XDP;
    XE.xclient.window = FP->XWin;
    XE.xclient.message_type = 0;
    XE.xclient.format = 32;
    XE.xclient.data.l[0] = ED_PUExecAtom;
    XSendEvent(ED_XDP, FP->XWin, 0, 0, &XE);
}

// ED_FrameExecPUCmd is called when the frame gets the PUExecAtom client msg.
// PUP data had to be saved in ED_CLPUP for easy access here.
void	ED_FrameExecPUCmd()
{
    ED_PanePointer	PaneP = ED_CLPUP->PaneP;
    ED_FRegFuncP	FuncP = ED_CLPUP->CLCmdFP;

    ED_CmdMult = 1;
    ED_CmdThisId = (Uns64)FuncP;
	ED_CLPUP->PaneP = NULL;		// Cleanup
	ED_CLPUP->CLCmdFP = NULL;
	(*FuncP)(PaneP);		// Exec
    ED_CmdLastId = ED_CmdThisId;
}

// Will ignore SelPaneP... if there is a Sel Range, selection from the PU will
// impact it as appropriate.
void	ED_CmdPopUpCmdList(ED_PanePointer PaneP)
{
    static ED_CmdDispRecord	DispStash;

    DispStash.FRP = ED_FirstFRegP;
    DispStash.Count = 0;
    ED_CLPUP = &ED_PUR;
 
    ED_PUListConfigure(&ED_PUR, ED_STR_PUCmdList, strlen(ED_STR_PUCmdList),
		       ED_FRegStore.UsedBlocks, ED_PUCL_ENTRYROWS, ED_PUCL_LEFTCOLCHARS);
    ED_PUListCreate(&ED_PUR, PaneP, EDCB_PUCLSelect, EDCB_PUCLDraw, &DispStash);
}

// ******************************************************************************
// ED_CmdPopUpBufferList -- Pop-up list of all buffers.  CB to Draw and Select.
//
// Display has to go through all the buffers, possible again and again.
// So uses a BDP as a cache/stash of sorts to save on counting from
// the first Buffer each time.

typedef struct {
    ED_BufferPointer	BP;
    Int32		Count;
} ED_BufDispRecord, * ED_BufDispPointer;

void	ED_AuxPUBLFindNth(ED_BufDispPointer BDP, Int32 N)
{
    if (N == BDP->Count) return;	// Already have it!
    
    if (N > BDP->Count)
	N -= BDP->Count;		// Go N more steps
    else {
	BDP->Count = 0;
	BDP->BP = ED_FirstBufP;
    }

    while (N && BDP->BP) {
	BDP->BP = BDP->BP->NextBufP;
	BDP->Count += 1;
	N -= 1;
    }
}

// ClipRect is in effect... helps DrawString!
// Returns DrawFlags.
// Second line:  Buffer Path
// Third line:  "** CharCount"
Int16	EDCB_PUBLDraw(ED_PUPointer PUP, Int32 CurEntry, Int32 StartX, Int32 CharLimit, Int32 StartY)
{
    ED_BufDispPointer	BDP = PUP->DataP;
    ED_BufferPointer	BufP;
    Int16		DrawFlags = 0;
    char		DirName[32];
    char		Str[128];
    Int32		Len;
    Int16		Color;
    char		MC;

    StartY += ED_Ascent;
    StartX -= (PUP->EntryCharScroll * ED_Advance);
    if (PUP->EntryCharScroll) DrawFlags |= ED_PU_DRAWLSCROLLFLAG;

    // Get which Buf we are drawing into BDP.
    ED_AuxPUBLFindNth(BDP, CurEntry);
    BufP = BDP->BP;

    Color = (BufP->Flags & ED_BUFNOFILEFLAG) ? ED_Brown : ED_Black;
    // Line 1:  Buffer name + (<DirName> if Buf has NameCollision)
    if (BufP->Flags & ED_BUFNAMECOLFLAG)
	sprintf(DirName, "<%.*s>", 30, BufP->PathName + BufP->DirNameOffset);
    else
	DirName[0] = 0;

    // Draw with same length limits as ModeLine on Pane.
    Len = sprintf(Str, "%.*s %.*s", 32, BufP->FileName, 32, DirName);
    if (Len > PUP->EntryCharMax) PUP->EntryCharMax = Len;
    if ((Len - PUP->EntryCharScroll) > CharLimit) DrawFlags |= ED_PU_DRAWRSCROLLFLAG;
    XftDrawString8(PUP->XftDP, &ED_XCArr[Color], ED_XFP, StartX, StartY, (XftChar8 *)Str, Len);

    // Line 2:  Full pathname (DirNameOffset has already done most of the strlen on PathName!)
    // Use Gray IFF BufP has no file
    StartY += ED_Row;
    if (BufP->Flags & ED_BUFREADONLYFLAG) {
	MC = '%';						// For Line 3
	Len = strlen(ED_STR_BufInfoPath);
	if (Len > PUP->EntryCharMax) PUP->EntryCharMax = Len;
	if ((Len - PUP->EntryCharScroll) > CharLimit) DrawFlags |= ED_PU_DRAWRSCROLLFLAG;
	XftDrawString8(PUP->XftDP, &ED_XCArr[Color], ED_XFP, StartX, StartY, (XftChar8 *)ED_STR_BufInfoPath, Len);
 
	} else {
	MC = (BufP->Flags & ED_BUFMODFLAG) ? '*' : '-';		// For Line 3
        Len = BufP->DirNameOffset + strlen(BufP->PathName + BufP->DirNameOffset);
	if (Len > PUP->EntryCharMax) PUP->EntryCharMax = Len;
	if ((Len - PUP->EntryCharScroll) > CharLimit) DrawFlags |= ED_PU_DRAWRSCROLLFLAG;
	XftDrawString8(PUP->XftDP, &ED_XCArr[Color], ED_XFP, StartX, StartY, (XftChar8 *)BufP->PathName, Len);
     }

    // Line 3: "-:**- CharCount"
    StartY += ED_Row;
    Len = sprintf(Str, ED_STR_BufMemDisp, MC, MC, BufP->LastPos,
		  BufP->BufEndP - BufP->BufStartP, BufP->USCount, BufP->USTotalSize);
		      
    if (Len > PUP->EntryCharMax) PUP->EntryCharMax = Len;
    if ((Len - PUP->EntryCharScroll) > CharLimit) DrawFlags |= ED_PU_DRAWRSCROLLFLAG;
    XftDrawString8(PUP->XftDP, &ED_XCArr[Color], ED_XFP, StartX, StartY, (XftChar8 *)Str, Len);

    return DrawFlags;
}

void	EDCB_PUBLSelect(ED_PUPointer PUP)
{
    ED_BufDispPointer	BDP = PUP->DataP;
    ED_PanePointer	PaneP = PUP->PaneP;

    ED_AuxPUBLFindNth(BDP, PUP->EntrySel);
    // BDP->BP is the selected buffer pointer!
    if (PaneP->BufP != BDP->BP)
	ED_PaneGetNewBuf(PaneP, BDP->BP);

    ED_FrameDrawAll(PaneP->FrameP);
}


void	ED_CmdPopUpBufferList(ED_PanePointer PaneP)
{
    Int32			BufCount = 0;
    ED_BufferPointer		BufP = ED_FirstBufP;
    static ED_BufDispRecord	DispStash;

    while (BufP) {
	BufP = BufP->NextBufP;
	BufCount += 1;
    }

    DispStash.Count = 0;
    DispStash.BP = ED_FirstBufP;

    ED_PUListConfigure(&ED_PUR, ED_STR_PUBufList, strlen(ED_STR_PUBufList),
		       BufCount, ED_PUBL_ENTRYROWS, ED_PUBL_LEFTCOLCHARS);
    ED_PUListCreate(&ED_PUR, PaneP, EDCB_PUBLSelect, EDCB_PUBLDraw, &DispStash);
}

// ******************************************************************************
// ED_CmdWriteFile -- Uses QR machinery to get Path/filename from user.
// Has a second layer of QR to confirm overwrite if necessary... therefore two
// callback functions, one for write, and one for over-write.

// Will truncate the file to length--O_TRUNC
// Return 0 --> Good response, accepted
// Return 1 --> Bad response, try again
Int16	EDCB_OverwriteFunc(void)
{
    ED_PanePointer	PaneP = ED_QRPaneP;
    ED_BufferPointer	BufP = PaneP->BufP;
    char		C;
    Int16		WFail;
    Int32		WErr;

    if (ED_QRRespLen == 1) {

	C = ED_QRRespS[0];
	if ((C == 'n') || (C == 'N')) {
	    // DO NOT overwrite!  Just exit.
	    ED_QRPaneP = NULL;
	    return 0;
	}

	if ((C == 'y') || (C == 'Y')) {
	    // DO OVERWRITE the existing file!
	    // Include O_TRUC so file can get smaller than before
	    ED_FD = open(ED_FullPath, O_TRUNC | O_WRONLY, 0);
	    if (ED_FD == -1) {
		ED_QRPaneP = NULL;
		ED_FrameSetEchoError(ED_STR_EchoOpenFailed, errno);
		ED_FrameDrawAll(PaneP->FrameP);
		return 0;
	    }

	    // Write head is initially at byte 0, perfect.
	    WFail = ED_BufferWriteFile(BufP, ED_FD);
	    WErr = errno;
	    close(ED_FD);

	    // Exit QR Mode, update Pane/Buf and continue
	    ED_QRPaneP = NULL;
	    if (WFail) ED_FrameSetEchoError(ED_STR_EchoWriteFailed, WErr);
	    else ED_BufferDidWrite(BufP, PaneP->FrameP);

	    ED_FrameDrawAll(PaneP->FrameP);
	    return 0;
	}
    }

    // Bad response, answer again!
    return 1;
}

// Return 0 == Good response
// Return 1 == Bad response
Int16	EDCB_WriteFileFunc(void)
{
    ED_PanePointer	PaneP = ED_QRPaneP;
    ED_BufferPointer	BufP = PaneP->BufP;
    char		*MemP;
    Int16		WFail;
    Int32		WErr;
    
    if (ED_QRRespLen == 0) return 1;
    
    // Handle Tilde, Dot, and DotDot... also leading blanks.
    if (ED_UtilNormalizePath(ED_QRRespS, ED_Path, ED_BUFFERPATHLEN + 1, ED_Name, NAME_MAX + 1)) return 1;

    // Call realpath on Path, just to make sure it works!
    MemP = realpath(ED_Path, NULL);
    if (MemP) free(MemP);
    else return 1;

    // FullPathLen does not include final NULL.
    ED_FullPathLen = sprintf(ED_FullPath, "%s/%s", ED_Path, ED_Name);

    // Ready to Write the file out... open it!
    ED_FD = open(ED_FullPath, O_WRONLY | O_CREAT | O_EXCL, S_IRWXU | S_IRWXG);
    if (ED_FD == -1) {
	if (errno == EEXIST) {
	    // Must ask to overwrite if already exists!
	    ED_FrameQRAsk(PaneP->FrameP, ED_STR_QueryOverwriteFile, NULL, ED_QRLetterType, EDCB_OverwriteFunc, NULL);
	    return 0;
	} else {
	    // Could not Open specifie file!
	    ED_QRPaneP = NULL;
	    ED_FrameSetEchoError(ED_STR_EchoOpenFailed, errno);
	    ED_FrameDrawAll(PaneP->FrameP);
	    return 0;
	}
    }

    WFail = ED_BufferWriteFile(BufP, ED_FD);
    WErr = errno;
    close(ED_FD);

    // Exit QR Mode, update Pane/Buf, and continue
    ED_QRPaneP = NULL;
    if (WFail) ED_FrameSetEchoError(ED_STR_EchoWriteFailed, WErr);
    else ED_BufferDidWrite(BufP, PaneP->FrameP);
    
    ED_FrameDrawAll(PaneP->FrameP);
    return 0;
}

// ED_RESPSTRLEN is smaller than (ED_BUFFERPATHLEN + 1)!!
void	ED_CmdWriteFile(ED_PanePointer PaneP)
{
    char	RespS[ED_RESPSTRLEN];
    
    sprintf(RespS, "%.*s/", ED_RESPSTRLEN - 16, PaneP->BufP->PathName);
    ED_FrameQRAsk(PaneP->FrameP, ED_STR_QueryWriteFile, RespS, ED_QRStringType, EDCB_WriteFileFunc, EDCB_QRAutoCompPathFunc);
}

// ******************************************************************************
// ED_CmdSaveFile -- saves file... must be pre-existing.
// Will ask permission if the file was filtered, as it will overwrite original!

// return 1 --> Success
// return 0 --> Failure
Int16	ED_AuxSaveFunc(ED_BufferPointer BufP, ED_FramePointer ThisFrameP)
{
    Int16		WFail;
    Int32		WErr;
    
    // File DOES need saving if we get here... Assemble the full path and open it
    ED_FullPathLen = sprintf(ED_FullPath, "%s/%s", BufP->PathName, BufP->FileName);
    ED_FD = open(ED_FullPath, O_TRUNC | O_WRONLY, 0);
    if (ED_FD == -1) {
	ED_FrameSetEchoError(ED_STR_EchoOpenFailed, errno);
	return 0;
    }

    WFail = ED_BufferWriteFile(BufP, ED_FD);
    WErr = errno;
    close(ED_FD);

    if (WFail) {
	ED_FrameSetEchoError(ED_STR_EchoWriteFailed, WErr);
	return 0;
    }

    ED_BufferDidSave(BufP, ThisFrameP);
    return 1;
}

// CALLBACK (ED_FrameQRAsk)
// Return 0 == Good response
// Return 1 == Bad response
Int16	EDCB_SaveFunc(void)
{
    char	C;

    if (ED_QRRespLen == 1) {

	C = ED_QRRespS[0];
	if ((C == 'n') || (C == 'N')) {
	    ED_QRPaneP = NULL;
	    return 0;
        }

	if ((C == 'y') || (C == 'Y')) {
	    ED_AuxSaveFunc(ED_QRPaneP->BufP, ED_QRPaneP->FrameP);
	    ED_FrameDrawAll(ED_QRPaneP->FrameP);
	    ED_QRPaneP = NULL;
	    return 0;
	}
    }

    // Bad response... get a nother one.
    return 1;
}

void	ED_CmdSaveFile(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;

    // If no file is assigned, do a CmdWriteFile instead... even if No MOD!
    if (BufP->Flags & ED_BUFNOFILEFLAG) {
	ED_CmdWriteFile(PaneP);
	return;
    }

    // Do not save if no MOD!
    if (! (BufP->Flags & ED_BUFMODFLAG)) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoNoChangeSave);
	goto DoneDone;
    }

    if (BufP->Flags & ED_BUFFILTERFLAG) {
	ED_FrameQRAsk(PaneP->FrameP, ED_STR_QuerySaveFilter, NULL, ED_QRLetterType, EDCB_SaveFunc, NULL);
	return;
    }

    ED_AuxSaveFunc(PaneP->BufP, PaneP->FrameP);

DoneDone:
    ED_FrameDrawAll(PaneP->FrameP);
    return;
}

// ******************************************************************************
// Abort out of save sequence if C-g !!

Int32	ED_CmdSaveFileCount;
Int16	EDCB_SaveSomeFunc(void);

// Return 1 --> Stop looping, waiting on QRAsk
// Return 0 --> Keep looping.
Int16	ED_AuxSaveSomeFiles(ED_PanePointer PaneP, ED_BufferPointer BP)
{
    char	QueryS[128];
    
    if (BP->Flags & ED_BUFFILTERFLAG) {
	ED_QRTempBufP = BP;
	snprintf(QueryS, 127, ED_STR_QuerySaveFilterNamed, BP->FileName);
	ED_FrameQRAsk(PaneP->FrameP, QueryS, NULL, ED_QRLetterType, EDCB_SaveSomeFunc, NULL);	
	return 1;
    }

    if (ED_AuxSaveFunc(BP, PaneP->FrameP))
	ED_CmdSaveFileCount += 1;	// Saved without having to ask!
    return 0;
}

// CALLBACK (ED_FrameQRAsk)
// Return 0 == Good response
// Return 1 == Bad response
Int16	EDCB_SaveSomeFunc(void)
{
    ED_PanePointer	PP;
    ED_BufferPointer	BP;
    char		C;
    Int16		SaveIt;

    if (ED_QRRespLen == 1) {

	C = ED_QRRespS[0];
	
	if ((C == 'n') || (C == 'N'))
	    SaveIt = 0;
        else if ((C == 'y') || (C == 'Y'))
	    SaveIt = 1;
	else
	    return 1;			// bad response!  Get another one.

	if (SaveIt && ED_AuxSaveFunc(ED_QRTempBufP, ED_QRPaneP->FrameP))
	    ED_CmdSaveFileCount += 1;	// Saved after asking.

	// Now look at the other buffers, starting *AFTER* this one.
	// Not recursive, as everything here is asynch... sets global and goes out to an event loop.
	
	BP = ED_QRTempBufP->NextBufP;
	while (BP) {
	    if ((BP->Flags & ED_BUFMODFLAG) && ! (BP->Flags & ED_BUFNOFILEFLAG))
		if (ED_AuxSaveSomeFiles(ED_QRPaneP, BP))	// Did we ask again
		    return 0;

	    BP = BP->NextBufP;
	}

	// All done with buffers if here!
	PP = ED_QRPaneP;
	ED_QRPaneP = NULL;
	if (ED_CmdSaveFileCount)
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoSavedNFiles, ED_CmdSaveFileCount);

	ED_FrameDrawAll(PP->FrameP);
	return 0;
    }

    return 1;		// Should never get here.
}


void	ED_CmdSaveSomeFiles(ED_PanePointer PaneP)
{
    ED_BufferPointer	BP;

    ED_CmdSaveFileCount = 0;
    BP = ED_FirstBufP;
    while (BP) {
	if ((BP->Flags & ED_BUFMODFLAG) && ! (BP->Flags & ED_BUFNOFILEFLAG))
	    if (ED_AuxSaveSomeFiles(PaneP, BP))
		return;

        BP = BP->NextBufP;
    }

    if (ED_CmdSaveFileCount)
	ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoSavedNFiles, ED_CmdSaveFileCount);

    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// ED_CmdFindFile -- Uses QR to ask Path/Filename.
// Launches second layer of QR to "Filter" file in Buffer--fix CR/LF and replace
// Tabs with spaces.
//
// File is first read into a Buffer, if it needs filtering, the user is asked.
// If Y, file is filtered and Buffer is joined to a Pane.  if N, Buffer is
// killed.
//
// NOTE:	ED_CmdFindFile and ED_CmdInsertFile have identical design.

// Callback Draws progress bar when "Filtering" file in Buffer.
void	EDCB_FilterEchoUpdate(Int16 Percent, void * DataP)
{
    ED_FrameDrawEchoProgressBar((*(ED_PanePointer)DataP).FrameP, ED_STR_Filter, Percent);
}

// Callback called when user responds to "Filter in Buffer" query--y/n.
// return 0 == Good response
// return 1 == Bad response
Int16 EDCB_FindFileDoFilterFunc(void)
{
    char		C;
    ED_PanePointer	PaneP = ED_QRPaneP;
    Int16		Res;

    Res = 0;
    if (ED_QRRespLen == 1) {
	C = ED_QRRespS[0];
	if ((C == 'n') || (C == 'N')) {
	    ED_QRPaneP = NULL;
	    ED_BufferKill(ED_QRTempBufP);
	    ED_QRTempBufP = NULL;
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoAborted);
	    goto Exit;
	}

	if ((C == 'y') || (C == 'Y')) {
	    ED_QRPaneP = NULL;
	    ED_BufferDoFilter(ED_QRTempBufP, EDCB_FilterEchoUpdate, PaneP);
	    ED_PaneGetNewBuf(PaneP, ED_QRTempBufP);
	    ED_QRTempBufP = NULL;
	    goto Exit;
	}
    }

    Res = 1;		// Bad Response if we get here.

Exit:
    ED_FrameDrawAll(PaneP->FrameP);
    return Res;
}

// Helper function, given FD open for Read, reads the file, then checks
// whether it needs filtering (CR/LF and Tab).  If so, creates a QR query
// to ask user.
//
// Return 1 if success.
// Return 0 if fail... errno will have code!
// Return -1 if launching QR query!
Int16	ED_AuxCmdFindFile(ED_PanePointer PaneP, Int32 FD)
{
    ED_QRTempBufP = ED_BufferReadFile(FD, ED_Name, ED_Path);
    if (ED_QRTempBufP == NULL) return 0;

    if (ED_BufferNeedsFilter(ED_QRTempBufP)) {
	ED_FrameQRAsk(PaneP->FrameP, ED_STR_QueryFilterFile, NULL, ED_QRLetterType, EDCB_FindFileDoFilterFunc, NULL);
	return -1;
    }else {
	ED_PaneGetNewBuf(PaneP, ED_QRTempBufP);
	ED_QRTempBufP = NULL;
    }

    return 1;
}

// Main callback for QR query, specifies file path/name to open.
// Uses QRAutoCompPathFunc callback to auto-complete file/dir names.
// return 0 == Good response
// return 1 == Bad response
Int16	EDCB_FindFileFunc(void)
{
    ED_PanePointer	PaneP = ED_QRPaneP;
    ED_BufferPointer	BufP;
    char		*MemP;
    Int32		RErr;
    Int16		Res;

    if (ED_QRRespLen == 0) return 1;

    // Handle Tilde, Dot, DotDot... and leading blanks... shoves response into ED_Path and ED_Name globals
    if (ED_UtilNormalizePath(ED_QRRespS, ED_Path, ED_BUFFERPATHLEN + 1, ED_Name, NAME_MAX + 1)) return 1;

    // Call realpath on path, just to make sure it works!
    MemP = realpath(ED_Path, NULL);
    if (MemP) free(MemP);
    else return 1;

    // We may already have this Buffer... just use the buffer if we have it!
    if ( (BufP = ED_BufferFindByName(ED_Name, ED_Path)) ) {
	if (PaneP->BufP != BufP)
	    ED_PaneGetNewBuf(PaneP, BufP);

	ED_QRPaneP = NULL;
	ED_FrameDrawAll(PaneP->FrameP);
	return 1;
    }

    // Create the FullPathLen
    ED_FullPathLen = sprintf(ED_FullPath, "%s/%s", ED_Path, ED_Name);

    // Open the file for read!
    ED_FD = open(ED_FullPath, O_RDONLY, NULL);
	if (ED_FD == -1) goto ErrorOut;
	Res = ED_AuxCmdFindFile(PaneP, ED_FD);
	RErr = errno;
    close(ED_FD);

    if (Res != -1) {
	ED_QRPaneP = NULL;
	ED_CmdLastId = ED_ReadId;
    }

    if (! Res) goto ErrorOut;

    ED_FrameDrawAll(PaneP->FrameP);
    return 1;

ErrorOut:
    ED_FrameSetEchoError(ED_STR_EchoReadFailed, RErr);    
    ED_FrameDrawAll(PaneP->FrameP);
    return 0;
}

void	ED_CmdFindFile(ED_PanePointer PaneP)
{
    char	RespS[ED_RESPSTRLEN];

    // Ignore SelRange and CmdMult.

    sprintf(RespS, "%.*s/", ED_RESPSTRLEN - 16, PaneP->BufP->PathName);
    ED_FrameQRAsk(PaneP->FrameP, ED_STR_QueryFindFile, RespS, ED_QRStringType, EDCB_FindFileFunc, EDCB_QRAutoCompPathFunc);
}

// ******************************************************************************
// ED_CmdInsertFile -- Inserts contest of file at current cursor position.  Will
// leave cursor at current position, but set mark at the end of the inserted text.
//
// Return 1 for success, 0 for failure
//
// NOTE:	We do NOT read directly into the Pane->Buf!
//		First we read into a separate *TEMP* Buf!
//		The buffer is then checked for <TAB> and <CR> chars.
//		The user is informed if any are found, and asked whether
//		to Filter.  If [Y], then (CR, CR/LF) --> (NewLine) and
//		(Tab) --> (Spaces) transformations are made... only then
//		is the contents of the *TEMP* buffer dumped into the Pane->Buf
//		and the *TEMP* buffer killed off!
//
// NOTE:	ED_CmdInsertFile and ED_CmdFindFile have identical design.

// Callback called when user responds to "Filter in Buffer" query--y/n.
// return 0 == Good response
// return 1 == Bad response
Int16	EDCB_InsertFileDoFilterFunc(void)
{
    char		C;
    ED_PanePointer	PaneP = ED_QRPaneP;
    Int16		Res;

    Res = 0;
    if (ED_QRRespLen == 1) {
	C = ED_QRRespS[0];
	if ((C == 'n') || (C == 'N')) {
	    ED_QRPaneP = NULL;
	    ED_BufferKill(ED_QRTempBufP);
	    ED_QRTempBufP = NULL;
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoAborted);
	    goto Exit;
	}

	if ((C == 'y') || (C == 'Y')) {
	    ED_QRPaneP = NULL;
	    ED_BufferDoFilter(ED_QRTempBufP, EDCB_FilterEchoUpdate, PaneP);
	    // Now, can finally insert into the real buffer!!
	    ED_PaneInsertBufferChars(PaneP, ED_QRTempBufP, 0);
	    ED_BufferKill(ED_QRTempBufP);
	    ED_QRTempBufP = NULL;
	    goto Exit;
	}
    }

    Res = 1;		// Bad Response if we get here.

Exit:
    ED_FrameDrawAll(PaneP->FrameP);
    return Res;
}

// Helper function, given FD open for Read, reads the file, then checks
// whether it needs filtering (CR/LF and Tab).  If so, creates a QR query
// to ask user.
//
// Return 1 if success.
// Return 0 if fail... errno will have code!
// Return -1 if launching QR query!
Int16	ED_AuxCmdInsertFile(ED_PanePointer PaneP, Int32 FD)
{
    ED_QRTempBufP = ED_BufferReadFile(FD, NULL, NULL);	// NULLs make it temporary
    if (ED_QRTempBufP == NULL) return 0;
    
    if (ED_BufferNeedsFilter(ED_QRTempBufP)) {
	ED_FrameQRAsk(PaneP->FrameP, ED_STR_QueryFilterFile, NULL, ED_QRLetterType, EDCB_InsertFileDoFilterFunc, NULL);
	return -1;
    } else {
	ED_PaneInsertBufferChars(PaneP, ED_QRTempBufP, 0);
	ED_BufferKill(ED_QRTempBufP);
	ED_QRTempBufP = NULL;
    }

    return 1;
}

// Main callback for QR query, specifies file path/name to open.
// Uses QRAutoCompPathFunc callback to auto-complete file/dir names.
// return 0 == Good response
// return 1 == Bad response
Int16	EDCB_InsertFileFunc(void)
{
    ED_PanePointer	PaneP = ED_QRPaneP;
    char		*MemP;
    Int32		RErr;
    Int16		Res;
    
    if (ED_QRRespLen == 0) return 1;

    // Handle Tilde, Dot, DotDot... and leading blanks... shoves response into ED_Path and ED_Name globals
    if (ED_UtilNormalizePath(ED_QRRespS, ED_Path, ED_BUFFERPATHLEN + 1, ED_Name, NAME_MAX + 1)) return 1;

    // Call realpath on path, just to make sure it works!
    MemP = realpath(ED_Path, NULL);
    if (MemP) free(MemP);
    else return 1;

    // Create the FullPathLen
    ED_FullPathLen = sprintf(ED_FullPath, "%s/%s", ED_Path, ED_Name);

    // Open the file for read!
    ED_FD = open(ED_FullPath, O_RDONLY, NULL);
	if (ED_FD == -1) goto ErrorOut;
	Res = ED_AuxCmdInsertFile(PaneP, ED_FD);
	RErr = errno;
    close(ED_FD);

    // AuxCmdInsertFile *MAY* itself invoke the QR machinery (ask to Filter the file)!
    if (Res != -1) {
	ED_QRPaneP = NULL;
	ED_CmdLastId = ED_InsertId;
    }
    
    if (! Res) goto ErrorOut;

    ED_FrameDrawAll(PaneP->FrameP);
    return 1;

ErrorOut:

    ED_FrameSetEchoError(ED_STR_EchoReadFailed, RErr);    
    ED_FrameDrawAll(PaneP->FrameP);
    return 0;
}

void	ED_CmdInsertFile(ED_PanePointer PaneP)
{
    char	RespS[ED_RESPSTRLEN];

    if (ED_BufferReadOnly(PaneP->BufP)) return;    
    // Ignore SelRange for now, will matter when inserting chars

    sprintf(RespS, "%.*s/", ED_RESPSTRLEN - 16, PaneP->BufP->PathName);
    ED_FrameQRAsk(PaneP->FrameP, ED_STR_QueryInsertFile, RespS, ED_QRStringType, EDCB_InsertFileFunc, EDCB_QRAutoCompPathFunc);
}

// ******************************************************************************
// ED_CmdSwitchBuffer

typedef struct {
    ED_BufferPointer	BP;
    char		Name[128];
} ED_BufSortRecord, *ED_BufSortPointer;

ED_BufSortPointer	*ED_BufSortArr;
Int32			ED_BufSortCount;

// return -1, Error
// return N, BuffCount == N
// Clearly a waste to allocate all this if N == 1, but easier than having a special case

Int16	ED_AuxCreateBufSortArr(void) {
    ED_BufferPointer	BufP = ED_FirstBufP;
    ED_BufSortPointer	BSP = NULL;
    Int32		BufCount = 0;
    Int32		I;

    // Count how many buffers there are
    while (BufP) BufCount++, BufP = BufP->NextBufP;
    ED_BufSortCount = BufCount;

    // Allocate the array
    ED_BufSortArr = malloc(BufCount * sizeof(ED_BufSortPointer));
    if (ED_BufSortArr == NULL) return -1;

    // Create individual BufSortRecord and sort into array
    BufP = ED_FirstBufP;
    BufCount = 0;
    while (BufP) {
	BSP = malloc(sizeof(ED_BufSortRecord));
	if (BSP == NULL) goto ErrorCleanUp;
	BSP->BP = BufP;
	if (BufP->Flags & ED_BUFNAMECOLFLAG)
	    sprintf(BSP->Name, "%.*s <%.*s>", 32, BufP->FileName, 30, BufP->PathName + BufP->DirNameOffset);
	else
	    strncpy(BSP->Name, BufP->FileName, 32);

	for (I = 0; I < BufCount; I++) {
	    if (0 > strcmp(BSP->Name, ED_BufSortArr[I]->Name))
		break;
	}
	// Insert at I... move elts down, then insert
	if (BufCount - I)
	    memmove(&ED_BufSortArr[I + 1], &ED_BufSortArr[I], (BufCount - I) * sizeof(ED_BufSortPointer));
	ED_BufSortArr[I] = BSP;

	BufCount += 1;
	BufP = BufP->NextBufP;
    }

#ifdef DEBUG    
    if (0) {
	for (I = 0; I < BufCount; I++)
	    printf("BufSortArr[%d] --> %s\n", I, ED_BufSortArr[I]->Name);
    }
#endif

    return BufCount;
	
ErrorCleanUp:
    for (I = 0; I < BufCount; I++)
	if (ED_BufSortArr[I])
	    free(ED_BufSortArr[I]);
    free(ED_BufSortArr);

    return -1;
}

void	ED_AuxFreeBufSortArr(void)
{
    Int32	I;

    if (ED_BufSortCount < 1) return;
    
    for (I = 0; I < ED_BufSortCount; I++)
	free(ED_BufSortArr[I]);
    free(ED_BufSortArr);
    ED_BufSortArr = NULL;
    ED_BufSortCount = 0;
}

// Return 0 == Good response, did complete
// Return 1 == Bad response, cannot complete
Int16	EDCB_AutoCompBufFunc(void)
{
    Int32		I, StartI, EndI;
    char		StrP[ED_RESPSTRLEN];
    Int16		Res;

    // Search the BufSortArr, can have a range of matches.
    StartI = EndI = -1;
    for (I = 0; I < ED_BufSortCount; I++) {
	Res = strncmp(ED_QRRespS, ED_BufSortArr[I]->Name, ED_QRRespLen);
	if (Res == 0) {
	    // Found 'a' match, but there may be others!!
	    if (StartI < 0) StartI = I;
	    EndI = I;
	} else if (Res < 0)
	    break;
    }

    if (StartI < 0) return 1;		// Nothing found

    // Perfet match if StartI == EndI.  Also if ED_BufSortArr[I]->Name
    // is the EXACT same length as QRRespS.  Name cannot be shorter,
    // so we check for length equality by checking that it is not longer!
    if ((StartI == EndI) ||
	(ED_BufSortArr[StartI]->Name[ED_QRRespLen] == 0)) {
	ED_FrameQRSetResp(ED_BufSortArr[StartI]->Name);
	return 0;
    }

    // More likely, multiple matches.  StartI != EndI.
    // Do a string intersection to find longest matching substring!
    strcpy(StrP, ED_BufSortArr[StartI]->Name);
    for (I = StartI + 1; I <= EndI; I++)
	ED_UtilIntersectStr(StrP, ED_BufSortArr[I]->Name);
    
    ED_FrameQRSetResp(StrP);
    return 0;
}

// return 0 == Good response
// return 1 == Bad response
Int16	EDCB_SwitchBufFunc(void)
{
    Int32	I, StartI, EndI;
    Int16	Res;

    // Take the default, if typed nothing
    if (ED_QRRespLen == 0 && ED_QRTempBufP) {
	ED_PaneGetNewBuf(ED_QRPaneP, ED_QRTempBufP);
	goto DoReturn;
    }

    // Match the response... good if only 1 match!
    StartI = EndI = -1;
    for (I = 0; I < ED_BufSortCount; I++) {
	Res = strncmp(ED_QRRespS, ED_BufSortArr[I]->Name, ED_QRRespLen);
	if (Res == 0) {
	    // Found 'a' match, but there may be others!!
	    if (StartI < 0) StartI = I;
	    EndI = I;
	} else if (Res < 0)
	    break;
    }

    // Found a match!  Go to it.
    if ((StartI >= 0) && (StartI == EndI)) {
	ED_PaneGetNewBuf(ED_QRPaneP, ED_BufSortArr[StartI]->BP);
	goto DoReturn;
    }

    // Create a new buffer with this name!  
    ED_PaneGetNewBuf(ED_QRPaneP, ED_BufferNew(0, ED_QRRespS, NULL, 0));
    // fall through to DoReturn
    
DoReturn:
    ED_FrameDrawAll(ED_QRPaneP->FrameP);
    ED_QRPaneP = NULL;
    ED_AuxFreeBufSortArr();
    ED_QRExitFP = NULL;
    return 0;
}


void	ED_CmdSwitchBuffer(ED_PanePointer PaneP)
{
    char		Str[ED_RESPSTRLEN];
    ED_BufferPointer	BufP = PaneP->BufP;

    ED_AuxCreateBufSortArr();
    ED_QRExitFP = ED_AuxFreeBufSortArr;

    ED_QRTempBufP = (BufP->NextBufP) ? BufP->NextBufP : BufP->PrevBufP;
    if (! ED_QRTempBufP) ED_QRTempBufP = BufP;
    sprintf(Str, ED_STR_SwitchBuffer, 32, ED_QRTempBufP->FileName);
    ED_FrameQRAsk(PaneP->FrameP, Str, NULL, ED_QRStringType, EDCB_SwitchBufFunc, EDCB_AutoCompBufFunc);	
}


// ******************************************************************************
// ED_CmdKillBuffer -- kills the specified buffer--provides default and auto-completion.
// Uses EDCB_AutoCompBufFunc and ED_BufSortArr machinery from ED_CmdSwitchBuffer!!
//
// NOTE:	Can kill one and only *TEMP_1* buffer, which is immediately replaced
//		with *TEMP_2*... *Temp_N*.
//
//		If killing Buffer XYZ, any Panes displaying XYZ will close.
//		If any such Pane is the only one on a Pane, it will immediately
//		get assigned the next Buffer.


void	ED_FrameKillBuf(ED_BufferPointer BufP)
{
    ED_FramePointer	FrameP = ED_FirstFrameP;
    ED_PanePointer	PaneP, NPaneP;
    ED_BufferPointer	NewBufP;

    // What buffer to replace it with?  Create a new Temp buffer
    // if BufP was the one and only buffer in the system!
    if (BufP->NextBufP)
	NewBufP = BufP->NextBufP;
    else if (BufP->PrevBufP)
	NewBufP = BufP->PrevBufP;
    else
	NewBufP = ED_BufferNew(0, NULL, NULL, 0);	

    // Go through all the Panes in all the Frames
    while (FrameP) {
	PaneP = FrameP->FirstPaneP;
	while (PaneP) {
	    if (PaneP->BufP == BufP) {
		if (FrameP->PaneCount == 1)
		    // This is the only Pane, so give it a new Buf!
		    ED_PaneGetNewBuf(PaneP, NewBufP);
		else {
		    // Just close this Pane
		    NPaneP = PaneP->NextPaneP;
			ED_PaneKill(PaneP);
		    PaneP = NPaneP;
		    continue;
		}
	    }
	    PaneP = PaneP->NextPaneP;
	}
	FrameP = FrameP->NextFrameP;
    }

    // Gone through all the Panes, finally get to kill the Buffer!
    ED_BufferKill(BufP);
}

// return 0 == Good response
// return 1 == Bad response
Int16	EDCB_KillBufFunc(void)
{
    Int32	I, StartI, EndI;
    Int16	Res;

    // Take the default, if typed nothing
    if (ED_QRRespLen == 0 && ED_QRTempBufP) {
	ED_FrameKillBuf(ED_QRTempBufP);
	goto DoReturn;
    }

    // Match the response... good if only 1 match!
    StartI = EndI = -1;
    for (I = 0; I < ED_BufSortCount; I++) {
	Res = strncmp(ED_QRRespS, ED_BufSortArr[I]->Name, ED_QRRespLen);
	if (Res == 0) {
	    // Found 'a' match, but there may be others!!
	    if (StartI < 0) StartI = I;
	    EndI = I;
	} else if (Res < 0)
	    break;
    }

    // Found a match!  Go to it.
    if ((StartI >= 0) && (StartI == EndI)) {
	ED_FrameKillBuf(ED_BufSortArr[StartI]->BP);
	goto DoReturn;
    }

    return 1;		// Bad buffer name!
    
DoReturn:
    ED_FrameDrawAll(ED_QRPaneP->FrameP);
    ED_QRPaneP = NULL;
    ED_AuxFreeBufSortArr();
    ED_QRExitFP = NULL;
    return 0;
}

void	ED_CmdKillBuffer(ED_PanePointer PaneP)
{
    char		Str[ED_RESPSTRLEN];
    ED_BufferPointer	BufP = PaneP->BufP;

    ED_AuxCreateBufSortArr();
    ED_QRExitFP = ED_AuxFreeBufSortArr;

    ED_QRTempBufP = BufP;
    sprintf(Str, ED_STR_KillBuffer, 32, ED_QRTempBufP->FileName);
    ED_FrameQRAsk(PaneP->FrameP, Str, NULL, ED_QRStringType, EDCB_KillBufFunc, EDCB_AutoCompBufFunc);	
}


// ******************************************************************************
void	ED_CmdGetWorkingDir(ED_PanePointer PaneP)
{
    Int32	MaxLen;

    MaxLen = ED_MSGSTRLEN - (1 + strlen(ED_STR_EchoDir));
    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoDir, MaxLen, PaneP->BufP->PathName);
    ED_FrameDrawEchoLine(PaneP->FrameP);
    sc_BlinkTimerReset();
}


// ******************************************************************************
// ED_CmdExecNamedCmd -- Performs M-x function, takes name of command and runs it.
// Does support auto-complete

// Return 0 == Good response--i.e. DID complete
// Return 1 == Bad response--i.e. cannot complete
//
// NOTE:	Our matching algorithm can be fooled
//		if we have cmds named "ABC" and "ABCDE".  The
//		matching algorithm thinks both are candidates when
//		looking for "ABC", when in fact, the first is a
//		perfect match.
Int16	EDCB_AutoCompNamedCmdFunc(void)
{
    ED_FRegPointer	FRP, StartFRP, EndFRP;
    char		StrP[ED_RESPSTRLEN];
    Int16		Res;

    // Search the function registry... can have a range of matches
    FRP = ED_FirstFRegP;
    StartFRP = EndFRP = NULL;
    while (FRP) {
	Res = strncmp(ED_QRRespS, FRP->NameP, ED_QRRespLen);
	if (Res == 0) {
	    // Found a match, but there may be others!!
	    if (StartFRP == NULL)	// First in match range
		StartFRP = FRP;
	    EndFRP = FRP;		// Last in match range
	} else if (Res < 0)
	    break;

	FRP = FRP->SortNextP;		// Sorted order, ESSENTIAL
    }

    if (StartFRP == NULL)		// Found nothing
	return 1;

    // Perfect match if StartFRP and EndFRP are the same.
    // Also perfect match if StartFRP->NameP is exactly the same
    // length as ED_QRRespS... since NameP CANNOT be shorter,
    // we can test for that by checking that it is NOT LONGER!
    if ((StartFRP == EndFRP) ||
	(StartFRP->NameP[ED_QRRespLen] == 0)) {
	ED_FrameQRSetResp(StartFRP->NameP);
	return 0;
    }
	
    // Multiple matches, StartFRP != EndFRP.
    // Do a string intersection to find longest matching substring!
    strcpy(StrP, StartFRP->NameP);
    FRP = StartFRP->SortNextP;
    while (FRP) {
	ED_UtilIntersectStr(StrP, FRP->NameP);
	if (FRP == EndFRP) break;
	FRP = FRP->SortNextP;
    }
    
    ED_FrameQRSetResp(StrP);
    return 0;
}

// return 0 == Good response
// return 1 == Bad response
Int16	EDCB_ExecNamedCmdFunc(void)
{
    ED_PanePointer	PaneP = ED_QRPaneP;
    ED_FRegPointer	FRP = ED_FirstFRegP;
    ED_FRegPointer	MatchFRP = NULL;
    Int16		Res;

    while (FRP) {
	Res = strncmp(ED_QRRespS, FRP->NameP, ED_QRRespLen);
	if (Res == 0) {
	    if (MatchFRP) return 1;	// Too many matches!
	    else MatchFRP = FRP;
	    // Check for perfect match--NameP is same Len as Resp!
	    // (NameP cannot match and be shorter!)
	    if (FRP->NameP[ED_QRRespLen] == 0)
		break;
	} else if (Res < 0)
	    break;

	FRP = FRP->SortNextP;
    }

    // Found 1 good match: execute it, and get out of QR mode!
    if (MatchFRP) {
	ED_QRPaneP = NULL;
	// Do not execute *THIS* cmd from itself!!
	if (MatchFRP->FuncP == ED_CmdExecNamedCmd) {
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoExecSelf);
	    ED_FrameDrawEchoLine(PaneP->FrameP);
	    return 0;
	}
	
	ED_CmdThisId = (Uns64)MatchFRP->FuncP;
	    (*MatchFRP->FuncP)(PaneP);
	ED_CmdLastId = ED_CmdThisId;
	ED_CmdMult = 1;
	
	ED_FrameDrawAll(PaneP->FrameP);
	return 0;
    }

    // Found no matches at all!
    return 1;
}

// Must take numeric count, negatives too!!
// Execute every command EXCEPT calling itself!!
void	ED_CmdExecNamedCmd(ED_PanePointer PaneP)
{
    char	QueryS[32] = "M-x ";
    Int32	Mult;

    if (ED_CmdMultCmd) {
	Mult = (ED_CmdMultNeg) ? - ED_CmdMult : ED_CmdMult;
	sprintf(QueryS, "%d M-x ", Mult);
    }

    ED_FrameQRAsk(PaneP->FrameP, QueryS, NULL, ED_QRStringType, EDCB_ExecNamedCmdFunc, EDCB_AutoCompNamedCmdFunc);
    ED_CmdThisId = ED_ExecId;
}

// ******************************************************************************
void	ED_CmdNewFrame(ED_PanePointer PaneP)
{
    ED_FramePointer	FrameP = PaneP->FrameP;
    ED_BufferPointer	BufP = PaneP->BufP;

    ED_FrameNew(FrameP->WinWidth, FrameP->WinHeight, BufP);
    ED_FrameDrawAll(FrameP);
}

// ******************************************************************************
void	ED_CmdOtherFrame(ED_PanePointer PaneP)
{
    ED_FramePointer	ThisFrameP = PaneP->FrameP;
    ED_FramePointer	OtherFrameP = NULL;

    OtherFrameP = (ThisFrameP->NextFrameP) ? ThisFrameP->NextFrameP : ED_FirstFrameP;
    if (OtherFrameP == ThisFrameP) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoOnlyFrame);
	ED_FrameDrawAll(ThisFrameP);
	return;
    }
    
    ED_FrameDrawAll(ThisFrameP);	// Old Frame
    XSetInputFocus(ED_XDP, OtherFrameP->XWin, RevertToNone, CurrentTime);
    ED_FrameDrawAll(OtherFrameP);	// New Frame
}

// ******************************************************************************

void	ED_CmdDeleteFrame(ED_PanePointer PaneP)
{
    ED_FramePointer	FrameP = PaneP->FrameP;

    if ((FrameP->NextFrameP == NULL) && (FrameP->PrevFrameP == NULL)) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoDelOnlyFrame);
	ED_FrameDrawAll(FrameP);
	return;
    }

    ED_FrameKill(FrameP);
}

// ******************************************************************************

void	ED_CmdDeleteOtherFrames(ED_PanePointer PaneP)
{
    ED_FramePointer	FrameP = PaneP->FrameP;
    ED_FramePointer	NextFrameP;
    
    if ((FrameP->NextFrameP == NULL) && (FrameP->PrevFrameP == NULL)) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoDelOnlyFrame);
	ED_FrameDrawAll(FrameP);
	return;
    }

    FrameP = ED_FirstFrameP;
    while (FrameP) {
	NextFrameP = FrameP->NextFrameP;
	if (FrameP != PaneP->FrameP)
	    ED_FrameKill(FrameP);

	FrameP = NextFrameP;
    }
}

// ******************************************************************************
// ED_CmdISearch, ED_CmdISearchBack -- implement incremental search.
// Current search match is hilited in Purple, alternative matches in turquoise.
// Both are shown *only* in ISPaneP.
//
// ED_ISMatchPos shows the current matching position, or -1. But ED_ISSearchPos
// says were to start searching for the next match!  These two vars are usually the
// same, but differ when starting the search and if the current search fails--but
// (just like RealEmacs) we want to keep displaying the existing (before failing) Match.
//
// As with Selection, a blinker that lands on the hilited region must have
// the matching foreground (GXxor) or the colors will be wrong.  So if CursorPos
// is same as ISMatchPos, it should have a purple foreground.
//
// Normally, the blinker will NOT land on an alternative (turquoise) match.
// (If in match mode, the blinker will be at ISMatchPos, otherwise, no alternative
// will be shown.)  *BUT* if an alternative match follows immediately after ISMatchPos,
// then the blinker can be on it.  So, ED_ISAltPos, normally -1, records the
// Pos for an alternative that follows immediately after the main match.
//
// Match result is explicitly found here... displayed by ED_PaneDrawText.
// Alternative results are found and displayed by ED_PaneDrawText.
//
// scEmacs behavior is NOT exactly like RealEmacs... but close enough.
// RealEmacs places the search string in a minibuffer that can be edited.  scEmacs
// simply echos the search string, allows only <backspace> for editing (does not
// even use the QR mechanism.)

// RealEmacs shows the current match in all panes that have the same buffer, but
// shows the alternatives only in the current pane.  scEmacs shows both in the
// current pane and nothing in other panes--less confusing to the user.

// RealEmacs appears to maintain a stack of search results, and Backspace will keep
// popping the results, until returning to the very original start of the search.
// scEmacs is (again) much simpler.  Backspace does not pop search results, but
// removes characters from the end of the search (template) string.  It simply
// goes back to the origin of the search, and begins again with the shorter string.
// There is also no notification of "overwrapped search failing"...


// Decides whether Pos in BufP starts a full match to ISStr.
// Match can be case sensitive or not--depending on ISCaseSen variable.
// Return 0 -> No match
// Return 1 -> *MATCH*
// Uses *faster* ptr-based algorithm, skips over Gap.  (Instead of *simpler* Pos method.)
Int16	ED_ISCheckMatch(ED_BufferPointer BufP, Int32 Pos)
{
    Int32	CurPos = Pos;
    char	*CurP, *EndP, *MatchP;
    Int16	LoopBack;
    Int32	MatchLen;

    MatchP = ED_ISStr;
    MatchLen = ED_ISStrLen;
    if (MatchLen == 0) return 0;

    LoopBack = 1;
    CurP = BufP->BufStartP + CurPos;
    EndP = BufP->GapStartP;
    if (CurP >= BufP->GapStartP) {
	CurP += (BufP->GapEndP - BufP->GapStartP);
	EndP = BufP->BufEndP;
	LoopBack = 0;
    }

DoRest:
    while (CurP < EndP) {

	if (*CurP != *MatchP) {		// If MatchP has any Caps, then ISCaseSen = 1;
	    if (ED_ISCaseSen)
		return 0;
	    if ((*CurP < 'A') || ('Z' < *CurP) || (*CurP + 'a' - 'A' != *MatchP))
		return 0;
	}    

	CurP++, MatchP++;
	if (--MatchLen == 0)		// Full match
	    return 1;
    }

    if (LoopBack) {
	CurP = BufP->GapEndP;
	EndP = BufP->BufEndP;
	LoopBack = 0;
	goto DoRest;
    }

    return 0;
}

// Implements forward search.
// Starts at StartPos, searches forward (to BufP->LastPos) to find a match.
// Returns Pos that starts a full match, or -1 if it fails.
Int32	ED_ISMatchForward(ED_BufferPointer BufP, Int32 StartPos)
{
    Int32	CurPos = StartPos;

    if (ED_ISDoWrap) {
	ED_ISDoWrap = 0;
	CurPos = 0;
    } else {
	if (CurPos < 0)
	    CurPos = 0;
	else if (CurPos >= BufP->LastPos)
	    return -1;
    }

    while (CurPos < BufP->LastPos) {
	if (ED_ISCheckMatch(BufP, CurPos)) {
	    return CurPos;
	}
        CurPos++;
    }

    return -1;
}

// Implements backwars search.
// Starts just before StartPos (often called with BufP->LastPos) and
// searches backwards for a Pos that starts a full match.  Return the
// matching Pos location or -1 if it fails.
Int32	ED_ISMatchBackward(ED_BufferPointer BufP, Int32 StartPos)
{
    Int32	CurPos = StartPos - 1;

    if (ED_ISDoWrap) {
	ED_ISDoWrap = 0;
	CurPos = BufP->LastPos - ED_ISStrLen;
    } else {
	if (CurPos < 0) {
	    CurPos = BufP->LastPos - ED_ISStrLen;
	    if (CurPos < 0) return -1;
	} else if (CurPos > BufP->LastPos - ED_ISStrLen)
	    return -1;
    }
    
    while (CurPos) {
	if (ED_ISCheckMatch(BufP, CurPos))
	    return CurPos;
	CurPos--;
    }

    return -1;
}

// Finds the next match, searches forward or backward.
void	ED_ISNewMatch(Int16 GoAfter)
{
    ED_BufferPointer	BufP = ED_ISPaneP->BufP;
    Int32		NewMatchPos;

    if (ED_ISSearchPos == -1) ED_ISSearchPos = ED_ISOriginPos;
    if (ED_ISDir > 0) {
	ED_ISSearchPos = (GoAfter) ? ED_ISSearchPos + ED_ISStrLen : ED_ISSearchPos;
	NewMatchPos = ED_ISMatchForward(BufP, ED_ISSearchPos);
	if (NewMatchPos == -1) {
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoISFail, ED_ISStrLen, ED_ISStr);
	    ED_ISDoWrap = 1;
	} else {
	    ED_ISMatchPos = NewMatchPos;
	    ED_ISSearchPos = NewMatchPos;
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoIS, ED_ISStrLen, ED_ISStr);
	    strcpy(ED_ISPrevStr, ED_ISStr);
	}
    } else {
	ED_ISSearchPos = (GoAfter) ? ED_ISSearchPos - 1 : ED_ISSearchPos;
	NewMatchPos = ED_ISMatchBackward(BufP, ED_ISSearchPos);
	if (NewMatchPos == -1) {
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoISBackFail, ED_ISStrLen, ED_ISStr);
	    ED_ISDoWrap = 1;
	} else {
	    ED_ISMatchPos = NewMatchPos;
	    ED_ISSearchPos = NewMatchPos;	    
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoISBack, ED_ISStrLen, ED_ISStr);
	    strcpy(ED_ISPrevStr, ED_ISStr);			
	}
    }
}

// Updates the match details, echo line, pane update, etc.
// DoClear if the search string has changed!  DO NOT SET DoClear if just advancing
// to the next match.  Set ED_ISDir = 0 if search is done, but leave ED_ISPaneP.
void	ED_ISUpdate(Int16 SetMark)
{
    ED_PanePointer	PP;
    Int32		RowStartPos;

    if (ED_ISDir == 0) {
	PP = ED_ISPaneP;
	ED_ISPaneP = NULL;
	ED_FrameEchoLen = 0;

	if (SetMark) {
	    ED_BufferPushMark(PP->BufP, ED_ISOriginPos);
	    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoSearchOriginMark);
	}
	
	ED_FrameDrawAll(PP->FrameP);
	return;
    }
    
    if (ED_ISDoWrap) {
	// ISMatchPos and CursorPos have NOT moved.
	if (ED_ISDir > 0)
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoISFail, ED_ISStrLen, ED_ISStr);
	else
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoISBackFail, ED_ISStrLen, ED_ISStr);
    } else {
	if (ED_ISDir > 0)
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoIS, ED_ISStrLen, ED_ISStr);
	else
	    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoISBack, ED_ISStrLen, ED_ISStr);

	if (ED_ISMatchPos >= 0) {
	    if (ED_ISDir > 0)
		ED_ISPaneP->CursorPos = ED_ISMatchPos + ED_ISStrLen;
	    else
		ED_ISPaneP->CursorPos = ED_ISMatchPos;
	    RowStartPos = ED_PaneFindLoc(ED_ISPaneP, ED_ISPaneP->CursorPos, &ED_ISPaneP->CursorRow, &ED_ISPaneP->CursorCol, 0, 0);
	    if ((ED_ISPaneP->CursorRow < 0) || (ED_ISPaneP->CursorRow > ED_ISPaneP->RowCount - 2))
		ED_PaneMoveAfterCursorMove(ED_ISPaneP, RowStartPos, (ED_ISPaneP->RowCount - 2) / 2, 0);
	}
    }

    // Possible to delete the whole search string... in that case, return cursor to the original position!
    if ((ED_ISStrLen == 0)) {
	ED_ISMatchPos = -1;
	ED_ISPaneP->CursorPos = ED_ISOriginPos;
	RowStartPos = ED_PaneFindLoc(ED_ISPaneP, ED_ISPaneP->CursorPos, &ED_ISPaneP->CursorRow, &ED_ISPaneP->CursorCol, 0, 0);
	if ((ED_ISPaneP->CursorRow < 0) || (ED_ISPaneP->CursorRow > ED_ISPaneP->RowCount - 2))
	    ED_PaneMoveAfterCursorMove(ED_ISPaneP, RowStartPos, (ED_ISPaneP->RowCount - 2) / 2, 0);
    }
	
    ED_FrameDrawAll(ED_ISPaneP->FrameP);
}

// Used for the C-w functionality of incremental search.
// Adds the next word (n spaces, or single non-alpanumeric char) to the ISStr search string.
void	ED_ISAddNextWord(void)
{
    ED_BufferPointer		BufP = ED_ISPaneP->BufP;
    Int32			CurPos, L;
    Int16			InWord, InSpace;
    char			*CurP;

    CurPos = ED_ISPaneP->CursorPos;
    if (CurPos == BufP->LastPos) {
	ED_FrameFlashError(ED_ISPaneP->FrameP);
	return;
    }
    
    CurP = ED_BufferPosToPtr(BufP, CurPos);
    L = ED_BufferGetUTF8Len(*CurP);
    InWord = ED_BufferCIsAlpha(*CurP);
    InSpace = (*CurP == ' ') || (*CurP == '\n');

    do {
	// Add to search string *IFF* have space for it
	if (ED_ISStrLen + L + 1 < ED_ISSTRLEN) {
	    strncat(ED_ISStr, CurP, L);
	    ED_ISStrLen += L;
	} else {
	    ED_FrameFlashError(ED_ISPaneP->FrameP);
	    return;
	}

	CurPos += L;
	CurP = ED_BufferPosToPtr(BufP, CurPos);
	L = ED_BufferGetUTF8Len(*CurP);
	if (InWord) {				// Keep reading until OUT of word
	    if (! ED_BufferCIsAlpha(*CurP)) return;

	} else if (InSpace) {			// Keep reading until Out of whitespace
	    if (*CurP != ' ' && *CurP != '\n') return;

	} else return;				// 1 non-word/non-space char is enough

    } while (CurPos < BufP->LastPos);
}

// Handles all key presses when in IS mode.
// NOTE: QREP takes over from this function... as IncSearch is subordinate to QueryReplace.
//
// Return 1 if char is handled here.
// Return 0 if want regular Frame/pane processing--abort out of ISearch.
Int16	ED_ISHandleChars(ED_FramePointer FrameP, Uns16 SymChar, Int16 Mods, Int32 NewSLen, char *NewSP)
{
    Int16	IsCtrl = (Mods & ControlMask);
    Int16	IsMeta = (Mods & Mod1Mask);

    // No Meta commands supported, abort out of search
    if (IsMeta) {
	ED_ISDir = 0;			// Signal ISUpdate to clean up!
	ED_ISUpdate(1);
	goto BadReturn;
    }

    if (IsCtrl) {
	switch (SymChar) {
	    case 'w': case 'W':		// Add next "word" to search
		ED_ISAddNextWord();
		ED_ISNewMatch(0);
		goto GoodReturn;

	    case 's': case 'S':
		if (ED_ISDir < 0) {
		    ED_ISDir = 1;
		    ED_ISDoWrap = 0;
		} else if (ED_ISStrLen == 0) {
		    strcpy(ED_ISStr, ED_ISPrevStr);
		    ED_ISStrLen = strlen(ED_ISStr);
		    ED_ISMatchPos = -1;
		    ED_ISSearchPos = ED_ISPaneP->CursorPos;
		    ED_ISNewMatch(0);
		} else
		    ED_ISNewMatch(1);
		goto GoodReturn;

	    case 'r': case 'R':
		if (ED_ISDir > 0) {
		    ED_ISDir = -1;
		    ED_ISDoWrap = 0;
		} else if (ED_ISStrLen == 0) {
		    strcpy(ED_ISStr, ED_ISPrevStr);
		    ED_ISStrLen = strlen(ED_ISStr);
		    ED_ISMatchPos = -1;
		    ED_ISSearchPos = ED_ISPaneP->CursorPos - ED_ISStrLen;
		    ED_ISNewMatch(0);		    
		} else
		    ED_ISNewMatch(1);
		goto GoodReturn;

	    case 'g': case 'G':			// Abort out, no marking though!
		ED_ISDir = 0;
		ED_ISUpdate(0);
		goto BadReturn;

	    default:
		ED_ISDir = 0;			// Signal ISUpdate to clean up!
		ED_ISUpdate(1);
		goto BadReturn;
	}

    } else {
	if (SymChar == 0xff0d) {		// Return
	    strcpy(ED_ISPrevStr, ED_ISStr);	// Stash string!	
	    ED_ISDir = 0;			// Signal ISUpdate to clean up!
	    goto GoodReturn;
	    
	} else if (SymChar == 0xff08) {		// Backspace
	    if (ED_ISStrLen) {
		// Deleted one char from ISStr, start at Origin and match again!
		ED_ISStr[--ED_ISStrLen] = 0;
		ED_ISMatchPos = -1;
		ED_ISSearchPos = ED_ISOriginPos;
		if (ED_ISStrLen) ED_ISNewMatch(0);
	    } else
		ED_FrameFlashError(FrameP);	// No chars to delete!
	    goto GoodReturn;
	    
	} else {				// Insert other Chars
	    if (('A' <= SymChar) && (SymChar <= 'Z'))
		ED_ISCaseSen = 1;

	    // Add to ISStr... then search again from old position
	    if (ED_ISStrLen + NewSLen + 1 < ED_ISSTRLEN) {
		strncat(ED_ISStr, NewSP, NewSLen);
		ED_ISStrLen += NewSLen;
		ED_ISNewMatch(0);
	    } else
		ED_FrameFlashError(FrameP);
		
	} // Other Chars
    } // NOT IsCtrl

GoodReturn:
    ED_ISUpdate(1);
    return 1;

BadReturn:
    ED_FrameDrawEchoLine(FrameP);
    return 0;
}

void	ED_ISAbortOut(char * MsgP)
{
    ED_PanePointer	PP = ED_ISPaneP;

    // Do Mark the starting position of the search
    ED_BufferPushMark(PP->BufP, ED_ISOriginPos);

    ED_ISPaneP = NULL;
    ED_PaneDrawText(PP);
    ED_PaneDrawBlinker(PP);
    
    ED_FrameEchoLen = 0;
    if (MsgP) ED_FrameSetEchoS(ED_ECHOMSGMODE, MsgP);
	
    ED_FrameDrawEchoLine(PP->FrameP);
}

void	ED_CmdISearch(ED_PanePointer PaneP)
{
    ED_ISPaneP = PaneP;
    ED_ISDir = 1;
    ED_ISStr[0] = 0;
    ED_ISStrLen = 0;
    ED_ISCaseSen = 0;
    ED_ISDoWrap = 0;
    ED_ISMatchPos = -1;
    ED_ISSearchPos = -1;
    ED_ISAltPos = -1;
    ED_ISOriginPos = PaneP->CursorPos;
    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoIS, 0, "");
    ED_FrameDrawAll(PaneP->FrameP);
}

void	ED_CmdISearchBack(ED_PanePointer PaneP)
{
    ED_ISPaneP = PaneP;
    ED_ISDir = -1;
    ED_ISStr[0] = 0;
    ED_ISStrLen = 0;
    ED_ISCaseSen = 0;
    ED_ISDoWrap = 0;
    ED_ISMatchPos = -1;
    ED_ISSearchPos = -1;
    ED_ISAltPos = -1;
    ED_ISOriginPos = PaneP->CursorPos;
    ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoISBack, 0, "");
    ED_FrameDrawAll(PaneP->FrameP);
}

// ******************************************************************************
// ED_CmdQueryReplace -- Handles Query-Replace (launches into QREP mode).
// Uses QR modeline buffer to get QREPFrom and QREPTo strings.  Then simply
// prompts on the EchoLine as user marches down the matches to replace or skip.
// QREP uses IS (IncSearch) to find matches to QREPFrom string.  As before, the
// main PaneDrawText loop does all the highlighting and drawing.
//
// QREP supports the following commands when it encounters a match:
//	<spc>, y, Y			Replace current match, move to next
//	<Delete>, <Backspace>, n, N	Skip current match, move to next
//	<Ret>				Done, exit QREP now
//	<Period>			Replace current match, then exit QREP
//	!				Replace all remaining matches, do not ask.
//	i				Replace all remaining matches, do show them!
//
//


// Update the screen.  Incremental means function is being called after each
// step.... otherwise, it will be called once at the very end.
void	ED_QREPPaneUpdate(Int16 Incremental)
{
    ED_PanePointer	PP = ED_QREPPaneP;
    Int32		DeltaLen;

    DeltaLen = ED_QREPToLen - ED_QREPFromLen;
    if (Incremental)
	PP->CursorPos += DeltaLen;
    else
	PP->CursorPos = ED_ISSearchPos + DeltaLen;
    ED_PaneUpdateAllPos(PP, 0);
    
    ED_PaneDrawText(PP);
    ED_PaneDrawScrollBar(PP, 0);

    ED_FrameDrawBlinker(PP->FrameP);
    ED_FrameDrawEchoLine(PP->FrameP);
    sc_BlinkTimerReset();
}

// Update the search.  MatchPos is the 'new' match-- or -1 if none were found.
// The latter will exit QREP after telling user how many instances were replaced.
void	ED_QREPSearchUpdate(Int32 MatchPos)
{
    ED_PanePointer	PP = ED_QREPPaneP;
    Int32		RowStartPos;

    if (MatchPos == -1) {
	ED_FrameSPrintEchoS(ED_ECHOMSGMODE, ED_STR_EchoQueryReplaceDone, ED_QREPCount);
	
	// Everything was off... calculate all Pane parameters, set cursor, and draw.
	if (ED_QREPDoAll) {
	    ED_QREPPaneUpdate(0);
	    ED_PaneUpdateOtherPanesIncrRest(PP);
	}
	ED_ISPaneP = NULL;
	ED_QREPPaneP = NULL;
	ED_FrameDrawAll(PP->FrameP);	

    } else {
	ED_ISMatchPos = MatchPos;
	ED_ISSearchPos = MatchPos + ED_ISStrLen;
	PP->CursorPos = ED_ISSearchPos;

	if (! ED_QREPDoAll) {
	    RowStartPos = ED_PaneFindLoc(PP, PP->CursorPos, &PP->CursorRow, &PP->CursorCol, 0, 0);
	    if ((PP->CursorRow < 0) || (PP->CursorRow > PP->RowCount - 2))
		ED_PaneMoveAfterCursorMove(PP, RowStartPos, (PP->RowCount - 2) / 2, 0);
	   ED_FrameDrawAll(PP->FrameP);
	}
    }
}

// Replace the matched text and update everything.
void	ED_QREPReplace(void)
{
    ED_PanePointer	PP = ED_QREPPaneP;
    ED_BufferPointer	BufP = PP->BufP;
    Int32		StartPos, DeltaLen;

    ED_QREPCount += 1;

    StartPos = ED_ISMatchPos;
    DeltaLen = ED_QREPToLen - ED_QREPFromLen;
    ED_BufferPlaceGap(BufP, StartPos, DeltaLen);

    // Kill the old text first.  Store in KillRing ONLY if first time, no point
    // in storing the same string N times.  (But DO put in Undo EVERY time.)
    ED_BufferAddUndoBlock(BufP, StartPos, ED_QREPFromLen, ED_UB_DEL | ED_UB_CHUNK, BufP->GapEndP);
    BufP->Flags |= ED_BUFMODFLAG;
    if (ED_QREPCount == 1)
	ED_KillRingAdd(BufP->GapEndP, ED_QREPFromLen, 1);
    BufP->GapEndP += ED_QREPFromLen;
    BufP->LastPos -= ED_QREPFromLen;

    // Now add new text.
    if (ED_QREPToLen) {
	ED_BufferAddUndoBlock(BufP, StartPos, ED_QREPToLen, ED_UB_ADD | ED_UB_CHUNK | ED_UB_CHAIN, BufP->GapStartP);
	memcpy(BufP->GapStartP, ED_QREPToStr, ED_QREPToLen);
	BufP->GapStartP += ED_QREPToLen;
	BufP->LastPos += ED_QREPToLen;
    }

    // Set the ISSearchPos properly... do not want to search IN the replacement string!
    ED_ISSearchPos = ED_ISMatchPos + ED_QREPToLen;

    // QREPDoAll does everything (with minumum update), otherwise show all changes.
    if (ED_QREPDoAll) {
	if (BufP->PaneRefCount > 1) ED_PaneUpdateOtherPanesIncrBasic(PP, StartPos, DeltaLen);
    } else {
	if (BufP->PaneRefCount > 1) ED_PaneUpdateOtherPanes(PP, StartPos, DeltaLen);
	ED_QREPPaneUpdate(1);
    }
}

#define		ED_QREPEventLoopInterval	75

// Handle all keyboard input while in QREP mode.
// Return 1 if char is handled here--all cases.
// Would return 0 for main loop to handle chars/cmds.
Int16	ED_QREPHandleChars(ED_FramePointer FrameP, Uns16 SymChar, Int16 Mods)
{
    Int32		MatchPos;
    Int32		Count;

    // Accept no Control/Meta/etc. Mods here.
    if (Mods & ED_COMMANDMODSMASK) {
	ED_FrameFlashError(FrameP);
	return 1;
    }

    switch (SymChar) {
	case 'y': case 'Y': case ' ':		// Y -> Do replace
	    ED_QREPReplace();
	    MatchPos = ED_ISMatchForward(ED_QREPPaneP->BufP, ED_ISSearchPos);
	    ED_QREPSearchUpdate(MatchPos);
	    break;

	case 'n': case 'N':			// N -> Do not replace
	case 0xffff: case 0xff08:		// Delete + Backspace
	    MatchPos = ED_ISMatchForward(ED_QREPPaneP->BufP, ED_ISSearchPos);
	    ED_QREPSearchUpdate(MatchPos);	
	    break;

	case 0xff0d:				// <Enter> -> Exit now
	    if (ED_ISPaneP) ED_BufferPushMark(ED_QREPPaneP->BufP, ED_ISOriginPos);	
	    ED_QREPSearchUpdate(-1);
	    break;

	case '.':				// '.' -> Replace, then exit
	    ED_QREPReplace();
	    ED_QREPSearchUpdate(-1);
	    break;

        case 'i': case 'I':			// 'i' -> replace all (Show it)
	    Count = 0;
	    do {
		ED_QREPReplace();
		MatchPos = ED_ISMatchForward(ED_QREPPaneP->BufP, ED_ISSearchPos);
		ED_QREPSearchUpdate(MatchPos);

		// This loop *can* take some time, especially in degenerate test cases.
		// But XSel clients may requests with a TEMPORARY Win which is deleted
		// if response is delayed... and a late response sent to a deleted Win will
		// error out the X-server!  (Service Main loop every 50-100 cycles,
		// otherwise QREP becomes tooo slow!)

		Count += 1;
		if (Count > ED_QREPEventLoopInterval) {
		    Count = 0;
		    sc_MainEventLoop();
		    // May abort, click on other windows, etc.
		    if (ED_QREPPaneP == NULL)
			break;
		}
	    } while (MatchPos > 0);
	    break;

	case '!':				// '!' -> Replace all (Don't show)
	    do {
		ED_QREPDoAll = 1;		// No updates until done!!
		ED_QREPReplace();
		MatchPos = ED_ISMatchForward(ED_QREPPaneP->BufP, ED_ISSearchPos);
		ED_QREPSearchUpdate(MatchPos);
	    } while (MatchPos > 0);
	    break;

	default:				// OTHER -> Not acceptable
	    ED_FrameFlashError(FrameP);
    }

    return 1;
}

// Turns off QREP *AND* IS... so when aborting, always do QREP first!
void	ED_QREPAbortOut(char * MsgP)
{
    ED_PanePointer		PP = ED_QREPPaneP;

    if (ED_ISPaneP) ED_BufferPushMark(PP->BufP, ED_ISOriginPos);
	
    ED_QREPPaneP = NULL;
    ED_ISPaneP = NULL;
    ED_PaneDrawText(PP);
    ED_PaneDrawBlinker(PP);
    
    ED_FrameEchoLen = 0;
    if (MsgP) ED_FrameSetEchoS(ED_ECHOMSGMODE, MsgP);
    ED_FrameDrawEchoLine(PP->FrameP);
}

// Start the actual matching--and replacing.
void	ED_QREPStart(void)
{
    ED_PanePointer	PP = ED_QRPaneP;
    Int16		I;
    Int32		MatchPos;

    ED_QREPPaneP = PP;
    strcpy(ED_QREPPrevFromStr, ED_QREPFromStr);
    strcpy(ED_QREPPrevToStr, ED_QREPToStr);

    // Exit QR (QueryResponse) mode, just use the echo line for status
    ED_QRPaneP = NULL;
    ED_FrameSPrintEchoS(ED_ECHOPROMPTMODE, ED_STR_EchoQueryReplace, ED_ISSTRLEN, ED_QREPFromStr, ED_ISSTRLEN, ED_QREPToStr);

    // begin searching!
    ED_ISPaneP = PP;
    ED_ISDir = 1;
    strcpy(ED_ISStr, ED_QREPFromStr);
    ED_ISStrLen = ED_QREPFromLen;
    ED_ISMatchPos = -1;
    ED_ISSearchPos = PP->CursorPos;
    ED_ISOriginPos = PP->CursorPos;
    
    // Case-sensitive ONLY if has UpperCase in From string!
    ED_ISCaseSen = 0;
    for (I = 0; I < ED_ISStrLen; I++)
	if (('A' <= ED_ISStr[I]) && (ED_ISStr[I] < 'Z')) {
	    ED_ISCaseSen = 1;
	    break;
	}
	
    MatchPos = ED_ISMatchForward(PP->BufP, ED_ISSearchPos);
    ED_QREPSearchUpdate(MatchPos);
}



// Handles second level of QueryReplace:  to specifyED_QREPToStr.
// Return 0 == Good response
// Return 1 == Bad response
Int16	EDCB_QREPFunc2(void)
{
    if (ED_QRRespLen > 0) {
	if (ED_QRRespLen > ED_ISSTRLEN - 1)
	    ED_QRRespLen = ED_ISSTRLEN - 1;
	strncpy(ED_QREPToStr, ED_QRRespS, ED_QRRespLen);
	ED_QREPToStr[ED_QRRespLen] = 0;				// Ensure null termination
	ED_QREPToLen = ED_QRRespLen;
    }

    ED_QREPStart();
    return 0;
}


// Handles first level of QueryReplace:  Either want default, or specify ED_QREPFromStr.
// Return 0 == Good response
// Return 1 == Bad response--user should enter another.
Int16	EDCB_QREPFunc1(void)
{
    Int16	GetToStr = 0;
    char	QueryS[ED_RESPSTRLEN];
    
    if (ED_QRRespLen == 0) {
	// Use defaults, if there are any!
	if (ED_QREPPrevFromStr[0] == 0)
	    return 1;

	strcpy(ED_QREPFromStr, ED_QREPPrevFromStr);
	strcpy(ED_QREPToStr, ED_QREPPrevToStr);
	ED_QREPFromLen = strlen(ED_QREPFromStr);
	ED_QREPToLen = strlen(ED_QREPToStr);
    } else {
	GetToStr = 1;

	if (ED_QRRespLen > ED_ISSTRLEN - 1)
	    ED_QRRespLen = ED_ISSTRLEN - 1;
	strncpy(ED_QREPFromStr, ED_QRRespS, ED_QRRespLen);
	ED_QREPFromStr[ED_QRRespLen] = 0;			// Ensure null termination
	ED_QREPFromLen = ED_QRRespLen;
    }

    if (GetToStr) {
	sprintf(QueryS, ED_STR_QueryReplaceWith, ED_ISSTRLEN, ED_QREPFromStr);
	ED_FrameQRAsk(ED_QRPaneP->FrameP, QueryS, NULL, ED_QRStringType, EDCB_QREPFunc2, NULL);
    } else
	ED_QREPStart();

    return 0;
}

// Main command!
void	ED_CmdQueryReplace(ED_PanePointer PaneP)
{
    char	QueryS[ED_RESPSTRLEN];
    char	*QP;

    if (ED_BufferReadOnly(PaneP->BufP)) return; // Don't even start
    ED_SelPaneP = NULL;				// In case one was active!

    ED_QREPPaneP = NULL;			// Have not started search/replace yet!
    ED_QREPFromStr[0] = 0;
    ED_QREPToStr[0] = 0;
    ED_QREPFromLen = 0;
    ED_QREPToLen = 0;
    ED_QREPCount = 0;
    ED_QREPDoAll = 0;

    if (ED_QREPPrevFromStr[0]) {
	Int32	MaxLen = (ED_RESPSTRLEN - (2 + strlen(ED_STR_QueryReplace))) / 2;
	sprintf(QueryS, ED_STR_QueryReplace, MaxLen, ED_QREPPrevFromStr, MaxLen, ED_QREPPrevToStr);
	QP = QueryS;
    } else
	QP = ED_STR_QueryReplaceNoDef;

    ED_FrameQRAsk(PaneP->FrameP, QP, NULL, ED_QRStringType, EDCB_QREPFunc1, NULL);
}

// ******************************************************************************
// ******************************************************************************
// ED_CreateXWin creates the Window that will house the given FP.

void	ED_XWinCreate(ED_FramePointer FP)
{
    Int16		XScreenN;
    char		NameS[ED_MSGSTRLEN] = "";
    Int32		W, H, X, Y;
    Int32		WMod, HMod;

    XScreenN = DefaultScreen(ED_XDP);
    ED_FrameGetWinSize(FP, &W, &H);

    WMod = (DisplayWidth(ED_XDP, XScreenN) - W) / 40;
    HMod = (DisplayHeight(ED_XDP, XScreenN) - H) / 40;
    X = (FP->Number % WMod) * 40;
    Y = (FP->Number % HMod) * 40;

    FP->XWin = XCreateSimpleWindow(ED_XDP, RootWindow(ED_XDP, XScreenN), X, Y, W, H, 2,
				   BlackPixel(ED_XDP, XScreenN), WhitePixel(ED_XDP, XScreenN));
    {
	XTextProperty	XWinNameProp;
	XSizeHints	* XWinSizeHintsP;
	XWMHints	XWinMHints;
	XClassHint	XWinClassHints;
	Uns32		XWinEventMask;
	char *		WinNameP = NameS;

	sprintf(NameS, "%s: %d", ED_NAMESTRING, ED_FrameN);
	if (XStringListToTextProperty(&WinNameP, 1, &XWinNameProp) == 0)
	    G_SETEXCEPTION("Frame name alloc failed", ED_FrameN);

	{
	    ED_FrameRecord	FakeF;
	    Int32		Wide, High;

	    FakeF.RowChars = ED_FRAMEMINROWCHARS;
	    FakeF.RowCount = ED_FRAMEMINROWCOUNT;
	    ED_FrameGetWinSize(&FakeF, &Wide, &High);

	    XWinSizeHintsP = XAllocSizeHints();
	    XWinSizeHintsP->min_width = Wide;
	    XWinSizeHintsP->min_height = High;
	    XWinSizeHintsP->width_inc = ED_Advance;
	    XWinSizeHintsP->height_inc = ED_Row;

	    XWinSizeHintsP->flags = PPosition | PSize | PMinSize | PResizeInc;
	}

	XWinMHints.initial_state = NormalState;
	XWinMHints.input = True;
	XWinMHints.flags = StateHint | InputHint;

	XWinClassHints.res_name = ED_NAMESTRING;
	XWinClassHints.res_class = NameS; // WinNameP;

	XSetWMProperties(ED_XDP, FP->XWin, &XWinNameProp, NULL,
			 NULL, 0, XWinSizeHintsP, &XWinMHints, &XWinClassHints);
	XFree(XWinSizeHintsP);
	XFree(XWinNameProp.value);

	XSetWMProtocols(ED_XDP, FP->XWin, &ED_XWMDelAtom, 1);
		     
	XWinEventMask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | // KeymapStateMask |
		 StructureNotifyMask |FocusChangeMask;		// 	ButtonMotionMask | PointerMotionHintMask |
	XSelectInput(ED_XDP, FP->XWin, XWinEventMask);
    }

    FP->XftDP = XftDrawCreate(ED_XDP, FP->XWin, DefaultVisual(ED_XDP, XScreenN), DefaultColormap(ED_XDP, XScreenN));
    if (! FP->XftDP) G_SETEXCEPTION("Frame XftDrawCreate failed", ED_FrameN);

    FP->BlinkerGC = XCreateGC(ED_XDP, FP->XWin, 0, NULL);
    XSetForeground(ED_XDP, FP->BlinkerGC, WhitePixel(ED_XDP, XScreenN));
    XSetFunction(ED_XDP, FP->BlinkerGC, GXxor);

    FP->HiliteGC = XCreateGC(ED_XDP, FP->XWin, 0, NULL);
    XSetForeground(ED_XDP, FP->HiliteGC, ED_XCArr[ED_Turquoise].pixel);
    XSetFunction(ED_XDP, FP->HiliteGC, GXand);

    FP->HLBlinkerGC = XCreateGC(ED_XDP, FP->XWin, 0, NULL);
    XSetForeground(ED_XDP, FP->HLBlinkerGC, ED_XCArr[ED_SelOrange].pixel);
    XSetFunction(ED_XDP, FP->HLBlinkerGC, GXxor);

    XDefineCursor(ED_XDP, FP->XWin, ED_TextCursor);

    sc_WERegAdd(FP->XWin, &ED_FrameHandleEvent, FP);	// Register with main loop
    XMapWindow(ED_XDP, FP->XWin);
    XSync(ED_XDP, False);

    FP->XICP = XCreateIC(ED_XIMP, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
				  XNClientWindow, FP->XWin,
				  NULL);
    if (FP->XICP == NULL) G_SETEXCEPTION("Cound not open XIC", 0);
    
    XSetICFocus(FP->XICP);
}

// ******************************************************************************
// ED_ColorArrCreate creates the colors needed for Xft.  These are stored
// in a global ED_XCArr.

#define	ED_COLORMAKE(Color, R, G, B)							\
    do { XRColor.red = R, XRColor.green = G, XRColor.blue = B;				\
	 XftColorAllocValue(ED_XDP, XVisualP, XColorMap, &XRColor, &ED_XCArr[Color]);	\
    } while (0);


void	ED_ColorArrCreate(void)
{
    Int16		XScreenN = DefaultScreen(ED_XDP);
    Colormap		XColorMap = DefaultColormap(ED_XDP, XScreenN);
    Visual *		XVisualP = DefaultVisual(ED_XDP, XScreenN);
    XRenderColor	XRColor;
	    
    XRColor.alpha = 0xFFFF;
    ED_COLORMAKE(ED_Black,	0x2222, 0x2222, 0x2222);
    ED_COLORMAKE(ED_White,	0xFF00, 0xFF00, 0xFF00);
    ED_COLORMAKE(ED_Touch,	0xF200, 0xF200, 0xF200);
    ED_COLORMAKE(ED_TouchPlus,	0xEC00, 0xEC00, 0xEC00);
    ED_COLORMAKE(ED_LightGray,	0xE500, 0xE500, 0xE500);
    ED_COLORMAKE(ED_Gray,	0xBF00, 0xBF00, 0xBF00);	// 0xAFFF is 50% gray
    ED_COLORMAKE(ED_TitleGray,	0x3F00, 0x3E00, 0x3A00);
    ED_COLORMAKE(ED_Red,	0xA000, 0x2200, 0x2200);
    ED_COLORMAKE(ED_Orange,	0xFF00, 0x7000, 0x0000);
    ED_COLORMAKE(ED_Brown,	0xB500, 0x5500, 0x1000);
    ED_COLORMAKE(ED_Green,	0x2200, 0xA000, 0x2200);
    ED_COLORMAKE(ED_Blue,	0x2200, 0x2200, 0xA000);
    ED_COLORMAKE(ED_SelOrange,	0xFF00, 0x7000, 0x4000);	// HiLite
    ED_COLORMAKE(ED_Turquoise,	0x6600, 0xFF00, 0xFF00);	// Hilite
    ED_COLORMAKE(ED_Purple,	0xFF00, 0x6600, 0xFF00);	// Hilite
}

// ******************************************************************************
// ED_ColorArrDestroy Clears out the ED_XCArr, freeing the colors.

void	ED_ColorArrDestroy(void)
{
    Int16	XScreenN = DefaultScreen(ED_XDP);
    Colormap	XColorMap = DefaultColormap(ED_XDP, XScreenN);
    Visual *	XVisualP = DefaultVisual(ED_XDP, XScreenN);
    Int16	I;
			    
    for (I = 0; I < ED_ColorCount; I++)
	XftColorFree(ED_XDP, XVisualP, XColorMap, &ED_XCArr[I]);
}

// ******************************************************************************
// ED_InitEditor will initialize the editor module and create an initial
// Frame and Window.  It will also register/bind the defined command functions.

#define	ED_DEFINECMD(FnP, NameS, KeyS)		ED_FRegNewBinding(FRP = ED_FRegNewFunction(FnP, NameS), KeyS)
#define ED_DEFINEADDITIONALBND(KeyS)		ED_FRegNewBinding(FRP, KeyS);

void	ED_EditorInit(Display * XDP, XftFont * XFP, XIM XIMP, Int32 WinWidth, Int32 WinHeight)
{
    ED_FRegPointer	FRP;
    Int16		I;

    ED_XDP = XDP;
    ED_XFP = XFP;
    ED_XIMP = XIMP;
    ED_XWMDelAtom = XInternAtom(ED_XDP, "WM_DELETE_WINDOW", 0);
    ED_PUExecAtom = XInternAtom(ED_XDP, "EXECUTE", 0);
    ED_ClipAtom = XInternAtom(ED_XDP, "CLIPBOARD", 0);
    ED_TargetsAtom = XInternAtom(ED_XDP, "TARGETS", 0);
    ED_TextAtom = XInternAtom(ED_XDP, "TEXT", 0);
    ED_UTF8Atom = XInternAtom(ED_XDP, "UTF8_STRING", 0);
    ED_IncrAtom = XInternAtom(ED_XDP, "INCR", 0);

    ED_Ascent = XFP->ascent;
    ED_Descent = XFP->descent;
    ED_Height = ED_Ascent + ED_Descent;
    ED_Row = ED_Height + 1;
    ED_Advance = XFP->max_advance_width;

    ED_ColorArrCreate();
    ED_TextCursor = XCreateFontCursor(ED_XDP, XC_xterm);
    ED_SizeCursor = XCreateFontCursor(ED_XDP, XC_sb_v_double_arrow);
    ED_ArrowCursor = XCreateFontCursor(ED_XDP, XC_left_ptr);

    for (I = 0; I <= ED_TABSTOP; I++) ED_TabStr[I] = ' ';
    ED_TabStr[I] = 0;		// Trailing 0 to be safe

    sc_SAStoreOpen(&ED_FrameStore, sizeof(ED_FrameRecord), ED_FRAMEALLOCCOUNT);
    sc_SAStoreOpen(&ED_PaneStore, sizeof(ED_PaneRecord), ED_PANEALLOCCOUNT);
    sc_SAStoreOpen(&ED_BufferStore, sizeof(ED_BufferRecord), ED_BUFFERALLOCCOUNT);

    ED_BufferNew(0, NULL, NULL, 0);		// Create new buffer, default size, name, and path
    ED_FrameNew(WinWidth, WinHeight, NULL);	// Create a new Frame, it will create 1 Pane

    ED_XSelInit();				// Init XSelection module
    ED_KillRingInit();				// Init global KillRing
    ED_FRegInit();				// Init Function Registry (+ Binding Reg)

    ED_FRegNewFunction(ED_CmdSelectLine,	"select-line");			// Not bound to keyboard
    ED_FRegNewFunction(ED_CmdSelectArea,	"select-area");			// Not bound to keyboard
    ED_FRegNewFunction(ED_CmdGetWorkingDir,	"pwd");				// Not bound to keyboard
    ED_FRegNewFunction(ED_CmdResetUndo,		"reset-undo");			// Not bound to keyboard
    ED_FRegNewFunction(ED_CmdDisableUndo,	"disable-undo");		// Not bound to keyboard

    ED_DEFINECMD(ED_CmdSplitPane,		"split-pane",			"\x16\x78\x32");	// C-x 2
    ED_DEFINECMD(ED_CmdKillPane,		"kill-pane",			"\x16\x78\x30");	// C-x 0
    ED_DEFINECMD(ED_CmdKillOtherPanes,		"kill-other-panes",		"\x16\x78\x31");	// C-x 1
    ED_DEFINECMD(ED_CmdGoNextPane,		"other-pane",			"\x16\x78\x6F");	// C-x o
    ED_DEFINECMD(ED_CmdExit,			"save-and-quit",		"\x16\x78\x16\x63");	// C-x C-c
    ED_DEFINECMD(ED_CmdHelp,			"help",				"\x16\x68");		// C-h
    ED_DEFINECMD(ED_CmdQuitAction,		"quit",				"\x16\x67");		// C-g
    ED_DEFINECMD(ED_CmdSetMark,			"set-mark",			"\x16\x20");		// C-space
					    ED_DEFINEADDITIONALBND(		"\x16\x40");		// C-@
    ED_DEFINECMD(ED_CmdGetCursorInfo,		"cursor-info",			"\x16\x78\x3d");	// C-x =
    ED_DEFINECMD(ED_CmdGoNextChar,		"forward-char",			"\x16\x66");		// C-f
					    ED_DEFINEADDITIONALBND(		"\x15\x53");		// RIGHT
    ED_DEFINECMD(ED_CmdGoNextWord,		"forward-word",			"\x17\x66");		// M-f
    ED_DEFINECMD(ED_CmdGoPrevChar,		"backward-char",		"\x16\x62");		// C-b
					    ED_DEFINEADDITIONALBND(		"\x15\x51");		// LEFT
    ED_DEFINECMD(ED_CmdGoPrevWord,		"backward-word",		"\x17\x62");		// M-b
    ED_DEFINECMD(ED_CmdGoNextPage,		"next-page",			"\x16\x76");		// C-v
					    ED_DEFINEADDITIONALBND(		"\x15\x56");		// Page-down
    ED_DEFINECMD(ED_CmdGoPrevPage,		"prev-page",			"\x17\x76");		// M-v
					    ED_DEFINEADDITIONALBND(		"\x15\x55");		// Page_Up
    ED_DEFINECMD(ED_CmdGoLineEnd,		"goto-end-of-line",		"\x16\x65");		// C-e
					    ED_DEFINEADDITIONALBND(		"\x15\x57");		// End
    ED_DEFINECMD(ED_CmdGoLineStart,		"goto-start-of-line",		"\x16\x61");		// C-a
					    ED_DEFINEADDITIONALBND(		"\x15\x50")		// Home
    ED_DEFINECMD(ED_CmdGoNextRow,		"go-down",			"\x16\x6e");		// C-n
					    ED_DEFINEADDITIONALBND(		"\x15\x54");		// Down
    ED_DEFINECMD(ED_CmdGoPrevRow,		"go-up",			"\x16\x70");		// C-p
					    ED_DEFINEADDITIONALBND(		"\x15\x52");		// UP
    ED_DEFINECMD(ED_CmdGoBufEnd,		"goto-end",			"\x17\x3e");		// M->
					    ED_DEFINEADDITIONALBND(		"\x16\x15\x57");	// C-End
    ED_DEFINECMD(ED_CmdGoBufStart,		"goto-start",			"\x17\x3c");		// M-<
					    ED_DEFINEADDITIONALBND(		"\x16\x15\x50");	// C-Home
    ED_DEFINECMD(ED_CmdRecenterPage,		"recenter-page",		"\x16\x6c");		// C-L
    ED_DEFINECMD(ED_CmdSelectAll,		"select-all",			"\x16\x78\x68");	// C-x h
    ED_DEFINECMD(ED_CmdExchMark,		"exchange-point-and-mark",	"\x16\x78\x16\x78");	// C-x C-x
    ED_DEFINECMD(ED_CmdInsertTab,		"insert-tab",			"\x15\x09");		// Tab
    ED_DEFINECMD(ED_CmdDelNextChar,		"delete-forward",		"\x16\x64");		// C-d
					    ED_DEFINEADDITIONALBND(		"\x15\xff");		// Delete
    ED_DEFINECMD(ED_CmdDelNextWord,		"delete-word-forward",		"\x17\x64");		// M-d
					    ED_DEFINEADDITIONALBND(		"\x16\x15\xff");	// C-Delete
    ED_DEFINECMD(ED_CmdDelPrevChar,		"delete-backward",		"\x15\x08");		// Backspace
    ED_DEFINECMD(ED_CmdDelPrevWord,		"delete-word-backward",		"\x17\x15\xff");	// M-Delete
					    ED_DEFINEADDITIONALBND(		"\x17\x15\x08");	// M-Backspace
					    ED_DEFINEADDITIONALBND(		"\x16\x15\x08");	// C-Backspace
    ED_DEFINECMD(ED_CmdDelHSpace,		"delete-horiz-space",		"\x17\x5c");		/* M-\ */
    ED_DEFINECMD(ED_CmdJoinLines,		"join-lines",			"\x17\x5e");		// M-^
    ED_DEFINECMD(ED_CmdDowncaseWord,		"downcase-word",		"\x17\x6c");		// M-l
    ED_DEFINECMD(ED_CmdUpcaseWord,		"upcase-word",			"\x17\x75");		// M-u
    ED_DEFINECMD(ED_CmdCapitalizeWord,		"capitalize-word",		"\x17\x63");		// M-c
    ED_DEFINECMD(ED_CmdGotoLine,		"goto-line",			"\x17\x67\x67");	// M-g g
					    ED_DEFINEADDITIONALBND(		"\x17\x67\x6c");	// M-g l
					    ED_DEFINEADDITIONALBND(		"\x17\x67\x17\x67");	// M-g M-g
    ED_DEFINECMD(ED_CmdGotoChar,		"goto-char",			"\x17\x67\x63");	// M-g c
    ED_DEFINECMD(ED_CmdYank,			"yank",				"\x16\x79");		// C-y
    ED_DEFINECMD(ED_CmdYankPop,			"yank-pop",			"\x17\x79");		// M-y
    ED_DEFINECMD(ED_CmdKillLine,		"kill-line",			"\x16\x6b");		// C-k
    ED_DEFINECMD(ED_CmdKillRegion,		"kill-region",			"\x16\x77");		// C-w
    ED_DEFINECMD(ED_CmdCopyRegion,		"copy-region",			"\x17\x77");		// M-w
    ED_DEFINECMD(ED_CmdModBuffer,		"unmodify",			"\x17\x7e");		// M-~

    ED_DEFINECMD(ED_CmdPopUpMarkList,		"mark-from-list",		"\x16\x17\x20");	// C-M-space
    ED_DEFINECMD(ED_CmdPopUpKillList,		"yank-from-list",		"\x16\x17\x79");	// C-M-y
    ED_DEFINECMD(ED_CmdPopUpCmdList,		"exec-from-list",		"\x16\x17\x78");	// C-M-x

    ED_DEFINECMD(ED_CmdWriteFile,		"write-file",			"\x16\x78\x16\x77");	// C-x C-w
    ED_DEFINECMD(ED_CmdSaveFile,		"save-file",			"\x16\x78\x16\x73");	// C-x C-s
    ED_DEFINECMD(ED_CmdSaveSomeFiles,		"save-some-files",		"\x16\x78\x73");	// C-x s
    ED_DEFINECMD(ED_CmdFindFile,		"find-file",			"\x16\x78\x16\x66");	// C-x C-f
    ED_DEFINECMD(ED_CmdInsertFile,		"insert-file",			"\x16\x78\x69");	// C-x i

    ED_DEFINECMD(ED_CmdPopUpBufferList,		"buffer-from-list",		"\x16\x17\x62");	// C-M-b
					    ED_DEFINEADDITIONALBND(		"\x16\x78\x16\x62");	// C-x C-b
					    
    ED_DEFINECMD(ED_CmdSwitchBuffer,		"switch-to-buffer",		"\x16\x78\x62");	// C-x b
    ED_DEFINECMD(ED_CmdKillBuffer,		"kill-buffer",			"\x16\x78\x6b");	// C-x k
    
    ED_DEFINECMD(ED_CmdExecNamedCmd,		"exec-named-cmd",		"\x17\x78");		// M-x

    ED_DEFINECMD(ED_CmdNewFrame,		"new-frame",			"\x16\x78\x35\x32");	// C-x 5 2
    ED_DEFINECMD(ED_CmdOtherFrame,		"other-frame",			"\x16\x78\x35\x6f");	// C-x 5 o
    ED_DEFINECMD(ED_CmdDeleteFrame,		"delete-frame",			"\x16\x78\x35\x30");	// C-x 5 0
    ED_DEFINECMD(ED_CmdDeleteOtherFrames,	"delete-other-frames",		"\x16\x78\x35\x31");	// C-x 5 1

    ED_DEFINECMD(ED_CmdISearch,			"search-forward",		"\x16\x73");		// C-s
    ED_DEFINECMD(ED_CmdISearchBack,		"search-backward",		"\x16\x72");		// C-r
    ED_DEFINECMD(ED_CmdQueryReplace,		"query-replace",		"\x17\x25");		// M-%
    ED_DEFINECMD(ED_CmdUndo,			"undo",				"\x16\x2f");		// C-/
    					    ED_DEFINEADDITIONALBND(		"\x16\x5f");		// C-_
					    ED_DEFINEADDITIONALBND(		"\x16\x78\x75");	// C-x u
    
    ED_CmdLastId = ED_StartId;
    
#ifdef DEBUG    
    // TEST_StuffText();
#endif
}

// ******************************************************************************
// ED_EditorKill is called to terminate the Editor module, it frees memory and
// releases XWin assets... by this point, the Frames should all have been killed!!
//
// NOTE:	ED_FrameKill is the proper way to shut things down, it will
//		eventually cause ED_EditorKill to be called.

void	ED_EditorKill(void)
{
    ED_BufferPointer		BufP;

    ED_XSelKill();
    
    ED_ColorArrDestroy();
    XFreeCursor(ED_XDP, ED_TextCursor);
    XFreeCursor(ED_XDP, ED_SizeCursor);
    XFreeCursor(ED_XDP, ED_ArrowCursor);
    
    ED_FRegKill();			// Closes FReg/FBind SA Stores.
    ED_KillRingFree();

    BufP = ED_FirstBufP;
    while (BufP) {
	free(BufP->BufStartP);		// Just free the Malloc memory
	ED_BufferKillUndo(BufP);	// Get rid of Undo Slabs!
    	BufP = BufP->NextBufP;		// BufP will be purged by Store
    }
    
    sc_SAStoreClose(&ED_FrameStore);
    sc_SAStoreClose(&ED_PaneStore);
    sc_SAStoreClose(&ED_BufferStore);
    sc_MainExit();
}

// ******************************************************************************
// ED_EditorOpenFile is called form the main loop to open the initial files
// specified in the command line.
// 
// It can be called with duplicate file names, in which case each copy gets
// its own Frame/Win, but obviously same Buffer.
//
// NOTE:	Incoming files are filtered as necessary *WITHOUT* query or consent.

void	ED_EditorOpenFile(char *PathP, Int16 NewFrame)
{
    ED_BufferPointer	BufP;
    char		*MemP;
    Int16		Res, CheckBuffer;

    // Handle Tilde, Dot, DotDot, etc.
    if (ED_UtilNormalizePath(PathP, ED_Path, ED_BUFFERPATHLEN + 1, ED_Name, NAME_MAX + 1))
	goto ErrorOut;

    MemP = realpath(ED_Path, NULL);
    if (MemP) free(MemP);
    else goto ErrorOut;

    // We may already have it!
    CheckBuffer = 0;
    if ( (BufP = ED_BufferFindByName(ED_Name, ED_Path)) )
	goto FoundBuf;

    // Create the FullPathLen
    ED_FullPathLen = sprintf(ED_FullPath, "%s/%s", ED_Path, ED_Name);

    // Open the file for read
    Res = 1;
    ED_FD = open(ED_FullPath, O_RDONLY, NULL);
	if (ED_FD == -1) goto ErrorOut;

	CheckBuffer = 1;
	BufP = ED_BufferReadFile(ED_FD, ED_Name, ED_Path);
	if (BufP == NULL) Res = 0;
    close(ED_FD);

    if (! Res) goto ErrorOut;

FoundBuf:
    // Create a new Frame
    if (NewFrame)
	ED_FrameNew(ED_FirstFrameP->WinWidth, ED_FirstFrameP->WinHeight, BufP);
    else
	ED_PaneGetNewBuf(ED_FirstFrameP->FirstPaneP, BufP);

    if (CheckBuffer && ED_BufferNeedsFilter(BufP))
	ED_FirstFrameP->Flags |= ED_FRAMEINITFILTERFLAG;	// Filter when EXPOSE 
    return;

ErrorOut:
    return;
}

// ******************************************************************************
// ******************************************************************************
// The QueryResponse (QR) routines implement a tiny 1 line editor in the Echo
// line of a frame.  The system enters a Prompt, and the user types a Response.
// In many cases, the response will be a single letter (from multiple choices)
// that is processed immediately (As in "Save File: [Y/N]").  But this same
// mechanism is used to specify file names/paths as well as buffer names.
//
// A QR is active (usurps the CurPane) if ED_QRPaneP (it is Null
// otherwise).  Both the Q and R can include UTF8 Chars, so there is a distinction
// between Len/Pos (byte lengths/positions) versus X (Col/CharCount)--just like Buffers.

// ******************************************************************************
// ED_AuxQRGetPos will start from StrP, go X Chars, then report the Pos ByteCount.

Int16	ED_AuxQRGetPos(char * StrP, Int16 X)
{
    char	*P = StrP;
    Int16	L, Pos = 0, CurX = 0;

    while (*P) {
	if (CurX == X) return Pos;
	L = ED_BufferGetUTF8Len(*P);
	P += L, Pos += L, CurX++;
    }

    return Pos;
}

// ******************************************************************************
// ED_AuxQRGetX will start from StrP, go Pos bytes, then report the X CharCount.

Int16	ED_AuxQRGetX(char * StrP, Int16 Pos)
{
    char	*P = StrP;
    char	*EndP = StrP + Pos;
    Int16	CurX = 0;

    while (P < EndP) {
	P += ED_BufferGetUTF8Len(*P);
	CurX += 1;
    }

    return CurX;
}

// ******************************************************************************
// ED_AuxQRDrawScrollMark will the draw the tiny orange ticks at either end of
// the Response line, indicating a longer line in that direction.
//
// (ED_QRStrikeOut) means strike out the whole area!

void	ED_AuxQRDrawScrollMark(ED_FramePointer FrameP, Int16 IsRight)
{
    Int32	X, Y;

    Y = ED_Ascent + ((FrameP->RowCount - 1) * ED_Row);
    if (ED_QRStrikeOut) {
	X = ED_FRAMELMARGIN + (ED_Advance * ED_QRPromptX);
	if (ED_QRPromptX < FrameP->RowChars)
	    XftDrawRect(FrameP->XftDP, &ED_XCArr[ED_SelOrange], X, Y-6,
			(ED_Advance * (FrameP->RowChars + 1 - ED_QRPromptX)) - 1, 4);
	return;
    }

    if (IsRight) {
	X = ED_FRAMELMARGIN + (ED_Advance * FrameP->RowChars);
 	XftDrawRect(FrameP->XftDP, &ED_XCArr[ED_SelOrange], X, Y-6, ED_Advance - 1, 4);   

    } else {
	X = ED_FRAMELMARGIN + (ED_Advance * ED_QRPromptX);
	XftDrawRect(FrameP->XftDP, &ED_XCArr[ED_SelOrange], X, Y-6, ED_Advance - 1, 4);
    }
}

// ******************************************************************************
// ED_QuxQRSanitize goes over strings that are to be pasted in the Response.  It
// skips over leading blanks, then stops at the end of the first line--never any
// reason to paste N lines in a 1-line response buffer!
//
// Return X length if success, 0 if not.
//
// NOTE:  X == (# of chars to draw) is returned.  *LenP gets # of bytes.

Int16	ED_AuxQRSanitize(char ** DataPP, Int32 *LenP)
{
    char	*E, *S = *DataPP;
    char	*EndP = *DataPP + *LenP;
    Int32	X = 0;
    Int32	L;

    // No point if there is no room!
    if (ED_QRRespLen == ED_RESPSTRLEN - 1)
	return 0;

    // First skip all whitespace (space + \n)
    while (S < EndP) {
	if ((*S == ' ') || (*S == '\n')) S++;
	else break;
    }
    if (S == EndP) return 0;

    // Have Start (S), so find End (E).  Go until NewLine, end, or out of room.
    E = S; X = 0;
    while (E < EndP) {
	if (*E == '\n') break;
	L = ED_BufferGetUTF8Len(*E);
	E += L;
	X += 1;

	if ((E - S) + ED_QRRespLen >= ED_RESPSTRLEN) {
	    E -= L; X -= 1;
	    break;
	}
    }

    *DataPP = S;
    *LenP = E - S;
    return X;
}

// ******************************************************************************
// ED_PaneQRAsk is the MAIN entry for the Query+Response.
//
// QueryP is the Query string... will become the Prompt.
// RespP is the Response string provided, as initial data.  It can be NULL.
// QRType is the response type expected... filename, Y/N etc.

void	ED_FrameQRAsk(ED_FramePointer FrameP, char * QueryP, char * RespP, Int16 QRType,
		      ED_QRRespFuncP RespFP, ED_QRAutoCompFuncP AutoCompFP)
{
    Int16	Len;
    char	*DP, *SP;

    Len = 0;
    SP = QueryP;
    DP = ED_QRPromptS;
    while ( (*DP++ = *SP++) ) Len++;
    ED_QRPromptLen = Len;
    ED_QRPromptX = ED_AuxQRGetX(ED_QRPromptS, Len);

    // Shorten the prompt if too long... normally only occurs with QueryReplace defaults.
    if (ED_QRPromptX > (FrameP->RowChars - 13)) {
	Len = ED_AuxQRGetPos(ED_QRPromptS, FrameP->RowChars - 13);
	ED_QRPromptX = FrameP->RowChars - 10;	// About to add 3 glyphs to it!
	ED_QRPromptS[Len++] = '>';
	ED_QRPromptS[Len++] = ':';
	ED_QRPromptS[Len++] = ' ';		
	ED_QRPromptS[Len] = 0;
	ED_QRPromptLen = Len;
    }

    ED_QRPaneP = FrameP->CurPaneP;		// Main determinant of QR mode
    ED_QRRespFP = RespFP;
    ED_QRAutoCompFP = AutoCompFP;
    ED_QRType = QRType;
    ED_QRRes = 0;
    
    ED_FrameQRSetResp(RespP);
    ED_FrameQRDraw(FrameP);
}

// ******************************************************************************
// ED_FrameQRSetResp will load the Strp into the QRRespS--as if the user just
// typed it.  It also takes care to set the related Len and X parameters as well
// as set the cursor.
//
// NOTE:	StrP can be NULL or empty... but must be null-terminated.

void	ED_FrameQRSetResp(char * StrP)
{
    Int16	Len;
    char	*DP, *SP;

    if (StrP) {
	Len = 0;
	SP = StrP;
	DP = ED_QRRespS;
	while ( (*DP++ = *SP++) ) Len++;
	ED_QRRespLen = Len;
	ED_QRRespX = ED_AuxQRGetX(ED_QRRespS, Len);
	ED_QRCursorPos = Len;
	ED_QRCursorX = ED_QRRespX;
	ED_QRStartPos = ED_QRStartX = 0;
	if (ED_QRCursorX + ED_QRPromptX >= ED_QRPaneP->FrameP->RowChars) {
	    ED_QRStartX = 5 + (ED_QRCursorX - (ED_QRPaneP->FrameP->RowChars - ED_QRPromptX));
	    ED_QRStartPos = ED_AuxQRGetPos(ED_QRRespS, ED_QRStartX);
	}
    } else {
	ED_QRRespS[0] = 0;
	ED_QRRespLen = 0;
	ED_QRRespX = 0;
	ED_QRCursorPos = 0;
	ED_QRCursorX = 0;
	ED_QRStartPos = 0;
	ED_QRStartX = 0;
    }
}

// ******************************************************************************
// ED_FrameQRDraw draws the Prompt as well as the typed Response.  It auto-scrolls
// horizontally to keep the cursor visible.

#define		ED_QRMINRESPX	((ED_QRType == ED_QRLetterType) ? 1 : 5)

void	ED_FrameQRDraw(ED_FramePointer FrameP)
{
    Int32	LineY = (FrameP->RowCount - 1) * ED_Row;
    Int32	L, Len;
    Int16	RemX;

    // Clear the area before drawing
    XftDrawRect(FrameP->XftDP, &ED_XCArr[ED_White], 0, LineY, FrameP->WinWidth, ED_Height + 1);
    if (ED_QRPaneP == NULL) {
	// May have to draw ED_FrameEchoS if there a msg pending!  Then just leave.
	ED_FrameDrawEchoLine(FrameP);
	return;
    }

    // Scroll the response horizontally to keep Cursor visible... assuming there is
    // any room to show it!  Manipulate CursorX, then update its Pos.

    RemX = FrameP->RowChars - ED_QRPromptX;
    ED_QRStrikeOut = (RemX < ED_QRMINRESPX);

    if (! ED_QRStrikeOut) {
	if (((ED_QRRespLen - ED_QRStartX) + 5) < RemX)		// Stretch out the line, if room
	    ED_QRStartX = ED_QRRespLen + 5 - RemX;
	if ((ED_QRCursorX - ED_QRStartX) >= RemX)		// Push line to the left
	    ED_QRStartX = 5 + (ED_QRCursorX - RemX);
	if (ED_QRCursorX <= ED_QRStartX)			// Pull line to the right
	    ED_QRStartX = ED_QRCursorX - 5;
	if (ED_QRStartX < 0) ED_QRStartX = 0;			// ... went too far right!

	ED_QRStartPos = ED_AuxQRGetPos(ED_QRRespS, ED_QRStartX);
    }
    // Draw the prompt... or as much of it that fits!
    Len = ED_QRPromptLen;
    if (ED_QRPromptX > FrameP->RowChars) Len = ED_AuxQRGetPos(ED_QRPromptS, FrameP->RowChars);
    XftDrawStringUtf8(FrameP->XftDP, &ED_XCArr[ED_Blue], ED_XFP, ED_FRAMELMARGIN, LineY + ED_Ascent,
		   (XftChar8 *)ED_QRPromptS, Len);

    // Draw the response, if there is room
    if (ED_QRRespLen) {
	if (ED_QRStrikeOut)
	    ED_AuxQRDrawScrollMark(FrameP, -1);			// -1 Arg is irrelevent here!
	else {
	    // If left side of response was scrolled, start midstream... and skip 1 char for scroll mark.
	    if (ED_QRStartX) {
		Int16	BeginH = ED_QRPromptX + 1;
		Int16	EndH = BeginH + ED_QRRespX - ED_QRStartX;
		char	*StartP;

		StartP = ED_QRRespS + ED_QRStartPos;
		L = ED_BufferGetUTF8Len(*StartP);		// Moving over 1 char
		StartP += L;

		Len = ED_QRRespLen - (ED_QRStartPos + L);
		if (EndH > FrameP->RowChars)
		    Len = ED_AuxQRGetPos(StartP, FrameP->RowChars - BeginH);

		XftDrawStringUtf8(FrameP->XftDP, &ED_XCArr[ED_Black], ED_XFP,
				  ED_FRAMELMARGIN + (ED_Advance * (ED_QRPromptX + 1)), LineY + ED_Ascent,
				  (XftChar8 *)StartP, Len);

		ED_AuxQRDrawScrollMark(FrameP, 0);	// Left scroll mark
		if (EndH > FrameP->RowChars)
		    ED_AuxQRDrawScrollMark(FrameP, 1);	// Right scroll mark

	    } else {
		// Start at beginning of response.
		Len = ED_QRRespLen;
		if (ED_QRPromptX + ED_QRRespX > FrameP->RowChars)
		    Len = ED_AuxQRGetPos(ED_QRRespS, FrameP->RowChars - (ED_QRPromptX));

		XftDrawStringUtf8(FrameP->XftDP, &ED_XCArr[ED_Black], ED_XFP,
				  ED_FRAMELMARGIN + (ED_Advance * ED_QRPromptX), LineY + ED_Ascent,
				  (XftChar8 *)ED_QRRespS, Len);
		if (Len < ED_QRRespLen)
		    ED_AuxQRDrawScrollMark(FrameP, 1);	// Right scroll mark
	    }
	}
    }

    // Drawing erases the cursor, so set the flags to reflect it.
    FrameP->Flags &= ~(ED_FRAMEQRBOXCURSORFLAG | ED_FRAMEQRSOLIDCURSORFLAG);		       
}


// ******************************************************************************
// ******************************************************************************
// Simple commands for a 1 line editor... shares KillRing with Panes/Frames.
// Does NOT support selection range.

Int16	ED_FrameQRDelPrevChar(void)
{
    char	*P;
    Int16	L;
    
    if (ED_QRCursorPos > 0) {
	P = ED_QRRespS + ED_QRCursorPos - 1;
	L = 1;
	while (ED_BUFFERISMIDUTF8(*P)) P--, L++;
       
	memmove(ED_QRRespS + ED_QRCursorPos - L,
		ED_QRRespS + ED_QRCursorPos,
		(ED_QRRespLen - ED_QRCursorPos) + 1);
	ED_QRCursorPos -= L;	// Lost L bytes
	ED_QRCursorX -= 1;	// Lost 1 char-width
	ED_QRRespLen -= L;
	ED_QRRespX -= 1;
	return 0;
    } 

    return 1;
}

Int16	ED_FrameQRDelNextChar(void)
{
    Int16	L;
    
    if (ED_QRRespLen > ED_QRCursorPos) {
	L = ED_BufferGetUTF8Len(ED_QRRespS[ED_QRCursorPos]);
    
	memmove(ED_QRRespS + ED_QRCursorPos,
		ED_QRRespS + ED_QRCursorPos + L,
		(ED_QRRespLen + 1) - ED_QRCursorPos);
	ED_QRRespLen -= L;
	ED_QRRespX -= 1;
	return 0;
    }

    return 1;
}

Int16	ED_FrameQRGoLeft(void)
{
    char	*P;
    
    if (ED_QRCursorPos) {
	ED_QRCursorX -= 1;
	ED_QRCursorPos -= 1;
	P = ED_QRRespS+ ED_QRCursorPos;
	while (ED_BUFFERISMIDUTF8(*P)) P--, ED_QRCursorPos--;

	return 0;
    }

    return 1;
}

Int16	ED_FrameQRGoRight(void)
{
    if (ED_QRCursorPos < ED_QRRespLen) {
	ED_QRCursorX += 1;
	ED_QRCursorPos += ED_BufferGetUTF8Len(ED_QRRespS[ED_QRCursorPos]);
	return 0;
    }

    return 1;
}

Int16	ED_AuxQRLeftWordScan(Int16 InWord)
{
    Int16	In;
    char	*P;

    In = InWord;    
    while (In == InWord) {
	ED_QRCursorPos -= 1;
	ED_QRCursorX -= 1;
	P = ED_QRRespS + ED_QRCursorPos;
	while (ED_BUFFERISMIDUTF8(*P)) P--, ED_QRCursorPos--;
	if (ED_QRCursorPos == 0) return 1;
	In = ED_BufferCIsAlpha(*P);
    }
    return 0;
}

Int16	ED_FrameQRGoLeftWord(void)
{
    Int16	OldCursorPos = ED_QRCursorPos;

    if (ED_QRCursorPos == 0) return 1;			// Failed
    
    // Initially take 1 step back
    ED_QRCursorX -= 1, ED_QRCursorPos -= 1;
    while (ED_BUFFERISMIDUTF8(ED_QRRespS[ED_QRCursorPos])) ED_QRCursorPos--;
    
	if (! ED_BufferCIsAlpha(ED_QRRespS[ED_QRCursorPos]))
	    if (ED_AuxQRLeftWordScan(0)) goto DoneDone;
	if (ED_AuxQRLeftWordScan(1)) goto DoneDone;

    // Now take 1 step forward (right)
    ED_QRCursorX += 1;
    ED_QRCursorPos += ED_BufferGetUTF8Len(ED_QRRespS[ED_QRCursorPos]);

DoneDone:
    if (ED_QRCursorPos == OldCursorPos)	return 1;	// Failed
	
    ED_QRCursorX = ED_AuxQRGetX(ED_QRRespS, ED_QRCursorPos);
    return 0;
}

Int16	ED_FrameQRKillPrevWord(void)
{
    Int16	OldCursorPos = ED_QRCursorPos;
    Int16	DeltaPos, DeltaX;

    if (ED_QRCursorPos) {

	// Initially take 1 step back
	ED_QRCursorX -= 1, ED_QRCursorPos -= 1;
	while (ED_BUFFERISMIDUTF8(ED_QRRespS[ED_QRCursorPos])) ED_QRCursorPos--;   

	    if (! ED_BufferCIsAlpha(ED_QRRespS[ED_QRCursorPos]))
		if (ED_AuxQRLeftWordScan(0)) goto DoneDone;
	    if (ED_AuxQRLeftWordScan(1)) goto DoneDone;

	// Now take 1 step forward (right)
	ED_QRCursorX += 1;
	ED_QRCursorPos += ED_BufferGetUTF8Len(ED_QRRespS[ED_QRCursorPos]);

    DoneDone:
	DeltaPos = OldCursorPos - ED_QRCursorPos;
	if (DeltaPos == 0) return 1;			// Failed.

	DeltaX = ED_AuxQRGetX(ED_QRRespS + ED_QRCursorPos, DeltaPos);
	ED_KillRingAdd(ED_QRRespS + ED_QRCursorPos, DeltaPos, 0);
	memmove(ED_QRRespS + ED_QRCursorPos, ED_QRRespS + OldCursorPos, ED_QRRespLen - OldCursorPos);
	ED_QRRespLen -= DeltaPos;
	ED_QRRespX -= DeltaX;
	// QRCursorPos and QRCursorX were already set above.
	ED_QRThisCmd = ED_QRKillCmd;

	return 0;
    }
    
    return 1;
}

Int16	ED_AuxQRRightWordScan(Int16 InWord)
{
    Int16	In;

    In = InWord;
    while (In == InWord) {
	ED_QRCursorPos += ED_BufferGetUTF8Len(ED_QRRespS[ED_QRCursorPos]);
	ED_QRCursorX += 1;
	if (ED_QRCursorPos >= ED_QRRespLen) return 1;
	In = ED_BufferCIsAlpha(ED_QRRespS[ED_QRCursorPos]);
    }
    return 0;
}

Int16	ED_FrameQRGoRightWord(void)
{
    Int16	OldCursorPos = ED_QRCursorPos;

    if (! ED_BufferCIsAlpha(ED_QRRespS[ED_QRCursorPos]))
	if (ED_AuxQRRightWordScan(0)) goto DoneDone;
    if (ED_AuxQRRightWordScan(1)) goto DoneDone;

DoneDone:
    if (ED_QRCursorPos == OldCursorPos)	return 1;	// Failed

    ED_QRCursorX = ED_AuxQRGetX(ED_QRRespS, ED_QRCursorPos);
    return 0;
}

Int16	ED_FrameQRKillNextWord(void)
{
    Int16	OldCursorPos = ED_QRCursorPos;
    Int16	DeltaPos, DeltaX;

    if (ED_QRCursorPos < ED_QRRespLen) {

	if (! ED_BufferCIsAlpha(ED_QRRespS[ED_QRCursorPos]))
	    if (ED_AuxQRRightWordScan(0)) goto DoneDone;
	if (ED_AuxQRRightWordScan(1)) goto DoneDone;
    DoneDone:
	DeltaPos = ED_QRCursorPos - OldCursorPos;
	if (DeltaPos == 0) return 1;	// Failed

	DeltaX = ED_AuxQRGetX(ED_QRRespS + OldCursorPos, DeltaPos);
	ED_KillRingAdd(ED_QRRespS + OldCursorPos, DeltaPos, 1);
	memmove(ED_QRRespS + OldCursorPos, ED_QRRespS + ED_QRCursorPos, ED_QRRespLen - ED_QRCursorPos);
	ED_QRRespLen -= DeltaPos;
	ED_QRRespX -= DeltaX;
	ED_QRCursorPos = OldCursorPos;
	ED_QRCursorX -= DeltaX;
	ED_QRThisCmd = ED_QRKillCmd;
	return 0;
    }

    return 1;
}

Int16	ED_FrameQRKill(void)
{
    if (ED_QRCursorPos < ED_QRRespLen) {
	ED_KillRingAdd(ED_QRRespS + ED_QRCursorPos,
		       ED_QRRespLen - ED_QRCursorPos, 1);
	ED_QRRespS[ED_QRCursorPos] = 0;
	ED_QRRespLen = ED_QRCursorPos;
	ED_QRRespX = ED_QRCursorX;
	ED_QRThisCmd = ED_QRKillCmd;
	return 0;
    }

    return 1;
}

Int16	ED_FrameQRYank(Int16 Pop)
{
    char	*DataP;
    Int32	DataLen;
    Int16	DataX;

    ED_KillRingYank(Pop, &DataP, &DataLen, 1);

    // Sanitize to get rid of leading space and limit length...
    if (DataLen && (DataX = ED_AuxQRSanitize(&DataP, &DataLen))) {
	// Open a gap first, then copy DataP to it.
	memmove(ED_QRRespS + ED_QRCursorPos + DataLen,
		ED_QRRespS + ED_QRCursorPos,
		ED_QRRespLen - ED_QRCursorPos);
	memmove(ED_QRRespS + ED_QRCursorPos, DataP, DataLen);
	// Record YankPos to allow YankPop, then move CursorPos
	ED_QRYankPos = ED_QRCursorPos;
	ED_QRYankX = DataX;
	ED_QRRespLen += DataLen;
	ED_QRRespX += DataX;
	ED_QRCursorPos += DataLen;
	ED_QRCursorX += DataX;
	ED_QRThisCmd = ED_QRYankCmd;
	return 0;
    }

    return 1;
}

Int16	ED_FrameQRYankPop(void)
{
//    Uns16	LastMod = ED_QRLastCmd >> 16;
//    Uns16	LastSym = (Uns16)(0x0000FFFF & ED_QRLastCmd);

    if (ED_QRLastCmd != ED_QRYankCmd)
	return 0;

    // First undo the last Yank!  (From YankPos to CursorPos)
    // Important to have the correct QRespLen because Sanitize needs it.
    memmove(ED_QRRespS + ED_QRYankPos,
	    ED_QRRespS + ED_QRCursorPos,
	    ED_QRRespLen - ED_QRCursorPos);
    ED_QRRespLen -= ED_QRCursorPos - ED_QRYankPos;
    ED_QRRespX -= ED_QRYankX;
    ED_QRCursorPos = ED_QRYankPos;
    ED_QRCursorX -= ED_QRYankX;

    // Now Yank again, but Pop 1 this time
    return ED_FrameQRYank(1);
}

// ******************************************************************************
// ED_FrameQRHandleCommand dispatches all commands for QR.
// return 1 to redraw, 0 otherwise.

Int16	ED_FrameQRHandleCommand(ED_FramePointer FrameP, Uns16 SymChar, Int32 Mods)
{
    Int16	IsCtrl = (Mods & ControlMask);
    Int16	IsMeta = (Mods & Mod1Mask);

    // No C-M commands supported
    if (IsCtrl && IsMeta) goto BadCommand;

    if (IsCtrl) {
	switch (SymChar) {
	    case 'a': case 'A':
		ED_QRCursorPos = 0;
		ED_QRCursorX = 0;
		break;

	    case 'e': case 'E':
		ED_QRCursorPos = ED_QRRespLen;
		ED_QRCursorX = ED_QRRespX;
		break;

	    case 'b': case 'B':
		if (ED_FrameQRGoLeft()) goto BadCommand;
		break;

	    case 'd': case 'D':
		if (ED_FrameQRDelNextChar()) goto BadCommand;
		break;

	    case 'f': case 'F':
		if (ED_FrameQRGoRight()) goto BadCommand;
		break;

	    case 'k': case 'K':
		if (ED_FrameQRKill()) goto BadCommand;
		break;

	    case 'y': case 'Y':
		if (ED_FrameQRYank(0)) goto BadCommand;
		break;

	    case 0xff08:	// c-backspace
		if (ED_FrameQRKillPrevWord()) goto BadCommand;
		break;

	    default: goto BadCommand;
	}

    } else if (IsMeta) {

	switch (SymChar) {
	    case 'y': case 'Y':
		if (ED_FrameQRYankPop()) goto BadCommand;
		break;

	    case 'b': case 'B':
		if (ED_FrameQRGoLeftWord()) goto BadCommand;
		break;

	    case 'f': case 'F':
		if (ED_FrameQRGoRightWord()) goto BadCommand;
		break;

	    case 'd': case 'D':
		if (ED_FrameQRKillNextWord()) goto BadCommand;
		break;

	    case 0xff08:	// M-backspace
		if (ED_FrameQRKillPrevWord()) goto BadCommand;
		break;
	   
	    default: goto BadCommand;
	}
    
    } else { // Not Ctrl or Meta

	switch (SymChar) {
	
	    case 0xff08:	// Backspace
		if (ED_FrameQRDelPrevChar()) goto BadCommand;
		break;

	    case 0xff09:	// Tab--auto-complete!
		if (ED_QRAutoCompFP) {
		    if ((*ED_QRAutoCompFP)()) goto BadCommand;
		    // Added to QRRespS, so re-draw!
		    ED_FrameQRDraw(FrameP);
		    ED_FrameQRDrawBlinker(FrameP);
		}
		break;

	    case 0xff0a:	// Linefeed /n
	    case 0xff0d:	// Return/Enter
		// Process the response
		if ((*ED_QRRespFP)()) goto BadCommand;
		break;

	    case 0xff50:	// Home
	    case 0xff58:	// begin
		ED_QRCursorPos = 0;
		ED_QRCursorX = 0;
		break;

	    case 0xff57:	// End
		ED_QRCursorPos = ED_QRRespLen;
		ED_QRCursorX = ED_QRRespX;
		break;

	    case 0xff51:	// Left
		if (ED_FrameQRGoLeft()) goto BadCommand;
		break;

	    case 0xff53:	// right
		if (ED_FrameQRGoRight()) goto BadCommand;
		break;

	    case 0xffff:	// Delete
		if (ED_FrameQRDelNextChar()) goto BadCommand;
		break;

	    default:
		goto BadCommand;

	}
    }

    return 1;

BadCommand:
    ED_FrameFlashError(FrameP);
    return 0;
}

// ******************************************************************************
void	ED_FrameQRAbortOut(char * MsgP)
{
    ED_FramePointer	FP;

    if (ED_QRExitFP) {
	(*ED_QRExitFP)();
	ED_QRExitFP = NULL;
    }

    FP = ED_QRPaneP->FrameP;
    ED_QRPaneP = NULL;

    ED_FrameEchoLen = 0;
    if (MsgP) ED_FrameSetEchoS(ED_ECHOMSGMODE, MsgP);
    ED_FrameDrawEchoLine(FP);
}

// ******************************************************************************
// ED_FrameQRHandleChars takes care of all keyboard input for QR mode.

void	ED_FrameQRHandleChars(ED_FramePointer FrameP, Uns16 SymChar, Int16 Mods, Int32 StrLen, char * StrP)
{
    Int16		StrX;
    
    ED_QRThisCmd = ED_QRSomeCmd;

    if ((Mods & (ControlMask | Mod1Mask)) ||
	((SymChar & 0xff00) == 0xff00)) {

	// Abort out!
	if (ControlMask && (SymChar == 'g' || SymChar == 'G')) {
	    ED_FrameQRAbortOut(ED_STR_EchoQuit);
	    goto GoodExit;
	}

	if (ED_QRStrikeOut || (ED_QRType == ED_QRLetterType))	// NO COMMANDS!
	    goto BadExit;
	
	if (ED_FrameQRHandleCommand(FrameP, SymChar, Mods)) {
	    // Possible that ED_FrameKill was just called, so no Window.
	    if (! (FrameP->Flags & ED_FRAMENOWINFLAG)) {
		ED_FrameQRDraw(FrameP);
		ED_FrameQRDrawBlinker(FrameP);
	    }
	}
	goto GoodExit;
    }
    
    // Will copy the StrP all at once... check for room... also good escape if StrikeOut!
    if (ED_QRStrikeOut || (ED_QRRespLen + StrLen >= ED_RESPSTRLEN))		// Leave room for NULL byte at end
	goto BadExit;

    // Single char responses should NOT wait for a <LINEFEED>.  Just process!
    if (ED_QRType == ED_QRLetterType) {
    
	memcpy(ED_QRRespS, StrP, StrLen);		// Set it so QRRespFP() can read it
	ED_QRRespLen += StrLen;
	ED_QRRespS[ED_QRRespLen] = 0;			// Terminating Null
    
	if ((*ED_QRRespFP)()) {
	    ED_QRRespLen = 0;				// Reset in case windows are switched/redrawn
	    ED_QRRespS[0] = 0;
	    goto BadExit;
	}
	ED_FrameQRDraw(FrameP);
	ED_FrameQRDrawBlinker(FrameP);	
	goto GoodExit;
    }

    // Figure out X displacement for StrP, UTF8 must be 2 bytes or more!
    StrX = 1;
    if (StrLen > 1)
	StrX = ED_AuxQRGetX(StrP, StrLen);
	
    // If not at end, make room first, then copy
    if (ED_QRCursorPos < ED_QRRespLen) {
	memmove(ED_QRRespS + ED_QRCursorPos + StrLen, ED_QRRespS + ED_QRCursorPos, ED_QRRespLen - ED_QRCursorPos);
	memcpy(ED_QRRespS + ED_QRCursorPos, StrP, StrLen);
    } else
	memcpy(ED_QRRespS + ED_QRRespLen, StrP, StrLen);

    ED_QRCursorPos += StrLen;
    ED_QRRespLen += StrLen;
    ED_QRCursorX += StrX;
    ED_QRRespX += StrX;
    
    ED_QRRespS[ED_QRRespLen] = 0;			// Terminating Null
    
    ED_FrameQRDraw(FrameP);				// Will erase old blinker
    ED_FrameQRDrawBlinker(FrameP);

GoodExit:
    ED_QRLastCmd = ED_QRThisCmd;
    sc_BlinkTimerReset();
    return;

BadExit:
    ED_QRLastCmd = ED_QRBadCmd;
    ED_FrameFlashError(FrameP);
    sc_BlinkTimerReset();
    return;
}

// ******************************************************************************
// ED_FrameQRDrawBlinker draws the Blinker for the QR mode in the EchoLine.

void	ED_FrameQRDrawBlinker(ED_FramePointer FrameP)
{
    if (ED_QRPaneP) {
	if (FrameP->Flags & ED_FRAMEFOCUSFLAG) {
	    // Erase box cursor if it had it.  Gets solid cursor.  

	    if (FrameP->Flags & ED_FRAMEQRBOXCURSORFLAG) ED_FrameQRDrawCursor(FrameP, 1);
	    if (! (FrameP->Flags & ED_FRAMEQRSOLIDCURSORFLAG)) ED_FrameQRDrawCursor(FrameP, 0);

	} else {
	    // Erase solid cursor if it had it.  Gets a box cursor.

	    if (FrameP->Flags & ED_FRAMEQRSOLIDCURSORFLAG) ED_FrameQRDrawCursor(FrameP, 0);
	    if (! (FrameP->Flags & ED_FRAMEQRBOXCURSORFLAG)) ED_FrameQRDrawCursor(FrameP, 1);
	}
    }
}

// ******************************************************************************
// ED_FrameQRDrawCursor draws the actual cursor, Box or Solid, for the Response
// in the Frame's Echo line.

void	ED_FrameQRDrawCursor(ED_FramePointer FrameP, Int16 Box)
{
    Int32	X, Y;

    if (ED_QRStrikeOut) return;

    X = ED_QRPromptX + ED_QRCursorX - ED_QRStartX;
    if (X >= FrameP->RowChars) return;

    X = ED_FRAMELMARGIN + (X * ED_Advance);
    Y = (FrameP->RowCount - 1) * ED_Row;

    if (Box) {
	XDrawRectangle(ED_XDP, FrameP->XWin, FrameP->BlinkerGC, X, Y, ED_Advance-1, ED_Height-1);
	FrameP->Flags ^= ED_FRAMEQRBOXCURSORFLAG;
    } else {
	XFillRectangle(ED_XDP, FrameP->XWin, FrameP->BlinkerGC, X, Y, ED_Advance, ED_Height);
	FrameP->Flags ^= ED_FRAMEQRSOLIDCURSORFLAG;
    }
}


// ******************************************************************************
// EDCB_QRAutoCompPathFunc is a callback to auto-complete file/dir names used
// in QR.  It uses scandir with custom Cmp and Filter functions.

// Used for scandir instead of alphasort--the latter DID NOT properly
// compare with '_' in names... confused everything!
Int32	EDCB_QRDirEntCmp(const struct dirent **APP, const struct dirent **BPP)
{
    return strcmp((*APP)->d_name, (*BPP)->d_name);
}

// Return 0 to ignore file
// Ignores "invisible" files, (Start with '.' or end with '~').
Int32	EDCB_QRDirEntFilter(const struct dirent *DP)
{
    return ((DP->d_name[0] != '.') && (DP->d_name[strlen(DP->d_name) - 1] != '~'));
}

// Return 0 == Good response (DID complete)
// Return 1 == Bad response (Cannot complete)
Int16	EDCB_QRAutoCompPathFunc(void)
{
    struct dirent	**DEntPArr;
    Int32		DCount, CurD, StartD, EndD, NameLen;
    Int16		Res;
    Int32		HalfLen = (ED_RESPSTRLEN / 2) - 1;
    
    ED_QRRespS[ED_QRRespLen] = 0;		// just in case
    if (ED_QRRespLen == 0) return 1;

    // Handle Tilde, Dot, DotDot and leading blanks.  Shoves response into ED_Path and ED_Name globals.
    if (ED_UtilNormalizePath(ED_QRRespS, ED_Path, ED_BUFFERPATHLEN + 1, ED_Name, NAME_MAX + 1)) return 1;
    if ((ED_Path[0] == 0) || (ED_Name[0] == 0)) return 1;
    NameLen = strlen(ED_Name);
    
    // ED_Path has the pathname, so far.
    DCount = scandir(ED_Path, &DEntPArr, EDCB_QRDirEntFilter, EDCB_QRDirEntCmp);
    if (DCount == 0) return 1;

#ifdef DEBUG
    if (0) {
	printf("AutoCompFile   Path:[%s]  Name:[%s]\n", ED_Path, ED_Name);

	CurD = 0;
	while (CurD < DCount) {
	    printf("    Ent[%d] -> [%s]\n", CurD, DEntPArr[CurD]->d_name);
	    CurD++;
	}
    }
#endif

    CurD = 0;
    StartD = EndD = -1;
    while (CurD < DCount) {
	Res = strncmp(ED_Name, DEntPArr[CurD]->d_name, NameLen);
	if (Res == 0) {
	    // Found a match, but there may be others!!  Catch first and last
	    if (StartD == -1) StartD = CurD;
	    EndD = CurD;
	} else if (Res < 0)
	    break;

	CurD++;
    }

    if (StartD == -1)			// Found nothing
	goto FailReturn;

    // Perfet match if StartD and EndD are the same!
    // Also if d_name is exactly the same length as NameLen...
    // By definition, d_name CANNOT be shorter, so if it is NOT
    // longer (easier test), then it *must* be the same length.
    if ((StartD == EndD) ||
	(DEntPArr[StartD]->d_name[NameLen] == 0)) {
	sprintf(ED_FullPath, "%.*s/%.*s", HalfLen, ED_Path, HalfLen, DEntPArr[StartD]->d_name);
	ED_FrameQRSetResp(ED_FullPath);
	goto SuccessReturn;
    }

    // If we get here, we have multiple matches, from StartD to EndD.
    // Must do a string intersection to find the longest matching substring!!
    strcpy(ED_Name, DEntPArr[StartD]->d_name);	// Start here
    CurD = StartD + 1;
    while (CurD <= EndD) {
	ED_UtilIntersectStr(ED_Name, DEntPArr[CurD]->d_name);
	CurD++;
    }
    
    // Report the result and fall through to Success!!
    sprintf(ED_FullPath, "%.*s/%.*s", HalfLen, ED_Path, HalfLen, ED_Name);
    ED_FrameQRSetResp(ED_FullPath);
    
SuccessReturn:
    Res = 0;
    goto Cleanup;
FailReturn:
    Res = 1;
    
Cleanup:
    while (DCount--) free(DEntPArr[DCount]);
    free(DEntPArr);
    return Res;
}

// ******************************************************************************
// ******************************************************************************
// The Pop-Up (PU) module creates a little pop-up window, like a pop-up menu,
// that can display a number of items and allow for selection.  The PU window
// can be moved around and CurFrame remembers its last location, so will open
// a new one where the last one was.  These windows are generally manipulated
// with the keyboard, but do support limited mouse-interactions.
//
// PUList is a particular kind of PU window.  It has a numbered column on the
// left and a selection area displaying a column of entried on the right.
// A number of PUList windows are currently implemented and they are differentiated
// with the callback routines (drawing and selection) provided by the creator.
//
// All state and information is stored in a PURecord and indicated with the PUP
// pointer.  The system currently allocates (statically) a single global PURecord.
// But this can be easily changed--e.g. can use a Store as slab allocator.

void	ED_PUListEventHandler(XEvent * EventP, ED_PUPointer PUP);
void	ED_PUListDraw(ED_PUPointer PUP);
void	ED_PUListDestroy(ED_PUPointer PUP);
void	ED_PUListHandleKey(ED_PUPointer PUP, XEvent * EventP);
void	ED_PUListFlashError(ED_PUPointer PUP);
void	ED_PUListHandleClick(ED_PUPointer PUP, XEvent * EventP);

// ******************************************************************************
// ED_PUListEventHandler handles all incoming events for PUList windows.

void	ED_PUListEventHandler(XEvent * EventP, ED_PUPointer PUP)
{
    switch (EventP->type){
	case Expose:
	    if (EventP->xexpose.count == 0)
		ED_PUListDraw(PUP);
	    break;

	case ButtonPress:
	    ED_PUListHandleClick(PUP, EventP);
	    break;

	case KeyPress:
	    ED_PUListHandleKey(PUP, EventP);
	    break;

	case FocusOut:
	    ED_PUListDestroy(PUP);
	    break;

	default: break;
    }
}

// ******************************************************************************
// ED_PUListScrollDown and ED_PUListScrollUp handle scrolling for PUList windows.
// There are no scroll bars/thumbs.  Instead UP/DOWN arrows are used to select
// entries above or below respectively.  If the selected entry falls outside the
// viewable area, the display is scrolled by one entry to make it visible.

void	ED_PUListScrollDown(ED_PUPointer PUP)
{
    if (PUP->EntrySel < PUP->EntryTotal - 1) {
	PUP->EntrySel += 1;
	if (PUP->EntrySel >= (PUP->EntryTop + PUP->EntryCount))
	    PUP->EntryTop += 1;
	ED_PUListDraw(PUP);
    } else
	ED_PUListFlashError(PUP);
}

void	ED_PUListScrollUp(ED_PUPointer PUP)
{
    if (PUP->EntrySel) {
	PUP->EntrySel -= 1;
	if (PUP->EntrySel < PUP->EntryTop)
	    PUP->EntryTop -= 1;
	ED_PUListDraw(PUP);		
    } else
	ED_PUListFlashError(PUP);
}

// ******************************************************************************
// ED_PUListDragWin allows the mouse to drag and reposition the PUList window.
// The new location is stashed in the Frame.

void	ED_PUListDragWin(ED_PUPointer PUP, XEvent * EventP)
{
    Window		RootW, ChildW;
    Int32		DragX, DragY, X, Y, LocalX, LocalY, DeltaX, DeltaY;
    Uns32		MouseMask;

    // Get the click coords in rootwindow coords + stash them.
    XTranslateCoordinates(ED_XDP, PUP->XWin, XDefaultRootWindow(ED_XDP),
			  EventP->xbutton.x, EventP->xbutton.y, &DragX, &DragY, &ChildW);

    while (1) {
	XQueryPointer(ED_XDP, PUP->XWin, &RootW, &ChildW, &X, &Y, &LocalX, &LocalY, &MouseMask);
	if (! (MouseMask & Button1Mask)) break;

	DeltaX = X - DragX;				// How much did we move?
	DeltaY = Y - DragY;
	if (DeltaX || DeltaY) {
	    PUP->WinX += DeltaX;			// New location
	    PUP->WinY += DeltaY;
	    XMoveWindow(ED_XDP, PUP->XWin, PUP->WinX, PUP->WinY);
	    DragX = X;					// Ready for new cycle
	    DragY = Y;
	}
    }

    // Record the pop-up location in the FrameP for next showing
    PUP->PaneP->FrameP->PUWinX = PUP->WinX;
    PUP->PaneP->FrameP->PUWinY = PUP->WinY;
}

// ******************************************************************************
// ED_PUListHandleClick handles mouse events...  MouseWheel (Button4/5)
// moves/scrolls the selection up and down.  Button1 can select from the
// visible entries as well as move/reposition the whole window.

void	ED_PUListHandleClick(ED_PUPointer PUP, XEvent * EventP)
{
    Int32	X = EventP->xbutton.x;
    Int32	Y = EventP->xbutton.y;
    Int32	Button = EventP->xbutton.button;
    Int32	XChar = (X - ED_PU_XMARGIN) / ED_Advance;
    Int32	YRow = (Y - ED_PU_YMARGIN) / ED_Row;

    if (Button == Button4) {		// Scroll Up
	ED_PUListScrollUp(PUP);
	return;
    } else if (Button == Button5) {	// Scroll Down
	ED_PUListScrollDown(PUP);
	return;
    } else if (Button == Button1) {

	if (YRow <= 1)
	    ED_PUListDragWin(PUP, EventP);
	
	else if ((XChar > PUP->Config1) && (XChar < PUP->RowChars) &&
	    (YRow > 1) && (YRow < (PUP->RowCount - 1))) {

	    PUP->EntrySel = ((YRow - 1) / PUP->EntryRows) + PUP->EntryTop;
	    ED_PUListDraw(PUP);
	} else
	    ED_PUListFlashError(PUP);

	return;
    }
}

// ******************************************************************************
// ED_PUListHandleKey handles the keyboard for the PUList window.
//
// NOTE:	Return physically selects an entry, then exits.
//		There is support for H scrolling, in case an entry is too long
//		fit... but all entries are H scrolled as one.
//
// NOTE:	ED_PUListDestroy will call the ExitFP, if there is one!

void	ED_PUListHandleKey(ED_PUPointer PUP, XEvent * EventP)
{
    char	KeyBuf[32];
    KeySym	KS;

    XLookupString(&EventP->xkey, KeyBuf, 31, &KS, NULL);

    switch (KS) {
	case XK_Up: case XK_KP_Up: case XK_P: case XK_p:
	    ED_PUListScrollUp(PUP);
	    break;

	case XK_Down: case XK_KP_Down: case XK_N: case XK_n:
	    ED_PUListScrollDown(PUP);
	    break;

	case XK_Right: case XK_KP_Right:
	    if (PUP->EntryCharMax > PUP->EntryCharScroll + PUP->RowChars - (PUP->Config1 + 1)) {
		PUP->EntryCharScroll += 1;
		ED_PUListDraw(PUP);
	    } else
		ED_PUListFlashError(PUP);
	    break;

	case XK_Left: case XK_KP_Left:
	    if (PUP->EntryCharScroll) {
		PUP->EntryCharScroll -= 1;
		ED_PUListDraw(PUP);
	    } else
		ED_PUListFlashError(PUP);
	    break;

	case XK_Return: case XK_Linefeed: case XK_KP_Enter:
	    (*PUP->ReturnFP)(PUP);			// NO BREAK, run through
	default:					// Everything else terminates!
	    ED_PUListDestroy(PUP);
	    break;
    }
}

// ******************************************************************************
// ED_PUListFlashError does the job of a missing SysBeep/bell for the PUList window.
// It simply flashes the currently selected entry.

void	ED_PUListFlashError(ED_PUPointer PUP)
{
    Int32		LineY, LineX, SpanX, SpanY;
    struct pollfd	PollR;
    XEvent		Event;
    GC *		GCP = &PUP->PaneP->FrameP->BlinkerGC;

    LineY = ((((PUP->EntrySel - PUP->EntryTop) * PUP->EntryRows) + 1) * ED_Row) + ED_PU_YMARGIN;
    SpanY = PUP->EntryRows * ED_Row;
    LineX = ED_PU_XMARGIN + (PUP->Config1 * ED_Advance);
    SpanX = (PUP->RowChars - PUP->Config1) * ED_Advance;

    PollR.fd = ConnectionNumber(ED_XDP);	// Negate to ignore all events
    PollR.events = POLLIN;
    XFillRectangle(ED_XDP, PUP->XWin, *GCP, LineX, LineY, SpanX, SpanY);
    XFlush(ED_XDP);
    poll(&PollR, 1, 100);
    XFillRectangle(ED_XDP, PUP->XWin, *GCP, LineX, LineY, SpanX, SpanY);
    XFlush(ED_XDP);

    while (XCheckMaskEvent(ED_XDP, KeyPressMask, &Event));
}

// ******************************************************************************
// ED_PUListDrawNumberFrame draws the main body of the PUList window--excluding the
// interior area for entries.  LeftCol indicates how many charspaces should be
// allocated to the numbered column on the left.

void	ED_PUListDrawNumberFrame(ED_PUPointer PUP, Int16 LeftCol)
{
    Int32	LineX, LineY, SpanX, SpanY;

    // Draw the right fringe
    LineX = ED_PU_XMARGIN + (ED_Advance * PUP->RowChars);
    XftDrawRect(PUP->XftDP, &ED_XCArr[ED_TitleGray], LineX, 0, ED_PU_XMARGIN, PUP->WinHeight);

    // Clear the left "Mark" column
    LineX = ED_PU_XMARGIN + (ED_Advance * LeftCol);
    XftDrawRect(PUP->XftDP, &ED_XCArr[ED_TitleGray], 0, 0, LineX, PUP->WinHeight);

    // Draw to Title/Top Scroll area
    SpanX = PUP->WinWidth - ED_PU_XMARGIN;
    SpanY = ED_PU_YMARGIN + ED_Row;
    XftDrawRect(PUP->XftDP, &ED_XCArr[ED_TitleGray], ED_PU_XMARGIN, 0, SpanX, SpanY);
    
    if (PUP->TitleLen > PUP->RowChars - 3) PUP->TitleLen = PUP->RowChars - 3;
    LineY = ED_PU_YMARGIN + ED_Ascent - (ED_PU_YMARGIN / 2);
    XftDrawStringUtf8(PUP->XftDP, &ED_XCArr[ED_White], ED_XFP, ED_PU_XMARGIN, LineY, (XftChar8 *)PUP->TitleStr, PUP->TitleLen);

    // Draw the bottom margin
    LineY = ED_PU_YMARGIN + ((PUP->RowCount - 1) * ED_Row);
    XftDrawRect(PUP->XftDP, &ED_XCArr[ED_TitleGray], LineX, LineY, SpanX, SpanY);
    XftDrawString8(PUP->XftDP, &ED_XCArr[ED_White], ED_XFP, LineX, LineY + ED_Ascent + (ED_PU_YMARGIN / 2),
		  (XftChar8 *)ED_STR_PUListSelect, strlen(ED_STR_PUListSelect));
}

// ******************************************************************************
// ED_PUListDrawEntryNumber actually draws the numbers on the left--one for each Entry.

void	ED_PUListDrawEntryNumber(ED_PUPointer PUP)
{
    Int32	I, LineY, StrLen;
    char	Str[32];
    Int32	NthLine = PUP->EntryRows / 2;		// Assume Odd number!

    LineY = ED_PU_YMARGIN + ED_Ascent + (ED_Row * (1 + NthLine));
    for (I = 1; I <= PUP->EntryCount; I++) {
	StrLen = sprintf(Str, "%d:", PUP->EntryTop + I);
	XftDrawString8(PUP->XftDP, &ED_XCArr[ED_White], ED_XFP, ED_PU_XMARGIN, LineY, (XftChar8 *)Str, StrLen);
	LineY += (ED_Row * PUP->EntryRows);
    }
}

// ******************************************************************************
// ED_PUDrawHScrollMarks will draw the H scroll marks for a given PU Entry--not
// just for PUList windows!
//
// If [L], draw a little mark on the left to indicate text beyond the edge.
// If [R], draw a little mark on the right to indicate text beyond the edge.
// If [M], then draw a mark on the bottom right to indicate MORE LINES are below
// the entry.

void	ED_PUDrawHScrollMarks(ED_PUPointer PUP, Int32 CurEntry, Int16 LeftCol, Int16 L, Int16 R, Int16 M)
{
    Int32		LineX, LineY, NthLine;
    Int16		Color;

    Color = (CurEntry == PUP->EntrySel) ? ED_SelOrange : ED_Gray;
    NthLine = PUP->EntryRows / 2;

    LineY = ((1 + ((CurEntry - PUP->EntryTop) * PUP->EntryRows) + NthLine) * ED_Row) + ED_PU_YMARGIN + ED_Ascent - 2;
    if (L) {
	LineX = (LeftCol * ED_Advance) + ED_PU_XMARGIN;
	XftDrawRect(PUP->XftDP, &ED_XCArr[Color], LineX, LineY - 4, ED_Advance - 1, 4);
    }

    if (R) {
	LineX = PUP->WinWidth - (ED_Advance + ED_PU_XMARGIN);
	XftDrawRect(PUP->XftDP, &ED_XCArr[Color], LineX, LineY - 4, ED_Advance - 1, 4);	
    }

    if (M) {
	LineY = (((CurEntry + 1 - PUP->EntryTop) * PUP->EntryRows) * ED_Row) + ED_PU_YMARGIN + 1;
	LineX = PUP->WinWidth + (ED_Advance / 2) - (ED_Advance + ED_PU_XMARGIN + 2);
	XftDrawRect(PUP->XftDP, &ED_XCArr[Color], LineX, LineY, 4, ED_Row - 1);
    }
    
}

// ******************************************************************************
// ED_PUDrawVScrollMarks will draw a little scroll mark on the top/bottom margin
// of the PU window to indicate the presence of more entries above/below the
// displayed area.

void	ED_PUDrawVScrollMarks(ED_PUPointer PUP, Int16 TopMark, Int16 BotMark)
{
    Int32		LineX, LineY;
    Int16		Color;

    // Top first
    Color = (TopMark) ? ED_SelOrange : ED_Gray;
    LineX = ED_PU_XMARGIN + (ED_Advance * (PUP->RowChars - 2));
    LineY = ED_Ascent - (ED_PU_YMARGIN / 2);
    XftDrawRect(PUP->XftDP, &ED_XCArr[Color], LineX, LineY - 2, ED_Advance, 4); 

    // Now bottom
    Color = (BotMark) ? ED_SelOrange : ED_Gray;
    LineY = ((PUP->RowCount - 1) * ED_Row) + ED_Ascent;
    XftDrawRect(PUP->XftDP, &ED_XCArr[Color], LineX, LineY - 2, ED_Advance, 4);
}

// ******************************************************************************
// ED_PUDrawEntries draws the column of entries in the PU window.  For each entry,
// it first clears the background, then dispatches to the caller-provided callback
// in PUP to actually draw the individual entry.
//
// The DrawFP callback operates within a ClipRect, allowing it to start drawing
// before the left edge (if scrolled) and continue past the right edge.  It returns
// DrawFlags that indicate what HScroll/More marks need be drawn.  The callback
// CANNOT draw the scroll marks itself, as they are placed outside the ClipRect.

void	ED_PUDrawEntries(ED_PUPointer PUP, Int32 LeftCol)
{
    Int32		I, LineX, LineY, SpanX, SpanY, CurEntry;
    Int16		DrawFlags, Color;
    XRectangle		ClipRect, UnclipRect;


    CurEntry = PUP->EntryTop;
    LineY = ED_PU_YMARGIN + ED_Row;
    LineX = ED_PU_XMARGIN + (ED_Advance * LeftCol);
    SpanX = PUP->WinWidth - (LineX + ED_PU_XMARGIN);
    SpanY = PUP->EntryRows * ED_Row;

    ClipRect.x = LineX + ED_Advance;
    ClipRect.width = SpanX - (ED_Advance * 2);
    ClipRect.y = LineY;
    ClipRect.height = (PUP->EntryCount * PUP->EntryRows) * ED_Row;

    UnclipRect.x = UnclipRect.y = 0;
    UnclipRect.width = UnclipRect.height = -1;

    for (I = 0; I < PUP->EntryCount; I++) {

	// Erase the entire background first, for all EntryRows at once.
	Color = (CurEntry == PUP->EntrySel) ? ED_White : ED_LightGray;
	XftDrawRect(PUP->XftDP, &ED_XCArr[Color], LineX, LineY + 1, SpanX, SpanY - 1);

	XftDrawSetClipRectangles(PUP->XftDP, 0, 0, &ClipRect, 1);
	    DrawFlags = (*PUP->DrawFP)(PUP, CurEntry, LineX + ED_Advance, PUP->RowChars - (LeftCol + 1), LineY);
	XftDrawSetClipRectangles(PUP->XftDP, 0, 0, &UnclipRect, 1);

	if (DrawFlags) ED_PUDrawHScrollMarks(PUP, CurEntry, LeftCol,
					     DrawFlags & ED_PU_DRAWLSCROLLFLAG,
					     DrawFlags & ED_PU_DRAWRSCROLLFLAG,
					     DrawFlags & ED_PU_DRAWMORELINESFLAG);
	CurEntry += 1;
	LineY += SpanY;
    }

}

// ******************************************************************************
// ED_PUListDraw is the main drawing function for PUList windows.

void	ED_PUListDraw(ED_PUPointer PUP)
{
    ED_PUListDrawNumberFrame(PUP, PUP->Config1);
    if (PUP->EntryCount < PUP->EntryTotal)
	ED_PUDrawVScrollMarks(PUP, (PUP->EntryTop > 0), (PUP->EntryTop + PUP->EntryCount) < PUP->EntryTotal);

    ED_PUListDrawEntryNumber(PUP);
    ED_PUDrawEntries(PUP, PUP->Config1);
}

// ******************************************************************************
// ED_PUListConfigure is the first of 2 functions necessary to create a new PUList
// window--a single function would be too long and require too many args.
//
// TitleP and TitleLen provide a title for the PUList.
// EntryTotal indicates how many entry slots should be drawn.
// EntryRows indicates how many rows are needed for each Entry slot.
// LeftColChars indicates how many charspaces are needed for the left number area.

void	ED_PUListConfigure(ED_PUPointer PUP, char * TitleP, Int32 TitleLen, Int32 EntryTotal, Int32 EntryRows, Int32 LeftColChars)
{
    Int32		W, H;
    Int16		XScreenN = DefaultScreen(ED_XDP);

    PUP->Flags = ED_PUNOFLAGS;
    PUP->Type = ED_PULISTTYPE;
    PUP->State = ED_PUNOSTATE;
    PUP->Config1 = LeftColChars;
    PUP->Config2 = 0;
    PUP->EntryTotal = EntryTotal;
    PUP->EntryTop = 0;
    PUP->EntrySel = 0;
    PUP->EntryRows = EntryRows;
    PUP->EntryCharMax = 0;
    PUP->EntryCharScroll = 0;

    memcpy(PUP->TitleStr, TitleP, TitleLen + 1);
    PUP->TitleLen = TitleLen;

    W = XDisplayWidth(ED_XDP, XScreenN) / (ED_Advance * 4);			// Chars
    ED_RANGELIMIT(W, ED_PU_MINCHARS, ED_PU_MAXCHARS);
    PUP->RowChars = W;
    PUP->WinWidth = (W * ED_Advance) + (ED_PU_XMARGIN * 2);

    // Get the # of entries, then convert to rows, add 1 row above, and 1 row below!
    H = XDisplayHeight(ED_XDP, XScreenN) / (ED_Row * 3);			// Rows
    ED_RANGELIMIT(H, ED_PU_MINROWS, ED_PU_MAXROWS);				// Rows
    PUP->EntryCount = H / EntryRows;						// Entries
    if (PUP->EntryCount > EntryTotal) PUP->EntryCount = EntryTotal;
    
    H = 2 + (PUP->EntryCount * EntryRows);					// Rows!!
    PUP->RowCount = H;
    PUP->WinHeight = (H * ED_Row) + (ED_PU_YMARGIN * 2);
}

// ******************************************************************************
// ED_PUListCreate is the second of 2 functions necessary for PUList creation.
//
// PUP is a pointer to the ED_PURecord that will hold all the state/data.
// PaneP is the selected/active Pane.
// ReturnFP is the callback function to call when an Entry is selected.
// DrawFP is the callback function for drawing each Entry.
// DataP is opaque client-provided data that for callbacks--if ncessary.
//
// Do not call if *PUP is already in use!

void	ED_PUListCreate(ED_PUPointer PUP, ED_PanePointer PaneP,
			ED_PUReturnFuncP ReturnFP, ED_PUDrawFuncP DrawFP, void * DataP)
{
    ED_FramePointer		FrameP = PaneP->FrameP;
    Int16			XScreenN = DefaultScreen(ED_XDP);
    Window			Child;
    Uns64			Mask;
    XSetWindowAttributes	AttrRec;
    Int32			X, Y;
    
    PUP->PaneP = PaneP;
    PUP->ReturnFP = ReturnFP;
    PUP->DrawFP = DrawFP;
    PUP->CLCmdFP = NULL;
    PUP->DataP = DataP;

    PUP->WinX = FrameP->PUWinX;
    PUP->WinY = FrameP->PUWinY;
    if ((PUP->WinX < 0) || (PUP->WinY < 0)) {
	XTranslateCoordinates(ED_XDP, FrameP->XWin, XDefaultRootWindow(ED_XDP), 0, 0, &X, &Y, &Child);
	PUP->WinX = X - (PUP->WinWidth / 2);
	if (PUP->WinX < 0) PUP->WinX = 10;
	PUP->WinY = Y - (PUP->WinHeight / 4);
	if (PUP->WinY < 0) PUP->WinY = 10;
    }
    
    Mask = CWCursor | CWEventMask | CWSaveUnder | CWOverrideRedirect | CWBackPixel;
    AttrRec.cursor = ED_ArrowCursor;
    AttrRec.save_under = 1;
    AttrRec.override_redirect = 1;
    AttrRec.event_mask =  ExposureMask | ButtonPressMask | KeyPressMask | Button1MotionMask | FocusChangeMask;
    AttrRec.background_pixel = WhitePixel(ED_XDP, XScreenN);

    PUP->XWin = XCreateWindow(ED_XDP, XDefaultRootWindow(ED_XDP),
			      PUP->WinX, PUP->WinY, PUP->WinWidth, PUP->WinHeight, 0, CopyFromParent, 
			      InputOutput, DefaultVisual(ED_XDP, XScreenN), Mask, &AttrRec);
    PUP->XftDP = XftDrawCreate(ED_XDP, PUP->XWin, DefaultVisual(ED_XDP, XScreenN), DefaultColormap(ED_XDP, XScreenN));
    if (! PUP->XftDP) G_SETEXCEPTION("PUP XftDrawCreate failed", 0);

    sc_WERegAdd(PUP->XWin, &ED_PUListEventHandler, PUP);
    XMapWindow(ED_XDP, PUP->XWin);
    
    XSetInputFocus(ED_XDP, PUP->XWin, RevertToNone, CurrentTime);
    XFlush(ED_XDP);
    
}

// ******************************************************************************
// ED_PUListDestroy gets rid of the PUList and its window.
//
// NOTE:	It calls the ExitFP function, if there is one.
//		PUP->XWin will be gone, but everything else is still there!

void	ED_PUListDestroy(ED_PUPointer PUP)
{
    // Unregister the handler from the Main loop!
    sc_WERegDel(PUP->XWin);

    XftDrawDestroy(PUP->XftDP);
    XDestroyWindow(ED_XDP, PUP->XWin);
    PUP->XWin = 0;

    if (PUP->CLCmdFP == NULL)		// Null unless we have a command to execute!
	PUP->PaneP = NULL;
}

// ******************************************************************************
// ******************************************************************************

#ifdef DEBUG
    void	TEST_StuffText(void)
    {
	Int32		I;
	char		String1[255];
	ED_FramePointer	FP;
	ED_BufferPointer	BP;
	Int32		Pos, StartingPos;
	char		HEllipses[] = {0xE2, 0x80, 0xA6, 0x00};


	FP = ED_CurFrameP;
	BP = FP->CurPaneP->BufP;
	Pos = StartingPos = FP->CurPaneP->CursorPos;	// Save

	sprintf(String1, "This%sis%sa%stest%s.", HEllipses, HEllipses, HEllipses, HEllipses);
	Pos = ED_BufferInsertLine(BP, Pos, String1);

	sprintf(String1, "A%sLineHeight: %d, CharHeight: %d, CharDescent: %d, CharAdvance: %d, Window: %lo",
		HEllipses, ED_Height, ED_Ascent, ED_Descent, ED_Advance, FP->XWin);
	Pos = ED_BufferInsertLine(BP, Pos, String1);
	String1[0] = 'X';
	Pos = ED_BufferInsertLine(BP, Pos, String1);
	String1[0] = 'Y';
	Pos = ED_BufferInsertLine(BP, Pos, String1);
	String1[0] = 'Z';
	Pos = ED_BufferInsertLine(BP, Pos, String1);

	{
	    ED_FRegPointer	FRP;
	    FRP = ED_FirstFRegP;
	    while (FRP) {
		Pos = ED_BufferInsertLine(BP, Pos, FRP->NameP);
		FRP = FRP->SortNextP;
	    }
	}

	{
	    ED_FBindPointer	FBP;
	    char		KeyNames[ED_MSGSTRLEN];

	    FBP = ED_FirstBindP;
	    while (FBP) {
		ED_KeyStrPrint(FBP->KeyStr, KeyNames);

		sprintf(String1, "Binding: %s --> Function: %s", KeyNames, FBP->FRegP->NameP);
		Pos = ED_BufferInsertLine(BP, Pos, String1);	
		FBP = FBP->SortNextP;
	    }
	}

	sprintf(String1, "0xff50 -> %s 0xff80 -> %s 0xff07 -> %s 0xff0b -> %s",
		XKeysymToString(0xff50), XKeysymToString(0xff80), XKeysymToString(0xff07), XKeysymToString(0xff0b));
	Pos = ED_BufferInsertLine(BP, Pos, String1);

	sprintf(String1, "Frame: %ld, Pane: %ld, Buffer: %ld", sizeof(ED_FrameRecord), sizeof(ED_PaneRecord), sizeof(ED_BufferRecord));
	Pos = ED_BufferInsertLine(BP, Pos, String1);

	{
	    char**		FontListP;
	    Int32		FontListCount;
	    I = FP->RowCount;
	    FontListP = XListFonts(ED_XDP, "*fixed*", I, &FontListCount);
	    for (I = 0; I < FontListCount; I++)
		Pos = ED_BufferInsertLine(BP, Pos, FontListP[I]);
	    XFreeFontNames(FontListP);
	}

	FP->CurPaneP->CursorPos = StartingPos;	// Restore
	ED_FrameUpdateTextRows(FP, FP->RowChars);
	ED_PaneSetScrollBar(FP->CurPaneP);

	// Testing PaneFindAbsLoc... used during initial debugging.
	// But a good template for other tests, as needed.
	if (0) {
	    Int32		Count;
	    Int32		Pos, Row, Col, MaxPos;
	    Int32		StartPos1, StartPos2;
	    Int32		TestPos;

	    Count = 500000;
	    MaxPos = FP->CurPaneP->BufP->LastPos;
	    while (Count--) {
		Pos = (Int32)(((Int64)rand() * MaxPos) / (Int64)RAND_MAX);

		StartPos1 = ED_PaneFindLoc(FP->CurPaneP, Pos, &Row, &Col, 0, 0);
		// printf("AbsLoc Pos:%d  --> Row:%d  Col:%d\n", Pos, Row, Col);
		StartPos2 = ED_PaneFindPos(FP->CurPaneP, &TestPos, &Row, &Col, 0, 0);
		// printf("AbsPos -> Pos:%d  <-- Row:%d  Col:%d\n", TestPos, Row, Col);

		if (StartPos1 != StartPos2) {
		    printf("Test Mismatch!  POS=%d	--> Pos1:%d  Pos2:%d\n", Pos, StartPos1, StartPos2);
		    break;
		} else if (TestPos != Pos) {
		    printf("Test Mismatch!  Loc-Pos: %d --> Pos: %d\n", Pos, TestPos);
		    break;
		} else
		    if (Pos < 10) printf("[%d] ", Pos);
	    }
	    printf("Done \n");
	}
	
    }
#endif

// ******************************************************************************
// ******************************************************************************
// Undo
//
// Each Buffer has a doubly-linked list of UndoSlabs (malloc/free) hanging off
// of it.  The number (and total size) is managed by a GC function that can
// free the oldest slabs to make room.  Each UndoSlab contains an array of
// UndoBlocks, each of which records a Buffer operation.  As the user Edits,
// commands that alter the Buffer (add/delete) call ED_BufferAddUdoBlock to
// record their operation...  At some point, the user will call the UndoCmd
// which proceeds to reverse-traverse the UndoBlocks to undo individual operations.
// (But the Undo operation itself alters the Buffer, so will record UBlocks of its
// own that can be Undone...).
//
// There are two operations on Buffers, ADD that places text in them and DEL
// that removes text.  The former simply records the location and length of
// added data (as the text is *IN* the Buffer).  The latter must also record
// the data, as it has been removed!  (While the KillRing only records killed data,
// the UndoBuffer record all kills and deletions.)
//
// The Undo machinery endeavors to faithfully reproduce the MOD or UnMOD state
// of the Buffer.  Therefore, it uses a SAVE block (a simple ADD block with an extra
// flag) to record when the Buffer is saved (also written or simply UnMod with
// the M-~ command).  Saving changes the 'origin' state of the Buffer as it
// impacts the MOD or UnMod state--UnModified is defined as the last Saved/origin
// state... not how the Buffer looked when it was first opened.
//
// UBlocks are WRITTEN during normal editing, as each command records it action.
// UBlocks are READ when the Undo cmd is invoked, as each undoes a previous edit.
//
// NOTE:	RealEmacs allows numeric args for Undo, and appears to gang/chain
//		them together (for the purpose of Undoing-the-undo).  This
//		implementation does not accept numeric args for the Undo cmd.

#define			ED_UNDO_DATASIZE	35	// Good size for an Undo block

Int32			ED_UndoBlock;			// Read for Undoing...0 means start at head
ED_USlabPointer		ED_UndoSlabP;			// Read for Undoing...NULL means Slab purged or start at head
Int16			ED_UndoSeenSave;		// Read for Undoing...Seen a SAVE UBlock


Int16	ED_BufferAddNewUndoSlab(ED_BufferPointer BufP, Int32 Payload);
Int32	ED_BufferAllocUndoBlock(ED_BufferPointer BufP, Int32 DataLen, ED_UBlockPointer *UBlockPP);


// Initializes Undo memory for BufP.
// USlab is really not that essential, but no point running if cannot
// even allocate an empty USlab.
void	ED_BufferInitUndo(ED_BufferPointer BufP)
{

    BufP->FirstUSP = BufP->LastUSP = NULL;
    BufP->USCount = 0;
    BufP->USTotalSize = 0;
    if (! ED_BufferAddNewUndoSlab(BufP, 0))
	G_SETEXCEPTION("Malloc Undo Memory Failed", 0);
}

// Disposes of Undo memory for BufP... generally called before
// getting rid of the Buffer itself.  But can be used to simply
// turn off Undo support for a Buffer.
void	ED_BufferKillUndo(ED_BufferPointer BufP)
{
    ED_USlabPointer	USP, NextUSP;

    USP = BufP->FirstUSP;
    while (USP) {
	NextUSP = USP->NextUSP;
	free(USP);
	USP = NextUSP;
    }

    BufP->FirstUSP = BufP->LastUSP = NULL;
    BufP->USCount = 0;
    BufP->USTotalSize = 0;
}

// Creates an UndoSlab large enough to handle Payload Bytes.  Attaches it
// at the Last end of the doubly linked USlab chain--latest in time.
Int16	ED_BufferAddNewUndoSlab(ED_BufferPointer BufP, Int32 Payload)
{
    ED_USlabPointer	USP;
    Int32		Size;

    if (Payload == 0)
	Size = ED_UNDOINITLEN;
    else {
	Size = Payload + ED_UNDOEXTRALEN;
	if (Size < ED_UNDOINITLEN) Size = ED_UNDOINITLEN;
    }

    USP = malloc(Size);
    if (! USP) return 0;
    
    USP->Tag = *(Uns32 *)ED_UndoTag;
    USP->Flags = ED_US_NOFLAG;
    USP->SlabSize = Size;
    USP->LastUBlock = 0;			// No blocks for now!

    USP->NextUSP = NULL;
    if (BufP->FirstUSP) {
	USP->PrevUSP = BufP->LastUSP;
	BufP->LastUSP->NextUSP = USP;
	BufP->LastUSP = USP;
    } else {
	BufP->FirstUSP = USP;
	BufP->LastUSP = USP;
	USP->PrevUSP = NULL;    
    }
    BufP->USCount += 1;
    BufP->USTotalSize += Size;
    return 1;
}

// Removes Count USlabs from the Buffer... starting from the oldest,
// BufP->FirstUSP side of the chain.  Used to GC memory.
void	ED_BufferKillUndoSlabs(ED_BufferPointer BufP, Int16 Count)
{
    ED_USlabPointer	USP, NextUSP;

    USP = BufP->FirstUSP;
    while (USP) {
	NextUSP = USP->NextUSP;
	BufP->FirstUSP = NextUSP;
	if (BufP->LastUSP == USP) BufP->LastUSP = NULL;
	if (NextUSP) NextUSP->PrevUSP = NULL;
	
	if (ED_UndoSlabP == USP) {
	    ED_UndoSlabP = NULL;
	    ED_UndoBlock = 0;
	}

	BufP->USCount -= 1;
	BufP->USTotalSize -= USP->SlabSize;

	free(USP);
	USP = NextUSP;
	if (--Count <= 0) break;
    }
}

// GarbageCollects (GC) the Undo memory--keeps it from getting too big.
// There are 4 GC levels:
// Level 0 --> Check against limits
// Level 1 --> Clean up a little
// Level 2 --> Clean up a LOT
// Level 3 --> Clean up everything
void	ED_BufferGCUndoSlabs(ED_BufferPointer BufP, Int16 Level)
{
      switch (Level) {
	case 0:
	    // Does not look at Bytes, just number of Slabs.
	    if (BufP->USCount > ED_UNDOGCSLABCOUNT)
		ED_BufferKillUndoSlabs(BufP, BufP->USCount - ED_UNDOGCL0SLABCOUNT);
	    break;

	case 1:
	    // More stringent than Level 0.
	    if (BufP->USCount > ED_UNDOGCL1SLABCOUNT) {
		ED_BufferKillUndoSlabs(BufP, BufP->USCount - ED_UNDOGCL1SLABCOUNT);
		ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoUndoMemFreed);
	    }
	    break;

        case 2:
	    // Severly curtails Undo memory... tries to leave with 1 Slab.
	    while (BufP->USTotalSize > ED_UNDOGCL2MEMMAX) {
		ED_BufferKillUndoSlabs(BufP, 1);
		ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoUndoMemFreed);		
	    }
	    if (BufP->USCount == 0) {
		if (! ED_BufferAddNewUndoSlab(BufP, 0))
		    ED_FrameSetEchoError(ED_STR_EchoUndoLost, errno);
	    }
	    break;


	default:
	    // Just turn it off... severe memory shortage.
	    ED_BufferKillUndo(BufP);
	    ED_FrameSetEchoError(ED_STR_EchoUndoLost, errno);
      }
}

// Allocates a new UndoBlock to hold DataLen bytes, will create a new USlab if there is not enough room.
// The new Slab will be LARGE ENOUGH to hold at least this UBlock.
//
// NOTE:	Block size is always multiple of 4--aligned.
//		Size of a Block is ((SizeOfRecord - 3 + DataLen) + 3) & ~0x0003 (round to multiple of 4)
//		But the -3 and +3 cancel out, so are left out.
//
// NOTE:	ADD blocks have no Data, so are always allocated on 1 USlab.  But DEL blocks
//		have Data... and long ones may not fit.  So it allocates as much as will fit,
//		on THIS USlab, then creates another USlab and places the remainder in a CHAINed
//		UBlock on the new USlab.
Int32	ED_BufferAllocUndoBlock(ED_BufferPointer BufP, Int32 DataLen, ED_UBlockPointer *UBlockPP)
{
    ED_USlabPointer		LastUSP = BufP->LastUSP;
    ED_UBlockPointer		UBP = NULL;
    Int32			MaxData, Size, NewOffset;

    do {
	// If the LastUSP is empty, create the first UBlock.
	if (LastUSP->LastUBlock == 0) {
	    LastUSP->LastUBlock = sizeof(ED_USlabRecord);
	    UBP = (ED_UBlockPointer)(LastUSP + 1);
	    UBP->PrevUBlock = UBP->DataPos = UBP->DataLen = -1;
	    UBP->Flags = 0;
	    MaxData = (LastUSP->SlabSize - LastUSP->LastUBlock) - offsetof(ED_UBlockRecord, Data);
	    *UBlockPP = UBP;
	    return MaxData;
	}
	
	// Get Last UBP and see its size.
	UBP = (ED_UBlockPointer)((char *)LastUSP + LastUSP->LastUBlock);
	Size = sizeof (ED_UBlockRecord);
	if (UBP->Flags & ED_UB_DEL)				// It has data!
	    Size = (Size + UBP->DataLen) & ~0x00000003;		// Round to 32-bit boundary

	// Room for a new block here?
	NewOffset = LastUSP->LastUBlock + Size;
	MaxData = (LastUSP->SlabSize - NewOffset) - offsetof(ED_UBlockRecord, Data);

	// Happy to make *a* block here if have at least ED_UNDO_DATASIZE bytes for data.
	// Anything extra, allocate another Slab and CHAIN the next UBlock to this one.
	if ((MaxData >= DataLen) || (MaxData >= ED_UNDO_DATASIZE)) {
	    LastUSP->LastUBlock = NewOffset;
	    UBP = (ED_UBlockPointer)((char *)LastUSP + NewOffset);
	    UBP->PrevUBlock = UBP->DataPos = UBP->DataLen = -1;
	    UBP->Flags = 0;
	    
	    *UBlockPP = UBP;
	    return MaxData;
	}

	// Not enough room for another block, allocate a new slab, make it big enough!
	if (! ED_BufferAddNewUndoSlab(BufP, DataLen)) {
	    ED_BufferGCUndoSlabs(BufP, 1);			// Level 1: Clean a little
	    if (! ED_BufferAddNewUndoSlab(BufP, DataLen)) {
		ED_BufferGCUndoSlabs(BufP, 2);			// Level 2: Clean a *LOT*
		if (! ED_BufferAddNewUndoSlab(BufP, DataLen)) {
		    ED_BufferGCUndoSlabs(BufP, 3);		// Level 3: Clean all, admit failure
		    return -1;
		}
	    }
	}

	// Assuming a new Slab was allocated, loop back and try a new block
	LastUSP = BufP->LastUSP;

    } while (1);

    return -1;							// Should never get here!
}

// Main routine for adding Undo blocks--called by all Cmds that modify the Buffer.
// Mode == ADD or DEL, but *NEVER* both!
//
// Normal type/delete alterations are grouped together in ED_UNDO_DATASIZE
// chunks to cut back on number of UBlocks.  But discrete yank/kill chunks are
// treated as standalone--Mode has ED_UB_CHUNK flag set.
//
// If the action is a replacement (e.g. YankPop or Capitalize), then there
// will be a DEL followed by a CHAINed ADD.  (DEL is *ALWAYS* first).  Once
// CHAINed, these are undone with a single command--better user experience.
//
// NOTE:	The UBlocks are READ in reverse order by the Undo command itself.
//		So where there is a DEL-ADD chain, the ADD is read first.  But
//		Undo will ITSELF record UBlocks (so it can in turn be undone).
//		And the opposite of ADD is DEL (and vice versa).  So even when
//		the DEL-ADD chain is read, it still creats another DEL-ADD
//		chain.
//
// NOTE:	The first in a chained series DOES NOT have CHAIN flag.
//		Only subsequent requests have it.  Once stored, the
//		initial DEL may end up being split in two (if too big to fit).
//
//		Request			Stored as
//		-------------------	---------------------------------
//		(DEL)			(DEL) or (if too big to fit in 1 Slab)
//					(DEL) + (DEL+CHAIN)
//
//		(DEL) + (ADD+CHAIN)	(DEL) + (ADD+CHAIN) or (if too big)
//					(DEL) + (DEL+CHAIN) + (ADD+CHAIN)
//					
//		(ADD)			(ADD)
//
// Just a DEL on its own can create two chained blocks if the data is long
// and will not fit in the remainder of the current UndoSlab.  When this pair of
// Undo blocks are *itself* Undone, they will call for TWO ADD blocks.  But these two
// are *ALWAYS* merged (easy since they have no Data) so when they are undone, they
// don't lead to even more DEL blocks... on and on.  Since the ADD blocks are merged,
// undoing them will *NEVER* request chained DEL blocks--although DEL blocks may end up
// split and chained if they are long.  No other (normal) editor command will ever
// explicitly ask for chained DEL blocks.  Therefore, there is no need to force merge
// chained DEL blocks like the ADD blocks.
//
// Generally, a few chars (about 30) are merged, to cut down on the number of UBlocks
// and their overhead memory.  Too many chars are not merged, as they would all have
// to be undone in 1 block. 
//
// When the Buf is saved, a new UBlock (SAVE) is added as a place holder, since saving
// changes the base "notion" of what the unmodified buffer looks like.  After a SAVE
// anything that restores the buffer to previous states (even its very original one)
// will still render the Buf as MOD.  (The explicit UNMODIFY command is regarded as a SAVE.)
void	ED_BufferAddUndoBlock(ED_BufferPointer BufP, Int32 Pos, Int32 Len, Uns8 Mode, char * DataP)
{
    ED_USlabPointer		LastUSP = BufP->LastUSP;
    ED_UBlockPointer		UBP = NULL;
    Int16			FirstMod, DoDel, DoSave, Count;
    Int32			PrevOffset, LenLeft, MaxLen;

    // BufP is altered... excellent place for ED_XSelAlterPrimary, in case BufP is PRIMARY!
    // This properly handles the case when BufP is altered by using the Undo cmd itself!!
    // Functionality is available even in ReadOnly buffers or when Undo is turned off.
    if (Mode & ED_UB_DEL)
	ED_XSelAlterPrimary(BufP, Pos, -Len);
    else if (Mode & ED_UB_ADD)
	ED_XSelAlterPrimary(BufP, Pos, Len);

    if (LastUSP == NULL) return;			// Undo is turned off!
    ED_BufferGCUndoSlabs(BufP, 0);			// Level 0, limits USlab count

    DoDel = (Mode & ED_UB_DEL);
    DoSave = (Mode & ED_UB_SAVE);

    // If FirstMod, reset the buf flag and DO NOT try to merge!!
    FirstMod = 0;
    if (BufP->Flags & ED_BUFCLEANUNDOFLAG) {
	BufP->Flags &= ~ ED_BUFCLEANUNDOFLAG;
	FirstMod = 1;
    } else if (LastUSP->LastUBlock) {
	UBP = (ED_UBlockPointer)((char *)LastUSP + LastUSP->LastUBlock);

	if (DoDel) {
	    if (!(Mode & ED_UB_CHUNK) &&
		(UBP->Flags & ED_UB_DEL) &&				// Prev is Del
		!(UBP->Flags & (ED_UB_CHUNK | ED_UB_CHAIN)) &&		// Prev is NOT Chunk or Chained	    
		(UBP->DataLen + Len <= ED_UNDO_DATASIZE) &&		// Prev has room
		((Int64)UBP->Data + UBP->DataLen + Len <= (Int64)LastUSP + LastUSP->SlabSize)) {	// Fits Slab!	

		// Two ways to put them together... depends on which direction the deletes were.
		// Can only Add in one direction, but can DEL in two!!
		if (UBP->DataPos == Pos) {				// Prev has contiguous Pos (Del != Add!!)
		    memcpy(&UBP->Data[UBP->DataLen], DataP, Len);	// Add in Data, after sequence
		    UBP->DataLen += Len;
		    return;
		} else if (UBP->DataPos - Len == Pos) {
		    memmove(&UBP->Data[Len], UBP->Data, UBP->DataLen);
		    memcpy(UBP->Data, DataP, Len);
		    UBP->DataLen += Len;
		    UBP->DataPos = Pos;
		    return;
		}
	    }	
	} else { // Do Add !!
	    if (UBP->Flags & ED_UB_ADD) {				// Previous was ADD too!
		// Very possible that a large (multi-part) CHAIN DEL is being UnDone.  This will request
		// CHAINed ADD blocks (as the original DEL blocks are processed one at a time... These
		// ADD blocks are force-merged.
		if (Mode & ED_UB_CHAIN) {
		    UBP->DataLen += Len;				// Simply add to the count
		    return;
		}

		// Normal ADDs (as in typing) are merged in small amounts.
		if ((UBP->DataPos >= 0) &&				// ADD + DataPos == -1 is reserved for Info!
		    !(UBP->Flags & (ED_UB_CHUNK | ED_UB_CHAIN)) &&	// Prev is NOT Chunk or Chained
		    (UBP->DataLen + Len <= ED_UNDO_DATASIZE) &&		// Prev has room (Not to Undo TOO much)
		    (UBP->DataPos + UBP->DataLen == Pos)) {		// Prev has contiguous Pos

		    UBP->DataLen += Len;				// No data, just add in the Len.
		    return;
		}
	    }
	}
    }

    // No more merging if we get here.  Just add new blocks.
    // Del always comes first, so no point checking for Chain!
    // If the first block is too small (used up all Cur Slab), next iteration
    // will alloc a Slab that is big enough.  So at most, 2 blocks will contain it.
    //
    // WARNING:	If ED_BufferAllocUndoBlock with a DataLen == 0, DO NOT access Data[0]...
    //		as these will NOT be allocated!

    PrevOffset = LastUSP->LastUBlock;
    if (DoSave) {
	MaxLen = ED_BufferAllocUndoBlock(BufP, 0, &UBP);
	if (MaxLen < 0) return;

	UBP->PrevUBlock = PrevOffset;
	UBP->DataPos = -1;
	UBP->DataLen = 0;
	UBP->Flags = ED_UB_ADD | ED_UB_SAVE;
	// Reset ED_BUFCLEANUNDOFLAG for the NEXT real UBlock!!
	// This one is just a do-nothing placeholder!
	if (FirstMod)
	    BufP->Flags |= ED_BUFCLEANUNDOFLAG;

    } else if (DoDel) {
	LenLeft = Len;
	Count = 0;
	do {
	    // Alloc a block--*MAY* allocate a new Slab, so new LastUSP !!
	    MaxLen = ED_BufferAllocUndoBlock(BufP, LenLeft, &UBP);
	    if (MaxLen < 0) return;				// Memory error
	    
	    UBP->PrevUBlock = PrevOffset;			// Chain blocks--could be to previous USP!
	    UBP->DataPos = Pos;					// Buffer Pos
	    if (MaxLen >= LenLeft) {				// If all fits here?
		UBP->DataLen = LenLeft;
		LenLeft = 0;
	    } else {						// Will loop to allocate a new Slab BIG ENOUGH
		UBP->DataLen = MaxLen;				// Get what fits here
		LenLeft -= MaxLen;
	    }
	    
	    memcpy(UBP->Data, DataP, UBP->DataLen);		// Get the data
	    DataP += UBP->DataLen;				// For next iteration!

	    UBP->Flags = Mode & (ED_UB_CHUNK | ED_UB_DEL);
	    if (Count == 0) {					// First in Chain
		if (FirstMod) UBP->Flags |= ED_UB_FIRSTMOD;	// This is the FirstMod, buffer was clean before this!!
	    } else
		UBP->Flags |= ED_UB_CHAIN;			// In case we looped!
	    
	    Count += 1;
	    PrevOffset = (Int64)UBP - (Int64)BufP->LastUSP;	// Offset of THIS block, for next iteration
	    
	} while (LenLeft);
	
    } else {  // Not DEL, so must be ADD
	MaxLen = ED_BufferAllocUndoBlock(BufP, 0, &UBP);	// May be a new USP
	if (MaxLen < 0) return;					// Memory error
	
	UBP->PrevUBlock = PrevOffset;
	UBP->DataPos = Pos;
	UBP->DataLen = Len;
	UBP->Flags = Mode & (ED_UB_CHUNK | ED_UB_CHAIN | ED_UB_ADD);
	if (FirstMod) UBP->Flags |= ED_UB_FIRSTMOD;
    }
}


// Helper function for UndoCmd.  Reads the Undo memory (in reverse).
// Return 1 --> Success
// Return 0 --> Fail
//
// IMPERATIVE:	Always Undo the cmd first, *THEN* call ED_BufferAddUndoBlock *FOR* the undoing...
//		the latter can cause the Slab for the former to purge!
//
// NOTE:	Must call again if ED_UB_CHAIN
Int16	ED_BufferUndo(ED_BufferPointer BufP, Uns8 * OpP, Int32 *PosP, Int32 *LenP, char **DataPP)
{
    ED_UBlockPointer		UBP;
    
    if (BufP->LastUSP == NULL) {
	ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBufNoUndo);
	if (ED_CmdLastId == ED_UndoId) ED_FrameFlashError(ED_CurFrameP);
	return 0;		// No Undo for this Buffer
    }

    if (ED_UndoBlock) {
	if (ED_UndoSlabP == NULL) goto UndoNoMore;		// Cannot Undo any more!
	
	// Undoing in sequence, so take 1 step further back!
	UBP = (ED_UBlockPointer)((char *)ED_UndoSlabP + ED_UndoBlock);
	ED_UndoBlock = UBP->PrevUBlock;
	if (ED_UndoBlock == 0) goto UndoNoMore;			// Very end of Undo history!

	// If UBP is the first block on the Slab, then we must go to previous Slab!
	if ((char *)UBP == (char *)(ED_UndoSlabP + 1)) {
	    ED_UndoSlabP = ED_UndoSlabP->PrevUSP;
	    if (ED_UndoSlabP == NULL) goto UndoNoMore;		// Last Slab is gone!
	}
	goto DoUndo;

    } else {
	// Possible that we got here, because we ran out of Undo
	// history, and want to Undo some more!
	if (ED_CmdLastId == ED_UndoId) {
	    ED_FrameFlashError(ED_CurFrameP);
	    goto UndoNoMore;
	}
    
	// Very first Undo!
	ED_UndoSlabP = BufP->LastUSP;
	ED_UndoBlock = BufP->LastUSP->LastUBlock;
	if (ED_UndoBlock == 0) goto UndoNoMore;
	goto DoUndo;
    }

DoUndo:
    UBP = (ED_UBlockPointer)((char *)ED_UndoSlabP + ED_UndoBlock);
    *PosP = UBP->DataPos;
    *LenP = UBP->DataLen;
    *DataPP = UBP->Data;
    *OpP = UBP->Flags & (ED_UB_CHAIN | ED_UB_ADD | ED_UB_DEL | ED_UB_FIRSTMOD | ED_UB_SAVE);
    return 1;

UndoNoMore:
    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoUndoNoMore);
    return 0;
}

// Main command.  If LastCmd was Undo, traverse farther back in time.
// Otherwise, start by undoing the last command.  If multiple blocks
// are chained, then the whole chain is processed by 1 invocation.
void	ED_CmdUndo(ED_PanePointer PaneP)
{
    ED_BufferPointer	BufP = PaneP->BufP;
    Int32		DataPos, DataLen, Total;
    char *		DataP;
    Int16		DidChain;
    Uns8		Op, NewOp;

    if (ED_BufferReadOnly(BufP)) return;

    // Reset the Undo 'read' head to the end of the sequence!
    if (ED_CmdLastId != ED_UndoId) {
	ED_UndoSlabP = NULL;
	ED_UndoBlock = 0;
	ED_UndoSeenSave = 0;
    }
    
    DidChain = 0;
    Total = 0;
    
GetAnother:
    // Get the previous UBlock data.
    if (! ED_BufferUndo(BufP, &Op, &DataPos, &DataLen, &DataP)) goto DoneDone;
    
    if (Op & ED_UB_ADD) {
	// an ADD with DataPos == -1 is used as a marker--SAVE, etc.
	if (DataPos == -1) {
	    if (Op & ED_UB_SAVE) ED_UndoSeenSave = 1;
	    // Now get another, the user does not see the SAVE and expects
	    // something to be undone.
	    goto GetAnother;
	}
    
	// DataLen bytes were added at DataPos.  So delete them now!
	NewOp = (DidChain) ? ED_UB_DEL | ED_UB_CHAIN : ED_UB_DEL;
	ED_BufferUpdateMark(BufP, DataPos, - DataLen);
	ED_BufferPlaceGap(BufP, DataPos, 0);
		    
	// Adding a new UBlock can purge the USlab we read from, but we
	// already have all the fields, and do not need DataP.
	ED_BufferAddUndoBlock(BufP, DataPos, DataLen, NewOp, BufP->GapEndP);
	BufP->GapEndP += DataLen;
	BufP->LastPos -= DataLen;
	Total -= DataLen;

	// Reset flags AFTER creating a new Undo block!
	if ((ED_UndoSeenSave == 0) && (Op & ED_UB_FIRSTMOD)) {
	    BufP->Flags |= ED_BUFCLEANUNDOFLAG;
	    BufP->Flags &= ~ED_BUFMODFLAG;
	} else
	    BufP->Flags |= ED_BUFMODFLAG;	
	
#ifdef DEBUG
    // TEST_FillGap(BufP);
#endif

    } else if (Op & ED_UB_DEL) {
	// Datalen bytes were deleted at DataPos.  So add them now!
	NewOp = (DidChain) ? ED_UB_ADD | ED_UB_CHAIN : ED_UB_ADD;
	ED_BufferUpdateMark(BufP, DataPos, DataLen);
	ED_BufferPlaceGap(BufP, DataPos, DataLen);
	memcpy(BufP->GapStartP, DataP, DataLen);
	BufP->GapStartP += DataLen;
	BufP->LastPos += DataLen;
	Total += DataLen;

	// Adding a UBlock can purge the slab we were reading from,
	// but it is OK, as have already copied the DataP.
	ED_BufferAddUndoBlock(BufP, DataPos, DataLen, NewOp, NULL);

	// Reset flags AFTER creating a new Undo block!
	if ((ED_UndoSeenSave == 0) && (Op & ED_UB_FIRSTMOD)) {
	    BufP->Flags |= ED_BUFCLEANUNDOFLAG;
	    BufP->Flags &= ~ED_BUFMODFLAG;
	} else
	    BufP->Flags |= ED_BUFMODFLAG;
    }

    DidChain = 1;
    if (Op & ED_UB_CHAIN) goto GetAnother;

    PaneP->CursorPos = DataPos;
    ED_PaneUpdateAllPos(PaneP, 0);
    ED_PaneUpdateOtherPanes(PaneP, DataPos, Total);
	
DoneDone:
    ED_FrameDrawAll(PaneP->FrameP);
    ED_CmdThisId = ED_UndoId;
}

// Resets Undo memory... complete wipe of the old.
// Starts with a single new USlab.
void	ED_CmdResetUndo(ED_PanePointer PaneP)
{
    if (ED_BufferReadOnly(PaneP->BufP)) return;
    
    ED_BufferKillUndo(PaneP->BufP);	// Kill the old
    ED_BufferInitUndo(PaneP->BufP);	// Create 1 new USlab.

    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoUndoReset);    
}

// Gets rid of Undo memory completely.  Reset to get back!
void	ED_CmdDisableUndo(ED_PanePointer PaneP)
{
    if (ED_BufferReadOnly(PaneP->BufP)) return;
    
    ED_BufferKillUndo(PaneP->BufP);
    ED_FrameSetEchoS(ED_ECHOMSGMODE, ED_STR_EchoBufNoUndo);        
}


#ifdef DEBUG
    // Prints out the Undo memory... debugging aid.
    // Function is currently attached to [C-U C-x =].
    void	TEST_PrintUndoMemory(ED_BufferPointer BufP)
    {
	ED_USlabPointer		USP;
	ED_UBlockPointer	UBP;
	Int16			Count;
	Int32			Offset, Size;
	char			AddOp[] = "Add";
	char			DelOp[] = "Del";
	char			SaveMark[] = "Save!\n";
	char			EndMark[] = "\n";
	char *			OpP;
	char *			EndP;

	printf("Buffer Undo Memory  Slabs:%d  Bytes:%d\n", BufP->USCount, BufP->USTotalSize);

	USP = BufP->FirstUSP;
	Count = 1;
	while (USP) {
	    printf("    Slab: %d     %d Bytes\n", Count, USP->SlabSize);
	    if (USP->LastUBlock == 0)
		printf("        No Blocks\n");
	    else {
		Offset = sizeof(ED_USlabRecord);
		UBP = (ED_UBlockPointer)(USP + 1);
		while (UBP) {
		    OpP = (UBP->Flags & ED_UB_DEL) ? DelOp : AddOp;
		    Size = (UBP->Flags & ED_UB_DEL) ? UBP->DataLen : 0;
		    if (Size > 50) Size = 50;
		    EndP = (UBP->Flags & ED_UB_SAVE) ? SaveMark : EndMark;
		    printf("        Block->%4d %s:0x%02x   Pos:%d Len:%d:[%.*s]%s",
			    Offset, OpP, UBP->Flags, UBP->DataPos, UBP->DataLen, Size, UBP->Data, EndP);

		    Size = sizeof(ED_UBlockRecord);
		    if (UBP->Flags & ED_UB_DEL)
			Size = (Size + UBP->DataLen) & ~0x00000003;

		    Offset += Size;
		    if (Offset > USP->LastUBlock) break; 

		    UBP = (ED_UBlockPointer)((char *)USP + Offset);
		}
	    }

	    USP = USP->NextUSP;
	    Count += 1;
	    printf("\n");
	}
	printf("\n");	
    }
#endif

