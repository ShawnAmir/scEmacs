scEmacs
        ... simple C Emacs
        ... small component Emacs

Copyright (C) 2018 Shawn Amir
All Rights Reserved
___________________________________________________________________________

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 3 or later of the License.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

THIS SOFTWARE IS PROVIDED ``AS IS'' WITHOUT ANY EXPRESS OR IMPLIED
WARRANTIES.  ANY WARRANTY, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.

IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
___________________________________________________________________________

The GNU GPL 3.0+ License does not permit incorporating scEmacs into
proprietary programs or distribution without source code.  If you
need to do this, please contact me.

ShawnAmir AT comcast DOT net
___________________________________________________________________________

NOTE:	scEmacs requires a *fixed-width* XftFont for display!
	It is currently set as "Ubuntu mono-13:weight=medium:slant=roman",
	in sc_Main.c.  ***BE SURE TO CHANGE IT TO A FONT YOU HAVE***

	scEmacs currently runs ONLY on 64-bit systems as it explicitly
	uses Int64 and Uns64 data types.  Easy matter to undo....

External dependencies are only XLib and Xft (text rendering).

The code is hyper-commented simple C.  Executable is only about 185K
(64Bit system) with no Debugging information, rougly 480K with debug.

Files:

    sc_Main.c           Main event loop, basic application.
    	sc_Main.h           Interface for Main, register windows, blink timer, etc.
    	sc_Public.h         Public data types, Int32, Uns32, etc.
    	sc_Error.h          Error reporting, Assert and Debug Macros
    sc_Editor.c         Body of scEmacs editor
    	sc_Editor.h         Interface for scEmacs editor

___________________________________________________________________________

scEmacs supports:

        UTF8 text data
        Multiple Frames/windows
        Multiple Panes on each Frame
        Multiple Buffers
        Mouse clicks, selection, and scrolling
        Selection ranges
        Incremental search and query replace
        Clipboard + Primary XSel (copy/paste)
        KillRings
        MarkRings
        Undo and Undoing Undos....
        Most commonly used commands
        A limited QR mini-buffer
	M-x commands
        Numeric args
        Special pop-up windows

Limitations:

        Fixed-width characters
	Tabs become spaces
        No syntax-coloring

___________________________________________________________________________

Design Priorities, in rough order:

1.     Useable, capable, and useful
2.     Easy to embed and incorporate
3.     Clear + easy to maintain
4.     Minimal external dependence
5.     Easy to expand, alter, and customize
6.     Small footprint and resource efficient
7.     Fast enough
8.     Makes me happy
___________________________________________________________________________

Design + Definitions

Buffer  One and only 1 per open file, contains char data. There
        will be a BufferRecord (allocated in a slab) that contain info
	about the conceptual buffer, and there will be a BufMem which
	is a large hunk of memory (from malloc) that contains the actual
	char data (and a Gap).  No meta-data is stored in the buffer
	memory--lexical role, color, etc.

Window  Refers to traditional X Windows, NOT an Emacs pane.
	All X windows register their event handler with the main event
	loop which dispatches events to the specific handler.  Each
	Frame has a main X Window, but also InputOnly Windows for
	scroll bars and moveable mode lines (all but bottom-most Pane).

Frame   Each (main) X Window contains a single Emacs Frame.

Pane    Each Frame contains 1 or more Panes.  traditionally emacs
        called these windows, back in the VT100 terminal days.
        Each Pane displays a single buffer, but a given buffer
        may have 0 or more Panes displaying it--each potentially
        showing a different location.  Only a single Frame and
        a single Pane in that Frame is ever Active (has focus)
        and shows an Active (blinking) cursor.

Cursor  Each Pane has a single Cursor.  It maintains the location
        of this cursor on the screen (Col, Row) and its indexed
        Position in the memory Buffer.  Every Pane will always
        show its cursor, and the cursor is never allowed to wander
        out of the displayed range.  Inactive Panes always
        show an inactive cursor Box while active Panes have a
        blinking solid cursor.  If a Buffer is no longer shown
        in any Pane, it stashes the location of the Cursor in the
        BufferRecord, just so it can go to the "last" place if and
        when it is displayed again.

Gap     Each buffer represents the character data as a contiguous
        array along with a Gap that is moved around to allow text
        insertion.  Moving the memory around to move the gap is
        easier than having a complex data structure.  The Buffer
        memory is indexed by Pos or accessed directly by a Pointer.

Pos     Sees the whole buffer as conceptually contiguous, so
        Buffer[Pos==N] is always the N'th data location, regardless
        of gap location or size.

