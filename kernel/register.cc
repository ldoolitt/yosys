/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/compatibility.h"
#include "kernel/register.h"
#include "kernel/log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

using namespace REGISTER_INTERN;
#define MAX_REG_COUNT 1000

namespace REGISTER_INTERN
{
	bool echo_mode = false;
	Pass *first_queued_pass;

	std::map<std::string, Frontend*> frontend_register;
	std::map<std::string, Pass*> pass_register;
	std::map<std::string, Backend*> backend_register;
}

std::vector<std::string> Frontend::next_args;

Pass::Pass(std::string name, std::string short_help) : pass_name(name), short_help(short_help)
{
	next_queued_pass = first_queued_pass;
	first_queued_pass = this;
	call_counter = 0;
}

void Pass::run_register()
{
	log_assert(pass_register.count(pass_name) == 0);
	pass_register[pass_name] = this;
}

void Pass::init_register()
{
	while (first_queued_pass) {
		first_queued_pass->run_register();
		first_queued_pass = first_queued_pass->next_queued_pass;
	}
}

void Pass::done_register()
{
	frontend_register.clear();
	pass_register.clear();
	backend_register.clear();
	log_assert(first_queued_pass == NULL);
}

Pass::~Pass()
{
}

void Pass::help()
{
	log("\n");
	log("No help message for command `%s'.\n", pass_name.c_str());
	log("\n");
}

void Pass::cmd_log_args(const std::vector<std::string> &args)
{
	if (args.size() <= 1)
		return;
	log("Full command line:");
	for (size_t i = 0; i < args.size(); i++)
		log(" %s", args[i].c_str());
	log("\n");
}

void Pass::cmd_error(const std::vector<std::string> &args, size_t argidx, std::string msg)
{
	std::string command_text;
	int error_pos = 0;

	for (size_t i = 0; i < args.size(); i++) {
		if (i < argidx)
			error_pos += args[i].size() + 1;
		command_text = command_text + (command_text.empty() ? "" : " ") + args[i];
	}

	log("\nSyntax error in command `%s':\n", command_text.c_str());
	help();

	log_cmd_error("Command syntax error: %s\n> %s\n> %*s^\n",
			msg.c_str(), command_text.c_str(), error_pos, "");
}

void Pass::extra_args(std::vector<std::string> args, size_t argidx, RTLIL::Design *design, bool select)
{
	for (; argidx < args.size(); argidx++)
	{
		std::string arg = args[argidx];

		if (arg.substr(0, 1) == "-")
			cmd_error(args, argidx, "Unkown option or option in arguments.");

		if (!select)
			cmd_error(args, argidx, "Extra argument.");

		handle_extra_select_args(this, args, argidx, args.size(), design);
		break;
	}
	// cmd_log_args(args);
}

void Pass::call(RTLIL::Design *design, std::string command)
{
	std::vector<std::string> args;
	char *s = strdup(command.c_str()), *sstart = s, *saveptr;
	s += strspn(s, " \t\r\n");
	if (*s == 0 || *s == '#') {
		free(sstart);
		return;
	}
	if (*s == '!') {
		for (s++; *s == ' ' || *s == '\t'; s++) { }
		char *p = s + strlen(s) - 1;
		while (p >= s && (*p == '\r' || *p == '\n'))
			*(p--) = 0;
		log_header("Shell command: %s\n", s);
		int retCode = system(s);
		if (retCode != 0)
			log_cmd_error("Shell command returned error code %d.\n", retCode);
		free(sstart);
		return;
	}
	for (char *p = strtok_r(s, " \t\r\n", &saveptr); p; p = strtok_r(NULL, " \t\r\n", &saveptr)) {
		std::string str = p;
		int strsz = str.size();
		if (str == "#")
			break;
		if (strsz > 0 && str[strsz-1] == ';') {
			int num_semikolon = 0;
			while (strsz > 0 && str[strsz-1] == ';')
				strsz--, num_semikolon++;
			if (strsz > 0)
				args.push_back(str.substr(0, strsz));
			call(design, args);
			args.clear();
			if (num_semikolon == 2)
				call(design, "clean");
			if (num_semikolon == 3)
				call(design, "clean -purge");
		} else
			args.push_back(str);
	}
	free(sstart);
	call(design, args);
}

