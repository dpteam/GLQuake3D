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

// draw.c -- this is the only file outside the refresh that touches the
// vid buffer

#include "quakedef.h"

#define GL_COLOR_INDEX8_EXT     0x80E5

cvar_t		gl_nobind = {"gl_nobind", "0"};
cvar_t		gl_max_size = {"gl_max_size", "1024"};
cvar_t		gl_picmip = {"gl_picmip", "0"};
cvar_t		gl_texquality = {"gl_texquality", "1"};

byte		*draw_chars;				// 8*8 graphic characters
qpic_t		*draw_disc;
qpic_t		*draw_backtile;

int			translate_texture;
int			char_texture;

typedef struct
{
	int		texnum;
	float	sl, tl, sh, th;
} glpic_t;

byte		conback_buffer[sizeof(qpic_t) + sizeof(glpic_t)];
qpic_t		*conback = (qpic_t *)&conback_buffer;

int		gl_lightmap_format = 4;
int		gl_solid_format = 3;
int		gl_alpha_format = 4;

int		gl_filter_min = GL_LINEAR_MIPMAP_NEAREST;
int		gl_filter_max = GL_LINEAR;


typedef struct
{
	int		texnum;
	char	identifier[64];
	int		width, height;
	qboolean	mipmap;
	qboolean	free;
	int		lhcsum; // used to verify textures are identical
        int             bytesperpixel;
} gltexture_t;

#define	MAX_GLTEXTURES	2048 //1024

gltexture_t	gltextures[MAX_GLTEXTURES];
int		numgltextures;


void GL_Bind (int texnum)
{
	if (gl_nobind.value)
		texnum = char_texture;
	if (currenttexture == texnum)
		return;
	currenttexture = texnum;
#ifdef _WIN32
	bindTexFunc (GL_TEXTURE_2D, texnum);
#else
	glBindTexture(GL_TEXTURE_2D, texnum);
#endif
}


/*
=============================================================================

  scrap allocation

  Allocate all the little status bar obejcts into a single texture
  to crutch up stupid hardware / drivers

=============================================================================
*/

#define	MAX_SCRAPS		2
#define	BLOCK_WIDTH		256
#define	BLOCK_HEIGHT	256

int			scrap_allocated[MAX_SCRAPS][BLOCK_WIDTH];
byte		scrap_texels[MAX_SCRAPS][BLOCK_WIDTH*BLOCK_HEIGHT*4];
qboolean	scrap_dirty;
int			scrap_texnum;

// returns a texture number and the position inside it
int Scrap_AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;
	int		bestx;
	int		texnum;

	for (texnum=0 ; texnum<MAX_SCRAPS ; texnum++)
	{
		best = BLOCK_HEIGHT;

		for (i=0 ; i<BLOCK_WIDTH-w ; i++)
		{
			best2 = 0;

			for (j=0 ; j<w ; j++)
			{
				if (scrap_allocated[texnum][i+j] >= best)
					break;
				if (scrap_allocated[texnum][i+j] > best2)
					best2 = scrap_allocated[texnum][i+j];
			}
			if (j == w)
			{	// this is a valid spot
				*x = i;
				*y = best = best2;
			}
		}

		if (best + h > BLOCK_HEIGHT)
			continue;

		for (i=0 ; i<w ; i++)
			scrap_allocated[texnum][*x + i] = best + h;

		return texnum;
	}

	Sys_Error ("Scrap_AllocBlock: full");

	return 0; // Shut up compiler
}

int	scrap_uploads;

void Scrap_Upload (void)
{
	int  texnum;
	char name[100];

	scrap_uploads++;

	for (texnum=0 ; texnum<MAX_SCRAPS ; texnum++)
	{
		GL_Bind(scrap_texnum + texnum);
		sprintf (name, "scrap%d", texnum);
		GL_Upload8 (scrap_texels[texnum], BLOCK_WIDTH, BLOCK_HEIGHT, false, true, name);
	}
	scrap_dirty = false;
}

//=============================================================================
/* Support Routines */

typedef struct cachepic_s
{
	char		name[MAX_QPATH];
	qpic_t		pic;
	byte		padding[32];	// for appended glpic
} cachepic_t;

#define	MAX_CACHED_PICS		128
cachepic_t	menu_cachepics[MAX_CACHED_PICS];
int			menu_numcachepics;

byte		menuplyr_pixels[4096];

int		pic_texels;
int		pic_count;

