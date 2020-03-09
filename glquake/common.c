/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// common.c -- misc functions used in client and server

#include "quakedef.h"

#define NUM_SAFE_ARGVS  7

static char     *largv[MAX_NUM_ARGVS + NUM_SAFE_ARGVS + 1];
static char     *argvdummy = " ";

static char     *safeargvs[NUM_SAFE_ARGVS] =
	{"-stdvid", "-nolan", "-nosound", "-nocdaudio", "-nojoy", "-nomouse", "-dibonly"};

cvar_t  registered = {"registered","0"};
cvar_t  cmdline = {"cmdline","0", false, true};

qboolean        com_modified;   // set true if using non-id files

qboolean		proghack;

int             static_registered = 1;  // only for startup check, then set

qboolean		msg_suppress_1 = 0;

void COM_InitFilesystem (void);

// if a packfile directory differs from this, it is assumed to be hacked
#define PAK0_COUNT              339
#define PAK0_CRC                32981

char	com_token[1024];
int		com_argc;
char	**com_argv;

#define CMDLINE_LENGTH	256
char	com_cmdline[CMDLINE_LENGTH];

qboolean		standard_quake = true, rogue, hipnotic, nehahra;

// this graphic needs to be in the pak file to use registered features
unsigned short pop[] =
{
 0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000
,0x0000,0x0000,0x6600,0x0000,0x0000,0x0000,0x6600,0x0000
,0x0000,0x0066,0x0000,0x0000,0x0000,0x0000,0x0067,0x0000
,0x0000,0x6665,0x0000,0x0000,0x0000,0x0000,0x0065,0x6600
,0x0063,0x6561,0x0000,0x0000,0x0000,0x0000,0x0061,0x6563
,0x0064,0x6561,0x0000,0x0000,0x0000,0x0000,0x0061,0x6564
,0x0064,0x6564,0x0000,0x6469,0x6969,0x6400,0x0064,0x6564
,0x0063,0x6568,0x6200,0x0064,0x6864,0x0000,0x6268,0x6563
,0x0000,0x6567,0x6963,0x0064,0x6764,0x0063,0x6967,0x6500
,0x0000,0x6266,0x6769,0x6a68,0x6768,0x6a69,0x6766,0x6200
,0x0000,0x0062,0x6566,0x6666,0x6666,0x6666,0x6562,0x0000
,0x0000,0x0000,0x0062,0x6364,0x6664,0x6362,0x0000,0x0000
,0x0000,0x0000,0x0000,0x0062,0x6662,0x0000,0x0000,0x0000
,0x0000,0x0000,0x0000,0x0061,0x6661,0x0000,0x0000,0x0000
,0x0000,0x0000,0x0000,0x0000,0x6500,0x0000,0x0000,0x0000
,0x0000,0x0000,0x0000,0x0000,0x6400,0x0000,0x0000,0x0000
};

/*


All of Quake's data access is through a hierchal file system, but the contents of the file system can be transparently merged from several sources.

The "base directory" is the path to the directory holding the quake.exe and all game directories.  The sys_* files pass this to host_init in quakeparms_t->basedir.  This can be overridden with the "-basedir" command line parm to allow code debugging in a different directory.  The base directory is
only used during filesystem initialization.

The "game directory" is the first tree on the search path and directory that all generated files (savegames, screenshots, demos, config files) will be saved to.  This can be overridden with the "-game" command line parameter.  The game directory can never be changed while quake is executing.  This is a precacution against having a malicious server instruct clients to write files over areas they shouldn't.

The "cache directory" is only used during development to save network bandwidth, especially over ISDN / T1 lines.  If there is a cache directory
specified, when a file is found by the normal search path, it will be mirrored
into the cache directory, then opened there.



FIXME:
The file "parms.txt" will be read out of the game directory and appended to the current command line arguments to allow different games to initialize startup parms differently.  This could be used to add a "-sspeed 22050" for the high quality sound edition.  Because they are added at the end, they will not override an explicit setting on the original command line.

*/

//============================================================================

qboolean InvalidPtr (long Ptr)
{
	if ((Ptr & 0xF0000000) != 0 || (Ptr & 0x0F000000) == 0)
		return true;

	return false;
}

qboolean IsTimeout (float *PrevTime, float WaitTime)
{
	float CurrTime;

	CurrTime = Sys_FloatTime ();

	if (*PrevTime && CurrTime - *PrevTime < WaitTime)
		return false;

	*PrevTime = CurrTime;

	return true;
}

/*
===============
COM_AllocBuf
Reallocates memory as needed. Use persistent variables for Buf and PSize, zero-initialized
===============
*/

#define MIN_ALLOC (512 * 1024) // Don't reallocate too often

void *COM_AllocBuf (char *Func, void *Buf, int *PSize, int NewSize, int OldLimit, char *Name)
{
//	static int TotCalls = 0, AllocCalls = 0;

//	++TotCalls;

	// Sanity ...
	if (NewSize & 0xC0000000)
		Sys_Error ("%s: invalid size for '%s': %dM", Func, Name, NewSize / (1024 * 1024));

	if (OldLimit && NewSize > OldLimit)
	{
		Con_SafePrintf ("\x02%s: ", Func);
		Con_SafePrintf ("excessive '%s' size %dk (normal max %dk)\n", Name, NewSize / 1024, OldLimit / 1024);
	}

	if (NewSize > *PSize || NewSize < *PSize / 2 && *PSize > MIN_ALLOC * 2)
	{
//		++AllocCalls;
//		Con_SafePrintf ("COM_AllocBuf: %s: %dk, Alloc:%d, Total:%d\n", Func, NewSize / 1024, AllocCalls, TotCalls);
		if (Buf)
			COM_FreeBuf (Buf);

		if (NewSize < MIN_ALLOC)
			NewSize = MIN_ALLOC;

		Buf = malloc (NewSize);

		if (!Buf)
			Sys_Error ("%s: not enough memory for '%s': %dk", Func, Name, NewSize / 1024);

		*PSize = NewSize;
	}

	return Buf;
}

/*
===============
COM_FreeBuf
Frees memory manually
===============
*/
void COM_FreeBuf (void *Buf)
{
	free (Buf);
}