void Pass::call(RTLIL::Design *design, std::vector<std::string> args)
{
	if (args.size() == 0 || args[0][0] == '#')
		return;

	if (echo_mode) {
		log("%s", create_prompt(design, 0));
		for (size_t i = 0; i < args.size(); i++)
			log("%s%s", i ? " " : "", args[i].c_str());
		log("\n");
	}

	if (pass_register.count(args[0]) == 0)
		log_cmd_error("No such command: %s (type 'help' for a command overview)\n", args[0].c_str());

	size_t orig_sel_stack_pos = design->selection_stack.size();
	pass_register[args[0]]->call_counter++;
	pass_register[args[0]]->execute(args, design);
	while (design->selection_stack.size() > orig_sel_stack_pos)
		design->selection_stack.pop_back();

	design->check();
}

void Pass::call_on_selection(RTLIL::Design *design, const RTLIL::Selection &selection, std::string command)
{
	std::string backup_selected_active_module = design->selected_active_module;
	design->selected_active_module.clear();
	design->selection_stack.push_back(selection);

	Pass::call(design, command);

	design->selection_stack.pop_back();
	design->selected_active_module = backup_selected_active_module;
}

void Pass::call_on_selection(RTLIL::Design *design, const RTLIL::Selection &selection, std::vector<std::string> args)
{
	std::string backup_selected_active_module = design->selected_active_module;
	design->selected_active_module.clear();
	design->selection_stack.push_back(selection);

	Pass::call(design, args);

	design->selection_stack.pop_back();
	design->selected_active_module = backup_selected_active_module;
}

void Pass::call_on_module(RTLIL::Design *design, RTLIL::Module *module, std::string command)
{
	std::string backup_selected_active_module = design->selected_active_module;
	design->selected_active_module = module->name;
	design->selection_stack.push_back(RTLIL::Selection(false));
	design->selection_stack.back().select(module);

	Pass::call(design, command);

	design->selection_stack.pop_back();
	design->selected_active_module = backup_selected_active_module;
}

void Pass::call_on_module(RTLIL::Design *design, RTLIL::Module *module, std::vector<std::string> args)
{
	std::string backup_selected_active_module = design->selected_active_module;
	design->selected_active_module = module->name;
	design->selection_stack.push_back(RTLIL::Selection(false));
	design->selection_stack.back().select(module);

	Pass::call(design, args);

	design->selection_stack.pop_back();
	design->selected_active_module = backup_selected_active_module;
}

Frontend::Frontend(std::string name, std::string short_help) :
		Pass(name.rfind("=", 0) == 0 ? name.substr(1) : "read_" + name, short_help),
		frontend_name(name.rfind("=", 0) == 0 ? name.substr(1) : name)
{
}

void Frontend::run_register()
{
	log_assert(pass_register.count(pass_name) == 0);
	pass_register[pass_name] = this;

	log_assert(frontend_register.count(frontend_name) == 0);
	frontend_register[frontend_name] = this;
}

Frontend::~Frontend()
{
}

void Frontend::execute(std::vector<std::string> args, RTLIL::Design *design)
{
	log_assert(next_args.empty());
	do {
		FILE *f = NULL;
		next_args.clear();
		call_counter++;
		execute(f, std::string(), args, design);
		args = next_args;
		fclose(f);
	} while (!args.empty());
}

FILE *Frontend::current_script_file = NULL;
std::string Frontend::last_here_document;

