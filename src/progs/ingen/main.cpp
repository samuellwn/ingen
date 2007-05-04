/* This file is part of Ingen.
 * Copyright (C) 2007 Dave Robillard <http://drobilla.net>
 * 
 * Ingen is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 * 
 * Ingen is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <iostream>
#include <string>
#include <signal.h>
#include <boost/optional.hpp>
#include <raul/Path.h>
#include <raul/RDFWorld.h>
#include "config.h"
#include "module/Module.h"
#include "engine/Engine.h"
#include "engine/QueuedEngineInterface.h"
#include "serialisation/Loader.h"
#include "cmdline.h"

using namespace std;
using namespace Ingen;

SharedPtr<Engine> engine;


void
catch_int(int)
{
	signal(SIGINT, catch_int);
	signal(SIGTERM, catch_int);

	cout << "[Main] Ingen interrupted." << endl;
	engine->quit();
}


int
main(int argc, char** argv)
{
	/* Parse command line options */
	gengetopt_args_info args;
	if (cmdline_parser (argc, argv, &args) != 0)
		return 1;

	if (argc <= 1) {
		cout << "No arguments provided.  Try something like:" << endl << endl;
		cout << "Run an engine: ingen -e" << endl;
		cout << "Run the GUI:   ingen -g" << endl;
		cout << "Pring full help: ingen -h" << endl << endl;
		cmdline_parser_print_help();
		return 0;
	}

	SharedPtr<Glib::Module> engine_module;
	SharedPtr<Glib::Module> gui_module;

	SharedPtr<Shared::EngineInterface> engine_interface;

	/* Run engine */
	if (args.engine_flag) {

		engine_module = Ingen::Shared::load_module("ingen_engine");

		if (engine_module) {
			/*if (args.engine_port_given) {
				bool (*launch_engine)(int) = NULL;
				if ( ! module->get_symbol("launch_osc_engine", (void*&)launch_engine))
					module.reset();
				else
					launch_engine(args.engine_port_arg);

			} else if (args.gui_given || args.load_given) {*/
				Engine* (*new_engine)() = NULL;
				if (engine_module->get_symbol("new_engine", (void*&)new_engine)) {
					engine = SharedPtr<Engine>(new_engine());
					engine->start_jack_driver();
					//engine->launch_osc_interface(args.engine_port_arg);
				} else {
					engine_module.reset();
				}
				engine_interface = engine->new_queued_interface();
			/*} else {
				cerr << "Nonsense command line parameters, engine not loaded." << endl;
			}*/
		} else {
			cerr << "Unable to load engine module, engine not loaded." << endl;
			cerr << "Try running ingen_dev or setting INGEN_MODULE_PATH." << endl;
		}

	}
	

	/* Connect to remote engine */
	if (args.connect_given || (args.load_given && !engine_interface)) {
		bool found = false;
		SharedPtr<Glib::Module> client_module
			= Ingen::Shared::load_module("ingen_client");

		SharedPtr<Shared::EngineInterface> (*new_osc_interface)(const std::string&) = NULL;

		if (client_module)
			found = client_module->get_symbol("new_osc_interface", (void*&)new_osc_interface);

		if (client_module && found) {
			engine_interface = new_osc_interface(args.connect_arg);
		} else {
			cerr << "Unable to load ingen_client module, aborting." << endl;
			return -1;
		}
	}


	/* Load a patch */
	if (args.load_given) {

		Raul::RDF::World rdf_world;
		rdf_world.add_prefix("xsd", "http://www.w3.org/2001/XMLSchema#");
		rdf_world.add_prefix("ingen", "http://drobilla.net/ns/ingen#");
		rdf_world.add_prefix("ingenuity", "http://drobilla.net/ns/ingenuity#");
		rdf_world.add_prefix("lv2", "http://lv2plug.in/ontology#");
		rdf_world.add_prefix("rdfs", "http://www.w3.org/2000/01/rdf-schema#");
		rdf_world.add_prefix("doap", "http://usefulinc.com/ns/doap#");

		boost::optional<Raul::Path> parent_path;
		if (args.path_given)
			parent_path = args.path_arg;

		bool found = false;
		SharedPtr<Glib::Module> serialisation_module
			= Ingen::Shared::load_module("ingen_serialisation");
			
		Serialisation::Loader* (*new_loader)() = NULL;

		if (serialisation_module)
			found = serialisation_module->get_symbol("new_loader", (void*&)new_loader);
		
		if (serialisation_module && found) {
			SharedPtr<Serialisation::Loader> loader(new_loader());
			loader->load(engine_interface, &rdf_world,
					string("file:") + args.load_arg, parent_path, "");
		} else {
			cerr << "Unable to load serialisation module, aborting." << endl;
			return -1;
		}
	}
	

	/* Run GUI */
	bool ran_gui = false;
	if (args.gui_given) {
		gui_module = Ingen::Shared::load_module("ingen_gui");
		void (*run)(int, char**) = NULL;
		bool found = gui_module->get_symbol("run", (void*&)run);

		if (found) {
			ran_gui = true;
			run(argc, argv);
		} else {
			cerr << "Unable to find GUI module, GUI not loaded." << endl;
			cerr << "Try running ingen_dev or setting INGEN_MODULE_PATH." << endl;
		}
	}


	/* Didn't run the GUI, do our own main thing. */
	if (engine && !ran_gui) {

		signal(SIGINT, catch_int);
		signal(SIGTERM, catch_int);
		
		engine->start_jack_driver();
		engine->start_osc_driver(args.engine_port_arg);

		engine->activate();

		engine->main();

		engine.reset();
	}

	return 0;
}

