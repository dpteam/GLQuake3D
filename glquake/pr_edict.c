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
// sv_edict.c -- entity dictionary

#include "quakedef.h"

#ifdef _WIN32
#include <windows.h>
#endif

dprograms_t	*progs;
dfunction_t	*pr_functions;
char		*pr_strings;
ddef_t		*pr_fielddefs;
ddef_t		*pr_globaldefs;
dstatement_t	*pr_statements;
globalvars_t	*pr_global_struct;
float		*pr_globals;			// same as pr_global_struct
int		pr_edict_size;			// in bytes
qboolean	pr_free[MAX_EDICTS];
static float	freetime[MAX_EDICTS];		// sv.time when the object was freed

typedef struct {
	string_t    classname;	// Used for free check
	string_t    model;
	int	    statement;	// Used for both free and spawn check
	dfunction_t *function;
} edict_extra;

static edict_extra pr_EdictExtra[MAX_EDICTS];

unsigned short	pr_crc;

#define		TYPE_SIZE   8
int		type_size[TYPE_SIZE] = {1,sizeof(string_t)/4,1,3,1,1,sizeof(func_t)/4,sizeof(void *)/4};

static qboolean NoCrash;

ddef_t		*ED_FieldAtOfs (int ofs);
qboolean	ED_ParseEpair (void *base, ddef_t *key, char *keyname, char *s, char *errstr);

cvar_t	nomonsters = {"nomonsters", "0"};
cvar_t	gamecfg = {"gamecfg", "0"};
cvar_t	scratch1 = {"scratch1", "0"};
cvar_t	scratch2 = {"scratch2", "0"};
cvar_t	scratch3 = {"scratch3", "0"};
cvar_t	scratch4 = {"scratch4", "0"};
cvar_t	savedgamecfg = {"savedgamecfg", "0", true};
cvar_t	saved1 = {"saved1", "0", true};
cvar_t	saved2 = {"saved2", "0", true};
cvar_t	saved3 = {"saved3", "0", true};
cvar_t	saved4 = {"saved4", "0", true};
cvar_t	edict_reuse = {"edict_reuse", "1"};

#define	MAX_FIELD_LEN	64
#define GEFV_CACHESIZE	2

typedef struct {
	ddef_t	*pcache;
	char	field[MAX_FIELD_LEN];
} gefv_cache;

static gefv_cache	gefvCache[GEFV_CACHESIZE] = {{NULL, ""}, {NULL, ""}};

/*
=================
ED_ClearEdict

Sets everything to NULL
=================
*/
void ED_ClearEdict (edict_t *e, int ednum)
{
	memset (&e->v, 0, progs->entityfields * 4);
	pr_free[ednum] = false;
}

static void ED_SaveSpawn (int ednum)
{
	memset (&pr_EdictExtra[ednum], 0, sizeof(edict_extra));

	// Save some edict info for later
	pr_EdictExtra[ednum].statement = pr_xstatement;
	pr_EdictExtra[ednum].function = pr_xfunction;
}

static void ED_SaveFree (edict_t *ed)
{
	int ednum = NUM_FOR_EDICT("ED_SaveFree", ed);

	// Save some edict info for later
	pr_EdictExtra[ednum].classname = ed->v.classname;
	pr_EdictExtra[ednum].model = ed->v.model;
	pr_EdictExtra[ednum].statement = pr_xstatement;
	pr_EdictExtra[ednum].function = pr_xfunction;
}

/*
=================
ED_DbgEdict

Returns string with classname and modelindex
=================
*/
char *ED_DbgEdict (entvars_t *v)
{
	char	    Str2[100], *Ptr;
	static char Str[100];

	strcpy (Str, pr_String("ED_DbgEdict1", v->classname));

	if (v->modelindex > 0)
	{
		sprintf (Str2, ", model *%g", v->modelindex - 1);
		strcat (Str, Str2);
	}

	Ptr = pr_String("ED_DbgEdict2", v->model);
	
	if (*Ptr && *Ptr != '*')
	{
		sprintf (Str2, ", '%s'", Ptr);
		strcat (Str, Str2);
	}

	return Str;
}

/*
=================
ED_ChkEdict
=================
*/
void ED_ChkEdict (qboolean LoadChk, qboolean DisableChk)
{
	static qboolean WarnPrinted, Disabled;

	if (LoadChk)
	{
		WarnPrinted = false;
		Disabled = DisableChk; // Prevent multiple printouts during map load
	}

	if (Disabled || WarnPrinted)
		return;

	// Check old edict limit
	if (sv.num_edicts > 600)
	{
		WarnPrinted = true;
		Con_Printf ("\002ED_ChkEdict: ");
		Con_Printf ("excessive edicts (%d, normal max = %d)\n", sv.num_edicts, 600);
	}
}

/*
=================
ED_Alloc

Either finds a free edict, or allocates a new one.
Try to avoid reusing an entity that was recently freed, because it
can cause the client to think the entity morphed into something else
instead of being removed and recreated, which can cause interpolated
angles and bad trails.
=================
*/
edict_t *ED_Alloc (void)
{
	int	i;
	edict_t	*e;

	++sv.active_edicts;

	if (edict_reuse.value)
	{
		// Classic edict reuse policy
		for ( i=svs.maxclients+1 ; i<sv.num_edicts ; i++)
		{
			// the first couple seconds of server time can involve a lot of
			// freeing and allocating, so relax the replacement policy
			if (pr_free[i] && ( freetime[i] < 2 || sv.time - freetime[i] > 0.5 ) )
			{
				e = EDICT_NUM("ED_Alloc1", i);
				ED_ClearEdict (e, i);
				ED_SaveSpawn (i);
				return e;
			}
		}
	}
	else
	{
		int	   j;
		static int free_start = MAX_EDICTS;

		// Reuse edicts sequentially in a loop, utilizing the entire edict range
		for (j = 0; j < 3; ++j)
		{
			for ( i=free_start ; i<sv.num_edicts ; i++)
			{
				if (pr_free[i] && (sv.time - freetime[i] > 0.5 || j == 2))
				{
					if (j == 2)
					{
						static float lastmsg = 0;

						// Any free edict will do, but warn about it
						if (IsTimeout (&lastmsg, 30))
						{
							Con_DPrintf ("\002ED_Alloc: ");
							Con_DPrintf ("excessive edict reusage\n");
						}
					}
					
					e = EDICT_NUM("ED_Alloc1", i);
					ED_ClearEdict (e, i);
					ED_SaveSpawn (i);
					free_start = i + 1; // Next position to start searching for free slots
					return e;
				}
			}
			
			if (sv.num_edicts < MAX_EDICTS)
			{
				i = sv.num_edicts; // Don't trigger limit check below
				break;
			}

			// Start from beginning again
			free_start = svs.maxclients + 1;
		}
	}

	if (i == MAX_EDICTS)
		PR_RunError2 ("ED_Alloc: no free edicts, max = %d", MAX_EDICTS);

	sv.num_edicts++;
	e = EDICT_NUM("ED_Alloc2", i);
	ED_ClearEdict (e, i);
	ED_SaveSpawn (i);

	ED_ChkEdict (false, false);

	return e;
}