void Frontend::extra_args(FILE *&f, std::string &filename, std::vector<std::string> args, size_t argidx)
{
	bool called_with_fp = f != NULL;

	next_args.clear();
	for (; argidx < args.size(); argidx++)
	{
		std::string arg = args[argidx];

		if (arg.substr(0, 1) == "-")
			cmd_error(args, argidx, "Unkown option or option in arguments.");
		if (f != NULL)
			cmd_error(args, argidx, "Extra filename argument in direct file mode.");

		filename = arg;
		if (filename == "<<" && argidx+1 < args.size())
			filename += args[++argidx];
		if (filename.substr(0, 2) == "<<") {
			if (Frontend::current_script_file == NULL)
				log_error("Unexpected here document '%s' outside of script!\n", filename.c_str());
			if (filename.size() <= 2)
				log_error("Missing EOT marker in here document!\n");
			std::string eot_marker = filename.substr(2);
			last_here_document.clear();
			while (1) {
				std::string buffer;
				char block[4096];
				while (1) {
					if (fgets(block, 4096, Frontend::current_script_file) == NULL)
						log_error("Unexpected end of file in here document '%s'!\n", filename.c_str());
					buffer += block;
					if (buffer.size() > 0 && (buffer[buffer.size() - 1] == '\n' || buffer[buffer.size() - 1] == '\r'))
						break;
				}
				int indent = buffer.find_first_not_of(" \t\r\n");
				if (buffer.substr(indent, eot_marker.size()) == eot_marker)
					break;
				last_here_document += buffer;
			}
			f = fmemopen((void*)last_here_document.c_str(), last_here_document.size(), "r");
		} else
			f = fopen(filename.c_str(), "r");
		if (f == NULL)
			log_cmd_error("Can't open input file `%s' for reading: %s\n", filename.c_str(), strerror(errno));

		for (size_t i = argidx+1; i < args.size(); i++)
			if (args[i].substr(0, 1) == "-")
				cmd_error(args, i, "Found option, expected arguments.");

		if (argidx+1 < args.size()) {
			next_args.insert(next_args.begin(), args.begin(), args.begin()+argidx);
			next_args.insert(next_args.begin()+argidx, args.begin()+argidx+1, args.end());
			args.erase(args.begin()+argidx+1, args.end());
		}
		break;
	}
	if (f == NULL)
		cmd_error(args, argidx, "No filename given.");

	if (called_with_fp)
		args.push_back(filename);
	args[0] = pass_name;
	// cmd_log_args(args);
}

void Frontend::frontend_call(RTLIL::Design *design, FILE *f, std::string filename, std::string command)
{
	std::vector<std::string> args;
	char *s = strdup(command.c_str());
	for (char *p = strtok(s, " \t\r\n"); p; p = strtok(NULL, " \t\r\n"))
		args.push_back(p);
	free(s);
	frontend_call(design, f, filename, args);
}

void Frontend::frontend_call(RTLIL::Design *design, FILE *f, std::string filename, std::vector<std::string> args)
{
	if (args.size() == 0)
		return;
	if (frontend_register.count(args[0]) == 0)
		log_cmd_error("No such frontend: %s\n", args[0].c_str());

	if (f != NULL) {
		frontend_register[args[0]]->call_counter++;
		frontend_register[args[0]]->execute(f, filename, args, design);
	} else if (filename == "-") {
		FILE *f_stdin = stdin; // workaround for OpenBSD 'stdin' implementation
		frontend_register[args[0]]->call_counter++;
		frontend_register[args[0]]->execute(f_stdin, "<stdin>", args, design);
	} else {
		if (!filename.empty())
			args.push_back(filename);
		frontend_register[args[0]]->execute(args, design);
	}

	design->check();
}

Backend::Backend(std::string name, std::string short_help) :
		Pass(name.rfind("=", 0) == 0 ? name.substr(1) : "write_" + name, short_help),
		backend_name(name.rfind("=", 0) == 0 ? name.substr(1) : name)
{
}

void Backend::run_register()
{
	log_assert(pass_register.count(pass_name) == 0);
	pass_register[pass_name] = this;

	log_assert(backend_register.count(backend_name) == 0);
	backend_register[backend_name] = this;
}

Backend::~Backend()
{
}

void Backend::execute(std::vector<std::string> args, RTLIL::Design *design)
{
	FILE *f = NULL;
	call_counter++;
	execute(f, std::string(), args, design);
	if (f != stdout)
		fclose(f);
}

