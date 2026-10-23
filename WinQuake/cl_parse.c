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
#include "localization.h"

char *svc_strings[] =
{
	"svc_bad",
	"svc_nop",
	"svc_disconnect",
	"svc_updatestat",
	"svc_version",		// [long] server version
	"svc_setview",		// [short] entity number
	"svc_sound",			// <see code>
	"svc_time",			// [float] server time
	"svc_print",			// [string] null terminated string
	"svc_stufftext",		// [string] stuffed into client's console buffer
						// the string should be \n terminated
	"svc_setangle",		// [vec3] set the view angle to this absolute value
	
	"svc_serverinfo",		// [long] version
						// [string] signon string
						// [string]..[0]model cache [string]...[0]sounds cache
						// [string]..[0]item cache
	"svc_lightstyle",		// [byte] [string]
	"svc_updatename",		// [byte] [string]
	"svc_updatefrags",	// [byte] [short]
	"svc_clientdata",		// <shortbits + data>
	"svc_stopsound",		// <see code>
	"svc_updatecolors",	// [byte] [byte]
	"svc_particle",		// [vec3] <variable>
	"svc_damage",			// [byte] impact [byte] blood [vec3] from
	
	"svc_spawnstatic",
	"OBSOLETE svc_spawnbinary",
	"svc_spawnbaseline",
	
	"svc_temp_entity",		// <variable>
	"svc_setpause",
	"svc_signonnum",
	"svc_centerprint",
	"svc_killedmonster",
	"svc_foundsecret",
	"svc_spawnstaticsound",
	"svc_intermission",
	"svc_finale",			// [string] music [string] text
	"svc_cdtrack",			// [byte] track [byte] looptrack
	"svc_sellscreen",
	"svc_cutscene",
	"", "",					// 35, 36
	"svc_skybox",			// 37: [string] name (protocols 666/999)
	"", "",					// 38, 39
	"svc_bf",				// 40
	"svc_fog",				// 41
	"svc_spawnbaseline2",	// 42
	"svc_spawnstatic2",		// 43
	"svc_spawnstaticsound2"	// 44
};

// LuminousQuake: protocols 666 and 999 add fields to the messages below.
static qboolean CL_ExtendedProtocol (void)
{
	return cl.protocol == PROTOCOL_FITZQUAKE || cl.protocol == PROTOCOL_RMQ;
}

//=============================================================================