/*
=================
ED_Free

Marks the edict as free
FIXME: walk all entities and NULL out references to this entity
=================
*/
void ED_Free (edict_t *ed)
{
	int ednum = NUM_FOR_EDICT2(ed);

	SV_UnlinkEdict (ed);		// unlink from world bsp

	// Sometimes edicts are freed several times (QC bug)
	if (!pr_free[ednum])
	{
		ED_SaveFree (ed);
		--sv.active_edicts;
	}

	pr_free[ednum] = true;
	ed->v.model = 0;
	ed->v.takedamage = 0;
	ed->v.modelindex = 0;
	ed->v.colormap = 0;
	ed->v.skin = 0;
	ed->v.frame = 0;
	VectorCopy (vec3_origin, ed->v.origin);
	VectorCopy (vec3_origin, ed->v.angles);
	ed->v.nextthink = -1;
	ed->v.solid = 0;

	freetime[ednum] = sv.time;
}

//===========================================================================

/*
============
ED_GlobalAtOfs
============
*/
ddef_t *ED_GlobalAtOfs (int ofs)
{
	ddef_t	*def;
	int	i;

	for (i=0 ; i<progs->numglobaldefs ; i++)
	{
		def = &pr_globaldefs[i];
		if (def->ofs == ofs)
			return def;
	}
	return NULL;
}

/*
============
ED_FieldAtOfs
============
*/
ddef_t *ED_FieldAtOfs (int ofs)
{
	ddef_t	*def;
	int	i;

	for (i=0 ; i<progs->numfielddefs ; i++)
	{
		def = &pr_fielddefs[i];
		if (def->ofs == ofs)
			return def;
	}
	return NULL;
}

/*
============
ED_FindField
============
*/
ddef_t *ED_FindField (char *name)
{
	ddef_t	*def;
	int	i;

	for (i=0 ; i<progs->numfielddefs ; i++)
	{
		def = &pr_fielddefs[i];

		// Too expensive for default
		if (!strcmp(NoCrash ? pr_String("ED_FindField", def->s_name) : pr_strings + def->s_name, name))
			return def;
	}
	return NULL;
}


/*
============
ED_FindGlobal
============
*/
ddef_t *ED_FindGlobal (char *name)
{
	ddef_t	*def;
	int	i;

	for (i=0 ; i<progs->numglobaldefs ; i++)
	{
		def = &pr_globaldefs[i];
		if (!strcmp(pr_String("ED_FindGlobal", def->s_name), name))
			return def;
	}
	return NULL;
}


/*
============
ED_FindFunction
============
*/
dfunction_t *ED_FindFunction (char *name)
{
	dfunction_t	*func;
	int		i;

	for (i=0 ; i<progs->numfunctions ; i++)
	{
		func = &pr_functions[i];

		// Too expensive for default
		if (!strcmp(NoCrash ? pr_String("ED_FindFunction", func->s_name) : pr_strings + func->s_name, name))
			return func;
	}
	return NULL;
}


eval_t *GetEdictFieldValue (edict_t *ed, char *field)
{
	ddef_t		*def = NULL;
	int		i;
	static int	rep = 0;

	for (i=0 ; i<GEFV_CACHESIZE ; i++)
	{
		if (!strcmp(field, gefvCache[i].field))
		{
			def = gefvCache[i].pcache;
			goto Done;
		}
	}

	def = ED_FindField (field);

	if (strlen(field) < MAX_FIELD_LEN)
	{
		gefvCache[rep].pcache = def;
		strcpy (gefvCache[rep].field, field);
		rep ^= 1;
	}

Done:
	if (!def)
		return NULL;

	return (eval_t *)((char *)&ed->v + pr_ChkEField ("GetEdictFieldValue", def->ofs) * 4);
}


/*
============
PR_ValueString

Returns a string describing *data in a type specific manner
=============
*/
char *PR_ValueString (etype_t type, eval_t *val)
{
	static char	line[MAX_VALUE];
	ddef_t		*def;
	dfunction_t	*f;

	type &= ~DEF_SAVEGLOBAL;

	switch (type)
	{
	case ev_string:
		COM_snprintf ("PR_ValueString1", line, sizeof(line) - 1, "%s", pr_String("PR_ValueString1", val->string));
		break;
	case ev_entity:
		sprintf (line, "entity %i", NUM_FOR_EDICT("PR_ValueString", PROG_TO_EDICT("PR_ValueString", val->edict)) );
		break;
	case ev_function:
		f = pr_functions + pr_ChkFunction ("PR_ValueString", val->function);
		COM_snprintf ("PR_ValueString2", line, sizeof(line) - 1, "%s()", pr_String("PR_ValueString2", f->s_name));
		break;
	case ev_field:
		def = ED_FieldAtOfs ( val->_int );
		if (!def)
			sprintf (line, "%i(???)", val->_int);
		else
			COM_snprintf ("PR_ValueString3", line, sizeof(line) - 1, ".%s", pr_String("PR_ValueString3", def->s_name));
		break;
	case ev_void:
		sprintf (line, "void");
		break;
	case ev_float:
		sprintf (line, "%5.1f", val->_float);
		break;
	case ev_vector:
		sprintf (line, "'%5.1f %5.1f %5.1f'", val->vector[0], val->vector[1], val->vector[2]);
		break;
	case ev_pointer:
		sprintf (line, "pointer");
		break;
	default:
		sprintf (line, "bad type %i", type);
		break;
	}

	return line;
}