qpic_t *Draw_PicFromWad (char *name)
{
	qpic_t	*p;
	glpic_t	*gl;

	p = W_GetLumpName (name);

	// Sanity ...
	if (p->width & 0xC0000000 || p->height & 0xC0000000)
		Sys_Error ("Draw_PicFromWad: invalid dimensions (%dx%d) for '%s'", p->width, p->height, name);

	// Check if pic is big enough for the gl struct
	if (p->width * p->height < sizeof(glpic_t))
	{
		qpic_t		*p2 = malloc (sizeof(qpic_t) + sizeof(glpic_t)); // This leaks, kludge
		static qboolean WarnPrinted;

		if (!WarnPrinted)
		{
			WarnPrinted = true;
			Con_Printf ("\002Draw_PicFromWad: ");
			Con_Printf ("image size insufficient (%d, normal min = %d) in '%s'\n", p->width * p->height, sizeof(glpic_t), name);
		}

		memset (p2, 0, sizeof(qpic_t) + sizeof(glpic_t));
		memcpy (p2, p, sizeof(qpic_t));
		p = p2;
	}

	gl = (glpic_t *)p->data;

	// load little ones into the scrap
	if (p->width < 64 && p->height < 64)
	{
		int		x, y;
		int		i, j, k;
		int		texnum;

		texnum = Scrap_AllocBlock (p->width, p->height, &x, &y);
		scrap_dirty = true;
		k = 0;
		for (i=0 ; i<p->height ; i++)
			for (j=0 ; j<p->width ; j++, k++)
				scrap_texels[texnum][(y+i)*BLOCK_WIDTH + x + j] = p->data[k];
		texnum += scrap_texnum;
		gl->texnum = texnum;
		gl->sl = (x+0.01)/(float)BLOCK_WIDTH;
		gl->sh = (x+p->width-0.01)/(float)BLOCK_WIDTH;
		gl->tl = (y+0.01)/(float)BLOCK_WIDTH;
		gl->th = (y+p->height-0.01)/(float)BLOCK_WIDTH;

		pic_count++;
		pic_texels += p->width*p->height;
	}
	else
	{
		gl->texnum = GL_LoadPicTexture (name, p);
		gl->sl = 0;
		gl->sh = 1;
		gl->tl = 0;
		gl->th = 1;
	}
	return p;
}


/*
================
Draw_CachePic
================
*/
qpic_t	*Draw_CachePic (char *path)
{
	cachepic_t	*pic;
	int			i;
	qpic_t		*dat;
	glpic_t		*gl;

	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
		if (!strcmp (path, pic->name))
			return &pic->pic;

	if (menu_numcachepics == MAX_CACHED_PICS)
		Sys_Error ("menu_numcachepics == MAX_CACHED_PICS (%d)", MAX_CACHED_PICS);
	menu_numcachepics++;
	strcpy (pic->name, path);

//
// load the pic from disk
//
	dat = (qpic_t *)COM_LoadTempFile (path);
	if (!dat)
		Sys_Error ("Draw_CachePic: failed to load %s", path);
	SwapPic (dat);

	// HACK HACK HACK --- we need to keep the bytes for
	// the translatable player picture just for the menu
	// configuration dialog
	if (!strcmp (path, "gfx/menuplyr.lmp"))
		memcpy (menuplyr_pixels, dat->data, dat->width*dat->height);

	pic->pic.width = dat->width;
	pic->pic.height = dat->height;

	gl = (glpic_t *)pic->pic.data;
	gl->texnum = GL_LoadPicTexture (path, dat);
	gl->sl = 0;
	gl->sh = 1;
	gl->tl = 0;
	gl->th = 1;

	return &pic->pic;
}


void Draw_CharToConback (int num, byte *dest)
{
	int		row, col;
	byte	*source;
	int		drawline;
	int		x;

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	drawline = 8;

	while (drawline--)
	{
		for (x=0 ; x<8 ; x++)
			if (source[x] != 255)
				dest[x] = 0x60 + source[x];
		source += 128;
		dest += 320;
	}

}

typedef struct
{
	char *name;
	int	minimize, maximize;
} glmode_t;

glmode_t modes[] = {
	{"GL_NEAREST", GL_NEAREST, GL_NEAREST},
	{"GL_LINEAR", GL_LINEAR, GL_LINEAR},
	{"GL_NEAREST_MIPMAP_NEAREST", GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST},
	{"GL_LINEAR_MIPMAP_NEAREST", GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR},
	{"GL_NEAREST_MIPMAP_LINEAR", GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST},
	{"GL_LINEAR_MIPMAP_LINEAR", GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR}
};

/*
===============
Draw_TextureMode_f
===============
*/
void Draw_TextureMode_f (void)
{
	int		i;
	gltexture_t	*glt;

	if (Cmd_Argc() == 1)
	{
		for (i=0 ; i< 6 ; i++)
			if (gl_filter_min == modes[i].minimize)
			{
				Con_Printf ("%s (%d)\n", modes[i].name, i + 1);
				return;
			}
		Con_Printf ("current filter is unknown???\n");
		return;
	}

	for (i=0 ; i< 6 ; i++)
	{
		char *str = Cmd_Argv(1);
		if (!Q_strcasecmp (modes[i].name, str ) || isdigit(*str) && Q_atoi(str) - 1 == i)
			break;
	}
	if (i == 6)
	{
		Con_Printf ("bad filter name, available are:\n");
		for (i=0 ; i< 6 ; i++)
			Con_Printf ("%s (%d)\n", modes[i].name, i + 1);
		return;
	}

	gl_filter_min = modes[i].minimize;
	gl_filter_max = modes[i].maximize;

	// change all the existing mipmap texture objects
	for (i=0, glt=gltextures ; i<numgltextures ; i++, glt++)
	{
		if (glt->mipmap)
		{
			GL_Bind (glt->texnum);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter_min);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter_max);
		}
	}
}

