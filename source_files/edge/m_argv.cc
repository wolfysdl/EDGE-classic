//------------------------------------------------------------------------
//  EDGE Arguments/Parameters Code
//------------------------------------------------------------------------
//
//  Copyright (C) 2023 The EDGE Team
//  Copyright (C) 2021-2022 The OBSIDIAN Team
//  Copyright (C) 2006-2017 Andrew Apted
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

#include "i_defs.h"
#include "m_argv.h"
#include "str_util.h"
#include "filesystem.h"
#ifdef _WIN32
#include <shellapi.h>
#include <processenv.h>
#endif

std::vector<std::string> argv::list;
#ifdef _WIN32
std::unordered_map<std::string, std::string> env::list;
#endif

// this one is here to avoid infinite recursion of param files.
typedef struct added_parm_s
{
	std::filesystem::path name;
	struct added_parm_s *next;
}
added_parm_t;

static added_parm_t *added_parms;

//
// ArgvInit
//
// Initialise argument list. The strings (and array) are copied.
//
// NOTE: doesn't merge multiple uses of an option, hence
//       using ArgvFind() will only return the first usage.
//
#ifdef _WIN32
void argv::Init(const int argc, const char *const *argv) 
{
	(void)argc;
	(void)argv;

	int win_argc = 0;
	size_t i;
	wchar_t **win_argv = CommandLineToArgvW(GetCommandLineW(), &win_argc);

	if (!win_argv)
		I_Error("argv::Init: Could not retrieve command line arguments!\n");

    list.reserve(win_argc);
    SYS_ASSERT(argv::list.size() >= 0);

	std::vector<std::string> argv_block;

    for (i = 0; i < win_argc; i++) 
	{
        SYS_ASSERT(win_argv[i] != nullptr);
		std::u16string arg = reinterpret_cast<char16_t *>(win_argv[i]);
		argv_block.push_back(epi::to_u8string(arg));
    }

	LocalFree(win_argv);

	for (i = 0; i < argv_block.size(); i++)
	{
		// Just place argv[0] as is
        if (i == 0)
        {
            list.emplace_back(argv_block[i]);
            continue;
        }

        if (argv_block[i][0] == '@')
        {  // add it as a response file
            ApplyResponseFile(&argv_block[i][1]);
            continue;
        }

        list.emplace_back(argv_block[i]);
	}
}
#else
void argv::Init(const int argc, const char *const *argv) 
{
    list.reserve(argc);
    SYS_ASSERT(argv::list.size() >= 0);

    for (int i = 0; i < argc; i++) 
	{
        SYS_ASSERT(argv[i] != nullptr);

#ifdef __APPLE__
        // ignore MacOS X rubbish
        if (argv[i] == "-psn")
            continue;
#endif
        // Just place argv[0] as is
        if (i == 0)
        {
            list.emplace_back(argv[i]);
            continue;
        }

        if (argv[i][0] == '@')
        {  // add it as a response file
            ApplyResponseFile(&argv[i][1]);
            continue;
        }

        list.emplace_back(argv[i]);
    }
}
#endif

int argv::Find(std::string longName, int *numParams) 
{
    SYS_ASSERT(!longName.empty());

    if (numParams) 
        *numParams = 0;

    size_t p = 0;

    for (; p < list.size(); ++p) 
	{
        if (!IsOption(p))
            continue;

        const std::string &str = list[p];

        if (epi::case_cmp(longName,
                std::string{&str[1], (str.size() - 1)}) == 0)
            break;
    }

    if (p == list.size())
        return -1;

    if (numParams) 
	{
        size_t q = p + 1;

        while (q < list.size() && !IsOption(q))
            ++q;

        *numParams = q - p - 1;
    }

    return p;
}

std::string argv::Value(std::string longName, int *numParams) 
{
    SYS_ASSERT(!longName.empty());

    int pos = Find(longName, numParams);

    if (pos <= 0)
        return "";

    if (pos + 1 < list.size() && !IsOption(pos+1))
        return list[pos+1];
    else
        return "";
}

// Sets boolean variable to true if parm (prefixed with `-') is
// present, sets it to false if parm prefixed with `-no' is present,
// otherwise leaves it unchanged.
//
void argv::CheckBooleanParm(std::string parm, bool *boolval, bool reverse)
{
	if (Find(parm) > 0)
	{
		*boolval = ! reverse;
		return;
	}

	if (Find(epi::STR_Format("no%s", parm.c_str())) > 0)
	{
		*boolval = reverse;
		return;
	}
}