/*
============
PR_UglyValueString

Returns a string describing *data in a type specific manner
Easier to parse than PR_ValueString
=============
*/
char *PR_UglyValueString (etype_t type, eval_t *val)
{
	static char	line[MAX_VALUE];
	ddef_t		*def;
	dfunction_t	*f;
	int		i;

	type &= ~DEF_SAVEGLOBAL;

	switch (type)
	{
	case ev_string:
		COM_snprintf ("PR_UglyValueString1", line, sizeof(line) - 1, "%s", pr_String("PR_UglyValueString1", val->string));

		for (i = 0; line[i] != 0; ++i)
		{
			if (line[i] == 0x1A)
				line[i] = ' '; // Don't allow EOF to be written to text file
		}

		break;
	case ev_entity:
		sprintf (line, "%i", NUM_FOR_EDICT("PR_UglyValueString", PROG_TO_EDICT("PR_UglyValueString", val->edict)));
		break;
	case ev_function:
		f = pr_functions + pr_ChkFunction ("PR_UglyValueString", val->function);
		COM_snprintf ("PR_UglyValueString2", line, sizeof(line) - 1, "%s", pr_String("PR_UglyValueString2", f->s_name));
		break;
	case ev_field:
		def = ED_FieldAtOfs ( val->_int );
		if (!def)
			sprintf (line, "%i(???)", val->_int);
		else
			COM_snprintf ("PR_UglyValueString3", line, sizeof(line) - 1, "%s", pr_String("PR_UglyValueString3", def->s_name));
		break;
	case ev_void:
		sprintf (line, "void");
		break;
	case ev_float:
		sprintf (line, "%f", val->_float);
		break;
	case ev_vector:
		sprintf (line, "%f %f %f", val->vector[0], val->vector[1], val->vector[2]);
		break;
	default:
		sprintf (line, "bad type %i", type);
		break;
	}

	return line;
}

/*
============
PR_GlobalString

Returns a string with a description and the contents of a global,
padded to 20 field width
============
*/
char *PR_GlobalString (int ofs)
{
	char	    *s;
	int	    i;
	ddef_t	    *def;
	void	    *val;
	static char line[MAX_VALUE];

	val = (void *)&pr_globals[pr_ChkGlobalsF("PR_GlobalString", ofs)];
	def = ED_GlobalAtOfs(ofs);
	if (!def)
		sprintf (line,"%i(???)", ofs);
	else
	{
		s = PR_ValueString (def->type, val);
		COM_snprintf ("PR_GlobalString", line, sizeof(line) - 1, "%i(%s)%s", ofs, pr_String("PR_GlobalString", def->s_name), s);
	}

	i = strlen(line);
	for ( ; i<20 ; i++)
		strcat (line," ");
	strcat (line," ");

	return line;
}

char *PR_GlobalStringNoContents (int ofs)
{
	int	    i;
	ddef_t	    *def;
	static char line[MAX_VALUE];

	def = ED_GlobalAtOfs(ofs);
	if (!def)
		sprintf (line,"%i(???)", ofs);
	else
		COM_snprintf ("PR_GlobalStringNoContents", line, sizeof(line) - 1, "%i(%s)", ofs, pr_String("PR_GlobalStringNoContents", def->s_name));

	i = strlen(line);
	for ( ; i<20 ; i++)
		strcat (line," ");
	strcat (line," ");

	return line;
}


/*
=============
ED_Print

For debugging
=============
*/
void ED_Print (edict_t *ed)
{
	ddef_t	    *d;
	dfunction_t *f;
	int	    *v;
	int	    i, j;
	char	    *name;
	int	    type;
	int	    ednum = NUM_FOR_EDICT("ED_Print", ed);

	Con_SafePrintf ("\nEDICT %i:%s\n", ednum, pr_free[ednum] ? " FREE" : "");

	if (pr_free[ednum])
		return;

	f = pr_EdictExtra[ednum].function;
	if (f)
		Con_SafePrintf ("spawned at %12s : %s : statement %i\n", pr_String("ED_Print1", f->s_file),
				pr_String("ED_Print2", f->s_name), pr_EdictExtra[ednum].statement - f->first_statement);

	for (i=1 ; i<progs->numfielddefs ; i++)
	{
		d = &pr_fielddefs[i];
		name = pr_String ("ED_Print3", d->s_name);
		if (name[strlen(name)-2] == '_')
			continue;	// skip _x, _y, _z vars

		v = (int *)((char *)&ed->v + pr_ChkEField ("ED_Print", d->ofs) * 4);

	// if the value is still all 0, skip the field
		type = d->type & ~DEF_SAVEGLOBAL;

		if (type >= TYPE_SIZE)
		{
			Con_SafePrintf ("ED_Print: bad type %d (max = %d)\n", type, TYPE_SIZE - 1);
			break;
		}

		for (j=0 ; j<type_size[type] ; j++)
			if (v[j])
				break;
		if (j == type_size[type])
			continue;

		Con_SafePrintf ("%-15s%s\n", name, PR_ValueString(d->type, (eval_t *)v));
	}
}

/*
=============
ED_Write

For savegames
=============
*/
void ED_Write (FILE *f, edict_t *ed)
{
	ddef_t	*d;
	int	*v;
	int	i, j;
	char	*name;
	int	type;
	int	ednum = NUM_FOR_EDICT("ED_Write", ed);

	fprintf (f, "{\n");

	if (pr_free[ednum])
	{
		fprintf (f, "}\n");
		return;
	}

	for (i=1 ; i<progs->numfielddefs ; i++)
	{
		d = &pr_fielddefs[i];
		name = pr_String ("ED_Write", d->s_name);
		if (name[strlen(name)-2] == '_')
			continue;	// skip _x, _y, _z vars

		v = (int *)((char *)&ed->v + pr_ChkEField ("ED_Write", d->ofs) * 4);

	// if the value is still all 0, skip the field
		type = d->type & ~DEF_SAVEGLOBAL;

		if (type >= TYPE_SIZE)
		{
			Con_SafePrintf ("ED_Write: bad type %d (max = %d)\n", type, TYPE_SIZE - 1);
			break;
		}

		for (j=0 ; j<type_size[type] ; j++)
			if (v[j])
				break;
		if (j == type_size[type])
			continue;

		fprintf (f,"\"%s\" ",name);
		fprintf (f,"\"%s\"\n", PR_UglyValueString(d->type, (eval_t *)v));
	}

	fprintf (f, "}\n");
}

void ED_PrintNum (int ent)
{
	ED_Print (EDICT_NUM("ED_PrintNum", ent));
}

/*
=============
ED_PrintEdicts

For debugging, prints all the entities in the current server
=============
*/
void ED_PrintEdicts (void)
{
	int		i;

	Con_SafePrintf ("%i entities\n", sv.num_edicts);
	for (i=0 ; i<sv.num_edicts ; i++)
		ED_PrintNum (i);
}

