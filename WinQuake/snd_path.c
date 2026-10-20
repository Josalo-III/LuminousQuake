/*
LuminousQuake: how far sound travels through open air to reach the listener.

Sound bends round corners and through openings; only a closed path (solid
walls, closed doors) shuts it off. Air is a grid of SND_CELL-unit cells around
the listener. Each update a shortest-path search spreads from the listener
through cells whose centres are open and joined by an unobstructed segment, out
to SND_PATH_RANGE units of travel. A sound is reached through the open cells
beside it. World edges are traced once per cell and cached; doors, lifts and
other brush entities move, so steps crossing one are traced against it during
every search.

A brush entity's sound comes from the centre of its bounds, which is often
inside the mover itself or inside the wall it slides into (a lift at the
bottom of its shaft, a bar or secret door in its slot). When no open cell
beside that centre reaches it, the sound is reached from the open cells
touching the mover's bounds instead, through clear air to its surface.

Doors muffle rather than shut. A second search ignores doors, lifts and other
brush entities; a sound reached only that way plays at SND_MUFFLE, as through a
closed door. The change fades: a door's seal rises from 0 to 1 over the last
SND_FADE seconds before it reaches its closed (spawn) position and falls over
the first SND_FADE seconds of opening; at rest it seals fully wherever it
stands, and blocks only what it physically crosses. A mover never blocks a
sound made inside it (a lift's or door's own noise).
*/

#include "quakedef.h"

#define SND_CELL		32
#define SND_WINDOW_BITS	6
#define SND_WINDOW		(1 << SND_WINDOW_BITS)	// cells per axis, wrapping
#define SND_WINDOW_MASK	(SND_WINDOW - 1)
#define SND_CELLS		(SND_WINDOW * SND_WINDOW * SND_WINDOW)
#define SND_PATH_RANGE	960		// stays inside the window: 30 cells either way
#define SND_REACH		(SND_PATH_RANGE / SND_CELL)
#define SND_SOURCE_SLACK	32.0	// sources sit inside fixtures and doors
#define SND_REFRESH		0.1		// seconds; doors move between searches
#define SND_MAX_MOVERS	64
#define SND_FADE		0.3		// seconds of door travel over which sound fades
#define SND_MUFFLE		0.35	// gain of a sound heard only through closed doors
#define SND_HEAP		(1 << 20)

#define CELL_KNOWN		(1u << 26)
#define CELL_OPEN		(1u << 27)

typedef struct
{
	short		x, y, z;
	int			leaf;		// world leaf of the centre, for free same-leaf steps
	unsigned	edges;		// bit per direction: open to that neighbour through the world
} snd_cell_t;

typedef struct
{
	entity_t	*ent;
	model_t		*model;	// the model when gathered; the entity may lose it before the next gather
	float		seal;	// at the search, 0 open .. 1 shut
} snd_mover_t;

typedef struct
{
	vec3_t		origin;
	double		time;
	float		speed;
} snd_motion_t;

static snd_cell_t	cells[SND_CELLS];
static unsigned short costs[2][SND_CELLS];	// pass 0: sealing doors block; pass 1: no mover does
static unsigned short stamps[2][SND_CELLS];
static unsigned short *cost = costs[0], *stamp = stamps[0];
static int		pass;
static qboolean	leaky;		// a mover was sealing at the search: pass 1 is valid
static unsigned short generation;
static unsigned	heap[SND_HEAP];
static int		heap_count;

static int		offsets[26][3];
static int		step_cost[26];
static qboolean	tables;

static model_t	*cached_world;
static int		origin_cell[3];
static double	searched_at;
static qboolean	searched;

static snd_mover_t	movers[SND_MAX_MOVERS];
static int		mover_count;
static snd_motion_t	motion[MAX_EDICTS];

// a brush entity's sound reached at its surface, per search and pass
typedef struct
{
	unsigned short	generation;
	unsigned char	found;
	vec_t			distance;
} snd_surface_t;
static snd_surface_t	surface_cache[2][MAX_EDICTS];

static void SND_Tables (void)
{
	int		x, y, z, n = 0, axes;

	for (x=-1 ; x<=1 ; x++)
		for (y=-1 ; y<=1 ; y++)
			for (z=-1 ; z<=1 ; z++)
			{
				if (!x && !y && !z)
					continue;
				offsets[n][0] = x; offsets[n][1] = y; offsets[n][2] = z;
				axes = (x != 0) + (y != 0) + (z != 0);
				step_cost[n] = axes == 1 ? SND_CELL : axes == 2 ? 45 : 55;	// 32, 32*sqrt2, 32*sqrt3
				n++;
			}
	tables = true;		// direction d is opposite 25-d
}

