/*
  This file is part of Ingen.
  Copyright 2007-2012 David Robillard <http://drobilla.net/>

  Ingen is free software: you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free
  Software Foundation, either version 3 of the License, or any later version.

  Ingen is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for details.

  You should have received a copy of the GNU Affero General Public License
  along with Ingen.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

#include "ganv/Port.hpp"
#include "ingen/Log.hpp"
#include "ingen/client/PluginModel.hpp"
#include "ingen/client/PortModel.hpp"
#include "ingen/serialisation/Parser.hpp"

#include "App.hpp"
#include "Configuration.hpp"
#include "Port.hpp"

using namespace std;

namespace Ingen {
namespace GUI {

using namespace Ingen::Client;

Configuration::Configuration(App& app)
	// Colours from the Tango palette with modified V
	: _app(app)
	, _name_style(HUMAN)
	, _audio_port_color(0x4A8A0EFF) // Green
	, _control_port_color(0x244678FF) // Blue
	, _cv_port_color(0x248780FF) // Teal (between audio and control)
	, _event_port_color(0x960909FF) // Red
	, _string_port_color(0x5C3566FF) // Plum
{
}

Configuration::~Configuration()
{
}

/** Loads settings from the rc file.  Passing no parameter will load from
 * the default location.
 */
void
Configuration::load_settings(string filename)
{
	/* ... */
}

/** Saves settings to rc file.  Passing no parameter will save to the
 * default location.
 */
void
Configuration::save_settings(string filename)
{
	/* ... */
}

/** Applies the current loaded settings to whichever parts of the app
 * need updating.
 */
void
Configuration::apply_settings()
{
	/* ... */
}

uint32_t
Configuration::get_port_color(const Client::PortModel* p)
{
	assert(p != NULL);
	const URIs& uris = _app.uris();
	if (p->is_a(uris.lv2_AudioPort)) {
		return _audio_port_color;
	} else if (p->is_a(uris.lv2_ControlPort)) {
		return _control_port_color;
	} else if (p->is_a(uris.lv2_CVPort)) {
		return _cv_port_color;
	} else if (p->supports(uris.atom_String)) {
		return _string_port_color;
	} else if (_app.can_control(p)) {
		return _control_port_color;
	} else if (p->is_a(uris.atom_AtomPort)) {
		return _event_port_color;
	}

	_app.log().warn(Raul::fmt("No known port type for %1%\n") % p->path());
	return 0x666666FF;
}

} // namespace GUI
} // namespace Ingen