/*
=============
ED_PrintEdict_f

For debugging, prints a single edicy
=============
*/
void ED_PrintEdict_f (void)
{
	int		i;

	i = Q_atoi (Cmd_Argv(1));
	if (i >= sv.num_edicts)
	{
		Con_Printf("Bad edict number %i\n", i);
		return;
	}
	ED_PrintNum (i);
}

/*
=============
ED_Count

For debugging
=============
*/
void ED_Count (void)
{
	int	i;
	edict_t	*ent;
	int	active, models, solid, step;

	active = models = solid = step = 0;
	for (i=0 ; i<sv.num_edicts ; i++)
	{
		if (pr_free[i])
			continue;
		ent = EDICT_NUM("ED_Count", i);
		active++;
		if (ent->v.solid)
			solid++;
		if (ent->v.model)
			models++;
		if (ent->v.movetype == MOVETYPE_STEP)
			step++;
	}

	Con_SafePrintf ("num_edicts:%3i\n", sv.num_edicts);
	Con_SafePrintf ("active    :%3i\n", active);
	Con_SafePrintf ("view      :%3i\n", models);
	Con_SafePrintf ("touch     :%3i\n", solid);
	Con_SafePrintf ("step      :%3i\n", step);

}

/*
==============================================================================

					ARCHIVING GLOBALS

FIXME: need to tag constants, doesn't really work
==============================================================================
*/

/*
=============
ED_WriteGlobals
=============
*/
void ED_WriteGlobals (FILE *f)
{
	ddef_t		*def;
	int			i;
	char		*name;
	int			type;

	fprintf (f,"{\n");
	for (i=0 ; i<progs->numglobaldefs ; i++)
	{
		def = &pr_globaldefs[i];
		type = def->type;
		if ( !(def->type & DEF_SAVEGLOBAL) )
			continue;
		type &= ~DEF_SAVEGLOBAL;

		if (type != ev_string
		&& type != ev_float
		&& type != ev_entity)
			continue;

		name = pr_String ("ED_WriteGlobals", def->s_name);
		fprintf (f,"\"%s\" ", name);
		fprintf (f,"\"%s\"\n", PR_UglyValueString(type, (eval_t *)&pr_globals[pr_ChkGlobalsF("ED_WriteGlobals", def->ofs)]));
	}
	fprintf (f,"}\n");
}

static char *PrintLine (int line)
{
	static char Str[100];

	if (line == -1)
		return "";

	sprintf (Str, " on line %d", line);
	
	return Str;
}

/*
=============
ED_ParseGlobals
=============
*/
void ED_ParseGlobals (char *data, int line)
{
	char	keyname[MAX_KEY], errstr[1024];
	ddef_t	*key;
	int	len;

	while (1)
	{
	// parse key
		data = COM_Parse (data);
		if (com_token[0] == '}')
			break;
		if (!data)
			Sys_Error ("ED_ParseGlobals: EOF without closing brace%s", PrintLine (line));

		len = strlen (com_token);

		if (len > sizeof(keyname) - 1)
			Sys_Error ("ED_ParseGlobals: keyname '%s' too long (%d, max = %d)%s", com_token, len, sizeof(keyname) - 1, PrintLine (line));

		strcpy (keyname, com_token);

	// parse value
		data = COM_Parse (data);
		if (!data)
			Sys_Error ("ED_ParseGlobals: EOF without closing brace%s", PrintLine (line));

		if (com_token[0] == '}')
			Sys_Error ("ED_ParseGlobals: closing brace without data for key '%s'%s", keyname, PrintLine (line));

		key = ED_FindGlobal (keyname);
		if (!key)
		{
			Con_Printf ("'%s' is not a global\n", keyname);
			continue;
		}

		if (!ED_ParseEpair ((void *)pr_globals, key, keyname, com_token, errstr))
			Con_SafePrintf ("%s in globals\n", errstr);
	}
}

//============================================================================


/*
=============
ED_NewString
=============
*/
char *ED_NewString (char *string)
{
	char	*new, *new_p;
	int		i,l;

	l = strlen(string) + 1;
	new = Hunk_Alloc (l);
	new_p = new;

	for (i=0 ; i< l ; i++)
	{
		if (string[i] == '\\' && i < l-1)
		{
			i++;
			if (string[i] == 'n')
				*new_p++ = '\n';
			else
				*new_p++ = '\\';
		}
		else
			*new_p++ = string[i];
	}

	return new;
}


/*
=============
ED_ParseEpair

Can parse either fields or globals
returns false if error
=============
*/
qboolean ED_ParseEpair (void *base, ddef_t *key, char *keyname, char *s, char *errstr)
{
	int		i, Size = 1;
	ddef_t		*def;
//	char		*ssave;
	void		*d;
	dfunction_t	*func;

	switch (key->type & ~DEF_SAVEGLOBAL)
	{
	case ev_vector:
		Size *= 3;
		// Fall through
	case ev_string:
	case ev_float:
	case ev_entity:
	case ev_field:
	case ev_function:
		if (base == (void *)pr_globals)
			pr_ChkGlobalsF ("ED_ParseEpair", key->ofs + Size - 1);
		else
			pr_ChkEField ("ED_ParseEpair", key->ofs + Size - 1);
		break;

	default:
		break;
	}

	d = (void *)((int *)base + key->ofs);

	switch (key->type & ~DEF_SAVEGLOBAL)
	{
	case ev_string:
		*(string_t *)d = ED_NewString (s) - pr_strings;
		break;

	case ev_float:
		*(float *)d = atof (s);
		break;

	case ev_vector:
//		ssave = s;

		for (i=0 ; i<3 ; i++)
		{
			while (isspace(*s))
				s++;

			if (*s)
				((float *)d)[i] = atof (s);
			else
			{
				// This is a bit dodgy, but e.g. the light tool properly handles some
				// mangle strings that this logic can't (e.g. "0-90 0"), so don't complain.
				// At least the logic now don't trash the stack anymore ...
/*				if (ssave)
				{
					Con_SafePrintf ("ED_ParseEpair: invalid ev_vector '%s' in key '%s'\n", ssave, keyname);
					ssave = NULL; // Only one printout per key
				}*/

				((float *)d)[i] = 0;
			}

			while (*s && !isspace(*s))
				s++;
		}
		break;

	case ev_entity:
		*(int *)d = EDICT_TO_PROG(EDICT_NUM("ED_ParseEpair", atoi (s)));
		break;

	case ev_field:
		def = ED_FindField (s);
		if (!def)
		{
			sprintf (errstr, "ED_ParseEpair: Can't find field '%s' in key '%s'", s, keyname);
			return false;
		}
		*(int *)d = G_INT(def->ofs);
		break;

	case ev_function:
		func = ED_FindFunction (s);
		if (!func)
		{
			sprintf (errstr, "ED_ParseEpair: Can't find function '%s' in key '%s'", s, keyname);
			return false;
		}
		*(func_t *)d = func - pr_functions;
		break;

	default:
		break;
	}
	return true;
}

