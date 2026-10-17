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
	vec3_t		mins, maxs;
} snd_mover_t;

static snd_cell_t	cells[SND_CELLS];
static unsigned short cost[SND_CELLS];
static unsigned short stamp[SND_CELLS];
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

static void SND_FindMovers (void)
{
	int			i;
	entity_t	*ent;
	snd_mover_t	*m;

	mover_count = 0;
	for (i=1 ; i<cl.num_entities && mover_count < SND_MAX_MOVERS ; i++)
	{
		ent = &cl_entities[i];
		if (!ent->model || ent->model->type != mod_brush || ent->model->name[0] != '*')
			continue;
		m = &movers[mover_count++];
		m->ent = ent;
		VectorAdd (ent->origin, ent->model->mins, m->mins);
		VectorAdd (ent->origin, ent->model->maxs, m->maxs);
	}
}

// does a closed door, lift or other brush entity sit across a..b
static vec_t SND_MoverReach (vec3_t a, vec3_t b, vec_t length)
{
	int			i, k;
	snd_mover_t	*m;
	vec3_t		la, lb;
	vec_t		reach = length, r;

	for (i=0 ; i<mover_count ; i++)
	{
		m = &movers[i];
		for (k=0 ; k<3 ; k++)
			if ((a[k] < m->mins[k] && b[k] < m->mins[k]) || (a[k] > m->maxs[k] && b[k] > m->maxs[k]))
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

static void SND_Search (vec3_t listener)
{
	int			d, x, y, z, c, index;
	unsigned	item;
	snd_cell_t	*cell;
	vec3_t		a, b, delta;

	if (!++generation)
	{
		memset (stamp, 0, sizeof(stamp));
		generation = 1;
	}
	heap_count = 0;
	SND_FindMovers ();
	SND_CellOf (listener, origin_cell);

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
					|| SND_MoverReach (listener, a, Length (delta)) < Length (delta))
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
				if (SND_MoverReach (a, b, step_cost[d]) < step_cost[d])
					continue;
			}
			SND_Relax (x, y, z, c + step_cost[d]);
		}
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

/* 1 with *distance the travel through air, -1 when no open path reaches the
   sound within range, 0 when the paths say nothing (keep the straight line). */
int LQ_SoundPathDistance (vec3_t source, vec_t straight, vec_t *distance)
{
	int			s[3], x, y, z, index, found = 0;
	snd_cell_t	*cell;
	vec3_t		a, delta;
	vec_t		length, best = 0, through;

	if (!searched || straight > SND_PATH_RANGE)
		return 0;
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
					|| SND_MoverReach (a, source, length) < length - SND_SOURCE_SLACK)
					continue;
				best = through;
				found = 1;
			}
	if (!found)
		return -1;
	*distance = best;
	return 1;
}