/*
===============
COM_vsnprintf
===============
*/
int COM_vsnprintf (char *function, char *buffer, int buffersize, char *format, va_list argptr)
{
	int result = _vsnprintf (buffer, buffersize, format, argptr);

	buffer[buffersize] = 0;

	if (result < 0 || result >= buffersize)
	{
		static qboolean inside;

		// Beware recursion here
		if (!inside)
		{
			inside = true;
			Con_SafePrintf ("%s: excessive string length, max = %d\n", function, buffersize);
		}

		inside = false;
		return -1;
	}

	return result;
}

/*
===============
COM_snprintf
===============
*/
int COM_snprintf (char *function, char *buffer, int buffersize, char *format, ...)
{
	va_list argptr;
	int	result;

	va_start (argptr, format);
	result = COM_vsnprintf (function, buffer, buffersize, format, argptr);
	va_end (argptr);

	return result;
}

 // ClearLink is used for new headnodes
void ClearLink (link_t *l)
{
	l->prev = l->next = l;
}

void RemoveLink (link_t *l)
{
	l->next->prev = l->prev;
	l->prev->next = l->next;
}

void InsertLinkBefore (link_t *l, link_t *before)
{
	l->next = before;
	l->prev = before->prev;
	l->prev->next = l;
	l->next->prev = l;
}
void InsertLinkAfter (link_t *l, link_t *after)
{
	l->next = after->next;
	l->prev = after;
	l->prev->next = l;
	l->next->prev = l;
}

/*
============================================================================

					LIBRARY REPLACEMENT FUNCTIONS

============================================================================
*/

void Q_memset (void *dest, int fill, int count)
{
	int             i;

	if ( (((long)dest | count) & 3) == 0)
	{
		count >>= 2;
		fill = fill | (fill<<8) | (fill<<16) | (fill<<24);
		for (i=0 ; i<count ; i++)
			((int *)dest)[i] = fill;
	}
	else
		for (i=0 ; i<count ; i++)
			((byte *)dest)[i] = fill;
}

void Q_memcpy (void *dest, void *src, int count)
{
	int             i;

	if (( ( (long)dest | (long)src | count) & 3) == 0 )
	{
		count>>=2;
		for (i=0 ; i<count ; i++)
			((int *)dest)[i] = ((int *)src)[i];
	}
	else
		for (i=0 ; i<count ; i++)
			((byte *)dest)[i] = ((byte *)src)[i];
}

int Q_memcmp (void *m1, void *m2, int count)
{
	while(count)
	{
		count--;
		if (((byte *)m1)[count] != ((byte *)m2)[count])
			return -1;
	}
	return 0;
}

void Q_strcpy (char *dest, char *src)
{
	while (*src)
	{
		*dest++ = *src++;
	}
	*dest++ = 0;
}

void Q_strncpy (char *dest, char *src, int count)
{
	while (*src && count--)
	{
		*dest++ = *src++;
	}
	// NOTE: if src is longer than count, dest is terminated, different from std strncpy!
	if (count)
		*dest++ = 0;
}

int Q_strlen (char *str)
{
	int             count;

	count = 0;
	while (str[count])
		count++;

	return count;
}

char *Q_strrchr(char *s, char c)
{
    int len = Q_strlen(s);
    s += len;
    while (len--)
	if (*--s == c) return s;
    return 0;
}

void Q_strcat (char *dest, char *src)
{
	dest += Q_strlen(dest);
	Q_strcpy (dest, src);
}

int Q_strcmp (char *s1, char *s2)
{
	while (1)
	{
		if (*s1 != *s2)
			return -1;              // strings not equal
		if (!*s1)
			return 0;               // strings are equal
		s1++;
		s2++;
	}

	return -1;
}

int Q_strncmp (char *s1, char *s2, int count)
{
	while (1)
	{
		if (!count--)
			return 0;
		if (*s1 != *s2)
			return -1;              // strings not equal
		if (!*s1)
			return 0;               // strings are equal
		s1++;
		s2++;
	}

	return -1;
}

int Q_strncasecmp (char *s1, char *s2, int n)
{
	int             c1, c2;

	while (1)
	{
		c1 = *s1++;
		c2 = *s2++;

		if (!n--)
			return 0;               // strings are equal until end point

		if (c1 != c2)
		{
			if (c1 >= 'a' && c1 <= 'z')
				c1 -= ('a' - 'A');
			if (c2 >= 'a' && c2 <= 'z')
				c2 -= ('a' - 'A');
			if (c1 != c2)
				return -1;              // strings not equal
		}
		if (!c1)
			return 0;               // strings are equal
//              s1++;
//              s2++;
	}

	return -1;
}

int Q_strcasecmp (char *s1, char *s2)
{
	return Q_strncasecmp (s1, s2, 99999);
}

int Q_atoi (char *str)
{
	int             val;
	int             sign;
	int             c;

	if (*str == '-')
	{
		sign = -1;
		str++;
	}
	else
		sign = 1;

	val = 0;

//
// check for hex
//
	if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X') )
	{
		str += 2;
		while (1)
		{
			c = *str++;
			if (c >= '0' && c <= '9')
				val = (val<<4) + c - '0';
			else if (c >= 'a' && c <= 'f')
				val = (val<<4) + c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				val = (val<<4) + c - 'A' + 10;
			else
				return val*sign;
		}
	}

//
// check for character
//
	if (str[0] == '\'')
	{
		return sign * str[1];
	}

//
// assume decimal
//
	while (1)
	{
		c = *str++;
		if (c <'0' || c > '9')
			return val*sign;
		val = val*10 + c - '0';
	}

	return 0;
}


