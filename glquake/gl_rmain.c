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
// r_main.c

#include "quakedef.h"

entity_t	r_worldentity;

qboolean	r_cache_thrash;		// compatability

vec3_t		modelorg, r_entorigin;
entity_t	*currententity;

int		r_visframecount;	// bumped when going to a new PVS
int		r_framecount;		// used for dlight push checking

mplane_t	frustum[4];

int		c_brush_polys, c_alias_polys;

qboolean	envmap;				// true during envmap command capture

int		currenttexture = -1;		// to avoid unnecessary texture sets

int		cnttextures[2] = {-1, -1};     // cached

int		particletexture;	// little dot for particles
int		playertextures;		// up to 16 color translated skins
int		skyboxtextures;

int		mirrortexturenum;	// quake texturenum, not gltexturenum
qboolean	mirror;
mplane_t	*mirror_plane;

//
// view origin
//
vec3_t	vup;
vec3_t	vpn;
vec3_t	vright;
vec3_t	r_origin;

float	r_world_matrix[16];
float	r_base_world_matrix[16];

//
// screen size info
//
refdef_t	r_refdef;

mleaf_t		*r_viewleaf, *r_oldviewleaf;

texture_t	*r_notexture_mip;

int		d_lightstylevalue[256];	// 8.8 fraction of base light value


// Interpolation
void GL_DrawAliasBlendedShadow (aliashdr_t *paliashdr, int pose1, int pose2, entity_t* e);
void R_BlendedRotateForEntity (entity_t *e, qboolean shadow);
void R_SetupAliasBlendedFrame (int frame, aliashdr_t *paliashdr, entity_t* e);
void GL_DrawAliasBlendedFrame (aliashdr_t *paliashdr, int pose1, int pose2, float blend);

cvar_t	r_norefresh = {"r_norefresh","0"};
cvar_t	r_drawentities = {"r_drawentities","1"};
cvar_t	r_drawviewmodel = {"r_drawviewmodel","1"};
cvar_t	r_speeds = {"r_speeds","0"};
cvar_t	r_fullbright = {"r_fullbright","0"};
cvar_t	r_lightmap = {"r_lightmap","0"};
cvar_t	r_shadows = {"r_shadows","1"};
cvar_t	r_mirroralpha = {"r_mirroralpha","1"};
cvar_t	r_wateralpha = {"r_wateralpha","1"};
cvar_t	r_dynamic = {"r_dynamic","1"};
cvar_t	r_novis = {"r_novis","0"};
cvar_t	r_flatlightstyles = {"r_flatlightstyles", "0"};
cvar_t	r_waterwarp = {"r_waterwarp", "0"};
cvar_t	r_clearcolor = {"r_clearcolor", "251"}; // Closest to the original
cvar_t  r_oldsky = {"r_oldsky", "1"};
cvar_t  r_skytq = {"r_skytq", "0"};
cvar_t  r_interpolate_model_animation = {"r_interpolate_model_animation", "1", true};
cvar_t  r_interpolate_model_transform = {"r_interpolate_model_transform", "1", true};
cvar_t  r_interpolate_model_weapon = {"r_interpolate_model_weapon", "1", true};

cvar_t	gl_finish = {"gl_finish","0"};
cvar_t	gl_clear = {"gl_clear","0"};
cvar_t	gl_cull = {"gl_cull","1"};
cvar_t	gl_texsort = {"gl_texsort","1"};
cvar_t	gl_smoothmodels = {"gl_smoothmodels","1"};
cvar_t	gl_affinemodels = {"gl_affinemodels","0"};
cvar_t	gl_polyblend = {"gl_polyblend","1"};
cvar_t	gl_flashblend = {"gl_flashblend","0"};
cvar_t	gl_playermip = {"gl_playermip","0"};
cvar_t	gl_nocolors = {"gl_nocolors","0"};
cvar_t	gl_keeptjunctions = {"gl_keeptjunctions","1"};
cvar_t	gl_doubleeyes = {"gl_doubleeys", "1"};
cvar_t  gl_fogenable = {"gl_fogenable", "0"};
cvar_t  gl_fogdensity = {"gl_fogdensity", "0.8"};
cvar_t  gl_fogred = {"gl_fogred","0.3"};
cvar_t  gl_fogblue = {"gl_fogblue","0.3"};
cvar_t  gl_foggreen = {"gl_foggreen","0.3"};
cvar_t  gl_skyclip = {"gl_skyclip", "4608"}; // Farclip for sky in fog
cvar_t  gl_alphablend = {"gl_alphablend", "0"}; // Automatic alpha blending of external sprites
cvar_t  gl_oldspr = {"gl_oldspr", "0"}; // Old opaque sprite

extern	cvar_t	gl_ztrick;

float model_alpha; // Nehahra

/*
=================
R_CullBox

Returns true if the box is completely outside the frustom
=================
*/
qboolean R_CullBox (vec3_t mins, vec3_t maxs)
{
	int		i;

	for (i=0 ; i<4 ; i++)
	if (BoxOnPlaneSide (mins, maxs, &frustum[i]) == 2)
	return true;
	return false;
}


void R_RotateForEntity (entity_t *e, qboolean shadow)
{
	glTranslatef (e->origin[0],  e->origin[1],  e->origin[2]);

	glRotatef (e->angles[1],  0, 0, 1);
	
	if (shadow)
	return;

	glRotatef (-e->angles[0],  0, 1, 0);
	glRotatef (e->angles[2],  1, 0, 0);
}

/*
=============================================================

SPRITE MODELS

=============================================================
*/

/*
================
R_GetSpriteFrame
================
*/
mspriteframe_t *R_GetSpriteFrame (entity_t *currententity)
{
	msprite_t	*psprite;
	mspritegroup_t	*pspritegroup;
	mspriteframe_t	*pspriteframe;
	int		i, numframes, frame;
	float		*pintervals, fullinterval, targettime, time;
	static float	lastmsg = 0;

	psprite = currententity->model->cache.data;
	frame = currententity->frame;

	if (frame >= psprite->numframes || frame < 0)
	{
		if (IsTimeout (&lastmsg, 2) && (frame == currententity->model->maxframe || frame < 0) ||
				frame > currententity->model->maxframe)
		{
			if (frame > currententity->model->maxframe)
			currententity->model->maxframe = frame; // Save peak

			Con_Printf ("R_GetSpriteFrame: no such frame %d (%d frames) in %s\n", frame, psprite->numframes, currententity->model->name);
		}

		frame = 0;
	}

	if (psprite->frames[frame].type == SPR_SINGLE)
	{
		pspriteframe = psprite->frames[frame].frameptr;
	}
	else
	{
		pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		pintervals = pspritegroup->intervals;
		numframes = pspritegroup->numframes;
		fullinterval = pintervals[numframes-1];

		time = cl.time + currententity->syncbase;

		// when loading in Mod_LoadSpriteGroup, we guaranteed all interval values
		// are positive, so we don't have to worry about division by 0
		targettime = time - ((int)(time / fullinterval)) * fullinterval;

		for (i=0 ; i<(numframes-1) ; i++)
		{
			if (pintervals[i] > targettime)
			break;
		}

		pspriteframe = pspritegroup->frames[i];
	}

	return pspriteframe;
}