/*
===============
Draw_SmoothFont_f
===============
*/
static qboolean smoothfont = 1;

static void SetSmoothFont (void)
{
	GL_Bind (char_texture);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smoothfont ? GL_LINEAR : GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smoothfont ? GL_LINEAR : GL_NEAREST);
}

void Draw_SmoothFont_f (void)
{
	if (Cmd_Argc() == 1)
	{
		Con_Printf ("gl_smoothfont is %d\n", smoothfont);
		return;
	}

	smoothfont = Q_atoi (Cmd_Argv(1));
	SetSmoothFont ();
}

static void Load_CharSet (void)
{
	int  i;
	byte *dest, *src;

	// load the console background and the charset
	// by hand, because we need to write the version
	// string into the background before turning
	// it into a texture
	draw_chars = W_GetLumpName ("conchars");
	for (i=0 ; i<256*64 ; i++)
		if (draw_chars[i] == 0)
			draw_chars[i] = 255;	// proper transparent color

	// Expand charset texture with blank lines in between to avoid in-line distortion
	dest = malloc (128 * 256);
	memset (dest, 0, 128 * 256);
	src = draw_chars;

	for (i = 0; i < 16; ++i)
		memcpy (&dest[8 * 128 * 2 * i], &src[8 * 128 * i], 8 * 128); // Copy each line

	// now turn them into textures
	char_texture = GL_LoadTexture ("charset", 128, 256, dest, false, true, 1, false);

	free (dest);

	SetSmoothFont ();
}

/*
===============
Draw_Init
===============
*/
void Draw_Init (void)
{
	int	i;
	qpic_t	*cb;
	byte	*dest, *src;
	int	x, y;
//	char	ver[40];
	glpic_t	*gl;
	byte	*ncdata;
	int	f, fstep;


	Cvar_RegisterVariable (&gl_nobind);
	Cvar_RegisterVariable (&gl_max_size);
	Cvar_RegisterVariable (&gl_picmip);
	Cvar_RegisterVariable (&gl_texquality);

	// 3dfx can only handle 256 wide textures
	if (!Q_strncasecmp ((char *)gl_renderer, "3dfx",4) ||
		strstr((char *)gl_renderer, "Glide"))
		Cvar_Set ("gl_max_size", "256");

	Cmd_AddCommand ("gl_texturemode", &Draw_TextureMode_f);
	Cmd_AddCommand ("gl_smoothfont", &Draw_SmoothFont_f);

	Load_CharSet ();

	cb = (qpic_t *)COM_LoadTempFile ("gfx/conback.lmp");
	if (!cb)
		Sys_Error ("Couldn't load gfx/conback.lmp");
	SwapPic (cb);

#if 0
	// hack the version number directly into the pic
#if defined(__linux__)
	sprintf (ver, "(Linux %2.2f, gl %4.2f) %4.2f", (float)LINUX_VERSION, (float)GLQUAKE_VERSION, (float)VERSION);
#else
	sprintf (ver, "(gl %4.2f) %4.2f", (float)GLQUAKE_VERSION, (float)VERSION);
#endif
	dest = cb->data + 320*186 + 320 - 11 - 8*strlen(ver);
	y = strlen(ver);
	for (x=0 ; x<y ; x++)
		Draw_CharToConback (ver[x], dest+(x<<3));
#endif

#if 0
	conback->width = vid.conwidth;
	conback->height = vid.conheight;

 	// scale console to vid size
 	dest = ncdata = Hunk_AllocName(vid.conwidth * vid.conheight, "conback");

 	for (y=0 ; y<vid.conheight ; y++, dest += vid.conwidth)
 	{
 		src = cb->data + cb->width * (y*cb->height/vid.conheight);
 		if (vid.conwidth == cb->width)
 			memcpy (dest, src, vid.conwidth);
 		else
 		{
 			f = 0;
 			fstep = cb->width*0x10000/vid.conwidth;
 			for (x=0 ; x<vid.conwidth ; x+=4)
 			{
 				dest[x] = src[f>>16];
 				f += fstep;
 				dest[x+1] = src[f>>16];
 				f += fstep;
 				dest[x+2] = src[f>>16];
 				f += fstep;
 				dest[x+3] = src[f>>16];
 				f += fstep;
 			}
 		}
 	}
#else
	conback->width = cb->width;
	conback->height = cb->height;
	ncdata = cb->data;
#endif

	gl = (glpic_t *)conback->data;
	gl->texnum = GL_LoadTexture ("conback", conback->width, conback->height, ncdata, false, false, 1, false);
	gl->sl = 0;
	gl->sh = 1;
	gl->tl = 0;
	gl->th = 1;
	conback->width = vid.width;
	conback->height = vid.height;

	// free loaded console
	Hunk_FreeToHighMark ();

	// save a texture slot for translated picture
	translate_texture = texture_extension_number++;

	// save slots for scraps
	scrap_texnum = texture_extension_number;
	texture_extension_number += MAX_SCRAPS;

	//
	// get the other pics we need
	//
	draw_disc = Draw_PicFromWad ("disc");
	draw_backtile = Draw_PicFromWad ("backtile");
}



