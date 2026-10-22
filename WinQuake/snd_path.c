/*
LuminousQuake: how far sound travels through open air to reach the listener.

Sound bends round corners and through openings; only a closed path (solid
walls, closed doors) shuts it off. Air is a grid of SND_CELL-unit cells around
the listener. A shortest-path search spreads from the listener through cells
whose centres are open and joined by an unobstructed segment, out to
SND_PATH_RANGE units of travel. A sound is reached through the open cells
beside it. World edges are traced once per cell and cached; doors, lifts and
other brush entities move, so steps crossing one are traced against it during
every search.

The search runs on the sound thread in the time mixing leaves, a slice at a
time, and settles near cells before far ones. Sounds keep the last finished
search until the next one is done: sound is slow, and a distant sound's late
update matters least. The main thread gathers where each brush entity is once
a frame; the sound thread never reads the game's entities.

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

Threads: LQ_SoundPathsGather and LQ_SoundPathsForget run on the main thread,
everything else on the sound thread (on the main thread when there is none).
Gather holds the mixer lock; Take holds the mixer and path locks; Work and the
lookups hold the path lock; Forget takes both.
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
#define SND_MAX_MOVERS	256		// brush entities gathered each frame
#define SND_FADE		0.3		// seconds of door travel over which sound fades
#define SND_MUFFLE		0.35	// gain of a sound heard only through closed doors
#define SND_HEAP		(1 << 20)
#define SND_CLOCK_EVERY	64		// cells settled between looks at the clock

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
	int			entnum;
	model_t		*model;
	vec3_t		origin;
	float		seal;		// when gathered: 0 open .. 1 shut
} snd_mover_t;

typedef struct
{
	snd_mover_t	list[SND_MAX_MOVERS];
	int			count;
} snd_movers_t;

typedef struct
{
	vec3_t		origin;
	double		time;
	float		speed;
} snd_motion_t;

// a brush entity's sound reached at its surface, per search and pass
typedef struct
{
	unsigned short	generation;
	unsigned char	found;
	vec_t			distance;
} snd_surface_t;

// one search's answer
typedef struct
{
	unsigned short	cost[2][SND_CELLS];	// pass 0: sealing doors block; pass 1: no mover does
	unsigned short	stamp[2][SND_CELLS];
	unsigned short	generation;
	int				origin[3];			// the listener's cell
	qboolean		leaky;				// a mover in reach was sealing: pass 1 is valid
	snd_surface_t	surface[2][MAX_EDICTS];
} snd_result_t;

// main thread, under the mixer lock
static snd_movers_t	gathered;
static model_t		*gathered_world;
static vec3_t		gathered_listener;
static double		gathered_time;
static snd_motion_t	motion[MAX_EDICTS];

// sound thread
static model_t		*world;
static snd_movers_t	now;		// as last gathered
static vec3_t		listener;
static double		gathered_at;	// realtime when gathered

static snd_cell_t	cells[SND_CELLS];
static snd_result_t	results[2];
static snd_result_t	*done;		// the last finished search; NULL when none
static snd_result_t	*work = &results[0];
static unsigned short generation;

static qboolean		searching;
static int			search_pass;
static vec3_t		search_from;
static double		search_started;
static snd_movers_t	blockers;	// sealing movers in reach, where they stood at the start
static unsigned short *cost, *stamp;	// the pass being searched
static unsigned		heap[SND_HEAP];
static int			heap_count;

static int		offsets[26][3];
static int		step_cost[26];
static qboolean	tables;

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
		leaf = Mod_PointInLeaf (p, world);
		c->leaf = leaf - world->leafs;
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
		if (n->leaf == c->leaf || SND_Clear (world->hulls, a, b))
			c->edges |= 1u << d;
	}
	c->edges |= CELL_KNOWN;
}

// how shut a door is now: anchored to 1 at its closed position, 0 when it is
// more than SND_FADE seconds of travel from it, 1 at rest anywhere
static float SND_Seal (entity_t *ent, snd_motion_t *mo)
{
	vec3_t			gap;
	vec_t			d;

	VectorSubtract (ent->origin, ent->baseline.origin, gap);
	d = Length (gap);
	if (d < 0.5 || mo->speed <= 0)
		return 1;
	d = 1 - d / (mo->speed * SND_FADE);
	return d > 0 ? d : 0;
}

// Main thread, once a frame: where the listener and every brush entity are.
void LQ_SoundPathsGather (vec3_t origin)
{
	int			i;
	entity_t	*ent;
	snd_mover_t	*m;
	model_t		*map = cls.state == ca_connected ? cl.worldmodel : NULL;

	if (map != gathered_world)
	{
		memset (motion, 0, sizeof(motion));
		gathered_world = map;
	}
	VectorCopy (origin, gathered_listener);
	gathered_time = realtime;
	gathered.count = 0;
	if (!map)
		return;
	for (i=1 ; i<cl.num_entities && gathered.count < SND_MAX_MOVERS ; i++)
	{
		snd_motion_t	*mo = &motion[i];
		vec3_t			step;
		double			dt = realtime - mo->time;

		ent = &cl_entities[i];
		if (!ent->model || ent->model->type != mod_brush || ent->model->name[0] != '*')
			continue;
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
		m = &gathered.list[gathered.count++];
		m->entnum = i;
		m->model = ent->model;
		VectorCopy (ent->origin, m->origin);
		m->seal = SND_Seal (ent, mo);
	}
}

// Main thread, before the map's memory is freed: let go of the world.
void LQ_SoundPathsForget (void)
{
	LQ_MixerLock ();
	LQ_PathsLock ();
	gathered_world = world = NULL;
	gathered.count = now.count = 0;
	done = NULL;
	searching = false;
	LQ_PathsUnlock ();
	LQ_MixerUnlock ();
}

// Sound thread: take what the main thread gathered.
void LQ_SoundPathsTake (void)
{
	if (!tables)
		SND_Tables ();
	if (gathered_world != world)
	{
		memset (cells, 0x80, sizeof(cells));	// no cell has key -32640
		world = gathered_world;
		done = NULL;
		searching = false;
	}
	now.count = gathered.count;
	memcpy (now.list, gathered.list, gathered.count * sizeof(snd_mover_t));
	VectorCopy (gathered_listener, listener);
	gathered_at = gathered_time;
}

static snd_mover_t *SND_Mover (int entnum)
{
	int		i;

	for (i=0 ; i<now.count ; i++)
		if (now.list[i].entnum == entnum)
			return &now.list[i];
	return NULL;
}

// does a closed door, lift or other brush entity of set sit across a..b
static vec_t SND_MoverReach (const snd_movers_t *set, vec3_t a, vec3_t b, vec_t length, const float *source)
{
	int					i, k;
	const snd_mover_t	*m;
	vec3_t				la, lb, mins, maxs;
	vec_t				reach = length, r;

	for (i=0 ; i<set->count ; i++)
	{
		m = &set->list[i];
		if (m->seal <= 0)
			continue;
		VectorAdd (m->origin, m->model->mins, mins);
		VectorAdd (m->origin, m->model->maxs, maxs);
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
		VectorSubtract (a, m->origin, la);
		VectorSubtract (b, m->origin, lb);
		r = SND_Reach (m->model->hulls, la, lb, length);
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

// begin pass p: the listener enters the air through every nearby cell it can see
static void SND_Seed (int p)
{
	int			x, y, z;
	snd_cell_t	*cell;
	vec3_t		a, delta;

	search_pass = p;
	cost = work->cost[p];
	stamp = work->stamp[p];
	heap_count = 0;
	for (x=-1 ; x<=1 ; x++)
		for (y=-1 ; y<=1 ; y++)
			for (z=-1 ; z<=1 ; z++)
			{
				cell = SND_Cell (work->origin[0] + x, work->origin[1] + y, work->origin[2] + z);
				if (!(cell->edges & CELL_OPEN))
					continue;
				SND_Centre (cell->x, cell->y, cell->z, a);
				VectorSubtract (a, search_from, delta);
				if (!SND_Clear (world->hulls, search_from, a)
					|| (!p && SND_MoverReach (&blockers, search_from, a, Length (delta), NULL) < Length (delta)))
					continue;
				SND_Relax (cell->x, cell->y, cell->z, (int)Length (delta));
			}
}

// settle cells nearest first until the pass is done (true) or the clock passes until
static qboolean SND_Spread (double until)
{
	int			d, x, y, z, c, index, settled = 0;
	unsigned	item;
	snd_cell_t	*cell;
	vec3_t		a, b;

	while (heap_count)
	{
		if (until && ++settled % SND_CLOCK_EVERY == 0 && Sys_FloatTime () >= until)
			return false;
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
			if (abs (x - work->origin[0]) > SND_REACH || abs (y - work->origin[1]) > SND_REACH
				|| abs (z - work->origin[2]) > SND_REACH)
				continue;
			if (!search_pass && blockers.count)
			{
				SND_Centre (x, y, z, b);
				if (SND_MoverReach (&blockers, a, b, step_cost[d], NULL) < step_cost[d])
					continue;
			}
			SND_Relax (x, y, z, c + step_cost[d]);
		}
	}
	return true;
}

static void SND_Start (void)
{
	int			i, k;
	snd_mover_t	*m;
	vec3_t		mins, maxs;

	if (!++generation)
	{
		for (i=0 ; i<2 ; i++)
		{
			memset (results[i].stamp, 0, sizeof(results[i].stamp));
			memset (results[i].surface, 0, sizeof(results[i].surface));
		}
		done = NULL;
		generation = 1;
	}
	work->generation = generation;
	SND_CellOf (listener, work->origin);
	VectorCopy (listener, search_from);
	search_started = gathered_at;

	// a sealing mover outside every step of the search blocks none of them
	blockers.count = 0;
	work->leaky = false;
	for (i=0 ; i<now.count ; i++)
	{
		m = &now.list[i];
		if (m->seal <= 0)
			continue;
		VectorAdd (m->origin, m->model->mins, mins);
		VectorAdd (m->origin, m->model->maxs, maxs);
		for (k=0 ; k<3 ; k++)
			if (maxs[k] < (work->origin[k] - SND_REACH) * SND_CELL
				|| mins[k] > (work->origin[k] + SND_REACH + 1) * SND_CELL)
				break;
		if (k < 3)
			continue;
		blockers.list[blockers.count++] = *m;
		work->leaky = true;
	}
	searching = true;
	SND_Seed (0);
}

// Sound thread: carry the search on for up to seconds (all of it when
// negative). True when a new answer is ready.
qboolean LQ_SoundPathsWork (double seconds)
{
	int		c[3];
	double	until = seconds >= 0 ? Sys_FloatTime () + seconds : 0;

	if (!world)
		return false;
	if (!searching)
	{
		if (Mod_PointInLeaf (listener, world)->contents == CONTENTS_SOLID)
		{
			if (!done)
				return false;
			done = NULL;		// noclip through a wall: nothing to search from
			return true;
		}
		SND_CellOf (listener, c);
		if (done && c[0] == done->origin[0] && c[1] == done->origin[1] && c[2] == done->origin[2]
			&& gathered_at - search_started < SND_REFRESH && gathered_at >= search_started)
			return false;
		SND_Start ();
	}
	for (;;)
	{
		if (!SND_Spread (until))
			return false;
		if (search_pass == 0 && work->leaky)
		{
			SND_Seed (1);
			continue;
		}
		break;
	}
	searching = false;
	done = work;
	work = done == &results[0] ? &results[1] : &results[0];
	return true;
}

// From the open cells touching a brush entity's bounds, the shortest way to
// its surface; 0 when none reaches it. The answer holds until the next
// search, so it is kept per entity and pass rather than traced every frame.

static int SND_MoverSurfaceSearch (const snd_result_t *r, int p, int entnum, vec_t *distance);

static int SND_MoverSurfaceDistance (snd_result_t *r, int p, int entnum, vec_t *distance)
{
	snd_surface_t	*c;

	if (entnum <= 0 || entnum >= MAX_EDICTS)
		return 0;
	c = &r->surface[p][entnum];
	if (c->generation != r->generation)
	{
		c->generation = r->generation;
		c->found = SND_MoverSurfaceSearch (r, p, entnum, &c->distance);
	}
	if (c->found)
		*distance = c->distance;
	return c->found;
}

static int SND_MoverSurfaceSearch (const snd_result_t *r, int p, int entnum, vec_t *distance)
{
	snd_mover_t	*m = SND_Mover (entnum);
	int			lo[3], hi[3], x, y, z, k, index, found = 0;
	snd_cell_t	*cell;
	vec3_t		mins, maxs, a, q, delta;
	vec_t		length, through, best = 0;

	if (!m)
		return 0;
	VectorAdd (m->origin, m->model->mins, mins);
	VectorAdd (m->origin, m->model->maxs, maxs);
	for (k=0 ; k<3 ; k++)
	{
		lo[k] = (int)floor((mins[k] - SND_CELL) / SND_CELL);
		hi[k] = (int)floor((maxs[k] + SND_CELL) / SND_CELL);
		if (lo[k] < r->origin[k] - SND_REACH)
			lo[k] = r->origin[k] - SND_REACH;
		if (hi[k] > r->origin[k] + SND_REACH)
			hi[k] = r->origin[k] + SND_REACH;
	}
	for (x=lo[0] ; x<=hi[0] ; x++)
		for (y=lo[1] ; y<=hi[1] ; y++)
			for (z=lo[2] ; z<=hi[2] ; z++)
			{
				index = SND_Index (x, y, z);
				cell = &cells[index];
				if (r->stamp[p][index] != r->generation || cell->x != x || cell->y != y || cell->z != z)
					continue;
				SND_Centre (x, y, z, a);
				for (k=0 ; k<3 ; k++)	// the nearest point of the mover's bounds
					q[k] = a[k] < mins[k] ? mins[k] : a[k] > maxs[k] ? maxs[k] : a[k];
				VectorSubtract (q, a, delta);
				length = Length (delta);
				if (length > SND_CELL * 1.75)	// not touching the bounds
					continue;
				through = r->cost[p][index] + length;
				if (found && through >= best)
					continue;
				if (length > 0 && (SND_Reach (world->hulls, a, q, length) < length - 1
					|| (!p && SND_MoverReach (&now, a, q, length, q) < length - 1)))
					continue;
				best = through;
				found = 1;
			}
	if (found)
		*distance = best;
	return found;
}

static int SND_PassDistance (snd_result_t *r, int p, int entnum, vec3_t source, vec_t *distance)
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
				if (abs (s[0] + x - r->origin[0]) > SND_REACH || abs (s[1] + y - r->origin[1]) > SND_REACH
					|| abs (s[2] + z - r->origin[2]) > SND_REACH)
					continue;
				index = SND_Index (s[0] + x, s[1] + y, s[2] + z);
				cell = &cells[index];
				if (r->stamp[p][index] != r->generation || cell->x != s[0] + x || cell->y != s[1] + y || cell->z != s[2] + z)
					continue;
				SND_Centre (cell->x, cell->y, cell->z, a);
				VectorSubtract (source, a, delta);
				length = Length (delta);
				through = r->cost[p][index] + length;
				if (found && through >= best)
					continue;
				if (SND_Reach (world->hulls, a, source, length) < length - SND_SOURCE_SLACK
					|| (!p && SND_MoverReach (&now, a, source, length, source) < length - SND_SOURCE_SLACK))
					continue;
				best = through;
				found = 1;
			}
	if (!found)
		return SND_MoverSurfaceDistance (r, p, entnum, distance) ? 1 : -1;
	*distance = best;
	return 1;
}

/* 1 with *distance the travel through air, -1 when no open path reaches the
   sound within range, 0 when the paths say nothing (keep the straight line).
   *open scales the sound when it is reached only through doors: SND_MUFFLE
   through a shut one, fading with the most sealed door's seal now, so the fade
   ends exactly as a door shuts. Doors are where they were last gathered. */
int LQ_SoundPathDistance (int entnum, vec3_t source, vec_t straight, vec_t *distance, vec_t *open)
{
	int		i, found;
	float	seal, most, partial = 0;

	*open = 1;
	if (!done || straight > SND_PATH_RANGE)
		return 0;
	found = SND_PassDistance (done, 0, entnum, source, distance);
	if (found > 0 || !done->leaky)
		return found;
	found = SND_PassDistance (done, 1, entnum, source, distance);
	if (found < 0)
		return -1;
	// a door partly shut is taken as the one in the way; with none, the sound
	// is behind shut doors
	most = 1;
	for (i=0 ; i<now.count ; i++)
		if (now.list[i].seal > 0 && now.list[i].seal < 1)
		{
			seal = now.list[i].seal;
			if (most == 1 || seal > partial)
				partial = seal;
			most = partial;
		}
	*open = 1 - most * (1 - SND_MUFFLE);
	return 1;
}