/*
=================
R_DrawSpriteModel

=================
*/
void R_DrawSpriteModel (entity_t *e)
{
	vec3_t		point;
	mspriteframe_t	*frame;
	float		*up, *right;
	vec3_t		v_forward, v_right, v_up;
	msprite_t	*psprite;

	// don't even bother culling, because it's just a single
	// polygon without a surface cache
	frame = R_GetSpriteFrame (e);
	psprite = currententity->model->cache.data;

	if (psprite->type == SPR_ORIENTED)
	{	// bullet marks on walls
		AngleVectors (currententity->angles, v_forward, v_right, v_up);
		up = v_up;
		right = v_right;
	}
	else
	{	// normal sprite
		up = vup;
		right = vright;
	}

	glColor3f (1,1,1);

	GL_DisableMultitexture();

	GL_Bind(frame->gl_texturenum);

	if (gl_oldspr.value)
	glEnable (GL_ALPHA_TEST);
	else
	{
		glEnable (GL_BLEND);
		glDepthMask (0); // disable zbuffer updates
	}

	glBegin (GL_QUADS);

	glTexCoord2f (0, 1);
	VectorMA (e->origin, frame->down, up, point);
	VectorMA (point, frame->left, right, point);
	glVertex3fv (point);

	glTexCoord2f (0, 0);
	VectorMA (e->origin, frame->up, up, point);
	VectorMA (point, frame->left, right, point);
	glVertex3fv (point);

	glTexCoord2f (1, 0);
	VectorMA (e->origin, frame->up, up, point);
	VectorMA (point, frame->right, right, point);
	glVertex3fv (point);

	glTexCoord2f (1, 1);
	VectorMA (e->origin, frame->down, up, point);
	VectorMA (point, frame->right, right, point);
	glVertex3fv (point);

	glEnd ();

	if (gl_oldspr.value)
	glDisable (GL_ALPHA_TEST);
	else
	{
		glDepthMask (1); // enable zbuffer updates
		glDisable (GL_BLEND);
	}
}

/*
=============================================================

ALIAS MODELS

=============================================================
*/


#define NUMVERTEXNORMALS	162

float	r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

vec3_t	shadevector;
float	shadelight, ambientlight;

// precalculated dot products for quantized angles
#define SHADEDOT_QUANT 16
float	r_avertexnormal_dots[SHADEDOT_QUANT][256] =
#include "anorm_dots.h"
;

float	*shadedots = r_avertexnormal_dots[0];

int	lastposenum;
int     lastposenum0; // Interpolation

/*
=============
GL_DrawAliasFrame
=============
*/
void GL_DrawAliasFrame (aliashdr_t *paliashdr, int posenum)
{
	float	s, t;
	float 	l;
	int		i, j;
	int		index;
	trivertx_t	*v, *verts;
	int		list;
	int		*order;
	vec3_t	point;
	float	*normal;
	int		count;

	if (model_alpha != 1.0)
	glEnable (GL_BLEND);

	lastposenum = posenum;

	verts = (trivertx_t *)((byte *)paliashdr + paliashdr->posedata);
	verts += posenum * paliashdr->poseverts;
	order = (int *)((byte *)paliashdr + paliashdr->commands);

	while (1)
	{
		// get the vertex count and primitive type
		count = *order++;
		if (!count)
		break;		// done
		if (count < 0)
		{
			count = -count;
			glBegin (GL_TRIANGLE_FAN);
		}
		else
		glBegin (GL_TRIANGLE_STRIP);

		do
		{
			// texture coordinates come from the draw list
			glTexCoord2f (((float *)order)[0], ((float *)order)[1]);
			order += 2;

			if (r_fullbright.value || !cl.worldmodel->lightdata)
			l = 1;
			else
			// normals and vertexes come from the frame list
			l = shadedots[verts->lightnormalindex] * shadelight;

			if (model_alpha != 1.0)
			glColor4f (l, l, l, model_alpha);
			else
			glColor3f (l, l, l);

			glVertex3f (verts->v[0], verts->v[1], verts->v[2]);
			verts++;
		} while (--count);

		glEnd ();
	}
}


/*
=============
GL_DrawAliasShadow
=============
*/
extern	vec3_t			lightspot;

void GL_DrawAliasShadow (aliashdr_t *paliashdr, int posenum)
{
	float	s, t, l;
	int		i, j;
	int		index;
	trivertx_t	*v, *verts;
	int		list;
	int		*order;
	vec3_t	point;
	float	*normal;
	float	height, lheight;
	int		count;

	lheight = currententity->origin[2] - lightspot[2];

	height = 0;
	verts = (trivertx_t *)((byte *)paliashdr + paliashdr->posedata);
	verts += posenum * paliashdr->poseverts;
	order = (int *)((byte *)paliashdr + paliashdr->commands);

	height = -lheight + 1.0;

	while (1)
	{
		// get the vertex count and primitive type
		count = *order++;
		if (!count)
		break;		// done
		if (count < 0)
		{
			count = -count;
			glBegin (GL_TRIANGLE_FAN);
		}
		else
		glBegin (GL_TRIANGLE_STRIP);

		do
		{
			// texture coordinates come from the draw list
			// (skipped for shadows) glTexCoord2fv ((float *)order);
			order += 2;

			// normals and vertexes come from the frame list
			point[0] = verts->v[0] * paliashdr->scale[0] + paliashdr->scale_origin[0];
			point[1] = verts->v[1] * paliashdr->scale[1] + paliashdr->scale_origin[1];
			point[2] = verts->v[2] * paliashdr->scale[2] + paliashdr->scale_origin[2];

			point[0] -= shadevector[0]*(point[2]+lheight);
			point[1] -= shadevector[1]*(point[2]+lheight);
			point[2] = height;
			//			height -= 0.001;
			glVertex3fv (point);

			verts++;
		} while (--count);

		glEnd ();
	}
}

/*
=================
R_ChkFrame
=================
*/
int R_ChkFrame (char *function, int frame, aliashdr_t *pahdr, float *plastmsg)
{
	if (frame >= pahdr->numframes || frame < 0)
	{
		if (IsTimeout (plastmsg, 2) && (frame == currententity->model->maxframe || frame < 0) ||
				frame > currententity->model->maxframe)
		{
			if (frame > currententity->model->maxframe)
			currententity->model->maxframe = frame; // Save peak

			Con_DPrintf ("%s: no such frame ", function);

			// Single frame?
			if (pahdr->frames[0].name[0])
			Con_DPrintf ("%d ('%s', %d frames)", frame, pahdr->frames[0].name, pahdr->numframes);
			else
			Con_DPrintf ("group %d (%d groups)", frame, pahdr->numframes);

			Con_DPrintf (" in %s\n", currententity->model->name);
		}

		frame = 0;
	}

	return frame;
}

/*
=================
R_SetupAliasFrame

=================
*/
void R_SetupAliasFrame (int frame, aliashdr_t *paliashdr)
{
	int		pose, numposes;
	float		interval;
	static float	lastmsg = 0;

	frame = R_ChkFrame ("R_SetupAliasFrame", frame, paliashdr, &lastmsg);

	pose = paliashdr->frames[frame].firstpose;
	numposes = paliashdr->frames[frame].numposes;

	if (numposes > 1)
	{
		int firstpose = pose;

		pose = numposes * currententity->syncbase; // Hack to make flames unsynchronized
		interval = paliashdr->frames[frame].interval;
		pose += (int)(cl.time / interval) % numposes;
		pose = firstpose + pose % numposes;
	}

	GL_DrawAliasFrame (paliashdr, pose);
}

#define SHADOW_HEIGHT 100 // No shadow above this height

static float rot_angle; // This is the current, possibly interpolated angle

