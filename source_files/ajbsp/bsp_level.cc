//------------------------------------------------------------------------
//
//  AJ-BSP  Copyright (C) 2000-2023  Andrew Apted, et al
//          Copyright (C) 1994-1998  Colin Reed
//          Copyright (C) 1997-1998  Lee Killough
//
//  Originally based on the program 'BSP', version 2.3.
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//------------------------------------------------------------------------

#include "bsp_system.h"
#include "bsp_local.h"
#include "bsp_raw_def.h"
#include "bsp_utility.h"
#include "bsp_wad.h"

// EPI
#include "str_lexer.h"

#include "miniz.h"

#define DEBUG_BLOCKMAP  0
#define DEBUG_REJECT    0

#define DEBUG_LOAD      0
#define DEBUG_BSP       0


namespace ajbsp
{

Wad_file * cur_wad;
Wad_file * xwa_wad;


static int block_x, block_y;
static int block_w, block_h;
static int block_count;

static int block_mid_x = 0;
static int block_mid_y = 0;

#define BLOCK_LIMIT  16000

#define DUMMY_DUP  0xFFFF


void GetBlockmapBounds(int *x, int *y, int *w, int *h)
{
	*x = block_x; *y = block_y;
	*w = block_w; *h = block_h;
}


int CheckLinedefInsideBox(int xmin, int ymin, int xmax, int ymax,
		int x1, int y1, int x2, int y2)
{
	int count = 2;
	int tmp;

	for (;;)
	{
		if (y1 > ymax)
		{
			if (y2 > ymax)
				return false;

			x1 = x1 + (int) ((x2-x1) * (double)(ymax-y1) / (double)(y2-y1));
			y1 = ymax;

			count = 2;
			continue;
		}

		if (y1 < ymin)
		{
			if (y2 < ymin)
				return false;

			x1 = x1 + (int) ((x2-x1) * (double)(ymin-y1) / (double)(y2-y1));
			y1 = ymin;

			count = 2;
			continue;
		}

		if (x1 > xmax)
		{
			if (x2 > xmax)
				return false;

			y1 = y1 + (int) ((y2-y1) * (double)(xmax-x1) / (double)(x2-x1));
			x1 = xmax;

			count = 2;
			continue;
		}

		if (x1 < xmin)
		{
			if (x2 < xmin)
				return false;

			y1 = y1 + (int) ((y2-y1) * (double)(xmin-x1) / (double)(x2-x1));
			x1 = xmin;

			count = 2;
			continue;
		}

		count--;

		if (count == 0)
			break;

		// swap end points
		tmp=x1;  x1=x2;  x2=tmp;
		tmp=y1;  y1=y2;  y2=tmp;
	}

	// linedef touches block
	return true;
}


/* ----- create blockmap ------------------------------------ */

#define BK_NUM    0
#define BK_MAX    1
#define BK_XOR    2
#define BK_FIRST  3

#define BK_QUANTUM  32

static void FindBlockmapLimits(bbox_t *bbox)
{
	double mid_x = 0;
	double mid_y = 0;

	bbox->minx = bbox->miny = SHRT_MAX;
	bbox->maxx = bbox->maxy = SHRT_MIN;

	for (int i=0 ; i < num_linedefs ; i++)
	{
		const linedef_t *L = lev_linedefs[i];

		if (! L->zero_len)
		{
			double x1 = L->start->x;
			double y1 = L->start->y;
			double x2 = L->end->x;
			double y2 = L->end->y;

			int lx = (int)floor(std::min(x1, x2));
			int ly = (int)floor(std::min(y1, y2));
			int hx = (int)ceil (std::max(x1, x2));
			int hy = (int)ceil (std::max(y1, y2));

			if (lx < bbox->minx) bbox->minx = lx;
			if (ly < bbox->miny) bbox->miny = ly;
			if (hx > bbox->maxx) bbox->maxx = hx;
			if (hy > bbox->maxy) bbox->maxy = hy;

			// compute middle of cluster
			mid_x += (lx + hx) / 2;
			mid_y += (ly + hy) / 2;
		}
	}

	if (num_linedefs > 0)
	{
		block_mid_x = I_ROUND(mid_x / (double)num_linedefs);
		block_mid_y = I_ROUND(mid_y / (double)num_linedefs);
	}

#if DEBUG_BLOCKMAP
	cur_info->Debug("Blockmap lines centered at (%d,%d)\n", block_mid_x, block_mid_y);
#endif
}


void InitBlockmap()
{
	bbox_t map_bbox;

	// find limits of linedefs, and store as map limits
	FindBlockmapLimits(&map_bbox);

	cur_info->Print(2, "    Map limits: (%d,%d) to (%d,%d)\n",
			map_bbox.minx, map_bbox.miny,
			map_bbox.maxx, map_bbox.maxy);

	block_x = map_bbox.minx - (map_bbox.minx & 0x7);
	block_y = map_bbox.miny - (map_bbox.miny & 0x7);

	block_w = ((map_bbox.maxx - block_x) / 128) + 1;
	block_h = ((map_bbox.maxy - block_y) / 128) + 1;

	block_count = block_w * block_h;
}


void PutBlockmap()
{
	// just create an empty blockmap lump
	CreateLevelLump("BLOCKMAP")->Finish();
	return;
}


//------------------------------------------------------------------------
// REJECT : Generate the reject table
//------------------------------------------------------------------------

void PutReject()
{
	// just create an empty reject lump
	CreateLevelLump("REJECT")->Finish();
	return;
}


//------------------------------------------------------------------------
// LEVEL : Level structure read/write functions.
//------------------------------------------------------------------------


// Note: ZDoom format support based on code (C) 2002,2003 Randy Heit


// per-level variables

const char *lev_current_name;

int lev_current_idx;
int lev_current_start;

map_format_e lev_format;

bool lev_force_v5;
bool lev_force_xnod;

bool lev_long_name;
bool lev_overflows;


// objects of loaded level, and stuff we've built
std::vector<vertex_t *>  lev_vertices;
std::vector<linedef_t *> lev_linedefs;
std::vector<sidedef_t *> lev_sidedefs;
std::vector<sector_t *>  lev_sectors;
std::vector<thing_t *>   lev_things;

std::vector<seg_t *>     lev_segs;
std::vector<subsec_t *>  lev_subsecs;
std::vector<node_t *>    lev_nodes;
std::vector<walltip_t *> lev_walltips;

int num_old_vert = 0;
int num_new_vert = 0;
int num_real_lines = 0;


/* ----- allocation routines ---------------------------- */

vertex_t *NewVertex()
{
	vertex_t *V = (vertex_t *) UtilCalloc(sizeof(vertex_t));
	V->index = (int)lev_vertices.size();
	lev_vertices.push_back(V);
	return V;
}

linedef_t *NewLinedef()
{
	linedef_t *L = (linedef_t *) UtilCalloc(sizeof(linedef_t));
	L->index = (int)lev_linedefs.size();
	lev_linedefs.push_back(L);
	return L;
}

sidedef_t *NewSidedef()
{
	sidedef_t *S = (sidedef_t *) UtilCalloc(sizeof(sidedef_t));
	S->index = (int)lev_sidedefs.size();
	lev_sidedefs.push_back(S);
	return S;
}

sector_t *NewSector()
{
	sector_t *S = (sector_t *) UtilCalloc(sizeof(sector_t));
	S->index = (int)lev_sectors.size();
	lev_sectors.push_back(S);
	return S;
}

thing_t *NewThing()
{
	thing_t *T = (thing_t *) UtilCalloc(sizeof(thing_t));
	T->index = (int)lev_things.size();
	lev_things.push_back(T);
	return T;
}

seg_t *NewSeg()
{
	seg_t *S = (seg_t *) UtilCalloc(sizeof(seg_t));
	lev_segs.push_back(S);
	return S;
}

subsec_t *NewSubsec()
{
	subsec_t *S = (subsec_t *) UtilCalloc(sizeof(subsec_t));
	lev_subsecs.push_back(S);
	return S;
}

node_t *NewNode()
{
	node_t *N = (node_t *) UtilCalloc(sizeof(node_t));
	lev_nodes.push_back(N);
	return N;
}

walltip_t *NewWallTip()
{
	walltip_t *WT = (walltip_t *) UtilCalloc(sizeof(walltip_t));
	lev_walltips.push_back(WT);
	return WT;
}


/* ----- free routines ---------------------------- */

void FreeVertices()
{
	for (unsigned int i = 0 ; i < lev_vertices.size() ; i++)
		UtilFree((void *) lev_vertices[i]);

	lev_vertices.clear();
}

void FreeLinedefs()
{
	for (unsigned int i = 0 ; i < lev_linedefs.size() ; i++)
		UtilFree((void *) lev_linedefs[i]);

	lev_linedefs.clear();
}

void FreeSidedefs()
{
	for (unsigned int i = 0 ; i < lev_sidedefs.size() ; i++)
		UtilFree((void *) lev_sidedefs[i]);

	lev_sidedefs.clear();
}

void FreeSectors()
{
	for (unsigned int i = 0 ; i < lev_sectors.size() ; i++)
		UtilFree((void *) lev_sectors[i]);

	lev_sectors.clear();
}

void FreeThings()
{
	for (unsigned int i = 0 ; i < lev_things.size() ; i++)
		UtilFree((void *) lev_things[i]);

	lev_things.clear();
}

void FreeSegs()
{
	for (unsigned int i = 0 ; i < lev_segs.size() ; i++)
		UtilFree((void *) lev_segs[i]);

	lev_segs.clear();
}

void FreeSubsecs()
{
	for (unsigned int i = 0 ; i < lev_subsecs.size() ; i++)
		UtilFree((void *) lev_subsecs[i]);

	lev_subsecs.clear();
}

void FreeNodes()
{
	for (unsigned int i = 0 ; i < lev_nodes.size() ; i++)
		UtilFree((void *) lev_nodes[i]);

	lev_nodes.clear();
}

void FreeWallTips()
{
	for (unsigned int i = 0 ; i < lev_walltips.size() ; i++)
		UtilFree((void *) lev_walltips[i]);

	lev_walltips.clear();
}


/* ----- reading routines ------------------------------ */

static vertex_t *SafeLookupVertex(int num)
{
	if (num >= num_vertices)
		cur_info->FatalError("illegal vertex number #%d\n", num);

	return lev_vertices[num];
}

static sector_t *SafeLookupSector(u16_t num)
{
	if (num == 0xFFFF)
		return NULL;

	if (num >= num_sectors)
		cur_info->FatalError("illegal sector number #%d\n", (int)num);

	return lev_sectors[num];
}

static inline sidedef_t *SafeLookupSidedef(u16_t num)
{
	if (num == 0xFFFF)
		return NULL;

	// silently ignore illegal sidedef numbers
	if (num >= (unsigned int)num_sidedefs)
		return NULL;

	return lev_sidedefs[num];
}


void GetVertices()
{
	int count = 0;

	Lump_c *lump = FindLevelLump("VERTEXES");

	if (lump)
		count = lump->Length() / (int)sizeof(raw_vertex_t);

#if DEBUG_LOAD
	cur_info->Debug("GetVertices: num = %d\n", count);
#endif

	if (lump == NULL || count == 0)
		return;

	if (! lump->Seek(0))
		cur_info->FatalError("Error seeking to vertices.\n");

	for (int i = 0 ; i < count ; i++)
	{
		raw_vertex_t raw;

		if (! lump->Read(&raw, sizeof(raw)))
			cur_info->FatalError("Error reading vertices.\n");

		vertex_t *vert = NewVertex();

		vert->x = (double) LE_S16(raw.x);
		vert->y = (double) LE_S16(raw.y);
	}

	num_old_vert = num_vertices;
}


void GetSectors()
{
	int count = 0;

	Lump_c *lump = FindLevelLump("SECTORS");

	if (lump)
		count = lump->Length() / (int)sizeof(raw_sector_t);

	if (lump == NULL || count == 0)
		return;

	if (! lump->Seek(0))
		cur_info->FatalError("Error seeking to sectors.\n");

#if DEBUG_LOAD
	cur_info->Debug("GetSectors: num = %d\n", count);
#endif

	for (int i = 0 ; i < count ; i++)
	{
		raw_sector_t raw;

		if (! lump->Read(&raw, sizeof(raw)))
			cur_info->FatalError("Error reading sectors.\n");

		sector_t *sector = NewSector();

		(void) sector;
	}
}


void GetThings()
{
	int count = 0;

	Lump_c *lump = FindLevelLump("THINGS");

	if (lump)
		count = lump->Length() / (int)sizeof(raw_thing_t);

	if (lump == NULL || count == 0)
		return;

	if (! lump->Seek(0))
		cur_info->FatalError("Error seeking to things.\n");

#if DEBUG_LOAD
	cur_info->Debug("GetThings: num = %d\n", count);
#endif

	for (int i = 0 ; i < count ; i++)
	{
		raw_thing_t raw;

		if (! lump->Read(&raw, sizeof(raw)))
			cur_info->FatalError("Error reading things.\n");

		thing_t *thing = NewThing();

		thing->x    = LE_S16(raw.x);
		thing->y    = LE_S16(raw.y);
		thing->type = LE_U16(raw.type);
	}
}


void GetThingsHexen()
{
	int count = 0;

	Lump_c *lump = FindLevelLump("THINGS");

	if (lump)
		count = lump->Length() / (int)sizeof(raw_hexen_thing_t);

	if (lump == NULL || count == 0)
		return;

	if (! lump->Seek(0))
		cur_info->FatalError("Error seeking to things.\n");

#if DEBUG_LOAD
	cur_info->Debug("GetThingsHexen: num = %d\n", count);
#endif

	for (int i = 0 ; i < count ; i++)
	{
		raw_hexen_thing_t raw;

		if (! lump->Read(&raw, sizeof(raw)))
			cur_info->FatalError("Error reading things.\n");

		thing_t *thing = NewThing();

		thing->x    = LE_S16(raw.x);
		thing->y    = LE_S16(raw.y);
		thing->type = LE_U16(raw.type);
	}
}


void GetSidedefs()
{
	int count = 0;

	Lump_c *lump = FindLevelLump("SIDEDEFS");

	if (lump)
		count = lump->Length() / (int)sizeof(raw_sidedef_t);

	if (lump == NULL || count == 0)
		return;

	if (! lump->Seek(0))
		cur_info->FatalError("Error seeking to sidedefs.\n");

#if DEBUG_LOAD
	cur_info->Debug("GetSidedefs: num = %d\n", count);
#endif

	for (int i = 0 ; i < count ; i++)
	{
		raw_sidedef_t raw;

		if (! lump->Read(&raw, sizeof(raw)))
			cur_info->FatalError("Error reading sidedefs.\n");

		sidedef_t *side = NewSidedef();

		side->sector = SafeLookupSector(LE_S16(raw.sector));
	}
}


void GetLinedefs()
{
	int count = 0;

	Lump_c *lump = FindLevelLump("LINEDEFS");

	if (lump)
		count = lump->Length() / (int)sizeof(raw_linedef_t);

	if (lump == NULL || count == 0)
		return;

	if (! lump->Seek(0))
		cur_info->FatalError("Error seeking to linedefs.\n");

#if DEBUG_LOAD
	cur_info->Debug("GetLinedefs: num = %d\n", count);
#endif

	for (int i = 0 ; i < count ; i++)
	{
		raw_linedef_t raw;

		if (! lump->Read(&raw, sizeof(raw)))
			cur_info->FatalError("Error reading linedefs.\n");

		linedef_t *line;

		vertex_t *start = SafeLookupVertex(LE_U16(raw.start));
		vertex_t *end   = SafeLookupVertex(LE_U16(raw.end));

		start->is_used = true;
		  end->is_used = true;

		line = NewLinedef();

		line->start = start;
		line->end   = end;

		// check for zero-length line
		line->zero_len =
			(fabs(start->x - end->x) < DIST_EPSILON) &&
			(fabs(start->y - end->y) < DIST_EPSILON);

		line->type  = LE_U16(raw.type);
		u16_t flags = LE_U16(raw.flags);
		s16_t tag   = LE_S16(raw.tag);

		line->two_sided   = (flags & MLF_TwoSided) != 0;
		line->is_precious = (tag >= 900 && tag < 1000); // Why is this the case? Need to investigate - Dasho

		line->right = SafeLookupSidedef(LE_U16(raw.right));
		line->left  = SafeLookupSidedef(LE_U16(raw.left));

		if (line->right || line->left)
			num_real_lines++;

		line->self_ref = (line->left && line->right &&
				(line->left->sector == line->right->sector));

		if (line->self_ref) line->is_precious = true;
	}
}


void GetLinedefsHexen()
{
	int count = 0;

	Lump_c *lump = FindLevelLump("LINEDEFS");

	if (lump)
		count = lump->Length() / (int)sizeof(raw_hexen_linedef_t);

	if (lump == NULL || count == 0)
		return;

	if (! lump->Seek(0))
		cur_info->FatalError("Error seeking to linedefs.\n");

#if DEBUG_LOAD
	cur_info->Debug("GetLinedefsHexen: num = %d\n", count);
#endif

	for (int i = 0 ; i < count ; i++)
	{
		raw_hexen_linedef_t raw;

		if (! lump->Read(&raw, sizeof(raw)))
			cur_info->FatalError("Error reading linedefs.\n");

		linedef_t *line;

		vertex_t *start = SafeLookupVertex(LE_U16(raw.start));
		vertex_t *end   = SafeLookupVertex(LE_U16(raw.end));

		start->is_used = true;
		  end->is_used = true;

		line = NewLinedef();

		line->start = start;
		line->end   = end;

		// check for zero-length line
		line->zero_len =
			(fabs(start->x - end->x) < DIST_EPSILON) &&
			(fabs(start->y - end->y) < DIST_EPSILON);

		line->type  = (u8_t) raw.type;
		u16_t flags = LE_U16(raw.flags);

		// -JL- Added missing twosided flag handling that caused a broken reject
		line->two_sided = (flags & MLF_TwoSided) != 0;

		line->right = SafeLookupSidedef(LE_U16(raw.right));
		line->left  = SafeLookupSidedef(LE_U16(raw.left));

		if (line->right || line->left)
			num_real_lines++;

		line->self_ref = (line->left && line->right &&
				(line->left->sector == line->right->sector));

		if (line->self_ref) line->is_precious = true;
	}
}


static inline int VanillaSegDist(const seg_t *seg)
{
	double lx = seg->side ? seg->linedef->end->x : seg->linedef->start->x;
	double ly = seg->side ? seg->linedef->end->y : seg->linedef->start->y;

	// use the "true" starting coord (as stored in the wad)
	double sx = round(seg->start->x);
	double sy = round(seg->start->y);

	return (int) floor(hypot(sx - lx, sy - ly) + 0.5);
}

static inline int VanillaSegAngle(const seg_t *seg)
{
	// compute the "true" delta
	double dx = round(seg->end->x) - round(seg->start->x);
	double dy = round(seg->end->y) - round(seg->start->y);

	double angle = ComputeAngle(dx, dy);

	if (angle < 0)
		angle += 360.0;

	int result = (int) floor(angle * 65536.0 / 360.0 + 0.5);

	return (result & 0xFFFF);
}


/* ----- UDMF reading routines ------------------------- */

#define UDMF_THING    1
#define UDMF_VERTEX   2
#define UDMF_SECTOR   3
#define UDMF_SIDEDEF  4
#define UDMF_LINEDEF  5

void ParseThingField(thing_t *thing, const std::string& key, const std::string& value)
{
	// Do we need more precision than an int for things? I think this would only be
	// an issue if/when polyobjects happen, as I think other thing types are ignored - Dasho

	if (key == "x")
		thing->x = I_ROUND(epi::LEX_Double(value));

	if (key == "y")
		thing->y = I_ROUND(epi::LEX_Double(value));

	if (key == "type")
		thing->type = epi::LEX_Int(value);
}


void ParseVertexField(vertex_t *vertex, const std::string& key, const std::string& value)
{
	if (key == "x")
		vertex->x = epi::LEX_Double(value);

	if (key == "y")
		vertex->y = epi::LEX_Double(value);
}


void ParseSidedefField(sidedef_t *side, const std::string& key, const std::string& value)
{
	if (key == "sector")
	{
		int num = epi::LEX_Int(value);

		if (num < 0 || num >= num_sectors)
			cur_info->FatalError("illegal sector number #%d\n", (int)num);

		side->sector = lev_sectors[num];
	}
}


void ParseLinedefField(linedef_t *line, const std::string& key, const std::string& value)
{
	if (key == "v1")
		line->start = SafeLookupVertex(epi::LEX_Int(value));

	if (key == "v2")
		line->end = SafeLookupVertex(epi::LEX_Int(value));

	if (key == "special")
		line->type = epi::LEX_Int(value);

	if (key == "twosided")
		line->two_sided = epi::LEX_Boolean(value);

	if (key == "sidefront")
	{
		int num = epi::LEX_Int(value);

		if (num < 0 || num >= (int)num_sidedefs)
			line->right = NULL;
		else
			line->right = lev_sidedefs[num];
	}

	if (key == "sideback")
	{
		int num = epi::LEX_Int(value);

		if (num < 0 || num >= (int)num_sidedefs)
			line->left = NULL;
		else
			line->left = lev_sidedefs[num];
	}
}


void ParseUDMF_Block(epi::lexer_c& lex, int cur_type)
{
	vertex_t  * vertex = NULL;
	thing_t   * thing  = NULL;
	sector_t  * sector = NULL;
	sidedef_t * side   = NULL;
	linedef_t * line   = NULL;

	switch (cur_type)
	{
		case UDMF_VERTEX:  vertex = NewVertex();  break;
		case UDMF_THING:   thing  = NewThing();   break;
		case UDMF_SECTOR:  sector = NewSector();  break;
		case UDMF_SIDEDEF: side   = NewSidedef(); break;
		case UDMF_LINEDEF: line   = NewLinedef(); break;
		default: break;
	}

	for (;;)
	{
		if (lex.Match("}"))
			break;

		std::string key;
		std::string value;

		epi::token_kind_e tok = lex.Next(key);

		if (tok == epi::TOK_EOF)
			cur_info->FatalError("Malformed TEXTMAP lump: unclosed block\n");

		if (tok != epi::TOK_Ident)
			cur_info->FatalError("Malformed TEXTMAP lump: missing key\n");

		if (! lex.Match("="))
			cur_info->FatalError("Malformed TEXTMAP lump: missing '='\n");

		tok = lex.Next(value);

		if (tok == epi::TOK_EOF || tok == epi::TOK_ERROR || value == "}")
			cur_info->FatalError("Malformed TEXTMAP lump: missing value\n");

		if (! lex.Match(";"))
			cur_info->FatalError("Malformed TEXTMAP lump: missing ';'\n");

		switch (cur_type)
		{
			case UDMF_VERTEX:  ParseVertexField (vertex, key, value); break;
			case UDMF_THING:   ParseThingField  (thing,  key, value); break;
			case UDMF_SIDEDEF: ParseSidedefField(side,   key, value); break;
			case UDMF_LINEDEF: ParseLinedefField(line,   key, value); break;

			case UDMF_SECTOR:
			default: /* just skip it */ break;
		}
	}

	// validate stuff

	if (line != NULL)
	{
		if (line->start == NULL || line->end == NULL)
			cur_info->FatalError("Linedef #%d is missing a vertex!\n", line->index);

		if (line->right || line->left)
			num_real_lines++;

		line->self_ref = (line->left && line->right &&
				(line->left->sector == line->right->sector));

		if (line->self_ref) line->is_precious = true;
	}
}


void ParseUDMF_Pass(const std::string& data, int pass)
{
	// pass = 1 : vertices, sectors, things
	// pass = 2 : sidedefs
	// pass = 3 : linedefs

	epi::lexer_c lex(data);

	for (;;)
	{
		std::string section;
		epi::token_kind_e tok = lex.Next(section);

		if (tok == epi::TOK_EOF)
			return;

		if (tok != epi::TOK_Ident)
		{
			cur_info->FatalError("Malformed TEXTMAP lump.\n");
			return;
		}

		// ignore top-level assignments
		if (lex.Match("="))
		{
			lex.Next(section);
			if (! lex.Match(";"))
				cur_info->FatalError("Malformed TEXTMAP lump: missing ';'\n");
			continue;
		}

		if (! lex.Match("{"))
			cur_info->FatalError("Malformed TEXTMAP lump: missing '{'\n");

		int cur_type = 0;

		if (section == "thing")
		{
			if (pass == 1)
				cur_type = UDMF_THING;
		}
		else if (section == "vertex")
		{
			if (pass == 1)
				cur_type = UDMF_VERTEX;
		}
		else if (section == "sector")
		{
			if (pass == 1)
				cur_type = UDMF_SECTOR;
		}
		else if (section == "sidedef")
		{
			if (pass == 2)
				cur_type = UDMF_SIDEDEF;
		}
		else if (section == "linedef")
		{
			if (pass == 3)
				cur_type = UDMF_LINEDEF;
		}

		// process the block
		ParseUDMF_Block(lex, cur_type);
	}
}


void ParseUDMF()
{
	Lump_c *lump = FindLevelLump("TEXTMAP");

	if (lump == NULL || ! lump->Seek(0))
		cur_info->FatalError("Error finding TEXTMAP lump.\n");

	// load the lump into this string
	std::string data(lump->Length(), 0);
	if (!lump->Read(data.data(), lump->Length()))
		cur_info->FatalError("Error reading TEXTMAP lump.\n");

	// now parse it...

	// the UDMF spec does not require objects to be in a dependency order.
	// for example: sidedefs may occur *after* the linedefs which refer to
	// them.  hence we perform multiple passes over the TEXTMAP data.

	ParseUDMF_Pass(data, 1);
	ParseUDMF_Pass(data, 2);
	ParseUDMF_Pass(data, 3);

	num_old_vert = num_vertices;
}


/* ----- writing routines ------------------------------ */

static const u8_t *lev_v2_magic = (u8_t *) "gNd2";
static const u8_t *lev_v5_magic = (u8_t *) "gNd5";


void MarkOverflow()
{
	lev_overflows = true;
}


void PutVertices(const char *name, int do_gl)
{
	int count, i;

	// this size is worst-case scenario
	int size = num_vertices * (int)sizeof(raw_vertex_t);

	Lump_c *lump = CreateLevelLump(name, size);

	for (i=0, count=0 ; i < num_vertices ; i++)
	{
		raw_vertex_t raw;

		const vertex_t *vert = lev_vertices[i];

		if ((do_gl ? 1 : 0) != (vert->is_new ? 1 : 0))
		{
			continue;
		}

		raw.x = LE_S16(I_ROUND(vert->x));
		raw.y = LE_S16(I_ROUND(vert->y));

		lump->Write(&raw, sizeof(raw));

		count++;
	}

	lump->Finish();

	if (count != (do_gl ? num_new_vert : num_old_vert))
		BugError("PutVertices miscounted (%d != %d)\n", count,
				do_gl ? num_new_vert : num_old_vert);

	if (! do_gl && count > 65534)
	{
		Failure("Number of vertices has overflowed.\n");
		MarkOverflow();
	}
}


void PutGLVertices(int do_v5)
{
	int count, i;

	// this size is worst-case scenario
	int size = 4 + num_vertices * (int)sizeof(raw_v2_vertex_t);

	Lump_c *lump = CreateLevelLump("GL_VERT", size);

	if (do_v5)
		lump->Write(lev_v5_magic, 4);
	else
		lump->Write(lev_v2_magic, 4);

	for (i=0, count=0 ; i < num_vertices ; i++)
	{
		raw_v2_vertex_t raw;

		const vertex_t *vert = lev_vertices[i];

		if (! vert->is_new)
			continue;

		raw.x = LE_S32(I_ROUND(vert->x * 65536.0));
		raw.y = LE_S32(I_ROUND(vert->y * 65536.0));

		lump->Write(&raw, sizeof(raw));

		count++;
	}

	lump->Finish();

	if (count != num_new_vert)
		BugError("PutGLVertices miscounted (%d != %d)\n", count, num_new_vert);
}


static inline u16_t VertexIndex16Bit(const vertex_t *v)
{
	if (v->is_new)
		return (u16_t) (v->index | 0x8000U);

	return (u16_t) v->index;
}


static inline u32_t VertexIndex_V5(const vertex_t *v)
{
	if (v->is_new)
		return (u32_t) (v->index | 0x80000000U);

	return (u32_t) v->index;
}


static inline u32_t VertexIndex_XNOD(const vertex_t *v)
{
	if (v->is_new)
		return (u32_t) (num_old_vert + v->index);

	return (u32_t) v->index;
}


void PutSegs()
{
	// this size is worst-case scenario
	int size = num_segs * (int)sizeof(raw_seg_t);

	Lump_c *lump = CreateLevelLump("SEGS", size);

	for (int i=0 ; i < num_segs ; i++)
	{
		raw_seg_t raw;

		const seg_t *seg = lev_segs[i];

		raw.start   = LE_U16(VertexIndex16Bit(seg->start));
		raw.end     = LE_U16(VertexIndex16Bit(seg->end));
		raw.angle   = LE_U16(VanillaSegAngle(seg));
		raw.linedef = LE_U16(seg->linedef->index);
		raw.flip    = LE_U16(seg->side);
		raw.dist    = LE_U16(VanillaSegDist(seg));

		lump->Write(&raw, sizeof(raw));

#if DEBUG_BSP
		cur_info->Debug("PUT SEG: %04X  Vert %04X->%04X  Line %04X %s  "
				"Angle %04X  (%1.1f,%1.1f) -> (%1.1f,%1.1f)\n", seg->index,
				LE_U16(raw.start), LE_U16(raw.end), LE_U16(raw.linedef),
				seg->side ? "L" : "R", LE_U16(raw.angle),
				seg->start->x, seg->start->y, seg->end->x, seg->end->y);
#endif
	}

	lump->Finish();

	if (num_segs > 65534)
	{
		Failure("Number of segs has overflowed.\n");
		MarkOverflow();
	}
}


void PutGLSegs_V2()
{
	// should not happen (we should have upgraded to V5)
	SYS_ASSERT(num_segs <= 65534);

	// this size is worst-case scenario
	int size = num_segs * (int)sizeof(raw_gl_seg_t);

	Lump_c *lump = CreateLevelLump("GL_SEGS", size);

	for (int i=0 ; i < num_segs ; i++)
	{
		raw_gl_seg_t raw;

		const seg_t *seg = lev_segs[i];

		raw.start = LE_U16(VertexIndex16Bit(seg->start));
		raw.end   = LE_U16(VertexIndex16Bit(seg->end));
		raw.side  = LE_U16(seg->side);

		if (seg->linedef != NULL)
			raw.linedef = LE_U16(seg->linedef->index);
		else
			raw.linedef = LE_U16(0xFFFF);

		if (seg->partner != NULL)
			raw.partner = LE_U16(seg->partner->index);
		else
			raw.partner = LE_U16(0xFFFF);

		lump->Write(&raw, sizeof(raw));

#if DEBUG_BSP
		cur_info->Debug("PUT GL SEG: %04X  Line %04X %s  Partner %04X  "
				"(%1.1f,%1.1f) -> (%1.1f,%1.1f)\n", seg->index, LE_U16(raw.linedef),
				seg->side ? "L" : "R", LE_U16(raw.partner),
				seg->start->x, seg->start->y, seg->end->x, seg->end->y);
#endif
	}

	lump->Finish();
}


void PutGLSegs_V5()
{
	// this size is worst-case scenario
	int size = num_segs * (int)sizeof(raw_v5_seg_t);

	Lump_c *lump = CreateLevelLump("GL_SEGS", size);

	for (int i=0 ; i < num_segs ; i++)
	{
		raw_v5_seg_t raw;

		const seg_t *seg = lev_segs[i];

		raw.start = LE_U32(VertexIndex_V5(seg->start));
		raw.end   = LE_U32(VertexIndex_V5(seg->end));
		raw.side  = LE_U16(seg->side);

		if (seg->linedef != NULL)
			raw.linedef = LE_U16(seg->linedef->index);
		else
			raw.linedef = LE_U16(0xFFFF);

		if (seg->partner != NULL)
			raw.partner = LE_U32(seg->partner->index);
		else
			raw.partner = LE_U32(0xFFFFFFFF);

		lump->Write(&raw, sizeof(raw));

#if DEBUG_BSP
		cur_info->Debug("PUT V3 SEG: %06X  Line %04X %s  Partner %06X  "
				"(%1.1f,%1.1f) -> (%1.1f,%1.1f)\n", seg->index, LE_U16(raw.linedef),
				seg->side ? "L" : "R", LE_U32(raw.partner),
				seg->start->x, seg->start->y, seg->end->x, seg->end->y);
#endif
	}

	lump->Finish();
}


void PutSubsecs(const char *name, int do_gl)
{
	int size = num_subsecs * (int)sizeof(raw_subsec_t);

	Lump_c * lump = CreateLevelLump(name, size);

	for (int i=0 ; i < num_subsecs ; i++)
	{
		raw_subsec_t raw;

		const subsec_t *sub = lev_subsecs[i];

		raw.first = LE_U16(sub->seg_list->index);
		raw.num   = LE_U16(sub->seg_count);

		lump->Write(&raw, sizeof(raw));

#if DEBUG_BSP
		cur_info->Debug("PUT SUBSEC %04X  First %04X  Num %04X\n",
				sub->index, LE_U16(raw.first), LE_U16(raw.num));
#endif
	}

	if (num_subsecs > 32767)
	{
		Failure("Number of %s has overflowed.\n", do_gl ? "GL subsectors" : "subsectors");
		MarkOverflow();
	}

	lump->Finish();
}


void PutGLSubsecs_V5()
{
	int size = num_subsecs * (int)sizeof(raw_v5_subsec_t);

	Lump_c *lump = CreateLevelLump("GL_SSECT", size);

	for (int i=0 ; i < num_subsecs ; i++)
	{
		raw_v5_subsec_t raw;

		const subsec_t *sub = lev_subsecs[i];

		raw.first = LE_U32(sub->seg_list->index);
		raw.num   = LE_U32(sub->seg_count);

		lump->Write(&raw, sizeof(raw));

#if DEBUG_BSP
		cur_info->Debug("PUT V3 SUBSEC %06X  First %06X  Num %06X\n",
					sub->index, LE_U32(raw.first), LE_U32(raw.num));
#endif
	}

	lump->Finish();
}


static int node_cur_index;

static void PutOneNode(node_t *node, Lump_c *lump)
{
	if (node->r.node)
		PutOneNode(node->r.node, lump);

	if (node->l.node)
		PutOneNode(node->l.node, lump);

	node->index = node_cur_index++;

	raw_node_t raw;

	// note that x/y/dx/dy are always integral in non-UDMF maps
	raw.x  = LE_S16(I_ROUND(node->x));
	raw.y  = LE_S16(I_ROUND(node->y));
	raw.dx = LE_S16(I_ROUND(node->dx));
	raw.dy = LE_S16(I_ROUND(node->dy));

	raw.b1.minx = LE_S16(node->r.bounds.minx);
	raw.b1.miny = LE_S16(node->r.bounds.miny);
	raw.b1.maxx = LE_S16(node->r.bounds.maxx);
	raw.b1.maxy = LE_S16(node->r.bounds.maxy);

	raw.b2.minx = LE_S16(node->l.bounds.minx);
	raw.b2.miny = LE_S16(node->l.bounds.miny);
	raw.b2.maxx = LE_S16(node->l.bounds.maxx);
	raw.b2.maxy = LE_S16(node->l.bounds.maxy);

	if (node->r.node)
		raw.right = LE_U16(node->r.node->index);
	else if (node->r.subsec)
		raw.right = LE_U16(node->r.subsec->index | 0x8000);
	else
		BugError("Bad right child in node %d\n", node->index);

	if (node->l.node)
		raw.left = LE_U16(node->l.node->index);
	else if (node->l.subsec)
		raw.left = LE_U16(node->l.subsec->index | 0x8000);
	else
		BugError("Bad left child in node %d\n", node->index);

	lump->Write(&raw, sizeof(raw));

#if DEBUG_BSP
	cur_info->Debug("PUT NODE %04X  Left %04X  Right %04X  "
			"(%d,%d) -> (%d,%d)\n", node->index, LE_U16(raw.left),
			LE_U16(raw.right), node->x, node->y,
			node->x + node->dx, node->y + node->dy);
#endif
}


static void PutOneNode_V5(node_t *node, Lump_c *lump)
{
	if (node->r.node)
		PutOneNode_V5(node->r.node, lump);

	if (node->l.node)
		PutOneNode_V5(node->l.node, lump);

	node->index = node_cur_index++;

	raw_v5_node_t raw;

	raw.x  = LE_S16(I_ROUND(node->x));
	raw.y  = LE_S16(I_ROUND(node->y));
	raw.dx = LE_S16(I_ROUND(node->dx));
	raw.dy = LE_S16(I_ROUND(node->dy));

	raw.b1.minx = LE_S16(node->r.bounds.minx);
	raw.b1.miny = LE_S16(node->r.bounds.miny);
	raw.b1.maxx = LE_S16(node->r.bounds.maxx);
	raw.b1.maxy = LE_S16(node->r.bounds.maxy);

	raw.b2.minx = LE_S16(node->l.bounds.minx);
	raw.b2.miny = LE_S16(node->l.bounds.miny);
	raw.b2.maxx = LE_S16(node->l.bounds.maxx);
	raw.b2.maxy = LE_S16(node->l.bounds.maxy);

	if (node->r.node)
		raw.right = LE_U32(node->r.node->index);
	else if (node->r.subsec)
		raw.right = LE_U32(node->r.subsec->index | 0x80000000U);
	else
		BugError("Bad right child in V5 node %d\n", node->index);

	if (node->l.node)
		raw.left = LE_U32(node->l.node->index);
	else if (node->l.subsec)
		raw.left = LE_U32(node->l.subsec->index | 0x80000000U);
	else
		BugError("Bad left child in V5 node %d\n", node->index);

	lump->Write(&raw, sizeof(raw));

#if DEBUG_BSP
	cur_info->Debug("PUT V5 NODE %08X  Left %08X  Right %08X  "
			"(%d,%d) -> (%d,%d)\n", node->index, LE_U32(raw.left),
			LE_U32(raw.right), node->x, node->y,
			node->x + node->dx, node->y + node->dy);
#endif
}


void PutNodes(const char *name, int do_v5, node_t *root)
{
	int struct_size = do_v5 ? (int)sizeof(raw_v5_node_t) : (int)sizeof(raw_node_t);

	// this can be bigger than the actual size, but never smaller
	int max_size = (num_nodes + 1) * struct_size;

	Lump_c *lump = CreateLevelLump(name, max_size);

	node_cur_index = 0;

	if (root != NULL)
	{
		if (do_v5)
			PutOneNode_V5(root, lump);
		else
			PutOneNode(root, lump);
	}

	lump->Finish();

	if (node_cur_index != num_nodes)
		BugError("PutNodes miscounted (%d != %d)\n", node_cur_index, num_nodes);

	if (!do_v5 && node_cur_index > 32767)
	{
		Failure("Number of nodes has overflowed.\n");
		MarkOverflow();
	}
}


void CheckLimits()
{
	// this could potentially be 65536, since there are no reserved values
	// for sectors, but there may be source ports or tools treating 0xFFFF
	// as a special value, so we are extra cautious here (and in some of
	// the other checks below, like the vertex counts).
	if (num_sectors > 65535)
	{
		Failure("Map has too many sectors.\n");
		MarkOverflow();
	}
	// the sidedef 0xFFFF is reserved to mean "no side" in DOOM map format
	if (num_sidedefs > 65535)
	{
		Failure("Map has too many sidedefs.\n");
		MarkOverflow();
	}
	// the linedef 0xFFFF is reserved for minisegs in GL nodes
	if (num_linedefs > 65535)
	{
		Failure("Map has too many linedefs.\n");
		MarkOverflow();
	}

	if (cur_info->gl_nodes && !cur_info->force_v5)
	{
		if (num_old_vert > 32767 ||
			num_new_vert > 32767 ||
			num_segs     > 65535 ||
			num_nodes    > 32767)
		{
			Warning("Forcing V5 of GL-Nodes due to overflows.\n");
			lev_force_v5 = true;
		}
	}

	if (! cur_info->force_xnod)
	{
		if (num_old_vert > 32767 ||
			num_new_vert > 32767 ||
			num_segs     > 32767 ||
			num_nodes    > 32767)
		{
			Warning("Forcing XNOD format nodes due to overflows.\n");
			lev_force_xnod = true;
		}
	}
}


struct Compare_seg_pred
{
	inline bool operator() (const seg_t *A, const seg_t *B) const
	{
		return A->index < B->index;
	}
};

void SortSegs()
{
	// do a sanity check
	for (int i = 0 ; i < num_segs ; i++)
		if (lev_segs[i]->index < 0)
			BugError("Seg %p never reached a subsector!\n", i);

	// sort segs into ascending index
	std::sort(lev_segs.begin(), lev_segs.end(), Compare_seg_pred());

	// remove unwanted segs
	while (lev_segs.size() > 0 && lev_segs.back()->index == SEG_IS_GARBAGE)
	{
		UtilFree((void *) lev_segs.back());
		lev_segs.pop_back();
	}
}


/* ----- ZDoom format writing --------------------------- */

static const u8_t *lev_XNOD_magic = (u8_t *) "XNOD";
static const u8_t *lev_XGL3_magic = (u8_t *) "XGL3";
static const u8_t *lev_ZGL3_magic = (u8_t *) "ZGL3";
static const u8_t *lev_ZNOD_magic = (u8_t *) "ZNOD";

void PutZVertices()
{
	int count, i;

	u32_t orgverts = LE_U32(num_old_vert);
	u32_t newverts = LE_U32(num_new_vert);

	ZLibAppendLump(&orgverts, 4);
	ZLibAppendLump(&newverts, 4);

	for (i=0, count=0 ; i < num_vertices ; i++)
	{
		raw_v2_vertex_t raw;

		const vertex_t *vert = lev_vertices[i];

		if (! vert->is_new)
			continue;

		raw.x = LE_S32(I_ROUND(vert->x * 65536.0));
		raw.y = LE_S32(I_ROUND(vert->y * 65536.0));

		ZLibAppendLump(&raw, sizeof(raw));

		count++;
	}

	if (count != num_new_vert)
		BugError("PutZVertices miscounted (%d != %d)\n", count, num_new_vert);
}


void PutZSubsecs()
{
	u32_t raw_num = LE_U32(num_subsecs);
	ZLibAppendLump(&raw_num, 4);

	int cur_seg_index = 0;

	for (int i=0 ; i < num_subsecs ; i++)
	{
		const subsec_t *sub = lev_subsecs[i];

		raw_num = LE_U32(sub->seg_count);
		ZLibAppendLump(&raw_num, 4);

		// sanity check the seg index values
		int count = 0;
		for (const seg_t *seg = sub->seg_list ; seg ; seg = seg->next, cur_seg_index++)
		{
			if (cur_seg_index != seg->index)
				BugError("PutZSubsecs: seg index mismatch in sub %d (%d != %d)\n",
						i, cur_seg_index, seg->index);

			count++;
		}

		if (count != sub->seg_count)
			BugError("PutZSubsecs: miscounted segs in sub %d (%d != %d)\n",
					i, count, sub->seg_count);
	}

	if (cur_seg_index != num_segs)
		BugError("PutZSubsecs miscounted segs (%d != %d)\n", cur_seg_index, num_segs);
}


void PutZSegs()
{
	u32_t raw_num = LE_U32(num_segs);
	ZLibAppendLump(&raw_num, 4);

	for (int i=0 ; i < num_segs ; i++)
	{
		const seg_t *seg = lev_segs[i];

		if (seg->index != i)
			BugError("PutZSegs: seg index mismatch (%d != %d)\n", seg->index, i);

		u32_t v1 = LE_U32(VertexIndex_XNOD(seg->start));
		u32_t v2 = LE_U32(VertexIndex_XNOD(seg->end));

		u16_t line = LE_U16(seg->linedef->index);
		u8_t  side = (u8_t) seg->side;

		ZLibAppendLump(&v1,   4);
		ZLibAppendLump(&v2,   4);
		ZLibAppendLump(&line, 2);
		ZLibAppendLump(&side, 1);
	}
}


void PutXGL3Segs()
{
	u32_t raw_num = LE_U32(num_segs);
	ZLibAppendLump(&raw_num, 4);

	for (int i=0 ; i < num_segs ; i++)
	{
		const seg_t *seg = lev_segs[i];

		if (seg->index != i)
			BugError("PutXGL3Segs: seg index mismatch (%d != %d)\n", seg->index, i);

		u32_t v1      = LE_U32(VertexIndex_XNOD(seg->start));
		u32_t partner = LE_U32(seg->partner ? seg->partner->index : -1);
		u32_t line    = LE_U32(seg->linedef ? seg->linedef->index : -1);
		u8_t  side    = (u8_t) seg->side;

		ZLibAppendLump(&v1,      4);
		ZLibAppendLump(&partner, 4);
		ZLibAppendLump(&line,    4);
		ZLibAppendLump(&side,    1);

#if DEBUG_BSP
		fprintf(stderr, "SEG[%d] v1=%d partner=%d line=%d side=%d\n", i, v1, partner, line, side);
#endif
	}
}


static void PutOneZNode(node_t *node, bool do_xgl3)
{
	raw_v5_node_t raw;

	if (node->r.node)
		PutOneZNode(node->r.node, do_xgl3);

	if (node->l.node)
		PutOneZNode(node->l.node, do_xgl3);

	node->index = node_cur_index++;

	if (do_xgl3)
	{
		u32_t x  = LE_S32(I_ROUND(node->x  * 65536.0));
		u32_t y  = LE_S32(I_ROUND(node->y  * 65536.0));
		u32_t dx = LE_S32(I_ROUND(node->dx * 65536.0));
		u32_t dy = LE_S32(I_ROUND(node->dy * 65536.0));

		ZLibAppendLump(&x,  4);
		ZLibAppendLump(&y,  4);
		ZLibAppendLump(&dx, 4);
		ZLibAppendLump(&dy, 4);
	}
	else
	{
		raw.x  = LE_S16(I_ROUND(node->x));
		raw.y  = LE_S16(I_ROUND(node->y));
		raw.dx = LE_S16(I_ROUND(node->dx));
		raw.dy = LE_S16(I_ROUND(node->dy));

		ZLibAppendLump(&raw.x,  2);
		ZLibAppendLump(&raw.y,  2);
		ZLibAppendLump(&raw.dx, 2);
		ZLibAppendLump(&raw.dy, 2);
	}

	raw.b1.minx = LE_S16(node->r.bounds.minx);
	raw.b1.miny = LE_S16(node->r.bounds.miny);
	raw.b1.maxx = LE_S16(node->r.bounds.maxx);
	raw.b1.maxy = LE_S16(node->r.bounds.maxy);

	raw.b2.minx = LE_S16(node->l.bounds.minx);
	raw.b2.miny = LE_S16(node->l.bounds.miny);
	raw.b2.maxx = LE_S16(node->l.bounds.maxx);
	raw.b2.maxy = LE_S16(node->l.bounds.maxy);

	ZLibAppendLump(&raw.b1, sizeof(raw.b1));
	ZLibAppendLump(&raw.b2, sizeof(raw.b2));

	if (node->r.node)
		raw.right = LE_U32(node->r.node->index);
	else if (node->r.subsec)
		raw.right = LE_U32(node->r.subsec->index | 0x80000000U);
	else
		BugError("Bad right child in V5 node %d\n", node->index);

	if (node->l.node)
		raw.left = LE_U32(node->l.node->index);
	else if (node->l.subsec)
		raw.left = LE_U32(node->l.subsec->index | 0x80000000U);
	else
		BugError("Bad left child in V5 node %d\n", node->index);

	ZLibAppendLump(&raw.right, 4);
	ZLibAppendLump(&raw.left,  4);

#if DEBUG_BSP
	cur_info->Debug("PUT Z NODE %08X  Left %08X  Right %08X  "
			"(%d,%d) -> (%d,%d)\n", node->index, LE_U32(raw.left),
			LE_U32(raw.right), node->x, node->y,
			node->x + node->dx, node->y + node->dy);
#endif
}


void PutZNodes(node_t *root, bool do_xgl3)
{
	u32_t raw_num = LE_U32(num_nodes);
	ZLibAppendLump(&raw_num, 4);

	node_cur_index = 0;

	if (root)
		PutOneZNode(root, do_xgl3);

	if (node_cur_index != num_nodes)
		BugError("PutZNodes miscounted (%d != %d)\n", node_cur_index, num_nodes);
}


static int CalcZDoomNodesSize()
{
	// compute size of the ZDoom format nodes.
	// it does not need to be exact, but it *does* need to be bigger
	// (or equal) to the actual size of the lump.

	int size = 32;  // header + a bit extra

	size += 8 + num_vertices * 8;
	size += 4 + num_subsecs  * 4;
	size += 4 + num_segs     * 11;
	size += 4 + num_nodes    * sizeof(raw_v5_node_t);

	if (cur_info->force_compress)
	{
		// according to RFC1951, the zlib compression worst-case
		// scenario is 5 extra bytes per 32KB (0.015% increase).
		// we are significantly more conservative!

		size += ((size + 255) >> 5);
	}

	return size;
}


void SaveZDFormat(node_t *root_node)
{
	// leave SEGS and SSECTORS empty
	CreateLevelLump("SEGS")->Finish();
	CreateLevelLump("SSECTORS")->Finish();

	int max_size = CalcZDoomNodesSize();

	Lump_c *lump = CreateLevelLump("NODES", max_size);

	if (cur_info->force_compress)
		lump->Write(lev_ZNOD_magic, 4);
	else
		lump->Write(lev_XNOD_magic, 4);

	// the ZLibXXX functions do no compression for XNOD format
	ZLibBeginLump(lump);

	PutZVertices();
	PutZSubsecs();
	PutZSegs();
	PutZNodes(root_node, false);

	ZLibFinishLump();
}


void SaveXGL3Format(Lump_c *lump, node_t *root_node)
{
	// WISH : compute a max_size

	if (cur_info->force_compress)
		lump->Write(lev_ZGL3_magic, 4);
	else
		lump->Write(lev_XGL3_magic, 4);

	ZLibBeginLump(lump);

	PutZVertices();
	PutZSubsecs();
	PutXGL3Segs();
	PutZNodes(root_node, true /* do_xgl3 */);

	ZLibFinishLump();
}


/* ----- whole-level routines --------------------------- */

void LoadLevel()
{
	Lump_c *LEV = cur_wad->GetLump(lev_current_start);

	lev_current_name = LEV->Name();
	lev_long_name = false;
	lev_overflows = false;

	cur_info->ShowMap(lev_current_name);

	num_new_vert   = 0;
	num_real_lines = 0;

	if (lev_format == MAPF_UDMF)
	{
		ParseUDMF();
	}
	else
	{
		GetVertices();
		GetSectors();
		GetSidedefs();

		if (lev_format == MAPF_Hexen)
		{
			GetLinedefsHexen();
			GetThingsHexen();
		}
		else
		{
			GetLinedefs();
			GetThings();
		}

		// always prune vertices at end of lump, otherwise all the
		// unused vertices from seg splits would keep accumulating.
		PruneVerticesAtEnd();
	}

	cur_info->Print(2, "    Loaded %d vertices, %d sectors, %d sides, %d lines, %d things\n",
				num_vertices, num_sectors, num_sidedefs, num_linedefs, num_things);

	DetectOverlappingVertices();
	DetectOverlappingLines();

	CalculateWallTips();

	// -JL- Find sectors containing polyobjs
	switch (lev_format)
	{
		case MAPF_Hexen: DetectPolyobjSectors(false); break;
		case MAPF_UDMF:  DetectPolyobjSectors(true);  break;
		default:         break;
	}
}


void FreeLevel()
{
	FreeVertices();
	FreeSidedefs();
	FreeLinedefs();
	FreeSectors();
	FreeThings();
	FreeSegs();
	FreeSubsecs();
	FreeNodes();
	FreeWallTips();
	FreeIntersections();
}


static u32_t CalcGLChecksum(void)
{
	u32_t crc;

	Adler32_Begin(&crc);

	Lump_c *lump = FindLevelLump("VERTEXES");

	if (lump && lump->Length() > 0)
	{
		u8_t *data = new u8_t[lump->Length()];

		if (! lump->Seek(0) ||
		    ! lump->Read(data, lump->Length()))
			cur_info->FatalError("Error reading vertices (for checksum).\n");

		Adler32_AddBlock(&crc, data, lump->Length());
		delete[] data;
	}

	lump = FindLevelLump("LINEDEFS");

	if (lump && lump->Length() > 0)
	{
		u8_t *data = new u8_t[lump->Length()];

		if (! lump->Seek(0) ||
		    ! lump->Read(data, lump->Length()))
			cur_info->FatalError("Error reading linedefs (for checksum).\n");

		Adler32_AddBlock(&crc, data, lump->Length());
		delete[] data;
	}

	return crc;
}


void UpdateGLMarker(Lump_c *marker)
{
	// this is very conservative, around 4 times the actual size
	const int max_size = 512;

	// we *must* compute the checksum BEFORE (re)creating the lump
	// [ otherwise we write data into the wrong part of the file ]
	u32_t crc = CalcGLChecksum();

	cur_wad->RecreateLump(marker, max_size);

	if (lev_long_name)
	{
		marker->Printf("LEVEL=%s\n", lev_current_name);
	}

	marker->Printf("BUILDER=%s\n", "AJBSP " AJBSP_VERSION);
	marker->Printf("CHECKSUM=0x%08x\n", crc);

	marker->Finish();
}


static void AddMissingLump(const char *name, const char *after)
{
	if (cur_wad->LevelLookupLump(lev_current_idx, name) >= 0)
		return;

	int exist = cur_wad->LevelLookupLump(lev_current_idx, after);

	// if this happens, the level structure is very broken
	if (exist < 0)
	{
		Warning("Missing %s lump -- level structure is broken\n", after);

		exist = cur_wad->LevelLastLump(lev_current_idx);
	}

	cur_wad->InsertPoint(exist + 1);

	cur_wad->AddLump(name)->Finish();
}


build_result_e SaveLevel(node_t *root_node)
{
	// Note: root_node may be NULL

	cur_wad->BeginWrite();

	// remove any existing GL-Nodes
	cur_wad->RemoveGLNodes(lev_current_idx);

	// ensure all necessary level lumps are present
	AddMissingLump("SEGS",     "VERTEXES");
	AddMissingLump("SSECTORS", "SEGS");
	AddMissingLump("NODES",    "SSECTORS");
	AddMissingLump("REJECT",   "SECTORS");
	AddMissingLump("BLOCKMAP", "REJECT");

	// user preferences
	lev_force_v5   = cur_info->force_v5;
	lev_force_xnod = cur_info->force_xnod;

	// check for overflows...
	// this sets the force_xxx vars if certain limits are breached
	CheckLimits();


	/* --- GL Nodes --- */

	Lump_c * gl_marker = NULL;

	if (cur_info->gl_nodes && num_real_lines > 0)
	{
		// this also removes minisegs and degenerate segs
		SortSegs();

		// create empty marker now, flesh it out later
		gl_marker = CreateGLMarker();

		PutGLVertices(lev_force_v5);

		if (lev_force_v5)
			PutGLSegs_V5();
		else
			PutGLSegs_V2();

		if (lev_force_v5)
			PutGLSubsecs_V5();
		else
			PutSubsecs("GL_SSECT", true);

		PutNodes("GL_NODES", lev_force_v5, root_node);

		// -JL- Add empty PVS lump
		CreateLevelLump("GL_PVS")->Finish();
	}


	/* --- Normal nodes --- */

	// remove all the mini-segs from subsectors
	NormaliseBspTree();

	if (lev_force_xnod && num_real_lines > 0)
	{
		SortSegs();
		SaveZDFormat(root_node);
	}
	else
	{
		// reduce vertex precision for classic DOOM nodes.
		// some segs can become "degenerate" after this, and these
		// are removed from subsectors.
		RoundOffBspTree();

		SortSegs();

		PutVertices("VERTEXES", false);

		PutSegs();
		PutSubsecs("SSECTORS", false);
		PutNodes("NODES", false, root_node);
	}

	PutBlockmap();
	PutReject();

	// keyword support (v5.0 of the specs).
	// must be done *after* doing normal nodes, for proper checksum.
	if (gl_marker)
	{
		UpdateGLMarker(gl_marker);
	}

	cur_wad->EndWrite();

	if (lev_overflows)
	{
		// no message here
		// [ in verbose mode, each overflow already printed a message ]
		// [ in normal mode, we don't want any messages at all ]

		return BUILD_LumpOverflow;
	}

	return BUILD_OK;
}


build_result_e SaveUDMF(node_t *root_node)
{
	cur_wad->BeginWrite();

	// remove any existing ZNODES lump
	cur_wad->RemoveZNodes(lev_current_idx);

	Lump_c *lump = CreateLevelLump("ZNODES", -1);

	if (num_real_lines == 0)
	{
		lump->Finish();
	}
	else
	{
		SortSegs();
		SaveXGL3Format(lump, root_node);
	}

	cur_wad->EndWrite();

	return BUILD_OK;
}


build_result_e SaveXWA(node_t *root_node)
{
	xwa_wad->BeginWrite();

	const char *lev_name = GetLevelName(lev_current_idx);
	Lump_c *lump = xwa_wad->AddLump(lev_name);

	if (num_real_lines == 0)
	{
		lump->Finish();
	}
	else
	{
		SortSegs();
		SaveXGL3Format(lump, root_node);
	}

	xwa_wad->EndWrite();

	return BUILD_OK;
}


//----------------------------------------------------------------------

static Lump_c  *zout_lump;

static z_stream zout_stream;
static Bytef    zout_buffer[1024];


void ZLibBeginLump(Lump_c *lump)
{
	zout_lump = lump;

	if (! cur_info->force_compress)
		return;

	zout_stream.zalloc = (alloc_func)0;
	zout_stream.zfree  = (free_func)0;
	zout_stream.opaque = (voidpf)0;

	if (Z_OK != deflateInit(&zout_stream, Z_DEFAULT_COMPRESSION))
		cur_info->FatalError("Trouble setting up zlib compression\n");

	zout_stream.next_out  = zout_buffer;
	zout_stream.avail_out = sizeof(zout_buffer);
}


void ZLibAppendLump(const void *data, int length)
{
	// ASSERT(zout_lump)
	// ASSERT(length > 0)

	if (! cur_info->force_compress)
	{
		zout_lump->Write(data, length);
		return;
	}

	zout_stream.next_in  = (Bytef*)data;   // const override
	zout_stream.avail_in = length;

	while (zout_stream.avail_in > 0)
	{
		int err = deflate(&zout_stream, Z_NO_FLUSH);

		if (err != Z_OK)
			cur_info->FatalError("Trouble compressing %d bytes (zlib)\n", length);

		if (zout_stream.avail_out == 0)
		{
			zout_lump->Write(zout_buffer, sizeof(zout_buffer));

			zout_stream.next_out  = zout_buffer;
			zout_stream.avail_out = sizeof(zout_buffer);
		}
	}
}


void ZLibFinishLump(void)
{
	if (! cur_info->force_compress)
	{
		zout_lump->Finish();
		zout_lump = NULL;
		return;
	}

	int left_over;

	// ASSERT(zout_stream.avail_out > 0)

	zout_stream.next_in  = Z_NULL;
	zout_stream.avail_in = 0;

	for (;;)
	{
		int err = deflate(&zout_stream, Z_FINISH);

		if (err == Z_STREAM_END)
			break;

		if (err != Z_OK)
			cur_info->FatalError("Trouble finishing compression (zlib)\n");

		if (zout_stream.avail_out == 0)
		{
			zout_lump->Write(zout_buffer, sizeof(zout_buffer));

			zout_stream.next_out  = zout_buffer;
			zout_stream.avail_out = sizeof(zout_buffer);
		}
	}

	left_over = sizeof(zout_buffer) - zout_stream.avail_out;

	if (left_over > 0)
		zout_lump->Write(zout_buffer, left_over);

	deflateEnd(&zout_stream);

	zout_lump->Finish();
	zout_lump = NULL;
}


/* ---------------------------------------------------------------- */

Lump_c * FindLevelLump(const char *name)
{
	int idx = cur_wad->LevelLookupLump(lev_current_idx, name);

	if (idx < 0)
		return NULL;

	return cur_wad->GetLump(idx);
}


Lump_c * CreateLevelLump(const char *name, int max_size)
{
	// look for existing one
	Lump_c *lump = FindLevelLump(name);

	if (lump)
	{
		cur_wad->RecreateLump(lump, max_size);
	}
	else
	{
		int last_idx = cur_wad->LevelLastLump(lev_current_idx);

		// in UDMF maps, insert before the ENDMAP lump, otherwise insert
		// after the last known lump of the level.
		if (lev_format != MAPF_UDMF)
			last_idx += 1;

		cur_wad->InsertPoint(last_idx);

		lump = cur_wad->AddLump(name, max_size);
	}

	return lump;
}


Lump_c * CreateGLMarker()
{
	char name_buf[64];

	if (strlen(lev_current_name) <= 5)
	{
		sprintf(name_buf, "GL_%s", lev_current_name);

		lev_long_name = false;
	}
	else
	{
		// support for level names longer than 5 letters
		strcpy(name_buf, "GL_LEVEL");

		lev_long_name = true;
	}

	int last_idx = cur_wad->LevelLastLump(lev_current_idx);

	cur_wad->InsertPoint(last_idx + 1);

	Lump_c *marker = cur_wad->AddLump(name_buf);

	marker->Finish();

	return marker;
}


//------------------------------------------------------------------------
// MAIN STUFF
//------------------------------------------------------------------------

buildinfo_t * cur_info = NULL;

void SetInfo(buildinfo_t *info)
{
	cur_info = info;
}


void OpenWad(std::filesystem::path filename)
{
	cur_wad = Wad_file::Open(filename, 'r');
	if (cur_wad == NULL)
		cur_info->FatalError("Cannot open file: %s\n", filename.u8string().c_str());
}

void OpenMem(std::filesystem::path filename, byte *raw_data, int raw_length)
{
	cur_wad = Wad_file::OpenMem(filename, raw_data, raw_length);
	if (cur_wad == NULL)
		cur_info->FatalError("Cannot open file from memory: %s\n", filename.u8string().c_str());
}

void CreateXWA(std::filesystem::path filename)
{
	xwa_wad = Wad_file::Open(filename, 'w');
	if (xwa_wad == NULL)
		cur_info->FatalError("Cannot create file: %s\n", filename.u8string().c_str());

	xwa_wad->BeginWrite();
	xwa_wad->AddLump("XG_START")->Finish();
	xwa_wad->EndWrite();
}


void FinishXWA()
{
	xwa_wad->BeginWrite();
	xwa_wad->AddLump("XG_END")->Finish();
	xwa_wad->EndWrite();
}


void CloseWad()
{
	if (cur_wad != NULL)
	{
		// this closes the file
		delete cur_wad;
		cur_wad = NULL;
	}

	if (xwa_wad != NULL)
	{
		delete xwa_wad;
		xwa_wad = NULL;
	}
}


int LevelsInWad()
{
	if (cur_wad == NULL)
		return 0;

	return cur_wad->LevelCount();
}


const char * GetLevelName(int lev_idx)
{
	SYS_ASSERT(cur_wad != NULL);

	int lump_idx = cur_wad->LevelHeader(lev_idx);

	return cur_wad->GetLump(lump_idx)->Name();
}


/* ----- build nodes for a single level ----- */

build_result_e BuildLevel(int lev_idx)
{
	if (cur_info->cancelled)
		return BUILD_Cancelled;

	node_t   *root_node = NULL;
	subsec_t *root_sub  = NULL;

	lev_current_idx   = lev_idx;
	lev_current_start = cur_wad->LevelHeader(lev_idx);
	lev_format        = cur_wad->LevelFormat(lev_idx);

	LoadLevel();

	InitBlockmap();

	build_result_e ret = BUILD_OK;

	if (num_real_lines > 0)
	{
		bbox_t dummy;

		// create initial segs
		seg_t *list = CreateSegs();

		// recursively create nodes
		ret = BuildNodes(list, 0, &dummy, &root_node, &root_sub);
	}

	if (ret == BUILD_OK)
	{
		cur_info->Print(2, "    Built %d NODES, %d SSECTORS, %d SEGS, %d VERTEXES\n",
				num_nodes, num_subsecs, num_segs, num_old_vert + num_new_vert);

		if (root_node != NULL)
		{
			cur_info->Print(2, "    Heights of subtrees: %d / %d\n",
					ComputeBspHeight(root_node->r.node),
					ComputeBspHeight(root_node->l.node));
		}

		ClockwiseBspTree();

		if (xwa_wad != NULL)
			ret = SaveXWA(root_node);
		else if (lev_format == MAPF_UDMF)
			ret = SaveUDMF(root_node);
		else
			ret = SaveLevel(root_node);
	}
	else
	{
		/* build was Cancelled by the user */
	}

	FreeLevel();

	return ret;
}


}  // namespace ajbsp


//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
