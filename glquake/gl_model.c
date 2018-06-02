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
// models.c -- model loading and caching

// models are the only shared resource between a client and server running
// on the same machine.

#include "quakedef.h"

model_t	*loadmodel;
char	loadname[32];	// for hunk tags

void Mod_LoadSpriteModel (model_t *mod, void *buffer);
void Mod_LoadBrushModel (model_t *mod, void *buffer);
void Mod_LoadAliasModel (model_t *mod, void *buffer);
model_t *Mod_LoadModel (model_t *mod, qboolean crash);

byte	mod_novis[MAX_MAP_LEAFS/8];

#define	MAX_MOD_KNOWN	2048 //512
model_t	mod_known[MAX_MOD_KNOWN];
int	mod_numknown;

cvar_t gl_subdivide_size = {"gl_subdivide_size", "512", true}; //128
cvar_t gl_autobright = {"gl_autobright", "1"};
cvar_t gl_exttex = {"gl_exttex", "0"}; // Load external textures from subdirectory 'textures'
cvar_t gl_exttexdp = {"gl_exttexdp", "0"}; // DP style model tex path (default JoeQ)
cvar_t gl_exttextq = {"gl_exttextq", "0"}; // Tomaz style model tex path
cvar_t gl_exttexworld = {"gl_exttexworld", "1"}; // World textures
cvar_t nospr32 = {"nospr32", "0"}; // Nehahra 32-bit sprites
cvar_t gl_oldspr32 = {"gl_oldspr32", "0"}; // Old buggy spr32

/*
===============
Mod_Init
===============
*/
void Mod_Init (void)
{
	Cvar_RegisterVariable (&gl_subdivide_size);
	Cvar_RegisterVariable (&gl_autobright);
	Cvar_RegisterVariable (&gl_exttexworld);
	Cvar_RegisterVariable (&gl_exttexdp);
	Cvar_RegisterVariable (&gl_exttextq);
	Cvar_RegisterVariable (&gl_exttex);
	Cvar_RegisterVariable (&nospr32);
	Cvar_RegisterVariable (&gl_oldspr32);
	memset (mod_novis, 0xff, sizeof(mod_novis));
}

/*
===============
Mod_Extradata

Caches the data if needed
===============
*/
void *Mod_Extradata (model_t *mod)
{
	void	*r;

	r = Cache_Check (&mod->cache);
	if (r)
		return r;

	Mod_LoadModel (mod, true);

	if (!mod->cache.data)
		Sys_Error ("Mod_Extradata: caching failed, model %s", mod->name);
	return mod->cache.data;
}

/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *Mod_PointInLeaf (vec3_t p, model_t *model)
{
	mnode_t		*node;
	float		d;
	mplane_t	*plane;

	if (!model || !model->nodes)
		Sys_Error ("Mod_PointInLeaf: bad model");

	node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return (mleaf_t *)node;
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return NULL;	// never reached
}


/*
===================
Mod_DecompressVis
===================
*/
byte *Mod_DecompressVis (byte *in, model_t *model)
{
	static byte	decompressed[MAX_MAP_LEAFS/8];
	int		c;
	byte		*out;
	int		row;

	row = (model->numleafs+7)>>3;
	out = decompressed;

	if (!in || r_novis.value == 2)
	{	// no vis info, so make all visible
		while (row)
		{
			*out++ = 0xff;
			row--;
		}
		return decompressed;
	}

	do
	{
		if (*in)
		{
			*out++ = *in++;
			continue;
		}

		c = in[1];
		in += 2;
		while (c)
		{
			*out++ = 0;
			c--;
		}
	} while (out - decompressed < row);

	return decompressed;
}

byte *Mod_LeafPVS (mleaf_t *leaf, model_t *model)
{
	if (leaf == model->leafs)
		return mod_novis;
	return Mod_DecompressVis (leaf->compressed_vis, model);
}

/*
=================
Mod_Alloc

Avoid putting large objects in Quake heap
=================
*/
static byte *Mod_Alloc (char *Function, int Size)
{
	byte *Buf = malloc (Size);

	if (!Buf)
		Sys_Error ("%s: not enough memory (%d) for '%s'", Function, Size, loadname);

//Con_SafePrintf ("Mod_Alloc: %s: %dk\n", Function, Size / 1024);
	return Buf;
}

/*
=================
Mod_AllocClear
=================
*/
static void Mod_AllocClear (model_t *mod)
{
	// Clear Mod_Alloc pointers
	mod->lightdata = NULL;
	mod->visdata = NULL;
}

/*
=================
Mod_Free2
=================
*/
static void Mod_Free2 (byte **Ptr)
{
	if (*Ptr)
	{
		free (*Ptr);
		*Ptr = NULL;
	}
}

/*
=================
Mod_Free
=================
*/
static void Mod_Free (model_t *mod)
{
	Mod_Free2 (&mod->lightdata);
	Mod_Free2 (&mod->visdata);
}

static qboolean FreeMdl; // Free also mdls

/*
===================
Mod_ClearAll
===================
*/
void Mod_ClearAll (void)
{
	int		i;
	model_t		*mod;
	static qboolean NoFree, Done;

	if (!Done)
	{
		// Some 3dfx miniGLs don't support glDeleteTextures (i.e. do nothing)
		NoFree = COM_CheckParm ("-nofreetex");
		
		FreeMdl = COM_CheckParm ("-freemdl"); // Free also mdls
		Done = true;
	}

	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (mod->type != mod_alias || FreeMdl)
			mod->needload = true;
		
		if (mod->type == mod_alias)
		{
			if (FreeMdl && mod->cache.data)
				Cache_Free(&mod->cache); // Release memory buffer

			mod->loadtimes = 0; // Clear inbetween maps
		}

		Mod_Free (mod);
	}

	if (!NoFree)
		GL_FreeTextures ();
}

/*
==================
Mod_FindName

==================
*/
model_t *Mod_FindName (char *name)
{
	int		i;
	model_t	*mod;

	if (!name[0])
		Sys_Error ("Mod_FindName: NULL name");

//
// search the currently loaded models
//
	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
#ifdef NOISE
		if (!stricmp (mod->name, name) )
#else
		if (!strcmp (mod->name, name) )
#endif
			break;

	if (i == mod_numknown)
	{
		int len;

		if (mod_numknown == MAX_MOD_KNOWN)
			Sys_Error ("Mod_FindName: mod_numknown == MAX_MOD_KNOWN (%d)", MAX_MOD_KNOWN);

		len = strlen (name);

		if (len >= sizeof(mod->name))
			Sys_Error ("Mod_FindName: name '%s' too long (%d, max = %d)", name, len, sizeof(mod->name) - 1);

		strcpy (mod->name, name);
		mod->needload = true;
		mod_numknown++;
	}

	return mod;
}

/*
==================
Mod_TouchModel

==================
*/
void Mod_TouchModel (char *name)
{
	model_t	*mod;

	mod = Mod_FindName (name);

	if (!mod->needload)
	{
		if (mod->type == mod_alias)
			Cache_Check (&mod->cache);
	}
}

