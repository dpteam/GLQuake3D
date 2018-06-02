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
// cl_parse.c  -- parse a message received from the server

#include "quakedef.h"

char *svc_strings[128] =
{
	"svc_bad",
	"svc_nop",
	"svc_disconnect",
	"svc_updatestat",
	"svc_version",		// [long] server version
	"svc_setview",		// [short] entity number
	"svc_sound",		// <see code>
	"svc_time",		// [float] server time
	"svc_print",		// [string] null terminated string
	"svc_stufftext",	// [string] stuffed into client's console buffer
				// the string should be \n terminated
	"svc_setangle",		// [vec3] set the view angle to this absolute value

	"svc_serverinfo",	// [long] version
				// [string] signon string
				// [string]..[0]model cache [string]...[0]sounds cache
				// [string]..[0]item cache
	"svc_lightstyle",	// [byte] [string]
	"svc_updatename",	// [byte] [string]
	"svc_updatefrags",	// [byte] [short]
	"svc_clientdata",	// <shortbits + data>
	"svc_stopsound",	// <see code>
	"svc_updatecolors",	// [byte] [byte]
	"svc_particle",		// [vec3] <variable>
	"svc_damage",		// [byte] impact [byte] blood [vec3] from

	"svc_spawnstatic",
	"OBSOLETE svc_spawnbinary",
	"svc_spawnbaseline",

	"svc_temp_entity",	// <variable>
	"svc_setpause",
	"svc_signonnum",
	"svc_centerprint",
	"svc_killedmonster",
	"svc_foundsecret",
	"svc_spawnstaticsound",
	"svc_intermission",
	"svc_finale",		// [string] music [string] text
	"svc_cdtrack",		// [byte] track [byte] looptrack
	"svc_sellscreen",
	"svc_cutscene",
	"svc_showlmp",		// [string] iconlabel [string] lmpfile [byte] x [byte] y
	"svc_hidelmp",		// [string] iconlabel
	"svc_skybox",		// [string] skyname
	"?", // 38
	"?", // 39
	"?", // 40
	"?", // 41
	"?", // 42
	"?", // 43
	"?", // 44
	"?", // 45
	"?", // 46
	"?", // 47
	"?", // 48
	"?", // 49
	"?", // 50
	"svc_fog"		// [byte] enable <optional past this point, only included if enable is true> [float] density [byte] red [byte] green [byte] blue
};

//=============================================================================

/*
===============
CL_EntityNum

This error checks and tracks the total number of entities
===============
*/
entity_t *CL_EntityNum (int num)
{
	if (num < 0 || num >= cl.num_entities)
	{
		if (num < 0 || num >= MAX_EDICTS)
			Host_Error ("CL_EntityNum: invalid edict (%d, max = %d)", num, MAX_EDICTS);

		while (cl.num_entities<=num)
		{
			cl_entities[cl.num_entities].colormap = vid.colormap;
			cl.num_entities++;
		}
	}

	return &cl_entities[num];
}

/*
==================
CL_ReadByteShort2
==================
*/
int CL_ReadByteShort2 (qboolean Compatibility)
{
	if (cl.Protocol < PROTOCOL_VERSION_BJP2 || Compatibility && cl.Protocol > PROTOCOL_VERSION_BJP2)
		return MSG_ReadByte (); // Some progs (Marcher) send sound services, maintain compatibility, kludge
	else
		return MSG_ReadShort ();
}

/*
==================
CL_ParseStartSoundPacket
==================
*/
void CL_ParseStartSoundPacket(void)
{
	vec3_t	pos;
	int 	channel, ent;
	int 	sound_num;
	int 	volume;
	int 	field_mask;
	float 	attenuation;
	int	i;

	field_mask = MSG_ReadByte();

	if (field_mask & SND_VOLUME)
		volume = MSG_ReadByte ();
	else
		volume = DEFAULT_SOUND_PACKET_VOLUME;

	if (field_mask & SND_ATTENUATION)
		attenuation = MSG_ReadByte () / 64.0;
	else
		attenuation = DEFAULT_SOUND_PACKET_ATTENUATION;

	channel = MSG_ReadShort ();
	sound_num = CL_ReadByteShort2 (false);

	if (sound_num >= MAX_SOUNDS)
		Host_Error("CL_ParseStartSoundPacket: invalid sound_num (%d, max = %d)", sound_num, MAX_SOUNDS);

	ent = (channel & 0x7FFF) >> 3;
	channel &= 7;

	if (ent < 0 || ent >= MAX_EDICTS)
		Host_Error ("CL_ParseStartSoundPacket: invalid edict (%d, max = %d)", ent, MAX_EDICTS);

	for (i=0 ; i<3 ; i++)
		pos[i] = MSG_ReadCoord ();

	S_StartSound (ent, channel, cl.sound_precache[sound_num], pos, volume/255.0, attenuation);
}