void argv::CheckBooleanCVar(std::string parm, cvar_c *var, bool reverse)
{
	if (Find(parm) > 0)
	{
		*var = (reverse ? 0 : 1);
		return;
	}

	if (Find(epi::STR_Format("no%s", parm.c_str())) > 0)
	{
		*var = (reverse ? 1 : 0);
		return;
	}
}

static int ParseOneFilename(FILE *fp, char *buf)
{
	// -AJA- 2004/08/30: added this routine, in order to handle
	// filenames with spaces (which must be double-quoted).

	int ch;

	// skip whitespace
	do
	{
		ch = fgetc(fp);

		if (ch == EOF)
			return EOF;
	}
	while (isspace(ch));

	bool quoting = false;

	for (;;)
	{
		if (ch == '"')
		{
			quoting = ! quoting;
			ch = fgetc(fp);
			continue;
		}

		if (ch == EOF || (isspace(ch) && ! quoting))
			break;

		*buf++ = ch;

		ch = fgetc(fp);
	}

	*buf++ = 0;

	return 0;
}

//
// Adds a response file
//
void argv::ApplyResponseFile(std::filesystem::path name)
{
	char buf[1024];
	FILE *f;
	added_parm_t this_parm;
	added_parm_t *p;

	// check if the file has already been added
	for (p = added_parms; p; p = p->next)
	{
		if (p->name.compare(name) == 0)
			return;
	}

	// mark that this file has been added
	this_parm.name = name;
	p = this_parm.next = added_parms;

	// add arguments from the given file
	f = EPIFOPEN(name, "rb");
	if (!f)
		I_Error("Couldn't open \"%s\" for reading!", name.u8string().c_str());

	for (; EOF != ParseOneFilename(f, buf);)
    {
#ifdef _WIN32
		// Can't really guarantee that a response file will have a certain encoding,
		// so try to detect paths in the response file and make them UTF-8
		std::filesystem::path path_check = buf;
		if (std::filesystem::exists(path_check))
		{
			list.push_back(path_check.u8string());
		}
		else
		{
			path_check = std::filesystem::u8path(buf);
			if (std::filesystem::exists(path_check))
			{
				list.push_back(path_check.u8string());
			}
			else
				list.push_back(strdup(buf));
		}
#else
        list.push_back(strdup(buf));
#endif
    }

	// unlink from list
	added_parms = p;

	fclose(f);
}

void argv::DebugDumpArgs(void)
{
	I_Printf("Command-line Options:\n");

	int i = 0;

	while (i < list.size())
	{
		bool pair_it_up = false;

		if (i>0 && i+1 < list.size() && !IsOption(i+1))
			pair_it_up = true;

		I_Printf("  %s %s\n", list[i].c_str(), pair_it_up ? list[i+1].c_str() : "");

		i += pair_it_up ? 2 : 1;
	}
}

bool argv::IsOption(const int index) { return list.at(index)[0] == '-'; }

#ifdef _WIN32
//
// EnvInit
//
// Initialise environment variable list. The strings are copied.
//
//
void env::Init() 
{

	wchar_t *win_env = GetEnvironmentStringsW();

	if (!win_env)
		I_Error("env::Init: Could not retrieve environment variables!\n");

	std::vector<std::string> block_vars;
	std::u16string raw_var;

	wchar_t *orig_env = win_env; // For cleanup

	for (; win_env; win_env++)
	{
		if (*win_env == '\0')
		{
			if (win_env+1 != nullptr && *(win_env+1) == '\0')
				break;
			else
			{
				block_vars.push_back(epi::to_u8string(raw_var));
				raw_var.clear();
			}
		}
		else
			raw_var.push_back(*reinterpret_cast<char16_t *>(win_env));
	}

	FreeEnvironmentStringsW(orig_env);

	for (auto var : block_vars)
	{
		std::vector<std::string> kv = epi::STR_SepStringVector(var, '=');
		if (kv.size() < 2) // Sanity check
			continue;
		else
			list.try_emplace(kv[0], kv[1]);
	}

}

std::string env::Value(std::string key) 
{
	for (auto var : list)
	{
		if (epi::case_cmp(key, var.first) == 0)
			return var.second;
	}
	return "";
}
#endif

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
