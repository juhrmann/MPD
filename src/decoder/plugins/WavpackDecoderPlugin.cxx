/*
 * Copyright 2003-2016 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "WavpackDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "CheckAudioFormat.hxx"
#include "tag/TagHandler.hxx"
#include "fs/Path.hxx"
#include "util/Macros.hxx"
#include "util/Alloc.hxx"
#include "util/ScopeExit.hxx"
#include "util/RuntimeError.hxx"

#include <wavpack/wavpack.h>

#include <stdexcept>
#include <memory>

#include <assert.h>

#define ERRORLEN 80

#ifdef OPEN_DSD_AS_PCM
/* libWavPack supports DSD since version 5 */
  #ifdef ENABLE_DSD
static constexpr int OPEN_DSD_FLAG = OPEN_DSD_NATIVE;
  #else
static constexpr int OPEN_DSD_FLAG = OPEN_DSD_AS_PCM;
  #endif
#else
/* no DSD support in this libWavPack version */
static constexpr int OPEN_DSD_FLAG = 0;
#endif

static WavpackContext *
WavpackOpenInput(Path path, int flags, int norm_offset)
{
	char error[ERRORLEN];
	auto *wpc = WavpackOpenFileInput(path.c_str(), error,
					 flags, norm_offset);
	if (wpc == nullptr)
		throw FormatRuntimeError("failed to open WavPack file \"%s\": %s",
					 path.c_str(), error);

	return wpc;
}

static WavpackContext *
WavpackOpenInput(WavpackStreamReader *reader, void *wv_id, void *wvc_id,
		 int flags, int norm_offset)
{
	char error[ERRORLEN];
	auto *wpc = WavpackOpenFileInputEx(reader, wv_id, wvc_id, error,
					   flags, norm_offset);
	if (wpc == nullptr)
		throw FormatRuntimeError("failed to open WavPack stream: %s",
					 error);

	return wpc;
}

gcc_pure
static SignedSongTime
GetDuration(WavpackContext *wpc)
{
#ifdef OPEN_DSD_AS_PCM
	/* libWavPack 5 */
	const auto n_samples = WavpackGetNumSamples64(wpc);
	if (n_samples == -1)
		/* unknown */
		return SignedSongTime::Negative();
#else
	const uint32_t n_samples = WavpackGetNumSamples(wpc);
	if (n_samples == uint32_t(-1))
		/* unknown */
		return SignedSongTime::Negative();
#endif

	return SongTime::FromScale<uint64_t>(n_samples,
					     WavpackGetSampleRate(wpc));
}

/*
 * Convert integer samples.
 */
template<typename T>
static void
format_samples_int(void *buffer, uint32_t count)
{
	int32_t *src = (int32_t *)buffer;
	T *dst = (T *)buffer;
	/*
	 * The asserts like the following one are because we do the
	 * formatting of samples within a single buffer. The size of
	 * the output samples never can be greater than the size of
	 * the input ones. Otherwise we would have an overflow.
	 */
	static_assert(sizeof(*dst) <= sizeof(*src), "Wrong size");

	/* pass through and align samples */
	std::copy_n(src, count, dst);
}

/*
 * No conversion necessary.
 */
static void
format_samples_nop(gcc_unused void *buffer, gcc_unused uint32_t count)
{
	/* do nothing */
}

/**
 * Choose a MPD sample format from libwavpacks' number of bits.
 */
static SampleFormat
wavpack_bits_to_sample_format(bool is_float,
#if defined(OPEN_DSD_AS_PCM) && defined(ENABLE_DSD)
			      bool is_dsd,
#endif
			      int bytes_per_sample)
{
	if (is_float)
		return SampleFormat::FLOAT;

#if defined(OPEN_DSD_AS_PCM) && defined(ENABLE_DSD)
	if (is_dsd)
		return SampleFormat::DSD;
#endif

	switch (bytes_per_sample) {
	case 1:
		return SampleFormat::S8;

	case 2:
		return SampleFormat::S16;

	case 3:
		return SampleFormat::S24_P32;

	case 4:
		return SampleFormat::S32;

	default:
		return SampleFormat::UNDEFINED;
	}
}

/*
 * This does the main decoding thing.
 * Requires an already opened WavpackContext.
 */