static int  mod_filesize;
static char mod_name[MAX_OSPATH];

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
model_t *Mod_LoadModel (model_t *mod, qboolean crash)
{
	void	*d;
	unsigned *buf;
	byte	stackbuf[1024];		// avoid dirtying the cache heap

	if (!mod->needload)
	{
		if (mod->type == mod_alias)
		{
			d = Cache_Check (&mod->cache);
			if (d)
				return mod;
		}
		else
			return mod;		// not cached at all
	}

//
// because the world is so huge, load it one piece at a time
//

//
// load the file
//
	buf = (unsigned *)COM_LoadStackFile (mod->name, stackbuf, sizeof(stackbuf));
	if (!buf)
	{
		// If last added mod_known, get rid of it
		if (mod == &mod_known[mod_numknown - 1])
			--mod_numknown;

		if (crash)
			Host_Error ("Mod_LoadModel: %s not found", mod->name);
		return NULL;
	}

	mod_filesize = com_filesize; // Save off
	COM_StripExtension (COM_SkipPath(mod->name), mod_name);

#ifdef _DEBUG
#ifdef NOISE
	// Noise generator for bsps/models, match with Sys_Error hook
//	if (LittleLong(*(unsigned *)buf) == IDPOLYHEADER)
//	if (LittleLong(*(unsigned *)buf) == IDSPRITEHEADER)
	if (LittleLong(*(unsigned *)buf) == BSPVERSION && mod_filesize > 32768) // Avoid small bsp models
	{
		int		i, j, Errors, ErrPos, ErrRange, FSize, ErrSegments = 0;
		unsigned	ErrVal;
		byte		*Buf2 = (byte *)buf;
		static qboolean Done = false;

		if (!Done)
		{
			// Only seed once
			srand ((unsigned)(Sys_FloatTime() * 1000));
			Done = true;
		}

		// Fixed # errors or file size dependant
//		Errors = 2000 * (float)rand () / RAND_MAX;

		FSize = mod_filesize < 2 * 1024 * 1024 ? mod_filesize : 2 * 1024 * 1024;
		Errors = (float)FSize / 275 * (float)rand () / RAND_MAX;

		// File size errors
//		Errors = mod_filesize = (float)mod_filesize * (float)rand () / RAND_MAX;

		Con_Printf ("noise errors : %d\n", Errors);

		// File size errors
//		Errors = 0;

		for (i = 0; i < Errors; ++i)
		{
			ErrPos = (float)mod_filesize * (float)rand () / RAND_MAX;
			ErrVal = 65536 * (float)rand () / RAND_MAX;

			ErrRange = 1;

			if ((float)rand () / RAND_MAX > 0.99)
			{
				// 1% chance for up to 16 byte error segment
				ErrRange = 16 * (float)rand () / RAND_MAX;

				if (ErrPos + ErrRange > mod_filesize)
					ErrRange = mod_filesize - ErrPos;

//				Con_SafePrintf ("noise segment : %d\n", ErrRange);
				++ErrSegments;
			}

			for (j = 0; j < ErrRange; ++j)
				Buf2[ErrPos + j] = ErrVal;
		}

		Con_SafePrintf ("noise segments : %d\n", ErrSegments);
	}
#endif
#endif

//
// allocate a new model
//
	COM_FileBase (mod->name, loadname);

	loadmodel = mod;

//
// fill it in
//

// call the apropriate loader
	mod->needload = false;

	switch (LittleLong(*(unsigned *)buf))
	{
	case IDPOLYHEADER:
		Mod_LoadAliasModel (mod, buf);
		break;

	case IDSPRITEHEADER:
		Mod_LoadSpriteModel (mod, buf);
		break;

	default:
		Mod_LoadBrushModel (mod, buf);
		break;
	}

	return mod;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *Mod_ForName (char *name, qboolean crash)
{
	model_t	*mod;

	mod = Mod_FindName (name);

	return Mod_LoadModel (mod, crash);
}


/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

static int MissTex;
byte	   *mod_base;

/*
=================
Mod_ChkLumpSize
=================
*/
static void Mod_ChkLumpSize (char *Function, lump_t *l, int Size)
{
	if (l->filelen % Size)
		Sys_Error ("%s: lump size %d is not a multiple of %d in %s", Function, l->filelen, Size, loadmodel->name);
}

/*
=================
WithinBounds
=================
*/
static qboolean WithinBounds (int SItem, int Items, int Limit, int *ErrItem)
{
	int EItem = 0;

	if (SItem < 0)
		EItem = SItem;
	else if (Items < 0)
		EItem = Items;
	else if (SItem + Items < 0 || SItem + Items > Limit)
		EItem = SItem + Items;

	if (ErrItem)
		*ErrItem = EItem;

	return EItem == 0;
}

/*
=================
Mod_ChkBounds
=================
*/
static void Mod_ChkBounds (char *Function, char *Object, int SItem, int Items, int Limit)
{
	int ErrItem;

	if (!WithinBounds (SItem, Items, Limit, &ErrItem))
		Sys_Error ("%s: %s is out of bounds (%d, max = %d) in %s", Function, Object, ErrItem, Limit, loadmodel->name);
}

/*
=================
Mod_ChkFileSize
=================
*/
static void Mod_ChkFileSize (char *Function, char *Object, int SItem, int Items)
{
	int ErrItem;

	if (!WithinBounds (SItem, Items, mod_filesize, &ErrItem))
		Sys_Error ("%s: %s is outside file (%d, max = %d) in %s", Function, Object, ErrItem, mod_filesize, loadmodel->name);
}

/*
=================
Mod_ChkType
=================
*/
static void Mod_ChkType (char *Function, char *Object, char *ObjNum, int Type, int Type1, int Type2)
{
	if (Type != Type1 && Type != Type2)
	{
		// Should be an error but ...
		Con_SafePrintf ("\002%s: ", Function);
		Con_Printf ("invalid %s %d in %s in %s\n", Object, Type, ObjNum, loadmodel->name);
//		Sys_Error ("%s: invalid %s %d in %s in %s", Function, Object, Type, ObjNum, loadmodel->name);
	}
}

qboolean Mod_IsWorldModel; // This is true when loading the main bsp

/*
=================
Mod_LoadExtTex
=================
*/
int Mod_LoadExtTex (char *inname, int inwidth, int inheight, byte *indata, qboolean bsptex, qboolean sprite, int inframe, int ingroup, int bytes)
{
	int  width, height, texnum = 0, i;
	byte *data = NULL;
	char name[MAX_OSPATH], name2[MAX_OSPATH], groupstr[32], *ptr;

	if (!bsptex)
	{
		groupstr[0] = 0;

		if (ingroup != -1 && !sprite)
			sprintf (groupstr, "_%i", ingroup);
	}

	// Searching for external texes takes time, therefore it's default off
	if (gl_exttex.value && (gl_exttexworld.value || !Mod_IsWorldModel) && !isDedicated)
	{
		// Try to load external texture
		for (i = 0; i < 2; ++i)
		{
			if (bsptex)
			{
				strcpy (name, inname);

				if (name[0] == '*')
					name[0] = '#'; // Wildcards are not allowed in file system

				// Follow JoeQ/DP style bsp tex path; first try bspname subdir,
				// although it might clash with the models/sprites subdirs
				sprintf (name2, "textures/%s%s%s", i == 0 ? mod_name : "", i == 0 ? "/" : "", name);
			}
			else
			{
				if (gl_exttexdp.value || gl_exttextq.value)
				{
					int  frame;
					char groupstr2[32];

					ptr = inname;
					frame = inframe;
					strcpy (groupstr2, groupstr);

					// DP/Tomaz style tex path (same as mdl and rearranged name)
					if (gl_exttextq.value)
					{
						COM_StripExtension (inname, name);
						ptr = name;
					}
					else if (sprite && ingroup != -1)
					{
						// Hack	
						sprintf (groupstr2, "_%i", ingroup);
						frame -= ingroup * 100;
					}

					sprintf (name2, "%s%s_%i", ptr, groupstr2, frame);
				}
				else
				{
					// JoeQ style mdl/spr tex path; try two places
					COM_StripExtension (COM_SkipPath(inname), name);
					sprintf (name2, "textures/%s%s%s_%i", i == 0 ? (sprite ? "sprites/" : "models/") : "", name, groupstr, inframe);
				}
			}

			data = R_LoadImage (name2, &width, &height, sprite);
		
			// Only loop if not found and (bsptex or JoeQ style)
			if (data || !bsptex && (gl_exttexdp.value || gl_exttextq.value))
				break;
		}
	}

	if (data)
	{
		if (gl_exttex.value == 2)
			memset (data, 255, width * height * 4); // Make tex white to indicate that it's external

		texnum = GL_LoadTexture (name2, width, height, data, true, sprite, 4, bsptex || /*sprite ||*/ FreeMdl); // sprite removed for performance

		free (data);
	}
	else if (inwidth * inheight > 0) // Some sprites are 0, e.g. s_null.spr
	{
		ptr = inname;
		
		if (!bsptex)
		{
			// Maintain old naming convention (compatibility)
			sprintf (name, "%s%s_%i", inname, groupstr, inframe);
			ptr = name;
		}
		
		texnum = GL_LoadTexture (ptr, inwidth, inheight, indata, true, sprite, bytes, bsptex || /*sprite ||*/ FreeMdl); // sprite removed for performance
	}

	return texnum;
}

static int  MissFrameNo;
static char MissFrameName[16 +1];

static qboolean AllFrames (texture_t *tx, texture_t *anims[], int max, qboolean Alternate)
{
	int i, frames = 0;

	for (i=0 ; i<max ; i++)
	{
		if (anims[i])
		    ++frames;
		else
		{
			// Avoid unnecessary warnings
			if (i != MissFrameNo || strcmp(tx->name, MissFrameName))
			{
				MissFrameNo = i;
				strcpy (MissFrameName, tx->name);
				Con_SafePrintf ("\002Mod_LoadTextures: ");
				Con_Printf ("missing %sframe %i of '%s'\n", Alternate ? "alternate" : "", i, tx->name);
			}
		}
	}

	return frames == max;
}

/*
=================
Mod_LoadTextures
=================
*/
void Mod_LoadTextures (lump_t *l)
{
	int		i, j, pixels, num, max, altmax;
	miptex_t	*mt;
	texture_t	*tx, *tx2;
	texture_t	*anims[10];
	texture_t	*altanims[10];
	dmiptexlump_t	*m;
	char		texname[16 + 1];

	if (!l->filelen)
	{
		loadmodel->textures = NULL;
		Con_SafePrintf ("\x02Mod_LoadTextures: ");
		Con_Printf ("no textures in %s\n", loadmodel->name);
		return;
	}

	// Check bounds for nummiptex var
	Mod_ChkBounds ("Mod_LoadTextures", "nummiptex", 0, sizeof(int), l->filelen);

	m = (dmiptexlump_t *)(mod_base + l->fileofs);
	m->nummiptex = LittleLong (m->nummiptex);

	// Check bounds for dataofs array
	if (m->nummiptex > 0)
		Mod_ChkBounds ("Mod_LoadTextures", "miptex lump", sizeof(int), m->nummiptex * sizeof(int), l->filelen);

	loadmodel->numtextures = m->nummiptex;
	loadmodel->textures = Hunk_AllocName (m->nummiptex * sizeof(*loadmodel->textures) , "modtex1");

	MissTex = 0;

	for (i=0 ; i<m->nummiptex ; i++)
	{
		int size, copypixels;

		m->dataofs[i] = LittleLong (m->dataofs[i]);
		if (m->dataofs[i] == -1)
		{
			++MissTex;
			continue;
		}
		mt = (miptex_t *)((byte *)m + m->dataofs[i]);

		// Check bounds for miptex entry
		Mod_ChkBounds ("Mod_LoadTextures", "miptex entry", (byte *)mt - (mod_base + l->fileofs), sizeof(miptex_t), l->filelen);

		mt->width = LittleLong (mt->width);
		mt->height = LittleLong (mt->height);
		for (j=0 ; j<MIPLEVELS ; j++)
			mt->offsets[j] = LittleLong (mt->offsets[j]);

		// Make sure tex name is terminated
		memset (texname, 0, sizeof(texname));
		memcpy (texname, mt->name, sizeof(texname) - 1);

		// Check if missing tex name
		if (!texname[0] && gl_exttex.value)
		{
			sprintf (texname, "unnamed%d", i);
			
			if (developer.value > 2)
				Con_DPrintf ("Mod_LoadTextures: unnamed texture in %s, renaming to %s\n", loadmodel->name, texname);
		}

		if (mt->width <= 0 || mt->height <= 0 || (mt->width & 15) || (mt->height & 15))
			Sys_Error ("Mod_LoadTextures: texture '%s' is not 16 aligned (%dx%d) in %s", texname, mt->width, mt->height, loadmodel->name);

		// Check bounds for miptex objects
		for (j=0 ; j<MIPLEVELS ; j++)
		{
			int ErrItem, Offset = (byte *)mt - (mod_base + l->fileofs) + mt->offsets[j];
			int MipSize = mt->width * mt->height / (1 << j * 2);

			if (!WithinBounds(Offset, MipSize, l->filelen, &ErrItem))
			{
				Con_SafePrintf ("\x02Mod_LoadTextures: ");
				Con_Printf ("miptex object %d for '%s' is outside texture lump (%d, max = %d) in %s\n", j, texname, ErrItem, l->filelen, loadmodel->name);
				mt->offsets[j] = sizeof (miptex_t); // OK?
			}
		}

		pixels = copypixels = mt->width*mt->height/64*85;

		size = (byte *)(mt + 1) + pixels - (mod_base + l->fileofs);

		// Check bounds for pixels
		if (size > l->filelen)
		{
			Con_SafePrintf ("\x02Mod_LoadTextures: ");
			Con_Printf ("pixels for '%s' is outside texture lump (%d, max = %d) in %s\n", texname, size, l->filelen, loadmodel->name);
			copypixels -= size - l->filelen; // Prevent access violation
		}

		tx = Hunk_AllocName (sizeof(texture_t) +pixels, "modtex2");
		loadmodel->textures[i] = tx;

		strcpy (tx->name, texname);
		_strlwr (tx->name);
		tx->width = mt->width;
		tx->height = mt->height;
		for (j=0 ; j<MIPLEVELS ; j++)
			tx->offsets[j] = mt->offsets[j] + sizeof(texture_t) - sizeof(miptex_t);
		// the pixels immediately follow the structures
		memcpy ( tx+1, mt+1, copypixels);

		if (!isDedicated) //no texture uploading for dedicated server
		{
			if (!Q_strncmp(tx->name, "sky", 3))
				R_InitSky (tx);
			else
			{
				texture_mode = GL_LINEAR_MIPMAP_NEAREST; //_LINEAR;
				tx->gl_texturenum = Mod_LoadExtTex (tx->name, tx->width, tx->height, (byte *)(tx+1), true, false, 0, 0, 1);
				texture_mode = GL_LINEAR;
			}
		}
	}

//
// sequence the animations
//
	MissFrameNo = 0;
	memset (MissFrameName, 0, sizeof(MissFrameName));

	for (i=0 ; i<m->nummiptex ; i++)
	{
		tx = loadmodel->textures[i];
		if (!tx || tx->name[0] != '+')
			continue;
		if (tx->anim_next)
			continue;	// allready sequenced

	// find the number of frames in the animation
		memset (anims, 0, sizeof(anims));
		memset (altanims, 0, sizeof(altanims));

		max = tx->name[1];
		altmax = 0;
		if (max >= 'a' && max <= 'z')
			max -= 'a' - 'A';
		if (max >= '0' && max <= '9')
		{
			max -= '0';
			altmax = 0;
			anims[max] = tx;
			max++;
		}
		else if (max >= 'A' && max <= 'J')
		{
			altmax = max - 'A';
			max = 0;
			altanims[altmax] = tx;
			altmax++;
		}
		else
			Sys_Error ("Bad animating texture '%s'", tx->name);

		for (j=i+1 ; j<m->nummiptex ; j++)
		{
			tx2 = loadmodel->textures[j];
			if (!tx2 || tx2->name[0] != '+')
				continue;
			if (strcmp (tx2->name+2, tx->name+2))
				continue;

			num = tx2->name[1];
			if (num >= 'a' && num <= 'z')
				num -= 'a' - 'A';
			if (num >= '0' && num <= '9')
			{
				num -= '0';
				anims[num] = tx2;
				if (num+1 > max)
					max = num + 1;
			}
			else if (num >= 'A' && num <= 'J')
			{
				num = num - 'A';
				altanims[num] = tx2;
				if (num+1 > altmax)
					altmax = num+1;
			}
			else
				Sys_Error ("Bad animating texture '%s'", tx->name);
		}

		// Check if all frames are present
		if (!AllFrames (tx, anims, max, false) || !AllFrames (tx, altanims, altmax, true))
			continue; // Disable animation sequence
		
#define	ANIM_CYCLE	2
	// link them all together
		for (j=0 ; j<max ; j++)
		{
			tx2 = anims[j];
			tx2->anim_total = max * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j+1) * ANIM_CYCLE;
			tx2->anim_next = anims[ (j+1)%max ];
			if (altmax)
				tx2->alternate_anims = altanims[0];
		}
		for (j=0 ; j<altmax ; j++)
		{
			tx2 = altanims[j];
			tx2->anim_total = altmax * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j+1) * ANIM_CYCLE;
			tx2->anim_next = altanims[ (j+1)%altmax ];
			if (max)
				tx2->alternate_anims = anims[0];
		}
	}
}