/*
================
Draw_Character

Draws one 8*8 graphics character with 0 being transparent.
It can be clipped to the top of the screen to allow the console to be
smoothly scrolled off.
================
*/
static qboolean IsValid (int y, int num)
{
	if ((num & 127) == 32)
		return false; // space

	if (y <= -8)
		return false; // totally off screen

	return true;
}

static void Character (int x, int y, int num)
{
	int	row, col;
	float	frow, fcol, size, offset;

	num &= 255;

	row = num>>4;
	col = num&15;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.0625;
//	offset = 0.002; // slight offset to avoid in-between lines distortion
	offset = 0.03125; // offset to match expanded charset texture

	glTexCoord2f (fcol, frow);
	glVertex2f (x, y);
	glTexCoord2f (fcol + size, frow);
	glVertex2f (x+8, y);
	glTexCoord2f (fcol + size, frow + size - offset);
	glVertex2f (x+8, y+8);
	glTexCoord2f (fcol, frow + size - offset);
	glVertex2f (x, y+8);
}

void Draw_Character (int x, int y, int num)
{
	if (!IsValid (y, num))
		return;

	GL_Bind (char_texture);
	glBegin (GL_QUADS);

	Character (x, y, num);

	glEnd ();
}

/*
================
Draw_String
================
*/
void Draw_String (int x, int y, char *str)
{
	GL_Bind (char_texture);
	glBegin (GL_QUADS);

	while (*str)
	{
		if (IsValid (y, *str))
			Character (x, y, *str);

		str++;
		x += 8;
	}
	
	glEnd ();
}

/*
================
Draw_DebugChar

Draws a single character directly to the upper right corner of the screen.
This is for debugging lockups by drawing different chars in different parts
of the code.
================
*/
void Draw_DebugChar (char num)
{
}

/*
=============
Draw_AlphaPic
=============
*/
void Draw_AlphaPic (int x, int y, qpic_t *pic, float alpha)
{
	byte			*dest, *source;
	unsigned short	*pusdest;
	int				v, u;
	glpic_t			*gl;

	if (scrap_dirty)
		Scrap_Upload ();
	gl = (glpic_t *)pic->data;
	glDisable(GL_ALPHA_TEST);
	glEnable (GL_BLEND);
//	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
//	glCullFace(GL_FRONT);
	glColor4f (1,1,1,alpha);
	GL_Bind (gl->texnum);
	glBegin (GL_QUADS);
	glTexCoord2f (gl->sl, gl->tl);
	glVertex2f (x, y);
	glTexCoord2f (gl->sh, gl->tl);
	glVertex2f (x+pic->width, y);
	glTexCoord2f (gl->sh, gl->th);
	glVertex2f (x+pic->width, y+pic->height);
	glTexCoord2f (gl->sl, gl->th);
	glVertex2f (x, y+pic->height);
	glEnd ();
	glColor4f (1,1,1,1);
	glEnable(GL_ALPHA_TEST);
	glDisable (GL_BLEND);
}


/*
=============
Draw_Pic
=============
*/
void Draw_Pic (int x, int y, qpic_t *pic)
{
	byte			*dest, *source;
	unsigned short	*pusdest;
	int				v, u;
	glpic_t			*gl;

	if (scrap_dirty)
		Scrap_Upload ();
	gl = (glpic_t *)pic->data;
	glColor4f (1,1,1,1);
	GL_Bind (gl->texnum);
	glBegin (GL_QUADS);
	glTexCoord2f (gl->sl, gl->tl);
	glVertex2f (x, y);
	glTexCoord2f (gl->sh, gl->tl);
	glVertex2f (x+pic->width, y);
	glTexCoord2f (gl->sh, gl->th);
	glVertex2f (x+pic->width, y+pic->height);
	glTexCoord2f (gl->sl, gl->th);
	glVertex2f (x, y+pic->height);
	glEnd ();
}


/*
=============
Draw_TransPic
=============
*/
void Draw_TransPic (int x, int y, qpic_t *pic)
{
	byte	*dest, *source, tbyte;
	unsigned short	*pusdest;
	int				v, u;

	if (x < 0 || (unsigned)(x + pic->width) > vid.width || y < 0 ||
		 (unsigned)(y + pic->height) > vid.height)
	{
		Sys_Error ("Draw_TransPic: bad coordinates (%d, %d)", x, y);
	}

	Draw_Pic (x, y, pic);
}

