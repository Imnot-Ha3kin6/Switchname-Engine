// Switch video decoder for hxvlc.flixel.FlxVideoSprite (Codename Engine Switch port).
// FFmpeg (devkitPro portlibs, software decoding) on a background thread.
// Video frames are converted to RGBA into a small queue; the whole audio track is
// decoded up front into interleaved signed 16-bit PCM so the game can play it as a sound.
//
// This file is #included into the generated FlxVideoSprite C++ (see @:cppInclude).

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

namespace cnevideo {

struct Frame {
	double pts;
	std::vector<uint8_t> rgba;
};

struct Video {
	AVFormatContext *fmt = nullptr;
	AVCodecContext *vctx = nullptr;
	int vstream = -1;
	double vtimebase = 0;
	SwsContext *sws = nullptr;
	int width = 0, height = 0;
	double duration = 0;

	std::vector<int16_t> pcm;
	int audioChannels = 0, audioRate = 0;

	std::thread worker;
	std::mutex lock;
	std::deque<Frame> queue;
	std::vector<Frame> pool;
	std::atomic<bool> running{false};
	std::atomic<bool> finished{false};
	std::atomic<double> seekTarget{-1};
	double lastDelivered = -1;
};

static const size_t MAX_QUEUE = 4;

static double ptsSeconds(Video *v, AVFrame *f) {
	int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
	if (pts == AV_NOPTS_VALUE) return 0;
	double base = v->fmt->streams[v->vstream]->start_time != AV_NOPTS_VALUE ? v->fmt->streams[v->vstream]->start_time * v->vtimebase : 0;
	return pts * v->vtimebase - base;
}

static bool decodeAudio(Video *v, const char *path) {
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) return false;
	if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return false; }
	const AVCodec *codec = nullptr;
	int astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
	if (astream < 0 || !codec) { avformat_close_input(&fmt); return false; }

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(ctx, fmt->streams[astream]->codecpar);
	if (avcodec_open2(ctx, codec, nullptr) < 0) { avcodec_free_context(&ctx); avformat_close_input(&fmt); return false; }

	int channels = ctx->ch_layout.nb_channels >= 2 ? 2 : 1;
	AVChannelLayout outLayout;
	av_channel_layout_default(&outLayout, channels);
	SwrContext *swr = nullptr;
	swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, ctx->sample_rate, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, 0, nullptr);
	if (!swr || swr_init(swr) < 0) { swr_free(&swr); avcodec_free_context(&ctx); avformat_close_input(&fmt); return false; }

	v->audioChannels = channels;
	v->audioRate = ctx->sample_rate;

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	auto drain = [&]() {
		while (avcodec_receive_frame(ctx, frame) == 0) {
			int outSamples = swr_get_out_samples(swr, frame->nb_samples);
			size_t start = v->pcm.size();
			v->pcm.resize(start + (size_t)outSamples * channels);
			uint8_t *out = (uint8_t *)(v->pcm.data() + start);
			int got = swr_convert(swr, &out, outSamples, (const uint8_t **)frame->extended_data, frame->nb_samples);
			v->pcm.resize(start + (size_t)(got > 0 ? got : 0) * channels);
		}
	};
	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == astream && avcodec_send_packet(ctx, pkt) == 0) drain();
		av_packet_unref(pkt);
	}
	avcodec_send_packet(ctx, nullptr);
	drain();

	av_frame_free(&frame);
	av_packet_free(&pkt);
	swr_free(&swr);
	avcodec_free_context(&ctx);
	avformat_close_input(&fmt);
	return !v->pcm.empty();
}

static void doSeek(Video *v, double target) {
	int64_t ts = (int64_t)(target / v->vtimebase);
	AVStream *s = v->fmt->streams[v->vstream];
	if (s->start_time != AV_NOPTS_VALUE) ts += s->start_time;
	av_seek_frame(v->fmt, v->vstream, ts, AVSEEK_FLAG_BACKWARD);
	avcodec_flush_buffers(v->vctx);
	std::lock_guard<std::mutex> g(v->lock);
	for (auto &f : v->queue) v->pool.push_back(std::move(f));
	v->queue.clear();
	v->lastDelivered = -1;
	v->finished = false;
}