/*
=================
R_DrawShadow

=================
*/
void R_DrawShadow (entity_t *e, aliashdr_t *paliashdr, int client_no, vec3_t mins)
{
	vec3_t	downmove;
	trace_t	downtrace;
	float	attenuate, angle;
	
	VectorCopy (e->origin, downmove);
	downmove[2] -= SHADOW_HEIGHT * 2;
	memset (&downtrace, 0, sizeof(downtrace));
	
	if (SV_RecursiveHullCheck (cl.worldmodel->hulls, 0, 0, 1, e->origin, downmove, &downtrace))
	return; // Didn't hit ground
	
	attenuate = 1 - CLAMP(0, (mins[2] - downtrace.endpos[2]) / SHADOW_HEIGHT, 1);

	//Con_SafePrintf ("R_DrawShadow: att = %.3f for %s\n", attenuate, e->model->name);
	if (attenuate == 0)
	return; // Entity too high above ground

	glPushMatrix ();

	rot_angle = e->angles[1];

	// fenix@io.com: model transform interpolation
	if (r_interpolate_model_transform.value && (client_no != cl.viewentity || !chase_active.value || !cls.demoplayback))
	R_BlendedRotateForEntity (e, true);
	else
	R_RotateForEntity (e, true);

	// Attenuate with alpha
	if (model_alpha < 1)
	attenuate *= model_alpha * model_alpha * model_alpha;

	glDisable (GL_TEXTURE_2D);
	glEnable (GL_BLEND);
	glColor4f (0, 0, 0, attenuate * (r_shadows.value == 1 ? 0.5 : r_shadows.value));
	glDepthMask(GL_FALSE);

	angle = rot_angle/180*M_PI;

	shadevector[0] = cos(-angle);
	shadevector[1] = sin(-angle);
	shadevector[2] = 1;
	VectorNormalize (shadevector);

	// fenix@io.com: model animation interpolation
	if (r_interpolate_model_animation.value)
	GL_DrawAliasBlendedShadow (paliashdr, lastposenum0, lastposenum, e);
	else
	GL_DrawAliasShadow (paliashdr, lastposenum);

	glDepthMask(GL_TRUE);
	glEnable (GL_TEXTURE_2D);
	glDisable (GL_BLEND);
	glColor4f (1,1,1,1);
	glPopMatrix ();
}

