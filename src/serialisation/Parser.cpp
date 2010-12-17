/* This file is part of Ingen.
 * Copyright (C) 2007-2009 David Robillard <http://drobilla.net>
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

#include <set>
#include <locale.h>
#include <glibmm/ustring.h>
#include <glibmm/miscutils.h>
#include <glibmm/fileutils.h>
#include <glibmm/convert.h>
#include "raul/log.hpp"
#include "redlandmm/Model.hpp"
#include "redlandmm/Node.hpp"
#include "redlandmm/Query.hpp"
#include "raul/TableImpl.hpp"
#include "raul/Atom.hpp"
#include "raul/AtomRDF.hpp"
#include "interface/EngineInterface.hpp"
#include "module/World.hpp"
#include "shared/LV2URIMap.hpp"
#include "Parser.hpp"
#include "names.hpp"

#define LOG(s) s << "[Parser] "

using namespace std;
using namespace Raul;
using namespace Ingen::Shared;

namespace Ingen {
namespace Serialisation {


static Glib::ustring
relative_uri(Glib::ustring base, const Glib::ustring uri, bool leading_slash)
{
	raptor_uri* base_uri = raptor_new_uri((const unsigned char*)base.c_str());
	raptor_uri* full_uri = raptor_new_uri((const unsigned char*)uri.c_str());

	Glib::ustring ret((const char*)raptor_uri_to_relative_uri_string(base_uri, full_uri));

	raptor_free_uri(base_uri);
	raptor_free_uri(full_uri);

	if (leading_slash && ret[0] != '/')
		ret = Glib::ustring("/") + ret;

	return ret;
}


static void
normalise_uri(Glib::ustring& uri)
{
	size_t dotslash = string::npos;
	while ((dotslash = uri.find("./")) != string::npos)
		uri = uri.substr(0, dotslash) + uri.substr(dotslash + 2);
}


Parser::PatchRecords
Parser::find_patches(
	Ingen::Shared::World* world,
	const Glib::ustring&  manifest_uri)
{
	Redland::Model model(*world->rdf_world(), manifest_uri, manifest_uri);
	Redland::Query query(*world->rdf_world(),
		"SELECT DISTINCT ?patch ?file WHERE {\n"
		"?patch a            ingen:Patch ;\n"
		"       rdfs:seeAlso ?file .\n"
		"}");

	std::list<PatchRecord> records;

	SharedPtr<Redland::QueryResults> results(query.run(*world->rdf_world(), model, manifest_uri));
	for (; !results->finished(); results->next()) {
		const Redland::Node& patch(results->get("patch"));
		const Redland::Node& file(results->get("file"));
		records.push_back(PatchRecord(patch.to_c_string(), file.to_c_string()));
	}

	return records;
}


/** Parse a patch from RDF into a CommonInterface (engine or client).
 * @return whether or not load was successful.
 */
bool
Parser::parse_document(
		Ingen::Shared::World*                    world,
		Ingen::Shared::CommonInterface*          target,
		Glib::ustring                            document_uri,
		boost::optional<Raul::Path>              data_path,
		boost::optional<Raul::Path>              parent,
		boost::optional<Raul::Symbol>            symbol,
		boost::optional<GraphObject::Properties> data)
{
	normalise_uri(document_uri);

	const std::string filename(Glib::filename_from_uri(document_uri));
	const size_t      ext = filename.find(INGEN_BUNDLE_EXT);
	const size_t      ext_len = strlen(INGEN_BUNDLE_EXT);
	if (ext == filename.length() - ext_len
			|| (ext == filename.length() - ext_len - 1 && filename[filename.length() - 1] == '/')) {
		std::string basename(Glib::path_get_basename(filename));
		basename = basename.substr(0, basename.find('.'));
		document_uri += "/" + basename + INGEN_PATCH_FILE_EXT;
	}

	Redland::Model model(*world->rdf_world(), document_uri, document_uri);

	LOG(info) << "Parsing " << document_uri << endl;
	if (data_path)
		LOG(info) << "Path: " << *data_path << endl;
	if (parent)
		LOG(info) << "Parent: " << *parent << endl;
	if (symbol)
		LOG(info) << "Symbol: " << *symbol << endl;

	boost::optional<Path> parsed_path
		= parse(world, target, model, document_uri, data_path, parent, symbol, data);

	if (parsed_path) {
		target->set_property(*parsed_path, "http://drobilla.net/ns/ingen#document",
				Atom(Atom::URI, document_uri.c_str()));
	} else {
		LOG(warn) << "Document URI lost" << endl;
	}

	return parsed_path;
}