/*
====================
ED_ParseEdict

Parses an edict out of the given string, returning the new position
ed should be a properly initialized empty edict.
Used for initial level load and for savegames.
====================
*/
char *ED_ParseEdict (char *data, edict_t *ent, int line)
{
	ddef_t	    *key;
	qboolean    anglehack;
	qboolean    init, Error = false;
	char	    keyname[MAX_KEY], keysave[MAX_KEY], errstr[1024];
	int	    n, i;

	init = false;

// clear it
	if (ent != sv.edicts)	// hack
		memset (&ent->v, 0, progs->entityfields * 4);

// go through all the dictionary pairs
	while (1)
	{
	// parse key
		data = COM_Parse (data);
		if (com_token[0] == '}')
			break;
		if (!data)
			Sys_Error ("ED_ParseEdict: EOF without closing brace%s", PrintLine (line));

		strcpy (keysave, com_token); // To properly display e.g. "light ", "angle " and "angles" keys

// anglehack is to allow QuakeEd to write single scalar angles
// and allow them to be turned into vectors. (FIXME...)
if (!strcmp(com_token, "angle"))
{
	strcpy (com_token, "angles");
	anglehack = true;
}
else
	anglehack = false;

// FIXME: change light to _light to get rid of this hack
if (!strcmp(com_token, "light"))
	strcpy (com_token, "light_lev");	// hack for single light def

		n = strlen (com_token);

		if (n > sizeof(keyname) - 1)
			Sys_Error ("ED_ParseEdict: keyname '%s' too long (%d, max = %d)%s", com_token, n, sizeof(keyname) - 1, PrintLine (line));

		strcpy (keyname, com_token);

		// another hack to fix heynames with trailing spaces
		while (n && keyname[n-1] == ' ')
		{
			keyname[n-1] = 0;
			n--;
		}

	// parse value
		data = COM_Parse (data);
		if (!data)
			Sys_Error ("ED_ParseEdict: EOF without closing brace%s", PrintLine (line));

		if (com_token[0] == '}')
			Sys_Error ("ED_ParseEdict: closing brace without data for key '%s'%s", keyname, PrintLine (line));

		init = true;

// keynames with a leading underscore are used for utility comments,
// and are immediately discarded by quake
		if (keyname[0] == '_')
			continue;

		key = ED_FindField (keyname);
		if (!key)
		{
#ifdef GLQUAKE
			if (strcmp(keyname, "sky") && strcmp(keyname, "fog")) // Now supported in worldspawn
#endif
			{
				Con_SafePrintf ("%s'%s' is not a field", Error ? "\n" : "", keysave);
				Error = true;
			}
			continue;
		}

if (anglehack)
{
char	temp[32];
strcpy (temp, com_token);
sprintf (com_token, "0 %s 0", temp);
}

		if (!ED_ParseEpair ((void *)&ent->v, key, keysave, com_token, errstr))
		{
			Con_SafePrintf ("%s%s", Error ? "\n" : "", errstr);
			Error = true;
		}
	}

	if (Error)
	{
		Con_SafePrintf (" in edict");

		// Any classname ?
		if (ent->v.classname)
			Con_SafePrintf (" '%s'", pr_String("ED_ParseEdict1", ent->v.classname));

		for (i = 0; i < 3; ++i)
		{
			if (ent->v.origin[i] != 0)
				break;
		}

		// Any origin ?
		if (i < 3)
		{
			Con_SafePrintf (" at '");

			for (i = 0; i < 3; ++i)
				Con_SafePrintf ("%s%.1f", i > 0 ? " " : "", ent->v.origin[i]);

			Con_SafePrintf ("'");
		}
		else if (ent->v.targetname)
			Con_SafePrintf (", targetname '%s'", pr_String("ED_ParseEdict2", ent->v.targetname));
		else if (ent->v.target)
			Con_SafePrintf (", target '%s'", pr_String("ED_ParseEdict3", ent->v.target));

		Con_SafePrintf ("\n");
	}

	if (!init)
		pr_free[NUM_FOR_EDICT2(ent)] = true;

	return data;
}

static void ChkPrecache (qboolean Models, char *Precache[], int NewLimit)
{
	int i, Items = 0;

	// Check old models/sounds limit
	for (i = 0; i < NewLimit; ++i)
	{
		if (Precache[i])
			++Items;
	}

	if (Items > 256)
	{
		Con_Printf ("\002ED_LoadFromFile: ");
		Con_Printf ("excessive precached %s (%d, normal max = %d)\n", Models ? "models" : "sounds", Items, 256);
	}
}

/*
================
ED_LoadFromFile

The entities are directly placed in the array, rather than allocated with
ED_Alloc, because otherwise an error loading the map would have entity
number references out of order.

Creates a server's entity / program execution context by
parsing textual entity definitions out of an ent file.

Used for both fresh maps and savegame loads.  A fresh map would also need
to call ED_CallSpawnFunctions () to let the objects initialize themselves.
================
*/
void ED_LoadFromFile (char *data)
{
	edict_t		*ent;
	int		inhibit;
	dfunction_t	*func;
	qboolean	WorldSpawn, WorldSpawnDeleted;
	char		*fname;

	ent = NULL;
	inhibit = 0;
	pr_global_struct->time = sv.time;

	ED_ChkEdict (true, true);

// parse ents
	while (1)
	{
// parse the opening brace
		data = COM_Parse (data);
		if (!data)
			break;
		if (com_token[0] != '{')
			Sys_Error ("ED_LoadFromFile: found '%s' when expecting '{'", com_token);

		if (!ent)
			ent = EDICT_NUM("ED_LoadFromFile", 0);
		else
			ent = ED_Alloc ();
		data = ED_ParseEdict (data, ent, -1);

// remove things from different skill levels or deathmatch

		// Check for deleting worldspawn
		WorldSpawn = ent->v.modelindex == 1 && ent->v.classname && !strcmp(pr_String("ED_LoadFromFile1", ent->v.classname), "worldspawn");
		WorldSpawnDeleted = false;

		if (deathmatch.value)
		{
			if (((int)ent->v.spawnflags & SPAWNFLAG_NOT_DEATHMATCH))
			{
				if (WorldSpawn)
					WorldSpawnDeleted = true;
				else
				{
					ED_Free (ent);
					inhibit++;
					continue;
				}
			}
		}
		else if ((current_skill == 0 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_EASY))
				|| (current_skill == 1 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_MEDIUM))
				|| (current_skill >= 2 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_HARD)) )
		{
			if (WorldSpawn)
				WorldSpawnDeleted = true;
			else
			{
				ED_Free (ent);
				inhibit++;
				continue;
			}
		}

		if (WorldSpawnDeleted)
		{
			Con_Printf ("\002ED_LoadFromFile: ");
			Con_Printf ("worldspawn deletion denied\n");
		}

