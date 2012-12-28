/***************************************************************************
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02111-1307 USA.           *
 ***************************************************************************/

/* 
* Handles all CD-ROM registers and functions.
*/

#include "cdrom.h"
#include "ppf.h"
#include "psxdma.h"

/* logging */
#if 0
#define CDR_LOG SysPrintf
#else
#define CDR_LOG(...)
#endif
#if 0
#define CDR_LOG_I SysPrintf
#else
#define CDR_LOG_I(...)
#endif
#if 0
#define CDR_LOG_IO SysPrintf
#else
#define CDR_LOG_IO(...)
#endif
//#define CDR_LOG_CMD_IRQ

cdrStruct cdr;
static unsigned char *pTransfer;

/* CD-ROM magic numbers */
#define CdlSync        0
#define CdlNop         1
#define CdlSetloc      2
#define CdlPlay        3
#define CdlForward     4
#define CdlBackward    5
#define CdlReadN       6
#define CdlStandby     7
#define CdlStop        8
#define CdlPause       9
#define CdlInit        10
#define CdlMute        11
#define CdlDemute      12
#define CdlSetfilter   13
#define CdlSetmode     14
#define CdlGetmode     15
#define CdlGetlocL     16
#define CdlGetlocP     17
#define CdlReadT       18
#define CdlGetTN       19
#define CdlGetTD       20
#define CdlSeekL       21
#define CdlSeekP       22
#define CdlSetclock    23
#define CdlGetclock    24
#define CdlTest        25
#define CdlID          26
#define CdlReadS       27
#define CdlReset       28
#define CdlReadToc     30

char *CmdName[0x100]= {
    "CdlSync",     "CdlNop",       "CdlSetloc",  "CdlPlay",
    "CdlForward",  "CdlBackward",  "CdlReadN",   "CdlStandby",
    "CdlStop",     "CdlPause",     "CdlInit",    "CdlMute",
    "CdlDemute",   "CdlSetfilter", "CdlSetmode", "CdlGetmode",
    "CdlGetlocL",  "CdlGetlocP",   "CdlReadT",   "CdlGetTN",
    "CdlGetTD",    "CdlSeekL",     "CdlSeekP",   "CdlSetclock",
    "CdlGetclock", "CdlTest",      "CdlID",      "CdlReadS",
    "CdlReset",    NULL,           "CDlReadToc", NULL
};

unsigned char Test04[] = { 0 };
unsigned char Test05[] = { 0 };
unsigned char Test20[] = { 0x98, 0x06, 0x10, 0xC3 };
unsigned char Test22[] = { 0x66, 0x6F, 0x72, 0x20, 0x45, 0x75, 0x72, 0x6F };
unsigned char Test23[] = { 0x43, 0x58, 0x44, 0x32, 0x39 ,0x34, 0x30, 0x51 };

// cdr.Stat:
#define NoIntr		0
#define DataReady	1
#define Complete	2
#define Acknowledge	3
#define DataEnd		4
#define DiskError	5

/* Modes flags */
#define MODE_SPEED       (1<<7) // 0x80
#define MODE_STRSND      (1<<6) // 0x40 ADPCM on/off
#define MODE_SIZE_2340   (1<<5) // 0x20
#define MODE_SIZE_2328   (1<<4) // 0x10
#define MODE_SF          (1<<3) // 0x08 channel on/off
#define MODE_REPORT      (1<<2) // 0x04
#define MODE_AUTOPAUSE   (1<<1) // 0x02
#define MODE_CDDA        (1<<0) // 0x01

/* Status flags */
#define STATUS_PLAY      (1<<7) // 0x80
#define STATUS_SEEK      (1<<6) // 0x40
#define STATUS_READ      (1<<5) // 0x20
#define STATUS_SHELLOPEN (1<<4) // 0x10
#define STATUS_UNKNOWN3  (1<<3) // 0x08
#define STATUS_UNKNOWN2  (1<<2) // 0x04
#define STATUS_ROTATING  (1<<1) // 0x02
#define STATUS_ERROR     (1<<0) // 0x01



// 1x = 75 sectors per second
// PSXCLK = 1 sec in the ps
// so (PSXCLK / 75) = cdr read time (linuzappz)
#define cdReadTime (PSXCLK / 75)

// for cdr.Seeked
enum seeked_state {
	SEEK_PENDING = 0,
	SEEK_DONE = 1,
};

static struct CdrStat stat;

static unsigned int msf2sec(u8 *msf) {
	return ((msf[0] * 60 + msf[1]) * 75) + msf[2];
}

static void sec2msf(unsigned int s, u8 *msf) {
	msf[0] = s / 75 / 60;
	s = s - msf[0] * 75 * 60;
	msf[1] = s / 75;
	s = s - msf[1] * 75;
	msf[2] = s;
}

// cdrInterrupt
#define CDR_INT(eCycle) { \
	psxRegs.interrupt |= (1 << PSXINT_CDR); \
	psxRegs.intCycle[PSXINT_CDR].cycle = eCycle; \
	psxRegs.intCycle[PSXINT_CDR].sCycle = psxRegs.cycle; \
	new_dyna_set_event(PSXINT_CDR, eCycle); \
}

// cdrReadInterrupt
#define CDREAD_INT(eCycle) { \
	psxRegs.interrupt |= (1 << PSXINT_CDREAD); \
	psxRegs.intCycle[PSXINT_CDREAD].cycle = eCycle; \
	psxRegs.intCycle[PSXINT_CDREAD].sCycle = psxRegs.cycle; \
	new_dyna_set_event(PSXINT_CDREAD, eCycle); \
}

// cdrLidSeekInterrupt
#define CDRLID_INT(eCycle) { \
	psxRegs.interrupt |= (1 << PSXINT_CDRLID); \
	psxRegs.intCycle[PSXINT_CDRLID].cycle = eCycle; \
	psxRegs.intCycle[PSXINT_CDRLID].sCycle = psxRegs.cycle; \
	new_dyna_set_event(PSXINT_CDRLID, eCycle); \
}

// cdrPlayInterrupt
#define CDRMISC_INT(eCycle) { \
	psxRegs.interrupt |= (1 << PSXINT_CDRPLAY); \
	psxRegs.intCycle[PSXINT_CDRPLAY].cycle = eCycle; \
	psxRegs.intCycle[PSXINT_CDRPLAY].sCycle = psxRegs.cycle; \
	new_dyna_set_event(PSXINT_CDRPLAY, eCycle); \
}

#define StopReading() { \
	if (cdr.Reading) { \
		cdr.Reading = 0; \
		psxRegs.interrupt &= ~(1 << PSXINT_CDREAD); \
	} \
	cdr.StatP &= ~STATUS_READ;\
}