/*
=============
Draw_TransPicTranslate

Only used for the player color selection menu
=============
*/
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, byte *translation)
{
	int		v, u, c;
	unsigned	*dest;
	byte		*src;
	int		p;
	unsigned	*trans = NULL;
	int		transsize = 0;

	c = pic->width * pic->height;

	trans = (unsigned *)COM_AllocBuf ("Draw_TransPicTranslate", trans, &transsize, c * sizeof(unsigned), 64 * 64 * sizeof(unsigned), "gfx/menuplyr.lmp");

	GL_Bind (translate_texture);

	dest = trans;
	for (v=0 ; v<64 ; v++, dest += 64)
	{
		src = &menuplyr_pixels[ ((v*pic->height)>>6) *pic->width];
		for (u=0 ; u<64 ; u++)
		{
			p = src[(u*pic->width)>>6];
			if (p == 255)
				dest[u] = p;
			else
				dest[u] =  d_8to24table[translation[p]];
		}
	}

	glTexImage2D (GL_TEXTURE_2D, 0, gl_alpha_format, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, trans);

	COM_FreeBuf (trans);

	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glColor3f (1,1,1);
	glBegin (GL_QUADS);
	glTexCoord2f (0, 0);
	glVertex2f (x, y);
	glTexCoord2f (1, 0);
	glVertex2f (x+pic->width, y);
	glTexCoord2f (1, 1);
	glVertex2f (x+pic->width, y+pic->height);
	glTexCoord2f (0, 1);
	glVertex2f (x, y+pic->height);
	glEnd ();
}


/*
================
Draw_ConsoleBackground

================
*/
void Draw_ConsoleBackground (int lines)
{
	int y = (vid.height * 3) >> 2;

	if (lines > y)
		Draw_Pic(0, lines - vid.height, conback);
	else
		Draw_AlphaPic (0, lines - vid.height, conback, (float)(1.2 * lines)/y);
}


/*
=============
Draw_TileClear

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void Draw_TileClear (int x, int y, int w, int h)
{
	glColor3f (1,1,1);
	GL_Bind (*(int *)draw_backtile->data);
	glBegin (GL_QUADS);
	glTexCoord2f (x/64.0, y/64.0);
	glVertex2f (x, y);
	glTexCoord2f ( (x+w)/64.0, y/64.0);
	glVertex2f (x+w, y);
	glTexCoord2f ( (x+w)/64.0, (y+h)/64.0);
	glVertex2f (x+w, y+h);
	glTexCoord2f ( x/64.0, (y+h)/64.0 );
	glVertex2f (x, y+h);
	glEnd ();
}


/*
=============
Draw_Fill

Fills a box of pixels with a single color
=============
*/
void Draw_Fill (int x, int y, int w, int h, int c)
{
	glDisable (GL_TEXTURE_2D);
	glColor3f (host_basepal[c*3]/255.0,
		host_basepal[c*3+1]/255.0,
		host_basepal[c*3+2]/255.0);

	glBegin (GL_QUADS);

	glVertex2f (x,y);
	glVertex2f (x+w, y);
	glVertex2f (x+w, y+h);
	glVertex2f (x, y+h);

	glEnd ();
	glColor3f (1,1,1);
	glEnable (GL_TEXTURE_2D);
}
//=============================================================================

/*
================
Draw_FadeScreen

================
*/
void Draw_FadeScreen (void)
{
	glEnable (GL_BLEND);
	glDisable (GL_TEXTURE_2D);
	glColor4f (0, 0, 0, 0.8);
	glBegin (GL_QUADS);

	glVertex2f (0,0);
	glVertex2f (vid.width, 0);
	glVertex2f (vid.width, vid.height);
	glVertex2f (0, vid.height);

	glEnd ();
	glColor4f (1,1,1,1);
	glEnable (GL_TEXTURE_2D);
	glDisable (GL_BLEND);

	Sbar_Changed();
}

//=============================================================================

/*
================
Draw_BeginDisc

Draws the little blue disc in the corner of the screen.
Call before beginning any disc IO.
================
*/
void Draw_BeginDisc (void)
{
	if (!draw_disc)
		return;
	glDrawBuffer  (GL_FRONT);
	Draw_Pic (vid.width - 24, 0, draw_disc);
	glDrawBuffer  (GL_BACK);
}


/*
================
Draw_EndDisc

Erases the disc icon.
Call after completing any disc IO
================
*/
void Draw_EndDisc (void)
{
}

/*
================
GL_Set2D

Setup as if the screen was 320*200
================
*/
void GL_Set2D (void)
{
	glViewport (glx, gly, glwidth, glheight);

	glMatrixMode(GL_PROJECTION);
    glLoadIdentity ();
	glOrtho  (0, vid.width, vid.height, 0, -99999, 99999);

	glMatrixMode(GL_MODELVIEW);
    glLoadIdentity ();

	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	glDisable (GL_BLEND);
	glEnable (GL_ALPHA_TEST);
//	glDisable (GL_ALPHA_TEST);

	glColor4f (1,1,1,1);
}

//====================================================================

/*
================
GL_FindTexture
================
*/
int GL_FindTexture (char *identifier)
{
	int		i;
	gltexture_t	*glt;

	for (i=0, glt=gltextures ; i<numgltextures ; i++, glt++)
	{
		if (!strcmp (identifier, glt->identifier))
			return gltextures[i].texnum;
	}

	return -1;
}