//
// immediately call spawn function
//
		if (!ent->v.classname)
		{
			Con_SafePrintf ("No classname for:\n");
			ED_Print (ent);
			ED_Free (ent);
			continue;
		}

	// look for the spawn function
		fname = pr_String("ED_LoadFromFile2", ent->v.classname);
		func = ED_FindFunction (fname);

		if (!func)
		{
			Con_SafePrintf ("No spawn function for:\n");
			ED_Print (ent);
			ED_Free (ent);
			continue;
		}

		pr_global_struct->self = EDICT_TO_PROG(ent);
		PR_ExecuteProgram (func - pr_functions, fname);
	}

	ED_ChkEdict (true, false);

	// Check old models/sounds limit
	ChkPrecache (true, sv.model_precache, MAX_MODELS);
	ChkPrecache (false, sv.sound_precache, MAX_SOUNDS);

	Con_DPrintf ("%i entities inhibited\n", inhibit);
}

void Neh_GameStart (void) // From host_cmd.c
{
	func_t	    RestoreGame;
        dfunction_t *f;

	if (!nehahra)
		return;

	if ((f = ED_FindFunction("RestoreGame")))
	{
		if ((RestoreGame = (func_t)(f - pr_functions)))
		{
			Con_SafePrintf ("Loading enhanced game - RestoreGame()\n");
			pr_global_struct->time = sv.time;
			pr_global_struct->self = EDICT_TO_PROG(sv_player);
			PR_ExecuteProgram (RestoreGame, "RestoreGame");
		}
	}
}

static void Chk_ProgsSize (char *Object, int Offset, int Size)
{
	int ObjEnd = Offset + Size;

	if (Offset < 0 || Size < 0 || ObjEnd < 0 || ObjEnd > com_filesize)
		Sys_Error ("PR_LoadProgs: progs object '%s' is outside file (%d, max = %d)", Object, ObjEnd, com_filesize);
}

static int InvalidOffsets;

static int Chk_StrOffset (char *Object, int ObjNum, int Offset)
{
	if (Offset < 0 || Offset >= progs->numstrings)
	{
		// Apparently, some qcc's may generate garbage here (scramblers?)
		// Also, some progs have bad offsets, so let's be generous ...
//		Sys_Error ("PR_LoadProgs: string offset for %s %d is outside string area (%d, max = %d)", Object, ObjNum, Offset, progs->numstrings);
		++InvalidOffsets;
		return 0; // Set it to a valid offset
	}

	return Offset;
}

void pr_ChkGlobalsFS (char *Function, int o, int Statement);

