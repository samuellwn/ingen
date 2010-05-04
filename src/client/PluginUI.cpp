/* This file is part of Ingen.
 * Copyright (C) 2007-2009 Dave Robillard <http://drobilla.net>
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

#include "raul/log.hpp"
#include "event.lv2/event-helpers.h"
#include "atom.lv2/atom.h"
#include "shared/LV2Features.hpp"
#include "shared/LV2URIMap.hpp"
#include "shared/LV2Atom.hpp"
#include "PluginUI.hpp"
#include "NodeModel.hpp"
#include "PortModel.hpp"

using namespace std;
using namespace Raul;
using Ingen::Shared::LV2URIMap;
using Ingen::Shared::LV2Features;

namespace Ingen {
namespace Client {

static void
lv2_ui_write(LV2UI_Controller controller,
             uint32_t         port_index,
             uint32_t         buffer_size,
             uint32_t         format,
             const void*      buffer)
{
	/*fprintf(stderr, "lv2_ui_write (format %u):\n", format);
	fprintf(stderr, "RAW:\n");
	for (uint32_t i=0; i < buffer_size; ++i) {
		unsigned char byte = ((unsigned char*)buffer)[i];
		if (byte >= 32 && byte <= 126)
			fprintf(stderr, "%c  ", ((unsigned char*)buffer)[i]);
		else
			fprintf(stderr, "%2X ", ((unsigned char*)buffer)[i]);
	}
	fprintf(stderr, "\n");*/

	PluginUI* ui = (PluginUI*)controller;

	SharedPtr<PortModel> port = ui->node()->ports()[port_index];

	const Shared::LV2URIMap& uris = *ui->world()->uris().get();

	// float (special case, always 0)
	if (format == 0) {
		assert(buffer_size == 4);
		if (*(float*)buffer == port->value().get_float())
			return; // do nothing (handle stupid plugin UIs that feed back)

		ui->world()->engine()->set_property(port->path(), uris.ingen_value, Atom(*(float*)buffer));

	} else if (format == uris.ui_format_events.id) {
		LV2_Event_Buffer*  buf = (LV2_Event_Buffer*)buffer;
		LV2_Event_Iterator iter;
		uint8_t*           data;
		lv2_event_begin(&iter, buf);
		while (lv2_event_is_valid(&iter)) {
			LV2_Event* const ev = lv2_event_get(&iter, &data);
			if (ev->type == uris.midi_event.id) {
				// FIXME: bundle multiple events by writing an entire buffer here
				ui->world()->engine()->set_property(port->path(), uris.ingen_value,
					Atom("http://lv2plug.in/ns/ext/midi#MidiEvent", ev->size, data));
			} else {
				warn << "Unable to send event type " << ev->type <<
					" over OSC, ignoring event" << endl;
			}

			lv2_event_increment(&iter);
		}

	} else if (format == uris.object_transfer.id) {
		LV2_Atom* buf = (LV2_Atom*)buffer;
		Raul::Atom val;
		Shared::LV2Atom::to_atom(uris, buf, val);
		ui->world()->engine()->set_property(port->path(), uris.ingen_value, val);

	} else {
		warn << "Unknown value format " << format
			<< ", either plugin " << ui->node()->plugin()->uri() << " is broken"
			<< " or this is an Ingen bug" << endl;
	}
}


PluginUI::PluginUI(Ingen::Shared::World* world,
                   SharedPtr<NodeModel>  node)
	: _world(world)
	, _node(node)
	, _instance(NULL)
{
}


PluginUI::~PluginUI()
{
	if (_instance) {
		Glib::Mutex::Lock lock(PluginModel::rdf_world()->mutex());
		slv2_ui_instance_free(_instance);
	}
}


SharedPtr<PluginUI>
PluginUI::create(Ingen::Shared::World* world,
                 SharedPtr<NodeModel>  node,
                 SLV2Plugin            plugin)
{
	Glib::Mutex::Lock lock(PluginModel::rdf_world()->mutex());
	SharedPtr<PluginUI> ret;

	SLV2Value gtk_gui_uri = slv2_value_new_uri(world->slv2_world(),
		"http://lv2plug.in/ns/extensions/ui#GtkUI");

	SLV2UIs uis = slv2_plugin_get_uis(plugin);
	SLV2UI  ui  = NULL;

	if (slv2_values_size(uis) > 0) {
		for (unsigned i=0; i < slv2_uis_size(uis); ++i) {
			SLV2UI this_ui = slv2_uis_get_at(uis, i);
			if (slv2_ui_is_a(this_ui, gtk_gui_uri)) {
				ui = this_ui;
				break;
			}
		}
	}

	if (ui) {
		info << "Found GTK Plugin UI: " << slv2_ui_get_uri(ui) << endl;
		ret = SharedPtr<PluginUI>(new PluginUI(world, node));
		ret->_features = world->lv2_features()->lv2_features(node.get());
		SLV2UIInstance inst = slv2_ui_instantiate(
				plugin, ui, lv2_ui_write, ret.get(), ret->_features->array());

		if (inst) {
			ret->set_instance(inst);
		} else {
			error << "Failed to instantiate Plugin UI" << endl;
			ret = SharedPtr<PluginUI>();
		}
	}

	slv2_value_free(gtk_gui_uri);
	return ret;
}


} // namespace Client
} // namespace Ingen