float Q_atof (char *str)
{
	double			val;
	int             sign;
	int             c;
	int             decimal, total;

	if (*str == '-')
	{
		sign = -1;
		str++;
	}
	else
		sign = 1;

	val = 0;

//
// check for hex
//
	if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X') )
	{
		str += 2;
		while (1)
		{
			c = *str++;
			if (c >= '0' && c <= '9')
				val = (val*16) + c - '0';
			else if (c >= 'a' && c <= 'f')
				val = (val*16) + c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				val = (val*16) + c - 'A' + 10;
			else
				return val*sign;
		}
	}

//
// check for character
//
	if (str[0] == '\'')
	{
		return sign * str[1];
	}

//
// assume decimal
//
	decimal = -1;
	total = 0;
	while (1)
	{
		c = *str++;
		if (c == '.')
		{
			decimal = total;
			continue;
		}
		if (c <'0' || c > '9')
			break;
		val = val*10 + c - '0';
		total++;
	}

	if (decimal == -1)
		return val*sign;
	while (total > decimal)
	{
		val /= 10;
		total--;
	}

	return val*sign;
}

/*
============================================================================

					BYTE ORDER FUNCTIONS

============================================================================
*/

qboolean        bigendien;

short   (*BigShort) (short l);
short   (*LittleShort) (short l);
int     (*BigLong) (int l);
int     (*LittleLong) (int l);
float   (*BigFloat) (float l);
float   (*LittleFloat) (float l);

short   ShortSwap (short l)
{
	byte    b1,b2;

	b1 = l&255;
	b2 = (l>>8)&255;

	return (b1<<8) + b2;
}

short   ShortNoSwap (short l)
{
	return l;
}

int    LongSwap (int l)
{
	byte    b1,b2,b3,b4;

	b1 = l&255;
	b2 = (l>>8)&255;
	b3 = (l>>16)&255;
	b4 = (l>>24)&255;

	return ((int)b1<<24) + ((int)b2<<16) + ((int)b3<<8) + b4;
}

int     LongNoSwap (int l)
{
	return l;
}

float FloatSwap (float f)
{
	union
	{
		float   f;
		byte    b[4];
	} dat1, dat2;


	dat1.f = f;
	dat2.b[0] = dat1.b[3];
	dat2.b[1] = dat1.b[2];
	dat2.b[2] = dat1.b[1];
	dat2.b[3] = dat1.b[0];
	return dat2.f;
}

float FloatNoSwap (float f)
{
	return f;
}

/*
==============================================================================

			MESSAGE IO FUNCTIONS

Handles byte ordering and avoids alignment errors
==============================================================================
*/

//
// writing functions
//

void MSG_WriteChar (sizebuf_t *sb, int c)
{
	byte    *buf;

#ifdef PARANOID
	if (c < -128 || c > 127)
		Sys_Error ("MSG_WriteChar: range error");
#endif

	buf = SZ_GetSpace (sb, 1);
	buf[0] = c;
}

void MSG_WriteByte (sizebuf_t *sb, int c)
{
	byte    *buf;

#ifdef PARANOID
	if (c < 0 || c > 255)
		Sys_Error ("MSG_WriteByte: range error");
#endif

	buf = SZ_GetSpace (sb, 1);
	buf[0] = c;
}

void MSG_WriteShort (sizebuf_t *sb, int c)
{
	byte    *buf;

#ifdef PARANOID
	if (c < ((short)0x8000) || c > (short)0x7fff)
		Sys_Error ("MSG_WriteShort: range error");
#endif

	buf = SZ_GetSpace (sb, 2);
	buf[0] = c&0xff;
	buf[1] = c>>8;
}

void MSG_WriteLong (sizebuf_t *sb, int c)
{
	byte    *buf;

	buf = SZ_GetSpace (sb, 4);
	buf[0] = c&0xff;
	buf[1] = (c>>8)&0xff;
	buf[2] = (c>>16)&0xff;
	buf[3] = c>>24;
}

void MSG_WriteFloat (sizebuf_t *sb, float f)
{
	union
	{
		float   f;
		int     l;
	} dat;


	dat.f = f;
	dat.l = LittleLong (dat.l);

	SZ_Write (sb, &dat.l, 4);
}

void MSG_WriteString (sizebuf_t *sb, char *s)
{
	if (!s)
		SZ_Write (sb, "", 1);
	else
		SZ_Write (sb, s, Q_strlen(s)+1);
}

void MSG_WriteCoord (sizebuf_t *sb, float f)
{
	MSG_WriteShort (sb, (int)(f*8));
}

void MSG_WriteAngle (sizebuf_t *sb, float f)
{
	MSG_WriteByte (sb, ((int)f*256/360) & 255);
}

//
// reading functions
//
int                     msg_readcount;
qboolean        msg_badread;

void MSG_BeginReading (void)
{
	msg_readcount = 0;
	msg_badread = false;
}

// returns -1 and sets msg_badread if no more characters are available
int MSG_ReadChar (void)
{
	int     c;

	if (msg_readcount+1 > net_message.cursize)
	{
		msg_badread = true;
		return -1;
	}

	c = (signed char)net_message.data[msg_readcount];
	msg_readcount++;

	return c;
}

int MSG_ReadByte (void)
{
	int     c;

	if (msg_readcount+1 > net_message.cursize)
	{
		msg_badread = true;
		return -1;
	}

	c = (unsigned char)net_message.data[msg_readcount];
	msg_readcount++;

	return c;
}

int MSG_ReadShort (void)
{
	int     c;

	if (msg_readcount+2 > net_message.cursize)
	{
		msg_badread = true;
		return -1;
	}

	c = (short)(net_message.data[msg_readcount]
	+ (net_message.data[msg_readcount+1]<<8));

	msg_readcount += 2;

	return c;
}

int MSG_ReadLong (void)
{
	int     c;

	if (msg_readcount+4 > net_message.cursize)
	{
		msg_badread = true;
		return -1;
	}

	c = net_message.data[msg_readcount]
	+ (net_message.data[msg_readcount+1]<<8)
	+ (net_message.data[msg_readcount+2]<<16)
	+ (net_message.data[msg_readcount+3]<<24);

	msg_readcount += 4;

	return c;
}

float MSG_ReadFloat (void)
{
	union
	{
		byte    b[4];
		float   f;
		int     l;
	} dat;

	dat.b[0] =      net_message.data[msg_readcount];
	dat.b[1] =      net_message.data[msg_readcount+1];
	dat.b[2] =      net_message.data[msg_readcount+2];
	dat.b[3] =      net_message.data[msg_readcount+3];
	msg_readcount += 4;

	dat.l = LittleLong (dat.l);

	return dat.f;
}