/*
===============
PR_LoadProgs
===============
*/
void PR_LoadProgs (void)
{
	dstatement_t	*st;
	int		i;
	char		str[100];
	static qboolean Done, Done2;

	if (!Done)
	{
		NoCrash = COM_CheckParm ("-nocrash");
		Done = true;
	}

	PR_RunClear ();

// flush the non-C variable lookup cache
	for (i=0 ; i<GEFV_CACHESIZE ; i++)
		gefvCache[i].field[0] = 0;

	CRC_Init (&pr_crc);

	progs = (dprograms_t *)COM_LoadHunkFile ("progs.dat");
	if (!progs)
		Sys_Error ("PR_LoadProgs: couldn't load progs.dat");

	Con_DPrintf ("Programs occupy %iK, %d statements, %d globals\n", com_filesize/1024, progs->numstatements, progs->numglobals);

	if (!Done2 && progs->numglobals > 32768)
	{
		Con_DPrintf ("\002PR_LoadProgs: ");
		Con_DPrintf ("excessive globals (normal max = %d)\n", 32768);
		Done2 = true; // Avoid repetitive warnings
	}

	for (i=0 ; i<com_filesize ; i++)
		CRC_ProcessByte (&pr_crc, ((byte *)progs)[i]);

// byte swap the header
	for (i=0 ; i<sizeof(*progs)/4 ; i++)
		((int *)progs)[i] = LittleLong ( ((int *)progs)[i] );

	if (progs->version != PROG_VERSION)
		Sys_Error ("progs.dat has wrong version number (%i should be %i)", progs->version, PROG_VERSION);
	if (progs->crc != PROGHEADER_CRC)
		Sys_Error ("progs.dat system vars have been modified, progdefs.h is out of date");

	pr_functions = (dfunction_t *)((byte *)progs + progs->ofs_functions);
	Chk_ProgsSize ("functions", progs->ofs_functions, progs->numfunctions * sizeof(dfunction_t));

	pr_strings = (char *)progs + progs->ofs_strings;
	Chk_ProgsSize ("strings", progs->ofs_strings, progs->numstrings);

	pr_globaldefs = (ddef_t *)((byte *)progs + progs->ofs_globaldefs);
	Chk_ProgsSize ("globaldefs", progs->ofs_globaldefs, progs->numglobaldefs * sizeof(ddef_t));

	pr_fielddefs = (ddef_t *)((byte *)progs + progs->ofs_fielddefs);
	Chk_ProgsSize ("fielddefs", progs->ofs_fielddefs, progs->numfielddefs * sizeof(ddef_t));

	pr_statements = (dstatement_t *)((byte *)progs + progs->ofs_statements);
	Chk_ProgsSize ("statements", progs->ofs_statements, progs->numstatements * sizeof(dstatement_t));

	pr_global_struct = (globalvars_t *)((byte *)progs + progs->ofs_globals);
	pr_globals = (float *)pr_global_struct;
	Chk_ProgsSize ("globals", progs->ofs_globals, progs->numglobals * sizeof(float));

	pr_edict_size = progs->entityfields * 4 + sizeof (edict_t) - sizeof(entvars_t);

	InvalidOffsets = 0;

// byte swap the lumps
	for (i=0 ; i<progs->numstatements ; i++)
	{
		pr_statements[i].op = LittleShort(pr_statements[i].op);
		pr_statements[i].a = LittleShort(pr_statements[i].a);
		pr_statements[i].b = LittleShort(pr_statements[i].b);
		pr_statements[i].c = LittleShort(pr_statements[i].c);
	}

	for (i=0 ; i<progs->numfunctions; i++)
	{
		pr_functions[i].first_statement = LittleLong (pr_functions[i].first_statement);

		if (pr_functions[i].first_statement >= progs->numstatements)
			Sys_Error ("PR_LoadProgs: invalid pr_functions[%d].first_statement (%d, max = %d)", i, pr_functions[i].first_statement, progs->numstatements);

		pr_functions[i].parm_start = LittleLong (pr_functions[i].parm_start);
		pr_functions[i].s_name = Chk_StrOffset ("function name", i, LittleLong (pr_functions[i].s_name));
		pr_functions[i].s_file = Chk_StrOffset ("function file", i, LittleLong (pr_functions[i].s_file));
		pr_functions[i].numparms = LittleLong (pr_functions[i].numparms);
		pr_functions[i].locals = LittleLong (pr_functions[i].locals);
	}

	for (i=0 ; i<progs->numglobaldefs ; i++)
	{
		pr_globaldefs[i].type = LittleShort (pr_globaldefs[i].type);
		pr_globaldefs[i].ofs = LittleShort (pr_globaldefs[i].ofs);
		pr_globaldefs[i].s_name = Chk_StrOffset ("global defs name", i, LittleLong (pr_globaldefs[i].s_name));
	}

	for (i=0 ; i<progs->numfielddefs ; i++)
	{
		pr_fielddefs[i].type = LittleShort (pr_fielddefs[i].type);
		if (pr_fielddefs[i].type & DEF_SAVEGLOBAL)
			Sys_Error ("PR_LoadProgs: pr_fielddefs[%d].type & DEF_SAVEGLOBAL (%d)", i, pr_fielddefs[i].type);
		pr_fielddefs[i].ofs = LittleShort (pr_fielddefs[i].ofs);
		pr_fielddefs[i].s_name = Chk_StrOffset ("field defs name", i, LittleLong (pr_fielddefs[i].s_name));
	}

	for (i=0 ; i<progs->numglobals ; i++)
		((int *)pr_globals)[i] = LittleLong (((int *)pr_globals)[i]);

	if (InvalidOffsets)
	{
		Con_Printf ("\002PR_LoadProgs: ");
		Con_Printf ("%d invalid string offsets found\n", InvalidOffsets);
	}

	strcpy (str, "PR_LoadProgs1"); // Kludge to avoid speed hit

	for (i = 0, st = pr_statements; i < progs->numstatements; i++, st++)
	{
		switch (st->op)
		{
		case OP_IF:
		case OP_IFNOT:
			pr_ChkGlobalsFS (str, st->a, i);
			if (st->b + i < 0 || st->b + i >= progs->numstatements)
				Sys_Error ("%s: statement %d: invalid IF/IFNOT statement %d (max = %d)", str, i, st->b + i, progs->numstatements);
			break;
		case OP_GOTO:
			if (st->a + i < 0 || st->a + i >= progs->numstatements)
				Sys_Error ("%s: statement %d: invalid GOTO statement %d (max = %d)", str, i, st->a + i, progs->numstatements);
			break;
		// global global global
		case OP_ADD_F:
		case OP_ADD_V:
		case OP_SUB_F:
		case OP_SUB_V:
		case OP_MUL_F:
		case OP_MUL_V:
		case OP_MUL_FV:
		case OP_MUL_VF:
		case OP_DIV_F:
		case OP_BITAND:
		case OP_BITOR:
		case OP_GE:
		case OP_LE:
		case OP_GT:
		case OP_LT:
		case OP_AND:
		case OP_OR:
		case OP_EQ_F:
		case OP_EQ_V:
		case OP_EQ_S:
		case OP_EQ_E:
		case OP_EQ_FNC:
		case OP_NE_F:
		case OP_NE_V:
		case OP_NE_S:
		case OP_NE_E:
		case OP_NE_FNC:
		case OP_ADDRESS:
		case OP_LOAD_F:
		case OP_LOAD_FLD:
		case OP_LOAD_ENT:
		case OP_LOAD_S:
		case OP_LOAD_FNC:
		case OP_LOAD_V:
			str[12] = '2';
			pr_ChkGlobalsFS (str, st->a, i);
			str[12] = '3';
			pr_ChkGlobalsFS (str, st->b, i);
			str[12] = '4';
			pr_ChkGlobalsFS (str, st->c, i);
			break;
		// global none global
		case OP_NOT_F:
		case OP_NOT_V:
		case OP_NOT_S:
		case OP_NOT_FNC:
		case OP_NOT_ENT:
			str[12] = '5';
			pr_ChkGlobalsFS (str, st->a, i);
			str[12] = '6';
			pr_ChkGlobalsFS (str, st->c, i);
			break;
		// 2 globals
		case OP_STOREP_F:
		case OP_STOREP_ENT:
		case OP_STOREP_FLD:
		case OP_STOREP_S:
		case OP_STOREP_FNC:
		case OP_STORE_F:
		case OP_STORE_ENT:
		case OP_STORE_FLD:
		case OP_STORE_S:
		case OP_STORE_FNC:
		case OP_STATE:
		case OP_STOREP_V:
		case OP_STORE_V:
			str[12] = '7';
			pr_ChkGlobalsFS (str, st->a, i);
			str[12] = '8';
			pr_ChkGlobalsFS (str, st->b, i);
			break;
		// 1 global
		case OP_CALL0:
		case OP_CALL1:
		case OP_CALL2:
		case OP_CALL3:
		case OP_CALL4:
		case OP_CALL5:
		case OP_CALL6:
		case OP_CALL7:
		case OP_CALL8:
		case OP_DONE:
		case OP_RETURN:
			str[12] = '9';
			pr_ChkGlobalsFS (str, st->a, i);
			break;
		default:
			Sys_Error ("%s: statement %d: bad opcode %d", str, i, st->op);
			break;
		}
	}
}


