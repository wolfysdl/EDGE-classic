//----------------------------------------------------------------------------
//  EDGE Data Definition File Code (States)
//----------------------------------------------------------------------------
// 
//  Copyright (c) 1999-2023  The EDGE Team.
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
//----------------------------------------------------------------------------

#include "local.h"

#include "states.h"

#include "p_action.h"


static const state_t template_state =
{
	0,          // sprite ref
	0,          // frame ref
	0,          // bright
	0,          // flags
	-1,         // tics

	NULL,		// model_frame
	NULL,       // label
	NULL,       // routine
	NULL,       // parameter
	0,          // rts_tag_type

	0,          // next state ref
	-1          // jump state ref
};

state_t *states = NULL;
int num_states;

std::vector<std::string> ddf_sprite_names;
std::vector<std::string> ddf_model_names;


// Until the DDF_StateFinishState() routine is called, the `nextstate'
// field of each state contains a special value.  0 for normal (no
// redirector).  -1 for the #REMOVE redirector.  Otherwise the top 16
// bits is a redirector, and the bottom 16 bits is a positive offset
// from that redirector (usually 0).
//
// Every time a new redirector is used, it is added to this list.  The
// top 16 bits (minus 1) will be an index into this list of redirector
// names.  These labels will be looked for in the states when the
// fixup routine is called.
static std::vector<std::string> redirs;


#define NUM_SPLIT 10  // Max Number of sections a state is split info

static std::string stateinfo[NUM_SPLIT + 1];


// a little caching makes a big difference here
// (because DDF entries are usually limited to a single sprite)
static int last_sprite = -1;
static int last_model  = -1;


static int AddSpriteName(const char *name)
{
	if (epi::case_cmp(name, "NULL") == 0)
		return SPR_NULL;
	
	if (last_sprite >= 0 &&
		epi::case_cmp(ddf_sprite_names[last_sprite], name) == 0)
		return last_sprite;

	// look backwards, assuming a recent sprite is more likely
	for (int i = (int)ddf_sprite_names.size() - 1; i > SPR_NULL; i--)
		if (epi::case_cmp(ddf_sprite_names[i], name) == 0)
			return ((last_sprite = i));

	last_sprite = (int)ddf_sprite_names.size();

	// not found, so insert it
	ddf_sprite_names.push_back(std::string(name));

	return last_sprite;
}

static int AddModelName(const char *name)
{
	if (epi::case_cmp(name, "NULL") == 0)
		return SPR_NULL;

	if (last_model >= 0 &&
		epi::case_cmp(ddf_model_names[last_model], name) == 0)
		return last_model;

	// look backwards, assuming a recent model is more likely
	for (int i = (int)ddf_model_names.size() - 1; i > SPR_NULL; i--)
		if (epi::case_cmp(ddf_model_names[i], name) == 0)
			return ((last_model = i));

	last_model = (int)ddf_model_names.size();

	// not found, so insert it
	ddf_model_names.push_back(std::string(name));

	return last_model;
}


void DDF_StateInit(void)
{
	// create states array with a single 'S_NULL' state
	states = (state_t*) malloc(sizeof(state_t));
	if (states == NULL)
		I_Error("could not allocate states\n");

	states[0] = template_state;
	num_states = 1;

	// create the 'SPR_NULL' sprite
	// (Not strictly needed, but means we can access the arrays
	//  without subtracting 1)
	AddSpriteName("!NULL!");
	AddModelName ("!NULL!");
}

void DDF_StateCleanUp(void)
{
	/* nothing to do */
}


//
// DDF_MainSplitIntoState
//
// Small procedure that takes the info and splits it into relevant stuff
//
// -KM- 1998/12/21 Rewrote procedure, much cleaner now.
//
// -AJA- 2000/09/03: Rewrote _again_ damn it, in order to handle `:'
//       appearing inside brackets.
//
static int DDF_MainSplitIntoState(const char *info)
{
	char *temp;
	char *first;

	int cur;
	int brackets=0;
	bool done=false;

	// use a buffer, since we modify the string
	char infobuf[512];

	strcpy(infobuf, info);

	for (cur=0; cur < NUM_SPLIT+1; cur++)
		stateinfo[cur] = std::string();

	first = temp = infobuf;

	for (cur=0; !done && cur < NUM_SPLIT; temp++)
	{
		if (*temp == '(')
		{
			brackets++;
			continue;
		}

		if (*temp == ')')
		{
			if (brackets == 0)
				DDF_Error("Mismatched ) bracket in states: %s\n", info);

			brackets--;
			continue;
		}

		if (*temp && *temp != ':')
			continue;

		if (brackets > 0)
			continue;

		if (*temp == 0)
			done = true;

		*temp = 0;

		if (first[0] == '#')
		{
			// signify that we have found redirector
			stateinfo[0] = std::string(first + 1);
			stateinfo[1] = std::string();
			stateinfo[2] = std::string();

			if (! done)
				stateinfo[1] = std::string(temp + 1);

			return -1;
		}

		stateinfo[cur++] = std::string(first);

		first = temp + 1;
	}

	if (brackets > 0)
		DDF_Error("Unclosed ( bracket in states: %s\n", info);

	return cur;
}