/*
=================
R_DrawAliasModel

=================
*/
void R_DrawAliasModel (entity_t *e, qboolean cull)
{
	int i, j;

	int		lnum;
	vec3_t		dist;
	float		add;
	model_t		*clmodel;
	vec3_t		mins, maxs;
	aliashdr_t	*paliashdr;
	trivertx_t	*verts, *v;
	int		index;
	float		s, t;
	int		anim;
	qboolean        torch = false, isclient = false;
	int		skinnum, clamp, client_no;
	static float	lastmsg = 0;

	clmodel = e->model;
	// Nehahra - Model_Alpha
	model_alpha = e->transparency;
	if (model_alpha == 0)
	model_alpha = 1;

	if (e == &cl.viewent)
	if (cl.items & IT_INVISIBILITY)
	model_alpha = 0.2;

	VectorAdd (e->origin, clmodel->mins, mins);
	VectorAdd (e->origin, clmodel->maxs, maxs);

	if (cull && R_CullBox (mins, maxs))
	return;


	VectorCopy (e->origin, r_entorigin);
	VectorSubtract (r_origin, r_entorigin, modelorg);

	//
	// get lighting information
	//

	ambientlight = shadelight = R_LightPoint (e->origin);

	// allways give the gun some light
	if (e == &cl.viewent && ambientlight < 24)
	ambientlight = shadelight = 24;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if (cl_dlights[lnum].die >= cl.time)
		{
			VectorSubtract (e->origin,
			cl_dlights[lnum].origin,
			dist);
			add = cl_dlights[lnum].radius - Length(dist);

			if (add > 0) {
				ambientlight += add;
				//ZOID models should be affected by dlights as well
				shadelight += add;
			}
		}
	}

	if (!gl_autobright.value)
	clamp = 192;
	else if (gl_autobright.value <= AUTOBRIGHTS)
	clamp = 230;
	else if (gl_autobright.value > AUTOBRIGHTS)
	clamp = 256;

	// clamp lighting so it doesn't overbright as much
	if (ambientlight > 128)
	ambientlight = 128;
	if (ambientlight + shadelight > clamp)
	shadelight = clamp - ambientlight;

	// ZOID: never allow players to go totally black
	client_no = e - cl_entities;
	if (client_no >= 1 && client_no <= cl.maxclients /* && !strcmp (e->model->name, "progs/player.mdl") */)
	{
		isclient = true;

		if (ambientlight < 8)
		ambientlight = shadelight = 8;
	}

	if (clmodel->fullbright)
	{
		ambientlight = shadelight = 256;
		torch = true;
	}
	else if (gl_autobright.value)
	{
		int autobright = gl_autobright.value - 1;

		// Clamp
		if (autobright < 0)
		autobright = 0;
		else if (autobright > AUTOBRIGHTS - 1)
		autobright = AUTOBRIGHTS - 1;

		autobright = clmodel->autobright[autobright];

		if (autobright)
		{
			if (autobright == 256)
			torch = true;

			if (gl_autobright.value > AUTOBRIGHTS)
			{
				// High intensity
				autobright += 50;
				
				if (autobright > 256)
				autobright = 256;
			}
			
			ambientlight = shadelight = autobright;
		}
	}
	
	shadedots = r_avertexnormal_dots[((int)(e->angles[1] * (SHADEDOT_QUANT / 360.0))) & (SHADEDOT_QUANT - 1)];
	shadelight = shadelight / 200.0;

	//
	// locate the proper data
	//
	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);

	c_alias_polys += paliashdr->numtris;

	skinnum = e->skinnum;

	// check skin bounds
	if (skinnum >= paliashdr->numskins || skinnum < 0)
	{
		if (IsTimeout (&lastmsg, 2))
		Con_DPrintf ("R_DrawAliasModel: no such skin %d (%d skins) in %s\n", skinnum, paliashdr->numskins, clmodel->name);

		skinnum = 0;
	}

	//
	// draw all the triangles
	//

	GL_DisableMultitexture();

	glPushMatrix ();
	// fenix@io.com: model transform interpolation
	// Don't interpolate the player in chasecam during demo playback
	if (r_interpolate_model_transform.value && !torch && (client_no != cl.viewentity || !chase_active.value || !cls.demoplayback))
	R_BlendedRotateForEntity (e, false);
	else
	R_RotateForEntity (e, false);

	if (clmodel->eyes && gl_doubleeyes.value) {
		glTranslatef (paliashdr->scale_origin[0], paliashdr->scale_origin[1], paliashdr->scale_origin[2] - (22 + 8));
		// double size of eyes, since they are really hard to see in gl
		glScalef (paliashdr->scale[0]*2, paliashdr->scale[1]*2, paliashdr->scale[2]*2);
	} else {
		glTranslatef (paliashdr->scale_origin[0], paliashdr->scale_origin[1], paliashdr->scale_origin[2]);
		glScalef (paliashdr->scale[0], paliashdr->scale[1], paliashdr->scale[2]);
	}

	anim = (int)(cl.time*10) & 3;
	GL_Bind(paliashdr->gl_texturenum[skinnum][anim]);

	// we can't dynamically colormap textures, so they are cached
	// seperately for the players.  Heads are just uncolored.
	if (e->colormap != vid.colormap && !gl_nocolors.value)
	{
		if (isclient)
		GL_Bind(playertextures - 1 + client_no);
	}

	if (gl_smoothmodels.value)
	glShadeModel (GL_SMOOTH);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	if (gl_affinemodels.value)
	glHint (GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);

	// fenix@io.com: model animation interpolation
	// Interpolating gun often has weird effects; default enabled but selectable
	if (r_interpolate_model_animation.value && (e != &cl.viewent || r_interpolate_model_weapon.value))
	R_SetupAliasBlendedFrame (e->frame, paliashdr, e);
	else
	R_SetupAliasFrame (e->frame, paliashdr);

	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	glShadeModel (GL_FLAT);
	if (gl_affinemodels.value)
	glHint (GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

	glPopMatrix ();
	
	if (model_alpha != 1.0)
	{
		glDisable (GL_BLEND);
		glColor4f (1,1,1,1);
	}

	//==================================================================================
	// hacked in from UNOFFICAL GLQUAKE code
	// muff@yakko.globalnet.co.uk
	// added lightning glow code  -  20 Mar 2000
	// added rocket tailglow code -  20 Mar 2000
	// updated to be pastable - 5 May 2000
	if((!strncmp (clmodel->name, "progs/flame.mdl",11)) || (!strncmp (clmodel->name, "progs/bolt",10)) || (!strcmp (clmodel->name, "progs/missile.mdl")) || (!strcmp (clmodel->name, "progs/quaddama.mdl")) || (!strcmp (clmodel->name, "progs/invulner.mdl")) || (!strcmp (clmodel->name, "progs/laser.mdl")))
	{
		// Draw torch flares. KH
		// NOTE: It would be better if we batched these up.
		//       All those state changes are not nice. KH
		// This relies on unchanged game code!
		const int TORCH_STYLE = 1; // Flicker.
		vec3_t	lightorigin;	// Origin of torch.
		vec3_t	v;				// Vector to torch.
		float	radius;			// Radius of torch flare.
		float	distance;		// Vector distance to torch.
		float	intensity;		// Intensity of torch flare.
		// NOTE: I don't think this is centered on the model.
		VectorCopy(currententity->origin, lightorigin);
		//set radius based on what model we are doing here
		if( (!strncmp (clmodel->name, "progs/flame.mdl",11))  ||
				(!strcmp (clmodel->name, "progs/missile.mdl")) )
		radius = 20.0f;
		else if( (!strncmp (clmodel->name, "progs/bolt",10)) ||
				(!strcmp (clmodel->name, "progs/laser.mdl")) )
		radius = 30.0f;
		/*else if( (!strcmp (clmodel->name, "progs/quaddama.mdl")) ||
				(!strcmp (clmodel->name, "progs/invulner.mdl"))
				)
		radius = 50.0f;*/
		VectorSubtract(lightorigin, r_origin, v);
		// See if view is outside the light.
		distance = Length(v);
		if (distance > radius) {
			glDepthMask (0);
			glDisable (GL_TEXTURE_2D);
			glShadeModel (GL_SMOOTH);
			glEnable (GL_BLEND);
			glBlendFunc (GL_ONE, GL_ONE);
			// Translate the glow to coincide with the flame. KH
			// or be at the tail of the missile - muff
			glPushMatrix();
			if (!strncmp (clmodel->name, "progs/flame.mdl",11))
			glTranslatef(0.0f, 0.0f, 8.0f);
			else if (!strcmp (clmodel->name, "progs/missile.mdl"))
			{
				glTranslatef(cos( e->angles[1]/180*M_PI)*(-20.0f), sin( e->angles[1]/180*M_PI)*(-20.0f), sin( e->angles[0]/180*M_PI)*(-20.0f));
			}
			else if( (!strcmp (clmodel->name, "progs/quaddama.mdl")) ||
					(!strcmp (clmodel->name, "progs/invulner.mdl"))
					)
			glTranslatef(0.0f, 0.0f, 20.0f);
			else if(!strcmp (clmodel->name, "progs/invulner.mdl"))
			glTranslatef(0.0f, 0.0f, 0.0f);
			glBegin(GL_TRIANGLE_FAN);
			// Invert (fades as you approach) - if we are a torch
			if (!strncmp (clmodel->name, "progs/flame.mdl",11))
			{
				// Diminish torch flare inversely with distance.
				intensity = (1024.0f - distance) / 1024.0f;
				intensity = (1.0f - intensity);
			}
			// or fix settings if lightning or missile
			else if( (!strncmp (clmodel->name, "progs/bolt",10)) ||
					(!strcmp (clmodel->name, "progs/laser.mdl")) )
			intensity = 0.2f;
			else if (!strcmp (clmodel->name, "progs/missile.mdl"))
			intensity = 0.5f;
			else if( (!strcmp (clmodel->name, "progs/quaddama.mdl")) ||
					(!strcmp (clmodel->name, "progs/invulner.mdl")) )
			intensity = 0.3f;
			// Clamp, but don't let the flare disappear.
			if (intensity > 1.0f) intensity = 1.0f;
			if (intensity < 0.0f) intensity = 0.0f;
			// Now modulate with flicker.
			i = (int)(cl.time*10);
			if (!cl_lightstyle[TORCH_STYLE].length) {
				j = 256;
			} else {
				j = i % cl_lightstyle[TORCH_STYLE].length;
				j = cl_lightstyle[TORCH_STYLE].map[j] - 'a';
				j = j*22;
			}
			intensity *= ((float)j / 255.0f);
			// Set yellow intensity
#if 0
			// Testing
			glColor3f(0.8f, 0.4f, 0.1f);
#else
			// set the colour of the glow - muff
			if ((!strncmp (clmodel->name, "progs/flame.mdl",11))  || (!strcmp (clmodel->name, "progs/missile.mdl"))  )
			glColor3f(0.8f*intensity, 0.4f*intensity, 0.1f);
			else if	(!strncmp (clmodel->name, "progs/bolt",10))
			glColor3f(0.2f*intensity, 0.2f*intensity, 0.8f*intensity);
			else if (!strcmp (clmodel->name, "progs/quaddama.mdl"))
			glColor3f(0.1f*intensity, 0.1f*intensity, 0.8f*intensity);
			else if (!strcmp (clmodel->name, "progs/invulner.mdl"))
			glColor3f(0.8f*intensity, 0.1f*intensity, 0.1f*intensity);
			else if (!strcmp (clmodel->name, "progs/laser.mdl"))
			glColor3f(0.8f*intensity, 0.4f*intensity, 0.1f*intensity);
#endif
			for (i=0 ; i<3 ; i++)
			v[i] = lightorigin[i] - vpn[i]*radius;
			glVertex3fv(v);
			glColor3f(0.0f, 0.0f, 0.0f);
			for (i=16; i>=0; i--) {
				float a = i/16.0f * M_PI*2;
				for (j=0; j<3; j++)
				v[j] =	lightorigin[j] +
				vright[j]*cos(a)*radius +
				vup[j]*sin(a)*radius;
				glVertex3fv(v);
			}
			glEnd();
			// Restore previous matrix! KH
			glPopMatrix();
			glColor3f (1,1,1);
			glDisable (GL_BLEND);
			glEnable (GL_TEXTURE_2D);
			glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask (1);
		}
	}
	
	// No shadow for selected models, if shadow would be above eye level or no lighting in map
	if (r_shadows.value && !clmodel->noshadow && e != &cl.viewent && r_refdef.vieworg[2] - lightspot[2] + 1 > 0 &&
			!r_fullbright.value && cl.worldmodel->lightdata)
	R_DrawShadow (e, paliashdr, client_no, mins);
}

//==================================================================================