static int SND_Index (int x, int y, int z)
{
	return (x & SND_WINDOW_MASK) | (y & SND_WINDOW_MASK) << SND_WINDOW_BITS
		| (z & SND_WINDOW_MASK) << (2 * SND_WINDOW_BITS);
}

static void SND_Centre (int x, int y, int z, vec3_t p)
{
	p[0] = (x + 0.5) * SND_CELL;
	p[1] = (y + 0.5) * SND_CELL;
	p[2] = (z + 0.5) * SND_CELL;
}

static void SND_CellOf (const vec3_t p, int c[3])
{
	int		i;

	for (i=0 ; i<3 ; i++)
		c[i] = (int)floor(p[i] / SND_CELL);
}

// true when the whole segment is open air in hull
static qboolean SND_Clear (hull_t *hull, vec3_t a, vec3_t b)
{
	trace_t	trace;

	memset (&trace, 0, sizeof(trace));
	trace.fraction = 1;
	trace.allsolid = true;
	SV_RecursiveHullCheck (hull, hull->firstclipnode, 0, 1, a, b, &trace);
	return trace.fraction == 1 && !trace.startsolid && !trace.allsolid;
}

// how far from a the segment runs before something solid
static vec_t SND_Reach (hull_t *hull, vec3_t a, vec3_t b, vec_t length)
{
	trace_t	trace;

	memset (&trace, 0, sizeof(trace));
	trace.fraction = 1;
	trace.allsolid = true;
	SV_RecursiveHullCheck (hull, hull->firstclipnode, 0, 1, a, b, &trace);
	if (trace.startsolid)
		return 0;
	return trace.fraction * length;
}

static snd_cell_t *SND_Cell (int x, int y, int z)
{
	snd_cell_t	*c = &cells[SND_Index (x, y, z)];
	vec3_t		p;
	mleaf_t		*leaf;

	if (c->x != x || c->y != y || c->z != z)
	{
		c->x = x; c->y = y; c->z = z; c->edges = 0;
		SND_Centre (x, y, z, p);
		leaf = Mod_PointInLeaf (p, cached_world);
		c->leaf = leaf - cached_world->leafs;
		if (leaf->contents != CONTENTS_SOLID)
			c->edges = CELL_OPEN;
	}
	return c;
}

// world openness of every direction out of an open cell, traced once
static void SND_Edges (snd_cell_t *c)
{
	int			d;
	snd_cell_t	*n;
	vec3_t		a, b;

	if (c->edges & CELL_KNOWN)
		return;
	SND_Centre (c->x, c->y, c->z, a);
	for (d=0 ; d<26 ; d++)
	{
		n = SND_Cell (c->x + offsets[d][0], c->y + offsets[d][1], c->z + offsets[d][2]);
		if (!(n->edges & CELL_OPEN))
			continue;
		if (n->edges & CELL_KNOWN)
		{
			if (n->edges & (1u << (25 - d)))
				c->edges |= 1u << d;
			continue;
		}
		SND_Centre (n->x, n->y, n->z, b);
		if (n->leaf == c->leaf || SND_Clear (cached_world->hulls, a, b))
			c->edges |= 1u << d;
	}
	c->edges |= CELL_KNOWN;
}

// how shut a door is now: anchored to 1 at its closed position, 0 when it is
// more than SND_FADE seconds of travel from it, 1 at rest anywhere
static float SND_Seal (entity_t *ent)
{
	snd_motion_t	*mo = &motion[ent - cl_entities];
	vec3_t			gap;
	vec_t			d;

	VectorSubtract (ent->origin, ent->baseline.origin, gap);
	d = Length (gap);
	if (d < 0.5 || mo->speed <= 0)
		return 1;
	d = 1 - d / (mo->speed * SND_FADE);
	return d > 0 ? d : 0;
}