/*
================
GL_ResampleTexture
================
*/
static void ResampleTextureQuality (unsigned *in, int inwidth, int inheight, unsigned *out,  int outwidth, int outheight, qboolean alpha)
{
	byte	 *nwpx, *nepx, *swpx, *sepx, *dest, *inlimit = (byte *)(in + inwidth * inheight);
	unsigned xfrac, yfrac, x, y, modx, mody, imodx, imody, injump, outjump;
	int	 i, j;

	// Make sure "out" size is at least 2x2!
	xfrac = ((inwidth-1) << 16) / (outwidth-1);
	yfrac = ((inheight-1) << 16) / (outheight-1);
	y = outjump = 0;

	for (i=0; i<outheight; i++)
	{
		mody = (y>>8) & 0xFF;
		imody = 256 - mody;
		injump = (y>>16) * inwidth;
		x = 0;

		for (j=0; j<outwidth; j++)
		{
			modx = (x>>8) & 0xFF;
			imodx = 256 - modx;

			nwpx = (byte *)(in + (x>>16) + injump);
			nepx = nwpx + sizeof(int);
			swpx = nwpx + inwidth * sizeof(int); // Next line

			// Don't exceed "in" size
			if (swpx + sizeof(int) >= inlimit)
			{
//				Con_SafePrintf ("\002ResampleTextureQuality: ");
//				Con_SafePrintf ("error: %4d\n", swpx + sizeof(int) - inlimit);
				swpx = nwpx; // There's no next line
			}

			sepx = swpx + sizeof(int);

			dest = (byte *)(out + outjump + j);

			dest[0] = (nwpx[0]*imodx*imody + nepx[0]*modx*imody + swpx[0]*imodx*mody + sepx[0]*modx*mody)>>16;
			dest[1] = (nwpx[1]*imodx*imody + nepx[1]*modx*imody + swpx[1]*imodx*mody + sepx[1]*modx*mody)>>16;
			dest[2] = (nwpx[2]*imodx*imody + nepx[2]*modx*imody + swpx[2]*imodx*mody + sepx[2]*modx*mody)>>16;
			if (alpha)
				dest[3] = (nwpx[3]*imodx*imody + nepx[3]*modx*imody + swpx[3]*imodx*mody + sepx[3]*modx*mody)>>16;
			else
				dest[3] = 255;

			x += xfrac;
		}
		outjump += outwidth;
		y += yfrac;
	}
}

void GL_ResampleTexture (unsigned *in, int inwidth, int inheight, unsigned *out,  int outwidth, int outheight, qboolean alpha)
{
	int	    i, j;
	unsigned    *inrow;
	unsigned    frac, fracstep;

	// Sanity ...
	if (inwidth <= 0 || inheight <= 0 || outwidth <= 0 || outheight <= 0 ||
	    inwidth * 0x10000 & 0xC0000000 || inheight * outheight & 0xC0000000 ||
	    inwidth * inheight & 0xC0000000)
		Sys_Error ("GL_ResampleTexture: invalid parameters (in:%dx%d, out:%dx%d)", inwidth, inheight, outwidth, outheight);

	if (gl_texquality.value)
	{
		ResampleTextureQuality (in, inwidth, inheight, out, outwidth, outheight, alpha);
		return;
	}

	fracstep = inwidth*0x10000/outwidth;
	for (i=0 ; i<outheight ; i++, out += outwidth)
	{
		inrow = in + inwidth*(i*inheight/outheight);
		frac = fracstep >> 1;
		for (j=0 ; j<outwidth ; j+=4)
		{
			out[j] = inrow[frac>>16];
			frac += fracstep;
			out[j+1] = inrow[frac>>16];
			frac += fracstep;
			
			// Don't exceed "in" size
			if (frac>>16 >= inwidth)
				break;

			out[j+2] = inrow[frac>>16];
			frac += fracstep;
			out[j+3] = inrow[frac>>16];
			frac += fracstep;
		}
	}
}

/*
================
GL_MipMap

Operates in place, quartering the size of the texture
================
*/
void GL_MipMap (byte *in, int width, int height)
{
	int		i, j;
	byte	*out;

	width <<=2;
	height >>= 1;
	out = in;
	for (i=0 ; i<height ; i++, in+=width)
	{
		for (j=0 ; j<width ; j+=8, out+=4, in+=8)
		{
			out[0] = (in[0] + in[4] + in[width+0] + in[width+4])>>2;
			out[1] = (in[1] + in[5] + in[width+1] + in[width+5])>>2;
			out[2] = (in[2] + in[6] + in[width+2] + in[width+6])>>2;
			out[3] = (in[3] + in[7] + in[width+3] + in[width+7])>>2;
		}
	}
}

