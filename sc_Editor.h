// ***********************************************************************
// Copyright (C) 2018 Shawn Amir
// All rights reserved
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
// sc_Editor.h
// Editor headors for scEmacs
// ***********************************************************************

void	ED_EditorInit(Display * XDP, XftFont * XFP, XIM XIMP, Int32 WinWidth, Int32 WinHeight);
void	ED_EditorKill(void);
void	ED_EditorOpenFile(char *PathP, Int16 NewFrame);

void	ED_BlinkHandler(void);