//
// DDF_MainSplitActionArg
//
// Small procedure that takes an action like "FOO(BAR)", and splits it
// into two strings "FOO" and "BAR".
//
// -AJA- 1999/08/10: written.
//
static void DDF_MainSplitActionArg(const char *info, char *actname, char *actarg)
{
	int len = strlen(info);

	const char *mid = strchr(info, '(');

	if (mid && len >= 4 && info[len - 1] == ')')
	{
		int len2 = (mid - info);

		Z_StrNCpy(actname, info, len2);

		len -= len2 + 2;
		Z_StrNCpy(actarg, mid + 1, len);
	}
	else
	{
		strcpy(actname, info);
		actarg[0] = 0;
	}
}

//
// StateGetRedirector
//
static int StateGetRedirector(const char *redir)
{
	for (size_t i = 0 ; i < redirs.size() ; i++)
	{
		if (DDF_CompareName(redirs[i].c_str(), redir) == 0)
			return (int) i;
	}

	redirs.push_back(redir);

	return (int)redirs.size() - 1;
}

//
// DDF_StateFindLabel
//
statenum_t DDF_StateFindLabel(const state_group_t& group,
                              const char *label, bool quiet)
{
	for (int g = (int)group.size()-1; g >= 0; g--)
	{
		for (statenum_t i = group[g].last; i >= group[g].first; i--)
		{
			if (! states[i].label)
				continue;

			if (DDF_CompareName(states[i].label, label) == 0)
				return i;
		}
	}

	// compatibility hack:
	if (DDF_CompareName(label, "IDLE") == 0)
	{
		return DDF_StateFindLabel(group, "SPAWN");
	}

	if (! quiet)
		DDF_Error("Unknown label '%s' (object has no such frames).\n", label);

	return S_NULL;
}