/*
=================
Mod_LoadLighting
=================
*/
void Mod_LoadLighting (lump_t *l)
{
	if (!l->filelen)
	{
		loadmodel->lightdata = NULL;
		return;
	}
	loadmodel->lightdata = Mod_Alloc ("Mod_LoadLighting", l->filelen);
	memcpy (loadmodel->lightdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVisibility
=================
*/
void Mod_LoadVisibility (lump_t *l)
{
	if (!l->filelen)
	{
		loadmodel->visdata = NULL;
		return;
	}
	loadmodel->visdata = Mod_Alloc ("Mod_LoadVisibility", l->filelen);
	memcpy (loadmodel->visdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadEntities
=================
*/
void Mod_LoadEntities (lump_t *l)
{
	if (!l->filelen)
	{
		loadmodel->entities = NULL;
		return;
	}
	loadmodel->entities = Hunk_AllocName ( l->filelen + 1, "modent"); // +1 for extra NUL below
	memcpy (loadmodel->entities, mod_base + l->fileofs, l->filelen);
	loadmodel->entities[l->filelen] = 0;

	if (loadmodel->entities[l->filelen - 1] != 0)
	{
		Con_SafePrintf ("\x02Mod_LoadEntities: ");
		Con_Printf ("missing NUL in entity lump in %s\n", loadmodel->name);
	}
}

/*
=================
Mod_LoadVertexes
=================
*/
void Mod_LoadVertexes (lump_t *l)
{
	dvertex_t	*in;
	mvertex_t	*out;
	int		i, j, count;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadVertexes", l, sizeof(*in));
	count = l->filelen / sizeof(*in);

	if (count > 65535)
	{
		Con_SafePrintf ("\x02Mod_LoadVertexes: ");
		Con_Printf ("excessive vertexes (%d, normal max = %d) in %s\n", count, 65535, loadmodel->name);
	}

	out = Hunk_AllocName ( count*sizeof(*out), "modvert");

	loadmodel->vertexes = out;
	loadmodel->numvertexes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j = 0; j < 3; ++j)
		{
			out->position[j] = LittleFloat (in->point[j]);

			// Check bounds
			if (fabs(out->position[j]) > 65536)
//				Sys_Error ("Mod_LoadVertexes: vertex way out of bounds (%.0f, max = %d) in %s", out->position[j], 4096, loadmodel->name);
			{
				Con_SafePrintf ("\x02Mod_LoadVertexes: ");
				Con_SafePrintf ("vertex %d,%d way out of bounds (%.0f, max = %d) in %s\n", i, j, out->position[j], 4096, loadmodel->name);
				out->position[j] = 0; // OK?
			}
		}
	}
}

/*
=================
Mod_LoadSubmodels
=================
*/
void Mod_LoadSubmodels (lump_t *l)
{
	dmodel_t	*in;
	dmodel_t	*out;
	int		i, j, count, firstface;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadSubmodels", l, sizeof(*in));
	count = l->filelen / sizeof(*in);

	if (count > MAX_MODELS) // Why not MAX_MAP_MODELS?
		Sys_Error ("Mod_LoadSubmodels: too many models (%d, max = %d) in %s", count, MAX_MODELS, loadmodel->name);

	if (count > 256) // Old limit
	{
		Con_SafePrintf ("\x02Mod_LoadSubmodels: ");
		Con_Printf ("excessive models (%d, normal max = %d) in %s\n", count, 256, loadmodel->name);
	}

	out = Hunk_AllocName ( count*sizeof(*out), "modsub");

	loadmodel->submodels = out;
	loadmodel->numsubmodels = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{	// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (in->mins[j]) - 1;
			out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
			out->origin[j] = LittleFloat (in->origin[j]);
		}
		for (j=0 ; j<MAX_MAP_HULLS ; j++)
			out->headnode[j] = LittleLong (in->headnode[j]);
		out->visleafs = LittleLong (in->visleafs);

		firstface = LittleLong (in->firstface);
		out->numfaces = LittleLong (in->numfaces);

		// Check bounds
		Mod_ChkBounds ("Mod_LoadSubmodels", "face", firstface, out->numfaces, loadmodel->numsurfaces);

		out->firstface = firstface;
	}

	// Check world visleafs
	out = loadmodel->submodels;

	if (out->visleafs > MAX_MAP_LEAFS)
		Sys_Error ("Mod_LoadSubmodels: too many visleafs (%d, max = %d) in %s", out->visleafs, MAX_MAP_LEAFS, loadmodel->name);

	if (out->visleafs > 8192) // Old limit
	{
		Con_SafePrintf ("\x02Mod_LoadSubmodels: ");
		Con_Printf ("excessive visleafs (%d, normal max = %d) in %s\n", out->visleafs, 8192, loadmodel->name);
	}
}

/*
=================
Mod_LoadEdges
=================
*/
void Mod_LoadEdges (lump_t *l)
{
	dedge_t	*in;
	medge_t *out;
	int 	i, j, count;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadEdges", l, sizeof(*in));
	count = l->filelen / sizeof(*in);
	out = Hunk_AllocName ( (count + 1) * sizeof(*out), "modedge");

	loadmodel->edges = out;
	loadmodel->numedges = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j = 0; j < 2; ++j)
		{
			out->v[j] = (unsigned short)LittleShort (in->v[j]);

			if (i == 0 && j == 0)
				continue; // Why is sometimes this edge invalid?

			// Check bounds
			if (out->v[j] >= loadmodel->numvertexes)
			{
				Con_SafePrintf ("\x02Mod_LoadEdges: ");
				Con_SafePrintf ("vertex %d in edge %d out of bounds (%d, max = %d) in %s\n", j, i, out->v[j], loadmodel->numvertexes, loadmodel->name);
				out->v[j] = 0; // OK?
			}
		}
	}
}

/*
=================
Mod_LoadTexinfo
=================
*/
void Mod_LoadTexinfo (lump_t *l)
{
	texinfo_t	*in;
	mtexinfo_t	*out;
	int		i, j, count;
	int		miptex;
	float		len1, len2;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadTexinfo", l, sizeof(*in));
	count = l->filelen / sizeof(*in);

	if (count > 32767)
	{
		Con_SafePrintf ("\x02Mod_LoadTexinfo: ");
		Con_Printf ("excessive texinfo (%d, normal max = %d) in %s\n", count, 32767, loadmodel->name);
	}

	out = Hunk_AllocName ( count*sizeof(*out), "modtxinf");

	loadmodel->texinfo = out;
	loadmodel->numtexinfo = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<8 ; j++)
			out->vecs[0][j] = LittleFloat (in->vecs[0][j]);
		len1 = Length (out->vecs[0]);
		len2 = Length (out->vecs[1]);
		len1 = (len1 + len2)/2;
		if (len1 < 0.32)
			out->mipadjust = 4;
		else if (len1 < 0.49)
			out->mipadjust = 3;
		else if (len1 < 0.99)
			out->mipadjust = 2;
		else
			out->mipadjust = 1;
#if 0
		if (len1 + len2 < 0.001)
			out->mipadjust = 1;		// don't crash
		else
			out->mipadjust = 1 / floor( (len1+len2)/2 + 0.1 );