bool
Parser::parse_string(
		Ingen::Shared::World*                    world,
		Ingen::Shared::CommonInterface*          target,
		const Glib::ustring&                     str,
		const Glib::ustring&                     base_uri,
		boost::optional<Raul::Path>              data_path,
		boost::optional<Raul::Path>              parent,
		boost::optional<Raul::Symbol>            symbol,
		boost::optional<GraphObject::Properties> data)
{
	Redland::Model model(*world->rdf_world(), str.c_str(), str.length(), base_uri);

	LOG(info) << "Parsing " << (data_path ? data_path->str() : "*") << " from string";
	if (!base_uri.empty())
		info << " (base " << base_uri << ")";
	info << endl;

	bool ret = parse(world, target, model, base_uri, data_path, parent, symbol, data);
	Redland::Resource subject(*world->rdf_world(), base_uri);
	parse_connections(world, target, model, subject, parent ? *parent : "/");

	return ret;
}


bool
Parser::parse_update(
		Ingen::Shared::World*                    world,
		Shared::CommonInterface*                 target,
		const Glib::ustring&                     str,
		const Glib::ustring&                     base_uri,
		boost::optional<Raul::Path>              data_path,
		boost::optional<Raul::Path>              parent,
		boost::optional<Raul::Symbol>            symbol,
		boost::optional<GraphObject::Properties> data)
{
	Redland::Model model(*world->rdf_world(), str.c_str(), str.length(), base_uri);

	// Delete anything explicitly declared to not exist
	Glib::ustring query_str = Glib::ustring("SELECT DISTINCT ?o WHERE { ?o a owl:Nothing }");
	Redland::Query query(*world->rdf_world(), query_str);
	SharedPtr<Redland::QueryResults> results(query.run(*world->rdf_world(), model, base_uri));

	for (; !results->finished(); results->next()) {
		const Redland::Node& object = results->get("o");
		target->del(object.to_string());
	}

	// Properties
	query = Redland::Query(*world->rdf_world(),
		"SELECT DISTINCT ?s ?p ?o WHERE {\n"
		"?s ?p ?o .\n"
		"}");

	results = query.run(*world->rdf_world(), model, base_uri);
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		string               obj_uri(results->get("s").to_string());
		const string         key(results->get("p").to_string());
		const Redland::Node& val_node(results->get("o"));
		const Atom           a(AtomRDF::node_to_atom(model, val_node));
		if (obj_uri.find(":") == string::npos)
			obj_uri = Path(obj_uri).str();
		obj_uri = relative_uri(base_uri, obj_uri, true);
		if (!key.empty())
			target->set_property(string("path:") + obj_uri, key, a);
	}


	// Connections
	Redland::Resource subject(*world->rdf_world(), base_uri);
	parse_connections(world, target, model, subject, "/");

	// Port values
	query = Redland::Query(*world->rdf_world(),
		"SELECT DISTINCT ?path ?value WHERE {\n"
		"?path ingen:value ?value .\n"
		"}");

	results = query.run(*world->rdf_world(), model, base_uri);
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		const string obj_path = results->get("path").to_string();
		const Redland::Node& val_node = results->get("value");
		const Atom a(AtomRDF::node_to_atom(model, val_node));
		target->set_property(obj_path, world->uris()->ingen_value, a);
	}

	return parse(world, target, model, base_uri, data_path, parent, symbol, data);
}