//
// DDF_StateReadState
//
void DDF_StateReadState(const char *info, const char *label,
						state_group_t& group, int *state_num, int index,
						const char *redir, const actioncode_t *action_list,
						bool is_weapon)
{
	SYS_ASSERT(group.size() > 0);

	state_range_t& range = group.back();

	int i, j;

	char action_name[128];
	char action_arg[128];

	state_t *cur;

	// Split the state info into component parts
	// -ACB- 1998/07/26 New Procedure, for cleaner code.

	i = DDF_MainSplitIntoState(info);
	if (i < 5 && i >= 0)
	{
		if (strchr(info, '['))
		{
			// -ES- 2000/02/02 Probably unterminated state.
			DDF_Error("DDF_MainLoadStates: Bad state '%s', possibly missing ';'\n", info);
		}
		DDF_Error("Bad state '%s'\n", info);
	}

	if (stateinfo[0].empty())
		DDF_Error("Missing sprite in state frames: `%s'\n", info);

	//--------------------------------------------------
	//----------------REDIRECTOR HANDLING---------------
	//--------------------------------------------------

	if (stateinfo[2].empty())
	{
		if (! range.first)
			DDF_Error("Redirector used without any states (`%s')\n", info);

		cur = &states[range.last];

		SYS_ASSERT(! stateinfo[0].empty());

		if (DDF_CompareName(stateinfo[0].c_str(), "REMOVE") == 0)
		{
			cur->nextstate = -1;
			return;
		}  

		cur->nextstate = (StateGetRedirector(stateinfo[0].c_str()) + 1) << 16;

		if (! stateinfo[1].empty())
			cur->nextstate += MAX(0, atoi(stateinfo[1].c_str()) - 1);

		return;
	}

	//--------------------------------------------------
	//---------------- ALLOCATE NEW STATE --------------
	//--------------------------------------------------

	num_states += 1;

	states = (state_t*) realloc(states, num_states * sizeof(state_t));
	if (states == NULL)
		I_Error("could not allocate states\n");

	cur = &states[num_states-1];

	// initialise with defaults
	cur[0] = template_state;
  
	if (range.first == 0)
	{
		// very first state for thing/weapon
		range.first = num_states-1;
	}

	range.last = num_states-1;
  
	if (index == 0)
	{
		// first state in this set of states
		if (state_num)
			state_num[0] = num_states-1;
    
		// ...therefore copy the label
		cur->label = strdup(label);
	}

	if (redir && cur->nextstate == 0)
	{
		if (DDF_CompareName("REMOVE", redir) == 0)
			cur->nextstate = -1;
		else
			cur->nextstate = (StateGetRedirector(redir) + 1) << 16;
	}

	//--------------------------------------------------
	//----------------SPRITE NAME HANDLING--------------
	//--------------------------------------------------

	if (stateinfo[1].empty() || stateinfo[2].empty() || stateinfo[3].empty())
		DDF_Error("Bad state frame, missing fields: %s\n", info);
  

	//--------------------------------------------------
	//--------------SPRITE INDEX HANDLING---------------
	//--------------------------------------------------

	cur->flags = 0;

	// look at the first character
	const char *sprite_x = stateinfo[1].c_str();

	j = sprite_x[0];

	if ('A' <= j && j <= ']')
	{
		cur->frame = j - (int)'A';
	}
	else if (j == '@')
	{
		cur->frame = -1;

		char first_ch = sprite_x[1];

		if (isdigit(first_ch))
		{
			cur->flags = SFF_Model;
			cur->frame = atol(sprite_x+1) - 1;
		}
		else if (isalpha(first_ch) || (first_ch == '_'))
		{
			cur->flags = SFF_Model | SFF_Unmapped;
			cur->frame = 0;
			cur->model_frame = strdup(sprite_x+1);
		}

		if (cur->frame < 0)
			DDF_Error("DDF_MainLoadStates: Illegal model frame: %s\n", sprite_x);
	}
	else
		DDF_Error("DDF_MainLoadStates: Illegal sprite frame: %s\n", sprite_x);

	if (is_weapon)
		cur->flags |= SFF_Weapon;

	if (cur->flags & SFF_Model)
		cur->sprite = AddModelName(stateinfo[0].c_str());
	else
		cur->sprite = AddSpriteName(stateinfo[0].c_str());

	//--------------------------------------------------
	//------------STATE TIC COUNT HANDLING--------------
	//--------------------------------------------------

	cur->tics = atol(stateinfo[2].c_str());

	//--------------------------------------------------
	//------------STATE BRIGHTNESS LEVEL----------------
	//--------------------------------------------------

	if (epi::strcmp(stateinfo[3].c_str(), "NORMAL") == 0)
		cur->bright = 0;
	else if (epi::strcmp(stateinfo[3].c_str(), "BRIGHT") == 0)
		cur->bright = 255;
	else if (epi::prefix_cmp(stateinfo[3].c_str(), "LIT") == 0)
	{
		cur->bright = strtol(stateinfo[3].c_str()+3, NULL, 10);
		cur->bright = CLAMP(0, cur->bright * 255 / 99, 255);
	}
	else
		DDF_WarnError("DDF_MainLoadStates: Lighting is not BRIGHT or NORMAL\n");

	//--------------------------------------------------
	//------------STATE ACTION CODE HANDLING------------
	//--------------------------------------------------

	if (! stateinfo[4].empty())
	{
		// Get Action Code Ref (Using remainder of the string).
		// Go through all the actions, end if terminator or action found
		//
		// -AJA- 1999/08/10: Updated to handle a special argument.

		DDF_MainSplitActionArg(stateinfo[4].c_str(), action_name, action_arg);

		for (i=0; action_list[i].actionname; i++)
		{
			const char *current = action_list[i].actionname;

			if (current[0] == '!')
				current++;
    
			if (DDF_CompareName(current, action_name) == 0)
				break;
		}

		if (!action_list[i].actionname)
		{
			DDF_WarnError("Unknown code pointer: %s\n", stateinfo[4].c_str());
		}
		else
		{
			cur->action = action_list[i].action;
			cur->action_par = NULL;

			if (action_list[i].handle_arg)
				(* action_list[i].handle_arg)(action_arg, cur);
		}
	}
}