#define StopCdda() { \
	if (cdr.Play) { \
		if (!Config.Cdda) CDR_stop(); \
		cdr.StatP &= ~STATUS_PLAY; \
		cdr.Play = FALSE; \
		cdr.FastForward = 0; \
		cdr.FastBackward = 0; \
		/*SPU_registerCallback( SPUirq );*/ \
	} \
}

#define SetResultSize(size) { \
	cdr.ResultP = 0; \
	cdr.ResultC = size; \
	cdr.ResultReady = 1; \
}

static void setIrq(void)
{
	if (cdr.Stat & cdr.Reg2)
		psxHu32ref(0x1070) |= SWAP32((u32)0x4);
}

void cdrLidSeekInterrupt()
{
	// turn back on checking
	if( cdr.LidCheck == 0x10 )
	{
		cdr.LidCheck = 0;
	}

	// official lid close
	else if( cdr.LidCheck == 0x30 )
	{
		// GS CDX 3.3: $13
		cdr.StatP |= STATUS_ROTATING;


		// GS CDX 3.3 - ~50 getlocp tries
		CDRLID_INT( cdReadTime * 3 );
		cdr.LidCheck = 0x40;
	}

	// turn off ready
	else if( cdr.LidCheck == 0x40 )
	{
		// GS CDX 3.3: $01
		cdr.StatP &= ~STATUS_SHELLOPEN;
		cdr.StatP &= ~STATUS_ROTATING;


		// GS CDX 3.3 - ~50 getlocp tries
		CDRLID_INT( cdReadTime * 3 );
		cdr.LidCheck = 0x50;
	}

	// now seek
	else if( cdr.LidCheck == 0x50 )
	{
		// GameShark Lite: Start seeking ($42)
		cdr.StatP |= STATUS_SEEK;
		cdr.StatP |= STATUS_ROTATING;
		cdr.StatP &= ~STATUS_ERROR;


		CDRLID_INT( cdReadTime * 3 );
		cdr.LidCheck = 0x60;
	}

	// done = cd ready
	else if( cdr.LidCheck == 0x60 )
	{
		// GameShark Lite: Seek detection done ($02)
		cdr.StatP &= ~STATUS_SEEK;

		cdr.LidCheck = 0;
	}
}

static void Check_Shell( int Irq )
{
	// check case open/close
	if (cdr.LidCheck > 0)
	{
		CDR_LOG( "LidCheck\n" );

		// $20 = check lid state
		if( cdr.LidCheck == 0x20 )
		{
			u32 i;

			i = stat.Status;
			if (CDR_getStatus(&stat) != -1)
			{
				// BIOS hangs + BIOS error messages
				//if (stat.Type == 0xff)
					//cdr.Stat = DiskError;

				// case now open
				if (stat.Status & STATUS_SHELLOPEN)
				{
					// Vib Ribbon: pre-CD swap
					StopCdda();


					// GameShark Lite: Death if DiskError happens
					//
					// Vib Ribbon: Needs DiskError for CD swap

					if (Irq != CdlNop)
					{
						cdr.Stat = DiskError;

						cdr.StatP |= STATUS_ERROR;
						cdr.Result[0] |= STATUS_ERROR;
					}

					// GameShark Lite: Wants -exactly- $10
					cdr.StatP |= STATUS_SHELLOPEN;
					cdr.StatP &= ~STATUS_ROTATING;


					CDRLID_INT( cdReadTime * 3 );
					cdr.LidCheck = 0x10;


					// GS CDX 3.3 = $11
				}

				// case just closed
				else if ( i & STATUS_SHELLOPEN )
				{
					cdr.StatP |= STATUS_ROTATING;

					CheckCdrom();


					if( cdr.Stat == NoIntr )
						cdr.Stat = Acknowledge;

					setIrq();

					// begin close-seek-ready cycle
					CDRLID_INT( cdReadTime * 3 );
					cdr.LidCheck = 0x30;


					// GameShark Lite: Wants -exactly- $42, then $02
					// GS CDX 3.3: Wants $11/$80, $13/$80, $01/$00
				}

				// case still closed - wait for recheck
				else
				{
					CDRLID_INT( cdReadTime * 3 );
					cdr.LidCheck = 0x10;
				}
			}
		}


		// GS CDX: clear all values but #1,#2
		if( (cdr.LidCheck >= 0x30) || (cdr.StatP & STATUS_SHELLOPEN) )
		{
			SetResultSize(16);
			memset( cdr.Result, 0, 16 );

			cdr.Result[0] = cdr.StatP;


			// GS CDX: special return value
			if( cdr.StatP & STATUS_SHELLOPEN )
			{
				cdr.Result[1] = 0x80;
			}


			if( cdr.Stat == NoIntr )
				cdr.Stat = Acknowledge;

			setIrq();
		}
	}
}

void Find_CurTrack() {
	cdr.CurTrack = 0;

	if (CDR_getTN(cdr.ResultTN) != -1) {
		int lcv;

		for( lcv = 1; lcv <= cdr.ResultTN[1]; lcv++ ) {
			if (CDR_getTD((u8)(lcv), cdr.ResultTD) != -1) {
				u32 sect1, sect2;

				CDR_LOG( "curtrack %d %d %d | %d %d %d | %d\n",
					cdr.SetSectorPlay[0], cdr.SetSectorPlay[1], cdr.SetSectorPlay[2],
					cdr.ResultTD[2], cdr.ResultTD[1], cdr.ResultTD[0],
					cdr.CurTrack );

				// find next track boundary - only need m:s accuracy
				sect1 = cdr.SetSectorPlay[0] * 60 * 75 + cdr.SetSectorPlay[1] * 75;
				sect2 = cdr.ResultTD[2] * 60 * 75 + cdr.ResultTD[1] * 75;

				// Twisted Metal 4 - psx cdda pregap (2-sec)
				// - fix in-game music
				sect2 -= 75 * 2;

				if( sect1 >= sect2 ) {
					cdr.CurTrack++;
					continue;
				}
			}

			break;
		}
	}
}

static void ReadTrack( u8 *time ) {
	cdr.Prev[0] = itob( time[0] );
	cdr.Prev[1] = itob( time[1] );
	cdr.Prev[2] = itob( time[2] );

	CDR_LOG("ReadTrack() Log: KEY *** %x:%x:%x\n", cdr.Prev[0], cdr.Prev[1], cdr.Prev[2]);
	cdr.RErr = CDR_readTrack(cdr.Prev);
}


static void AddIrqQueue(unsigned char irq, unsigned long ecycle) {
	if (cdr.Irq != 0)
		CDR_LOG_I("cdr: override cmd %02x -> %02x\n", cdr.Irq, irq);

	cdr.Irq = irq;
	cdr.eCycle = ecycle;

	CDR_INT(ecycle);
}