static void run(Video *v) {
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	double skipUntil = -1;

	while (v->running) {
		double seek = v->seekTarget.exchange(-1);
		if (seek >= 0) { doSeek(v, seek); skipUntil = seek; }

		{
			std::unique_lock<std::mutex> g(v->lock);
			if (v->queue.size() >= MAX_QUEUE || v->finished) {
				g.unlock();
				std::this_thread::sleep_for(std::chrono::milliseconds(4));
				continue;
			}
		}

		int r = avcodec_receive_frame(v->vctx, frame);
		if (r == AVERROR(EAGAIN)) {
			int rr = av_read_frame(v->fmt, pkt);
			if (rr < 0) { avcodec_send_packet(v->vctx, nullptr); }
			else {
				if (pkt->stream_index == v->vstream) avcodec_send_packet(v->vctx, pkt);
				av_packet_unref(pkt);
			}
			continue;
		}
		if (r == AVERROR_EOF) { v->finished = true; continue; }
		if (r < 0) { v->finished = true; continue; }

		double pts = ptsSeconds(v, frame);
		if (skipUntil >= 0 && pts + 0.05 < skipUntil) { av_frame_unref(frame); continue; }
		skipUntil = -1;

		Frame out;
		{
			std::lock_guard<std::mutex> g(v->lock);
			if (!v->pool.empty()) { out = std::move(v->pool.back()); v->pool.pop_back(); }
		}
		out.pts = pts;
		out.rgba.resize((size_t)v->width * v->height * 4);
		v->sws = sws_getCachedContext(v->sws, frame->width, frame->height, (AVPixelFormat)frame->format,
			v->width, v->height, AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
		uint8_t *dst[1] = { out.rgba.data() };
		int dstStride[1] = { v->width * 4 };
		sws_scale(v->sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
		av_frame_unref(frame);

		std::lock_guard<std::mutex> g(v->lock);
		v->queue.push_back(std::move(out));
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
}

} // namespace cnevideo

extern "C" {

static char cnevLastError[256] = "";
const char *cnev_last_error() { return cnevLastError; }

void *cnev_open(const char *rawPath) {
	using namespace cnevideo;
	// "sdmc:/..." looks like a URL with an unknown "sdmc" protocol to FFmpeg; force the file protocol
	std::string fullPath = std::string("file:") + rawPath;
	const char *path = fullPath.c_str();
	Video *v = new Video();
	int err = avformat_open_input(&v->fmt, path, nullptr, nullptr);
	if (err < 0) { av_strerror(err, cnevLastError, sizeof(cnevLastError)); delete v; return nullptr; }
	if (avformat_find_stream_info(v->fmt, nullptr) < 0) { avformat_close_input(&v->fmt); delete v; return nullptr; }

	const AVCodec *codec = nullptr;
	v->vstream = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if (v->vstream < 0 || !codec) { avformat_close_input(&v->fmt); delete v; return nullptr; }

	v->vctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(v->vctx, v->fmt->streams[v->vstream]->codecpar);
	v->vctx->thread_count = 3; // leave one core for the game
	v->vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	if (avcodec_open2(v->vctx, codec, nullptr) < 0) { avcodec_free_context(&v->vctx); avformat_close_input(&v->fmt); delete v; return nullptr; }

	AVStream *s = v->fmt->streams[v->vstream];
	v->vtimebase = av_q2d(s->time_base);
	v->width = v->vctx->width;
	v->height = v->vctx->height;
	v->duration = v->fmt->duration != AV_NOPTS_VALUE ? v->fmt->duration / (double)AV_TIME_BASE : (s->duration != AV_NOPTS_VALUE ? s->duration * v->vtimebase : 0);

	decodeAudio(v, path);

	v->running = true;
	v->worker = std::thread(run, v);
	return v;
}

// Size the video was authored at, from a "cne_source_size=WxH" tag in the file's comment metadata
// (written when a video is re-encoded smaller for Switch). Returns 0 when there is no tag.
int cnev_source_size(void *h, int wantHeight) {
	if (!h) return 0;
	AVFormatContext *fmt = ((cnevideo::Video *)h)->fmt;
	if (!fmt || !fmt->metadata) return 0;
	AVDictionaryEntry *e = av_dict_get(fmt->metadata, "comment", nullptr, 0);
	if (!e || !e->value) return 0;
	const char *tag = strstr(e->value, "cne_source_size=");
	if (!tag) return 0;
	int w = 0, hgt = 0;
	if (sscanf(tag + 16, "%dx%d", &w, &hgt) != 2 || w <= 0 || hgt <= 0) return 0;
	return wantHeight ? hgt : w;
}
int cnev_width(void *h) { return h ? ((cnevideo::Video *)h)->width : 0; }
int cnev_height(void *h) { return h ? ((cnevideo::Video *)h)->height : 0; }
double cnev_duration(void *h) { return h ? ((cnevideo::Video *)h)->duration : 0; }
int cnev_audio_channels(void *h) { return h ? ((cnevideo::Video *)h)->audioChannels : 0; }
int cnev_audio_rate(void *h) { return h ? ((cnevideo::Video *)h)->audioRate : 0; }
int cnev_audio_bytes(void *h) { return h ? (int)(((cnevideo::Video *)h)->pcm.size() * sizeof(int16_t)) : 0; }

void cnev_audio_copy(void *h, uint8_t *dst) {
	if (!h) return;
	auto *v = (cnevideo::Video *)h;
	memcpy(dst, v->pcm.data(), v->pcm.size() * sizeof(int16_t));
	std::vector<int16_t>().swap(v->pcm); // the game owns the audio now
}

// Copies the newest frame with pts <= time into dst (width*height*4). Returns 1 if dst changed.
int cnev_frame(void *h, double time, uint8_t *dst) {
	if (!h) return 0;
	auto *v = (cnevideo::Video *)h;
	std::lock_guard<std::mutex> g(v->lock);
	int idx = -1;
	for (size_t i = 0; i < v->queue.size(); i++) {
		if (v->queue[i].pts <= time) idx = (int)i;
		else break;
	}
	// show the first frame right away instead of a blank sprite
	if (idx < 0 && v->lastDelivered < 0 && !v->queue.empty()) idx = 0;
	if (idx < 0) return 0;

	memcpy(dst, v->queue[idx].rgba.data(), v->queue[idx].rgba.size());
	v->lastDelivered = v->queue[idx].pts;
	for (int i = 0; i <= idx; i++) {
		v->pool.push_back(std::move(v->queue.front()));
		v->queue.pop_front();
	}
	return 1;
}

double cnev_last_pts(void *h) { return h ? ((cnevideo::Video *)h)->lastDelivered : -1; }
double cnev_timebase(void *h) { return h ? ((cnevideo::Video *)h)->vtimebase : 0; }
int cnev_queue(void *h) {
	if (!h) return 0;
	auto *v = (cnevideo::Video *)h;
	std::lock_guard<std::mutex> g(v->lock);
	return (int)v->queue.size();
}

int cnev_finished(void *h) {
	if (!h) return 1;
	auto *v = (cnevideo::Video *)h;
	std::lock_guard<std::mutex> g(v->lock);
	return v->finished && v->queue.empty() ? 1 : 0;
}

void cnev_seek(void *h, double seconds) {
	if (h) ((cnevideo::Video *)h)->seekTarget = seconds < 0 ? 0 : seconds;
}

void cnev_close(void *h) {
	if (!h) return;
	auto *v = (cnevideo::Video *)h;
	v->running = false;
	if (v->worker.joinable()) v->worker.join();
	if (v->sws) sws_freeContext(v->sws);
	avcodec_free_context(&v->vctx);
	avformat_close_input(&v->fmt);
	delete v;
}

}