bool DDF_MainParseState(byte *object, state_group_t& group,
						const char *field, const char *contents,
						int index, bool is_last, bool is_weapon,
						const state_starter_t *starters,
						const actioncode_t *actions)
{
	if (epi::prefix_case_cmp(field, "STATES(") != 0)
		return false;

	// extract label name
	field += 7;

	const char *pos = strchr(field, ')');

	if (pos == NULL || pos == field || (pos - field) > 64)
		return false;

	std::string labname(field, pos - field);

	// check for the "standard" states
	int i;
	for (i=0; starters[i].label; i++)
		if (DDF_CompareName(starters[i].label, labname.c_str()) == 0)
			break;

	const state_starter_t *starter = NULL;
	if (starters[i].label)
		starter = &starters[i];

	int * var = NULL;
	if (starter)
		var = (int *)(object + starter->offset);

	const char *redir = NULL;
	if (is_last)
		redir = starter ? starter->last_redir : (is_weapon ? "READY" : "IDLE");

	DDF_StateReadState(contents, labname.c_str(), group, var, index,
					   redir, actions, is_weapon);
	return true;
}


void DDF_StateBeginRange(state_group_t& group)
{
	state_range_t range;

	range.first = S_NULL;
	range.last  = S_NULL;

	group.push_back(range);
}


//
// DDF_StateFinishRange
//
// Check through the states on an mobj and attempts to dereference any
// encoded state redirectors.
//
void DDF_StateFinishRange(state_group_t& group)
{
	SYS_ASSERT(! group.empty());

	state_range_t& range = group.back();

	// if no states were added, remove the unused range
	if (range.first == S_NULL)
	{
		group.pop_back();

		redirs.clear();
		return;
	}

	for (int i = range.first; i <= range.last; i++)
	{
		// handle next state ref
		if (states[i].nextstate == -1)
		{
			states[i].nextstate = S_NULL;
		}
		else if ((states[i].nextstate >> 16) == 0)
		{
			states[i].nextstate = (i == range.last) ? S_NULL : i+1;
		}
		else
		{
			int RI = (states[i].nextstate >> 16) - 1;

			// FIXME: is this validated anywhere?
			states[i].nextstate = DDF_StateFindLabel(group, redirs[RI].c_str()) +
				(states[i].nextstate & 0xFFFF);
		}

		// handle jump state ref
		if (states[i].jumpstate == -1)
		{
			states[i].jumpstate = S_NULL;
		}
		else if ((states[i].jumpstate >> 16) == 0)
		{
			states[i].jumpstate = (i == range.last) ? S_NULL : i+1;
		}
		else
		{
			int RI = (states[i].jumpstate >> 16) - 1;

			states[i].jumpstate = DDF_StateFindLabel(group, redirs[RI].c_str()) +
				(states[i].jumpstate & 0xFFFF);
		}
	}
  
	redirs.clear();
}


bool DDF_StateGroupHasState(const state_group_t& group, statenum_t st)
{
	for (int g = 0; g < (int)group.size(); g++)
	{
		const state_range_t& range = group[g];

		if (range.first <= st && st <= range.last)
			return true;
	}

	return false;
}


//----------------------------------------------------------------------------

//
// DDF_StateGetAttack
//
// Parse the special argument for the state as an attack.
//
// -AJA- 1999/08/10: written.
//
void DDF_StateGetAttack(const char *arg, state_t * cur_state)
{
	if (!arg || !arg[0])
		return;

	cur_state->action_par = (void *)atkdefs.Lookup(arg);
	if (cur_state->action_par == NULL)
		DDF_WarnError("Unknown Attack (States): %s\n", arg);
}


void DDF_StateGetMobj(const char *arg, state_t * cur_state)
{
	if (!arg || !arg[0])
		return;

	cur_state->action_par = new mobj_strref_c(arg);
}


void DDF_StateGetSound(const char *arg, state_t * cur_state)
{
	if (!arg || !arg[0])
		return;

	cur_state->action_par = (void *)sfxdefs.GetEffect(arg);
}


void DDF_StateGetInteger(const char *arg, state_t * cur_state)
{
	if (!arg || !arg[0])
		return;

	int *val_ptr = new int;

	if (sscanf(arg, " %i ", val_ptr) != 1)
		DDF_Error("DDF_StateGetInteger: bad value: %s\n", arg);

	cur_state->action_par = val_ptr;
}


void DDF_StateGetIntPair(const char *arg, state_t * cur_state)
{
	// Parses two integers separated by commas.
	//
	int *values;

	if (!arg || !arg[0])
		return;

	values = new int[2];

	if (sscanf(arg, " %i , %i ", &values[0], &values[1]) != 2)
		DDF_Error("DDF_StateGetIntPair: bad values: %s\n", arg);

	cur_state->action_par = values;
}