void Set_Track()
{
	if (CDR_getTN(cdr.ResultTN) != -1) {
		int lcv;

		for( lcv = 1; lcv < cdr.ResultTN[1]; lcv++ ) {
			if (CDR_getTD((u8)(lcv), cdr.ResultTD) != -1) {
				CDR_LOG( "settrack %d %d %d | %d %d %d | %d\n",
					cdr.SetSectorPlay[0], cdr.SetSectorPlay[1], cdr.SetSectorPlay[2],
					cdr.ResultTD[2], cdr.ResultTD[1], cdr.ResultTD[0],
					cdr.CurTrack );

				// check if time matches track start (only need min, sec accuracy)
				// - m:s:f vs f:s:m
				if( cdr.SetSectorPlay[0] == cdr.ResultTD[2] &&
						cdr.SetSectorPlay[1] == cdr.ResultTD[1] ) {
					// skip pregap frames
					if( cdr.SetSectorPlay[2] < cdr.ResultTD[0] )
						cdr.SetSectorPlay[2] = cdr.ResultTD[0];

					break;
				}
				else if( cdr.SetSectorPlay[0] < cdr.ResultTD[2] )
					break;
			}
		}
	}
}


static u8 fake_subq_local[3], fake_subq_real[3], fake_subq_index, fake_subq_change;
static void Create_Fake_Subq()
{
	u8 temp_cur[3], temp_next[3], temp_start[3], pregap;
	int diff;

	if (CDR_getTN(cdr.ResultTN) == -1) return;
	if( cdr.CurTrack+1 <= cdr.ResultTN[1] ) {
		pregap = 150;
		if( CDR_getTD(cdr.CurTrack+1, cdr.ResultTD) == -1 ) return;
	} else {
		// last track - cd size
		pregap = 0;
		if( CDR_getTD(0, cdr.ResultTD) == -1 ) return;
	}

	if( cdr.Play == TRUE ) {
		temp_cur[0] = cdr.SetSectorPlay[0];
		temp_cur[1] = cdr.SetSectorPlay[1];
		temp_cur[2] = cdr.SetSectorPlay[2];
	} else {
		temp_cur[0] = btoi( cdr.Prev[0] );
		temp_cur[1] = btoi( cdr.Prev[1] );
		temp_cur[2] = btoi( cdr.Prev[2] );
	}

	fake_subq_real[0] = temp_cur[0];
	fake_subq_real[1] = temp_cur[1];
	fake_subq_real[2] = temp_cur[2];

	temp_next[0] = cdr.ResultTD[2];
	temp_next[1] = cdr.ResultTD[1];
	temp_next[2] = cdr.ResultTD[0];


	// flag- next track
	if( msf2sec(temp_cur) >= msf2sec( temp_next )-pregap ) {
		fake_subq_change = 1;

		cdr.CurTrack++;

		// end cd
		if( pregap == 0 ) StopCdda();
	}

	//////////////////////////////////////////////////
	//////////////////////////////////////////////////

	// repair
	if( cdr.CurTrack <= cdr.ResultTN[1] ) {
		if( CDR_getTD(cdr.CurTrack, cdr.ResultTD) == -1 ) return;
	} else {
		// last track - cd size
		if( CDR_getTD(0, cdr.ResultTD) == -1 ) return;
	}
	
	temp_start[0] = cdr.ResultTD[2];
	temp_start[1] = cdr.ResultTD[1];
	temp_start[2] = cdr.ResultTD[0];


	CDR_LOG( "CDDA FAKE SUB - %d:%d:%d / %d:%d:%d / %d:%d:%d\n",
		temp_cur[0], temp_cur[1], temp_cur[2],
		temp_start[0], temp_start[1], temp_start[2],
		temp_next[0], temp_next[1], temp_next[2]);



	// local time - pregap / real
	diff = msf2sec(temp_cur) - msf2sec( temp_start );
	if( diff < 0 ) {
		fake_subq_index = 0;

		sec2msf( -diff, fake_subq_local );
	} else {
		fake_subq_index = 1;

		sec2msf( diff, fake_subq_local );
	}
}


static void cdrPlayInterrupt_Autopause()
{
	struct SubQ *subq = (struct SubQ *)CDR_getBufferSub();
	int track_changed = 0;
	if (subq != NULL ) {
		/*
		CDDA Autopause

		Silhouette Mirage ($3)
		Tomb Raider 1 ($7)
		*/

		// .. + 1 is probably wrong, but deals with corrupted subq + good checksum
		// (how does real thing handle those?)
		if( cdr.CurTrack + 1 == btoi( subq->TrackNumber ) )
			track_changed = 1;
	} else {
		track_changed = fake_subq_change;
		fake_subq_change = 0;
	}

	if ((cdr.Mode & MODE_AUTOPAUSE) && track_changed) {
		CDR_LOG( "CDDA STOP\n" );

		// Magic the Gathering
		// - looping territory cdda

		// ...?
		//cdr.ResultReady = 1;
		//cdr.Stat = DataReady;
		cdr.Stat = DataEnd;
		setIrq();

		StopCdda();
	}
	else if (cdr.Mode & MODE_REPORT) {
		if (subq != NULL) {
			CDR_LOG( "REPPLAY SUB - %X:%X:%X\n",
				subq->AbsoluteAddress[0], subq->AbsoluteAddress[1], subq->AbsoluteAddress[2] );

			// breaks when .sub doesn't have index 0 for some reason (bad rip?)
			//cdr.CurTrack = btoi( subq->TrackNumber );

			if (subq->AbsoluteAddress[2] & 0xf)
				return;

			cdr.Result[0] = cdr.StatP;
			// BIOS CD Player: data already BCD format
			cdr.Result[1] = subq->TrackNumber;
			cdr.Result[2] = subq->IndexNumber;

			cdr.Result[3] = subq->AbsoluteAddress[0];
			cdr.Result[4] = subq->AbsoluteAddress[1];
			cdr.Result[5] = subq->AbsoluteAddress[2];
		} else {
			CDR_LOG( "REPPLAY FAKE - %d:%d:%d\n",
				fake_subq_real[0], fake_subq_real[1], fake_subq_real[2] );

			if (fake_subq_real[2] & 0xf)
				return;

			cdr.Result[0] = cdr.StatP;
			// track # / index #
			cdr.Result[1] = itob(cdr.CurTrack);
			cdr.Result[2] = itob(fake_subq_index);
			// absolute
			cdr.Result[3] = itob( fake_subq_real[0] );
			cdr.Result[4] = itob( fake_subq_real[1] );
			cdr.Result[5] = itob( fake_subq_real[2] );
		}

		// Rayman: Logo freeze (resultready + dataready)
		cdr.ResultReady = 1;
		cdr.Stat = DataReady;

		SetResultSize(8);
		setIrq();
	}
}