boost::optional<Path>
Parser::parse(
		Ingen::Shared::World*                    world,
		Ingen::Shared::CommonInterface*          target,
		Redland::Model&                          model,
		Glib::ustring                            document_uri,
		boost::optional<Raul::Path>              data_path,
		boost::optional<Raul::Path>              parent,
		boost::optional<Raul::Symbol>            symbol,
		boost::optional<GraphObject::Properties> data)
{
	const Redland::Node::Type res = Redland::Node::RESOURCE;

	const Glib::ustring query_str = data_path
		? Glib::ustring("SELECT DISTINCT ?t WHERE { <") + data_path->chop_start("/") + "> a ?t . }"
		: Glib::ustring("SELECT DISTINCT ?s ?t WHERE { ?s a ?t . }");

	Redland::Query query(*world->rdf_world(), query_str);
	SharedPtr<Redland::QueryResults> results(query.run(*world->rdf_world(), model, document_uri));

#define NS_INGEN "http://drobilla.net/ns/ingen#"
#define NS_LV2   "http://lv2plug.in/ns/lv2core#"

	const Redland::Node patch_class    (*world->rdf_world(), res, NS_INGEN "Patch");
	const Redland::Node node_class     (*world->rdf_world(), res, NS_INGEN "Node");
	const Redland::Node internal_class (*world->rdf_world(), res, NS_INGEN "Internal");
	const Redland::Node ladspa_class   (*world->rdf_world(), res, NS_INGEN "LADSPAPlugin");
	const Redland::Node in_port_class  (*world->rdf_world(), res, NS_LV2 "InputPort");
	const Redland::Node out_port_class (*world->rdf_world(), res, NS_LV2 "OutputPort");
	const Redland::Node lv2_class      (*world->rdf_world(), res, NS_LV2 "Plugin");

	const Redland::Node subject_node = (data_path && !data_path->is_root())
		? Redland::Node(*world->rdf_world(), res, data_path->chop_start("/"))
		: model.base_uri();

	std::string           path_str;
	boost::optional<Path> ret;
	boost::optional<Path> root_path;

	for (; !results->finished(); results->next()) {
		const Redland::Node& subject = (data_path ? subject_node : results->get("s"));
		const Redland::Node& rdf_class = results->get("t");

		if (!data_path)
			path_str = relative_uri(document_uri, subject.to_c_string(), true);

		const bool is_plugin =    (rdf_class == ladspa_class)
		                       || (rdf_class == lv2_class)
		                       || (rdf_class == internal_class);

		const bool is_object =    (rdf_class == patch_class)
		                       || (rdf_class == node_class)
		                       || (rdf_class == in_port_class)
		                       || (rdf_class == out_port_class);

		const Glib::ustring subject_uri_tok = Glib::ustring("<").append(subject).append(">");

		if (is_object) {
			if (path_str.empty() || path_str[0] != '/')
				path_str = "/" + path_str;

			if (!Path::is_valid(path_str)) {
				LOG(warn) << "Invalid path '" << path_str << "', object skipped" << endl;
				continue;
			}

			string path = (parent && symbol)
				? parent->child(*symbol).str()
				: (parent ? *parent : Path("/")).child(path_str.substr(path_str.find("/")+1)).str();

			if (!Path::is_valid(path)) {
				LOG(warn) << "Invalid path '" << path << "' transformed to /" << endl;
				path = "/";
			}

			if (rdf_class == patch_class) {
				ret = parse_patch(world, target, model, subject, parent, symbol, data);
			} else if (rdf_class == node_class) {
				ret = parse_node(world, target, model, subject, path, data);
			} else if (rdf_class == in_port_class || rdf_class == out_port_class) {
				parse_properties(world, target, model, subject, path, data);
				ret = path;
			}

			if (!ret) {
				LOG(error) << "Failed to parse object " << path << endl;
				return boost::optional<Path>();
			}

			if (data_path && subject.to_string() == data_path->str())
				root_path = ret;

		} else if (is_plugin) {
			string subject_str = subject.to_string();
			if (URI::is_valid(subject_str)) {
				if (subject == document_uri)
					subject_str = Path::root().str();
				parse_properties(world, target, model, subject, subject_str);
			}
		}

	}

	return root_path;
}