/*
==================
CL_KeepaliveMessage

When the client is taking a long time to load stuff, send keepalive messages
so the server doesn't disconnect.
==================
*/
void CL_KeepaliveMessage (void)
{
	float	     time;
	static float lastmsg;
	int	     ret;
	sizebuf_t    old;
	byte	     olddata[NET_MAXMESSAGE], data;

	if (sv.active)
		return;		// no need if server is local
	if (cls.demoplayback)
		return;

// read messages from server, should just be nops
	old = net_message;
	memcpy (olddata, net_message.data, net_message.cursize);

	do
	{
		ret = CL_GetMessage ();
		switch (ret)
		{
		default:
			Host_Error ("CL_KeepaliveMessage: CL_GetMessage failed (%d)", ret);
		case 0:
			break;	// nothing waiting
		case 1:
			Host_Error ("CL_KeepaliveMessage: received a message");
			break;
		case 2:
			data = MSG_ReadByte ();

			if (data != svc_nop)
				Host_Error ("CL_KeepaliveMessage: datagram %d wasn't a nop", data);
			break;
		}
	} while (ret);

	net_message = old;
	memcpy (net_message.data, olddata, net_message.cursize);

// check time
	time = Sys_FloatTime ();
	if (time - lastmsg < 5)
		return;
	lastmsg = time;

// write out a nop
	Con_Printf ("--> client to server keepalive\n");

	MSG_WriteByte (&cls.message, clc_nop);
	NET_SendMessage (cls.netcon, &cls.message);
	SZ_Clear (&cls.message);
}

/*
==================
CL_ParseServerInfo
==================
*/
void CL_ParseServerInfo (void)
{
	char	*str, Str2[MAX_QPATH + 100];
	int	i;
	int	nummodels, numsounds;
	char	model_precache[MAX_MODELS][MAX_QPATH];
	char	sound_precache[MAX_SOUNDS][MAX_QPATH];

	Con_DPrintf ("Serverinfo packet received.\n");
//
// wipe the client_state_t struct
//
	CL_ClearState ();

// parse protocol version number
	i = MSG_ReadLong ();
	if (i != PROTOCOL_VERSION && (i < PROTOCOL_VERSION_BJP || i > PROTOCOL_VERSION_BJP3))
		Host_Error ("Server returned version %i, not %i or %i-%i", i, PROTOCOL_VERSION, PROTOCOL_VERSION_BJP, PROTOCOL_VERSION_BJP3);

	cl.Protocol = i;

//	Con_DPrintf ("Server protocol is %i\n", i);

// parse maxclients
	cl.maxclients = MSG_ReadByte ();
	if (cl.maxclients < 1 || cl.maxclients > MAX_SCOREBOARD)
	{
		Con_Printf("Bad maxclients (%u) from server\n", cl.maxclients);
		return;
	}
	cl.scores = Hunk_AllocName (cl.maxclients*sizeof(*cl.scores), "scores");

// parse gametype
	cl.gametype = MSG_ReadByte ();

// parse signon message
	str = MSG_ReadString ();
	strncpy (cl.levelname, str, sizeof(cl.levelname)-1);

// seperate the printfs so the server message can have a color
	Con_SafePrintf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n\n");
	Con_SafePrintf ("%c%s\n", 2, str);

//
// first we go through and touch all of the precache data that still
// happens to be in the cache, so precaching something else doesn't
// needlessly purge it
//

// precache models
	memset (cl.model_precache, 0, sizeof(cl.model_precache));
	for (nummodels=1 ; ; nummodels++)
	{
		str = MSG_ReadString ();
		if (!str[0])
			break;
		if (nummodels==MAX_MODELS)
			Host_Error ("Server sent too many model precaches (max = %d)", MAX_MODELS);
		strcpy (model_precache[nummodels], str);
		Mod_TouchModel (str);
	}

// precache sounds
	memset (cl.sound_precache, 0, sizeof(cl.sound_precache));
	for (numsounds=1 ; ; numsounds++)
	{
		str = MSG_ReadString ();
		if (!str[0])
			break;
		if (numsounds==MAX_SOUNDS)
			Host_Error ("Server sent too many sound precaches (max = %d)", MAX_SOUNDS);
		strcpy (sound_precache[numsounds], str);
		S_TouchSound (str);
	}

//
// now we try to load everything else until a cache allocation fails
//

	Mod_IsWorldModel = true;

	for (i=1 ; i<nummodels ; i++)
	{
		cl.model_precache[i] = Mod_ForName (model_precache[i], false);
		Mod_IsWorldModel = false; // Make sure to reset this
		if (cl.model_precache[i] == NULL)
		{
			sprintf (Str2, "Model %s not found", model_precache[i]);

			if (i == 1)
				Host_Error (Str2); // World not found

			Con_Printf ("%s\n", Str2);
//			return; // Warning: Not returning might cause crashes later on in GL, e.g. in R_DrawBrushModel
				// Probably fixed now, allowing NULL entries in the list
		}
		CL_KeepaliveMessage ();
	}

	S_BeginPrecaching ();
	for (i=1 ; i<numsounds ; i++)
	{
		cl.sound_precache[i] = S_PrecacheSound (sound_precache[i]);
		CL_KeepaliveMessage ();
	}
	S_EndPrecaching ();


// local state
	cl_entities[0].model = cl.worldmodel = cl.model_precache[1];

	R_NewMap ();

	Hunk_Check ();		// make sure nothing is hurt

	noclip_anglehack = false;		// noclip is turned off at start
}