static void
wavpack_decode(DecoderClient &client, WavpackContext *wpc, bool can_seek)
{
	const bool is_float = (WavpackGetMode(wpc) & MODE_FLOAT) != 0;
#if defined(OPEN_DSD_AS_PCM) && defined(ENABLE_DSD)
	const bool is_dsd = (WavpackGetQualifyMode(wpc) & QMODE_DSD_AUDIO) != 0;
#else
	constexpr bool is_dsd = false;
#endif
	SampleFormat sample_format =
		wavpack_bits_to_sample_format(is_float,
#if defined(OPEN_DSD_AS_PCM) && defined(ENABLE_DSD)
					      is_dsd,
#endif
					      WavpackGetBytesPerSample(wpc));

	auto audio_format = CheckAudioFormat(WavpackGetSampleRate(wpc),
					     sample_format,
					     WavpackGetReducedChannels(wpc));

	auto *format_samples = format_samples_nop;
	if (is_dsd)
		format_samples = format_samples_int<uint8_t>;
	else if (!is_float) {
		switch (WavpackGetBytesPerSample(wpc)) {
		case 1:
			format_samples = format_samples_int<int8_t>;
			break;

		case 2:
			format_samples = format_samples_int<int16_t>;
			break;
		}
	}

	client.Ready(audio_format, can_seek, GetDuration(wpc));

	const int output_sample_size = audio_format.GetFrameSize();

	/* wavpack gives us all kind of samples in a 32-bit space */
	int32_t chunk[1024];
	const uint32_t samples_requested = ARRAY_SIZE(chunk) /
		audio_format.channels;

	DecoderCommand cmd = client.GetCommand();
	while (cmd != DecoderCommand::STOP) {
		if (cmd == DecoderCommand::SEEK) {
			if (can_seek) {
				auto where = client.GetSeekFrame();

				if (WavpackSeekSample(wpc, where)) {
					client.CommandFinished();
				} else {
					client.SeekError();
				}
			} else {
				client.SeekError();
			}
		}

		uint32_t samples_got = WavpackUnpackSamples(wpc, chunk,
							    samples_requested);
		if (samples_got == 0)
			break;

		int bitrate = (int)(WavpackGetInstantBitrate(wpc) / 1000 +
				    0.5);
		format_samples(chunk, samples_got * audio_format.channels);

		cmd = client.SubmitData(nullptr, chunk,
					samples_got * output_sample_size,
					bitrate);
	}
}

/*
 * #InputStream <=> WavpackStreamReader wrapper callbacks
 */

/* This struct is needed for per-stream last_byte storage. */
struct WavpackInput {
	DecoderClient *const client;
	InputStream &is;
	/* Needed for push_back_byte() */
	int last_byte;

	constexpr WavpackInput(DecoderClient *_client, InputStream &_is)
		:client(_client), is(_is), last_byte(EOF) {}

	int32_t ReadBytes(void *data, size_t bcount);
};

/**
 * Little wrapper for struct WavpackInput to cast from void *.
 */
static WavpackInput *
wpin(void *id)
{
	assert(id);
	return (WavpackInput *)id;
}

static int32_t
wavpack_input_read_bytes(void *id, void *data, int32_t bcount)
{
	return wpin(id)->ReadBytes(data, bcount);
}

int32_t
WavpackInput::ReadBytes(void *data, size_t bcount)
{
	uint8_t *buf = (uint8_t *)data;
	int32_t i = 0;

	if (last_byte != EOF) {
		*buf++ = last_byte;
		last_byte = EOF;
		--bcount;
		++i;
	}

	/* wavpack fails if we return a partial read, so we just wait
	   until the buffer is full */
	while (bcount > 0) {
		size_t nbytes = decoder_read(client, is, buf, bcount);
		if (nbytes == 0) {
			/* EOF, error or a decoder command */
			break;
		}

		i += nbytes;
		bcount -= nbytes;
		buf += nbytes;
	}

	return i;
}

static uint32_t
wavpack_input_get_pos(void *id)
{
	WavpackInput &wpi = *wpin(id);

	return wpi.is.GetOffset();
}

static int
wavpack_input_set_pos_abs(void *id, uint32_t pos)
{
	WavpackInput &wpi = *wpin(id);

	try {
		wpi.is.LockSeek(pos);
		return 0;
	} catch (const std::runtime_error &) {
		return -1;
	}
}