boost::optional<Path>
Parser::parse_patch(
		Ingen::Shared::World*                    world,
		Ingen::Shared::CommonInterface*          target,
		Redland::Model&                          model,
		const Redland::Node&                     subject_node,
		boost::optional<Raul::Path>              parent,
		boost::optional<Raul::Symbol>            a_symbol,
		boost::optional<GraphObject::Properties> data)
{
	const LV2URIMap& uris = *world->uris().get();
	uint32_t patch_poly = 0;

	typedef Redland::QueryResults Results;

	/* Use parameter overridden polyphony, if given */
	if (data) {
		GraphObject::Properties::iterator poly_param = data.get().find(uris.ingen_polyphony);
		if (poly_param != data.get().end() && poly_param->second.type() == Atom::INT)
			patch_poly = poly_param->second.get_int32();
	}

	const Glib::ustring subject = subject_node.to_turtle_token();

	/* Load polyphony from file if necessary */
	if (patch_poly == 0) {
		Redland::Query query(*world->rdf_world(), Glib::ustring(
			"SELECT DISTINCT ?poly WHERE { ") + subject + " ingen:polyphony ?poly }");

		SharedPtr<Redland::QueryResults> results(query.run(*world->rdf_world(), model));
		if (!results->finished()) {
			const Redland::Node& poly_node = results->get("poly");
			if (poly_node.is_int())
				patch_poly = poly_node.to_int();
			else
				LOG(warn) << "Patch has non-integer polyphony, assuming 1" << endl;
		}
	}

	/* No polyphony value anywhere, 1 it is */
	if (patch_poly == 0)
		patch_poly = 1;

	const Glib::ustring base_uri = model.base_uri().to_string();

	Raul::Symbol symbol = "_";
	if (a_symbol) {
		symbol = *a_symbol;
	} else {
		const std::string basename = Glib::path_get_basename(base_uri);
		symbol = Raul::Symbol::symbolify(basename.substr(0, basename.find('.')));
	}

	string patch_path_str = relative_uri(base_uri, subject_node.to_string(), true);
	if (parent && a_symbol)
		patch_path_str = parent->child(*a_symbol).str();

	if (!Path::is_valid(patch_path_str)) {
		LOG(error) << "Patch has invalid path: " << patch_path_str << endl;
		return boost::optional<Raul::Path>();
	}


	/* Create patch */
	Path patch_path(patch_path_str);
	Resource::Properties props;
	props.insert(make_pair(uris.rdf_type,        Raul::URI(uris.ingen_Patch)));
	props.insert(make_pair(uris.ingen_polyphony, Raul::Atom(int32_t(patch_poly))));
	target->put(patch_path, props);


	/* Find patches in document */
	Redland::Query query(*world->rdf_world(), Glib::ustring(
		"SELECT DISTINCT ?patch WHERE {\n") +
			"?patch a ingen:Patch .\n"
		"}");
	set<string> patches;
	SharedPtr<Redland::QueryResults> results(query.run(*world->rdf_world(), model, base_uri));
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		patches.insert(results->get("patch").to_string());
	}

	typedef multimap<Raul::URI, Raul::Atom> Properties;
	typedef map<string, Redland::Node>      Resources;
	typedef map<string, Properties>         Objects;
	typedef map<string, string>             Types;

	/* Find nodes on this patch */
	query = Redland::Query(*world->rdf_world(), Glib::ustring(
		"SELECT DISTINCT ?node ?type WHERE {\n") +
			subject + " ingen:node     ?node .\n"
			"?node      rdf:instanceOf ?type .\n"
		"}");
	Objects   patch_nodes;
	Objects   plugin_nodes;
	Resources resources;
	Types     types;
	results = query.run(*world->rdf_world(), model, base_uri);
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		const Redland::Node& node = results->get("node");
		const Redland::Node& type = results->get("type");
		if (node.type() == Redland::Node::RESOURCE && type.type() == Redland::Node::RESOURCE) {
			types.insert(make_pair(node.to_string(), type.to_string()));
			if (patches.find(type.to_string()) != patches.end()) {
				patch_nodes.insert(make_pair(node.to_string(), Properties()));
				resources.insert(make_pair(type.to_string(), type));
			} else {
				plugin_nodes.insert(make_pair(node.to_string(), Properties()));
			}
		}
	}


	/* Load nodes on this patch */
	query = Redland::Query(*world->rdf_world(), Glib::ustring(
		"SELECT DISTINCT ?node ?predicate ?object WHERE {\n") +
			subject + " ingen:node ?node .\n"
			"?node      ?predicate ?object .\n"
		"}");
	results = query.run(*world->rdf_world(), model, base_uri);
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		const Redland::Node& node      = results->get("node");
		const Redland::Node& predicate = results->get("predicate");
		const Redland::Node& object    = results->get("object");
		Objects::iterator    patch_i   = patch_nodes.find(node.to_string());
		Objects::iterator    plug_i    = plugin_nodes.find(node.to_string());
		Types::iterator      type_i    = types.find(node.to_string());
		if (node.type() == Redland::Node::RESOURCE && type_i != types.end()) {
			if (skip_property(predicate))
				continue;
			const string key = predicate.to_string();
			if (patch_i != patch_nodes.end()) {
				patch_i->second.insert(make_pair(key, AtomRDF::node_to_atom(model, object)));
			} else if (plug_i != plugin_nodes.end()) {
				plug_i->second.insert(make_pair(key, AtomRDF::node_to_atom(model, object)));
			} else {
				LOG(warn) << "Unrecognized node: " << node.to_string() << endl;
			}
		}
	}

	/* Create patch nodes */
	for (Objects::iterator i = patch_nodes.begin(); i != patch_nodes.end(); ++i) {
		const Path      node_path = patch_path.child(relative_uri(base_uri, i->first, false));
		Types::iterator type_i    = types.find(i->first);
		if (type_i == types.end())
			continue;
		Resources::iterator res_i = resources.find(type_i->second);
		if (res_i == resources.end())
			continue;
		parse_patch(world, target, model, res_i->second, patch_path, Symbol(node_path.symbol()));
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		target->put(node_path, i->second);
	}

	/* Create plugin nodes */
	for (Objects::iterator i = plugin_nodes.begin(); i != plugin_nodes.end(); ++i) {
		Types::iterator type_i = types.find(i->first);
		if (type_i == types.end())
			continue;
		const Path node_path = patch_path.child(relative_uri(base_uri, i->first, false));
		Resource::Properties props;
		props.insert(make_pair(uris.rdf_type,       Raul::URI(uris.ingen_Node)));
		props.insert(make_pair(uris.rdf_instanceOf, Raul::URI(type_i->second)));
		props.insert(i->second.begin(), i->second.end());
		target->put(node_path, props);
	}


	/* Load node ports */
	query = Redland::Query(*world->rdf_world(), Glib::ustring(
		"SELECT DISTINCT ?node ?port ?key ?val WHERE {\n") +
			subject + " ingen:node ?node .\n"
			"?node      lv2:port   ?port .\n"
			"?port      ?key       ?val .\n"
		"}");
	Objects node_ports;
	results = query.run(*world->rdf_world(), model, base_uri);
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		const string node_uri = results->get("node").to_string();
		const string port_uri = results->get("port").to_string();
		if (port_uri.length() <= node_uri.length()) {
			LOG(warn) << "Port on " << node_uri << " has bad URI: " << port_uri << endl;
			continue;
		}

		Objects::iterator p = node_ports.find(port_uri);
		if (p == node_ports.end())
			p = node_ports.insert(make_pair(port_uri, Properties())).first;

		const Path   node_path = patch_path.child(relative_uri(base_uri, node_uri, false));
		const Symbol port_sym  = port_uri.substr(node_uri.length() + 1);
		const Path   port_path = node_path.child(port_sym);
		const string key       = results->get("key").to_string();
		p->second.insert(make_pair(key, AtomRDF::node_to_atom(model, results->get("val"))));
	}

	for (Objects::iterator i = node_ports.begin(); i != node_ports.end(); ++i) {
		target->put(patch_path.child(relative_uri(base_uri, i->first, false)), i->second);
	}


	/* Find ports on this patch */
	query = Redland::Query(*world->rdf_world(), Glib::ustring(
		"SELECT DISTINCT ?port ?type WHERE {\n") +
			subject + " lv2:port       ?port .\n"
			"?port      rdf:instanceOf ?type .\n"
		"}");
	Objects patch_ports;
	results = query.run(*world->rdf_world(), model, base_uri);
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		const Redland::Node& port = results->get("port");
		const Redland::Node& type = results->get("type");
		if (port.type() == Redland::Node::RESOURCE && type.type() == Redland::Node::RESOURCE) {
			types.insert(make_pair(port.to_string(), type.to_string()));
			patch_ports.insert(make_pair(port.to_string(), Properties()));
		}
	}


	/* Load patch ports */
	query = Redland::Query(*world->rdf_world(), Glib::ustring(
		"SELECT DISTINCT ?port ?key ?val WHERE {\n") +
			subject + " lv2:port ?port .\n"
			"?port      ?key     ?val .\n"
		"}");
	results = query.run(*world->rdf_world(), model, base_uri);
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		const string port_uri  = results->get("port").to_string();
		const Path   port_path = patch_path.child(relative_uri(base_uri, port_uri, false));
		const string key       = results->get("key").to_string();
		Objects::iterator ports_i = patch_ports.find(port_uri);
		if (ports_i == patch_ports.end())
			ports_i = patch_ports.insert(make_pair(port_uri, Properties())).first;
		ports_i->second.insert(make_pair(key, AtomRDF::node_to_atom(model, results->get("val"))));
	}

	std::vector<Objects::iterator> ports_by_index(patch_ports.size(), patch_ports.end());
	for (Objects::iterator i = patch_ports.begin(); i != patch_ports.end(); ++i) {
		Properties::const_iterator index_i = i->second.lower_bound(uris.lv2_index);
		if (index_i == i->second.end()) {
			error << "Patch port has no index" << endl;
			return boost::optional<Raul::Path>();
		}

		if (index_i->second.type() != Atom::INT) {
			error << "Patch port index has non-integer type" << endl;
			return boost::optional<Raul::Path>();
		}

		const int32_t index = index_i->second.get_int32();

		++index_i;
		if (index_i != i->second.end() && index_i->first == uris.lv2_index) {
			error << "Patch port has multiple indices" << endl;
			return boost::optional<Raul::Path>();
		}

		if (index < 0 || static_cast<size_t>(index) >= patch_ports.size()) {
			error << "Patch port index out of range" << endl;
			return boost::optional<Raul::Path>();
		}

		if (ports_by_index[index] != patch_ports.end()) {
			error << "Patch has several ports with index " << index << endl;
			return boost::optional<Raul::Path>();
		}

		ports_by_index[index] = i;
	}

	for (uint32_t index = 0; index < patch_ports.size(); ++index) {
		Objects::iterator i = ports_by_index[index];
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		const Path port_path = patch_path.child(relative_uri(base_uri, i->first, false));
		std::pair<Properties::iterator,Properties::iterator> types_range
				= i->second.equal_range(uris.rdf_type);
		if (types_range.first == i->second.end()) {
			LOG(warn) << "Patch port has no types" << endl;
			continue;
		}
		bool is_input  = false;
		bool is_output = false;
		Raul::Atom* type = 0;
		for (Properties::iterator t = types_range.first; t != types_range.second; ++t) {
			if (t->second.type() != Atom::URI) {
				continue;
			} else if (!strcmp(t->second.get_uri(), uris.lv2_InputPort.c_str())) {
				is_input = true;
			} else if (!strcmp(t->second.get_uri(), uris.lv2_OutputPort.c_str())) {
				is_output = true;
			} else if (!type) {
				type = &t->second;
			} else {
				LOG(error) << "Port has several data types" << endl;
				continue;
			}
		}
		if ((is_input && is_output) || !type) {
			LOG(error) << "Corrupt patch port" << endl;
			continue;
		}

		target->put(port_path, i->second);
	}

	parse_properties(world, target, model, subject_node, patch_path, data);
	parse_connections(world, target, model, subject_node, patch_path);


	/* Enable */
	query = Redland::Query(*world->rdf_world(), Glib::ustring(
		"SELECT DISTINCT ?enabled WHERE {\n")
			+ subject + " ingen:enabled ?enabled .\n"
		"}");

	results = query.run(*world->rdf_world(), model, base_uri);
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		const Redland::Node& enabled_node = results->get("enabled");
		if (enabled_node.is_bool() && enabled_node) {
			target->set_property(patch_path, uris.ingen_enabled, (bool)true);
			break;
		} else {
			LOG(warn) << "Unknown type for ingen:enabled" << endl;
		}
	}

	return patch_path;
}