#endif

		miptex = LittleLong (in->miptex);
		out->flags = LittleLong (in->flags);

		if (!loadmodel->textures)
		{
			out->texture = r_notexture_mip;	// checkerboard texture
			out->flags = 0;
		}
		else
		{
			Mod_ChkBounds ("Mod_LoadTexinfo", "miptex", miptex, 1, loadmodel->numtextures);
			out->texture = loadmodel->textures[miptex];
			if (!out->texture)
			{
				out->texture = r_notexture_mip; // texture not found
				out->flags = 0;
			}
		}
	}

	if (MissTex > 0)
	{
		Con_SafePrintf ("\x02Mod_LoadTexinfo: ");
		Con_Printf ("%d texture%s missing in %s\n", MissTex, MissTex == 1 ? " is" : "s are", loadmodel->name);
	}

	if (!loadmodel->textures || MissTex > 0)
	{
		texture_mode = GL_LINEAR_MIPMAP_NEAREST; //_LINEAR;
		r_notexture_mip->gl_texturenum = GL_LoadTexture (r_notexture_mip->name, r_notexture_mip->width, r_notexture_mip->height, (byte *)(r_notexture_mip+1), true, false, 1, false);
		texture_mode = GL_LINEAR;
	}
}

#define MAX_SURF_EXTENTS 512

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
void CalcSurfaceExtents (msurface_t *s)
{
	float		mins[2], maxs[2], val;
	int		i,j, e;
	mvertex_t	*v;
	mtexinfo_t	*tex;
	int		bmins[2], bmaxs[2];

	mins[0] = mins[1] = 999999;
	maxs[0] = maxs[1] = -99999;

	tex = s->texinfo;

	for (i=0 ; i<s->numedges ; i++)
	{
		e = loadmodel->surfedges[s->firstedge+i];
		if (e >= 0)
			v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
		else
			v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];

		for (j=0 ; j<2 ; j++)
		{
			val = v->position[0] * tex->vecs[j][0] +
				v->position[1] * tex->vecs[j][1] +
				v->position[2] * tex->vecs[j][2] +
				tex->vecs[j][3];
			if (val < mins[j])
				mins[j] = val;
			if (val > maxs[j])
				maxs[j] = val;
		}
	}

	for (i=0 ; i<2 ; i++)
	{
		bmins[i] = floor(mins[i]/16);
		bmaxs[i] = ceil(maxs[i]/16);

		s->texturemins[i] = bmins[i] * 16;
		s->extents[i] = (bmaxs[i] - bmins[i]) * 16;
		if ( !(tex->flags & TEX_SPECIAL) && s->extents[i] > MAX_SURF_EXTENTS /* 256 */ )
		{
			Con_SafePrintf ("\002CalcSurfaceExtents: ");
			Con_SafePrintf ("excessive surface extents (%d, normal max = %d), texture %s in %s\n", s->extents[i], MAX_SURF_EXTENTS, tex->texture->name, loadmodel->name);
			s->extents[i] = MAX_SURF_EXTENTS; // Kludge, but seems to work
		}
	}
}


/*
=================
Mod_LoadFaces
=================
*/
void Mod_LoadFaces (lump_t *l, int lightdatasize)
{
	dface_t		*in;
	msurface_t 	*out;
	int		i, count, surfnum, texinfo, firstsurfedge;
	int		planenum, side;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadFaces", l, sizeof(*in));
	count = l->filelen / sizeof(*in);

	if (count > 32767)
	{
		Con_SafePrintf ("\x02Mod_LoadFaces: ");
		Con_Printf ("excessive faces (%d, normal max = %d) in %s\n", count, 32767, loadmodel->name);
	}

	out = Hunk_AllocName ( count*sizeof(*out), "modface");

	loadmodel->surfaces = out;
	loadmodel->numsurfaces = count;

	for ( surfnum=0 ; surfnum<count ; surfnum++, in++, out++)
	{
		firstsurfedge = LittleLong (in->firstedge);
		out->numedges = LittleShort (in->numedges);

		// Check bounds
		Mod_ChkBounds ("MOD_LoadFaces", "surfedge", firstsurfedge, out->numedges, loadmodel->numsurfedges);

		out->firstedge = firstsurfedge;
		out->flags = 0;

		planenum = LittleShort (in->planenum);

		Mod_ChkBounds ("MOD_LoadFaces", "plane", planenum, 1, loadmodel->numplanes);

		side = LittleShort (in->side);
		if (side)
			out->flags |= SURF_PLANEBACK;

		out->plane = loadmodel->planes + planenum;

		texinfo = LittleShort (in->texinfo);

		Mod_ChkBounds ("MOD_LoadFaces", "texinfo", texinfo, 1, loadmodel->numtexinfo);

		out->texinfo = loadmodel->texinfo + texinfo;

		CalcSurfaceExtents (out);

	// lighting info

		for (i=0 ; i<MAXLIGHTMAPS ; i++)
			out->styles[i] = in->styles[i];
		i = LittleLong(in->lightofs);

		if (i != -1)
		{
			if (!WithinBounds (i, 1, lightdatasize, NULL))
			{
				Con_SafePrintf ("\002MOD_LoadFaces: ");
				Con_SafePrintf ("lightofs in face %d out of bounds (%d, max = %d) in %s\n", surfnum, i, lightdatasize, loadmodel->name);
				i = -1;
			}
		}

		if (i == -1)
			out->samples = NULL;
		else
			out->samples = loadmodel->lightdata + i;

	// set the drawing flags flag

		if (!Q_strncmp(out->texinfo->texture->name,"sky",3))	// sky
		{
			out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
#ifndef QUAKE2
			GL_SubdivideSurface (out);	// cut up polygon for warps
#endif
			continue;
		}

		if (out->texinfo->texture->name[0] == '*')		// turbulent
		{
			out->flags |= (SURF_DRAWTURB | SURF_DRAWTILED);
			for (i=0 ; i<2 ; i++)
			{
				out->extents[i] = 16384;
				out->texturemins[i] = -8192;
			}
			GL_SubdivideSurface (out);	// cut up polygon for warps
			continue;
		}

	}
}

static unsigned RecursLevel;

/*
=================
Mod_SetParent
=================
*/
void static Mod_SetParent (mnode_t *node, mnode_t *parent)
{
	if (!node || ++RecursLevel > 4096) // 512 seems enough for huge maps and 8192 might create stack overflow
		Sys_Error ("Mod_SetParent: %s in %s", !node ? "invalid node" : "excessive tree depth", loadmodel->name);

	node->parent = parent;
	if (node->contents >= 0)
	{
		Mod_SetParent (node->children[0], node);
		Mod_SetParent (node->children[1], node);
	}

	--RecursLevel;
}

/*
=================
Mod_LoadNodes
=================
*/
void Mod_LoadNodes (lump_t *l)
{
	int	i, j, count, p, firstface;
	dnode_t	*in;
	mnode_t *out;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadNodes", l, sizeof(*in));
	count = l->filelen / sizeof(*in);

	// Check nodes
	if (count > 32767)
//		Sys_Error ("MOD_LoadNodes: too many nodes (%d, max = %d) in %s", count, 32767, loadmodel->name);
	{
		Con_SafePrintf ("\x02Mod_LoadNodes: ");
		Con_Printf ("excessive nodes (%d, normal max = %d) in %s\n", count, 32767, loadmodel->name);
	}

	out = Hunk_AllocName ( count*sizeof(*out), "modnode");

	loadmodel->nodes = out;
	loadmodel->numnodes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		p = LittleLong (in->planenum);

		Mod_ChkBounds ("MOD_LoadNodes", "plane", p, 1, loadmodel->numplanes);

		out->plane = loadmodel->planes + p;

		firstface = (unsigned short)LittleShort (in->firstface);
		out->numsurfaces = (unsigned short)LittleShort (in->numfaces);

		// Check bounds
		Mod_ChkBounds ("MOD_LoadNodes", "face", firstface, out->numsurfaces, loadmodel->numsurfaces);

		out->firstsurface = firstface;

		for (j=0 ; j<2 ; j++)
		{
			p = LittleShort (in->children[j]);

			if (p < -loadmodel->numleafs)
				p += 65536; // Gross hack to connect as much as possible of the crippled bsp tree

			if (p >= 0)
			{
				if (p >= loadmodel->numnodes)
//					Sys_Error ("MOD_LoadNodes: node out of bounds (%d, max = %d)", p, loadmodel->numnodes);
					p = loadmodel->numnodes - 1;

				out->children[j] = loadmodel->nodes + p;
			}
			else
			{
				if ((-1 - p) >= loadmodel->numleafs)
//					Sys_Error ("MOD_LoadNodes: leaf out of bounds (%d, max = %d)", (-1 - p), loadmodel->numleafs);
					p = -loadmodel->numleafs;

				out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
			}
		}
	}

	RecursLevel = 0;
	Mod_SetParent (loadmodel->nodes, NULL);	// sets nodes and leafs
}

/*
=================
Mod_LoadLeafs
=================
*/
void Mod_LoadLeafs (lump_t *l, int visdatasize)
{
	dleaf_t *in;
	mleaf_t *out;
	int	i, j, count, p, firstmarksurface;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadLeafs", l, sizeof(*in));
	count = l->filelen / sizeof(*in);

	// Check leafs
	if (count > 32767)
		Sys_Error ("MOD_LoadLeafs: too many leafs (%d, max = %d) in %s", count, 32767, loadmodel->name);

	out = Hunk_AllocName ( count*sizeof(*out), "modleaf");

	loadmodel->leafs = out;
	loadmodel->numleafs = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		p = LittleLong(in->contents);
		out->contents = p;

		firstmarksurface = (unsigned short)LittleShort (in->firstmarksurface);
		out->nummarksurfaces = (unsigned short)LittleShort (in->nummarksurfaces);

		// Check bounds
		Mod_ChkBounds ("MOD_LoadLeafs", "marksurfaces", firstmarksurface, out->nummarksurfaces, loadmodel->nummarksurfaces);

		out->firstmarksurface = loadmodel->marksurfaces + firstmarksurface;

		p = LittleLong(in->visofs);
		
		if (p != -1)
		{
			if (!loadmodel->visdata)
				p = -1;
			else if (!WithinBounds (p, 1, visdatasize, NULL))
			{
				if (i > 0) // Don't warn about leaf 0, sometimes bad (?)
				{
					Con_SafePrintf ("\002MOD_LoadLeafs: ");
					Con_SafePrintf ("visofs in leaf %d out of bounds (%d, max = %d) in %s\n", i, p, visdatasize, loadmodel->name);
				}
				p = -1;
			}
		}

		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = loadmodel->visdata + p;
		out->efrags = NULL;

		for (j=0 ; j<4 ; j++)
			out->ambient_sound_level[j] = in->ambient_level[j];

		// gl underwater warp
		if (out->contents != CONTENTS_EMPTY)
		{
			for (j=0 ; j<out->nummarksurfaces ; j++)
				out->firstmarksurface[j]->flags |= SURF_UNDERWATER;
		}
	}
}