static void SND_FindMovers (void)
{
	int			i;
	entity_t	*ent;
	snd_mover_t	*m;

	mover_count = 0;
	leaky = false;
	for (i=1 ; i<cl.num_entities && mover_count < SND_MAX_MOVERS ; i++)
	{
		ent = &cl_entities[i];
		if (!ent->model || ent->model->type != mod_brush || ent->model->name[0] != '*')
			continue;
		m = &movers[mover_count++];
		m->ent = ent;
		m->model = ent->model;
		{
			snd_motion_t	*mo = &motion[i];
			vec3_t			step;
			double			dt = realtime - mo->time;

			if (dt < 0 || dt > 1)
			{
				VectorCopy (ent->origin, mo->origin);
				mo->time = realtime;
				mo->speed = 0;
			}
			else if (dt >= 0.05)
			{
				VectorSubtract (ent->origin, mo->origin, step);
				mo->speed = Length (step) / dt;
				VectorCopy (ent->origin, mo->origin);
				mo->time = realtime;
			}
		}
		m->seal = SND_Seal (ent);
		if (m->seal > 0)
			leaky = true;
	}
}

// does a closed door, lift or other brush entity sit across a..b
static vec_t SND_MoverReach (vec3_t a, vec3_t b, vec_t length, const float *source)
{
	int			i, k;
	snd_mover_t	*m;
	vec3_t		la, lb, mins, maxs;
	vec_t		reach = length, r;

	for (i=0 ; i<mover_count ; i++)
	{
		m = &movers[i];
		if (pass || m->ent->model != m->model || m->seal <= 0)
			continue;
		// bounds where the door is now, not where it was at the last search
		VectorAdd (m->ent->origin, m->model->mins, mins);
		VectorAdd (m->ent->origin, m->model->maxs, maxs);
		if (source)
		{
			for (k=0 ; k<3 ; k++)
				if (source[k] < mins[k] - 1 || source[k] > maxs[k] + 1)
					break;
			if (k == 3)
				continue;	// the mover's own sound
		}
		for (k=0 ; k<3 ; k++)
			if ((a[k] < mins[k] && b[k] < mins[k]) || (a[k] > maxs[k] && b[k] > maxs[k]))
				break;
		if (k < 3)
			continue;
		VectorSubtract (a, m->ent->origin, la);
		VectorSubtract (b, m->ent->origin, lb);
		r = SND_Reach (m->ent->model->hulls, la, lb, length);
		if (r < reach)
			reach = r;
	}
	return reach;
}

static void SND_Push (int c, int index)
{
	int			i, parent;
	unsigned	item = (unsigned)c << 18 | index, swap;

	if (heap_count == SND_HEAP)
		return;
	i = heap_count++;
	heap[i] = item;
	while (i > 0 && heap[parent = (i - 1) / 2] > heap[i])
	{
		swap = heap[parent]; heap[parent] = heap[i]; heap[i] = swap;
		i = parent;
	}
}

static unsigned SND_Pop (void)
{
	unsigned	top = heap[0], swap;
	int			i = 0, child;

	heap[0] = heap[--heap_count];
	while ((child = 2 * i + 1) < heap_count)
	{
		if (child + 1 < heap_count && heap[child + 1] < heap[child])
			child++;
		if (heap[i] <= heap[child])
			break;
		swap = heap[i]; heap[i] = heap[child]; heap[child] = swap;
		i = child;
	}
	return top;
}

static void SND_Relax (int x, int y, int z, int c)
{
	int		index = SND_Index (x, y, z);

	if (c > SND_PATH_RANGE)
		return;
	if (stamp[index] == generation && cost[index] <= c)
		return;
	stamp[index] = generation;
	cost[index] = c;
	SND_Push (c, index);
}

static void SND_UsePass (int p)
{
	pass = p;
	cost = costs[p];
	stamp = stamps[p];
}