// also handles seek
void cdrPlayInterrupt()
{
	if (cdr.Seeked == SEEK_PENDING) {
		if (cdr.Stat) {
			CDR_LOG_I("cdrom: seek stat hack\n");
			CDRMISC_INT(0x1000);
			return;
		}
		SetResultSize(1);
		cdr.StatP |= STATUS_ROTATING;
		cdr.StatP &= ~STATUS_SEEK;
		cdr.Result[0] = cdr.StatP;
		cdr.Seeked = SEEK_DONE;
		if (cdr.Irq == 0) {
			cdr.Stat = Complete;
			setIrq();
		}

		memcpy(cdr.SetSectorPlay, cdr.SetSector, 4);
		Find_CurTrack();
		ReadTrack(cdr.SetSectorPlay);
	}

	// update for CdlGetlocP/autopause
	struct SubQ *subq = (struct SubQ *)CDR_getBufferSub();
	if (subq != NULL)
		// update subq
		ReadTrack(cdr.SetSectorPlay);
	else
		Create_Fake_Subq();

	if (!cdr.Play) return;

	CDR_LOG( "CDDA - %d:%d:%d\n",
		cdr.SetSectorPlay[0], cdr.SetSectorPlay[1], cdr.SetSectorPlay[2] );

	CDRMISC_INT( cdReadTime );

	if (!cdr.Irq && !cdr.Stat && (cdr.Mode & (MODE_AUTOPAUSE|MODE_REPORT)))
		cdrPlayInterrupt_Autopause();

	cdr.SetSectorPlay[2]++;
	if (cdr.SetSectorPlay[2] == 75) {
		cdr.SetSectorPlay[2] = 0;
		cdr.SetSectorPlay[1]++;
		if (cdr.SetSectorPlay[1] == 60) {
			cdr.SetSectorPlay[1] = 0;
			cdr.SetSectorPlay[0]++;
		}
	}

	//Check_Shell(0);
}