boost::optional<Path>
Parser::parse_node(
		Ingen::Shared::World*                    world,
		Ingen::Shared::CommonInterface*          target,
		Redland::Model&                          model,
		const Redland::Node&                     subject,
		const Raul::Path&                        path,
		boost::optional<GraphObject::Properties> data)
{
	const LV2URIMap& uris = *world->uris().get();

	/* Get plugin */
	Redland::Query query(*world->rdf_world(), Glib::ustring(
		"SELECT DISTINCT ?plug WHERE {\n")
			+ subject.to_turtle_token() + " rdf:instanceOf ?plug .\n"
		"}");

	SharedPtr<Redland::QueryResults> results(query.run(*world->rdf_world(), model));

	if (results->finished()) {
		LOG(error) << "Node missing mandatory rdf:instanceOf property" << endl;
		return boost::optional<Path>();
	}

	const Redland::Node& plugin_node = results->get("plug");
	if (plugin_node.type() != Redland::Node::RESOURCE) {
		LOG(error) << "Node's rdf:instanceOf property is not a resource" << endl;
		return boost::optional<Path>();
	}

	const string plugin_uri = world->rdf_world()->expand_uri(plugin_node.to_c_string());
	Resource::Properties props;
	props.insert(make_pair(uris.rdf_type,       Raul::URI(uris.ingen_Node)));
	props.insert(make_pair(uris.rdf_instanceOf, Raul::Atom(Raul::Atom::URI, plugin_uri)));
	target->put(path, props);

	parse_properties(world, target, model, subject, path, data);
	return path;
}