void Backend::extra_args(FILE *&f, std::string &filename, std::vector<std::string> args, size_t argidx)
{
	bool called_with_fp = f != NULL;

	for (; argidx < args.size(); argidx++)
	{
		std::string arg = args[argidx];

		if (arg.substr(0, 1) == "-" && arg != "-")
			cmd_error(args, argidx, "Unkown option or option in arguments.");
		if (f != NULL)
			cmd_error(args, argidx, "Extra filename argument in direct file mode.");

		if (arg == "-") {
			filename = "<stdout>";
			f = stdout;
			continue;
		}

		filename = arg;
		f = fopen(filename.c_str(), "w");
		if (f == NULL)
			log_cmd_error("Can't open output file `%s' for writing: %s\n", filename.c_str(), strerror(errno));
	}

	if (called_with_fp)
		args.push_back(filename);
	args[0] = pass_name;
	// cmd_log_args(args);

	if (f == NULL) {
		filename = "<stdout>";
		f = stdout;
	}
}

void Backend::backend_call(RTLIL::Design *design, FILE *f, std::string filename, std::string command)
{
	std::vector<std::string> args;
	char *s = strdup(command.c_str());
	for (char *p = strtok(s, " \t\r\n"); p; p = strtok(NULL, " \t\r\n"))
		args.push_back(p);
	free(s);
	backend_call(design, f, filename, args);
}

void Backend::backend_call(RTLIL::Design *design, FILE *f, std::string filename, std::vector<std::string> args)
{
	if (args.size() == 0)
		return;
	if (backend_register.count(args[0]) == 0)
		log_cmd_error("No such backend: %s\n", args[0].c_str());

	size_t orig_sel_stack_pos = design->selection_stack.size();

	if (f != NULL) {
		backend_register[args[0]]->call_counter++;
		backend_register[args[0]]->execute(f, filename, args, design);
	} else if (filename == "-") {
		FILE *f_stdout = stdout; // workaround for OpenBSD 'stdout' implementation
		backend_register[args[0]]->call_counter++;
		backend_register[args[0]]->execute(f_stdout, "<stdout>", args, design);
	} else {
		if (!filename.empty())
			args.push_back(filename);
		backend_register[args[0]]->execute(args, design);
	}

	while (design->selection_stack.size() > orig_sel_stack_pos)
		design->selection_stack.pop_back();

	design->check();
}