/*
===============
PR_Init
===============
*/
void PR_Init (void)
{
	Cmd_AddCommand ("edict", ED_PrintEdict_f);
	Cmd_AddCommand ("edicts", ED_PrintEdicts);
	Cmd_AddCommand ("edictcount", ED_Count);
	Cmd_AddCommand ("profile", PR_Profile_f);
	Cvar_RegisterVariable (&nomonsters);
	Cvar_RegisterVariable (&gamecfg);
	Cvar_RegisterVariable (&scratch1);
	Cvar_RegisterVariable (&scratch2);
	Cvar_RegisterVariable (&scratch3);
	Cvar_RegisterVariable (&scratch4);
	Cvar_RegisterVariable (&savedgamecfg);
	Cvar_RegisterVariable (&saved1);
	Cvar_RegisterVariable (&saved2);
	Cvar_RegisterVariable (&saved3);
	Cvar_RegisterVariable (&saved4);
	Cvar_RegisterVariable (&edict_reuse);
}


edict_t *EDICT_NUM(char *Function, int n)
{
	if (n < 0 || n >= sv.max_edicts)
		PR_RunError2 ("%s: bad edict %i, max = %d", Function, n, sv.max_edicts);

	return (edict_t *)((byte *)sv.edicts+ (n)*pr_edict_size);
}

static int ChkEdict (char *Function, int e, unsigned Size)
{
	if (e < 0 || e / pr_edict_size >= sv.num_edicts)
		PR_RunError2 ("%s: invalid edict %d, max = %d", Function, e / pr_edict_size, sv.num_edicts);
	else
	{
		int Offset = e % pr_edict_size;

		// Size == 0 => Aligned edict (Offset must be 0)
		// Size > 0  => Unaligned edict (Offset to (Offset+Size) must be completely inside entvars_t object v)
		if (!Size && Offset || Size && (Offset < ((size_t)&((edict_t *)0)->v) || Offset + Size > pr_edict_size))
			PR_RunError2 ("%s: misaligned edict %g", Function, (float)e / pr_edict_size);
	}

	return e;
}

int NUM_FOR_EDICT(char *Function, edict_t *e)
{
	return ChkEdict (Function, (byte *)e - (byte *)sv.edicts, 0) / pr_edict_size;
}

static int ChkGlobals (char *Function, int o, int Statement)
{
	if (o < 0)
		o += 65536; // Handle more globals?

	if (o < 0 || o >= progs->numglobals)
	{
		char StateStr[100] = "";

		if (Statement >= 0)
			sprintf (StateStr, ": statement %d", Statement);

		PR_RunError2 ("%s%s: invalid global %d, max = %d", Function, StateStr, o, progs->numglobals);
	}

	return o;
}

static int ChkString (char *Function, int StrIndex, qboolean Abort)
{
//	extern char pr_string_temp[128];

//	if (StrIndex < 0 && pr_strings + StrIndex != pr_string_temp || StrIndex >= progs->numstrings)
//		Sys_Error ("%s: invalid string index %d, max = %d", Function, StrIndex, progs->numstrings);
#ifdef _WIN32
	if (IsBadStringPtr(pr_strings + StrIndex, MAX_PRINTMSG)) // Can't seem to do this in a better way ...
	{
//		if (Abort)
			PR_RunError2 ("%s: invalid string pointer 0x%08x", Function, pr_strings + StrIndex);

//		Con_Printf ("\002%s: ", Function);
//		Con_Printf ("invalid string pointer 0x%08x\n", pr_strings + StrIndex);
//		return 0; // Set it to a valid offset
	}
#endif

	return StrIndex;
}

static int ChkEField (char *Function, int o)
{
	if (o < 0 || o >= progs->entityfields)
		PR_RunError2 ("%s: invalid entity field (%d, max = %d)", Function, o, progs->entityfields);

	return o;
}

int pr_ChkFunction (char *Function, int f)
{
	if (f < 0 || f >= progs->numfunctions)
		PR_RunError2 ("%s: invalid function %d, max = %d", Function, f, progs->numfunctions);

	return f;
}

int pr_ChkGlobals (int o)
{
	return ChkGlobals ("pr_ChkGlobals", o, -1);
}

int pr_ChkGlobalsF (char *Function, int o)
{
	return ChkGlobals (Function, o, -1);
}

void pr_ChkGlobalsFS (char *Function, int o, int Statement)
{
	ChkGlobals (Function, o, Statement);
}

char *pr_String (char *Function, int o)
{
	return pr_strings + ChkString (Function, o, false);
}

char *pr_GetString (int o)
{
	return pr_strings + ChkString ("pr_GetString", *(string_t *)&pr_globals[ChkGlobals("pr_GetString", o, -1)], false);
}

char *pr_GetEString (edict_t *e, int o)
{
	o = *(string_t *)&((float *)&e->v)[ChkEField("pr_GetEString", o)];

	// Too expensive for default
	return pr_strings + (NoCrash ? ChkString ("pr_GetEString", o, true) : o);
}

void pr_PrMember (string_t strnum)
{
	char *str;

	if (!strnum)
		return;

	str = pr_String ("pr_PrMember", strnum);

	if (*str)
		Con_SafePrintf (", %s", str);
}

edict_t *pr_GetEdict (char *Function, int o)
{
	edict_t *ed = (edict_t *)((byte *)sv.edicts + ChkEdict (Function, *(int *)&pr_globals[ChkGlobals(Function, o, -1)], 0));
	int	ednum = NUM_FOR_EDICT2(ed);

	if (pr_free[ednum] && developer.value > 1)
	{
		dfunction_t *f;

		PR_RunError3 ();
		
		// Print saved info about freed edict
		f = pr_EdictExtra[ednum].function;
		if (f)
			Con_SafePrintf ("removed at %s : %s : statement %i\n", pr_String("pr_GetEdict1", f->s_file),
					pr_String("pr_GetEdict2", f->s_name), pr_EdictExtra[ednum].statement - f->first_statement);
		
		Con_SafePrintf ("\002pr_GetEdict: ");
		Con_SafePrintf ("accessing free edict %d", ednum);

		pr_PrMember (pr_EdictExtra[ednum].classname);
		pr_PrMember (pr_EdictExtra[ednum].model);

		// Print time since freed
		Con_SafePrintf (", %.3fs\n", fabs(sv.time - freetime[ednum]));
	}

	return ed;
}

int pr_ChkEField (char *Function, int o)
{
	return ChkEField (Function, o);
}

int pr_ChkEdict (char *Function, int e, unsigned Size)
{
	return ChkEdict (Function, e, Size);
}

