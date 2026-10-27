/**************************************************************************/
/*  movie_writer.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "movie_writer.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/templates/rb_set.h"
#include "scene/main/window.h"
#include "servers/audio/audio_driver_dummy.h"
#include "servers/display/display_server_enums.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

MovieWriter *MovieWriter::writers[MovieWriter::MAX_WRITERS];
uint32_t MovieWriter::writer_count = 0;

void MovieWriter::add_writer(MovieWriter *p_writer) {
	ERR_FAIL_COND(writer_count == MAX_WRITERS);
	writers[writer_count++] = p_writer;
}

MovieWriter *MovieWriter::find_writer_for_file(const String &p_file) {
	for (int32_t i = writer_count - 1; i >= 0; i--) { // More recent last, to have override ability.
		if (writers[i]->handles_file(p_file)) {
			return writers[i];
		}
	}
	return nullptr;
}

uint32_t MovieWriter::get_audio_mix_rate() const {
	uint32_t ret = 48000;
	GDVIRTUAL_CALL(_get_audio_mix_rate, ret);
	return ret;
}
AudioServer::SpeakerMode MovieWriter::get_audio_speaker_mode() const {
	AudioServer::SpeakerMode ret = AudioServer::SPEAKER_MODE_STEREO;
	GDVIRTUAL_CALL(_get_audio_speaker_mode, ret);
	return ret;
}

Error MovieWriter::write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) {
	Error ret = ERR_UNCONFIGURED;
	GDVIRTUAL_CALL(_write_begin, p_movie_size, p_fps, p_base_path, ret);
	return ret;
}

Error MovieWriter::write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data) {
	Error ret = ERR_UNCONFIGURED;
	GDVIRTUAL_CALL(_write_frame, p_image, p_audio_data, ret);
	return ret;
}

void MovieWriter::write_end() {
	GDVIRTUAL_CALL(_write_end);
}

bool MovieWriter::handles_file(const String &p_path) const {
	bool ret = false;
	GDVIRTUAL_CALL(_handles_file, p_path, ret);
	return ret;
}

void MovieWriter::get_supported_extensions(List<String> *r_extensions) const {
	Vector<String> exts;
	GDVIRTUAL_CALL(_get_supported_extensions, exts);
	for (int i = 0; i < exts.size(); i++) {
		r_extensions->push_back(exts[i]);
	}
}

void MovieWriter::begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) {
	project_name = GLOBAL_GET("application/config/name");
	movie_size = p_movie_size;

	print_line(vformat(U"Movie Maker mode enabled, recording movie in %s×%s @ %d FPS...", movie_size.width, movie_size.height, p_fps));

	// Check for available disk space and warn the user if needed.
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	String path = p_base_path.get_base_dir();
	if (path.is_relative_path()) {
		path = "res://" + path;
	}
	dir->open(path);
	if (dir->get_space_left() < 10 * Math::pow(1024.0, 3.0)) {
		// Less than 10 GiB available.
		WARN_PRINT(vformat("Current available space on disk is low (%s). MovieWriter will fail during movie recording if the disk runs out of available space.", String::humanize_size(dir->get_space_left())));
	}

	cpu_time = 0.0f;
	gpu_time = 0.0f;
	encoding_time_usec = 0;

	mix_rate = get_audio_mix_rate();
	AudioDriverDummy::get_dummy_singleton()->set_mix_rate(mix_rate);
	AudioDriverDummy::get_dummy_singleton()->set_speaker_mode(AudioDriver::SpeakerMode(get_audio_speaker_mode()));
	fps = p_fps;
	if ((mix_rate % fps) != 0) {
		WARN_PRINT("MovieWriter's audio mix rate (" + itos(mix_rate) + ") can not be divided by the recording FPS (" + itos(fps) + "). Audio may go out of sync over time.");
	}

	audio_channels = AudioDriverDummy::get_dummy_singleton()->get_channels();
	audio_mix_buffer.resize(mix_rate * audio_channels / fps);

	write_begin(movie_size, p_fps, p_base_path);
	recording_start_usec = Time::get_singleton()->get_ticks_usec();
	next_frame_id = 0;
	pending_readbacks = 0;
}

void MovieWriter::_bind_methods() {
	ClassDB::bind_static_method("MovieWriter", D_METHOD("add_writer", "writer"), &MovieWriter::add_writer);

	GDVIRTUAL_BIND(_get_audio_mix_rate)
	GDVIRTUAL_BIND(_get_audio_speaker_mode)

	GDVIRTUAL_BIND(_handles_file, "path")
	GDVIRTUAL_BIND(_get_supported_extensions)

	GDVIRTUAL_BIND(_write_begin, "movie_size", "fps", "base_path")
	GDVIRTUAL_BIND(_write_frame, "frame_image", "audio_frame_block")
	GDVIRTUAL_BIND(_write_end)

	GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/mix_rate", PROPERTY_HINT_RANGE, "8000,192000,1,suffix:Hz"), 48000);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/speaker_mode", PROPERTY_HINT_ENUM, "Stereo,3.1,5.1,7.1"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "editor/movie_writer/video_quality", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), 0.75);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/audio_bit_depth", PROPERTY_HINT_ENUM, "16:16,32:32"), 16);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "editor/movie_writer/ogv/audio_quality", PROPERTY_HINT_RANGE, "-0.1,1.0,0.01"), 0.5);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/ogv/encoding_speed", PROPERTY_HINT_ENUM, "Fastest (Lowest Efficiency):4,Fast (Low Efficiency):3,Slow (High Efficiency):2,Slowest (Highest Efficiency):1"), 4);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/ogv/keyframe_interval", PROPERTY_HINT_RANGE, "1,1024,1"), 64);

	// Used by the editor.
	GLOBAL_DEF_BASIC("editor/movie_writer/movie_file", "");
	GLOBAL_DEF_BASIC("editor/movie_writer/disable_vsync", false);
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "editor/movie_writer/fps", PROPERTY_HINT_RANGE, "1,300,1,suffix:FPS"), 60);
}

void MovieWriter::set_extensions_hint() {
	RBSet<String> found;
	for (uint32_t i = 0; i < writer_count; i++) {
		List<String> extensions;
		writers[i]->get_supported_extensions(&extensions);
		for (const String &ext : extensions) {
			found.insert(ext);
		}
	}

	String ext_hint;

	for (const String &S : found) {
		if (ext_hint != "") {
			ext_hint += ",";
		}
		ext_hint += "*." + S;
	}
	ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::STRING, "editor/movie_writer/movie_file", PROPERTY_HINT_GLOBAL_SAVE_FILE, ext_hint));
}

bool MovieWriter::_resize_rgba_frame(PendingFrame *p_frame) {
	if (!p_frame->allow_rgb_resize || p_frame->hdr || p_frame->image->get_format() != Image::FORMAT_RGBA8 || p_frame->image->has_mipmaps()) {
		return false;
	}
	const Size2i source_size = p_frame->image->get_size();
	const Size2i target_size = p_frame->size;
	int crop_width = source_size.width;
	int crop_height = source_size.height;
	int crop_x = 0;
	int crop_y = 0;
	const float source_aspect = source_size.aspect();
	const float target_aspect = target_size.aspect();
	if (source_aspect > target_aspect) {
		crop_width = int(source_size.height * target_aspect);
		crop_x = (source_size.width - crop_width) / 2;
	} else if (source_aspect < target_aspect) {
		crop_height = int(source_size.width / target_aspect);
		crop_y = (source_size.height - crop_height) / 2;
	}
	if (crop_width <= 0 || crop_height <= 0 || target_size.width <= 0 || target_size.height <= 0) {
		return false;
	}

	struct HorizontalSample {
		uint32_t left;
		uint32_t right;
		uint32_t fraction;
	};
	LocalVector<HorizontalSample> samples;
	samples.resize(target_size.width);
	for (int x = 0; x < target_size.width; x++) {
		const uint32_t fixed = (x + 0.5) * crop_width * 256 / target_size.width;
		samples[x].left = (crop_x + (fixed >= 128 ? (fixed - 128) >> 8 : 0)) * 4;
		samples[x].right = (crop_x + MIN((fixed + 128) >> 8, uint32_t(crop_width - 1))) * 4;
		const uint32_t fraction = fixed & 255;
		samples[x].fraction = fraction >= 128 ? fraction - 128 : fraction + 128;
	}

	Vector<uint8_t> input = p_frame->image->get_data();
	Vector<uint8_t> output;
	output.resize(target_size.width * target_size.height * 3);
	const uint8_t *src = input.ptr();
	uint8_t *dst = output.ptrw();
	for (int y = 0; y < target_size.height; y++) {
		const uint32_t fixed = (y + 0.5) * crop_height * 256 / target_size.height;
		const uint32_t top = crop_y + (fixed >= 128 ? (fixed - 128) >> 8 : 0);
		const uint32_t bottom = crop_y + MIN((fixed + 128) >> 8, uint32_t(crop_height - 1));
		const uint32_t fraction = fixed & 255;
		const uint32_t fy = fraction >= 128 ? fraction - 128 : fraction + 128;
		const uint8_t *row_top = src + top * source_size.width * 4;
		const uint8_t *row_bottom = src + bottom * source_size.width * 4;
		for (int x = 0; x < target_size.width; x++) {
			const HorizontalSample &sample = samples[x];
			for (uint32_t c = 0; c < 3; c++) {
				const uint32_t p00 = uint32_t(row_top[sample.left + c]) << 8;
				const uint32_t p10 = uint32_t(row_top[sample.right + c]) << 8;
				const uint32_t p01 = uint32_t(row_bottom[sample.left + c]) << 8;
				const uint32_t p11 = uint32_t(row_bottom[sample.right + c]) << 8;
				const uint32_t upper = p00 + (((p10 - p00) * sample.fraction) >> 8);
				const uint32_t lower = p01 + (((p11 - p01) * sample.fraction) >> 8);
				*dst++ = uint8_t((upper + (((lower - upper) * fy) >> 8)) >> 8);
			}
		}
	}
	p_frame->image = Image::create_from_data(target_size.width, target_size.height, false, Image::FORMAT_RGB8, output);
	return true;
}

void MovieWriter::_prepare_frame(void *p_frame) {
	PendingFrame *frame = static_cast<PendingFrame *>(p_frame);
	Ref<Image> vp_tex = frame->image;
	const Size2i movie_size = frame->size;
	const bool resized = vp_tex->get_size() != movie_size && _resize_rgba_frame(frame);
	if (!resized && vp_tex->get_size() != movie_size) {
		// Resize the texture to the output resolution if it differs from the current viewport size.
		// This ensures all frames have the same resolution, as not all video formats and players
		// support resolution changes during playback.

		const float src_aspect = vp_tex->get_size().aspect();
		const float dst_aspect = movie_size.aspect();

		int crop_width = vp_tex->get_size().width;
		int crop_height = vp_tex->get_size().height;
		int crop_x = 0;
		int crop_y = 0;

		// If the aspect ratio differs, crop the image to cover the base resolution's aspect ratio
		// in a way similar to `TextureRect.STRETCH_KEEP_ASPECT_COVERED`.
		if (src_aspect > dst_aspect) {
			// Source is wider, crop horizontally.
			crop_width = int(vp_tex->get_size().height * dst_aspect);
			crop_x = (vp_tex->get_size().width - crop_width) / 2;
			vp_tex->crop_from_point(crop_x, crop_y, crop_width, crop_height);
		} else if (src_aspect < dst_aspect) {
			// Source is taller, crop vertically.
			crop_height = int(vp_tex->get_size().width / dst_aspect);
			crop_y = (vp_tex->get_size().height - crop_height) / 2;
			vp_tex->crop_from_point(crop_x, crop_y, crop_width, crop_height);
		}

		vp_tex->resize(movie_size.width, movie_size.height, Image::INTERPOLATE_BILINEAR);
	}

	if (frame->hdr) {
		vp_tex->convert(Image::FORMAT_RGBA8);
		vp_tex->linear_to_srgb();
	}
	if (frame->writer->can_prepare_frames_async()) {
		frame->encoded = frame->writer->encode_frame_async(frame->image);
		frame->image.unref();
	}
}

void MovieWriter::_write_pending_frame() {
	PendingFrame *frame = pending_frames[0];
	ERR_FAIL_COND(!frame->ready);
	if (frame->task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(frame->task);
	}
	if ((can_prepare_frames_async() && !frame->encoded.is_empty()) || (!can_prepare_frames_async() && frame->image.is_valid() && !frame->image->is_empty())) {
		const uint64_t encoding_start_usec = Time::get_singleton()->get_ticks_usec();
		Error err;
		if (can_prepare_frames_async()) {
			err = write_encoded_frame(frame->encoded, frame->audio.ptr());
		} else {
			err = write_frame(frame->image, frame->audio.ptr());
		}
		encoding_time_usec += Time::get_singleton()->get_ticks_usec() - encoding_start_usec;
		if (err != OK) {
			ERR_PRINT(vformat("Failed to write a movie frame (error %d).", err));
			OS::get_singleton()->set_exit_code(EXIT_FAILURE);
		}
	} else {
		ERR_PRINT("Failed to capture a movie frame.");
		OS::get_singleton()->set_exit_code(EXIT_FAILURE);
	}
	pending_frames.remove_at(0);
	memdelete(frame);
}

void MovieWriter::_start_preparing_frame(PendingFrame *p_frame) {
	if (can_prepare_frames_async()) {
		p_frame->task = WorkerThreadPool::get_singleton()->add_native_task(_prepare_frame, p_frame, true, "Movie frame preparation and encoding");
	} else if (p_frame->image->get_size() != p_frame->size || p_frame->hdr) {
		_prepare_frame(p_frame);
	}
	p_frame->ready = true;
}

void MovieWriter::_readback_complete(const Vector<uint8_t> &p_data, int64_t p_id) {
	for (PendingFrame *frame : pending_frames) {
		if (frame->id == p_id) {
			pending_readbacks--;
			frame->image = Image::create_from_data(frame->source_size.width, frame->source_size.height, false, Image::FORMAT_RGBA8, p_data);
			if (frame->image.is_null() || frame->image->is_empty()) {
				frame->ready = true;
				return;
			}
			_start_preparing_frame(frame);
			return;
		}
	}
	ERR_FAIL_MSG("Movie frame readback completed after its frame was discarded.");
}

void MovieWriter::flush_pending_readbacks() {
	if (pending_readbacks > 0) {
		RenderingDevice::get_singleton()->flush_pending_texture_readbacks();
	}
}

void MovieWriter::add_frame() {
	const int movie_time_seconds = Engine::get_singleton()->get_frames_drawn() / fps;
	const int frame_remainder = Engine::get_singleton()->get_frames_drawn() % fps;
	const String movie_time = vformat("%s:%s:%s:%s",
			String::num(movie_time_seconds / 3600, 0).pad_zeros(2),
			String::num((movie_time_seconds % 3600) / 60, 0).pad_zeros(2),
			String::num(movie_time_seconds % 60, 0).pad_zeros(2),
			String::num(frame_remainder, 0).pad_zeros(2));

	Window *main_window = Window::get_from_id(DisplayServerEnums::MAIN_WINDOW_ID);
	if (main_window) {
		main_window->set_title(vformat("MovieWriter: Frame %d (time: %s) - %s", Engine::get_singleton()->get_frames_drawn(), movie_time, project_name));
	}

	RID main_vp_rid = RenderingServer::get_singleton()->viewport_find_from_screen_attachment(DisplayServerEnums::MAIN_WINDOW_ID);
	RID main_vp_texture = RenderingServer::get_singleton()->viewport_get_texture(main_vp_rid);
	while (!pending_frames.is_empty() && pending_frames[0]->ready && (pending_frames[0]->task == WorkerThreadPool::INVALID_TASK_ID || WorkerThreadPool::get_singleton()->is_task_completed(pending_frames[0]->task))) {
		_write_pending_frame();
	}
	if (pending_frames.size() >= 16) {
		flush_pending_readbacks();
		while (!pending_frames.is_empty() && pending_frames[0]->ready) {
			_write_pending_frame();
		}
	}
	PendingFrame *frame = memnew(PendingFrame);
	frame->writer = this;
	frame->id = next_frame_id++;
	frame->size = movie_size;
	frame->hdr = RenderingServer::get_singleton()->viewport_is_using_hdr_2d(main_vp_rid);
	frame->allow_rgb_resize = can_prepare_frames_async();
	pending_frames.push_back(frame);
	bool async_readback = false;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	Image::Format source_format = RenderingServer::get_singleton()->texture_get_format(main_vp_texture);
	if (can_prepare_frames_async() && !frame->hdr && rd && RenderingServer::get_singleton()->is_on_render_thread() && (source_format == Image::FORMAT_RGB8 || source_format == Image::FORMAT_RGBA8)) {
		RID rd_texture = RenderingServer::get_singleton()->texture_get_rd_texture(main_vp_texture);
		if (rd_texture.is_valid()) {
			RenderingDevice::TextureFormat format = rd->texture_get_format(rd_texture);
			const uint64_t staging_mb = MAX(1, int(GLOBAL_GET("rendering/rendering_device/staging_buffer/max_size_mb")));
			const uint64_t staging_block_kb = MAX(4, int(GLOBAL_GET("rendering/rendering_device/staging_buffer/block_size_kb")));
			const uint64_t staging_size = MAX(staging_mb * 1024 * 1024, staging_block_kb * 1024 * 4);
			const uint64_t image_bytes = uint64_t(format.width) * format.height * 4;
			// Leave room for padding and other in-flight frames in the shared staging buffers.
			if ((format.format == RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM || format.format == RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB) && format.mipmaps == 1 && (format.usage_bits & RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT) && image_bytes <= staging_size / 4) {
				frame->source_size = Size2i(format.width, format.height);
				pending_readbacks++;
				if (rd->texture_get_data_async(rd_texture, 0, callable_mp(this, &MovieWriter::_readback_complete).bind(frame->id)) == OK) {
					async_readback = true;
				} else {
					pending_readbacks--;
				}
			}
		}
	}
	if (!async_readback) {
		frame->image = RenderingServer::get_singleton()->texture_2d_get(main_vp_texture);
		if (frame->image.is_valid() && !frame->image->is_empty()) {
			_start_preparing_frame(frame);
		} else {
			frame->ready = true;
		}
	}

	RenderingServer::get_singleton()->viewport_set_measure_render_time(main_vp_rid, true);
	cpu_time += RenderingServer::get_singleton()->viewport_get_measured_render_time_cpu(main_vp_rid);
	cpu_time += RenderingServer::get_singleton()->get_frame_setup_time_cpu();
	gpu_time += RenderingServer::get_singleton()->viewport_get_measured_render_time_gpu(main_vp_rid);

	AudioDriverDummy::get_dummy_singleton()->mix_audio(mix_rate / fps, audio_mix_buffer.ptr());

	frame->audio.resize(audio_mix_buffer.size());
	memcpy(frame->audio.ptrw(), audio_mix_buffer.ptr(), audio_mix_buffer.size() * sizeof(int32_t));
	if (!can_prepare_frames_async()) {
		_write_pending_frame();
	}
}

void MovieWriter::end() {
	flush_pending_readbacks();
	while (!pending_frames.is_empty()) {
		if (!pending_frames[0]->ready) {
			ERR_PRINT("Movie frame readback did not complete.");
			OS::get_singleton()->set_exit_code(EXIT_FAILURE);
			break;
		}
		_write_pending_frame();
	}
	uint64_t encoding_start_usec = Time::get_singleton()->get_ticks_usec();
	write_end();
	uint64_t encoding_end_usec = Time::get_singleton()->get_ticks_usec();
	encoding_time_usec += encoding_end_usec - encoding_start_usec;
	const uint64_t recording_time_usec = encoding_end_usec - recording_start_usec;

	// Print a report with various statistics.
	print_line("--------------------------------------------------------------------------------");
	String movie_path = Engine::get_singleton()->get_write_movie_path();
	if (movie_path.is_relative_path()) {
		// Print absolute path to make finding the file easier,
		// and to make it clickable in terminal emulators that support this.
		movie_path = ProjectSettings::get_singleton()->globalize_path("res://").path_join(movie_path);
	}
	print_line(vformat("Done recording movie at path: %s", movie_path));

	const int movie_time_seconds = Engine::get_singleton()->get_frames_drawn() / fps;
	const int frame_remainder = Engine::get_singleton()->get_frames_drawn() % fps;
	const String movie_time = vformat("%s:%s:%s:%s",
			String::num(movie_time_seconds / 3600, 0).pad_zeros(2),
			String::num((movie_time_seconds % 3600) / 60, 0).pad_zeros(2),
			String::num(movie_time_seconds % 60, 0).pad_zeros(2),
			String::num(frame_remainder, 0).pad_zeros(2));

	const int real_time_seconds = Time::get_singleton()->get_ticks_msec() / 1000;
	const String real_time = vformat("%s:%s:%s",
			String::num(real_time_seconds / 3600, 0).pad_zeros(2),
			String::num((real_time_seconds % 3600) / 60, 0).pad_zeros(2),
			String::num(real_time_seconds % 60, 0).pad_zeros(2));

	const double recording_seconds = recording_time_usec / 1000000.0;
	const double speed_percent = recording_seconds > 0.0 ? (double(Engine::get_singleton()->get_frames_drawn()) / fps) / recording_seconds * 100.0 : 0.0;
	const uint64_t measured_frames = MAX(uint64_t(1), Engine::get_singleton()->get_frames_drawn());
	print_line(vformat("%d frames at %d FPS (movie length: %s), recorded in %.2f seconds (%.1f%% of real-time speed; process elapsed: %s).", Engine::get_singleton()->get_frames_drawn(), fps, movie_time, recording_seconds, speed_percent, real_time));
	print_line(vformat("CPU render time: %.2f seconds (average: %.2f ms/frame)", cpu_time / 1000, cpu_time / measured_frames));
	print_line(vformat("GPU render time: %.2f seconds (average: %.2f ms/frame)", gpu_time / 1000, gpu_time / measured_frames));
	print_line(vformat("Writer calls and finalization: %.2f seconds (average: %.2f ms/frame; excludes asynchronous encoding work)", encoding_time_usec / 1000000.f, encoding_time_usec / 1000.f / measured_frames));
	print_line("--------------------------------------------------------------------------------");
}