/*
=================
Mod_LoadClipnodes
=================
*/
void Mod_LoadClipnodes (lump_t *l)
{
	dclipnode_t	*in, *out;
	int		i, count;
	hull_t		*hull;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadClipnodes", l, sizeof(*in));
	count = l->filelen / sizeof(*in);

	if (count > 32767) // Normal limit
	{
		Con_SafePrintf ("\x02Mod_LoadClipnodes: ");
		Con_Printf ("excessive clipnodes (%d, normal max = %d) in %s\n", count, 32767, loadmodel->name);
	}

	out = Hunk_AllocName ( count*sizeof(*out), "modclip1");

	loadmodel->clipnodes = out;
	loadmodel->numclipnodes = count;

	hull = &loadmodel->hulls[1];
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;
	hull->clip_mins[0] = -16;
	hull->clip_mins[1] = -16;
	hull->clip_mins[2] = -24;
	hull->clip_maxs[0] = 16;
	hull->clip_maxs[1] = 16;
	hull->clip_maxs[2] = 32;

	hull = &loadmodel->hulls[2];
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;
	hull->clip_mins[0] = -32;
	hull->clip_mins[1] = -32;
	hull->clip_mins[2] = -24;
	hull->clip_maxs[0] = 32;
	hull->clip_maxs[1] = 32;
	hull->clip_maxs[2] = 64;

	for (i=0 ; i<count ; i++, out++, in++)
	{
		out->planenum = LittleLong(in->planenum);

		// Check bounds
		if (out->planenum < 0 || out->planenum >= loadmodel->numplanes)
		{
			Con_SafePrintf ("\x02Mod_LoadClipnodes: ");
			Con_SafePrintf ("plane in clipnode %d out of bounds (%d, max = %d) in %s\n", i, out->planenum, loadmodel->numplanes, loadmodel->name);
			out->planenum = 0; // Kludge, but seems to work
		}

		out->children[0] = LittleShort(in->children[0]);
		out->children[1] = LittleShort(in->children[1]);
	}
}

/*
=================
Mod_MakeHull0

Deplicate the drawing hull structure as a clipping hull
=================
*/
void Mod_MakeHull0 (void)
{
	mnode_t		*in, *child;
	dclipnode_t *out;
	int			i, j, count;
	hull_t		*hull;

	hull = &loadmodel->hulls[0];

	in = loadmodel->nodes;
	count = loadmodel->numnodes;
	out = Hunk_AllocName ( count*sizeof(*out), "modclip2");

	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;

	for (i=0 ; i<count ; i++, out++, in++)
	{
		out->planenum = in->plane - loadmodel->planes;
		for (j=0 ; j<2 ; j++)
		{
			child = in->children[j];
			if (child->contents < 0)
				out->children[j] = child->contents;
			else
				out->children[j] = child - loadmodel->nodes;
		}
	}
}

/*
=================
Mod_LoadMarksurfaces
=================
*/
void Mod_LoadMarksurfaces (lump_t *l)
{
	int		i, j, count;
	unsigned short	*in;
	msurface_t	**out;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadMarksurfaces", l, sizeof(*in));
	count = l->filelen / sizeof(*in);

	// Check marksurfaces
//	if (count > 65535)
//		Sys_Error ("Mod_LoadMarksurfaces: too many marksurfaces (%d, max = %d) in %s", count, 65535, loadmodel->name);

	if (count > 32767) // Normal limit
	{
		Con_SafePrintf ("\x02Mod_LoadMarksurfaces: ");
		Con_Printf ("excessive marksurfaces (%d, normal max = %d) in %s\n", count, 32767, loadmodel->name);
	}

	out = Hunk_AllocName ( count*sizeof(*out), "modsurf");

	loadmodel->marksurfaces = out;
	loadmodel->nummarksurfaces = count;

	for ( i=0 ; i<count ; i++)
	{
		j = (unsigned short)LittleShort(in[i]);
		Mod_ChkBounds ("Mod_LoadMarksurfaces", "face", j, 1, loadmodel->numsurfaces);
		out[i] = loadmodel->surfaces + j;
	}
}

/*
=================
Mod_LoadSurfedges
=================
*/
void Mod_LoadSurfedges (lump_t *l)
{
	int	i, count;
	int	*in, *out;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadSurfedges", l, sizeof(*in));
	count = l->filelen / sizeof(*in);
	out = Hunk_AllocName ( count*sizeof(*out), "modsedge");

	loadmodel->surfedges = out;
	loadmodel->numsurfedges = count;

	for ( i=0 ; i<count ; i++)
	{
		out[i] = LittleLong (in[i]);

		// Check bounds
		if (abs(out[i]) >= loadmodel->numedges)
		{
			Con_SafePrintf ("\x02Mod_LoadSurfedges: ");
			Con_SafePrintf ("edge in surfedge %d out of bounds (%d, max = %d) in %s\n", i, out[i], loadmodel->numedges, loadmodel->name);
			out[i] = 0; // OK?
		}
	}
}

/*
=================
Mod_LoadPlanes
=================
*/
void Mod_LoadPlanes (lump_t *l)
{
	int		i, j;
	mplane_t	*out;
	dplane_t 	*in;
	int		count;
	int		bits;

	in = (void *)(mod_base + l->fileofs);
	Mod_ChkLumpSize ("Mod_LoadPlanes", l, sizeof(*in));
	count = l->filelen / sizeof(*in);

	if (count > 32767)
	{
		Con_SafePrintf ("\x02Mod_LoadPlanes: ");
		Con_Printf ("excessive planes (%d, normal max = %d) in %s\n", count, 32767, loadmodel->name);
	}

	out = Hunk_AllocName ( count*2*sizeof(*out), "modplan");

	loadmodel->planes = out;
	loadmodel->numplanes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}

		out->dist = LittleFloat (in->dist);
		out->type = LittleLong (in->type);
		out->signbits = bits;
	}
}

/*
=================
RadiusFromBounds
=================
*/
float RadiusFromBounds (vec3_t mins, vec3_t maxs)
{
	int		i;
	vec3_t	corner;

	for (i=0 ; i<3 ; i++)
	{
		corner[i] = fabs(mins[i]) > fabs(maxs[i]) ? fabs(mins[i]) : fabs(maxs[i]);
	}

	return Length (corner);
}

static char *LumpDesc[] =
{
	"entities",
	"planes",
	"textures",
	"vertexes",
	"visdata",
	"nodes",
	"texinfo",
	"faces",
	"lighting",
	"clipnodes",
	"leafs",
	"marksurfaces",
	"edges",
	"surfedges",
	"models",
};