/*
==================
CL_ReadByteShort
==================
*/
int CL_ReadByteShort (void)
{
	return cl.Protocol == PROTOCOL_VERSION ? MSG_ReadByte () : MSG_ReadShort ();
}

/*
==================
CL_ParseUpdate

Parse an entity update message from the server
If an entities model or origin changes from frame to frame, it must be
relinked.  Other attributes can change without relinking.
==================
*/
void CL_ParseUpdate (int bits)
{
	int	    i;
	model_t	    *model;
	int	    modnum;
	qboolean    forcelink;
	entity_t    *ent;
	int	    num;
	int	    skin;

	if (cls.signon == SIGNONS - 1)
	{	// first update is the final signon stage
		cls.signon = SIGNONS;
		CL_SignonReply ();
	}

	if (bits & U_MOREBITS)
	{
		i = MSG_ReadByte ();
		bits |= (i<<8);
	}

	if (bits & U_LONGENTITY)
		num = MSG_ReadShort ();
	else
		num = MSG_ReadByte ();

	ent = CL_EntityNum (num);

	// Exclude the view from this, otherwise screen sometimes jerks badly in some demos
	if (ent->msgtime != cl.mtime[1] && (!cls.demoplayback || ent != &cl_entities[cl.viewentity]))
		forcelink = true;	// no previous frame to lerp from
	else
		forcelink = false;

	ent->msgtime = cl.mtime[0];

	if (bits & U_MODEL)
	{
		modnum = CL_ReadByteShort ();
		if (modnum < 0 || modnum >= MAX_MODELS)
			Host_Error ("CL_ParseUpdate: invalid model (%d, max = %d)", modnum, MAX_MODELS);
	}
	else
		modnum = ent->baseline.modelindex;

	model = cl.model_precache[modnum];
	if (model != ent->model)
	{
		ent->model = model;
	// automatic animation (torches, etc) can be either all together
	// or randomized
		if (model)
		{
			if (model->synctype == ST_RAND)
				ent->syncbase = (float)(rand()&0x7fff) / 0x7fff;
			else
				ent->syncbase = 0.0;
		}
		else
			forcelink = true;	// hack to make null model players work
#ifdef GLQUAKE
		if (num > 0 && num <= cl.maxclients)
			R_TranslatePlayerSkin (num - 1);
#endif
	}

	if (bits & U_FRAME)
		ent->frame = MSG_ReadByte ();
	else
		ent->frame = ent->baseline.frame;

	if (bits & U_COLORMAP)
		i = MSG_ReadByte();
	else
		i = ent->baseline.colormap;
	if (!i)
		ent->colormap = vid.colormap;
	else
	{
		if (i < 0 || i > cl.maxclients)
			Sys_Error ("CL_ParseUpdate: invalid colormap (%d, max = %d)", i, cl.maxclients);
		ent->colormap = cl.scores[i-1].translations;
	}

#ifdef GLQUAKE
	if (bits & U_SKIN)
		skin = MSG_ReadByte();
	else
		skin = ent->baseline.skin;
	if (skin != ent->skinnum) {
		ent->skinnum = skin;
		if (num > 0 && num <= cl.maxclients)
			R_TranslatePlayerSkin (num - 1);
	}

#else

	if (bits & U_SKIN)
		ent->skinnum = MSG_ReadByte();
	else
		ent->skinnum = ent->baseline.skin;
#endif

	if (bits & U_EFFECTS)
		ent->effects = MSG_ReadByte();
	else
		ent->effects = ent->baseline.effects;

// shift the known values for interpolation
	VectorCopy (ent->msg_origins[0], ent->msg_origins[1]);
	VectorCopy (ent->msg_angles[0], ent->msg_angles[1]);

	if (bits & U_ORIGIN1)
		ent->msg_origins[0][0] = MSG_ReadCoord ();
	else
		ent->msg_origins[0][0] = ent->baseline.origin[0];
	if (bits & U_ANGLE1)
		ent->msg_angles[0][0] = MSG_ReadAngle();
	else
		ent->msg_angles[0][0] = ent->baseline.angles[0];

	if (bits & U_ORIGIN2)
		ent->msg_origins[0][1] = MSG_ReadCoord ();
	else
		ent->msg_origins[0][1] = ent->baseline.origin[1];
	if (bits & U_ANGLE2)
		ent->msg_angles[0][1] = MSG_ReadAngle();
	else
		ent->msg_angles[0][1] = ent->baseline.angles[1];

	if (bits & U_ORIGIN3)
		ent->msg_origins[0][2] = MSG_ReadCoord ();
	else
		ent->msg_origins[0][2] = ent->baseline.origin[2];
	if (bits & U_ANGLE3)
		ent->msg_angles[0][2] = MSG_ReadAngle();
	else
		ent->msg_angles[0][2] = ent->baseline.angles[2];

        // Nehahra
	if (bits & U_TRANS)
	{
		int temp = MSG_ReadFloat ();
		
#ifdef GLQUAKE
		ent->transparency = MSG_ReadFloat ();

		if (temp == 2)
			ent->fullbright = MSG_ReadFloat();
#else
		// Just parse some fields
		MSG_ReadFloat ();
		
		if (temp == 2)
			MSG_ReadFloat ();
#endif
	}
#ifdef GLQUAKE
	else
	{
		ent->transparency = 1.0;
		ent->fullbright = 0;
	}
#endif

#ifdef GLQUAKE
	if ((bits & U_NOLERP) || ent->model && ent->model->nolerp)
	{
		extern cvar_t cl_altlerp, host_framerate;

		// Avoid forcelink when local server not active, interpolation and cl_altlerp enabled,
		// cl_altlerp == 2 or host_framerate not 0 or model is nolerp type (wizard, wraith ...) ... kludge
		if (sv.active || !r_interpolate_model_animation.value || !cl_altlerp.value ||
		    !(cl_altlerp.value == 2 || host_framerate.value || ent->model && ent->model->nolerp))
#else
	if (bits & U_NOLERP)
	{
#endif
			ent->forcelink = true;
	}

	if ( forcelink )
	{	// didn't have an update last message
		VectorCopy (ent->msg_origins[0], ent->msg_origins[1]);
		VectorCopy (ent->msg_origins[0], ent->origin);
		VectorCopy (ent->msg_angles[0], ent->msg_angles[1]);
		VectorCopy (ent->msg_angles[0], ent->angles);
		ent->forcelink = true;
	}
}

/*
==================
CL_ParseBaseline
==================
*/
void CL_ParseBaseline (entity_t *ent)
{
	int			i;

	ent->baseline.modelindex = CL_ReadByteShort ();
	ent->baseline.frame = MSG_ReadByte ();
	ent->baseline.colormap = MSG_ReadByte();
	ent->baseline.skin = MSG_ReadByte();
	for (i=0 ; i<3 ; i++)
	{
		ent->baseline.origin[i] = MSG_ReadCoord ();
		ent->baseline.angles[i] = MSG_ReadAngle ();
	}
}


/*
==================
CL_ParseClientdata

Server information pertaining to this client only
==================
*/
void CL_ParseClientdata (int bits)
{
	int		i, j;

	if (bits & SU_VIEWHEIGHT)
		cl.viewheight = MSG_ReadChar ();
	else
		cl.viewheight = DEFAULT_VIEWHEIGHT;

	if (bits & SU_IDEALPITCH)
		cl.idealpitch = MSG_ReadChar ();
	else
		cl.idealpitch = 0;

	VectorCopy (cl.mvelocity[0], cl.mvelocity[1]);
	for (i=0 ; i<3 ; i++)
	{
		if (bits & (SU_PUNCH1<<i) )
			cl.punchangle[i] = MSG_ReadChar();
		else
			cl.punchangle[i] = 0;
		if (bits & (SU_VELOCITY1<<i) )
			cl.mvelocity[0][i] = MSG_ReadChar()*16;
		else
			cl.mvelocity[0][i] = 0;
	}

// [always sent]	if (bits & SU_ITEMS)
		i = MSG_ReadLong ();

	if (cl.items != i)
	{	// set flash times
		Sbar_Changed ();
		for (j=0 ; j<32 ; j++)
			if ( (i & (1<<j)) && !(cl.items & (1<<j)))
				cl.item_gettime[j] = cl.time;
		cl.items = i;
	}

	cl.onground = (bits & SU_ONGROUND) != 0;
	cl.inwater = (bits & SU_INWATER) != 0;

	if (bits & SU_WEAPONFRAME)
		cl.stats[STAT_WEAPONFRAME] = MSG_ReadByte ();
	else
		cl.stats[STAT_WEAPONFRAME] = 0;

	if (bits & SU_ARMOR)
		i = MSG_ReadByte ();
	else
		i = 0;
	if (cl.stats[STAT_ARMOR] != i)
	{
		cl.stats[STAT_ARMOR] = i;
		Sbar_Changed ();
	}

	if (bits & SU_WEAPON)
		i = CL_ReadByteShort ();
	else
		i = 0;
	if (cl.stats[STAT_WEAPON] != i)
	{
		cl.stats[STAT_WEAPON] = i;
		Sbar_Changed ();
	}

	i = MSG_ReadShort ();
	if (cl.stats[STAT_HEALTH] != i)
	{
		cl.stats[STAT_HEALTH] = i;
		Sbar_Changed ();
	}

	i = MSG_ReadByte ();
	if (cl.stats[STAT_AMMO] != i)
	{
		cl.stats[STAT_AMMO] = i;
		Sbar_Changed ();
	}

	for (i=0 ; i<4 ; i++)
	{
		j = MSG_ReadByte ();
		if (cl.stats[STAT_SHELLS+i] != j)
		{
			cl.stats[STAT_SHELLS+i] = j;
			Sbar_Changed ();
		}
	}

	i = MSG_ReadByte ();

	if (standard_quake)
	{
		if (cl.stats[STAT_ACTIVEWEAPON] != i)
		{
			cl.stats[STAT_ACTIVEWEAPON] = i;
			Sbar_Changed ();
		}
	}
	else
	{
		if (cl.stats[STAT_ACTIVEWEAPON] != (1<<i))
		{
			cl.stats[STAT_ACTIVEWEAPON] = (1<<i);
			Sbar_Changed ();
		}
	}
}

/*
=====================
CL_NewTranslation
=====================
*/
void CL_NewTranslation (int slot)
{
	int	i, j;
	int	top, bottom;
	byte	*dest, *source;

	if (slot > cl.maxclients)
		Sys_Error ("CL_NewTranslation: invalid slot (%d, max = %d)", slot, cl.maxclients);
	dest = cl.scores[slot].translations;
	source = vid.colormap;
	memcpy (dest, vid.colormap, sizeof(cl.scores[slot].translations));
	top = cl.scores[slot].colors & 0xf0;
	bottom = (cl.scores[slot].colors &15)<<4;
#ifdef GLQUAKE
	R_TranslatePlayerSkin (slot);
#endif

	for (i=0 ; i<VID_GRADES ; i++, dest += 256, source+=256)
	{
		if (top < 128)	// the artists made some backwards ranges.  sigh.
			memcpy (dest + TOP_RANGE, source + top, 16);
		else
			for (j=0 ; j<16 ; j++)
				dest[TOP_RANGE+j] = source[top+15-j];

		if (bottom < 128)
			memcpy (dest + BOTTOM_RANGE, source + bottom, 16);
		else
			for (j=0 ; j<16 ; j++)
				dest[BOTTOM_RANGE+j] = source[bottom+15-j];
	}
}

/*
=====================
CL_ParseStatic
=====================
*/
void CL_ParseStatic (void)
{
	entity_t    *ent;
	int	    i;

	i = cl.num_statics;
	if (i >= MAX_STATIC_ENTITIES)
		Host_Error ("CL_ParseStatic: too many (%d) static entities, max = %d", i, MAX_STATIC_ENTITIES);
	ent = &cl_static_entities[i];
	cl.num_statics++;
	CL_ParseBaseline (ent);

// copy it to the current state
	ent->model = cl.model_precache[ent->baseline.modelindex];
	ent->frame = ent->baseline.frame;
	ent->syncbase = (float)rand() / RAND_MAX; // Hack to make flames unsynchronized
	ent->colormap = vid.colormap;
	ent->skinnum = ent->baseline.skin;
	ent->effects = ent->baseline.effects;

	VectorCopy (ent->baseline.origin, ent->origin);
	VectorCopy (ent->baseline.angles, ent->angles);
	R_AddEfrags (ent);
}

/*
===================
CL_ParseStaticSound
===================
*/
void CL_ParseStaticSound (void)
{
	vec3_t	org;
	int	sound_num, vol, atten;
	int	i;

	for (i=0 ; i<3 ; i++)
		org[i] = MSG_ReadCoord ();
	sound_num = CL_ReadByteShort2 (true);
	
	if (sound_num >= MAX_SOUNDS)
		Host_Error ("CL_ParseStaticSound: invalid sound (%d, max = %d", sound_num, MAX_SOUNDS);
		
	vol = MSG_ReadByte ();
	atten = MSG_ReadByte ();

	S_StaticSound (cl.sound_precache[sound_num], org, vol, atten);
}

/*
===================
CL_FixIntermission
===================
*/
void CL_FixIntermission (int Intermission)
{
	cl.intermission = Intermission;
	cl.completed_time = cl.time;
	vid.recalc_refdef = true;	// go to full screen
	
	if (cls.demoplayback)
	{
		// Fix camera view angles (better way to do it?)
		entity_t *ent = &cl_entities[cl.viewentity];
		VectorCopy (ent->msg_angles[0], ent->angles);
	}
	
	if (cl.intermission != 1)
		SCR_CenterPrint (MSG_ReadString ());
}

static char *Svc_Name (int cmd)
{
	if (cmd == -1)
		return "none";

	cmd &= 255;

	if (cmd & 128)
		return "fast update";

	return svc_strings[cmd];
}

#define SHOWNET(x) if(cl_shownet.value==2)Con_Printf ("%3i:%s\n", msg_readcount-1, x);

/*
=====================
CL_ParseServerMessage
=====================
*/
void CL_ParseServerMessage (void)
{
	int cmd = -1;
	int i, lastpos, lastcmd;

//
// if recording demos, copy the message out
//
	if (cl_shownet.value == 1)
		Con_Printf ("%i ",net_message.cursize);
	else if (cl_shownet.value == 2)
		Con_Printf ("------------------\n");

	cl.onground = false;	// unless the server says otherwise
//
// parse the message
//
	MSG_BeginReading ();

	while (1)
	{
		if (msg_badread)
		{
			char Str[512];

			sprintf (Str, "CL_ParseServerMessage: insufficient data in service '%s', size %d", Svc_Name(cmd), msg_readcount - lastpos);
			Host_Error (Str);
		}

		lastpos = msg_readcount;
		lastcmd = cmd;

		cmd = MSG_ReadByte ();

		if (cmd == -1)
		{
			SHOWNET("END OF MESSAGE");
			return;		// end of message
		}

	// if the high bit of the command byte is set, it is a fast update
		if (cmd & 128)
		{
			SHOWNET("fast update");
			CL_ParseUpdate (cmd&127);
			continue;
		}

		SHOWNET(svc_strings[cmd]);

	// other commands
		switch (cmd)
		{
		default:
			Host_Error ("CL_ParseServerMessage: illegible server message (service %d, last service '%s')", cmd, Svc_Name(lastcmd));
			break;

		case svc_nop:
//			Con_Printf ("svc_nop\n");
			break;

		case svc_time:
			cl.mtime[1] = cl.mtime[0];
			cl.mtime[0] = MSG_ReadFloat ();
			break;

		case svc_clientdata:
			i = MSG_ReadShort ();
			CL_ParseClientdata (i);
			break;

		case svc_version:
			i = MSG_ReadLong ();
			if (i != PROTOCOL_VERSION && (i < PROTOCOL_VERSION_BJP || i > PROTOCOL_VERSION_BJP3))
				Host_Error ("CL_ParseServerMessage: Server is protocol %i instead of %i or %i-%i", i, PROTOCOL_VERSION, PROTOCOL_VERSION_BJP, PROTOCOL_VERSION_BJP3);
			cl.Protocol = i;
			break;

		case svc_disconnect:
			Host_EndGame ("Server disconnected\n");

		case svc_print:
			Con_SafePrintf ("%s", MSG_ReadString ());
			break;

		case svc_centerprint:
			SCR_CenterPrint (MSG_ReadString ());
			break;

		case svc_stufftext:
			Cbuf_AddText (MSG_ReadString ());
			break;

		case svc_damage:
			V_ParseDamage ();
			break;

		case svc_serverinfo:
			CL_ParseServerInfo ();
			vid.recalc_refdef = true;	// leave intermission full screen
			break;

		case svc_setangle:
			for (i=0 ; i<3 ; i++)
				cl.viewangles[i] = MSG_ReadAngle ();
			break;

		case svc_setview:
			cl.viewentity = MSG_ReadShort ();
			if (cl.viewentity >= MAX_EDICTS)
				Host_Error ("CL_ParseServerMessage: svc_setview %d >= MAX_EDICTS (%d)", cl.viewentity, MAX_EDICTS);
			break;

		case svc_lightstyle:
			i = MSG_ReadByte ();
			if (i >= MAX_LIGHTSTYLES)
				Sys_Error ("CL_ParseServerMessage: svc_lightstyle %d >= MAX_LIGHTSTYLES (%d)", i, MAX_LIGHTSTYLES);
			Q_strcpy (cl_lightstyle[i].map,  MSG_ReadString());
			cl_lightstyle[i].length = Q_strlen(cl_lightstyle[i].map);
			break;

		case svc_sound:
			CL_ParseStartSoundPacket();
			break;

		case svc_stopsound:
			i = MSG_ReadShort();
			S_StopSound(i>>3, i&7);
			break;

		case svc_updatename:
			Sbar_Changed ();
			i = MSG_ReadByte ();
			if (i >= cl.maxclients)
				Host_Error ("CL_ParseServerMessage: svc_updatename %d >= cl.maxclients (%d)", i, cl.maxclients);
			strcpy (cl.scores[i].name, MSG_ReadString ());
			break;

		case svc_updatefrags:
			Sbar_Changed ();
			i = MSG_ReadByte ();
			if (i >= cl.maxclients)
				Host_Error ("CL_ParseServerMessage: svc_updatefrags %d >= cl.maxclients (%d)", i, cl.maxclients);
			cl.scores[i].frags = MSG_ReadShort ();
			break;

		case svc_updatecolors:
			Sbar_Changed ();
			i = MSG_ReadByte ();
			if (i >= cl.maxclients)
				Host_Error ("CL_ParseServerMessage: svc_updatecolors %d >= cl.maxclients (%d)", i, cl.maxclients);
			cl.scores[i].colors = MSG_ReadByte ();
			CL_NewTranslation (i);
			break;

		case svc_particle:
			R_ParseParticleEffect ();
			break;

		case svc_spawnbaseline:
			i = MSG_ReadShort ();
			// must use CL_EntityNum() to force cl.num_entities up
			CL_ParseBaseline (CL_EntityNum(i));
			break;
		case svc_spawnstatic:
			CL_ParseStatic ();
			break;
		case svc_temp_entity:
			CL_ParseTEnt ();
			break;

		case svc_setpause:
			{
				cl.paused = MSG_ReadByte ();

				if (cl.paused)
				{
					CDAudio_Pause ();
#ifdef _WIN32
					VID_HandlePause (true);
#endif
				}
				else
				{
					CDAudio_Resume ();
#ifdef _WIN32
					VID_HandlePause (false);
#endif
				}
			}
			break;

		case svc_signonnum:
			i = MSG_ReadByte ();
			if (i <= cls.signon)
				Host_Error ("CL_ParseServerMessage: Received signon %i when at %i", i, cls.signon);
			cls.signon = i;
			CL_SignonReply ();
			break;

		case svc_killedmonster:
			cl.stats[STAT_MONSTERS]++;

			if ((!cls.demoplayback || developer.value) &&
			    cl.stats[STAT_TOTALMONSTERS] && cl.stats[STAT_MONSTERS] > cl.stats[STAT_TOTALMONSTERS])
			{
				Con_Printf ("\002CL_ParseServerMessage: ");
				Con_Printf ("killed monsters %d > total monsters %d\n", cl.stats[STAT_MONSTERS], cl.stats[STAT_TOTALMONSTERS]);
			}
			
			break;

		case svc_foundsecret:
			cl.stats[STAT_SECRETS]++;
			break;

		case svc_updatestat:
			i = MSG_ReadByte ();
			if (i >= MAX_CL_STATS)
				Sys_Error ("CL_ParseServerMessage: invalid svc_updatestat (%d, max = %d)", i, MAX_CL_STATS);
			cl.stats[i] = MSG_ReadLong ();;
			break;

		case svc_spawnstaticsound:
			CL_ParseStaticSound ();
			break;

		case svc_cdtrack:
			cl.cdtrack = MSG_ReadByte ();
			cl.looptrack = MSG_ReadByte ();
			if ( (cls.demoplayback || cls.demorecording) && (cls.forcetrack != -1) )
				CDAudio_Play ((byte)cls.forcetrack, true);
			else
				CDAudio_Play ((byte)cl.cdtrack, true);
			break;

		case svc_intermission:
			CL_FixIntermission (1);
			break;

		case svc_finale:
			CL_FixIntermission (2);
			break;

		case svc_cutscene:
			CL_FixIntermission (3);
			break;

		case svc_sellscreen:
			Cmd_ExecuteString ("help", src_command);
			break;

// Nehahra
		case svc_hidelmp:
			MSG_ReadString (); // Just parse msg
                        break;
                
		case svc_showlmp:
			// Just parse msg
			MSG_ReadString ();
			MSG_ReadString ();
			MSG_ReadByte();
			MSG_ReadByte();
			break;
                
		case svc_skybox:
#ifdef GLQUAKE
                        R_LoadSkys (MSG_ReadString());
#else
                        MSG_ReadString (); // Just parse msg
#endif
                        break;
		case svc_fog:
			if (MSG_ReadByte())
			{
#ifdef GLQUAKE
                                Cvar_SetValue ("gl_fogdensity", MSG_ReadFloat());
                                Cvar_SetValue ("gl_fogred", MSG_ReadByte() / 255.0);
                                Cvar_SetValue ("gl_foggreen", MSG_ReadByte() / 255.0);
                                Cvar_SetValue ("gl_fogblue", MSG_ReadByte() / 255.0);
                                Cvar_SetValue ("gl_fogenable", 1);
#else
				// Just parse msg
				MSG_ReadFloat ();
				MSG_ReadByte ();
				MSG_ReadByte ();
				MSG_ReadByte ();
#endif
                        } else {
#ifdef GLQUAKE
                                Cvar_SetValue ("gl_fogenable", 0);
                                Cvar_SetValue ("gl_fogdensity", 0);
#endif
                        }
			break;
		}
	}
}

