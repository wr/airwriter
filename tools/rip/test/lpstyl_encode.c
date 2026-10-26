// SPDX-License-Identifier: BSD-2-Clause
//
// The scanline coder of lpstyl 0.9.9 (https://github.com/Godzil/lpstyl, lpstyl.c:
// appendEncode and encodescanline), a printer driver for Apple StyleWriters. Used by
// the host tests only (tools/rip/test/swcheck.py), never in the firmware: its output
// is decoded by swfake.py and must give the same dots as ours. Changed from the
// original: types (fixed width in place of unsigned char and long), const
// arguments, the padding width as a parameter in place of the PRINT_ROWBYTES
// constant, and the constants' names. The logic is lpstyl's.
//
// lpstyl.c:
//
//	Copyright (C) 1996-2000 Monroe Williams (monroe@pobox.com)
//	All rights reserved.
//
//	Redistribution and use in source and binary forms, with or without
//	modification, are permitted provided that the following conditions
//	are met:
//	1. Redistributions of source code must retain the above copyright
//		notice, this list of conditions and the following disclaimer.
//	2. Redistributions in binary form must reproduce the above copyright
//		notice, this list of conditions and the following disclaimer in the
//		documentation and/or other materials provided with the distribution.
//
//	THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
//	IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
//	OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
//	IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
//	INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
//	NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//	DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//	THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//	(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//	OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include "lpstyl_encode.h"

#define MAX_RUN 0x3E
#define MAX_BLOCK 0x3E
#define RUN_THRESH 0x01
#define DATA_WHITE 0x00
#define DATA_BLACK 0xFF
#define DATA_OTHER 0x0A
#define MASK_RUNWHT 0x80
#define MASK_RUNBLK 0xC0
#define MASK_RUNDATA 0x00

size_t lpstyl_encodescanline(const uint8_t *src, size_t srcLen, size_t padTo, uint8_t *dst)
{
	long s, d, i, runStart, runLen;
	uint8_t runChar;
	long len = (long)srcLen;

	/* SPECIAL CASE: check for a blank line */
	for(i=0;i < len;i++)
		if(src[i] != DATA_WHITE)
			break;

	if(i == len)
	{
		*dst = MASK_RUNWHT;
		return(1);
	}

	s = d = 0;
	while(s < len)
	{
		runStart = runLen = 0;
		runChar = DATA_OTHER;	/* Just so long as it's not black or white... */

		/* Find the first run */
		for(i = s; (i < len); i++)
		{
			if((runChar == DATA_WHITE) || (runChar == DATA_BLACK))
			{
				if(src[i] != runChar)
				{	/* This run is over */
					if(i - runStart >= RUN_THRESH)
					{	/* The run was long enough to count.  Break out. */
						break;
					}
					else
					{	/* The run was too short to count. */
						runChar = DATA_OTHER;
					}
				}
				else
				{
					if(i - runStart >= MAX_RUN)
					{	/* This is enough of a run to encode. */
						break;
					}
				}
			}
			else
			{	/* runChar == DATA_OTHER */
				if((src[i] == DATA_WHITE) || (src[i] == DATA_BLACK))
				{	/* Start a run */
					runChar = src[i];
					runStart = i;
				}
				else if(i - s >= MAX_BLOCK)
				{	/* This block is the maximum length. */
					break;
				}
			}
		}	/* end for */

		if(runChar != DATA_OTHER)
		{
			/* We were in a run when we broke out. */
			runLen = i - runStart;
		}
		else
		{
			runStart = i;
			runLen = 0;
		}

		if(runStart != s)
		{
			/* Encode a run of random data */
			dst[d++] = (uint8_t)(runStart - s);
			for(;s < runStart;)
				dst[d++] = src[s++];
		}

		if(runLen > 0)
		{
			/* Encode a run of black or white */
			if(runChar == DATA_BLACK)
				dst[d++] = (uint8_t)(MASK_RUNBLK + runLen);
			else
			{
				if(s + runLen < len)
					dst[d++] = (uint8_t)(MASK_RUNWHT + runLen);
				else
					break;	/* Let this be taken up with the padding. */
			}
			s += runLen;
		}


	}	/* end while */

	/* pad out to the width of the page with white */
	for(;s < (long)padTo;)
	{
		runLen = (long)padTo - s;
		if(runLen > MAX_RUN)
			runLen = MAX_RUN;
		dst[d++] = (uint8_t)(MASK_RUNWHT + runLen);
		s += runLen;
	}
	return((size_t)d);
}

size_t lpstyl_appendEncode(size_t length, const uint8_t *in, const uint8_t *last, uint8_t *out)
{
	static uint8_t delta[1024];	/* bigger than we'll ever need */
	size_t result = 0;

	if(length > sizeof delta)
		return(0);

	if(last)
	{
		/* XOR the input with the last scanline */
		const uint8_t *src1 = in, *src2 = last;
		uint8_t *dst = delta;
		size_t size = length;

		for(;size > 0; size--)
			*dst++ = (*src1++) ^ (*src2++);

		/* encode the difference into the buffer */
		result = lpstyl_encodescanline(delta, length, length, out);
	}
	else
	{
		/* encode the scanline into the buffer */
		result = lpstyl_encodescanline(in, length, length, out);
	}

	return(result);
}