/*
=============
R_DrawEntitiesOnList
=============
*/
void R_DrawEntitiesOnList (void)
{
	int i, j;

	if (!r_drawentities.value)
	return;

	// draw sprites seperately, because of alpha blending
	for (i=0 ; i<cl_numvisedicts ; i++)
	{
		if ((i + 1) % 100 == 0)
		S_ExtraUpdateTime ();	// don't let sound get messed up if going slow

		currententity = cl_visedicts[i];

		if (currententity == &cl_entities[cl.viewentity])
		{
			// chase_active.value not checked as player model is necessary for shadows
			currententity->angles[0] *= 0.3;
		}

		if (currententity->transparency != 1 && currententity->transparency != 0)
		{
			continue; // draw transparent entities last (?)
		}

		switch (currententity->model->type)
		{
		case mod_alias:
			R_DrawAliasModel (currententity, true);
			break;

		case mod_brush:
			R_DrawBrushModel (currententity);
			break;

		default:
			break;
		}
	}
}

// For rendering speed when many trans ents
typedef struct distent_s
{
	float	 dist;
	entity_t *ent;
} distent_t;

static int distcomp (const void *arg1, const void *arg2)
{
	return ((distent_t *)arg2)->dist - ((distent_t *)arg1)->dist;
}

/*
=============
R_DrawTransEntities
=============
*/
void R_DrawTransEntities (void)
{
	// need to draw back to front
	// fixme: this isn't my favorite option
	int	    i, j;
	distent_t   dent[MAX_VISEDICTS];
	vec3_t	    start, test;

	VectorCopy(r_refdef.vieworg, start);

	if (!r_drawentities.value)
	return;

	// Only do this once to save time
	for (i=j=0 ; i<cl_numvisedicts ; i++)
	{
		currententity = cl_visedicts[i];
		if (currententity->transparency == 1 || currententity->transparency == 0)
		continue;

		VectorCopy(currententity->origin, test);
		if (currententity->model->type == mod_brush)
		{
			test[0] += currententity->model->mins[0];
			test[1] += currententity->model->mins[1];
			test[2] += currententity->model->mins[2];
		}
		dent[j].dist = (((test[0] - start[0]) * (test[0] - start[0])) +
		((test[1] - start[1]) * (test[1] - start[1])) +
		((test[2] - start[2]) * (test[2] - start[2])));
		dent[j++].ent = currententity;
	}

	// Sort in descending dist order, i.e. back to front
	qsort (dent, j, sizeof(distent_t), distcomp);

	for (i = 0; i < j; ++i)
	{
		if ((i + 1) % 100 == 0)
		S_ExtraUpdateTime ();	// don't let sound get messed up if going slow

		currententity = dent[i].ent;

		switch (currententity->model->type)
		{
		case mod_alias:
			R_DrawAliasModel (currententity, true);
			break;
		case mod_brush:
			R_DrawBrushModel (currententity);
			break;
		default:
			break;
		}
	}
}

/*
=============
R_DrawSprites
=============
*/
void R_DrawSprites (void)
{
	int i;

	if (!r_drawentities.value)
	return;

	// draw sprites seperately, because of alpha blending
	for (i=0 ; i<cl_numvisedicts ; i++)
	{
		currententity = cl_visedicts[i];

		switch (currententity->model->type)
		{
		case mod_sprite:
			R_DrawSpriteModel (currententity);
			break;
		}
	}
}

/*
=============
R_DrawViewModel
=============
*/
void R_DrawViewModel (void)
{
	float	    save;
	float	    ambient[4], diffuse[4];
	int	    j;
	int	    lnum;
	vec3_t	    dist;
	float	    add;
	dlight_t    *dl;
	int	    ambientlight, shadelight;

	if (!r_drawviewmodel.value)
	return;

	if (chase_active.value)
	return;

	if (envmap)
	return;

	if (!r_drawentities.value)
	return;

	if (cl.items & IT_INVISIBILITY)
	return;

	if (cl.stats[STAT_HEALTH] <= 0)
	return;

	currententity = &cl.viewent;
	if (!currententity->model)
	return;

	// Prevent weapon model error
	if (currententity->model->name[0] == '*')
	{
		Con_Printf ("\x02R_DrawViewModel: ");
		Con_Printf ("viewmodel %s invalid\n", currententity->model->name);
		Cvar_Set ("r_drawviewmodel", "0");
		return;
	}

	j = R_LightPoint (currententity->origin);

	if (j < 24)
	j = 24;		// allways give some light on gun
	ambientlight = j;
	shadelight = j;

	// add dynamic lights
	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		dl = &cl_dlights[lnum];
		if (!dl->radius)
		continue;
		if (!dl->radius)
		continue;
		if (dl->die < cl.time)
		continue;

		VectorSubtract (currententity->origin, dl->origin, dist);
		add = dl->radius - Length(dist);
		if (add > 0)
		ambientlight += add;
	}

	ambient[0] = ambient[1] = ambient[2] = ambient[3] = (float)ambientlight / 128;
	diffuse[0] = diffuse[1] = diffuse[2] = diffuse[3] = (float)shadelight / 128;

	currententity->transparency = model_alpha = cl_entities[cl.viewentity].transparency; // LordHavoc: if the player is transparent, so is his gun
	currententity->effects = cl_entities[cl.viewentity].effects;

	// hack the depth range to prevent view model from poking into walls
	glDepthRange (gldepthmin, gldepthmin + 0.3*(gldepthmax-gldepthmin));
	
	// fenix@io.com: model transform interpolation
	save = r_interpolate_model_transform.value;
	Cvar_SetValue ("r_interpolate_model_transform", 0);
	R_DrawAliasModel (currententity, false);
	Cvar_SetValue ("r_interpolate_model_transform", save);
	
	glDepthRange (gldepthmin, gldepthmax);
}


/*
============
R_PolyBlend
============
*/
void R_PolyBlend (void)
{
	if (!gl_polyblend.value)
	return;
	if (!v_blend[3])
	return;

	GL_DisableMultitexture();

	glDisable (GL_ALPHA_TEST);
	glEnable (GL_BLEND);
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_TEXTURE_2D);

	glLoadIdentity ();

	glRotatef (-90,  1, 0, 0);	    // put Z going up
	glRotatef (90,  0, 0, 1);	    // put Z going up

	glColor4fv (v_blend);

	glBegin (GL_QUADS);

	glVertex3f (10, 100, 100);
	glVertex3f (10, -100, 100);
	glVertex3f (10, -100, -100);
	glVertex3f (10, 100, -100);
	glEnd ();

	glDisable (GL_BLEND);
	glEnable (GL_TEXTURE_2D);
	glEnable (GL_ALPHA_TEST);
}


int SignbitsForPlane (mplane_t *out)
{
	int	bits, j;

	// for fast box on planeside test

	bits = 0;
	for (j=0 ; j<3 ; j++)
	{
		if (out->normal[j] < 0)
		bits |= 1<<j;
	}
	return bits;
}

/*
===============
TurnVector

turn forward towards side on the plane defined by forward and side
if angle = 90, the result will be equal to side
assumes side and forward are perpendicular, and normalized
to turn away from side, use a negative angle
===============
*/
void TurnVector (vec3_t out, vec3_t forward, vec3_t side, float angle)
{
	float scale_forward, scale_side;

	scale_forward = cos (DEG2RAD(angle));
	scale_side = sin (DEG2RAD(angle));

	out[0] = scale_forward*forward[0] + scale_side*side[0];
	out[1] = scale_forward*forward[1] + scale_side*side[1];
	out[2] = scale_forward*forward[2] + scale_side*side[2];
}