static int ScaleSize (int OldSize, qboolean Force)
{
	int NewSize, NextSize;

	// gl_texquality == 0 : GLQuake compatibility
	// gl_texquality == 1 : default, better resampling, less blurring of non-mipmapped texes
	// gl_texquality == 2 : better resampling, less blurring of all texes, requires a lot of memory
	if (gl_texquality.value < 1 || Force)
		NextSize = OldSize;
	else
		NextSize = 3 * OldSize / 2; // Avoid unfortunate resampling

	for (NewSize = 1; NewSize < NextSize && NewSize != OldSize; NewSize <<= 1)
		;

	return NewSize;
}

static void MipMap (unsigned *scaled, int *scaled_width, int *scaled_height)
{
	GL_MipMap ((byte *)scaled, *scaled_width, *scaled_height);

	*scaled_width >>= 1;
	*scaled_height >>= 1;
	if (*scaled_width < 1)
		*scaled_width = 1;
	if (*scaled_height < 1)
		*scaled_height = 1;
}

static void Brighten (byte *data, int width, int height, int value)
{
	int i, j, size = width * height;

	for (i = 0; i < size; ++i)
	{
		for (j = 0; j < 3; ++j)
		{
			byte *rgb = &data[i * sizeof(int) + j];

			if (*rgb < 255 - value)
				*rgb += value;
			else
				*rgb = 255;
		}
	}
}

/*
===============
GL_Upload32
===============
*/
void GL_Upload32 (unsigned *data, int width, int height, qboolean mipmap, qboolean alpha, char *name)
{
	int		samples;
	int		scaled_width, scaled_height;
	static unsigned	*scaled = NULL;
	static int	scaledsize = 0;

	scaled_width = ScaleSize (width, gl_texquality.value == 1 && mipmap);
	scaled_height = ScaleSize (height, gl_texquality.value == 1 && mipmap);

	if (width && height)
	{
		// Preserve proportions
		if ((float)scaled_width / width > 2 && fabs ((float)width / height - (float)scaled_width / scaled_height) >
						       fabs ((float)width / height - ((float)scaled_width / 2) / scaled_height))
			scaled_width /= 2;
		else if ((float)scaled_height / height > 2 && fabs ((float)width / height - (float)scaled_width / scaled_height) >
							      fabs ((float)width / height - (float)scaled_width / (scaled_height / 2)))
			scaled_height /= 2;
	}

	// Note: Can't use Con_Printf here!
	if (developer.value > 1 && (scaled_width != ScaleSize (width, true) || scaled_height != ScaleSize (height, true)))
		Con_SafePrintf ("GL_Upload32: in:%dx%d, out:%dx%d, '%s'\n", width, height, scaled_width, scaled_height, name);

	// Prevent too large or too small images (might otherwise crash resampling)
	scaled_width = CLAMP(2, scaled_width, gl_max_size.value);
	scaled_height = CLAMP(2, scaled_height, gl_max_size.value);

	scaled = (unsigned *)COM_AllocBuf ("GL_Upload32", scaled, &scaledsize, scaled_width * scaled_height * sizeof(unsigned), 0, name);

	samples = alpha ? gl_alpha_format : gl_solid_format;

#if 0
	if (mipmap)
		gluBuild2DMipmaps (GL_TEXTURE_2D, samples, width, height, GL_RGBA, GL_UNSIGNED_BYTE, trans);
	else if (scaled_width == width && scaled_height == height)
		glTexImage2D (GL_TEXTURE_2D, 0, samples, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, trans);
	else
	{
		gluScaleImage (GL_RGBA, width, height, GL_UNSIGNED_BYTE, trans,
			scaled_width, scaled_height, GL_UNSIGNED_BYTE, scaled);
		glTexImage2D (GL_TEXTURE_2D, 0, samples, scaled_width, scaled_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, scaled);
	}
#else
	if (scaled_width == width && scaled_height == height)
	{
		if (!mipmap)
		{
			if (!strcmp(name, "charset"))
				Brighten ((byte *)data, width, height, 10); // Improve readability of console text

			glTexImage2D (GL_TEXTURE_2D, 0, samples, scaled_width, scaled_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
			goto done;
		}
		memcpy (scaled, data, width*height*4);
	}
	else if (width && height) // Don't resample 0-sized images
		GL_ResampleTexture (data, width, height, scaled, scaled_width, scaled_height, alpha);

	if (mipmap)
	{
		int i;

		// Only affect mipmapped texes, typically not console graphics
		for (i = 0; i < gl_picmip.value && (scaled_width > 1 || scaled_height > 1); ++i)
			MipMap (scaled, &scaled_width, &scaled_height);
	}

	glTexImage2D (GL_TEXTURE_2D, 0, samples, scaled_width, scaled_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, scaled);
	if (mipmap)
	{
		int miplevel;

		miplevel = 0;
		while (scaled_width > 1 || scaled_height > 1)
		{
			MipMap (scaled, &scaled_width, &scaled_height);
			miplevel++;
			glTexImage2D (GL_TEXTURE_2D, miplevel, samples, scaled_width, scaled_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, scaled);
		}
	}
done: ;
#endif

	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmap ? gl_filter_min : gl_filter_max);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter_max);
}