static int
wavpack_input_set_pos_rel(void *id, int32_t delta, int mode)
{
	WavpackInput &wpi = *wpin(id);
	InputStream &is = wpi.is;

	offset_type offset = delta;
	switch (mode) {
	case SEEK_SET:
		break;

	case SEEK_CUR:
		offset += is.GetOffset();
		break;

	case SEEK_END:
		if (!is.KnownSize())
			return -1;

		offset += is.GetSize();
		break;

	default:
		return -1;
	}

	try {
		wpi.is.LockSeek(offset);
		return 0;
	} catch (const std::runtime_error &) {
		return -1;
	}
}

static int
wavpack_input_push_back_byte(void *id, int c)
{
	WavpackInput &wpi = *wpin(id);

	if (wpi.last_byte == EOF) {
		wpi.last_byte = c;
		return c;
	} else {
		return EOF;
	}
}

static uint32_t
wavpack_input_get_length(void *id)
{
	WavpackInput &wpi = *wpin(id);
	InputStream &is = wpi.is;

	if (!is.KnownSize())
		return 0;

	return is.GetSize();
}

static int
wavpack_input_can_seek(void *id)
{
	WavpackInput &wpi = *wpin(id);
	InputStream &is = wpi.is;

	return is.IsSeekable();
}

static WavpackStreamReader mpd_is_reader = {
	wavpack_input_read_bytes,
	wavpack_input_get_pos,
	wavpack_input_set_pos_abs,
	wavpack_input_set_pos_rel,
	wavpack_input_push_back_byte,
	wavpack_input_get_length,
	wavpack_input_can_seek,
	nullptr /* no need to write edited tags */
};

static InputStreamPtr
wavpack_open_wvc(DecoderClient &client, const char *uri)
{
	/*
	 * As we use dc->utf8url, this function will be bad for
	 * single files. utf8url is not absolute file path :/
	 */
	if (uri == nullptr)
		return nullptr;

	char *wvc_url = xstrcatdup(uri, "c");
	AtScopeExit(wvc_url) {
		free(wvc_url);
	};

	try {
		return client.OpenUri(uri);
	} catch (const std::runtime_error &) {
		return nullptr;
	}
}

/*
 * Decodes a stream.
 */
static void
wavpack_streamdecode(DecoderClient &client, InputStream &is)
{
	int open_flags = OPEN_DSD_FLAG | OPEN_NORMALIZE;
	bool can_seek = is.IsSeekable();

	std::unique_ptr<WavpackInput> wvc;
	auto is_wvc = wavpack_open_wvc(client, is.GetURI());
	if (is_wvc) {
		open_flags |= OPEN_WVC;
		can_seek &= wvc->is.IsSeekable();

		wvc.reset(new WavpackInput(&client, *is_wvc));
	}

	if (!can_seek) {
		open_flags |= OPEN_STREAMING;
	}

	WavpackInput isp(&client, is);

	auto *wpc = WavpackOpenInput(&mpd_is_reader, &isp, wvc.get(),
				     open_flags, 0);
	AtScopeExit(wpc) {
		WavpackCloseFile(wpc);
	};

	wavpack_decode(client, wpc, can_seek);
}

/*
 * Decodes a file.
 */
static void
wavpack_filedecode(DecoderClient &client, Path path_fs)
{
	auto *wpc = WavpackOpenInput(path_fs,
				     OPEN_DSD_FLAG | OPEN_NORMALIZE | OPEN_WVC,
				     0);
	AtScopeExit(wpc) {
		WavpackCloseFile(wpc);
	};

	wavpack_decode(client, wpc, true);
}

/*
 * Reads metainfo from the specified file.
 */
static bool
wavpack_scan_file(Path path_fs,
		  const TagHandler &handler, void *handler_ctx)
{
	auto *wpc = WavpackOpenInput(path_fs, OPEN_DSD_FLAG, 0);
	AtScopeExit(wpc) {
		WavpackCloseFile(wpc);
	};

	const auto duration = GetDuration(wpc);
	if (!duration.IsNegative())
		tag_handler_invoke_duration(handler, handler_ctx, SongTime(duration));

	return true;
}

static char const *const wavpack_suffixes[] = {
	"wv",
	nullptr
};

static char const *const wavpack_mime_types[] = {
	"audio/x-wavpack",
	nullptr
};

const struct DecoderPlugin wavpack_decoder_plugin = {
	"wavpack",
	nullptr,
	nullptr,
	wavpack_streamdecode,
	wavpack_filedecode,
	wavpack_scan_file,
	nullptr,
	nullptr,
	wavpack_suffixes,
	wavpack_mime_types
};