/*
===============
R_SetFrustum
===============
*/
void R_SetFrustum (void)
{
	int i;

	TurnVector (frustum[0].normal, vpn, vright, r_refdef.fov_x/2 - 90); //left plane
	TurnVector (frustum[1].normal, vpn, vright, 90 - r_refdef.fov_x/2); //right plane
	TurnVector (frustum[2].normal, vpn, vup, 90 - r_refdef.fov_y/2); //bottom plane
	TurnVector (frustum[3].normal, vpn, vup, r_refdef.fov_y/2 - 90); //top plane

	for (i=0 ; i<4 ; i++)
	{
		frustum[i].type = PLANE_ANYZ;
		frustum[i].dist = DotProduct (r_origin, frustum[i].normal);
		frustum[i].signbits = SignbitsForPlane (&frustum[i]);
	}
}

/*
===============
R_CheckVariables
===============
*/
void R_CheckVariables (void)
{
	static float oldbright = 0, oldclear = 0, oldgamma = 0;

	if (r_fullbright.value != oldbright)
	{
		oldbright = r_fullbright.value;

		// Refresh view
		GL_RefreshLightMaps ();
	}

	if (r_clearcolor.value != oldclear)
	{
		byte *rgb;

		oldclear = r_clearcolor.value;

		// Refresh view
		rgb = (byte *)(d_8to24table + ((int)r_clearcolor.value & 0xFF));
		glClearColor (rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0, 0);
	}

	if (v_gamma.value != oldgamma)
	{
		oldgamma = v_gamma.value;

		// Refresh view
		VID_Gamma_f ();
	}
}

/*
===============
R_SetupFrame
===============
*/
void R_SetupFrame (void)
{
	int				edgecount;
	vrect_t			vrect;
	float			w, h;

	// don't allow cheats in multiplayer
	if (cl.maxclients > 1)
	Cvar_Set ("r_fullbright", "0");

	R_CheckVariables ();

	R_AnimateLight ();

	r_framecount++;

	// build the transformation matrix for the given view angles
	VectorCopy (r_refdef.vieworg, r_origin);

	AngleVectors (r_refdef.viewangles, vpn, vright, vup);

	// current viewleaf
	r_oldviewleaf = r_viewleaf;
	r_viewleaf = Mod_PointInLeaf (r_origin, cl.worldmodel);

	V_SetContentsColor (r_viewleaf->contents);
	V_CalcBlend ();

	r_cache_thrash = false;

	c_brush_polys = 0;
	c_alias_polys = 0;

}


void MYgluPerspective( GLdouble fovy, GLdouble aspect,
GLdouble zNear, GLdouble zFar )
{
	GLdouble xmin, xmax, ymin, ymax;

	ymax = zNear * tan( fovy * M_PI / 360.0 );
	ymin = -ymax;

	xmin = ymin * aspect;
	xmax = ymax * aspect;

	glFrustum( xmin, xmax, ymin, ymax, zNear, zFar );
}


/*
=============
R_SetupGL
=============
*/
void R_SetupGL (void)
{
	float	screenaspect;
	float	yfov;
	int		i;
	extern	int glwidth, glheight;
	int		x, x2, y2, y, w, h;

	//
	// set up viewpoint
	//
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity ();
	x = r_refdef.vrect.x * glwidth/vid.width;
	x2 = (r_refdef.vrect.x + r_refdef.vrect.width) * glwidth/vid.width;
	y = (vid.height-r_refdef.vrect.y) * glheight/vid.height;
	y2 = (vid.height - (r_refdef.vrect.y + r_refdef.vrect.height)) * glheight/vid.height;

	// fudge around because of frac screen scale
	if (x > 0)
	x--;
	if (x2 < glwidth)
	x2++;
	if (y2 < 0)
	y2--;
	if (y < glheight)
	y++;

	w = x2 - x;
	h = y - y2;

	if (envmap)
	{
		x = y2 = 0;
		w = h = 256;
	}

	glViewport (glx + x, gly + y2, w, h);
	screenaspect = (float)r_refdef.vrect.width/r_refdef.vrect.height;
	//	yfov = 2*atan((float)r_refdef.vrect.height/r_refdef.vrect.width)*180/M_PI;
	MYgluPerspective (r_refdef.fov_y,  screenaspect,  4,  GL_FARCLIP); // 4096

	if (mirror)
	{
		if (mirror_plane->normal[2])
		glScalef (1, -1, 1);
		else
		glScalef (-1, 1, 1);
		glCullFace(GL_BACK);
	}
	else
	glCullFace(GL_FRONT);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity ();

	glRotatef (-90,  1, 0, 0);	    // put Z going up
	glRotatef (90,  0, 0, 1);	    // put Z going up
	glRotatef (-r_refdef.viewangles[2],  1, 0, 0);
	glRotatef (-r_refdef.viewangles[0],  0, 1, 0);
	glRotatef (-r_refdef.viewangles[1],  0, 0, 1);
	glTranslatef (-r_refdef.vieworg[0],  -r_refdef.vieworg[1],  -r_refdef.vieworg[2]);

	glGetFloatv (GL_MODELVIEW_MATRIX, r_world_matrix);

	//
	// set drawing parms
	//
	if (gl_cull.value)
	glEnable(GL_CULL_FACE);
	else
	glDisable(GL_CULL_FACE);

	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glEnable(GL_DEPTH_TEST);
}

/*
================
R_RenderScene

r_refdef must be set before the first call
================
*/
void R_RenderScene (void)
{
	S_ExtraUpdateTime ();	// don't let sound get messed up if going slow

	R_SetupFrame ();

	R_SetFrustum ();

	R_SetupGL ();

	R_MarkLeaves ();	// done here so we know if we're in water

	R_DrawWorld ();		// adds static entities to the list

	S_ExtraUpdateTime ();	// don't let sound get messed up if going slow

	R_DrawEntitiesOnList ();

	GL_DisableMultitexture();

	R_RenderDlights ();

	S_ExtraUpdateTime ();	// don't let sound get messed up if going slow

#ifdef GLTEST
	Test_Draw ();
#endif

}


/*
=============
R_Clear
=============
*/
void R_Clear (void)
{
	if (r_mirroralpha.value != 1.0)
	{
		if (gl_clear.value)
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		else
		glClear (GL_DEPTH_BUFFER_BIT);
		gldepthmin = 0;
		gldepthmax = 0.5;
		glDepthFunc (GL_LEQUAL);
	}
	else if (gl_ztrick.value)
	{
		static int trickframe;

		if (gl_clear.value)
		glClear (GL_COLOR_BUFFER_BIT);

		trickframe++;
		if (trickframe & 1)
		{
			gldepthmin = 0;
			gldepthmax = 0.49999;
			glDepthFunc (GL_LEQUAL);
		}
		else
		{
			gldepthmin = 1;
			gldepthmax = 0.5;
			glDepthFunc (GL_GEQUAL);
		}
	}
	else
	{
		if (gl_clear.value)
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		else
		glClear (GL_DEPTH_BUFFER_BIT);
		gldepthmin = 0;
		gldepthmax = 1;
		glDepthFunc (GL_LEQUAL);
	}

	glDepthRange (gldepthmin, gldepthmax);
}