char *MSG_ReadString (void)
{
	static char     string[2048];
	int             l,c;

	l = 0;
	do
	{
		c = MSG_ReadChar ();

		if (c == -1 || c == 0)
			break;

		if (l < sizeof(string) - 1)
			string[l++] = c;
	}
	while (true); // Make sure to read entire string even if string is full

	string[l] = 0;

	return string;
}

float MSG_ReadCoord (void)
{
	return MSG_ReadShort() * (1.0/8);
}

float MSG_ReadAngle (void)
{
	return MSG_ReadChar() * (360.0/256);
}



//===========================================================================

void SZ_Alloc (sizebuf_t *buf, int startsize)
{
	if (startsize < 256)
		startsize = 256;
	buf->data = Hunk_AllocName (startsize, "sizebuf");
	buf->maxsize = startsize;
	buf->cursize = 0;
}


void SZ_Free (sizebuf_t *buf)
{
//      Z_Free (buf->data);
//      buf->data = NULL;
//      buf->maxsize = 0;
	buf->cursize = 0;
}

void SZ_Clear (sizebuf_t *buf)
{
	buf->cursize = 0;
}

void *SZ_GetSpace (sizebuf_t *buf, int length)
{
	void    *data;

	if (buf->cursize + length > buf->maxsize)
	{
		char *function = !buf->function ? "Unknown" : buf->function;

		if (!buf->allowoverflow)
			Sys_Error ("%s: SZ_GetSpace: overflow without allowoverflow set (%d, max = %d)", function, buf->cursize + length, buf->maxsize);

		if (length > buf->maxsize)
			Sys_Error ("%s: SZ_GetSpace: %i is > full buffer size %d", function, length, buf->maxsize);

		buf->overflowed = true;
		Con_Printf ("%s: SZ_GetSpace: overflow (%d, max = %d)\n", function, buf->cursize + length, buf->maxsize);
		SZ_Clear (buf);
	}

	data = buf->data + buf->cursize;
	buf->cursize += length;

	return data;
}

void SZ_Write (sizebuf_t *buf, void *data, int length)
{
	Q_memcpy (SZ_GetSpace(buf,length),data,length);
}

void SZ_Print (sizebuf_t *buf, char *data)
{
	int len;

	len = Q_strlen(data)+1;

// byte * cast to keep VC++ happy
	if (!buf->cursize || buf->data[buf->cursize-1])
		Q_memcpy ((byte *)SZ_GetSpace(buf, len),data,len); // no trailing 0
	else
		Q_memcpy ((byte *)SZ_GetSpace(buf, len-1)-1,data,len); // write over trailing 0
}


//============================================================================


/*
============
COM_SkipPath
============
*/
char *COM_SkipPath (char *pathname)
{
	char    *last;

	last = pathname;
	while (*pathname)
	{
		if (*pathname=='/')
			last = pathname+1;
		pathname++;
	}
	return last;
}

/*
============
COM_StripExtension
============
*/
void COM_StripExtension (char *in, char *out)
{
	while (*in && *in != '.')
		*out++ = *in++;
	*out = 0;
}

/*
============
COM_FileExtension
============
*/
char *COM_FileExtension (char *in)
{
	static char exten[8];
	int             i;

	while (*in && *in != '.')
		in++;
	if (!*in)
		return "";
	in++;
	for (i=0 ; i<7 && *in ; i++,in++)
		exten[i] = *in;
	exten[i] = 0;
	return exten;
}

/*
============
COM_FileBase
============
*/
void COM_FileBase (char *in, char *out)
{
	char *s, *s2;

	s = in + strlen(in) - 1;

	while (s != in && *s != '.')
		s--;

	for (s2 = s ; s2 != in && *s2 != '/' && *s2 != '\\'; s2--)
		;
	
	if (s-s2 < 2)
		strcpy (out,"?model?");
	else
	{
		if (*s2 == '/' || *s2 == '\\')
			++s2;
		if (s - s2 >= 32)
			s = s2 + 32 - 1;
		strncpy (out,s2, s-s2);
		out[s-s2] = 0;
	}
}


/*
==================
COM_DefaultExtension
==================
*/
void COM_DefaultExtension (char *path, char *extension)
{
	char    *src;
//
// if path doesn't have a .EXT, append extension
// (extension should include the .)
//
	src = path + strlen(path) - 1;

	while (*src != '/' && src != path)
	{
		if (*src == '.')
			return;                 // it has an extension
		src--;
	}

	strcat (path, extension);
}

static int IncLen (int len)
{
	if (++len >= sizeof(com_token))
		Sys_Error ("IncLen: com_token overflow (max = %d)", sizeof(com_token));

	return len;
}

/*
==============
COM_Parse

Parse a token out of a string
==============
*/
char *COM_Parse (char *data)
{
	int             c;
	int             len;

	len = 0;
	com_token[0] = 0;

	if (!data)
		return NULL;

// skip whitespace
skipwhite:
	while ( (c = *data) <= ' ')
	{
		if (c == 0)
			return NULL;                    // end of file;
		data++;
	}

// skip // comments
	if (c=='/' && data[1] == '/')
	{
		while (*data && *data != '\n')
			data++;
		goto skipwhite;
	}


// handle quoted strings specially
	if (c == '\"')
	{
		data++;
		while (1)
		{
			c = *data++;
			if (c=='\"' || !c)
			{
				com_token[len] = 0;
				return !c ? NULL : data; // NULL, otherwise data becomes out of bounds
			}
			com_token[len] = c;
			len = IncLen (len);
		}
	}

// parse single characters
	if (c=='{' || c=='}'|| c==')'|| c=='(' || c=='\'' || c==':')
	{
		com_token[len] = c;
		len = IncLen (len);
		com_token[len] = 0;
		return data+1;
	}

// parse a regular word
	do
	{
		com_token[len] = c;
		data++;
		len = IncLen (len);
		c = *data;
	if (c=='{' || c=='}'|| c==')'|| c=='(' || c=='\'' || c==':')
			break;
	} while (c>32);

	com_token[len] = 0;
	return data;
}