/*
===============
CL_EntityNum

This error checks and tracks the total number of entities
===============
*/
entity_t	*CL_EntityNum (int num)
{
	if (num >= cl.num_entities)
	{
		if (num >= MAX_EDICTS)
			Host_Error ("CL_EntityNum: %i is an invalid number",num);
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
CL_ParseStartSoundPacket
==================
*/
void CL_ParseStartSoundPacket(void)
{
    vec3_t  pos;
    int 	channel, ent;
    int 	sound_num;
    int 	volume;
    int 	field_mask;
    float 	attenuation;  
 	int		i;
	           
    field_mask = MSG_ReadByte(); 

    if (field_mask & SND_VOLUME)
		volume = MSG_ReadByte ();
	else
		volume = DEFAULT_SOUND_PACKET_VOLUME;
	
    if (field_mask & SND_ATTENUATION)
		attenuation = MSG_ReadByte () / 64.0;
	else
		attenuation = DEFAULT_SOUND_PACKET_ATTENUATION;
	
	if (field_mask & SND_LARGEENTITY)
	{
		ent = (unsigned short)MSG_ReadShort ();
		channel = MSG_ReadByte ();
	}
	else
	{
		channel = (unsigned short)MSG_ReadShort ();
		ent = channel >> 3;
		channel &= 7;
	}
	if (field_mask & SND_LARGESOUND)
		sound_num = (unsigned short)MSG_ReadShort ();
	else
		sound_num = MSG_ReadByte ();

	if (ent >= MAX_EDICTS)
		Host_Error ("CL_ParseStartSoundPacket: ent = %i", ent);
	if (sound_num >= MAX_SOUNDS)
		Host_Error ("CL_ParseStartSoundPacket: sound = %i", sound_num);
	
	for (i=0 ; i<3 ; i++)
		pos[i] = MSG_ReadCoord (cl.protocolflags);
 
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
	float	time;
	static float lastmsg;
	int		ret;
	sizebuf_t	old;
	static byte	olddata[NET_MAXMESSAGE];
	
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
			Host_Error ("CL_KeepaliveMessage: CL_GetMessage failed");		
		case 0:
			break;	// nothing waiting
		case 1:
			Host_Error ("CL_KeepaliveMessage: received a message");
			break;
		case 2:
			if (MSG_ReadByte() != svc_nop)
				Host_Error ("CL_KeepaliveMessage: datagram wasn't a nop");
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
CL_ParseWorldspawn

LuminousQuake: the FitzQuake worldspawn keys "fog" (density red green blue)
and "sky" (a gfx/env skybox name); svc_fog and svc_skybox override them.
==================
*/
static void CL_FogSettle (void);
static void CL_ParseWorldspawn (void)
{
	char	key[128], *data;

	memset (&cl_fog, 0, sizeof(cl_fog));
	if (!cl.worldmodel || !cl.worldmodel->entities)
		return;
	data = COM_Parse (cl.worldmodel->entities);
	if (!data || com_token[0] != '{')
		return;
	while (1)
	{
		data = COM_Parse (data);
		if (!data || com_token[0] == '}')
			return;
		Q_strncpy (key, com_token[0] == '_' ? com_token + 1 : com_token, sizeof(key) - 1);
		key[sizeof(key) - 1] = 0;
		data = COM_Parse (data);
		if (!data)
			return;
		if (!Q_strcmp (key, "fog"))
		{
			float v[4] = {0, 0, 0, 0};
			sscanf (com_token, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
			cl_fog.density = v[0] < 0 ? 0 : v[0];
			cl_fog.color[0] = v[1]; cl_fog.color[1] = v[2]; cl_fog.color[2] = v[3];
			CL_FogSettle ();
		}
		else if (!Q_strcmp (key, "sky") || !Q_strcmp (key, "skyname"))
		{
			Q_strncpy (cl.skyboxname, com_token, sizeof(cl.skyboxname) - 1);
			cl.skyboxname[sizeof(cl.skyboxname) - 1] = 0;
		}
	}
}

/*
==================
CL_ParseServerInfo
==================
*/
void CL_ParseServerInfo (void)
{
	char	*str;
	int		i;
	int		nummodels, numsounds;
	static char	model_precache[MAX_MODELS][MAX_QPATH];
	static char	sound_precache[MAX_SOUNDS][MAX_QPATH];
	
	Con_DPrintf ("Serverinfo packet received.\n");
//
// wipe the client_state_t struct
//
	CL_ClearState ();

// parse protocol version number
	i = MSG_ReadLong ();
	if (i != PROTOCOL_NETQUAKE && i != PROTOCOL_FITZQUAKE && i != PROTOCOL_RMQ)
	{
		Con_Printf ("Server returned version %i, not %i, %i or %i\n", i,
			PROTOCOL_NETQUAKE, PROTOCOL_FITZQUAKE, PROTOCOL_RMQ);
		return;
	}
	cl.protocol = i;
	cl.protocolflags = 0;
	if (cl.protocol == PROTOCOL_RMQ)
	{
		const unsigned int supported = PRFL_SHORTANGLE | PRFL_FLOATANGLE | PRFL_24BITCOORD
			| PRFL_FLOATCOORD | PRFL_EDICTSCALE | PRFL_ALPHASANITY | PRFL_INT32COORD;
		cl.protocolflags = (unsigned int)MSG_ReadLong ();
		if (cl.protocolflags & ~supported)
			Con_Printf ("PROTOCOL_RMQ protocolflags %i contains unknown flags\n", cl.protocolflags);
	}

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
	Con_Printf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n\n");
	Con_Printf ("%c%s\n", 2, str);

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
		{
			Con_Printf ("Server sent too many model precaches\n");
			return;
		}
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
		{
			Con_Printf ("Server sent too many sound precaches\n");
			return;
		}
		strcpy (sound_precache[numsounds], str);
		S_TouchSound (str);
	}

//
// now we try to load everything else until a cache allocation fails
//

	for (i=1 ; i<nummodels ; i++)
	{
		cl.model_precache[i] = Mod_ForName (model_precache[i], false);
		if (cl.model_precache[i] == NULL)
		{
			Con_Printf("Model %s not found\n", model_precache[i]);
			return;
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
	CL_ParseWorldspawn ();
	
	R_NewMap ();

	Hunk_Check ();		// make sure nothing is hurt
	
	noclip_anglehack = false;		// noclip is turned off at start	
}


/*
==================
CL_ParseUpdate

Parse an entity update message from the server
If an entities model or origin changes from frame to frame, it must be
relinked.  Other attributes can change without relinking.
==================
*/
int	bitcounts[32];

void CL_ParseUpdate (int bits)
{
	int			i;
	model_t		*model;
	int			modnum;
	qboolean	forcelink;
	entity_t	*ent;
	int			num;
	int			skin;

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
	if (CL_ExtendedProtocol ())
	{
		if (bits & U_EXTEND1)
			bits |= MSG_ReadByte () << 16;
		if (bits & U_EXTEND2)
			bits |= MSG_ReadByte () << 24;
	}

	if (bits & U_LONGENTITY)	
		num = MSG_ReadShort ();
	else
		num = MSG_ReadByte ();

	ent = CL_EntityNum (num);

for (i=0 ; i<32 ; i++)
if (bits&(1u<<i))
	bitcounts[i]++;

	if (ent->msgtime != cl.mtime[1])
		forcelink = true;	// no previous frame to lerp from
	else
		forcelink = false;

	ent->msgtime = cl.mtime[0];
	
	if (bits & U_MODEL)
		modnum = MSG_ReadByte ();
	else
		modnum = ent->baseline.modelindex;
	
	if (bits & U_FRAME)
		ent->frame = MSG_ReadByte ();
	else
		ent->frame = ent->baseline.frame;

	// LuminousQuake: colormap, skin, effects and the coordinates follow;
	// the 666/999 high bytes of model and frame come after those, so the
	// model is resolved below once they are read.

	if (bits & U_COLORMAP)
		i = MSG_ReadByte();
	else
		i = ent->baseline.colormap;
	if (!i)
		ent->colormap = vid.colormap;
	else
	{
		if (i > cl.maxclients)
			Sys_Error ("i >= cl.maxclients");
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
		ent->msg_origins[0][0] = MSG_ReadCoord (cl.protocolflags);
	else
		ent->msg_origins[0][0] = ent->baseline.origin[0];
	if (bits & U_ANGLE1)
		ent->msg_angles[0][0] = MSG_ReadAngle (cl.protocolflags);
	else
		ent->msg_angles[0][0] = ent->baseline.angles[0];

	if (bits & U_ORIGIN2)
		ent->msg_origins[0][1] = MSG_ReadCoord (cl.protocolflags);
	else
		ent->msg_origins[0][1] = ent->baseline.origin[1];
	if (bits & U_ANGLE2)
		ent->msg_angles[0][1] = MSG_ReadAngle (cl.protocolflags);
	else
		ent->msg_angles[0][1] = ent->baseline.angles[1];

	if (bits & U_ORIGIN3)
		ent->msg_origins[0][2] = MSG_ReadCoord (cl.protocolflags);
	else
		ent->msg_origins[0][2] = ent->baseline.origin[2];
	if (bits & U_ANGLE3)
		ent->msg_angles[0][2] = MSG_ReadAngle (cl.protocolflags);
	else
		ent->msg_angles[0][2] = ent->baseline.angles[2];

	if ( bits & U_NOLERP )
		ent->forcelink = true;
	ent->movestep = (bits & U_NOLERP) != 0;

	if (CL_ExtendedProtocol ())
	{
		ent->alpha = (bits & U_ALPHA) ? MSG_ReadByte () : ent->baseline.alpha;
		ent->scale = (bits & U_SCALE) ? MSG_ReadByte () : ent->baseline.scale;
		if (bits & U_FRAME2)
			ent->frame = (ent->frame & 0x00FF) | (MSG_ReadByte () << 8);
		if (bits & U_MODEL2)
			modnum = (modnum & 0x00FF) | (MSG_ReadByte () << 8);
		if (bits & U_LERPFINISH)
			ent->lerpfinish = ent->msgtime + MSG_ReadByte () / 255.0f;
		else
			ent->lerpfinish = 0;
	}
	else
	{
		ent->alpha = ent->baseline.alpha;
		ent->scale = ent->baseline.scale;
		ent->lerpfinish = 0;
	}

	if (modnum >= MAX_MODELS)
		Host_Error ("CL_ParseModel: bad modnum");
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
void CL_ParseBaseline (entity_t *ent, int version)
{
	int			i;
	int			bits = version == 2 ? MSG_ReadByte () : 0;	// svc_spawnbaseline2 / svc_spawnstatic2
	
	ent->baseline.modelindex = (bits & B_LARGEMODEL) ? (unsigned short)MSG_ReadShort () : MSG_ReadByte ();
	ent->baseline.frame = (bits & B_LARGEFRAME) ? (unsigned short)MSG_ReadShort () : MSG_ReadByte ();
	ent->baseline.colormap = MSG_ReadByte();
	ent->baseline.skin = MSG_ReadByte();
	for (i=0 ; i<3 ; i++)
	{
		ent->baseline.origin[i] = MSG_ReadCoord (cl.protocolflags);
		ent->baseline.angles[i] = MSG_ReadAngle (cl.protocolflags);
	}
	ent->baseline.alpha = (bits & B_ALPHA) ? MSG_ReadByte () : ENTALPHA_DEFAULT;
	ent->baseline.scale = (bits & B_SCALE) ? MSG_ReadByte () : ENTSCALE_DEFAULT;
	if (ent->baseline.modelindex >= MAX_MODELS)
		Host_Error ("CL_ParseBaseline: bad modelindex %i", ent->baseline.modelindex);
}


/*
==================
CL_ParseClientdata

Server information pertaining to this client only
==================
*/
void CL_ParseClientdata (void)
{
	int		i, j;
	int		bits;
	int		weapon, armor, ammo, packs[4], weaponframe;

	bits = (unsigned short)MSG_ReadShort ();
	if (CL_ExtendedProtocol ())
	{
		if (bits & SU_EXTEND1)
			bits |= MSG_ReadByte () << 16;
		if (bits & SU_EXTEND2)
			bits |= MSG_ReadByte () << 24;
	}
	
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
			if ( (i & (1U<<j)) && !(cl.items & (1U<<j)))
				cl.item_gettime[j] = cl.time;
		cl.items = i;
	}
		
	cl.onground = (bits & SU_ONGROUND) != 0;
	cl.inwater = (bits & SU_INWATER) != 0;

	weaponframe = (bits & SU_WEAPONFRAME) ? MSG_ReadByte () : 0;
	armor = (bits & SU_ARMOR) ? MSG_ReadByte () : 0;
	weapon = (bits & SU_WEAPON) ? MSG_ReadByte () : 0;
	
	i = MSG_ReadShort ();
	if (cl.stats[STAT_HEALTH] != i)
	{
		cl.stats[STAT_HEALTH] = i;
		Sbar_Changed ();
	}

	ammo = MSG_ReadByte ();
	for (i=0 ; i<4 ; i++)
		packs[i] = MSG_ReadByte ();

	i = MSG_ReadByte ();	// the active weapon, handled below

	// LuminousQuake: the 666/999 high bytes come after the original fields
	if (CL_ExtendedProtocol ())
	{
		if (bits & SU_WEAPON2)
			weapon |= MSG_ReadByte () << 8;
		if (bits & SU_ARMOR2)
			armor |= MSG_ReadByte () << 8;
		if (bits & SU_AMMO2)
			ammo |= MSG_ReadByte () << 8;
		for (j=0 ; j<4 ; j++)
			if (bits & (SU_SHELLS2 << j))
				packs[j] |= MSG_ReadByte () << 8;
		if (bits & SU_WEAPONFRAME2)
			weaponframe |= MSG_ReadByte () << 8;
		cl.viewent.alpha = (bits & SU_WEAPONALPHA) ? MSG_ReadByte () : ENTALPHA_DEFAULT;
	}
	else
		cl.viewent.alpha = ENTALPHA_DEFAULT;

	cl.stats[STAT_WEAPONFRAME] = weaponframe;
	if (cl.stats[STAT_ARMOR] != armor)
	{
		cl.stats[STAT_ARMOR] = armor;
		Sbar_Changed ();
	}
	if (cl.stats[STAT_WEAPON] != weapon)
	{
		cl.stats[STAT_WEAPON] = weapon;
		Sbar_Changed ();
	}
	if (cl.stats[STAT_AMMO] != ammo)
	{
		cl.stats[STAT_AMMO] = ammo;
		Sbar_Changed ();
	}
	for (j=0 ; j<4 ; j++)
		if (cl.stats[STAT_SHELLS+j] != packs[j])
		{
			cl.stats[STAT_SHELLS+j] = packs[j];
			Sbar_Changed ();
		}

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
	int		i, j;
	int		top, bottom;
	byte	*dest, *source;
	
	if (slot > cl.maxclients)
		Sys_Error ("CL_NewTranslation: slot > cl.maxclients");
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
void CL_ParseStatic (int version)
{
	entity_t *ent;
	int		i;
		
	i = cl.num_statics;
	if (i >= MAX_STATIC_ENTITIES)
		Host_Error ("Too many static entities");
	ent = &cl_static_entities[i];
	cl.num_statics++;
	CL_ParseBaseline (ent, version);

// copy it to the current state
	ent->model = cl.model_precache[ent->baseline.modelindex];
	ent->frame = ent->baseline.frame;
	ent->colormap = vid.colormap;
	ent->skinnum = ent->baseline.skin;
	ent->effects = ent->baseline.effects;
	ent->alpha = ent->baseline.alpha;
	ent->scale = ent->baseline.scale;

	VectorCopy (ent->baseline.origin, ent->origin);
	VectorCopy (ent->baseline.angles, ent->angles);	
	R_AddEfrags (ent);
}

/*
===================
CL_ParseStaticSound
===================
*/
void CL_ParseStaticSound (int version)
{
	vec3_t		org;
	int			sound_num, vol, atten;
	int			i;
	
	for (i=0 ; i<3 ; i++)
		org[i] = MSG_ReadCoord (cl.protocolflags);
	sound_num = version == 2 ? (unsigned short)MSG_ReadShort () : MSG_ReadByte ();
	if (sound_num >= MAX_SOUNDS)
		Host_Error ("CL_ParseStaticSound: bad sound %i", sound_num);
	vol = MSG_ReadByte ();
	atten = MSG_ReadByte ();
	
	S_StaticSound (cl.sound_precache[sound_num], org, vol, atten);
}


/*
=====================
CL_ParseFog / CL_Fog_f

LuminousQuake: FitzQuake fog. A change given a time fades to the new fog
over that many seconds, from whatever was shown when it arrived.
=====================
*/
cl_fog_t	cl_fog;

void CL_FogCurrent (float fog[4])
{
	int		i;
	float	f = 1;
	if (cl_fog.fade_end > cl_fog.fade_start && cl.time < cl_fog.fade_end)
		f = cl.time <= cl_fog.fade_start ? 0 : (float)((cl.time - cl_fog.fade_start) / (cl_fog.fade_end - cl_fog.fade_start));
	fog[0] = cl_fog.old_density + (cl_fog.density - cl_fog.old_density) * f;
	for (i=0 ; i<3 ; i++)
		fog[1+i] = cl_fog.old_color[i] + (cl_fog.color[i] - cl_fog.old_color[i]) * f;
}

static void CL_FogFade (float time)
{
	float	now[4];
	int		i;
	CL_FogCurrent (now);
	cl_fog.old_density = now[0];
	for (i=0 ; i<3 ; i++)
		cl_fog.old_color[i] = now[1+i];
	cl_fog.fade_start = cl.time;
	cl_fog.fade_end = cl.time + (time > 0 ? time : 0);
}

static void CL_FogSettle (void)
{
	int		i;
	cl_fog.old_density = cl_fog.density;
	for (i=0 ; i<3 ; i++)
		cl_fog.old_color[i] = cl_fog.color[i];
	cl_fog.fade_start = cl_fog.fade_end = 0;
}

void CL_ParseFog (void)
{
	int		i;
	float	time;
	byte	values[4];
	for (i=0 ; i<4 ; i++)
		values[i] = MSG_ReadByte ();
	time = MSG_ReadShort () / 100.0f;	// fade time in hundredths of a second
	CL_FogFade (time);
	cl_fog.density = values[0] / 255.0f;
	for (i=0 ; i<3 ; i++)
		cl_fog.color[i] = values[1+i] / 255.0f;
	if (time <= 0)
		CL_FogSettle ();
}

void CL_Fog_f (void)
{
	int		i, argc = Cmd_Argc ();
	float	time = argc >= 6 ? Q_atof (Cmd_Argv (5)) : 0;
	if (argc > 1)
		CL_FogFade (time);
	switch (argc)
	{
	case 1:
		Con_Printf ("usage: fog <density> [<red> <green> <blue> [<time>]] or fog <red> <green> <blue>\n");
		Con_Printf ("current values: %g %g %g %g\n", cl_fog.density,
			cl_fog.color[0], cl_fog.color[1], cl_fog.color[2]);
		return;
	case 4:		// red green blue
		for (i=0 ; i<3 ; i++)
			cl_fog.color[i] = Q_atof (Cmd_Argv (1 + i));
		break;
	default:	// density [red green blue [time]]
		cl_fog.density = Q_atof (Cmd_Argv (1));
		if (argc >= 5)
			for (i=0 ; i<3 ; i++)
				cl_fog.color[i] = Q_atof (Cmd_Argv (2 + i));
		break;
	}
	for (i=0 ; i<3 ; i++)
		cl_fog.color[i] = cl_fog.color[i] < 0 ? 0 : cl_fog.color[i] > 1 ? 1 : cl_fog.color[i];
	if (cl_fog.density < 0)
		cl_fog.density = 0;
	if (time <= 0)
		CL_FogSettle ();
}

/*
=====================
CL_Sky_f

LuminousQuake: "sky <name>" (FitzQuake; "loadsky" in DarkPlaces) shows the
skybox gfx/env/<name>{rt,bk,lf,ft,up,dn}; "sky" alone prints it, and an
empty name ("sky \"\"") goes back to the map's sky texture.
=====================
*/
void CL_Sky_f (void)
{
	if (Cmd_Argc () < 2)
	{
		Con_Printf ("sky: %s\n", cl.skyboxname[0] ? cl.skyboxname : "(the map's sky texture)");
		return;
	}
	Q_strncpy (cl.skyboxname, Cmd_Argv (1), sizeof(cl.skyboxname) - 1);
	cl.skyboxname[sizeof(cl.skyboxname) - 1] = 0;
}

#define SHOWNET(x) if(cl_shownet.value==2)Con_Printf ("%3i:%s\n", msg_readcount-1, x);

/*
=====================
CL_ParseServerMessage
=====================
*/
void CL_ParseServerMessage (void)
{
	int			cmd;
	int			i;
	
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
			Host_Error ("CL_ParseServerMessage: Bad server message");

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

		SHOWNET(cmd < (int)(sizeof(svc_strings) / sizeof(svc_strings[0])) ? svc_strings[cmd] : "unknown command");
	
	// other commands
		switch (cmd)
		{
		default:
			Host_Error ("CL_ParseServerMessage: Illegible server message (command %d at byte %d of %d, %s)\n",
				cmd, msg_readcount - 1, net_message.cursize,
				cls.demoplayback ? "demo playback" : "server connection");
			break;
			
        case svc_achievement:
        {
            char *achievement = MSG_ReadString();
            if (msg_badread)
                Host_Error("CL_ParseServerMessage: truncated achievement notification");
            /* Consume rerelease metadata without requiring an online service. */
            Con_DPrintf("Achievement notification: %s\n", achievement);
            break;
        }

		case svc_nop:
//			Con_Printf ("svc_nop\n");
			break;
			
		case svc_time:
			cl.mtime[1] = cl.mtime[0];
			cl.mtime[0] = MSG_ReadFloat ();			
			break;
			
		case svc_clientdata:
			CL_ParseClientdata ();
			break;
		
		case svc_version:
			i = MSG_ReadLong ();
			if (i != PROTOCOL_NETQUAKE && i != PROTOCOL_FITZQUAKE && i != PROTOCOL_RMQ)
				Host_Error ("CL_ParseServerMessage: Server is protocol %i instead of %i, %i or %i\n",
					i, PROTOCOL_NETQUAKE, PROTOCOL_FITZQUAKE, PROTOCOL_RMQ);
			cl.protocol = i;
			break;
			
		case svc_disconnect:
			Host_EndGame ("Server disconnected\n");

		case svc_print:
			Con_Printf ("%s", MSG_ReadString ());
			break;
			
		case svc_centerprint:
			SCR_CenterPrint ((char *)LQ_Localize(MSG_ReadString ()));
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
				cl.viewangles[i] = MSG_ReadAngle (cl.protocolflags);
			break;
			
		case svc_setview:
			cl.viewentity = MSG_ReadShort ();
			break;
					
		case svc_lightstyle:
			i = MSG_ReadByte ();
			if (i >= MAX_LIGHTSTYLES)
				Sys_Error ("svc_lightstyle > MAX_LIGHTSTYLES");
			Q_strcpy (cl_lightstyle[i].map,  MSG_ReadString());
			cl_lightstyle[i].length = Q_strlen(cl_lightstyle[i].map);
			break;
			
		case svc_sound:
			CL_ParseStartSoundPacket();
			break;
			
		case svc_stopsound:
			i = (unsigned short)MSG_ReadShort();
			S_StopSound(i>>3, i&7);
			break;
		
		case svc_updatename:
			Sbar_Changed ();
			i = MSG_ReadByte ();
			if (i >= cl.maxclients)
				Host_Error ("CL_ParseServerMessage: svc_updatename > MAX_SCOREBOARD");
			strcpy (cl.scores[i].name, MSG_ReadString ());
			break;
			
		case svc_updatefrags:
			Sbar_Changed ();
			i = MSG_ReadByte ();
			if (i >= cl.maxclients)
				Host_Error ("CL_ParseServerMessage: svc_updatefrags > MAX_SCOREBOARD");
			cl.scores[i].frags = MSG_ReadShort ();
			break;			

		case svc_updatecolors:
			Sbar_Changed ();
			i = MSG_ReadByte ();
			if (i >= cl.maxclients)
				Host_Error ("CL_ParseServerMessage: svc_updatecolors > MAX_SCOREBOARD");
			cl.scores[i].colors = MSG_ReadByte ();
			CL_NewTranslation (i);
			break;
			
		case svc_particle:
			R_ParseParticleEffect ();
			break;

		case svc_spawnbaseline:
			i = (unsigned short)MSG_ReadShort ();
			// must use CL_EntityNum() to force cl.num_entities up
			CL_ParseBaseline (CL_EntityNum(i), 1);
			break;
		case svc_spawnstatic:
			CL_ParseStatic (1);
			break;			

		// LuminousQuake: protocols 666 and 999
		case svc_spawnbaseline2:
			i = (unsigned short)MSG_ReadShort ();
			CL_ParseBaseline (CL_EntityNum(i), 2);
			break;
		case svc_spawnstatic2:
			CL_ParseStatic (2);
			break;
		case svc_spawnstaticsound2:
			CL_ParseStaticSound (2);
			break;
		case svc_skybox:
			Q_strncpy (cl.skyboxname, MSG_ReadString (), sizeof(cl.skyboxname) - 1);
			cl.skyboxname[sizeof(cl.skyboxname) - 1] = 0;
			break;
		case svc_bf:
			Cmd_ExecuteString ("bf", src_command);
			break;
		case svc_fog:
			CL_ParseFog ();
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
				Host_Error ("Received signon %i when at %i", i, cls.signon);
			cls.signon = i;
			CL_SignonReply ();
			break;

		case svc_killedmonster:
			cl.stats[STAT_MONSTERS]++;
			break;

		case svc_foundsecret:
			cl.stats[STAT_SECRETS]++;
			break;

		case svc_updatestat:
			i = MSG_ReadByte ();
			if (i < 0 || i >= MAX_CL_STATS)
				Sys_Error ("svc_updatestat: %i is invalid", i);
			cl.stats[i] = MSG_ReadLong ();;
			break;
			
		case svc_spawnstaticsound:
			CL_ParseStaticSound (1);
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
			cl.intermission = 1;
			cl.completed_time = cl.time;
			vid.recalc_refdef = true;	// go to full screen
			break;

		case svc_finale:
			cl.intermission = 2;
			cl.completed_time = cl.time;
			vid.recalc_refdef = true;	// go to full screen
			SCR_CenterPrint ((char *)LQ_Localize(MSG_ReadString ()));
			break;

		case svc_cutscene:
			cl.intermission = 3;
			cl.completed_time = cl.time;
			vid.recalc_refdef = true;	// go to full screen
			SCR_CenterPrint ((char *)LQ_Localize(MSG_ReadString ()));
			break;

		case svc_sellscreen:
			Cmd_ExecuteString ("help", src_command);
			break;
		}
	}
}