/*
=============
R_Mirror
=============
*/
void R_Mirror (void)
{
	float		d;
	msurface_t	*s;
	entity_t	*ent;

	if (!mirror)
	return;

	memcpy (r_base_world_matrix, r_world_matrix, sizeof(r_base_world_matrix));

	d = DotProduct (r_refdef.vieworg, mirror_plane->normal) - mirror_plane->dist;
	VectorMA (r_refdef.vieworg, -2*d, mirror_plane->normal, r_refdef.vieworg);

	d = DotProduct (vpn, mirror_plane->normal);
	VectorMA (vpn, -2*d, mirror_plane->normal, vpn);

	r_refdef.viewangles[0] = -asin (vpn[2])/M_PI*180;
	r_refdef.viewangles[1] = atan2 (vpn[1], vpn[0])/M_PI*180;
	r_refdef.viewangles[2] = -r_refdef.viewangles[2];

	ent = &cl_entities[cl.viewentity];
	if (cl_numvisedicts < MAX_VISEDICTS)
	{
		cl_visedicts[cl_numvisedicts] = ent;
		cl_numvisedicts++;
	}

	gldepthmin = 0.5;
	gldepthmax = 1;
	glDepthRange (gldepthmin, gldepthmax);
	glDepthFunc (GL_LEQUAL);

	R_RenderScene ();
	R_DrawWaterSurfaces ();

	gldepthmin = 0;
	gldepthmax = 0.5;
	glDepthRange (gldepthmin, gldepthmax);
	glDepthFunc (GL_LEQUAL);

	// blend on top
	glEnable (GL_BLEND);
	glMatrixMode(GL_PROJECTION);
	if (mirror_plane->normal[2])
	glScalef (1,-1,1);
	else
	glScalef (-1,1,1);
	glCullFace(GL_FRONT);
	glMatrixMode(GL_MODELVIEW);

	glLoadMatrixf (r_base_world_matrix);

	glColor4f (1,1,1,r_mirroralpha.value);
	s = cl.worldmodel->textures[mirrortexturenum]->texturechain;
	for ( ; s ; s=s->texturechain)
	R_RenderBrushPoly (s);
	cl.worldmodel->textures[mirrortexturenum]->texturechain = NULL;
	glDisable (GL_BLEND);
	glColor4f (1,1,1,1);
}

/*
================
R_RenderView

r_refdef must be set before the first call
================
*/
void R_RenderView(void)
{
	double	time1, time2;
	// Original
	//GLfloat	colors[4] = {(GLfloat) 0.0, (GLfloat) 0.0, (GLfloat) 1, (GLfloat) 0.20};
	//ON
	//GLfloat	colors[4] = {(GLfloat) 0.8, (GLfloat) 0.8, (GLfloat) 0, (GLfloat) 0.8};
	//OFF
	//GLfloat	colors[4] = {(GLfloat) 0.0, (GLfloat) 0.0, (GLfloat) 0, (GLfloat) 1.0};


	/*if (level_t()) // TODO: Need to add a checking for game (id1 or no) function
	{
		if (COM_CheckParm("-fog"))
		{
			if (strcpy("e1m1", level_t[0].name) // TODO: Need to fix this shit -_-
			{
				GLfloat	colors[4] = { (GLfloat) 0.8, (GLfloat) 0.8, (GLfloat) 0.8, (GLfloat) 0.8 };
			}
			else if (strcpy(levels[0].name, "e2m1")
			{
				GLfloat	colors[4] = { (GLfloat) 0.0, (GLfloat) 0.0, (GLfloat) 0, (GLfloat) 1.0 };
			}
		}
	}*/

	//GLfloat	colors[4] = { (GLfloat) 0.8, (GLfloat) 0.8, (GLfloat) 0.8, (GLfloat) 0.8 };
	GLfloat	colors[4] = { (GLfloat) 0.0, (GLfloat) 0.0, (GLfloat) 0, (GLfloat) 1.0 };

	if (r_norefresh.value)
	return;

	if (!r_worldentity.model || !cl.worldmodel)
	Sys_Error ("R_RenderView: NULL worldmodel");

	if (r_speeds.value)
	{
		glFinish ();
		time1 = Sys_FloatTime ();
		c_brush_polys = 0;
		c_alias_polys = 0;
	}

	mirror = false;

	if (gl_finish.value)
	glFinish ();

	R_Clear ();

	// render normal view

	///***** Experimental silly looking fog ******
//****** Use r_fullbright if you enable ******
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogfv(GL_FOG_COLOR, colors);
	glFogf(GL_FOG_END, 5120.0); // 512.0
	glEnable(GL_FOG);
//********************************************/

	R_Fog_BeginFrame();
	R_RenderScene ();
	R_DrawViewModel ();
	R_DrawWaterSurfaces ();
	R_DrawSprites ();

	R_DrawTransEntities();

	R_DrawParticles ();

	R_Fog_EndFrame();

	//  More fog right here :)
		glDisable(GL_FOG);
	//  End of all fog code...

	// render mirror view
	R_Mirror ();

	R_PolyBlend ();

	if (r_speeds.value)
	{
		//		glFinish ();
		time2 = Sys_FloatTime ();
		Con_Printf ("%3i ms  %4i wpoly %4i epoly\n", (int)((time2-time1)*1000), c_brush_polys, c_alias_polys);
	}
}

/*
=============
GL_DrawAliasBlendedFrame

fenix@io.com: model animation interpolation
=============
*/
void GL_DrawAliasBlendedFrame (aliashdr_t *paliashdr, int pose1, int pose2, float blend)
{
	float       l;
	trivertx_t  *verts1;
	trivertx_t  *verts2;
	int	    *order;
	int         count;
	vec3_t      d;

	if (model_alpha != 1.0)
	glEnable (GL_BLEND);

	lastposenum0 = pose1;
	lastposenum  = pose2;
	verts1  = (trivertx_t *)((byte *)paliashdr + paliashdr->posedata);
	verts2  = verts1;

	verts1 += pose1 * paliashdr->poseverts;
	verts2 += pose2 * paliashdr->poseverts;

	order = (int *)((byte *)paliashdr + paliashdr->commands);

	for (;;)
	{
		// get the vertex count and primitive type
		count = *order++;

		if (!count) break;

		if (count < 0)
		{
			count = -count;
			glBegin (GL_TRIANGLE_FAN);
		}
		else
		glBegin (GL_TRIANGLE_STRIP);

		do
		{
			// texture coordinates come from the draw list
			glTexCoord2f (((float *)order)[0], ((float *)order)[1]);
			order += 2;

			// normals and vertexes come from the frame list
			// blend the light intensity from the two frames together
			d[0] = shadedots[verts2->lightnormalindex] -
			shadedots[verts1->lightnormalindex];

			if (r_fullbright.value || !cl.worldmodel->lightdata)
			l = 1;
			else
			l = shadelight * (shadedots[verts1->lightnormalindex] + (blend * d[0]));

			if (model_alpha != 1.0)
			glColor4f (l, l, l, model_alpha);
			else
			glColor3f (l, l, l);

			VectorSubtract(verts2->v, verts1->v, d);

			// blend the vertex positions from each frame together
			glVertex3f (verts1->v[0] + (blend * d[0]),
			verts1->v[1] + (blend * d[1]),
			verts1->v[2] + (blend * d[2]));

			verts1++;
			verts2++;
		}
		while (--count);

		glEnd ();
	}
}

static float TimePassed (float lasttime)
{
	float timepassed = cl.time - lasttime;

	if (timepassed < 0)
	{
		if (timepassed > -0.01)
		timepassed = 0; // This happens in demos
		//		else
		//		{
		//			Con_SafePrintf ("\002TimePassed: ");
		//			Con_SafePrintf ("timepassed = %f\n", timepassed);
		//		}
	}
	
	return timepassed;
}