/*
================
COM_CheckParm

Returns the position (1 to argc-1) in the program's argument list
where the given parameter apears, or 0 if not present
================
*/
int COM_CheckParm (char *parm)
{
	int             i;

	for (i=1 ; i<com_argc ; i++)
	{
		if (!com_argv[i])
			continue;               // NEXTSTEP sometimes clears appkit vars.
		if (!Q_strcmp (parm,com_argv[i]))
			return i;
	}

	return 0;
}

/*
================
COM_CheckRegistered

Looks for the pop.txt file and verifies it.
Sets the "registered" cvar.
Immediately exits out if an alternate game was attempted to be started without
being registered.
================
*/
void COM_CheckRegistered (void)
{
	int             h;
	unsigned short  check[128];
	int                     i;

	COM_OpenFile("gfx/pop.lmp", &h);
	static_registered = 0;

	if (h == -1)
	{
#if WINDED
	Sys_Error ("This dedicated server requires a full registered copy of Quake");
#endif
		Con_Printf ("Playing shareware version.\n");
		if (com_modified)
			Sys_Error ("You must have the registered version to use modified games");
		return;
	}

	Sys_FileRead (h, check, sizeof(check));
	COM_CloseFile (h);

	for (i=0 ; i<128 ; i++)
		if (pop[i] != (unsigned short)BigShort (check[i]))
			Sys_Error ("Corrupted data file.");

	Cvar_Set ("cmdline", com_cmdline);
	Cvar_Set ("registered", "1");
	static_registered = 1;
	Con_Printf ("Playing registered version.\n");
}


void COM_Path_f (void);


/*
================
COM_InitArgv
================
*/
void COM_InitArgv (int argc, char **argv)
{
	qboolean        safe;
	int             i, j, n;

// reconstitute the command line for the cmdline externally visible cvar
	n = 0;

	for (j=0 ; (j<MAX_NUM_ARGVS) && (j< argc) ; j++)
	{
		i = 0;

		while ((n < (CMDLINE_LENGTH - 1)) && argv[j][i])
		{
			com_cmdline[n++] = argv[j][i++];
		}

		if (n < (CMDLINE_LENGTH - 1))
			com_cmdline[n++] = ' ';
		else
			break;
	}

	com_cmdline[n] = 0;

	safe = false;

	for (com_argc=0 ; (com_argc<MAX_NUM_ARGVS) && (com_argc < argc) ;
		 com_argc++)
	{
		largv[com_argc] = argv[com_argc];
		if (!Q_strcmp ("-safe", argv[com_argc]))
			safe = true;
	}

	if (safe)
	{
	// force all the safe-mode switches. Note that we reserved extra space in
	// case we need to add these, so we don't need an overflow check
		for (i=0 ; i<NUM_SAFE_ARGVS ; i++)
		{
			largv[com_argc] = safeargvs[i];
			com_argc++;
		}
	}

	largv[com_argc] = argvdummy;
	com_argv = largv;

	if (COM_CheckParm ("-rogue"))
	{
		rogue = true;
		standard_quake = false;
	}

	if (COM_CheckParm ("-hipnotic") || COM_CheckParm ("-quoth"))
	{
		hipnotic = true;
		standard_quake = false;
	}

	if (COM_CheckParm ("-nehahra"))
		nehahra = true;
}


/*
================
COM_Init
================
*/
void COM_Init (char *basedir)
{
	byte    swaptest[2] = {1,0};

// set the byte swapping variables in a portable manner
	if ( *(short *)swaptest == 1)
	{
		bigendien = false;
		BigShort = ShortSwap;
		LittleShort = ShortNoSwap;
		BigLong = LongSwap;
		LittleLong = LongNoSwap;
		BigFloat = FloatSwap;
		LittleFloat = FloatNoSwap;
	}
	else
	{
		bigendien = true;
		BigShort = ShortNoSwap;
		LittleShort = ShortSwap;
		BigLong = LongNoSwap;
		LittleLong = LongSwap;
		BigFloat = FloatNoSwap;
		LittleFloat = FloatSwap;
	}

	Cvar_RegisterVariable (&registered);
	Cvar_RegisterVariable (&cmdline);
	Cmd_AddCommand ("path", COM_Path_f);

	COM_InitFilesystem ();
	COM_CheckRegistered ();
}


/*
============
va

does a varargs printf into a temp buffer, so I don't need to have
varargs versions of all text functions.
FIXME: make this buffer size safe someday
============
*/
char    *va(char *format, ...)
{
	va_list     argptr;
	static char string[MAX_PRINTMSG]; //1024

	va_start (argptr, format);
	COM_vsnprintf ("va", string, sizeof(string) - 1, format, argptr);
	va_end (argptr);

	return string;
}


/// just for debugging
int     memsearch (byte *start, int count, int search)
{
	int             i;

	for (i=0 ; i<count ; i++)
		if (start[i] == search)
			return i;
	return -1;
}

/*
=============================================================================

QUAKE FILESYSTEM

=============================================================================
*/

int     com_filesize;


//
// in memory
//

#pragma pack(1)
typedef struct
{
	char    name[MAX_QPATH];
	int     filepos, filelen;
	byte	hash;
} packfile_t;
#pragma pack()

#define PAK_HASHSIZE 256

typedef struct pack_s
{
	char		filename[MAX_OSPATH];
	int             handle;
	int             numfiles;
	short		hashoffset[PAK_HASHSIZE + 1];
	packfile_t      *files;
} pack_t;

//
// on disk
//
typedef struct
{
	char    name[56];
	int             filepos, filelen;
} dpackfile_t;

typedef struct
{
	char    id[4];
	int             dirofs;
	int             dirlen;
} dpackheader_t;

#define MAX_FILES_IN_PACK       2048

char    com_cachedir[MAX_OSPATH];
char    com_gamedir[MAX_OSPATH];

typedef struct searchpath_s
{
	char    filename[MAX_OSPATH];
	pack_t  *pack;          // only one of filename / pack will be used
	struct searchpath_s *next;
} searchpath_t;

searchpath_t    *com_searchpaths;

static byte CalcHash (char *str)
{
	byte hash = 0;

        while (*str)
		hash += *str++;
	
	return hash;
}

