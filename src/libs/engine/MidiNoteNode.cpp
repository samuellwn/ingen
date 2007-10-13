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

#include <raul/Array.hpp>
#include <raul/Maid.hpp>
#include <raul/midi_events.h>
#include <cmath>
#include <iostream>
#include "MidiNoteNode.hpp"
#include "MidiBuffer.hpp"
#include "AudioBuffer.hpp"
#include "InputPort.hpp"
#include "OutputPort.hpp"
#include "PluginImpl.hpp"
#include "AudioDriver.hpp"
#include "PatchImpl.hpp"
#include "ProcessContext.hpp"
#include "util.hpp"

using std::cerr; using std::cout; using std::endl;


namespace Ingen {


MidiNoteNode::MidiNoteNode(const string& path, bool polyphonic, PatchImpl* parent, SampleRate srate, size_t buffer_size)
: NodeBase(new PluginImpl(Plugin::Internal, "ingen:note_node"), path, polyphonic, parent, srate, buffer_size),
  _voices(new Raul::Array<Voice>(_polyphony)),
  _prepared_voices(NULL),
  _sustain(false)
{
	_ports = new Raul::Array<PortImpl*>(5);
	
	_midi_in_port = new InputPort(this, "MIDIIn", 0, 1, DataType::MIDI, _buffer_size);
	_ports->at(0) = _midi_in_port;

	_freq_port = new OutputPort(this, "Frequency", 1, _polyphony, DataType::AUDIO, _buffer_size);
	_ports->at(1) = _freq_port;
	
	_vel_port = new OutputPort(this, "Velocity", 2, _polyphony, DataType::AUDIO, _buffer_size);
	_vel_port->set_variable("ingen:minimum", 0.0f);
	_vel_port->set_variable("ingen:maximum", 1.0f);
	_ports->at(2) = _vel_port;
	
	_gate_port = new OutputPort(this, "Gate", 3, _polyphony, DataType::AUDIO, _buffer_size);
	_gate_port->set_variable("ingen:toggled", 1);
	_gate_port->set_variable("ingen:default", 0.0f);
	_ports->at(3) = _gate_port;
	
	_trig_port = new OutputPort(this, "Trigger", 4, _polyphony, DataType::AUDIO, _buffer_size);
	_trig_port->set_variable("ingen:toggled", 1);
	_trig_port->set_variable("ingen:default", 0.0f);
	_ports->at(4) = _trig_port;
	
	PluginImpl* p = const_cast<PluginImpl*>(_plugin);
	p->plug_label("note_in");
	assert(p->uri() == "ingen:note_node");
	p->name("Ingen Note Node (MIDI, OSC)");
}


MidiNoteNode::~MidiNoteNode()
{
	delete _voices;
}


bool
MidiNoteNode::prepare_poly(uint32_t poly)
{
	if (!_polyphonic)
		return true;

	NodeBase::prepare_poly(poly);

	if (_prepared_voices && poly <= _prepared_voices->size())
		return true;

	_prepared_voices = new Raul::Array<Voice>(poly, *_voices);

	return true;
}


bool
MidiNoteNode::apply_poly(Raul::Maid& maid, uint32_t poly)
{
	if (!_polyphonic)
		return true;

	NodeBase::apply_poly(maid, poly);

	if (_prepared_voices) {
		assert(poly <= _prepared_voices->size());
		maid.push(_voices);
		_voices = _prepared_voices;
		_prepared_voices = NULL;
	}

	_polyphony = poly;
	assert(_voices->size() >= _polyphony);

	return true;
}


void
MidiNoteNode::process(ProcessContext& context)
{
	NodeBase::pre_process(context);
	
	double         timestamp = 0;
	uint32_t       size = 0;
	unsigned char* buffer = NULL;

	MidiBuffer* const midi_in = (MidiBuffer*)_midi_in_port->buffer(0);
	assert(midi_in->this_nframes() == context.nframes());

	//cerr << path() << " # input events: " << midi_in->event_count() << endl;

	if (midi_in->event_count() > 0)
	while (midi_in->get_event(&timestamp, &size, &buffer) < context.nframes()) {
		
		const FrameTime time = context.start() + (FrameTime)timestamp;

		if (size >= 3) {
			switch (buffer[0] & 0xF0) {
			case MIDI_CMD_NOTE_ON:
				if (buffer[2] == 0)
					note_off(buffer[1], time, context);
				else
					note_on(buffer[1], buffer[2], time, context);
				break;
			case MIDI_CMD_NOTE_OFF:
				note_off(buffer[1], time, context);
				break;
			case MIDI_CMD_CONTROL:
				switch (buffer[1]) {
				case MIDI_CTL_ALL_NOTES_OFF:
				case MIDI_CTL_ALL_SOUNDS_OFF:
					all_notes_off(time, context);
					break;
				case MIDI_CTL_SUSTAIN:
					if (buffer[2] > 63)
						sustain_on(time, context);
					else
						sustain_off(time, context);
					break;
				case MIDI_CMD_BENDER:
					// ?
					break;
				default:
					//cerr << "Ignored controller " << buffer[1] << endl;
					break;
				}
				break;
			default:
				fprintf(stderr, "Unknown (size %d) MIDI event %X\n", size, buffer[0]);
				break;
			}
		} else {
			fprintf(stderr, "Unknown (size %d) MIDI event %X\n", size, buffer[0]);
		}

		if (midi_in->increment() == midi_in->this_nframes())
			break;
	}
	
	NodeBase::post_process(context);
}


void
MidiNoteNode::note_on(uchar note_num, uchar velocity, FrameTime time, ProcessContext& context)
{
	assert(time >= context.start() && time <= context.end());
	assert(time - context.start() < _buffer_size);
	assert(note_num <= 127);

	Key*   key         = &_keys[note_num];
	Voice* voice       = NULL;
	uint32_t voice_num = 0;
	
	if (key->state != Key::OFF) {
		cerr << "[MidiNoteNode] Double note.  Who be sendin dem crazy midis?" << endl;
		return;
	}

	// Look for free voices
	for (uint32_t i=0; i < _polyphony; ++i) {
		if ((*_voices)[i].state == Voice::Voice::FREE) {
			voice = &(*_voices)[i];
			voice_num = i;
			break;
		}
	}

	// If we didn't find a free one, steal the oldest
	if (voice == NULL) {
		voice_num = 0;
		voice = &(*_voices)[0];
		jack_nframes_t oldest_time = (*_voices)[0].time;
		for (uint32_t i=1; i < _polyphony; ++i) {
			if ((*_voices)[i].time < oldest_time) {
				voice = &(*_voices)[i];
				voice_num = i;
				oldest_time = voice->time;
			}
		}
	}		
	assert(voice != NULL);
	assert(voice == &(*_voices)[voice_num]);

	//cerr << "[MidiNoteNode] Note " << (int)note_num << " on @ " << time
	//	<< ". Voice " << voice_num << " / " << _polyphony << endl;
	
	// Update stolen key, if applicable
	if (voice->state == Voice::Voice::ACTIVE) {
		assert(_keys[voice->note].state == Key::ON_ASSIGNED);
		assert(_keys[voice->note].voice == voice_num);
		_keys[voice->note].state = Key::Key::ON_UNASSIGNED;
		//cerr << "[MidiNoteNode] Stole voice " << voice_num << endl;
	}
	
	// Store key information for later reallocation on note off
	key->state = Key::Key::ON_ASSIGNED;
	key->voice = voice_num;
	key->time  = time;

	// Trigger voice
	voice->state = Voice::Voice::ACTIVE;
	voice->note  = note_num;
	voice->time  = time;
	
	assert(_keys[voice->note].state == Key::Key::ON_ASSIGNED);
	assert(_keys[voice->note].voice == voice_num);
	
	// FIXME FIXME FIXME
	
	SampleCount offset = time - context.start();

	// one-sample jitter hack to avoid having to deal with trigger sample "next time"
	if (offset == (SampleCount)(_buffer_size-1))
		--offset;
	
	((AudioBuffer*)_freq_port->buffer(voice_num))->set(note_to_freq(note_num), offset);
	((AudioBuffer*)_vel_port->buffer(voice_num))->set(velocity/127.0, offset);
	((AudioBuffer*)_gate_port->buffer(voice_num))->set(1.0f, offset);
	
	// trigger (one sample)
	((AudioBuffer*)_trig_port->buffer(voice_num))->set(1.0f, offset, offset);
	((AudioBuffer*)_trig_port->buffer(voice_num))->set(0.0f, offset+1);

	assert(key->state == Key::Key::ON_ASSIGNED);
	assert(voice->state == Voice::Voice::ACTIVE);
	assert(key->voice == voice_num);
	assert((*_voices)[key->voice].note == note_num);
}


void
MidiNoteNode::note_off(uchar note_num, FrameTime time, ProcessContext& context)
{
	assert(time >= context.start() && time <= context.end());
	assert(time - context.start() < _buffer_size);

	Key* key = &_keys[note_num];
	
	//cerr << "[MidiNoteNode] Note " << (int)note_num << " off @ " << time << endl;

	if (key->state == Key::ON_ASSIGNED) {
		// Assigned key, turn off voice and key
		if ((*_voices)[key->voice].state == Voice::ACTIVE) {
			assert((*_voices)[key->voice].note == note_num);

			if ( ! _sustain) {
				//cerr << "... free voice " << key->voice << endl;
				free_voice(key->voice, time, context);
			} else {
				//cerr << "... hold voice " << key->voice << endl;
				(*_voices)[key->voice].state = Voice::HOLDING;
			}

		} else {
#ifndef NDEBUG
			cerr << "WARNING: Assigned key, but voice not active" << endl;
#endif
		}
	}

	key->state = Key::OFF;
}

	
void
MidiNoteNode::free_voice(uint32_t voice, FrameTime time, ProcessContext& context)
{
	assert(time >= context.start() && time <= context.end());
	assert(time - context.start() < _buffer_size);

	// Find a key to reassign to the freed voice (the newest, if there is one)
	Key*  replace_key     = NULL;
	uchar replace_key_num = 0;

	for (uchar i = 0; i <= 127; ++i) {
		if (_keys[i].state == Key::ON_UNASSIGNED) {
			if (replace_key == NULL || _keys[i].time > replace_key->time) {
				replace_key = &_keys[i];
				replace_key_num = i;
			}
		}
	}

	if (replace_key != NULL) {  // Found a key to assign to freed voice
		assert(&_keys[replace_key_num] == replace_key);
		assert(replace_key->state == Key::ON_UNASSIGNED);
		
		// Change the freq but leave the gate high and don't retrigger
		((AudioBuffer*)_freq_port->buffer(voice))->set(note_to_freq(replace_key_num), time - context.start());

		replace_key->state = Key::ON_ASSIGNED;
		replace_key->voice = voice;
		_keys[(*_voices)[voice].note].state = Key::ON_UNASSIGNED;
		(*_voices)[voice].note = replace_key_num;
		(*_voices)[voice].state = Voice::ACTIVE;
	} else {
		// No new note for voice, deactivate (set gate low)
		//cerr << "[MidiNoteNode] Note off. Key " << (int)note_num << ", Voice " << voice << " Killed" << endl;
		((AudioBuffer*)_gate_port->buffer(voice))->set(0.0f, time - context.start());
		(*_voices)[voice].state = Voice::FREE;
	}
}


void
MidiNoteNode::all_notes_off(FrameTime time, ProcessContext& context)
{
	assert(time >= context.start() && time <= context.end());
	assert(time - context.start() < _buffer_size);

	//cerr << "All notes off @ " << offset << endl;

	// FIXME: set all keys to Key::OFF?
	
	for (uint32_t i=0; i < _polyphony; ++i) {
		((AudioBuffer*)_gate_port->buffer(i))->set(0.0f, time - context.start());
		(*_voices)[i].state = Voice::FREE;
	}
}


float
MidiNoteNode::note_to_freq(int num)
{
	static const float A4 = 440.0f;
	if (num >= 0 && num <= 119)
		return A4 * powf(2.0f, (float)(num - 57.0f) / 12.0f);
	return 1.0f;  // Some LADSPA plugins don't like freq=0
}


void
MidiNoteNode::sustain_on(FrameTime time, ProcessContext& context)
{
	_sustain = true;
}


void
MidiNoteNode::sustain_off(FrameTime time, ProcessContext& context)
{
	assert(time >= context.start() && time <= context.end());
	assert(time - context.start() < _buffer_size);

	_sustain = false;
	
	for (uint32_t i=0; i < _polyphony; ++i)
		if ((*_voices)[i].state == Voice::HOLDING)
			free_voice(i, time, context);
}


} // namespace Ingen