/*
=================
Mod_LoadBrushModel
=================
*/
void Mod_LoadBrushModel (model_t *mod, void *buffer)
{
	int		i, j;
	dheader_t	*header;
	dmodel_t 	*bm;

	loadmodel->type = mod_brush;

	header = (dheader_t *)buffer;

	// Check header is inside file buffer
	Mod_ChkFileSize ("Mod_LoadBrushModel", "header", 0, sizeof(dheader_t));

	i = LittleLong (header->version);
	if (i != BSPVERSION)
		Sys_Error ("Mod_LoadBrushModel: %s has wrong version number (%i should be %i)", mod->name, i, BSPVERSION);

// swap all the lumps
	mod_base = (byte *)header;

	for (i=0 ; i<sizeof(dheader_t)/4 ; i++)
		((int *)header)[i] = LittleLong ( ((int *)header)[i]);

	// Check all lumps are OK and inside file buffer
	for (i = 0; i < HEADER_LUMPS; ++i)
		Mod_ChkFileSize ("Mod_LoadBrushModel", LumpDesc[i], header->lumps[i].fileofs, header->lumps[i].filelen);

// load into heap

	Mod_LoadVertexes (&header->lumps[LUMP_VERTEXES]);
	Mod_LoadEdges (&header->lumps[LUMP_EDGES]);
	Mod_LoadSurfedges (&header->lumps[LUMP_SURFEDGES]);
	Mod_LoadTextures (&header->lumps[LUMP_TEXTURES]);
	Mod_LoadLighting (&header->lumps[LUMP_LIGHTING]);
	Mod_LoadPlanes (&header->lumps[LUMP_PLANES]);
	Mod_LoadTexinfo (&header->lumps[LUMP_TEXINFO]);
	Mod_LoadFaces (&header->lumps[LUMP_FACES], header->lumps[LUMP_LIGHTING].filelen);
	Mod_LoadMarksurfaces (&header->lumps[LUMP_MARKSURFACES]);
	Mod_LoadVisibility (&header->lumps[LUMP_VISIBILITY]);
	Mod_LoadLeafs (&header->lumps[LUMP_LEAFS], header->lumps[LUMP_VISIBILITY].filelen);
	Mod_LoadNodes (&header->lumps[LUMP_NODES]);
	Mod_LoadClipnodes (&header->lumps[LUMP_CLIPNODES]);
	Mod_LoadEntities (&header->lumps[LUMP_ENTITIES]);
	Mod_LoadSubmodels (&header->lumps[LUMP_MODELS]);

	Mod_MakeHull0 ();

	mod->numframes = 2;		// regular and alternate animation

	Mod_IsWorldModel = false; // Make sure to reset this after world has been loaded

//
// set up the submodels (FIXME: this is confusing)
//
	for (i=0 ; i<mod->numsubmodels ; i++)
	{
		bm = &mod->submodels[i];

		mod->hulls[0].firstclipnode = bm->headnode[0];
		for (j=1 ; j<MAX_MAP_HULLS ; j++)
		{
			mod->hulls[j].firstclipnode = bm->headnode[j];
			mod->hulls[j].lastclipnode = mod->numclipnodes-1;
		}

		mod->firstmodelsurface = bm->firstface;
		mod->nummodelsurfaces = bm->numfaces;

		VectorCopy (bm->maxs, mod->maxs);
		VectorCopy (bm->mins, mod->mins);

		mod->radius = RadiusFromBounds (mod->mins, mod->maxs);

		mod->numleafs = bm->visleafs;

		if (i < mod->numsubmodels-1)
		{	// duplicate the basic information
			char	name[10];

			sprintf (name, "*%i", i+1);
			loadmodel = Mod_FindName (name);
			*loadmodel = *mod;
			strcpy (loadmodel->name, name);
			Mod_AllocClear (loadmodel);
			mod = loadmodel;
		}
	}
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

aliashdr_t	*pheader;

stvert_t	stverts[MAXALIASVERTS];
mtriangle_t	triangles[MAXALIASTRIS];

// a pose is a single set of vertexes.  a frame may be
// an animating sequence of poses
trivertx_t	*poseverts[MAXALIASFRAMES];
int		posenum;

byte		**player_8bit_texels_tbl;
byte		*player_8bit_texels;

int		aliasbboxmins[3], aliasbboxmaxs[3]; // proper alias model bboxes

/*
=================
Mod_LoadAliasFrame
=================
*/
void * Mod_LoadAliasFrame (void * pin, maliasframedesc_t *frame)
{
	trivertx_t	*pframe, *pinframe;
	int		i, j;
	daliasframe_t	*pdaliasframe;

	pdaliasframe = (daliasframe_t *)pin;

	// Check frame is inside file buffer
	Mod_ChkFileSize ("Mod_LoadAliasFrame", "frame", (byte *)pdaliasframe - mod_base, sizeof(daliasframe_t));

	strcpy (frame->name, pdaliasframe->name);
	frame->firstpose = posenum;
	frame->numposes = 1;

	for (i=0 ; i<3 ; i++)
	{
	// these are byte values, so we don't have to worry about
	// endianness
		frame->bboxmin.v[i] = pdaliasframe->bboxmin.v[i];
		frame->bboxmax.v[i] = pdaliasframe->bboxmax.v[i];

		aliasbboxmins[i] = min (frame->bboxmin.v[i], aliasbboxmins[i]);
		aliasbboxmaxs[i] = max (frame->bboxmax.v[i], aliasbboxmaxs[i]);
	}

	pinframe = (trivertx_t *)(pdaliasframe + 1);

	// Check trivertex is inside file buffer
	Mod_ChkFileSize ("Mod_LoadAliasFrame", "trivertex", (byte *)pinframe - mod_base, sizeof(trivertx_t));

	if (posenum >= MAXALIASFRAMES)
		Sys_Error ("Mod_LoadAliasFrame: invalid # of frames (%d, max = %d) in %s", posenum, MAXALIASFRAMES, loadmodel->name);

	poseverts[posenum] = pinframe;
	posenum++;

	pinframe += pheader->numverts;

	return (void *)pinframe;
}


/*
=================
Mod_LoadAliasGroup
=================
*/
void *Mod_LoadAliasGroup (void * pin,  maliasframedesc_t *frame)
{
	daliasgroup_t		*pingroup;
	int			i, numframes;
	daliasinterval_t	*pin_intervals;
	void			*ptemp;

	pingroup = (daliasgroup_t *)pin;

	// Check group is inside file buffer
	Mod_ChkFileSize ("Mod_LoadAliasGroup", "group", (byte *)pingroup - mod_base, sizeof(daliasgroup_t));

	numframes = LittleLong (pingroup->numframes);

	frame->firstpose = posenum;
	frame->numposes = numframes;

	for (i=0 ; i<3 ; i++)
	{
	// these are byte values, so we don't have to worry about endianness
		frame->bboxmin.v[i] = pingroup->bboxmin.v[i];
		frame->bboxmax.v[i] = pingroup->bboxmax.v[i];

		aliasbboxmins[i] = min (frame->bboxmin.v[i], aliasbboxmins[i]);
		aliasbboxmaxs[i] = max (frame->bboxmax.v[i], aliasbboxmaxs[i]);
	}

	pin_intervals = (daliasinterval_t *)(pingroup + 1);

	// Check intervals are inside file buffer
	Mod_ChkFileSize ("Mod_LoadAliasGroup", "intervals", (byte *)pin_intervals - mod_base, numframes * sizeof(daliasinterval_t));

	frame->interval = LittleFloat (pin_intervals->interval);

	pin_intervals += numframes;

	ptemp = (void *)pin_intervals;

	for (i=0 ; i<numframes ; i++)
	{
		// Check trivertex is inside file buffer
		Mod_ChkFileSize ("Mod_LoadAliasGroup", "trivertex", (byte *)((daliasframe_t *)ptemp + 1) - mod_base, sizeof(trivertx_t));

		if (posenum >= MAXALIASFRAMES)
			Sys_Error ("Mod_LoadAliasGroup: invalid # of frames (%d, max = %d) in %s", posenum, MAXALIASFRAMES, loadmodel->name);

		poseverts[posenum] = (trivertx_t *)((daliasframe_t *)ptemp + 1);
		posenum++;

		ptemp = (trivertx_t *)((daliasframe_t *)ptemp + 1) + pheader->numverts;
	}

	return ptemp;
}

//=========================================================

/*
=================
Mod_FloodFillSkin

Fill background pixels so mipmapping doesn't have haloes - Ed
=================
*/

typedef struct
{
	short		x, y;
} floodfill_t;

extern unsigned d_8to24table[];

// must be a power of 2
#define FLOODFILL_FIFO_SIZE 0x1000
#define FLOODFILL_FIFO_MASK (FLOODFILL_FIFO_SIZE - 1)

#define FLOODFILL_STEP( off, dx, dy ) \
{ \
	if (pos[off] == fillcolor) \
	{ \
		pos[off] = 255; \
		fifo[inpt].x = x + (dx), fifo[inpt].y = y + (dy); \
		inpt = (inpt + 1) & FLOODFILL_FIFO_MASK; \
	} \
	else if (pos[off] != 255) fdc = pos[off]; \
}

void Mod_FloodFillSkin( byte *skin, int skinwidth, int skinheight )
{
	byte		fillcolor = *skin; // assume this is the pixel to fill
	floodfill_t	fifo[FLOODFILL_FIFO_SIZE];
	int		inpt = 0, outpt = 0;
	int		filledcolor = -1;
	int		i, size = skinwidth * skinheight, notfill;

	if (filledcolor == -1)
	{
		filledcolor = 0;
		// attempt to find opaque black
		for (i = 0; i < 256; ++i)
			if (d_8to24table[i] == (255 << 0)) // alpha 1.0
			{
				filledcolor = i;
				break;
			}
	}

	// can't fill to filled color or to transparent color (used as visited marker)
	if ((fillcolor == filledcolor) || (fillcolor == 255))
	{
		//printf( "not filling skin from %d to %d\n", fillcolor, filledcolor );
		return;
	}

	for (i = notfill = 0; i < size && notfill < 2; ++i)
	{
		if (skin[i] != fillcolor)
			++notfill;
	}

	// Don't fill almost mono-coloured texes
	if (notfill < 2)
	{
//		Con_SafePrintf ("\002Mod_FloodFillSkin: ");
//		Con_SafePrintf ("not filling skin in %s\n", loadmodel->name);
		return;
	}

	fifo[inpt].x = 0, fifo[inpt].y = 0;
	inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;

	while (outpt != inpt)
	{
		int			x = fifo[outpt].x, y = fifo[outpt].y;
		int			fdc = filledcolor;
		byte		*pos = &skin[x + skinwidth * y];

		outpt = (outpt + 1) & FLOODFILL_FIFO_MASK;

		if (x > 0)				FLOODFILL_STEP( -1, -1, 0 );
		if (x < skinwidth - 1)	FLOODFILL_STEP( 1, 1, 0 );
		if (y > 0)				FLOODFILL_STEP( -skinwidth, 0, -1 );
		if (y < skinheight - 1)	FLOODFILL_STEP( skinwidth, 0, 1 );
		skin[x + skinwidth * y] = fdc;
	}
}

/*
===============
Mod_LoadAllSkins
===============
*/
void *Mod_LoadAllSkins (int numskins, daliasskintype_t *pskintype)
{
	int			i, j, k;
	int			s;
	byte			*copy;
	byte			*skin;
	byte			*texels;
	daliasskingroup_t	*pinskingroup;
	int			groupskins;
	daliasskininterval_t	*pinskinintervals;

	if (numskins < 1 || numskins > MAX_SKINS)
		Sys_Error ("Mod_LoadAllSkins: invalid # of skins (%d, max = %d) in %s", numskins, MAX_SKINS, loadmodel->name);

	s = pheader->skinwidth * pheader->skinheight;

	for (i=0 ; i<numskins ; i++)
	{
		char Str[256];

		skin = (byte *)(pskintype + 1);

		sprintf (Str, "skin %d", i);
		Mod_ChkFileSize ("Mod_LoadAllSkins", Str, (byte *)pskintype - mod_base, sizeof(daliasskintype_t));

		Mod_ChkType ("Mod_LoadAllSkins", "skintype", Str, pskintype->type, ALIAS_SKIN_SINGLE, ALIAS_SKIN_GROUP);

		if (pskintype->type == ALIAS_SKIN_SINGLE) {
			Mod_ChkFileSize ("Mod_LoadAllSkins", Str, skin - mod_base, s);

			Mod_FloodFillSkin( skin, pheader->skinwidth, pheader->skinheight );

			// save 8 bit texels for the player model to remap
	//		if (!strcmp(loadmodel->name,"progs/player.mdl")) {
				texels = Hunk_AllocName(s, "modskin1");
				pheader->texels[i] = texels - (byte *)pheader;
				memcpy (texels, skin, s);
	//		}
			
			pheader->gl_texturenum[i][0] =
			pheader->gl_texturenum[i][1] =
			pheader->gl_texturenum[i][2] =
			pheader->gl_texturenum[i][3] = Mod_LoadExtTex (loadmodel->name, pheader->skinwidth, pheader->skinheight, skin, false, false, i, -1, 1);
			pskintype = (daliasskintype_t *)(skin + s);
		}
		else
		{
			// animating skin group.  yuck.
			pskintype++;
			pinskingroup = (daliasskingroup_t *)pskintype;

			sprintf (Str, "group %d", i);
			Mod_ChkFileSize ("Mod_LoadAllSkins", Str, (byte *)pinskingroup - mod_base, sizeof(daliasskingroup_t));

			groupskins = LittleLong (pinskingroup->numskins);
			pinskinintervals = (daliasskininterval_t *)(pinskingroup + 1);

			pskintype = (void *)(pinskinintervals + groupskins);

			for (j=0 ; j<groupskins ; j++)
			{
				sprintf (Str, "group %d, skin %d", i, j);
				Mod_ChkFileSize ("Mod_LoadAllSkins", Str, (byte *)pskintype - mod_base, s);

				Mod_FloodFillSkin( skin, pheader->skinwidth, pheader->skinheight ); // Is 'skin' really correct here?
				if (j == 0) {
					texels = Hunk_AllocName(s, "modskin2");
					pheader->texels[i] = texels - (byte *)pheader;
					memcpy (texels, (byte *)(pskintype), s);
				}
				pheader->gl_texturenum[i][j&3] = Mod_LoadExtTex (loadmodel->name, pheader->skinwidth, pheader->skinheight, (byte *)(pskintype), false, false, i, j, 1);
				pskintype = (daliasskintype_t *)((byte *)(pskintype) + s);
			}
			k = j;
			for (/* */; j < 4; j++)
				pheader->gl_texturenum[i][j&3] =
				pheader->gl_texturenum[i][j - k];
		}
	}

	return (void *)pskintype;
}

//=========================================================================

/*
=================
Mod_LoadAliasModel
=================
*/
void Mod_LoadAliasModel (model_t *mod, void *buffer)
{
	int		    i, j;
	mdl_t		    *pinmodel;
	stvert_t	    *pinstverts;
	dtriangle_t	    *pintriangles;
	int		    version, numframes, numskins;
	int		    size;
	daliasframetype_t   *pframetype;
	daliasskintype_t    *pskintype;
	int		    start, end, total;
	char		    *Name;

	// HACK HACK HACK -- no fullbright colors, so make torches/flames full light

	Name = strrchr (mod->name, '/');

	if (Name)
	{
		int autobright = 0, abstart = 0;

		++Name;

		if (!strcmp (mod->name, "progs/flame.mdl") ||
		    !strcmp (mod->name, "progs/flame2.mdl"))
			mod->fullbright = mod->noshadow = true; // GLQuake compatibility
		else if (!strcmp (mod->name, "progs/eyes.mdl"))
			mod->eyes = true;
		else if (!strcmp (mod->name, "progs/bolt.mdl"))
			mod->noshadow = true;
		else if (!strcmp (mod->name, "progs/missile.mdl"))
			mod->noshadow = true;
		else if (!strncmp (Name, "flame", 5) ||
			 !strncmp (Name, "torch", 5) ||
			 !strcmp (Name, "newfire.mdl") ||   // For Kinn ...
			 !strcmp (Name, "longtrch.mdl") ||  // For Chapters ...
			 !strcmp (Name, "bm_reap.mdl"))	    // For Neh Revamp ...
		{
			mod->noshadow = true;
			autobright = 256;
		}
		else if (!strncmp (Name, "lantern", 7) ||
			 !strcmp (Name, "brazshrt.mdl") ||  // For Chapters ...
			 !strcmp (Name, "braztall.mdl"))
		{
			mod->noshadow = true;
			autobright = 150;
		}
		else if (!strncmp (Name, "bolt", 4) ||	    // Bolts ...
			 !strcmp (Name, "s_light.mdl"))
		{
			mod->noshadow = true;
			mod->autobright[abstart++] = 100;
			autobright = 150;
		}
		else if (!strncmp (Name, "candle", 6))
		{
			mod->noshadow = true;
			autobright = 125;
		}
		else if (!strcmp (&Name[1], "_g_key.mdl") ||// Keys ...
			 !strcmp (&Name[1], "_s_key.mdl") ||
			 !strcmp (Name, "bloodkey.mdl") ||  // Nehahra
			 !strcmp (Name, "catkey.mdl") ||
			 !strcmp (Name, "spikekey.mdl") ||
			 !strcmp (Name, "end1.mdl") ||
			 !strcmp (Name, "end2.mdl") ||
			 !strcmp (Name, "end3.mdl") ||
			 !strcmp (Name, "end4.mdl"))
			autobright = 125;
		else if (!strcmp (Name, "necro.mdl") && nehahra ||
			 !strcmp (Name, "wizard.mdl") ||
			 !strcmp (Name, "wraith.mdl"))	    // Nehahra
			mod->nolerp = mod->noshadow = true;
		else if (!strcmp (Name, "beam.mdl") ||	    // Rogue
			 !strcmp (Name, "dragon.mdl") ||    // Rogue
			 !strcmp (Name, "eel2.mdl") ||	    // Rogue
			 !strcmp (Name, "fish.mdl") ||
			 !strcmp (Name, "flak.mdl") ||	    // Marcher
			 (Name[0] != 'v' && !strcmp (&Name[1], "_spike.mdl")) ||
			 !strcmp (Name, "imp.mdl") ||
			 !strcmp (Name, "laser.mdl") ||
			 !strcmp (Name, "lasrspik.mdl") ||  // Hipnotic
			 !strcmp (Name, "lspike.mdl") ||    // Rogue
			 !strncmp (Name, "plasma", 6) ||    // Rogue
			 !strcmp (Name, "spike.mdl") ||
			 !strncmp (Name, "tree", 4) ||
			 !strcmp (Name, "wr_spike.mdl"))    // Nehahra
			mod->noshadow = true;

		if (autobright)
		{
			for (i = abstart; i < AUTOBRIGHTS; ++i)
				mod->autobright[i] = autobright;

//			Con_DPrintf ("\002autobright ");
//			Con_DPrintf ("%s (level %d)\n", mod->name, autobright);
		}
	}

	start = Hunk_LowMark ();

	mod_base = (byte *)buffer;
	pinmodel = (mdl_t *)buffer;

	// Check version+header is inside file buffer
	Mod_ChkFileSize ("Mod_LoadAliasModel", "model header", 0, sizeof(int) + sizeof(mdl_t));

	version = LittleLong (pinmodel->version);
	if (version != ALIAS_VERSION)
		Sys_Error ("Mod_LoadAliasModel: %s has wrong version number (%i should be %i)",
				 mod->name, version, ALIAS_VERSION);

//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
	size = 	sizeof (aliashdr_t)
			+ (LittleLong (pinmodel->numframes) - 1) *
			sizeof (pheader->frames[0]);
	pheader = Hunk_AllocName (size, "modmdl");

	mod->flags = LittleLong (pinmodel->flags);

//
// endian-adjust and copy the data, starting with the alias model header
//
	pheader->boundingradius = LittleFloat (pinmodel->boundingradius);
	pheader->numskins = LittleLong (pinmodel->numskins);
	pheader->skinwidth = LittleLong (pinmodel->skinwidth);
	pheader->skinheight = LittleLong (pinmodel->skinheight);

	if (pheader->skinheight > MAX_LBM_HEIGHT)
		Sys_Error ("Mod_LoadAliasModel: model %s has a skin taller than %d (%d)", mod->name,
				   MAX_LBM_HEIGHT, pheader->skinheight);

	if (pheader->skinheight > 480)
	{
		// Old limit
		Con_SafePrintf ("\002Mod_LoadAliasModel: ");
		Con_Printf ("excessive skin height (%d, normal max = %d) in %s\n", pheader->skinheight, 480, mod->name);
	}

	pheader->numverts = LittleLong (pinmodel->numverts);

	if (pheader->numverts <= 0)
		Sys_Error ("Mod_LoadAliasModel: model %s has no vertices", mod->name);

	if (pheader->numverts > MAXALIASVERTS)
		Sys_Error ("Mod_LoadAliasModel: model %s has too many vertices (%d, max = %d)", mod->name, pheader->numverts, MAXALIASVERTS);

	if (pheader->numverts > 1024 && developer.value > 2)
	{
		// Old limit
		Con_SafePrintf ("\002Mod_LoadAliasModel: ");
		Con_Printf ("excessive vertices (%d, normal max = %d) in %s\n", pheader->numverts, 1024, mod->name);
	}

	pheader->numtris = LittleLong (pinmodel->numtris);

	if (pheader->numtris <= 0)
		Sys_Error ("Mod_LoadAliasModel: model %s has no triangles", mod->name);

	if (pheader->numtris > MAXALIASTRIS)
		Sys_Error ("Mod_LoadAliasModel: model %s has too many triangles (%d, max = %d)", mod->name, pheader->numtris, MAXALIASTRIS);

	if (pheader->numtris > 2048)
	{
		// Old limit
		Con_SafePrintf ("\002Mod_LoadAliasModel: ");
		Con_Printf ("excessive triangles (%d, normal max = %d) in %s\n", pheader->numtris, 2048, mod->name);
	}

	pheader->numframes = LittleLong (pinmodel->numframes);

	if (pheader->numframes > MAXALIASFRAMES)
	{
		Con_SafePrintf ("\002Mod_LoadAliasModel: ");
		Con_Printf ("too many frames (%d, max = %d) in %s\n", pheader->numframes, MAXALIASFRAMES, mod->name);
		pheader->numframes = MAXALIASFRAMES; // Cap
	}

	numframes = pheader->numframes;
	if (numframes < 1)
		Sys_Error ("Mod_LoadAliasModel: invalid # of frames %d in %s", numframes, mod->name);

	pheader->size = LittleFloat (pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
	mod->synctype = LittleLong (pinmodel->synctype);
	mod->numframes = pheader->numframes;

	for (i=0 ; i<3 ; i++)
	{
		pheader->scale[i] = LittleFloat (pinmodel->scale[i]);
		pheader->scale_origin[i] = LittleFloat (pinmodel->scale_origin[i]);
		pheader->eyeposition[i] = LittleFloat (pinmodel->eyeposition[i]);
	}

//	if (pheader->skinwidth & 0x03)
//		Sys_Error ("Mod_LoadAliasModel: skinwidth %d is not a multiple of 4 in %s", pheader->skinwidth, mod->name);

//
// load the skins
//
	pskintype = (daliasskintype_t *)&pinmodel[1];
	pskintype = Mod_LoadAllSkins (pheader->numskins, pskintype);

	if (!mod->fullbright && !mod->autobright[0])
	{
		byte *skin = (byte *)((daliasskintype_t *)&pinmodel[1] + 1);
		int  s = pheader->skinwidth * pheader->skinheight;
		int  FullBrights, FullBrights2, Yellow, Blue;

		Yellow = Blue = 0;

		for (i = 0; i < s; ++i)
		{
			if (skin[i] >= 224 && skin[i] <= 254)
			{
				if (skin[i] >= 244 && skin[i] <= 246)
					++Blue;
				else
					++Yellow;
			}
		}

		for (i = 0; i < AUTOBRIGHTS; ++i)
		{
			// gl_autobright == 0 : GLQuake compatibility
			// gl_autobright == 1 : default, count mostly yellow/red fullbrights
			// gl_autobright == 2 : count all fullbrights
			// gl_autobright == 3 : count all fullbrights, high intensity
			if (i == 0)
			{
				FullBrights = Yellow;
				FullBrights2 = Blue;
			}
			else
			{
				FullBrights = Yellow + Blue;
				FullBrights2 = 0;
			}

			FullBrights *= 100.0 / s;
			FullBrights2 *= 100.0 / s;

			if (FullBrights > 75)
				mod->autobright[i] = 150;
			else if (FullBrights > 50)
				mod->autobright[i] = 125;
			else if (FullBrights2 > 75)
				mod->autobright[i] = 100;

			if (mod->autobright[i])
			{
				mod->noshadow = true;
//				Con_DPrintf ("\002autobright ");
//				Con_DPrintf ("%s (type %d, level %d, fb %d, fb2 %d)\n", mod->name, i, mod->autobright[i], FullBrights, FullBrights2);
			}
		}
	}

//
// load base s and t vertices
//
	pinstverts = (stvert_t *)pskintype;

	// Check vertices are inside file buffer
	Mod_ChkFileSize ("Mod_LoadAliasModel", "vertices", (byte *)pinstverts - mod_base, pheader->numverts * sizeof(stvert_t));

	for (i=0 ; i<pheader->numverts ; i++)
	{
		stverts[i].onseam = LittleLong (pinstverts[i].onseam);
		stverts[i].s = LittleLong (pinstverts[i].s);
		stverts[i].t = LittleLong (pinstverts[i].t);
	}

//
// load triangle lists
//
	pintriangles = (dtriangle_t *)&pinstverts[pheader->numverts];

	// Check triangles are inside file buffer
	Mod_ChkFileSize ("Mod_LoadAliasModel", "triangles", (byte *)pintriangles - mod_base, pheader->numtris * sizeof(dtriangle_t));

	for (i=0 ; i<pheader->numtris ; i++)
	{
		triangles[i].facesfront = LittleLong (pintriangles[i].facesfront);

		for (j=0 ; j<3 ; j++)
		{
			triangles[i].vertindex[j] =
					LittleLong (pintriangles[i].vertindex[j]);

			if (triangles[i].vertindex[j] < 0 || triangles[i].vertindex[j] >= MAXALIASVERTS)
				Sys_Error ("Mod_LoadAliasModel: invalid triangles[%d].vertindex[%d] (%d, max = %d) in %s", i, j, triangles[i].vertindex[j], MAXALIASVERTS, mod->name);
		}
	}

//
// load the frames
//
	posenum = 0;
	pframetype = (daliasframetype_t *)&pintriangles[pheader->numtris];

	aliasbboxmins[0] = aliasbboxmins[1] = aliasbboxmins[2] =  99999;
	aliasbboxmaxs[0] = aliasbboxmaxs[1] = aliasbboxmaxs[2] = -99999;

	for (i=0 ; i<numframes ; i++)
	{
		aliasframetype_t frametype;
		char		 Str[256];

		sprintf (Str, "frame %d", i);
		Mod_ChkFileSize ("Mod_LoadAliasModel", Str, (byte *)pframetype - mod_base, sizeof(aliasframetype_t));

		frametype = LittleLong (pframetype->type);

		Mod_ChkType ("Mod_LoadAliasModel", "frametype", Str, frametype, ALIAS_SINGLE, ALIAS_GROUP);

		if (frametype == ALIAS_SINGLE)
		{
			pframetype = (daliasframetype_t *)
					Mod_LoadAliasFrame (pframetype + 1, &pheader->frames[i]);
		}
		else
		{
			pframetype = (daliasframetype_t *)
					Mod_LoadAliasGroup (pframetype + 1, &pheader->frames[i]);
		}
	}

	pheader->numposes = posenum;

	mod->type = mod_alias;

	if (!strcmp(mod->name, "models/null.mdl"))
	{
		// Otherwise the player in QRally/GoldCup is stuck
		mod->mins[0] = mod->mins[1] = mod->mins[2] = -16;
		mod->maxs[0] = mod->maxs[1] = mod->maxs[2] = 16;
	}
	else
	{
		// proper alias model bboxes
		for (i = 0; i < 3; i++)
		{
			mod->mins[i] = min (aliasbboxmins[i] * pheader->scale[i] + pheader->scale_origin[i], -16);
			mod->maxs[i] = max (aliasbboxmaxs[i] * pheader->scale[i] + pheader->scale_origin[i], 16);
		}
	}

	//
	// build the draw lists
	//
	GL_MakeAliasModelDisplayLists (mod, pheader);

	Cache_Excess ("Mod_LoadAliasModel", "mdl", &mod->loadtimes, mod->name);

//
// move the complete, relocatable alias model to the cache
//
	end = Hunk_LowMark ();
	total = end - start;

	mod->size = total;

	Cache_Alloc (&mod->cache, total, loadname);
	if (!mod->cache.data)
		return;
	memcpy (mod->cache.data, pheader, total);

	Hunk_FreeToLowMark (start);
}

//=============================================================================

/*
=================
Mod_LoadSpriteFrame
=================
*/
void * Mod_LoadSpriteFrame (void * pin, mspriteframe_t **ppframe, int framenum, int bytes, int group)
{
	dspriteframe_t		*pinframe;
	mspriteframe_t		*pspriteframe;
	int			i, width, height, size, origin[2];
	unsigned short		*ppixout;
	byte			*ppixin;
	char			name[64], str[256];

	pinframe = (dspriteframe_t *)pin;

	// Check sprite frame is inside file buffer
	sprintf (str, "frame %d", framenum);
	Mod_ChkFileSize ("Mod_LoadSpriteFrame", str, (byte *)pinframe - mod_base, sizeof(dspriteframe_t));

	width = LittleLong (pinframe->width);
	height = LittleLong (pinframe->height);
	size = width * height * (gl_oldspr32.value ? 1 : bytes);

	Mod_ChkFileSize ("Mod_LoadSpriteFrame", str, (byte *)(pinframe + 1) - mod_base, size);

	pspriteframe = Hunk_AllocName (sizeof (mspriteframe_t), "modspr2");

	Q_memset (pspriteframe, 0, sizeof (mspriteframe_t));

	*ppframe = pspriteframe;

	pspriteframe->width = width;
	pspriteframe->height = height;
	origin[0] = LittleLong (pinframe->origin[0]);
	origin[1] = LittleLong (pinframe->origin[1]);

	pspriteframe->up = origin[1];
	pspriteframe->down = origin[1] - height;
	pspriteframe->left = origin[0];
	pspriteframe->right = width + origin[0];

	sprintf (name, "%s_%i", loadmodel->name, framenum);
	pspriteframe->gl_texturenum = Mod_LoadExtTex (loadmodel->name, width, height, (byte *)(pinframe + 1), false, true, framenum, group, bytes);

	return (void *)((byte *)pinframe + sizeof (dspriteframe_t) + size);
}


/*
=================
Mod_LoadSpriteGroup
=================
*/
void * Mod_LoadSpriteGroup (void * pin, mspriteframe_t **ppframe, int framenum, int bytes)
{
	dspritegroup_t		*pingroup;
	mspritegroup_t		*pspritegroup;
	int			i, numframes;
	dspriteinterval_t	*pin_intervals;
	float			*poutintervals;
	void			*ptemp;

	pingroup = (dspritegroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);

	pspritegroup = Hunk_AllocName (sizeof (mspritegroup_t) +
				(numframes - 1) * sizeof (pspritegroup->frames[0]), "modspr3");

	pspritegroup->numframes = numframes;

	*ppframe = (mspriteframe_t *)pspritegroup;

	pin_intervals = (dspriteinterval_t *)(pingroup + 1);

	poutintervals = Hunk_AllocName (numframes * sizeof (float), "modint");

	pspritegroup->intervals = poutintervals;

	for (i=0 ; i<numframes ; i++)
	{
		*poutintervals = LittleFloat (pin_intervals->interval);
		if (*poutintervals <= 0.0)
			Sys_Error ("Mod_LoadSpriteGroup: interval %f <= 0 in %s", *poutintervals, loadmodel->name);

		poutintervals++;
		pin_intervals++;
	}

	ptemp = (void *)pin_intervals;

	for (i=0 ; i<numframes ; i++)
	{
		ptemp = Mod_LoadSpriteFrame (ptemp, &pspritegroup->frames[i], framenum * 100 + i, bytes, framenum);
	}

	return ptemp;
}


/*
=================
Mod_LoadSpriteModel
=================
*/
void Mod_LoadSpriteModel (model_t *mod, void *buffer)
{
	int			i;
	int			version;
	dsprite_t		*pin;
	msprite_t		*psprite;
	int			numframes;
	int			size;
	dspriteframetype_t	*pframetype;
        int			bytesperpixel = 1;

	mod_base = (byte *)buffer;
	pin = (dsprite_t *)buffer;

	// Check version+header is inside file buffer
	Mod_ChkFileSize ("Mod_LoadSpriteModel", "sprite header", 0, sizeof(int) + sizeof(dsprite_t));

	version = LittleLong (pin->version);

        // Nehahra 32-bit sprites
        if (version != SPRITE_VERSION && version != SPRITE32_VERSION)
		Sys_Error ("Mod_LoadSpriteModel: %s has wrong version number (%i should be %i or %i)", mod->name, version, SPRITE_VERSION, SPRITE32_VERSION);

	if (version == SPRITE32_VERSION)
		bytesperpixel = 4;

	numframes = LittleLong (pin->numframes);

	size = sizeof (msprite_t) +	(numframes - 1) * sizeof (psprite->frames);

	psprite = Hunk_AllocName (size, "modspr1");

	mod->cache.data = psprite;

	psprite->type = LittleLong (pin->type);
	psprite->maxwidth = LittleLong (pin->width);
	psprite->maxheight = LittleLong (pin->height);
	psprite->beamlength = LittleFloat (pin->beamlength);
	mod->synctype = LittleLong (pin->synctype);
	psprite->numframes = numframes;

	mod->mins[0] = mod->mins[1] = -psprite->maxwidth/2;
	mod->maxs[0] = mod->maxs[1] = psprite->maxwidth/2;
	mod->mins[2] = -psprite->maxheight/2;
	mod->maxs[2] = psprite->maxheight/2;

//
// load the frames
//
	if (numframes < 1)
		Sys_Error ("Mod_LoadSpriteModel: invalid # of frames %d in %s", numframes, mod->name);

	mod->numframes = numframes;

	pframetype = (dspriteframetype_t *)(pin + 1);

	for (i=0 ; i<numframes ; i++)
	{
		spriteframetype_t   frametype;
		char		    Str[256];

		sprintf (Str, "frame %d", i);
		Mod_ChkFileSize ("Mod_LoadSpriteModel", Str, (byte *)pframetype - mod_base, sizeof(spriteframetype_t));

		frametype = LittleLong (pframetype->type);
		psprite->frames[i].type = frametype;

		Mod_ChkType ("Mod_LoadSpriteModel", "frametype", Str, frametype, SPR_SINGLE, SPR_GROUP);

		if (frametype == SPR_SINGLE)
		{
			pframetype = (dspriteframetype_t *)
					Mod_LoadSpriteFrame (pframetype + 1,
										 &psprite->frames[i].frameptr, i, bytesperpixel, -1);
		}
		else
		{
			pframetype = (dspriteframetype_t *)
					Mod_LoadSpriteGroup (pframetype + 1,
										 &psprite->frames[i].frameptr, i, bytesperpixel);
		}
	}

	mod->type = mod_sprite;
}

//=============================================================================

/*
================
Mod_Print
================
*/
void Mod_Print (void)
{
	int	i, total;
	model_t	*mod;

	Con_SafePrintf ("Cached models:\n");
	total = 0;
	for (i=0, mod=mod_known ; i < mod_numknown ; i++, mod++)
	{
		total += mod->size;
		
		Con_SafePrintf ("(%8p) ", mod->cache.data);
		
		if (mod->type == mod_alias)
			Con_SafePrintf ("%6.1fk", mod->size / (float)1024);
		else
			Con_SafePrintf ("%6s ", "");
		
		Con_SafePrintf (" : %s\n", mod->name);
	}

	Con_SafePrintf ("\n%d models (%.1f megabyte)\n", mod_numknown, total / (float)(1024 * 1024));
}