/*
============
COM_Path_f

============
*/
void COM_Path_f (void)
{
	searchpath_t    *s;

	Con_Printf ("Current search path:\n");
	for (s=com_searchpaths ; s ; s=s->next)
	{
		if (s->pack)
		{
			Con_Printf ("%s (%i files)\n", s->pack->filename, s->pack->numfiles);
		}
		else
			Con_Printf ("%s\n", s->filename);
	}
}

/*
============
COM_WriteFile

The filename will be prefixed by the current game directory
============
*/
void COM_WriteFile (char *filename, void *data, int len)
{
	int             handle;
	char    name[MAX_OSPATH];

	sprintf (name, "%s/%s", com_gamedir, filename);

	handle = Sys_FileOpenWrite (name);
	if (handle == -1)
	{
		Sys_Printf ("COM_WriteFile: failed on %s\n", name);
		return;
	}

	Sys_Printf ("COM_WriteFile: %s\n", name);
	Sys_FileWrite (handle, data, len);
	Sys_FileClose (handle);
}


/*
============
COM_CreatePath

Only used for CopyFile
============
*/
void    COM_CreatePath (char *path)
{
	char    *ofs;

	for (ofs = path+1 ; *ofs ; ofs++)
	{
		if (*ofs == '/')
		{       // create the directory
			*ofs = 0;
			Sys_mkdir (path);
			*ofs = '/';
		}
	}
}


/*
===========
COM_CopyFile

Copies a file over from the net to the local cache, creating any directories
needed.  This is for the convenience of developers using ISDN from home.
===========
*/
void COM_CopyFile (char *netpath, char *cachepath)
{
	int             in, out;
	int             remaining, count;
	char    buf[4096];

	remaining = Sys_FileOpenRead (netpath, &in);
	COM_CreatePath (cachepath);     // create directories up to the cache file
	out = Sys_FileOpenWrite (cachepath);

	while (remaining)
	{
		if (remaining < sizeof(buf))
			count = remaining;
		else
			count = sizeof(buf);
		Sys_FileRead (in, buf, count);
		Sys_FileWrite (out, buf, count);
		remaining -= count;
	}

	Sys_FileClose (in);
	Sys_FileClose (out);
}

/*
===========
COM_FindFile

Finds the file in the search path.
Sets com_filesize and one of handle or file
===========
*/
int COM_FindFile (char *filename, int *handle, FILE **file)
{
	searchpath_t    *search;
	char            netpath[MAX_OSPATH];
	char            cachepath[MAX_OSPATH];
	pack_t          *pak;
	int             i;
	int             findtime, cachetime;
	byte		hash = CalcHash (filename);

	if (file && handle)
		Sys_Error ("COM_FindFile: both handle and file set");
	if (!file && !handle)
		Sys_Error ("COM_FindFile: neither handle or file set");

//
// search through the path, one element at a time
//
	search = com_searchpaths;
	if (proghack)
	{	// gross hack to use quake 1 progs with quake 2 maps
		if (!strcmp(filename, "progs.dat"))
			search = search->next;
	}

	for ( ; search ; search = search->next)
	{
	// is the element a pak file?
		if (search->pack)
		{
			int  start, stop;

		// look through all the pak file elements
			pak = search->pack;
                        start = pak->hashoffset[hash];
                        stop = pak->hashoffset[hash + 1];
			for (i=start ; i<stop ; i++)
				if (!strcmp (pak->files[i].name, filename))
				{       // found it!
					Sys_Printf ("PackFile: %s : %s\n",pak->filename, filename);
					if (developer.value > 2)
						Con_SafePrintf ("PackFile: %s : %s\n",pak->filename, filename);
					if (handle)
					{
						*handle = pak->handle;
						Sys_FileSeek (pak->handle, pak->files[i].filepos);
					}
					else
					{       // open a new file on the pakfile
						*file = fopen (pak->filename, "rb");
						if (*file)
							fseek (*file, pak->files[i].filepos, SEEK_SET);
					}
					com_filesize = pak->files[i].filelen;
					return com_filesize;
				}
		}
		else
		{
	// check a file in the directory tree
			if (!static_registered)
			{       // if not a registered version, don't ever go beyond base
				if ( strchr (filename, '/') || strchr (filename,'\\'))
					continue;
			}

			sprintf (netpath, "%s/%s",search->filename, filename);

			findtime = Sys_FileTime (netpath);
			if (findtime == -1)
				continue;

		// see if the file needs to be updated in the cache
			if (!com_cachedir[0])
				strcpy (cachepath, netpath);
			else
			{
#if defined(_WIN32)
				if ((strlen(netpath) < 2) || (netpath[1] != ':'))
					sprintf (cachepath,"%s%s", com_cachedir, netpath);
				else
					sprintf (cachepath,"%s%s", com_cachedir, netpath+2);
#else
				sprintf (cachepath,"%s%s", com_cachedir, netpath);
#endif

				cachetime = Sys_FileTime (cachepath);

				if (cachetime < findtime)
					COM_CopyFile (netpath, cachepath);
				strcpy (netpath, cachepath);
			}

			Sys_Printf ("FindFile: %s\n",netpath);
			if (developer.value > 2)
				Con_SafePrintf ("FindFile: %s\n",netpath);
			com_filesize = Sys_FileOpenRead (netpath, &i);
			if (handle)
				*handle = i;
			else
			{
				Sys_FileClose (i);
				*file = fopen (netpath, "rb");
			}
			return com_filesize;
		}

	}

	Sys_Printf ("FindFile: can't find %s\n", filename);

	if (handle)
		*handle = -1;
	else
		*file = NULL;
	com_filesize = -1;
	return -1;
}


/*
===========
COM_OpenFile

filename never has a leading slash, but may contain directory walks
returns a handle and a length
it may actually be inside a pak file
===========
*/
int COM_OpenFile (char *filename, int *handle)
{
	return COM_FindFile (filename, handle, NULL);
}

/*
===========
COM_FOpenFile

If the requested file is inside a packfile, a new FILE * will be opened
into the file.
===========
*/
int COM_FOpenFile (char *filename, FILE **file)
{
	return COM_FindFile (filename, NULL, file);
}

