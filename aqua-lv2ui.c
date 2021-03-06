#include <vector>
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/extensions/ui/ui.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>
#include <lv2/lv2plug.in/ns/ext/patch/patch.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/ext/log/log.h>
#define HTTPSERVER_IMPL
#define AQUA_IMPL
#include "lv2_external_ui.h"
#include "aqua.h"
#include "process.hpp"


#define PATH_MAX 4096
#define SFIZZ__sfzFile "https://github.com/atsushieno/aqua:sfzfile"

extern "C" {

// rewrite this part to whichever port your plugin is listening.
#ifndef AQUA_LV2_CONTROL_PORT
#define AQUA_LV2_CONTROL_PORT 0 //  0 is sfizz control-in port.
#endif
#ifndef AQUA_LV2_NOTIFY_PORT
#define AQUA_LV2_NOTIFY_PORT 1 //  1 is sfizz notification port.
#endif

typedef struct aqualv2ui_tag {
	// This is tricky, but do NOT move this member from TOP of this struct, as
	// this plugin implementation makes use of the fact that the LV2_External_UI_Widget* also
	// points to the address of this containing `aqualv2ui` struct.
	// It is due to the fact that LV2_External_UI_Widget is not designed well and lacks
	// "context" parameter to be passed to its callbacks and therefore we have no idea
	// "which" UI instance to manipulate.
	LV2_External_UI_Widget extui;

	pthread_t ui_launcher_thread;
	std::unique_ptr<TinyProcessLib::Process> aqua_process;

	const char *plugin_uri;
	char *bundle_path;
	LV2_URID_Map *urid_map;
	LV2_Log_Log *log;

	LV2UI_Write_Function write_function;
	LV2UI_Controller controller;
	const LV2_Feature *const *features;
	int urid_atom, urid_frame_time, urid_midi_event, urid_atom_object, urid_patch_set, urid_patch_get, urid_patch_property,
		urid_patch_value, urid_atom_urid, urid_atom_path, urid_sfzfile, urid_voice_message, urid_atom_event_transfer;
	bool is_visible_now{false};

	char atom_buffer[PATH_MAX + 256];
} aqualv2ui;

char* fill_atom_message_base(aqualv2ui* a, LV2_Atom_Sequence* seq)
{
	seq->atom.type = a->urid_midi_event;
	seq->body.unit = a->urid_frame_time;
	seq->body.pad = 0; // only for cleanness

	auto ev = (LV2_Atom_Event*) (seq + 1);
	ev->time.frames = 0; // dummy
	auto msg = (char*) &seq->body; // really?

	seq->atom.size = sizeof(LV2_Atom) + 3; // really? shouldn't this be werapped in LV2_Atom_Event?

	return msg;
}

// Unlike the name implies, it is actually not that general, but more specific to sfizz...
// See https://github.com/sfztools/sfizz/blob/a1fdfa2e/lv2/sfizz.c#L491 for MIDI message processing details.
void aqualv2_cc_callback(void* context, int cc, int value)
{
	auto a = (aqualv2ui*) context;

	auto seq = (LV2_Atom_Sequence*) a->atom_buffer;
	char* msg = fill_atom_message_base(a, seq);
	msg[0] = 0xB0; // channel does not matter on sfizz
	msg[1] = (char) cc;
	msg[2] = (char) value;
	
	a->write_function(a->controller, AQUA_LV2_CONTROL_PORT, seq->atom.size + sizeof(LV2_Atom), a->urid_atom_event_transfer, a->atom_buffer);
}

// Unlike the name implies, it is actually not that general, but more specific to sfizz...
// See https://github.com/sfztools/sfizz/blob/a1fdfa2e/lv2/sfizz.c#L491 for MIDI message processing details.
void aqualv2_note_callback(void* context, int key, int velocity)
{
	auto a = (aqualv2ui*) context;
	
	auto seq = (LV2_Atom_Sequence*) a->atom_buffer;
	char* msg = fill_atom_message_base(a, seq);
	msg[0] = velocity == 0 ? 0x80 : 0x90; // channel does not matter on sfizz
	msg[1] = (char) key;
	msg[2] = (char) velocity;
	
	a->write_function(a->controller, AQUA_LV2_CONTROL_PORT, seq->atom.size + sizeof(LV2_Atom), a->urid_atom_event_transfer, a->atom_buffer);
}

void aqualv2_select_sfz_callback(void* context, const char* sfzfile)
{
	auto a = (aqualv2ui*) context;

#if 0
	// It is based on the code in sfizz that generates port notification,
	// but this does NOT generate the right Atom buffer.
	// This does not result in the right atom type at top level,
	// and I have no further idea on how so.
	// The entire Atom API reference is far from complete and not reliable.
	auto seq = (LV2_Atom_Sequence*) a->atom_buffer;

	memset(a->atom_buffer, 0, PATH_MAX + 256);
	LV2_Atom_Forge forge;
	lv2_atom_forge_init(&forge, a->urid_map);
	lv2_atom_forge_set_buffer(&forge, a->atom_buffer, PATH_MAX + 256);
	LV2_Atom_Forge_Frame seq_frame;
	lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
	lv2_atom_forge_frame_time(&forge, 0);
	LV2_Atom_Forge_Frame obj_frame;
	lv2_atom_forge_object(&forge, &obj_frame, 0, a->urid_patch_set);
	lv2_atom_forge_key(&forge, a->urid_patch_property);
	lv2_atom_forge_urid(&forge, a->urid_sfzfile);
	lv2_atom_forge_key(&forge, a->urid_patch_value);
	lv2_atom_forge_path(&forge, sfzfile, (uint32_t)strlen(sfzfile));
	lv2_atom_forge_pop(&forge, &obj_frame);
	lv2_atom_forge_pop(&forge, &seq_frame);
#else
	// Since Atom forge API is not documented appropriately, I avoid using it here.
	//
	// The event should contain an atom:Object whose body object type
	// is a patch:Set whose patch:property is sfzfile
	auto seq = (LV2_Atom_Sequence*) a->atom_buffer;
	memset(seq, 0, PATH_MAX + 256);
	seq->atom.type = a->urid_atom_object;
	auto body = (LV2_Atom_Object_Body*) &seq->body;
	body->otype = a->urid_patch_set;
	auto propBody = (LV2_Atom_Property_Body*) lv2_atom_object_begin(body);
	propBody->key = a->urid_patch_property;
	propBody->value.type = a->urid_atom_urid;
	propBody->value.size = sizeof(LV2_URID);
	((LV2_Atom_URID *) &propBody->value)->body = a->urid_sfzfile;

	propBody = lv2_atom_object_next(propBody);
	propBody->key = a->urid_patch_value;
	propBody->value.type = a->urid_atom_path;
	propBody->value.size = strlen(sfzfile) + 1;
	auto filenameDst = ((char*) &propBody->value) + sizeof(LV2_Atom);
	strncpy(filenameDst, sfzfile, strlen(sfzfile));
	filenameDst[strlen(sfzfile)] = '\0';

	seq->atom.size = sizeof(LV2_Atom_Event) + sizeof(LV2_Atom_Object) + sizeof(LV2_Atom_Property) * 2 + strlen(sfzfile);
#endif

	a->write_function(a->controller, AQUA_LV2_CONTROL_PORT, seq->atom.size + sizeof(LV2_Atom), a->urid_atom_event_transfer, a->atom_buffer);
}

void send_sfzfile_request(aqualv2ui* a)
{
	// see aqualv2_select_sfz_callback(), the same notes on why manually implement atom buffering applies...
	auto seq = (LV2_Atom_Sequence*) a->atom_buffer;
	memset(seq, 0, PATH_MAX + 256);
	seq->atom.type = a->urid_atom_object;
	auto body = (LV2_Atom_Object_Body*) &seq->body;
	body->otype = a->urid_patch_get;
	auto propBody = (LV2_Atom_Property_Body*) lv2_atom_object_begin(body);
	propBody->key = a->urid_patch_property;
	propBody->value.type = a->urid_atom_urid;
	propBody->value.size = sizeof(LV2_URID);
	((LV2_Atom_URID *) &propBody->value)->body = a->urid_sfzfile;

	seq->atom.size = sizeof(LV2_Atom_Event) + sizeof(LV2_Atom_Object) + sizeof(LV2_Atom_Property);

	a->write_function(a->controller, AQUA_LV2_CONTROL_PORT, seq->atom.size + sizeof(LV2_Atom), a->urid_atom_event_transfer, a->atom_buffer);
}

void extui_show_callback(LV2_External_UI_Widget* widget) {
	auto aui = (aqualv2ui*) (void*) widget;

	aui->aqua_process->write("show\n");
}

void extui_hide_callback(LV2_External_UI_Widget* widget) {
	auto aui = (aqualv2ui*) (void*) widget;

	aui->aqua_process->write("hide\n");
}

void extui_run_callback(LV2_External_UI_Widget* widget) {
	// nothing to do here...
}

aqualv2_initialized_callback(void* context)
{
	// should we use Worker? It seems to work though.
	send_sfzfile_request((aqualv2ui*) context);
}

void* runloop_aqua_host(void* context)
{
	auto aui = (aqualv2ui*) context;
	std::string cmd = std::string{} + aui->bundle_path + "/aqua-host --plugin";
	aui->aqua_process.reset(new TinyProcessLib::Process(cmd, "", [&aui](const char* bytes, size_t n) {
		if (n <= 0)
			return;
		int v1, v2;
		const char *sfz;
		switch (bytes[0]) {
			case 'I':
				aqualv2_initialized_callback(aui);
				break;
			case 'N':
				sscanf(bytes + 1, "#%x,#%x", &v1, &v2);
				aqualv2_note_callback(aui, v1, v2);
				break;
			case 'C':
				sscanf(bytes + 1, "#%x,#%x", &v1, &v2);
				aqualv2_cc_callback(aui, v1, v2);
				break;
			case 'P':
				sfz = bytes + 2;
				*((char*)strchr(sfz, '\n')) = '\0';
				aqualv2_select_sfz_callback(aui, sfz);
				break;
			default:
				aui->log->printf(aui->log, LV2_LOG__Warning, "Unrecognized command sent by aqua-host: %s\n", bytes);
		}
	}, nullptr, true));

	return nullptr;
}

int aqua_lv2ui_default_vprintf(LV2_Log_Handle handle, LV2_URID type, const char *fmt, va_list ap)
{
	vprintf(fmt, ap);
}

int aqua_lv2ui_default_printf(LV2_Log_Handle handle, LV2_URID type, const char *fmt,...)
{
	va_list args;
	va_start(args, fmt);
	aqua_lv2ui_default_vprintf(handle, type, fmt, args);
	va_end(args);
}

LV2UI_Handle aqua_lv2ui_instantiate(
	const LV2UI_Descriptor *descriptor,
	const char *plugin_uri,
	const char *bundle_path,
	LV2UI_Write_Function write_function,
	LV2UI_Controller controller,
	LV2UI_Widget *widget,
	const LV2_Feature *const *features)
{
	static LV2_Log_Log default_log{nullptr, aqua_lv2ui_default_printf, aqua_lv2ui_default_vprintf};

	auto ret = new aqualv2ui();
	ret->log = &default_log;
	ret->plugin_uri = plugin_uri;
	ret->bundle_path = strdup(bundle_path);
	ret->write_function = write_function;
	ret->controller = controller;
	ret->features = features;

	auto extui = &ret->extui;
	extui->show = extui_show_callback;
	extui->hide = extui_hide_callback;
	extui->run = extui_run_callback;

	for (int i = 0; features[i]; i++) {
		auto f = features[i];
		if (strcmp(f->URI, LV2_LOG__log) == 0)
			ret->log = (LV2_Log_Log*) f->data;
		if (strcmp(f->URI, LV2_URID__map) == 0) {
			auto urid = (LV2_URID_Map*) f->data;
			ret->urid_map = urid;
			ret->urid_atom = urid->map(urid->handle, LV2_ATOM_URI);
			ret->urid_frame_time = urid->map(urid->handle, LV2_ATOM__frameTime);
			ret->urid_midi_event = urid->map(urid->handle, LV2_MIDI__MidiEvent);
			ret->urid_atom_object = urid->map(urid->handle, LV2_ATOM__Object);
			ret->urid_patch_set = urid->map(urid->handle, LV2_PATCH__Set);
			ret->urid_patch_get = urid->map(urid->handle, LV2_PATCH__Get);
			ret->urid_patch_property = urid->map(urid->handle, LV2_PATCH__property);
			ret->urid_patch_value = urid->map(urid->handle, LV2_PATCH__value);
			ret->urid_atom_urid = urid->map(urid->handle, LV2_ATOM__URID);
			ret->urid_atom_path = urid->map(urid->handle, LV2_ATOM__Path);
			ret->urid_sfzfile = urid->map(urid->handle, SFIZZ__sfzFile);
			ret->urid_voice_message = urid->map(urid->handle, LV2_MIDI__VoiceMessage);
			ret->urid_atom_event_transfer = urid->map(urid->handle, LV2_ATOM__eventTransfer);
			break;
		}
	}

	pthread_t thread;
	pthread_create(&thread, nullptr, runloop_aqua_host, ret);
	pthread_setname_np(thread, "aqua_lv2ui_host_launcher");
	ret->ui_launcher_thread = thread;
	while (ret->aqua_process.get() == nullptr)
		usleep(1000);

	*widget = &ret->extui;

	return ret;
}

void aqua_lv2ui_cleanup(LV2UI_Handle ui)
{
	auto aui = (aqualv2ui*) ui;
	aui->aqua_process->kill();
	aui->aqua_process->write("quit\n");
	free(aui->bundle_path);
	free(aui);
}

// It is invoked by host when the UI is instantiated. sfizz port 4-8 values will be sent in float protocol.
void aqua_lv2ui_port_event(LV2UI_Handle ui, uint32_t port_index, uint32_t buffer_size, uint32_t format, const void *buffer)
{
	auto a = (aqualv2ui*) ui;
	// FIXME: reflect the full value changes back to UI.
	switch (format) {
	case 0:
		a->log->printf(a->log, LV2_LOG__Note, "aqua_lv2ui: port event received. port: %d, value: %f\n", port_index, *(float*) buffer);
		break;
	default:
		if (port_index == AQUA_LV2_NOTIFY_PORT) {
			auto obj = (LV2_Atom_Object *) buffer;

			if (obj->body.otype != a->urid_patch_set) {
				a->log->printf(a->log, LV2_LOG__Warning, "aqua_lv2ui: Unknown object of type is notified. Only patch:set is expected: %d\n", obj->body.otype);
				break;
			}
			const LV2_Atom *property = nullptr;
			lv2_atom_object_get(obj, a->urid_patch_property, &property, 0);
			if (property == nullptr) {
				a->log->printf(a->log, LV2_LOG__Warning, "aqua_lv2ui: missing patch property URI in patch:get");
				break;
			} else if (property->type != a->urid_atom_urid) {
				a->log->printf(a->log, LV2_LOG__Error, "aqua_lv2ui: patch property is not URID in patch:get: %d\n", property->type);
				break;
			}
			const uint32_t key = ((const LV2_Atom_URID *) property)->body;
			const LV2_Atom *atom = nullptr;
			lv2_atom_object_get(obj, a->urid_patch_value, &atom, 0);
			if (atom == nullptr) {
				a->log->printf(a->log, LV2_LOG__Error, "aqua_lv2ui: patch value is missing in patch:get\n");
			} else if (key != a->urid_sfzfile) {
				a->log->printf(a->log, LV2_LOG__Warning, "aqua_lv2ui: patch value was not sfzfile in patch:get: %d\n", key);
			} else {
				const uint32_t original_atom_size = lv2_atom_total_size((const LV2_Atom *) atom);
				const uint32_t null_terminated_atom_size = original_atom_size + 1;
				char atom_buffer[PATH_MAX];
				memcpy(&atom_buffer, atom, original_atom_size);
				atom_buffer[original_atom_size] = 0; // Null terminate the string for safety

				std::string cmd{"SFZ "};
				cmd = cmd + (atom_buffer + sizeof(LV2_Atom)) + '\n';
				a->aqua_process->write(cmd.c_str());
			}
		}
		else
			a->log->printf(a->log, LV2_LOG__Warning, "aqua_lv2ui: unrecognized port event received. port: %d, buffer size: %d bytes\n", port_index, buffer_size);
		break;
	}
}

const void * extension_data(const char *uri) {
	//printf("!!! extension_data requested : %s\n", uri);
	return nullptr;
}

LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index)
{
	static const LV2UI_Descriptor uidesc{
		"https://github.com/atsushieno/aqua#ui",
		aqua_lv2ui_instantiate,
		aqua_lv2ui_cleanup,
		aqua_lv2ui_port_event,
		extension_data};

	return &uidesc;
}

} // extern "C"
