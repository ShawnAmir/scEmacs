// ***********************************************************************
// Copyright (C) 2018 Shawn Amir
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
// sc_Main.h
// Interface exported from sc_Main.c
// ***********************************************************************

#define _sc_SASTOREPOINTER	struct _sc_SAStore *
#define _sc_SARACKPOINTER	struct _sc_SARack *

    #define sc_SASTORENOFLAG	0L

    typedef struct _sc_SAStore {
	_sc_SARACKPOINTER	FirstRackP;			// First allocated Block
	void *			EmptyBlockP;			// First Empty Data Block
	Uns32			BytesPerBlock;			// Size of Data Blocks
	Uns32			BlocksPerRack;			// How many blocks in 1 Rack
	Uns32			UsedBlocks;			// Data blocks in use
	Uns32			FreeBlocks;			// Data blocks still free
	Uns32			RackCount;			// Racks in this store
	Uns32			Flags;
    } sc_SAStore, *sc_SAStorePointer;

    typedef struct _sc_SARack {
	_sc_SARACKPOINTER	NextP;				// Next Rack in Store
	_sc_SARACKPOINTER	PrevP;				// Prev Rack in Store
	// Followed by N Data Blocks
    } sc_SARack, * sc_SARackPointer;

#undef _sc_SARACKPOINTER
#undef _sc_SASTOREPOINTER


void	sc_SAStoreOpen(sc_SAStorePointer StoreP, Uns32 BytesPerBlock, Uns32 BlocksPerRack);
void	sc_SAStoreClose(sc_SAStorePointer StoreP);
void *	sc_SAStoreAllocBlock(sc_SAStorePointer StoreP);
void	sc_SAStoreFreeBlock(sc_SAStorePointer StoreP, void * BlockP);

void	sc_WERegAdd(Uns64 Id, void * FP, void * DataP);
void	sc_WERegDel(Uns64 Id);

void	sc_BlinkTimerReset(void);

void	sc_MainExit(void);
void	sc_MainEventLoop(void);