/*
============
COM_CloseFile

If it is a pak file handle, don't really close it
============
*/
void COM_CloseFile (int h)
{
	searchpath_t    *s;

	for (s = com_searchpaths ; s ; s=s->next)
		if (s->pack && s->pack->handle == h)
			return;

	Sys_FileClose (h);
}


/*
============
COM_LoadFile

Filename are reletive to the quake directory.
Allways appends a 0 byte.
============
*/
cache_user_t *loadcache;
byte    *loadbuf;
int             loadsize;
byte *COM_LoadFile (char *path, int usehunk)
{
	int     h;
	byte    *buf;
	char    base[32];
	int     len;

	buf = NULL;     // quiet compiler warning

// look for it in the filesystem or pack files
	len = COM_OpenFile (path, &h);
	if (h == -1)
		return NULL;

// extract the filename base name for hunk tag
	COM_FileBase (path, base);

	if (usehunk == 1)
		buf = Hunk_AllocName (len+1, base);
	else if (usehunk == 0)
		buf = Z_Malloc (len+1);
	else if (usehunk == 3)
		buf = Cache_Alloc (loadcache, len+1, base);
	else if (usehunk == 2 || usehunk == 4)
	{
		if (usehunk == 2 || len+1 > loadsize)
		{
			static byte *Buf = NULL;
			static int  BufSize = 0;

			if (!Buf)
				buf = Hunk_TempAlloc (len+1, base); // Do this until failure

			// If failure, use COM_AllocBuf onwards
			if (!buf || Buf)
				buf = Buf = COM_AllocBuf ("COM_LoadFile", Buf, &BufSize, len + 1, 0, path);
		}
		else
			buf = loadbuf;
	}
	else
		Sys_Error ("COM_LoadFile: bad usehunk %d", usehunk);

	if (!buf)
		Sys_Error ("COM_LoadFile: not enough space for %s (%dk)", path, (len + 1) / 1024);

	((byte *)buf)[len] = 0;

	if (!cls.demoplayback)
		Draw_BeginDisc ();

	if (Sys_FileRead (h, buf, len) != len)
		Sys_Error ("COM_LoadFile: error reading %s", path);

	COM_CloseFile (h);
	if (!cls.demoplayback)
		Draw_EndDisc ();

	return buf;
}

byte *COM_LoadHunkFile (char *path)
{
	return COM_LoadFile (path, 1);
}

byte *COM_LoadTempFile (char *path)
{
	return COM_LoadFile (path, 2);
}

void COM_LoadCacheFile (char *path, struct cache_user_s *cu)
{
	loadcache = cu;
	COM_LoadFile (path, 3);
}

// uses temp hunk if larger than bufsize
byte *COM_LoadStackFile (char *path, void *buffer, int bufsize)
{
	byte    *buf;

	loadbuf = (byte *)buffer;
	loadsize = bufsize;
	buf = COM_LoadFile (path, 4);

	return buf;
}

static int pakcomp (const void *arg1, const void *arg2)
{
	return ((packfile_t *)arg1)->hash - ((packfile_t *)arg2)->hash;
}

/*
=================
COM_LoadPackFile

Takes an explicit (not game tree related) path to a pak file.

Loads the header and directory, adding the files at the beginning
of the list so they override previous pack files.
=================
*/
pack_t *COM_LoadPackFile (char *packfile)
{
	dpackheader_t   header;
	int             i, filelen, dofs, dlen;
	packfile_t      *newfiles;
	int             numpackfiles;
	pack_t          *pack;
	int             packhandle;
	dpackfile_t     info[MAX_FILES_IN_PACK];
	unsigned short  crc;

	filelen = Sys_FileOpenRead (packfile, &packhandle);

	if (filelen == -1)
	{
//              Con_Printf ("Couldn't open %s\n", packfile);
		return NULL;
	}
	
	if (Sys_FileRead (packhandle, (void *)&header, sizeof(header)) != sizeof(header) ||
	    header.id[0] != 'P' || header.id[1] != 'A' || header.id[2] != 'C' || header.id[3] != 'K')
		Sys_Error ("Can't read header in packfile %s", packfile);

	dofs = header.dirofs = LittleLong (header.dirofs);
	dlen = header.dirlen = LittleLong (header.dirlen);

	if (dofs < sizeof(header) || dlen < sizeof(dpackfile_t) || dofs + dlen > filelen)
		Sys_Error ("Directory is outside range (%d-%d, pak = %d-%d) in packfile %s", dofs, dofs + dlen - 1, sizeof(header), filelen - 1, packfile);

	numpackfiles = header.dirlen / sizeof(dpackfile_t);

	if (numpackfiles > MAX_FILES_IN_PACK)
		Sys_Error ("Packfile %s has too many files (%i, max = %i)", packfile, numpackfiles, MAX_FILES_IN_PACK);

	if (numpackfiles != PAK0_COUNT)
		com_modified = true;    // not the original file

	newfiles = Hunk_AllocName (numpackfiles * sizeof(packfile_t), "packfile");

	Sys_FileSeek (packhandle, header.dirofs);
	
	if (Sys_FileRead (packhandle, (void *)info, header.dirlen) != header.dirlen)
		Sys_Error ("Can't read directory in packfile %s", packfile);

// crc the directory to check for modifications
	CRC_Init (&crc);
	for (i=0 ; i<header.dirlen ; i++)
		CRC_ProcessByte (&crc, ((byte *)info)[i]);
	if (crc != PAK0_CRC)
		com_modified = true;

// parse the directory
	for (i=0 ; i<numpackfiles ; i++)
	{
		int ofs = newfiles[i].filepos = LittleLong(info[i].filepos);
		int len = newfiles[i].filelen = LittleLong(info[i].filelen);
		
		info[i].name[sizeof(info[i].name) - 1] = 0; // Make sure it's terminated
		strcpy (newfiles[i].name, info[i].name);
		
		// In pak0 of chapters_gom, a dir entry is (0,0), so allow 0 sized bad entries
		if (ofs < 0 || ofs >= filelen || len < 0 || len > filelen - (sizeof(header) + dlen) ||
		    len > 0 &&						// Entry has a size
		    (ofs < sizeof(header) ||				// Entry start is inside header
		     ofs >= dofs && ofs < dofs + dlen ||		// Entry start is inside directory
		     ofs + len > dofs && ofs + len <= dofs + dlen ||	// Entry end is inside directory
		     ofs < dofs && ofs + len > dofs + dlen))		// Directory is inside entry
			Sys_Error ("File %s is outside range (%d-%d, pak = %d-%d, dir = %d-%d) in packfile %s",
				   newfiles[i].name, ofs, ofs + len - 1, sizeof(header), filelen - 1, dofs, dofs + dlen - 1, packfile);
	}

	pack = Hunk_Alloc (sizeof (pack_t));
	strcpy (pack->filename, packfile);
	pack->handle = packhandle;
	pack->numfiles = numpackfiles;
	pack->files = newfiles;
	
	// Generate hash offsets to speed file searches
	for (i = 0; i < numpackfiles; ++i)
		newfiles[i].hash = CalcHash (newfiles[i].name);

	// Sort in ascending order
	qsort (newfiles, numpackfiles, sizeof(packfile_t), pakcomp);

	for (i = 0; i < PAK_HASHSIZE; ++i)
		pack->hashoffset[i] = -1;

	pack->hashoffset[i] = numpackfiles;

	// Find first file index with a certain hash
	for (i = numpackfiles - 1; i >= 0; --i)
		pack->hashoffset[newfiles[i].hash] = i;

	// Propagate real offsets downwards in array
	for (i = PAK_HASHSIZE - 1; i >= 0; --i)
	{
		if (pack->hashoffset[i] == -1)
			pack->hashoffset[i] = pack->hashoffset[i + 1];
	}

	Con_Printf ("Added packfile %s (%i files)\n", packfile, numpackfiles);
	return pack;
}


