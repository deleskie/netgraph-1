/*
 * Netgraph -- network data collection and graphing, server component
 * Author: Chris Wilson <chris+netgraph@qwirx.com>
 *
 * Based on: iptables -- IP firewall administration for kernels with
 * firewall table (aimed for the 2.3 kernels)
 *
 * Author: Paul.Russell@rustcorp.com.au and mneuling@radlogic.com.au
 *
 * Based on the ipchains code by Paul Russell and Michael Neuling
 *
 * (C) 2000-2002 by the netfilter coreteam <coreteam@netfilter.org>:
 * 		    Paul 'Rusty' Russell <rusty@rustcorp.com.au>
 * 		    Marc Boucher <marc+nf@mbsi.ca>
 * 		    James Morris <jmorris@intercode.com.au>
 * 		    Harald Welte <laforge@gnumonks.org>
 * 		    Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <iptables.h>

#include <memory>
#include <sstream>

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include <cgicc/Cgicc.h>
#include "cgicc/HTTPResponseHeader.h"

using namespace cgicc;

static void
print_header(struct xtc_handle *handle, const char *chain, 
	JsonBuilder *builder)
{
	struct xt_counters counters;

	const char *pol = iptc_get_policy(chain, &counters, handle);
	if (pol)
	{
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, "builtin");

		json_builder_set_member_name(builder, "policy");
		json_builder_add_string_value(builder, pol);
		
		json_builder_set_member_name(builder, "packets");
		json_builder_add_int_value(builder, counters.pcnt);

		json_builder_set_member_name(builder, "bytes");
		json_builder_add_int_value(builder, counters.bcnt);
	}
	else
	{
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, "user");

		unsigned int refs;
		if (iptc_get_references(&refs, chain, handle))
		{
			json_builder_set_member_name(builder, "refs");
			json_builder_add_int_value(builder, refs);
		}
	}
}

static bool
list_entries(struct xtc_handle *handle, const xt_chainlabel list_chain,
	JsonBuilder *builder)
{
	bool found = false;
	const char *this_chain;

	json_builder_set_member_name(builder, "chains");
	json_builder_begin_object(builder);

	for (this_chain = iptc_first_chain(handle);
	     this_chain;
	     this_chain = iptc_next_chain(handle))
	{
		const struct ipt_entry *i;
		unsigned int num;

		if (list_chain && strcmp(list_chain, this_chain) != 0)
			continue;

		json_builder_set_member_name(builder, this_chain);
		json_builder_begin_object(builder);
		print_header(handle, this_chain, builder);

		json_builder_set_member_name(builder, "rules");
		json_builder_begin_array(builder);
		
		i = iptc_first_rule(this_chain, handle);
		int rule_num = 0;
		
		while (i)
		{
			rule_num++;
			/*
			print_firewall(i,
				iptc_get_target(i, handle),
				num,
				format,
				handle);
			*/
			i = iptc_next_rule(i, handle);
		}
		
		json_builder_end_array(builder); // rules
		json_builder_end_object(builder); // this_chain
		found = true;
	}

	json_builder_end_object(builder);

	errno = found ? 0 : ENOENT;
	return found;
}

class netgraph_exception : public std::exception
{
	private:
	std::string message;
	
	public:
	netgraph_exception(const std::string& rMessage)
	: message(rMessage)
	{ }
	virtual ~netgraph_exception() throw () { }
	
	virtual const char* what() const throw ()
	{
		return message.c_str();
	}
};

#define THROW(class, stuff) \
{ \
	std::ostringstream __out; \
	__out << stuff; \
	throw class(__out.str()); \
}

int
main(int argc, char *argv[])
{
	const char *table = "filter";
	const char *chain = NULL;
	struct xtc_handle *handle = NULL;
	int http_status_code = 500;
	
	g_type_init();
	
	std::auto_ptr<Cgicc> apCgi;
	
	try
	{
		apCgi.reset(new Cgicc());
	}
	catch(std::exception& e)
	{
		fprintf(stderr, "Failed to initialize Cgicc: %s\n", e.what());
		exit(1);
	}

	std::string server_string = 
		std::string(PACKAGE_NAME "/" PACKAGE_VERSION) +
		" (GNU cgicc/" + apCgi->getVersion() + ")";
	std::ostream* pOut = &(std::cout);
	bool isCommandLine = true;

	// If we're a CGI, we should be ready to output JSON, whatever
	// happens next.
	JsonBuilder *builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "protocol");
	json_builder_add_string_value(builder, PACKAGE_NAME);

	json_builder_set_member_name(builder, "app");
	json_builder_add_string_value(builder, PACKAGE_URL);

	json_builder_set_member_name(builder, "version");
	json_builder_begin_array(builder);
	json_builder_add_int_value(builder, 1);
	json_builder_add_int_value(builder, 1);
	json_builder_end_array(builder);
	
	try
	{
		std::string env = apCgi->getEnvironment().getGatewayInterface();
	
		if (env == "")
		{
			// looks like command-line usage
			isCommandLine = true;
		}
		else
		{
			// looks like a CGI invocation
			isCommandLine = false;
		}

		iptables_globals.program_name = argv[0];
		int ret = xtables_init_all(&iptables_globals, NFPROTO_IPV4);
		if (ret != 0)
		{
			THROW(netgraph_exception, "failed to initialize xtables");
		}
	
#if defined(ALL_INCLUSIVE) || defined(NO_SHARED_LIBS)
		init_extensions();
		init_extensions4();
#endif

		xtables_load_ko(xtables_modprobe_program, false);
		handle = iptc_init(table);
		if (!handle)
		{
			THROW(netgraph_exception, "failed to initialize iptables "
				"table '" << table << "': " << iptc_strerror(errno));
		}
	
		bool success = list_entries(handle, chain, builder);

		if (success)
		{
			// iptc_commit(handle);
			iptc_free(handle);
		}
		else
		{
			if (errno == EINVAL) {
				fprintf(stderr, "iptables: %s. "
						"Run `dmesg' for more information.\n",
					iptc_strerror(errno));
			} else {
				fprintf(stderr, "iptables: %s.\n",
					iptc_strerror(errno));
			}
			if (errno == EAGAIN) {
				exit(RESOURCE_PROBLEM);
			}
		}

		http_status_code = 200; // success!
	}
	catch (std::exception &e)
	{
		if (isCommandLine)
		{
			// print the error message and die quickly
			std::cerr << PACKAGE_NAME << ": " << e.what() << std::endl;
			exit(1);
		}
		else
		{
			// continue to render a well-formed JSON output
			json_builder_set_member_name(builder, "error");
			json_builder_add_string_value(builder, e.what());
		}
	}
	
	// If we got here, we've either successfully processed the
	// request, or we've got an error that needs to be sent as
	// valid JSON. Either way, we must build a well-formed response.
	
	json_builder_end_object(builder);

	JsonGenerator *gen = json_generator_new();
	JsonNode * root = json_builder_get_root(builder);
	json_generator_set_root(gen, root);
	gchar *str = json_generator_to_data(gen, NULL);

	json_node_free(root);
	g_object_unref(gen);
	g_object_unref(builder);

	// Tell the server not to parse our headers
	*pOut << HTTPResponseHeader("HTTP/1.1", http_status_code,
		http_status_code == 200 ? "OK" : "Application Error")
		// .addHeader("Date", current_date)
		.addHeader("Server", server_string)
		.addHeader("Content-Type", "application/json")
		<< str;
	
	exit(http_status_code == 200 ? 0 : 1);
}