struct HelpPass : public Pass {
	HelpPass() : Pass("help", "display help messages") { }
	virtual void help()
	{
		log("\n");
		log("    help  .............  list all commands\n");
		log("    help <command>  ...  print help message for given command\n");
		log("    help -all  ........  print complete command reference\n");
		log("\n");
	}
	void escape_tex(std::string &tex)
	{
		size_t pos = 0;
		while ((pos = tex.find('_', pos)) != std::string::npos) {
			tex.replace(pos, 1, "\\_");
			pos += 2;
		}
	}
	void write_tex(FILE *f, std::string cmd, std::string title, std::string text)
	{
		size_t begin = text.find_first_not_of("\n"), end = text.find_last_not_of("\n");
		if (begin != std::string::npos && end != std::string::npos && begin < end)
			text = text.substr(begin, end-begin+1);
		std::string cmd_unescaped = cmd;
		escape_tex(cmd);
		escape_tex(title);
		fprintf(f, "\\section{%s -- %s}\n", cmd.c_str(), title.c_str());
		fprintf(f, "\\label{cmd:%s}\n", cmd_unescaped.c_str());
		fprintf(f, "\\begin{lstlisting}[numbers=left,frame=single]\n");
		fprintf(f, "%s\n\\end{lstlisting}\n\n", text.c_str());
	}
	void escape_html(std::string &html)
	{
		size_t pos = 0;
		while ((pos = html.find_first_of("<>&", pos)) != std::string::npos)
			switch (html[pos]) {
			case '<':
				html.replace(pos, 1, "&lt;");
				pos += 4;
				break;
			case '>':
				html.replace(pos, 1, "&gt;");
				pos += 4;
				break;
			case '&':
				html.replace(pos, 1, "&amp;");
				pos += 5;
				break;
			}
	}
	void write_html(FILE *idxf, std::string cmd, std::string title, std::string text)
	{
		FILE *f = fopen(stringf("cmd_%s.in", cmd.c_str()).c_str(), "wt");
		fprintf(idxf, "<li><a href=\"cmd_%s.html\"> ", cmd.c_str());

		escape_html(cmd);
		escape_html(title);
		escape_html(text);

		fprintf(idxf, "%s</a> <span>%s</span></a>\n", cmd.c_str(), title.c_str());

		fprintf(f, "@cmd_header %s@\n", cmd.c_str());
		fprintf(f, "<h1>%s - %s</h1>\n", cmd.c_str(), title.c_str());
		fprintf(f, "<pre>%s</pre>\n", text.c_str());
		fprintf(f, "@footer@\n");

		fclose(f);
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design*)
	{
		if (args.size() == 1) {
			log("\n");
			for (auto &it : REGISTER_INTERN::pass_register)
				log("    %-20s %s\n", it.first.c_str(), it.second->short_help.c_str());
			log("\n");
			log("Type 'help <command>' for more information on a command.\n");
			log("\n");
			return;
		}

		if (args.size() == 2) {
			if (args[1] == "-all") {
				for (auto &it : REGISTER_INTERN::pass_register) {
					log("\n\n");
					log("%s  --  %s\n", it.first.c_str(), it.second->short_help.c_str());
					for (size_t i = 0; i < it.first.size() + it.second->short_help.size() + 6; i++)
						log("=");
					log("\n");
					it.second->help();
				}
			}
			// this option is undocumented as it is for internal use only
			else if (args[1] == "-write-tex-command-reference-manual") {
				FILE *f = fopen("command-reference-manual.tex", "wt");
				fprintf(f, "%% Generated using the yosys 'help -write-tex-command-reference-manual' command.\n\n");
				for (auto &it : REGISTER_INTERN::pass_register) {
					size_t memsize;
					char *memptr;
					FILE *memf = open_memstream(&memptr, &memsize);
					log_files.push_back(memf);
					it.second->help();
					log_files.pop_back();
					fclose(memf);
					write_tex(f, it.first, it.second->short_help, memptr);
					free(memptr);
				}
				fclose(f);
			}
			// this option is undocumented as it is for internal use only
			else if (args[1] == "-write-web-command-reference-manual") {
				FILE *f = fopen("templates/cmd_index.in", "wt");
				for (auto &it : REGISTER_INTERN::pass_register) {
					size_t memsize;
					char *memptr;
					FILE *memf = open_memstream(&memptr, &memsize);
					log_files.push_back(memf);
					it.second->help();
					log_files.pop_back();
					fclose(memf);
					write_html(f, it.first, it.second->short_help, memptr);
					free(memptr);
				}
				fclose(f);
			}
			else if (REGISTER_INTERN::pass_register.count(args[1]) == 0)
				log("No such command: %s\n", args[1].c_str());
			else
				REGISTER_INTERN::pass_register.at(args[1])->help();
			return;
		}

		help();
	}
} HelpPass;
 
struct EchoPass : public Pass {
	EchoPass() : Pass("echo", "turning echoing back of commands on and off") { }
	virtual void help()
	{
		log("\n");
		log("    echo on\n");
		log("\n");
		log("Print all commands to log before executing them.\n");
		log("\n");
		log("\n");
		log("    echo off\n");
		log("\n");
		log("Do not print all commands to log before executing them. (default)\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design*)
	{
		if (args.size() > 2)
			cmd_error(args, 2, "Unexpected argument.");

		if (args.size() == 2) {
			if (args[1] == "on")
				echo_mode = true;
			else if (args[1] == "off")
				echo_mode = false;
			else
				cmd_error(args, 1, "Unexpected argument.");
		}

		log("echo %s\n", echo_mode ? "on" : "off");
	}
} EchoPass;
 