void DDF_StateGetFloat(const char *arg, state_t * cur_state)
{
	if (!arg || !arg[0])
		return;

	float *val_ptr = new float;

	if (sscanf(arg, " %f ", val_ptr) != 1)
		DDF_Error("DDF_StateGetFloat: bad value: %s\n", arg);

	cur_state->action_par = val_ptr;
}


void DDF_StateGetPercent(const char *arg, state_t * cur_state)
{
	if (!arg || !arg[0])
		return;

	percent_t *val_ptr = new percent_t;

	if (sscanf(arg, " %f%% ", val_ptr) != 1 || (*val_ptr) < 0)
		DDF_Error("DDF_StateGetPercent: Bad percentage: %s\n", arg);

	(* val_ptr) /= 100.0f;

	cur_state->action_par = val_ptr;
}


act_jump_info_s::act_jump_info_s() :
	chance(PERCENT_MAKE(100)) // -ACB- 2001/02/04 tis a precent_t
{ }

act_jump_info_s::~act_jump_info_s()
{ }

void DDF_StateGetJump(const char *arg, state_t * cur_state)
{
	// JUMP(label)
	// JUMP(label,chance)
	
	// Dasho 2023.10.16 - Changed to allow negative percentages to use for
	// special values (A_RefireTo ammo check, etc)
  
	if (!arg || !arg[0])
		return;

	act_jump_info_t *jump = new act_jump_info_t;

	int len;
	int offset = 0;

	const char *s = strchr(arg, ',');

	if (! s)
	{
		len = strlen(arg);
	}
	else
	{
		// convert chance value
		DDF_MainGetPercentAny(s+1, &jump->chance);
    
		len = s - arg;
	}

	if (len == 0)
		DDF_Error("DDF_StateGetJump: missing label!\n");

	if (len > 75)
		DDF_Error("DDF_StateGetJump: label name too long!\n");

	// copy label name
	char buffer[80];

	for (len=0; *arg && (*arg != ':') && (*arg != ','); len++, arg++)
		buffer[len] = *arg;

	buffer[len] = 0;

	if (*arg == ':')
		offset = MAX(0, atoi(arg+1) - 1);

	// set the jump state   
	cur_state->jumpstate = ((StateGetRedirector(buffer) + 1) << 16) + offset;
	cur_state->action_par = jump;
}


void DDF_StateGetFrame(const char *arg, state_t * cur_state)
{
	// Sets the jump_state, like DDF_StateGetJump above.
	//
	// ACTION(label)
  
	if (!arg || !arg[0])
		return;

	int len;
	int offset = 0;

	// copy label name
	char buffer[80];

	for (len = 0; *arg && (*arg != ':'); len++, arg++)
		buffer[len] = *arg;

	buffer[len] = 0;

	if (*arg == ':')
		offset = MAX(0, atoi(arg+1) - 1);

	// set the jump state   
	cur_state->jumpstate = ((StateGetRedirector(buffer) + 1) << 16) + offset;
}


act_morph_info_s::act_morph_info_s() :
	info(NULL), info_ref(), start()
{ }

act_morph_info_s::~act_morph_info_s()
{ }

void DDF_StateGetMorph(const char *arg, state_t * cur_state)
{
	// MORPH(typename)
	// MORPH(typename,label)
  
	if (!arg || !arg[0])
		return;

	act_morph_info_t *morph = new act_morph_info_t;

	morph->start.label = "IDLE";

	const char *s = strchr(arg, ',');

	// get type name
	char buffer[80];

	int len = s ? (s - arg) : strlen(arg);

	if (len == 0)
		DDF_Error("DDF_StateGetMorph: missing type name!\n");

	if (len > 75)
		DDF_Error("DDF_StateGetMorph: type name too long!\n");

	for (len=0; *arg && (*arg != ':') && (*arg != ','); len++, arg++)
		buffer[len] = *arg;

	buffer[len] = 0;

	morph->info_ref = buffer;

	
	// get start label (if present)
	if (s)
	{
		s++;

		len = strlen(s);

		if (len == 0)
			DDF_Error("DDF_StateGetMorph: missing label!\n");

		if (len > 75)
			DDF_Error("DDF_StateGetMorph: label too long!\n");

		for (len=0; *s && (*s != ':') && (*s != ','); len++, s++)
			buffer[len] = *s;

		buffer[len] = 0;

		morph->start.label = buffer;

		if (*s == ':')
			morph->start.offset = MAX(0, atoi(s+1) - 1);

	}

	cur_state->action_par = morph;
}