/*
=================
R_SetupAliasBlendedFrame

fenix@io.com: model animation interpolation
=================
*/
void R_SetupAliasBlendedFrame (int frame, aliashdr_t *paliashdr, entity_t* e)
{
	int	     pose;
	int	     numposes;
	float	     blend, timepassed;
	static float lastmsg = 0;

	frame = R_ChkFrame ("R_SetupAliasBlendedFrame", frame, paliashdr, &lastmsg);

	pose = paliashdr->frames[frame].firstpose;
	numposes = paliashdr->frames[frame].numposes;

	if (numposes > 1)
	{
		int firstpose = pose;

		pose = numposes * currententity->syncbase; // Hack to make flames unsynchronized
		e->frame_interval = paliashdr->frames[frame].interval;
		pose += (int)(cl.time / e->frame_interval) % numposes;
		pose = firstpose + pose % numposes;
	}
	else
	{
		/* One tenth of a second is a good for most Quake animations.
		If the nextthink is longer then the animation is usually meant to pause
		(e.g. check out the shambler magic animation in shambler.qc).  If its
		shorter then things will still be smoothed partly, and the jumps will be
		less noticable because of the shorter time.  So, this is probably a good
		assumption. */
		e->frame_interval = 0.1;
	}

	timepassed = TimePassed (e->frame_start_time);

	if (e->pose2 != pose || timepassed < 0)
	{
		e->frame_start_time = cl.time;
		e->pose1 = e->pose2;
		e->pose2 = pose;
		blend = 0;
	}
	else
	blend = timepassed / e->frame_interval;

	// wierd things start happening if blend passes 1
	if (cl.paused || blend > 1)
	blend = 1;

	/*	if (e->pose1 >= paliashdr->numposes || e->pose2 >= paliashdr->numposes)
	{
		Con_SafePrintf ("\002R_SetupAliasBlendedFrame: ");
		Con_SafePrintf ("pose1 (%d) or pose2 (%d) is >= numposes (%d) in %s\n", e->pose1, e->pose2, paliashdr->numposes, e->model->name);
	}*/

	// Prevent crash later in GL_DrawAliasBlendedFrame, probably
	// caused by entity morphing e.g. from monster to gibbed head
	if (e->pose1 >= paliashdr->numposes)
	e->pose1 = 0; 
	
	if (e->pose2 >= paliashdr->numposes)
	e->pose2 = 0; // I haven't seen this happen, but just in case ...
	
	GL_DrawAliasBlendedFrame (paliashdr, e->pose1, e->pose2, blend);
}

/*
=============
R_BlendedRotateForEntity

fenix@io.com: model transform interpolation
=============
*/
void R_BlendedRotateForEntity (entity_t *e, qboolean shadow)
{
	float 	timepassed;
	float 	blend;
	vec3_t	d;
	int	i;

	// positional interpolation

	timepassed = TimePassed (e->translate_start_time);

	if (e->translate_start_time == 0 || timepassed > 1 || timepassed < 0)
	{
		timepassed = 0;
		e->translate_start_time = cl.time;
		VectorCopy (e->origin, e->origin1);
		VectorCopy (e->origin, e->origin2);
	}

	if (!VectorCompare (e->origin, e->origin2))
	{
		e->translate_start_time = cl.time;
		VectorCopy (e->origin2, e->origin1);
		VectorCopy (e->origin,  e->origin2);
		blend = 0;
	}
	else
	{
		blend =  timepassed / 0.1;

		if (cl.paused || blend > 1)
		blend = 1;
	}

	VectorSubtract (e->origin2, e->origin1, d);

	glTranslatef (e->origin1[0] + (blend * d[0]),
	e->origin1[1] + (blend * d[1]),
	e->origin1[2] + (blend * d[2]));

	// orientation interpolation (Euler angles, yuck!)

	timepassed = TimePassed (e->rotate_start_time);

	if (e->rotate_start_time == 0 || timepassed > 1 || timepassed < 0)
	{
		timepassed = 0;
		e->rotate_start_time = cl.time;
		VectorCopy (e->angles, e->angles1);
		VectorCopy (e->angles, e->angles2);
	}

	if (!VectorCompare (e->angles, e->angles2))
	{
		e->rotate_start_time = cl.time;
		VectorCopy (e->angles2, e->angles1);
		VectorCopy (e->angles,  e->angles2);
		blend = 0;
	}
	else
	{
		blend =  timepassed / 0.1;

		if (cl.paused || blend > 1)
		blend = 1;
	}

	VectorSubtract (e->angles2, e->angles1, d);

	// always interpolate along the shortest path
	for (i = 0; i < 3; i++)
	{
		if (d[i] > 180)
		d[i] -= 360;
		else if (d[i] < -180)
		d[i] += 360;
	}

	glRotatef ( rot_angle = e->angles1[1] + ( blend * d[1]),  0, 0, 1);

	if (shadow)
	return;

	glRotatef (-e->angles1[0] + (-blend * d[0]),  0, 1, 0);
	glRotatef ( e->angles1[2] + ( blend * d[2]),  1, 0, 0);
}

/*
=============
GL_DrawAliasBlendedShadow

fenix@io.com: model animation interpolation
=============
*/
void GL_DrawAliasBlendedShadow (aliashdr_t *paliashdr, int pose1, int pose2, entity_t* e)
{
	trivertx_t* verts1;
	trivertx_t* verts2;
	int*        order;
	vec3_t      point1;
	vec3_t      point2;
	vec3_t      d;
	float       height;
	float       lheight;
	int         count;
	float       blend;

	// Negative intervals already fixed in R_SetupAliasBlendedFrame
	blend = (cl.time - e->frame_start_time) / e->frame_interval;

	if (blend > 1)
	blend = 1;

	lheight = e->origin[2] - lightspot[2];
	height  = -lheight + 1.0;

	verts1 = (trivertx_t *)((byte *)paliashdr + paliashdr->posedata);
	verts2 = verts1;

	verts1 += pose1 * paliashdr->poseverts;
	verts2 += pose2 * paliashdr->poseverts;

	order = (int *)((byte *)paliashdr + paliashdr->commands);

	for (;;)
	{
		// get the vertex count and primitive type
		count = *order++;

		if (!count)
		break;

		if (count < 0)
		{
			count = -count;
			glBegin (GL_TRIANGLE_FAN);
		}
		else
		glBegin (GL_TRIANGLE_STRIP);

		do
		{
			order += 2;

			point1[0] = verts1->v[0] * paliashdr->scale[0] + paliashdr->scale_origin[0];
			point1[1] = verts1->v[1] * paliashdr->scale[1] + paliashdr->scale_origin[1];
			point1[2] = verts1->v[2] * paliashdr->scale[2] + paliashdr->scale_origin[2];

			point1[0] -= shadevector[0]*(point1[2]+lheight);
			point1[1] -= shadevector[1]*(point1[2]+lheight);

			point2[0] = verts2->v[0] * paliashdr->scale[0] + paliashdr->scale_origin[0];
			point2[1] = verts2->v[1] * paliashdr->scale[1] + paliashdr->scale_origin[1];
			point2[2] = verts2->v[2] * paliashdr->scale[2] + paliashdr->scale_origin[2];

			point2[0] -= shadevector[0]*(point2[2]+lheight);
			point2[1] -= shadevector[1]*(point2[2]+lheight);

			VectorSubtract(point2, point1, d);

			glVertex3f (point1[0] + (blend * d[0]),
			point1[1] + (blend * d[1]),
			height);

			verts1++;
			verts2++;
		} while (--count);

		glEnd ();
	}
}