static void SND_Spread (vec3_t listener)
{
	int			d, x, y, z, c, index;
	unsigned	item;
	snd_cell_t	*cell;
	vec3_t		a, b, delta;

	heap_count = 0;

	// the listener enters the air through every nearby cell it can see
	for (x=-1 ; x<=1 ; x++)
		for (y=-1 ; y<=1 ; y++)
			for (z=-1 ; z<=1 ; z++)
			{
				cell = SND_Cell (origin_cell[0] + x, origin_cell[1] + y, origin_cell[2] + z);
				if (!(cell->edges & CELL_OPEN))
					continue;
				SND_Centre (cell->x, cell->y, cell->z, a);
				VectorSubtract (a, listener, delta);
				if (!SND_Clear (cached_world->hulls, listener, a)
					|| SND_MoverReach (listener, a, Length (delta), NULL) < Length (delta))
					continue;
				SND_Relax (cell->x, cell->y, cell->z, (int)Length (delta));
			}

	while (heap_count)
	{
		item = SND_Pop ();
		c = item >> 18;
		index = item & ((1 << 18) - 1);
		if (cost[index] != c)
			continue;		// superseded
		cell = &cells[index];
		SND_Edges (cell);
		SND_Centre (cell->x, cell->y, cell->z, a);
		for (d=0 ; d<26 ; d++)
		{
			if (!(cell->edges & (1u << d)))
				continue;
			x = cell->x + offsets[d][0];
			y = cell->y + offsets[d][1];
			z = cell->z + offsets[d][2];
			if (abs (x - origin_cell[0]) > SND_REACH || abs (y - origin_cell[1]) > SND_REACH
				|| abs (z - origin_cell[2]) > SND_REACH)
				continue;
			if (mover_count)
			{
				SND_Centre (x, y, z, b);
				if (SND_MoverReach (a, b, step_cost[d], NULL) < step_cost[d])
					continue;
			}
			SND_Relax (x, y, z, c + step_cost[d]);
		}
	}
}

static void SND_Search (vec3_t listener)
{
	if (!++generation)
	{
		memset (stamps, 0, sizeof(stamps));
		memset (surface_cache, 0, sizeof(surface_cache));
		generation = 1;
	}
	SND_FindMovers ();
	SND_CellOf (listener, origin_cell);
	SND_UsePass (0);
	SND_Spread (listener);
	if (leaky)
	{
		SND_UsePass (1);
		SND_Spread (listener);
		SND_UsePass (0);
	}
	searched = true;
}

void LQ_SoundPathsUpdate (vec3_t listener)
{
	int		c[3];

	if (!tables)
		SND_Tables ();
	if (cls.state != ca_connected || !cl.worldmodel)
	{
		searched = false;
		return;
	}
	if (cached_world != cl.worldmodel)
	{
		memset (cells, 0x80, sizeof(cells));	// no cell has key -32640
		memset (motion, 0, sizeof(motion));
		cached_world = cl.worldmodel;
		searched = false;
	}
	if (Mod_PointInLeaf (listener, cached_world)->contents == CONTENTS_SOLID)
	{
		searched = false;		// noclip through a wall: nothing to search from
		return;
	}
	SND_CellOf (listener, c);
	if (searched && c[0] == origin_cell[0] && c[1] == origin_cell[1] && c[2] == origin_cell[2]
		&& realtime - searched_at < SND_REFRESH && realtime >= searched_at)
		return;
	SND_Search (listener);
	searched_at = realtime;
}

// From the open cells touching a brush entity's bounds, the shortest way to
// its surface; 0 when none reaches it. The answer holds until the next
// search, so it is kept per entity and pass rather than traced every frame.

static int SND_MoverSurfaceSearch (int entnum, vec_t *distance);

static int SND_MoverSurfaceDistance (int entnum, vec_t *distance)
{
	snd_surface_t	*c;

	if (entnum <= 0 || entnum >= MAX_EDICTS)
		return 0;
	c = &surface_cache[pass][entnum];
	if (c->generation != generation)
	{
		c->generation = generation;
		c->found = SND_MoverSurfaceSearch (entnum, &c->distance);
	}
	if (c->found)
		*distance = c->distance;
	return c->found;
}