bool
Parser::parse_connections(
		Ingen::Shared::World*           world,
		Ingen::Shared::CommonInterface* target,
		Redland::Model&                 model,
		const Redland::Node&            subject,
		const Raul::Path&               parent)
{
	Redland::Query query(*world->rdf_world(), Glib::ustring(
		"SELECT DISTINCT ?src ?dst WHERE {\n")
			+ subject.to_turtle_token() + " ingen:connection  ?connection .\n"
			"?connection  ingen:source      ?src ;\n"
			"             ingen:destination ?dst .\n"
		"}");

	const Glib::ustring& base_uri = model.base_uri().to_string();

	SharedPtr<Redland::QueryResults> results(query.run(*world->rdf_world(), model));
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		const Path src_path(parent.child(relative_uri(base_uri, results->get("src").to_string(), false)));
		const Path dst_path(parent.child(relative_uri(base_uri, results->get("dst").to_string(), false)));
		target->connect(src_path, dst_path);
	}

	return true;
}


bool
Parser::parse_properties(
		Ingen::Shared::World*                    world,
		Ingen::Shared::CommonInterface*          target,
		Redland::Model&                          model,
		const Redland::Node&                     subject_node,
		const Raul::URI&                         uri,
		boost::optional<GraphObject::Properties> data)
{
	const Glib::ustring& subject = subject_node.to_turtle_token();

	Redland::Query query(*world->rdf_world(), Glib::ustring(
		"SELECT DISTINCT ?key ?val WHERE {\n") +
			subject + " ?key ?val .\n"
		"}");

	Resource::Properties properties;
	SharedPtr<Redland::QueryResults> results(query.run(*world->rdf_world(), model));
	for (; !results->finished(); results->next()) {
		Glib::Mutex::Lock lock(world->rdf_world()->mutex());
		const string         key = string(results->get("key"));
		const Redland::Node& val = results->get("val");
		if (skip_property(results->get("key")))
			continue;
		if (!key.empty() && val.type() != Redland::Node::BLANK)
			properties.insert(make_pair(key, AtomRDF::node_to_atom(model, val)));
	}

	target->put(uri, properties);

	// Set passed properties last to override any loaded values
	if (data)
		target->put(uri, data.get());

	return true;
}


bool
Parser::skip_property(const Redland::Node& predicate)
{
	return (predicate.to_string() == "http://drobilla.net/ns/ingen#node"
			|| predicate.to_string() == "http://lv2plug.in/ns/lv2core#port");
}


} // namespace Serialisation
} // namespace Ingen