act_become_info_s::act_become_info_s() :
	info(NULL), info_ref(), start()
{ }

act_become_info_s::~act_become_info_s()
{ }

void DDF_StateGetBecome(const char *arg, state_t * cur_state)
{
	// BECOME(typename)
	// BECOME(typename,label)
  
	if (!arg || !arg[0])
		return;

	act_become_info_t *become = new act_become_info_t;

	become->start.label = "IDLE";

	const char *s = strchr(arg, ',');

	// get type name
	char buffer[80];

	int len = s ? (s - arg) : strlen(arg);

	if (len == 0)
		DDF_Error("DDF_StateGetBecome: missing type name!\n");

	if (len > 75)
		DDF_Error("DDF_StateGetBecome: type name too long!\n");

	for (len=0; *arg && (*arg != ':') && (*arg != ','); len++, arg++)
		buffer[len] = *arg;

	buffer[len] = 0;

	become->info_ref = buffer;

	
	// get start label (if present)
	if (s)
	{
		s++;

		len = strlen(s);

		if (len == 0)
			DDF_Error("DDF_StateGetBecome: missing label!\n");

		if (len > 75)
			DDF_Error("DDF_StateGetBecome: label too long!\n");

		for (len=0; *s && (*s != ':') && (*s != ','); len++, s++)
			buffer[len] = *s;

		buffer[len] = 0;

		become->start.label = buffer;

		if (*s == ':')
			become->start.offset = MAX(0, atoi(s+1) - 1);

	}

	cur_state->action_par = become;
}


wep_become_info_s::wep_become_info_s() :
	info(NULL), info_ref(), start()
{ }

wep_become_info_s::~wep_become_info_s()
{ }

void DDF_StateGetBecomeWeapon(const char *arg, state_t * cur_state)
{
	// BECOME(typename)
	// BECOME(typename,label)
  
	if (!arg || !arg[0])
		return;

	wep_become_info_t *become = new wep_become_info_t;

	become->start.label = "READY";

	const char *s = strchr(arg, ',');

	// get type name
	char buffer[80];

	int len = s ? (s - arg) : strlen(arg);

	if (len == 0)
		DDF_Error("DDF_StateGetBecomeWeapon: missing type name!\n");

	if (len > 75)
		DDF_Error("DDF_StateGetBecomeWeapon: type name too long!\n");

	for (len=0; *arg && (*arg != ':') && (*arg != ','); len++, arg++)
		buffer[len] = *arg;

	buffer[len] = 0;

	become->info_ref = buffer;

	
	// get start label (if present)
	if (s)
	{
		s++;

		len = strlen(s);

		if (len == 0)
			DDF_Error("DDF_StateGetBecomeWeapon: missing label!\n");

		if (len > 75)
			DDF_Error("DDF_StateGetBecomeWeapon: label too long!\n");

		for (len=0; *s && (*s != ':') && (*s != ','); len++, s++)
			buffer[len] = *s;

		buffer[len] = 0;

		become->start.label = buffer;

		if (*s == ':')
			become->start.offset = MAX(0, atoi(s+1) - 1);

	}

	cur_state->action_par = become;
}


void DDF_StateGetAngle(const char *arg, state_t * cur_state)
{
	angle_t *value;
	float tmp;

	if (!arg || !arg[0])
		return;

	value = new angle_t;

	if (sscanf(arg, " %f ", &tmp) != 1)
		DDF_Error("DDF_StateGetAngle: bad value: %s\n", arg);

	*value = FLOAT_2_ANG(tmp);

	cur_state->action_par = value;
}


void DDF_StateGetSlope(const char *arg, state_t * cur_state)
{
	float *value, tmp;

	if (!arg || !arg[0])
		return;

	value = new float;

	if (sscanf(arg, " %f ", &tmp) != 1)
		DDF_Error("DDF_StateGetSlope: bad value: %s\n", arg);

	if (tmp > +89.5f)
		tmp = +89.5f;
	if (tmp < -89.5f)
		tmp = -89.5f;

	*value = tan(tmp * M_PI / 180.0);

	cur_state->action_par = value;
}


void DDF_StateGetRGB(const char *arg, state_t * cur_state)
{
	if (!arg || !arg[0])
		return;

	cur_state->action_par = new rgbcol_t;

	DDF_MainGetRGB(arg, cur_state->action_par);
}


//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
