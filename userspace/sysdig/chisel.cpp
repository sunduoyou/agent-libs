#include <iostream>
#include <fstream>
#include <algorithm> 
#include <functional> 
#include <cctype>
#include <locale>

#include <sinsp.h>
#include "sysdig.h"
#include "chisel.h"

///////////////////////////////////////////////////////////////////////////////
// String helpers
///////////////////////////////////////////////////////////////////////////////
//
// trim from start
//
string& ltrim(string &s) 
{
	s.erase(s.begin(), find_if(s.begin(), s.end(), not1(ptr_fun<int, int>(isspace))));
	return s;
}

//
// trim from end
//
string& rtrim(string &s) 
{
	s.erase(find_if(s.rbegin(), s.rend(), not1(ptr_fun<int, int>(isspace))).base(), s.end());
	return s;
}

//
// trim from both ends
//
string& trim(string &s) 
{
	return ltrim(rtrim(s));
}

void replace_in_place(string &s, const string &search, const string &replace)
{
    for(size_t pos = 0; ; pos += replace.length()) 
	{
        // Locate the substring to replace
        pos = s.find(search, pos);
        if(pos == string::npos ) break;
        // Replace by erasing and inserting
        s.erase(pos, search.length());
        s.insert(pos, replace );
    }
}

void replace_in_place(string& str, string& substr_to_replace, string& new_substr) 
{
	uint32_t index = 0;
	uint32_t nsize = new_substr.size();

	while (true) 
	{
		 index = str.find(substr_to_replace, index);
		 if (index == string::npos) break;

		 str.replace(index, nsize, new_substr);

		 index += nsize;
	}
}

///////////////////////////////////////////////////////////////////////////////
// chiselinfo implementation
///////////////////////////////////////////////////////////////////////////////
chiselinfo::chiselinfo()
{
	m_filter = NULL;
	m_formatter = NULL;
}

void chiselinfo::init(sinsp* inspector, string filterstr, string formatterstr)
{
	if(filterstr != "")
	{
		m_filter = new sinsp_filter(inspector, filterstr);
	}

	if(formatterstr != "")
	{
		m_formatter = new sinsp_evt_formatter(inspector, formatterstr);
	}
	else
	{
		m_formatter = new sinsp_evt_formatter(inspector, DEFAULT_OUTPUT_STR);
	}
}

chiselinfo::~chiselinfo()
{
	if(m_filter)
	{
		delete m_filter;
	}

	if(m_formatter)
	{
		delete m_formatter;
	}
}

///////////////////////////////////////////////////////////////////////////////
// chisel implementation
///////////////////////////////////////////////////////////////////////////////
chisel::chisel(sinsp* inspector, string filename)
{
	m_inspector = inspector;
	load(filename);
}

chisel::~chisel()
{
	for(chiselinfo* ch : m_subchisels) 
	{
		delete ch;
	}
}

void chisel::load(string cmdstr)
{
	string filename;
	trim(cmdstr);

	if(cmdstr[cmdstr.size() - 1] == ')')
	{
		if(cmdstr.find('(') == string::npos)
		{
			throw sinsp_exception("invalid chisel invoke syntax " + cmdstr);
		}

		filename = cmdstr.substr(0, cmdstr.find('('));
		trim(filename);

		string args = cmdstr.substr(cmdstr.find('('), string::npos);
		args = args.substr(1, string::npos);
		args = args.substr(0, args.size() - 1);

		char *tok = strtok((char*)args.c_str(), ",");
		while(tok) 
		{
			string tokstr(tok);
			trim(tokstr);
			m_argvals.push_back(tokstr);
			tok = strtok(NULL, ",");
		}	
	}
	else
	{
		filename = cmdstr;
	}

	ifstream is(filename);

	if(is.is_open())
	{
		uint32_t j, k;

		//
		// Bring the file into a string
		//
		string docstr((istreambuf_iterator<char>(is)),
			istreambuf_iterator<char>());

		//
		// Parse the json
		//
		Json::Reader reader;
		bool parsingSuccessful = reader.parse(docstr, m_root);
		if(!parsingSuccessful)
		{
			throw sinsp_exception("Failed to parse chisel " + filename + ":" + 
				reader.getFormattedErrorMessages());
		}

		//
		// Extract the info
		//
		m_description = m_root["info"]["description"].asString();
		const Json::Value args = m_root["info"]["arguments"];
		
		//
		// Validate the arguments
		//
		if(m_argvals.size() != args.size())
		{
			throw sinsp_exception("wrong number of parameters for chisel " + filename);
		}

		//
		// Extract the info
		//
		const Json::Value clst = m_root["chisels"];
		
		for(j = 0; j < clst.size(); j++)
		{
			string filter = clst[j]["filter"].asString();
			for(k = 0; k < args.size(); k++)
			{
				replace_in_place(filter, 
					string("$") + args[k]["name"].asString(), 
					string(m_argvals[k]));
			}

			string formatter = clst[j]["format"].asString();
			for(k = 0; k < args.size(); k++)
			{
				replace_in_place(formatter, 
					string("$") + args[k]["name"].asString(), 
					string(m_argvals[k]));
			}

			chiselinfo* ci = new chiselinfo();
			ci->init(m_inspector, filter, formatter);
			m_subchisels.push_back(ci);
		}

		is.close();
	}
	else
	{
		throw sinsp_exception("can't open file " + filename);
	}
}

void chisel::run(sinsp_evt* evt)
{
	uint32_t j;
	string line;

	for(j = 0; j < m_subchisels.size(); j++)
	{
		//
		// Output the line
		//
		if(m_subchisels[j]->m_filter != NULL)
		{
			if(!m_subchisels[j]->m_filter->run(evt))
			{
				continue;
			}
		}

		if(m_subchisels[j]->m_formatter->tostring(evt, &line))
		{
			cout << line << endl;
		}
	}
}