/*
================
COM_AddGameDirectory

Sets com_gamedir, adds the directory to the head of the path,
then loads and adds pak1.pak pak2.pak ...
================
*/
void COM_AddGameDirectory (char *dir)
{
	int                             i;
	searchpath_t    *search;
	pack_t                  *pak;
	char                    pakfile[MAX_OSPATH];

	strcpy (com_gamedir, dir);

//
// add the directory to the search path
//
	search = Hunk_Alloc (sizeof(searchpath_t));
	strcpy (search->filename, dir);
	search->next = com_searchpaths;
	com_searchpaths = search;

//
// add any pak files in the format pak0.pak pak1.pak, ...
//
	for (i=0 ; ; i++)
	{
		sprintf (pakfile, "%s/pak%i.pak", dir, i);
		pak = COM_LoadPackFile (pakfile);
		if (!pak)
			break;
		search = Hunk_Alloc (sizeof(searchpath_t));
		search->pack = pak;
		search->next = com_searchpaths;
		com_searchpaths = search;
	}

//
// add the contents of the parms.txt file to the end of the command line
//

}

static void AddDir (qboolean Cond, char *basedir, char *gamedir, char *Dir)
{
	if (Cond && stricmp(gamedir, Dir))
		COM_AddGameDirectory (va("%s/%s", basedir, Dir));
}

/*
================
COM_InitFilesystem
================
*/
void COM_InitFilesystem (void)
{
	int             i, j;
	char		basedir[MAX_OSPATH], gamedir[MAX_OSPATH];
	searchpath_t    *search;

//
// -basedir <path>
// Overrides the system supplied base directory (under GAMENAME)
//
	i = COM_CheckParm ("-basedir");
	if (i && i < com_argc-1)
		strcpy (basedir, com_argv[i+1]);
	else
		strcpy (basedir, host_parms.basedir);

	j = strlen (basedir);

	if (j > 0)
	{
		if ((basedir[j-1] == '\\') || (basedir[j-1] == '/'))
			basedir[j-1] = 0;
	}

//
// -cachedir <path>
// Overrides the system supplied cache directory (NULL or /qcache)
// -cachedir - will disable caching.
//
	i = COM_CheckParm ("-cachedir");
	if (i && i < com_argc-1)
	{
		if (com_argv[i+1][0] == '-')
			com_cachedir[0] = 0;
		else
			strcpy (com_cachedir, com_argv[i+1]);
	}
	else if (host_parms.cachedir)
		strcpy (com_cachedir, host_parms.cachedir);
	else
		com_cachedir[0] = 0;

	// Don't add unnecessary dirs if already specified in "-game"
	memset (gamedir, 0, sizeof(gamedir));
	
	i = COM_CheckParm ("-game");
	if (i && i < com_argc-1)
		strncpy (gamedir, com_argv[i+1], sizeof(gamedir) - 1);

//
// start up with GAMENAME by default (id1)
//
	AddDir (true, basedir, gamedir, GAMENAME);
	AddDir (nehahra, basedir, gamedir, "nehahra"); // Add Nehahra
	AddDir (COM_CheckParm ("-rogue"), basedir, gamedir, "rogue");
	AddDir (COM_CheckParm ("-hipnotic"), basedir, gamedir, "hipnotic");
	AddDir (COM_CheckParm ("-quoth"), basedir, gamedir, "quoth");

//
// -game <gamedir>
// Adds basedir/gamedir as an override game
//
	if (gamedir[0])
	{
		com_modified = true;
		COM_AddGameDirectory (va("%s/%s", basedir, gamedir));
	}

//
// -path <dir or packfile> [<dir or packfile>] ...
// Fully specifies the exact serach path, overriding the generated one
//
	i = COM_CheckParm ("-path");
	if (i)
	{
		com_modified = true;
		com_searchpaths = NULL;
		while (++i < com_argc)
		{
			if (!com_argv[i] || com_argv[i][0] == '+' || com_argv[i][0] == '-')
				break;

			search = Hunk_Alloc (sizeof(searchpath_t));
			if ( !strcmp(COM_FileExtension(com_argv[i]), "pak") )
			{
				search->pack = COM_LoadPackFile (com_argv[i]);
				if (!search->pack)
					Sys_Error ("Couldn't load packfile: %s", com_argv[i]);
			}
			else
				strcpy (search->filename, com_argv[i]);
			search->next = com_searchpaths;
			com_searchpaths = search;
		}
	}

	if (COM_CheckParm ("-proghack"))
		proghack = true;
}