void cdrInterrupt() {
	int i;
	unsigned char Irq = cdr.Irq;
	struct SubQ *subq;

	// Reschedule IRQ
	if (cdr.Stat) {
		CDR_LOG_I("cdrom: stat hack: %02x %x\n", cdr.Irq, cdr.Stat);
		CDR_INT(0x1000);
		return;
	}

	cdr.Irq = 0;
	cdr.Ctrl &= ~0x80;

	switch (Irq) {
		case CdlSync:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge; 
			break;

		case CdlNop:
			SetResultSize(1);
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;

			if (cdr.LidCheck == 0) cdr.LidCheck = 0x20;
			break;

		case CdlSetloc:
			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
			break;

		case CdlPlay:
			fake_subq_change = 0;

			if (cdr.Seeked == SEEK_PENDING) {
				// XXX: wrong, should seek instead..
				memcpy( cdr.SetSectorPlay, cdr.SetSector, 4 );
				cdr.Seeked = SEEK_DONE;
			}

			/*
			Rayman: detect track changes
			- fixes logo freeze

			Twisted Metal 2: skip PREGAP + starting accurate SubQ
			- plays tracks without retry play

			Wild 9: skip PREGAP + starting accurate SubQ
			- plays tracks without retry play
			*/
			/* unneeded with correct cdriso?
			Set_Track();
			*/
			Find_CurTrack();
			ReadTrack( cdr.SetSectorPlay );

			// GameShark CD Player: Calls 2x + Play 2x
			if( cdr.FastBackward || cdr.FastForward ) {
				if( cdr.FastForward ) cdr.FastForward--;
				if( cdr.FastBackward ) cdr.FastBackward--;

				if( cdr.FastBackward == 0 && cdr.FastForward == 0 ) {
					if( cdr.Play && CDR_getStatus(&stat) != -1 ) {
						cdr.SetSectorPlay[0] = stat.Time[0];
						cdr.SetSectorPlay[1] = stat.Time[1];
						cdr.SetSectorPlay[2] = stat.Time[2];
					}
				}
			}


			if (!Config.Cdda) {
				// BIOS CD Player
				// - Pause player, hit Track 01/02/../xx (Setloc issued!!)

				// GameShark CD Player: Resume play
				if( cdr.ParamC == 0 ) {
					CDR_LOG( "PLAY Resume @ %d:%d:%d\n",
						cdr.SetSectorPlay[0], cdr.SetSectorPlay[1], cdr.SetSectorPlay[2] );

					//CDR_play( cdr.SetSectorPlay );
				}
				else
				{
					// BIOS CD Player: Resume play
					if( cdr.Param[0] == 0 ) {
						CDR_LOG( "PLAY Resume T0 @ %d:%d:%d\n",
							cdr.SetSectorPlay[0], cdr.SetSectorPlay[1], cdr.SetSectorPlay[2] );

						//CDR_play( cdr.SetSectorPlay );
					}
					else {
						CDR_LOG( "PLAY Resume Td @ %d:%d:%d\n",
							cdr.SetSectorPlay[0], cdr.SetSectorPlay[1], cdr.SetSectorPlay[2] );

						// BIOS CD Player: Allow track replaying
						StopCdda();


						cdr.CurTrack = btoi( cdr.Param[0] );

						if (CDR_getTN(cdr.ResultTN) != -1) {
							// check last track
							if (cdr.CurTrack > cdr.ResultTN[1])
								cdr.CurTrack = cdr.ResultTN[1];

							if (CDR_getTD((u8)(cdr.CurTrack), cdr.ResultTD) != -1) {
								cdr.SetSectorPlay[0] = cdr.ResultTD[2];
								cdr.SetSectorPlay[1] = cdr.ResultTD[1];
								cdr.SetSectorPlay[2] = cdr.ResultTD[0];

								// reset data
								//Set_Track();
								Find_CurTrack();
								ReadTrack( cdr.SetSectorPlay );

								//CDR_play(cdr.SetSectorPlay);
							}
						}
					}
				}
			}


			// Vib Ribbon: gameplay checks flag
			cdr.StatP &= ~STATUS_SEEK;


			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;

			cdr.StatP |= STATUS_PLAY;

			
			// BIOS player - set flag again
			cdr.Play = TRUE;

			CDRMISC_INT( cdReadTime );
			break;

		case CdlForward:
			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Complete;


			// GameShark CD Player: Calls 2x + Play 2x
			if( cdr.FastForward == 0 ) cdr.FastForward = 2;
			else cdr.FastForward++;

			cdr.FastBackward = 0;
			break;

		case CdlBackward:
			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Complete;


			// GameShark CD Player: Calls 2x + Play 2x
			if( cdr.FastBackward == 0 ) cdr.FastBackward = 2;
			else cdr.FastBackward++;

			cdr.FastForward = 0;
			break;

		case CdlStandby:
			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Complete;
			break;

		case CdlStop:
			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP &= ~STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Complete;
//			cdr.Stat = Acknowledge;

			if (cdr.LidCheck == 0) cdr.LidCheck = 0x20;
			break;

		case CdlPause:
			SetResultSize(1);
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;

			/*
			Gundam Battle Assault 2: much slower (*)
			- Fixes boot, gameplay

			Hokuto no Ken 2: slower
			- Fixes intro + subtitles

			InuYasha - Feudal Fairy Tale: slower
			- Fixes battles
			*/
			AddIrqQueue(CdlPause + 0x20, cdReadTime * 3);
			cdr.Ctrl |= 0x80;
			break;

		case CdlPause + 0x20:
			SetResultSize(1);
			cdr.StatP &= ~STATUS_READ;
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Complete;
			break;

		case CdlInit:
			SetResultSize(1);
			cdr.StatP = STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
//			if (!cdr.Init) {
				AddIrqQueue(CdlInit + 0x20, 0x800);
//			}
        	break;

		case CdlInit + 0x20:
			SetResultSize(1);
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Complete;
			cdr.Init = 1;
			break;

		case CdlMute:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
			break;

		case CdlDemute:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
			break;

		case CdlSetfilter:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge; 
			break;

		case CdlSetmode:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
			break;

		case CdlGetmode:
			SetResultSize(6);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Result[1] = cdr.Mode;
			cdr.Result[2] = cdr.File;
			cdr.Result[3] = cdr.Channel;
			cdr.Result[4] = 0;
			cdr.Result[5] = 0;
			cdr.Stat = Acknowledge;
			break;

		case CdlGetlocL:
			SetResultSize(8);
			for (i = 0; i < 8; i++)
				cdr.Result[i] = cdr.Transfer[i];
			cdr.Stat = Acknowledge;
			break;

		case CdlGetlocP:
			// GameShark CDX CD Player: uses 17 bytes output (wraps around)
			SetResultSize(17);
			memset( cdr.Result, 0, 16 );

			subq = (struct SubQ *)CDR_getBufferSub();

			if (subq != NULL) {
				// update subq
				ReadTrack( cdr.SetSector );

				cdr.Result[0] = subq->TrackNumber;
				cdr.Result[1] = subq->IndexNumber;
				memcpy(cdr.Result + 2, subq->TrackRelativeAddress, 3);
				memcpy(cdr.Result + 5, subq->AbsoluteAddress, 3);


				// subQ integrity check - data only (skip audio)
				if( subq->TrackNumber == 1 && stat.Type == 0x01 ) {
				if (calcCrc((u8 *)subq + 12, 10) != (((u16)subq->CRC[0] << 8) | subq->CRC[1])) {
					memset(cdr.Result + 2, 0, 3 + 3); // CRC wrong, wipe out time data
				}
				}
			} else {
				if (cdr.Play == FALSE)
					Create_Fake_Subq();

				// track # / index #
				cdr.Result[0] = itob(cdr.CurTrack);
				cdr.Result[1] = itob(fake_subq_index);

				// local
				cdr.Result[2] = itob( fake_subq_local[0] );
				cdr.Result[3] = itob( fake_subq_local[1] );
				cdr.Result[4] = itob( fake_subq_local[2] );

				// absolute
				cdr.Result[5] = itob( fake_subq_real[0] );
				cdr.Result[6] = itob( fake_subq_real[1] );
				cdr.Result[7] = itob( fake_subq_real[2] );
			}

			if (!cdr.Play && !cdr.Reading)
				cdr.Result[1] = 0; // HACK?

			// redump.org - wipe time
			if( !cdr.Play && CheckSBI(cdr.Result+5) ) {
				memset( cdr.Result+2, 0, 6 );
			}

			cdr.Stat = Acknowledge;
			break;

		case CdlGetTN:
			// 5-Star Racing: don't stop CDDA
			//
			// Vib Ribbon: CD swap
			StopReading();

			cdr.CmdProcess = 0;
			SetResultSize(3);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			if (CDR_getTN(cdr.ResultTN) == -1) {
				cdr.Stat = DiskError;
				cdr.Result[0] |= STATUS_ERROR;
			} else {
				cdr.Stat = Acknowledge;
				cdr.Result[1] = itob(cdr.ResultTN[0]);
				cdr.Result[2] = itob(cdr.ResultTN[1]);
			}
			break;

		case CdlGetTD:
			cdr.CmdProcess = 0;
			cdr.Track = btoi(cdr.Param[0]);
			SetResultSize(4);
			cdr.StatP |= STATUS_ROTATING;
			if (CDR_getTD(cdr.Track, cdr.ResultTD) == -1) {
				cdr.Stat = DiskError;
				cdr.Result[0] |= STATUS_ERROR;
			} else {
				cdr.Stat = Acknowledge;
				cdr.Result[0] = cdr.StatP;
				cdr.Result[1] = itob(cdr.ResultTD[2]);
				cdr.Result[2] = itob(cdr.ResultTD[1]);
				cdr.Result[3] = itob(cdr.ResultTD[0]);
			}
			break;

		case CdlSeekL:
		case CdlSeekP:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.StatP |= STATUS_SEEK;
			cdr.Stat = Acknowledge;

			/*
			Crusaders of Might and Magic = 0.5x-4x
			- fix cutscene speech start

			Eggs of Steel = 2x-?
			- fix new game

			Medievil = ?-4x
			- fix cutscene speech

			Rockman X5 = 0.5-4x
			- fix capcom logo
			*/
			CDRMISC_INT(cdr.Seeked == SEEK_DONE ? 0x800 : cdReadTime * 4);
			cdr.Seeked = SEEK_PENDING;
			break;

		case CdlTest:
			cdr.Stat = Acknowledge;
			switch (cdr.Param[0]) {
				case 0x20: // System Controller ROM Version
					SetResultSize(4);
					memcpy(cdr.Result, Test20, 4);
					break;
				case 0x22:
					SetResultSize(8);
					memcpy(cdr.Result, Test22, 4);
					break;
				case 0x23: case 0x24:
					SetResultSize(8);
					memcpy(cdr.Result, Test23, 4);
					break;
			}
			break;

		case CdlID:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
			AddIrqQueue(CdlID + 0x20, 0x800);
			break;

		case CdlID + 0x20:
			SetResultSize(8);

			if (CDR_getStatus(&stat) == -1) {
				cdr.Result[0] = 0x00; // 0x08 and cdr.Result[1]|0x10 : audio cd, enters cd player
				cdr.Result[1] = 0x80; // 0x80 leads to the menu in the bios, else loads CD
			}
			else {
				if (stat.Type == 2) {
					// Music CD
					cdr.Result[0] = 0x08;
					cdr.Result[1] = 0x10;

					cdr.Result[1] |= 0x80;
				}
				else {
					// Data CD
					if (CdromId[0] == '\0') {
						cdr.Result[0] = 0x00;
						cdr.Result[1] = 0x80;
					}
					else {
						cdr.Result[0] = 0x08;
						cdr.Result[1] = 0x00;
					}
				}
			}

			cdr.Result[2] = 0x00;
			cdr.Result[3] = 0x00;
			strncpy((char *)&cdr.Result[4], "PCSX", 4);
			cdr.Stat = Complete;
			break;

		case CdlReset:
			SetResultSize(1);
			cdr.StatP = STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
			break;

		case CdlReadT:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
			AddIrqQueue(CdlReadT + 0x20, 0x800);
			break;

		case CdlReadT + 0x20:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Complete;
			break;

		case CdlReadToc:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
			AddIrqQueue(CdlReadToc + 0x20, 0x800);
			break;

		case CdlReadToc + 0x20:
			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Complete;
			break;

		case CdlReadN:
		case CdlReadS:
			if (!cdr.Reading) return;


			// Fighting Force 2 - update subq time immediately
			// - fixes new game
			ReadTrack( cdr.SetSector );


			// Crusaders of Might and Magic - update getlocl now
			// - fixes cutscene speech
			{
				u8 *buf = CDR_getBuffer();
				if (buf != NULL)
					memcpy(cdr.Transfer, buf, 8);
			}

			/*
			Duke Nukem: Land of the Babes - seek then delay read for one frame
			- fixes cutscenes
			C-12 - Final Resistance - doesn't like seek
			*/

			if (cdr.Seeked != SEEK_DONE) {
				cdr.StatP |= STATUS_SEEK;
				cdr.StatP &= ~STATUS_READ;

				// Crusaders of Might and Magic - use short time
				// - fix cutscene speech (startup)

				// ??? - use more accurate seek time later
				CDREAD_INT((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime * 1);
			} else {
				cdr.StatP |= STATUS_READ;
				cdr.StatP &= ~STATUS_SEEK;

				CDREAD_INT((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime * 1);
			}

			SetResultSize(1);
			cdr.StatP |= STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
			break;

		case 0xff:
			return;

		default:
			cdr.Stat = Complete;
			break;
	}

	Check_Shell( Irq );

	cdr.ParamC = 0;

	setIrq();

#ifdef CDR_LOG_CMD_IRQ
	SysPrintf("cdrInterrupt() Log: CDR Interrupt IRQ %d %02x: ",
		cdr.Stat != NoIntr && cdr.Reg2 != 0x18, Irq);
	for (i = 0; i < cdr.ResultC; i++)
		SysPrintf("%02x ", cdr.Result[i]);
	SysPrintf("\n");
#endif
}

void cdrReadInterrupt() {
	u8 *buf;

	if (!cdr.Reading)
		return;

	if (cdr.Irq || cdr.Stat) {
		CDR_LOG_I("cdrom: read stat hack %02x %x\n", cdr.Irq, cdr.Stat);
		CDREAD_INT(0x1000);
		return;
	}

	cdr.OCUP = 1;
	SetResultSize(1);
	cdr.StatP |= STATUS_READ|STATUS_ROTATING;
	cdr.StatP &= ~STATUS_SEEK;
	cdr.Result[0] = cdr.StatP;
	cdr.Seeked = SEEK_DONE;

	ReadTrack( cdr.SetSector );

	buf = CDR_getBuffer();
	if (buf == NULL)
		cdr.RErr = -1;

	if (cdr.RErr == -1) {
		CDR_LOG_I("cdrReadInterrupt() Log: err\n");
		memset(cdr.Transfer, 0, DATA_SIZE);
		cdr.Stat = DiskError;
		cdr.Result[0] |= STATUS_ERROR;
		CDREAD_INT((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime);
		return;
	}

	memcpy(cdr.Transfer, buf, DATA_SIZE);
	CheckPPFCache(cdr.Transfer, cdr.Prev[0], cdr.Prev[1], cdr.Prev[2]);


	CDR_LOG("cdrReadInterrupt() Log: cdr.Transfer %x:%x:%x\n", cdr.Transfer[0], cdr.Transfer[1], cdr.Transfer[2]);

	if ((!cdr.Muted) && (cdr.Mode & MODE_STRSND) && (!Config.Xa) && (cdr.FirstSector != -1)) { // CD-XA
		// Firemen 2: Multi-XA files - briefings, cutscenes
		if( cdr.FirstSector == 1 && (cdr.Mode & MODE_SF)==0 ) {
			cdr.File = cdr.Transfer[4 + 0];
			cdr.Channel = cdr.Transfer[4 + 1];
		}

		if ((cdr.Transfer[4 + 2] & 0x4) &&
			 (cdr.Transfer[4 + 1] == cdr.Channel) &&
			(cdr.Transfer[4 + 0] == cdr.File)) {
			int ret = xa_decode_sector(&cdr.Xa, cdr.Transfer+4, cdr.FirstSector);

			if (!ret) {
				// only handle attenuator basic channel switch for now
				if (cdr.Xa.stereo) {
					int i;
					if ((cdr.AttenuatorLeft[0] | cdr.AttenuatorLeft[1])
					    && !(cdr.AttenuatorRight[0] | cdr.AttenuatorRight[1]))
					{
						for (i = 0; i < cdr.Xa.nsamples; i++)
							cdr.Xa.pcm[i*2 + 1] = cdr.Xa.pcm[i*2];
					}
					else if (!(cdr.AttenuatorLeft[0] | cdr.AttenuatorLeft[1])
					    && (cdr.AttenuatorRight[0] | cdr.AttenuatorRight[1]))
					{
						for (i = 0; i < cdr.Xa.nsamples; i++)
							cdr.Xa.pcm[i*2] = cdr.Xa.pcm[i*2 + 1];
					}
				}

				SPU_playADPCMchannel(&cdr.Xa);
				cdr.FirstSector = 0;

#if 0
				// Crash Team Racing: music, speech
				// - done using cdda decoded buffer (spu irq)
				// - don't do here

				// signal ADPCM data ready
				setIrq();
#endif
			}
			else cdr.FirstSector = -1;
		}
	}

	cdr.SetSector[2]++;
	if (cdr.SetSector[2] == 75) {
		cdr.SetSector[2] = 0;
		cdr.SetSector[1]++;
		if (cdr.SetSector[1] == 60) {
			cdr.SetSector[1] = 0;
			cdr.SetSector[0]++;
		}
	}

	cdr.Readed = 0;

	CDREAD_INT((cdr.Mode & MODE_SPEED) ? (cdReadTime / 2) : cdReadTime);

	/*
	Croc 2: $40 - only FORM1 (*)
	Judge Dredd: $C8 - only FORM1 (*)
	Sim Theme Park - no adpcm at all (zero)
	*/

	if (!(cdr.Mode & MODE_STRSND) || !(cdr.Transfer[4+2] & 0x4)) {
		cdr.Stat = DataReady;
		setIrq();
	}

	Check_Shell(0);
}

/*
cdrRead0:
	bit 0,1 - mode
	bit 2 - unknown
	bit 3 - unknown
	bit 4 - unknown
	bit 5 - 1 result ready
	bit 6 - 1 dma ready
	bit 7 - 1 command being processed
*/

unsigned char cdrRead0(void) {
	if (cdr.ResultReady)
		cdr.Ctrl |= 0x20;
	else
		cdr.Ctrl &= ~0x20;

	if (cdr.OCUP)
		cdr.Ctrl |= 0x40;
//  else
//		cdr.Ctrl &= ~0x40;

	// What means the 0x10 and the 0x08 bits? I only saw it used by the bios
	cdr.Ctrl |= 0x18;

	CDR_LOG_IO("cdr r0: %02x\n", cdr.Ctrl);

	return psxHu8(0x1800) = cdr.Ctrl;
}

void cdrWrite0(unsigned char rt) {
	CDR_LOG_IO("cdr w0: %02x\n", rt);

	cdr.Ctrl = (rt & 3) | (cdr.Ctrl & ~3);
}

unsigned char cdrRead1(void) {
	if ((cdr.ResultP & 0xf) < cdr.ResultC)
		psxHu8(0x1801) = cdr.Result[cdr.ResultP & 0xf];
	else
		psxHu8(0x1801) = 0;
	cdr.ResultP++;
	if (cdr.ResultP == cdr.ResultC)
		cdr.ResultReady = 0;

	CDR_LOG_IO("cdr r1: %02x\n", psxHu8(0x1801));

	return psxHu8(0x1801);
}

void cdrWrite1(unsigned char rt) {
	u8 set_loc[3];
	int i;

	CDR_LOG_IO("cdr w1: %02x\n", rt);

	switch (cdr.Ctrl & 3) {
	case 0:
		break;
	case 3:
		cdr.AttenuatorRight[1] = rt;
		return;
	default:
		return;
	}

	cdr.Cmd = rt;
	cdr.OCUP = 0;

#ifdef CDR_LOG_CMD_IRQ
	SysPrintf("cdrWrite1() Log: CD1 write: %x (%s)", rt, CmdName[rt]);
	if (cdr.ParamC) {
		SysPrintf(" Param[%d] = {", cdr.ParamC);
		for (i = 0; i < cdr.ParamC; i++)
			SysPrintf(" %x,", cdr.Param[i]);
		SysPrintf("}\n");
	} else {
		SysPrintf("\n");
	}
#endif

	cdr.ResultReady = 0;
	cdr.Ctrl |= 0x80;
	// cdr.Stat = NoIntr; 
	AddIrqQueue(cdr.Cmd, 0x800);

	switch (cdr.Cmd) {
	case CdlSync:
	case CdlNop:
	case CdlForward:
	case CdlBackward:
	case CdlReadT:
	case CdlTest:
	case CdlID:
	case CdlReadToc:
	case CdlGetmode:
	case CdlGetlocL:
	case CdlGetlocP:
	case CdlGetTD:
		break;

	case CdlSetloc:
		StopReading();
		for (i = 0; i < 3; i++)
			set_loc[i] = btoi(cdr.Param[i]);

		i = abs(msf2sec(cdr.SetSector) - msf2sec(set_loc));
		if (i > 16)
			cdr.Seeked = SEEK_PENDING;

		memcpy(cdr.SetSector, set_loc, 3);
		cdr.SetSector[3] = 0;
		break;

	case CdlPlay:
		// Vib Ribbon: try same track again
		StopCdda();

		if (!cdr.SetSector[0] & !cdr.SetSector[1] & !cdr.SetSector[2]) {
			if (CDR_getTN(cdr.ResultTN) != -1) {
				if (cdr.CurTrack > cdr.ResultTN[1])
					cdr.CurTrack = cdr.ResultTN[1];
				if (CDR_getTD((unsigned char)(cdr.CurTrack), cdr.ResultTD) != -1) {
					int tmp = cdr.ResultTD[2];
					cdr.ResultTD[2] = cdr.ResultTD[0];
					cdr.ResultTD[0] = tmp;
					if (!Config.Cdda) CDR_play(cdr.ResultTD);
				}
			}
		} else if (!Config.Cdda) {
			CDR_play(cdr.SetSector);
		}

		// Vib Ribbon - decoded buffer IRQ for CDDA reading
		// - fixes ribbon timing + music CD mode
		//TODO?
		//CDRDBUF_INT( PSXCLK / 44100 * 0x100 );

		cdr.Play = TRUE;

		cdr.StatP |= STATUS_SEEK;
		cdr.StatP &= ~STATUS_ROTATING;
		break;

	case CdlReadN:
		StopReading();
		cdr.Reading = 1;
		cdr.FirstSector = 1;
		cdr.Readed = 0xff;
		break;

	case CdlStandby:
		StopCdda();
		StopReading();
		break;

	case CdlStop:
		// GameShark CD Player: Reset CDDA to track start
		if( cdr.Play && CDR_getStatus(&stat) != -1 ) {
			cdr.SetSectorPlay[0] = stat.Time[0];
			cdr.SetSectorPlay[1] = stat.Time[1];
			cdr.SetSectorPlay[2] = stat.Time[2];

			Find_CurTrack();

			// grab time for current track
			CDR_getTD((u8)(cdr.CurTrack), cdr.ResultTD);

			cdr.SetSectorPlay[0] = cdr.ResultTD[2];
			cdr.SetSectorPlay[1] = cdr.ResultTD[1];
			cdr.SetSectorPlay[2] = cdr.ResultTD[0];
		}

		StopCdda();
		StopReading();
		break;

	case CdlPause:
		/*
		   GameShark CD Player: save time for resume

		   Twisted Metal - World Tour: don't mix Setloc / CdlPlay cursors
		*/

		StopCdda();
		StopReading();
		break;

	case CdlReset:
	case CdlInit:
		cdr.Seeked = SEEK_DONE;
		StopCdda();
		StopReading();
		break;

	case CdlMute:
		cdr.Muted = TRUE;
			// Duke Nukem - Time to Kill
			// - do not directly set cd-xa volume
			//SPU_writeRegister( H_CDLeft, 0x0000 );
			//SPU_writeRegister( H_CDRight, 0x0000 );
		break;

	case CdlDemute:
		cdr.Muted = FALSE;

			// Duke Nukem - Time to Kill
			// - do not directly set cd-xa volume
			//SPU_writeRegister( H_CDLeft, 0x7f00 );
			//SPU_writeRegister( H_CDRight, 0x7f00 );
		break;

    	case CdlSetfilter:
        	cdr.File = cdr.Param[0];
        	cdr.Channel = cdr.Param[1];
        	break;

    	case CdlSetmode:
		CDR_LOG("cdrWrite1() Log: Setmode %x\n", cdr.Param[0]);

        	cdr.Mode = cdr.Param[0];

		// Squaresoft on PlayStation 1998 Collector's CD Vol. 1
		// - fixes choppy movie sound
		if( cdr.Play && (cdr.Mode & MODE_CDDA) == 0 )
			StopCdda();
        	break;

    	case CdlGetTN:
		//AddIrqQueue(cdr.Cmd, 0x800);

		// GameShark CDX CD Player: very long time
		AddIrqQueue(cdr.Cmd, 0x100000);
		break;

    	case CdlSeekL:
    	case CdlSeekP:
		// Tomb Raider 2 - reset cdda
		StopCdda();
		StopReading();
		break;

    	case CdlReadS:
		StopReading();
		cdr.Reading = 2;
		cdr.FirstSector = 1;
		cdr.Readed = 0xff;
		break;

	default:
		cdr.ParamC = 0;
		CDR_LOG_I("cdrWrite1() Log: Unknown command: %x\n", cdr.Cmd);
		return;
	}
}

unsigned char cdrRead2(void) {
	unsigned char ret;

	if (cdr.Readed == 0) {
		ret = 0;
	} else {
		ret = *pTransfer++;
	}

	CDR_LOG_IO("cdr r2: %02x\n", ret);
	return ret;
}

void cdrWrite2(unsigned char rt) {
	CDR_LOG_IO("cdr w2: %02x\n", rt);

	switch (cdr.Ctrl & 3) {
	case 0:
		if (cdr.ParamC < 8) // FIXME: size and wrapping
			cdr.Param[cdr.ParamC++] = rt;
		return;
	case 1:
		cdr.Reg2 = rt;
		setIrq();
		return;
	case 2:
		cdr.AttenuatorLeft[0] = rt;
		return;
	case 3:
		cdr.AttenuatorRight[0] = rt;
		return;
	}
}

unsigned char cdrRead3(void) {
	if (cdr.Ctrl & 0x1)
		psxHu8(0x1803) = cdr.Stat | 0xE0;
	else
		psxHu8(0x1803) = cdr.Reg2 | 0xE0;

	CDR_LOG_IO("cdr r3: %02x\n", psxHu8(0x1803));
	return psxHu8(0x1803);
}

void cdrWrite3(unsigned char rt) {
	CDR_LOG_IO("cdr w3: %02x\n", rt);

	switch (cdr.Ctrl & 3) {
	case 0:
		break; // transfer
	case 1:
		cdr.Stat &= ~rt;

		if (rt & 0x40)
			cdr.ParamC = 0;
		return;
	case 2:
		cdr.AttenuatorLeft[1] = rt;
		return;
	case 3:
		if (rt == 0x20)
		CDR_LOG( "CD-XA Volume: %X %X | %X %X\n",
			cdr.AttenuatorLeft[0], cdr.AttenuatorLeft[1],
			cdr.AttenuatorRight[0], cdr.AttenuatorRight[1] );
		return;
	}

	if ((rt & 0x80) && cdr.Readed == 0) {
		cdr.Readed = 1;
		pTransfer = cdr.Transfer;

		switch (cdr.Mode & 0x30) {
			case MODE_SIZE_2328:
			case 0x00:
				pTransfer += 12;
				break;

			case MODE_SIZE_2340:
				pTransfer += 0;
				break;

			default:
				break;
		}
	}
}

void psxDma3(u32 madr, u32 bcr, u32 chcr) {
	u32 cdsize;
	int size;
	u8 *ptr;

	CDR_LOG("psxDma3() Log: *** DMA 3 *** %x addr = %x size = %x\n", chcr, madr, bcr);

	switch (chcr) {
		case 0x11000000:
		case 0x11400100:
			if (cdr.Readed == 0) {
				CDR_LOG("psxDma3() Log: *** DMA 3 *** NOT READY\n");
				break;
			}

			cdsize = (bcr & 0xffff) * 4;

			// Ape Escape: bcr = 0001 / 0000
			// - fix boot
			if( cdsize == 0 )
			{
				switch (cdr.Mode & 0x30) {
					case 0x00: cdsize = 2048; break;
					case MODE_SIZE_2328: cdsize = 2328; break;
					case MODE_SIZE_2340: cdsize = 2340; break;
				}
			}


			ptr = (u8 *)PSXM(madr);
			if (ptr == NULL) {
				CDR_LOG("psxDma3() Log: *** DMA 3 *** NULL Pointer!\n");
				break;
			}

			/*
			GS CDX: Enhancement CD crash
			- Setloc 0:0:0
			- CdlPlay
			- Spams DMA3 and gets buffer overrun
			*/
			size = CD_FRAMESIZE_RAW - (pTransfer - cdr.Transfer);
			if (size > cdsize)
				size = cdsize;
			if (size > 0)
			{
				memcpy(ptr, pTransfer, size);
			}

			psxCpu->Clear(madr, cdsize / 4);
			pTransfer += cdsize;


			// burst vs normal
			if( chcr == 0x11400100 ) {
				CDRDMA_INT( (cdsize/4) / 4 );
			}
			else if( chcr == 0x11000000 ) {
				CDRDMA_INT( (cdsize/4) * 1 );
			}
			return;

		default:
			CDR_LOG("psxDma3() Log: Unknown cddma %x\n", chcr);
			break;
	}

	HW_DMA3_CHCR &= SWAP32(~0x01000000);
	DMA_INTERRUPT(3);
}

void cdrDmaInterrupt()
{
	if (HW_DMA3_CHCR & SWAP32(0x01000000))
	{
		HW_DMA3_CHCR &= SWAP32(~0x01000000);
		DMA_INTERRUPT(3);
	}
}

void cdrReset() {
	memset(&cdr, 0, sizeof(cdr));
	cdr.CurTrack = 1;
	cdr.File = 1;
	cdr.Channel = 1;
	pTransfer = cdr.Transfer;

	// BIOS player - default values
	cdr.AttenuatorLeft[0] = 0x80;
	cdr.AttenuatorLeft[1] = 0x00;
	cdr.AttenuatorRight[0] = 0x00;
	cdr.AttenuatorRight[1] = 0x80;
}

int cdrFreeze(void *f, int Mode) {
	u32 tmp;

	if( Mode == 0 ) {
		StopCdda();
	}
	
	gzfreeze(&cdr, sizeof(cdr));
	
	if (Mode == 1) {
		cdr.ParamP = cdr.ParamC;
		tmp = pTransfer - cdr.Transfer;
	}

	gzfreeze(&tmp, sizeof(tmp));

	if (Mode == 0) {
		pTransfer = cdr.Transfer + tmp;

		if (cdr.Play && !Config.Cdda)
			CDR_play(cdr.SetSectorPlay);
	}

	return 0;
}

void LidInterrupt() {
	cdr.LidCheck = 0x20; // start checker

	CDRLID_INT( cdReadTime * 3 );

	// generate interrupt if none active - open or close
	if (cdr.Irq == 0 || cdr.Irq == 0xff) {
		cdr.Ctrl |= 0x80;
		cdr.Stat = NoIntr;
		AddIrqQueue(CdlNop, 0x800);
	}
}