Pointer Is an actual pointer to the buffer, may fall within gap.
        Worse, the Pointer may be completely invalid if the Buffer
        memory is re-allocated (expanded) and moved to a new
        location.

        Rather than constantly worrying about the position of the
        gap, a Pos is generally used, manipulated, and passed around
        instead of a pointer.  Each time a character is to be read
        from the buffer, the 'Pos' has to be converted to a real
        Pointer to access the memory location.  The conversion process
        will take care to skip over the gap.

        Characters in the buffer are stored in UTF8 format.  While
        most will take a single byte of memory, some may take up to
        4 bytes.  Therefore, there is NOT a 1-to-1 correspondance
        between characters in the file and bytes (indexed) in the buffer.
        A Utf8 character (up to 4 bytes) will NEVER straddle the Gap.

        Every glyph is drawn as 1 mono-spaced character wide.  Double-
        spaced wide (typically Asian) glyphs are *NOT SUPPORTED* here.
        Tab characters (variable width) are also *NOT SUPPORTED* and are
        converted into space charactes as part of the initial "filtering"
        when a new file is read.  (Filtering also handles any necessary
        CR/LF -> '\n' conversions.)

Line    A string of characters starting after a '\n' (or beginning
        of the buffer) and ending in a '\n' (or end of buffer).
        A long line may occupy multiple display Rows if the Frame
        window is fairly narrow.

Row     One horizontal sweep of the screen.

        Operations that move up or down by one screen row must care about
        Rows and not Lines.  But while the beginning and end of a Line
        is fairly obvious (begin/end of buffer or '\n' character) the Row
        does not have an algorithmically obviously terminus.  So the trick
        is to first isolate the line origin, then move over one pane-width
        (Row) at a time, thus finding all the Rows.

___________________________________________________________________________

                  Commands --> Bindings
                  --------     --------
             backward-char --> <Left> or C-b 
             backward-word --> M-b 
          buffer-from-list --> C-x C-b or C-M-b 
           capitalize-word --> M-c 
               copy-region --> M-w 
               cursor-info --> C-x = 
           delete-backward --> <BackSpace> 
            delete-forward --> <Delete> or C-d 
              delete-frame --> C-x 5 0 
        delete-horiz-space --> M-\ 
       delete-other-frames --> C-x 5 1 
      delete-word-backward --> C-<BackSpace> or M-<BackSpace> or M-<Delete> 
       delete-word-forward --> C-<Delete> or M-d 
              disable-undo --> Not bound, use M-x disable-undo
             downcase-word --> M-l 
   exchange-point-and-mark --> C-x C-x 
            exec-from-list --> C-M-x 
            exec-named-cmd --> M-x 
                 find-file --> C-x C-f 
              forward-char --> <Right> or C-f 
              forward-word --> M-f 
                   go-down --> <Down> or C-n 
                     go-up --> <Up> or C-p 
                 goto-char --> M-g c 
                  goto-end --> C-<End> or M-> 
          goto-end-of-line --> <End> or C-e 
                 goto-line --> M-g M-g or M-g l or M-g g 
                goto-start --> C-<Home> or M-< 
        goto-start-of-line --> <Home> or C-a 
                      help --> C-h 
               insert-file --> C-x i 
                insert-tab --> <Tab> 
                join-lines --> M-^ 
               kill-buffer --> C-x k 
                 kill-line --> C-k 
          kill-other-panes --> C-x 1 
                 kill-pane --> C-x 0 
               kill-region --> C-w 
            mark-from-list --> C-M-<space> 
                 new-frame --> C-x 5 2 
                 next-page --> <Next> or C-v 
               other-frame --> C-x 5 o 
                other-pane --> C-x o 
                 prev-page --> <Prior> or M-v 
                       pwd --> Not bound, use M-x pwd
             query-replace --> M-% 
                      quit --> C-g 
             recenter-page --> C-l 
                reset-undo --> Not bound, use M-x reset-undo
             save-and-quit --> C-x C-c 
                 save-file --> C-x C-s 
           save-some-files --> C-x s 
           search-backward --> C-r 
            search-forward --> C-s 
                select-all --> C-x h 
               select-area --> Not bound, use M-x select-area
               select-line --> Not bound, use M-x select-line
                  set-mark --> C-@ or C-<space> 
                split-pane --> C-x 2 
          switch-to-buffer --> C-x b 
                      undo --> C-x u or C-_ or C-/ 
                  unmodify --> M-~ 
               upcase-word --> M-u 
                write-file --> C-x C-w 
                      yank --> C-y 
            yank-from-list --> C-M-y 
                  yank-pop --> M-y 
___________________________________________________________________________

Thanks for looking.

Shawn Amir
Jan 2018
Menlo Park, CA