/*
===============
GL_Upload8
===============
*/
void GL_Upload8 (byte *data, int width, int height,  qboolean mipmap, qboolean alpha, char *name)
{
	int		    i, s;
	qboolean	    noalpha;
	int		    p;
	static unsigned	    *trans = NULL;
	static int	    transsize = 0;

	s = width*height;

	trans = (unsigned *)COM_AllocBuf ("GL_Upload8", trans, &transsize, s * sizeof(unsigned), 640 * 480 * sizeof(unsigned), name);

	// if there are no transparent pixels, make it a 3 component
	// texture even if it was specified as otherwise
	if (alpha)
	{
		noalpha = true;
		for (i=0 ; i<s ; i++)
		{
			p = data[i];
			if (p == 255)
				noalpha = false;
			trans[i] = d_8to24table[p];
		}

		if (alpha && noalpha)
			alpha = false;
	}
	else
	{
		if (s&3)
		{
			Con_SafePrintf ("\002GL_Upload8: ");
			Con_SafePrintf ("size %d is not a multiple of 4 in '%s'\n", s, name);
		}
		
		for (i=0 ; i<s ; ++i)
			trans[i] = d_8to24table[data[i]];
	}

	GL_Upload32 (trans, width, height, mipmap, alpha, name);
}

/*
================
GL_FreeTextures
================
*/
void GL_FreeTextures (void)
{
	int i, j;
//static int MaxTex = 0;

	// Free textures and pack array
	for (i = j = 0; i < numgltextures; ++i, ++j)
	{
		if (gltextures[i].free)
		{
			glDeleteTextures(1, &gltextures[i].texnum);
			--j;
		}
		else if (j < i)
			gltextures[j] = gltextures[i];
	}

/*if (numgltextures > MaxTex)
	MaxTex = numgltextures;

Con_SafePrintf ("\x02GL_FreeTextures: ");
Con_SafePrintf ("numgltextures = %d, was %d, max %d\n", j, numgltextures, MaxTex);*/
	numgltextures = j;
}

/*
================
GL_LoadTexture
================
*/

static int lhcsumtable[256]; // used to verify textures are identical

int GL_LoadTexture (char *identifier, int width, int height, byte *data, qboolean mipmap, qboolean alpha, int bytes, qboolean free)
{
	int		i, p, s, lhcsum;
	gltexture_t	*glt;
	float		size;

	if (isDedicated)
		return 0; // No textures in dedicated mode

	for (i=0 ; i<256 ; i++)
		lhcsumtable[i] = i + 1;

	s = width * height * bytes;
	size = (float)width * (float)height * (float)bytes;

	if (size == 0)
		Con_DPrintf ("GL_LoadTexture: texture '%s' has size 0\n", identifier);

	// Sanity check, max = 32kx32k
	if (width < 0 || height < 0 || size > 0x40000000)
		Sys_Error ("GL_LoadTexture: texture '%s' has invalid size (%.0fM, max = %dM)", identifier, size / (1024 * 1024), 0x40000000 / (1024 * 1024));

	lhcsum = 0;

	for (i=0; i<s ; i++)
		lhcsum += (lhcsumtable[data[i] & 255]++);

	// see if the texture is allready present
	if (identifier[0])
	{
		for (i=0, glt=gltextures ; i<numgltextures ; i++, glt++)
		{
			if (lhcsum == glt->lhcsum && width == glt->width && height == glt->height && !strcmp (identifier, glt->identifier))
				return glt->texnum;
		}
	}

	if (numgltextures >= MAX_GLTEXTURES)
		Sys_Error ("GL_LoadTexture: cache full, max is %i textures", MAX_GLTEXTURES);

	glt = &gltextures[numgltextures];
	numgltextures++;

	strcpy (glt->identifier, identifier);
	glt->texnum = texture_extension_number;
	texture_extension_number++;

	glt->lhcsum = lhcsum;
	glt->width = width;
	glt->height = height;
	glt->mipmap = mipmap;
	glt->free = free;
        glt->bytesperpixel = bytes;

	GL_Bind (glt->texnum);

	if (bytes == 1)
		GL_Upload8 (data, width, height, mipmap, alpha, identifier);
	else if (bytes == 4)
		GL_Upload32 ((unsigned *)data, width, height, mipmap, alpha, identifier);
	else
		Sys_Error ("GL_LoadTexture: unknown bytesperpixel %d for %s\n", bytes, identifier);

	return glt->texnum;
}

/*
================
GL_LoadPicTexture
================
*/
int GL_LoadPicTexture (char *name, qpic_t *pic)
{
	return GL_LoadTexture (name, pic->width, pic->height, pic->data, false, true, 1, false);
}

/****************************************/

GLenum gl_oldtarget;

void GL_SelectTexture (GLenum target)
{
	if (!gl_mtexable)
		return;
	qglSelectTextureSGIS(target);
	if (target == gl_oldtarget)
		return;
	cnttextures[gl_oldtarget-TEXTURE0_SGIS] = currenttexture;
	currenttexture = cnttextures[target-TEXTURE0_SGIS];
	gl_oldtarget = target;
}