static int SND_MoverSurfaceSearch (int entnum, vec_t *distance)
{
	entity_t	*ent;
	int			lo[3], hi[3], x, y, z, k, index, found = 0;
	snd_cell_t	*cell;
	vec3_t		mins, maxs, a, p, delta;
	vec_t		length, through, best = 0;

	if (entnum <= 0 || entnum >= cl.num_entities)
		return 0;
	ent = &cl_entities[entnum];
	if (!ent->model || ent->model->type != mod_brush || ent->model->name[0] != '*')
		return 0;
	VectorAdd (ent->origin, ent->model->mins, mins);
	VectorAdd (ent->origin, ent->model->maxs, maxs);
	for (k=0 ; k<3 ; k++)
	{
		vec3_t	edge;

		edge[0] = edge[1] = edge[2] = 0;
		edge[k] = mins[k] - SND_CELL;
		lo[k] = (int)floor(edge[k] / SND_CELL);
		edge[k] = maxs[k] + SND_CELL;
		hi[k] = (int)floor(edge[k] / SND_CELL);
		if (lo[k] < origin_cell[k] - SND_REACH)
			lo[k] = origin_cell[k] - SND_REACH;
		if (hi[k] > origin_cell[k] + SND_REACH)
			hi[k] = origin_cell[k] + SND_REACH;
	}
	for (x=lo[0] ; x<=hi[0] ; x++)
		for (y=lo[1] ; y<=hi[1] ; y++)
			for (z=lo[2] ; z<=hi[2] ; z++)
			{
				index = SND_Index (x, y, z);
				cell = &cells[index];
				if (stamp[index] != generation || cell->x != x || cell->y != y || cell->z != z)
					continue;
				SND_Centre (x, y, z, a);
				for (k=0 ; k<3 ; k++)	// the nearest point of the mover's bounds
					p[k] = a[k] < mins[k] ? mins[k] : a[k] > maxs[k] ? maxs[k] : a[k];
				VectorSubtract (p, a, delta);
				length = Length (delta);
				if (length > SND_CELL * 1.75)	// not touching the bounds
					continue;
				through = cost[index] + length;
				if (found && through >= best)
					continue;
				if (length > 0 && (SND_Reach (cached_world->hulls, a, p, length) < length - 1
					|| SND_MoverReach (a, p, length, p) < length - 1))
					continue;
				best = through;
				found = 1;
			}
	if (found)
		*distance = best;
	return found;
}

static int SND_PassDistance (int entnum, vec3_t source, vec_t *distance)
{
	int			s[3], x, y, z, index, found = 0;
	snd_cell_t	*cell;
	vec3_t		a, delta;
	vec_t		length, best = 0, through;

	SND_CellOf (source, s);
	for (x=-1 ; x<=1 ; x++)
		for (y=-1 ; y<=1 ; y++)
			for (z=-1 ; z<=1 ; z++)
			{
				if (abs (s[0] + x - origin_cell[0]) > SND_REACH || abs (s[1] + y - origin_cell[1]) > SND_REACH
					|| abs (s[2] + z - origin_cell[2]) > SND_REACH)
					continue;
				index = SND_Index (s[0] + x, s[1] + y, s[2] + z);
				cell = &cells[index];
				if (stamp[index] != generation || cell->x != s[0] + x || cell->y != s[1] + y || cell->z != s[2] + z)
					continue;
				SND_Centre (cell->x, cell->y, cell->z, a);
				VectorSubtract (source, a, delta);
				length = Length (delta);
				through = cost[index] + length;
				if (found && through >= best)
					continue;
				if (SND_Reach (cached_world->hulls, a, source, length) < length - SND_SOURCE_SLACK
					|| SND_MoverReach (a, source, length, source) < length - SND_SOURCE_SLACK)
					continue;
				best = through;
				found = 1;
			}
	if (!found)
		return SND_MoverSurfaceDistance (entnum, distance) ? 1 : -1;
	*distance = best;
	return 1;
}

/* 1 with *distance the travel through air, -1 when no open path reaches the
   sound within range, 0 when the paths say nothing (keep the straight line).
   *open scales the sound when it is reached only through doors: SND_MUFFLE
   through a shut one, fading with the most sealed door's seal now, so the fade
   ends exactly as a door shuts. */
int LQ_SoundPathDistance (int entnum, vec3_t source, vec_t straight, vec_t *distance, vec_t *open)
{
	int		i, found;
	float	seal, most, partial = 0;

	*open = 1;
	if (!searched || straight > SND_PATH_RANGE)
		return 0;
	found = SND_PassDistance (entnum, source, distance);
	if (found > 0 || !leaky)
		return found;
	SND_UsePass (1);
	found = SND_PassDistance (entnum, source, distance);
	SND_UsePass (0);
	if (found < 0)
		return -1;
	// a door partly shut at the search is taken as the one in the way; with
	// none, the sound is behind shut doors
	most = 1;
	for (i=0 ; i<mover_count ; i++)
		if (movers[i].ent->model == movers[i].model && movers[i].seal > 0 && movers[i].seal < 1)
		{
			seal = SND_Seal (movers[i].ent);
			if (most == 1 || seal > partial)
				partial = seal;
			most = partial;
		}
	*open = 1 - most * (1 - SND_MUFFLE);
	return 1;
}
