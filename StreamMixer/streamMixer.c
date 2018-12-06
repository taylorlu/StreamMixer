#ifdef WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <stdio.h>
#include <string.h>
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavdevice/avdevice.h"
#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/avassert.h"
#include "libswscale/swscale.h"
#include "libswscale/swscale_internal.h"
#include "libavutil/pixfmt.h"
#include "libavformat/avformat.h"
#include "libavutil/samplefmt.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
#include "libavutil/audio_fifo.h"
#include "cJSON.h"

#define MAX_QUEUE_SIZE		20
#define MAX_STREAM_COUNT	10

struct AVFrameNode {
	AVFrame *frame;
	struct AVFrameNode *next;
};

struct AVFrameList {
	struct AVFrameNode *head;
	struct AVFrameNode *tail;
	int size;
#ifdef WIN32
	HANDLE hMutex;
#else
	pthread_mutex_t pMutex;
#endif
};

struct StreamInput {
	char *stream;
	int layer;
	int dest_x;
	int dest_y;
	int dest_width;
	int dest_height;

	int fps;
	int bitrate;

	AVFormatContext *pFormatCtx;
	AVCodecContext *pVideoCodecCtx;
	AVCodecContext *pAudioCodecCtx;

	SwsContext *img_convert_ctx;
	SwrContext *swr_ctx;
	AVAudioFifo *audioFifo;

	struct AVFrameList *video_frameList;
	struct AVFrameList *audio_frameList;

	AVFilterContext *buffersrc_ctx;
	int audio_stream_index;
	int video_stream_index;

	int hasAudio;
	int hasVideo;

	struct StreamOutput *streamOutput;
};

struct StreamOutput {
	char *out_stream;
	int final_width;
	int final_height;
	int bitrate;
	int fps;
	int64_t cVideo_timestamp_gap;
	int64_t cAudio_timestamp_gap;
	int64_t cVideoTimestamp;
	int64_t cAudioTimestamp;

	AVFormatContext *ofmt_ctx;
	AVStream *audioOutStream;
	AVStream *videoOutStream;
	AVCodecContext *videoEnc_ctx;
	AVCodecContext *audioEnc_ctx;

	int64_t audio_sequence_number;
	int64_t video_sequence_number;
};

static void push_back(struct AVFrameList *list, AVFrame *frame) {

	struct AVFrameNode *node = (struct AVFrameNode *)malloc(sizeof(struct AVFrameNode));
	memset(node, 0, sizeof(struct AVFrameNode));
	node->frame = frame;

#ifdef WIN32
	WaitForSingleObject(list->hMutex, INFINITE);
#else
	pthread_mutex_lock(&list->pMutex);
#endif

	if (!list->tail){
		list->head = node;
	}
	else {
		list->tail->next = node;
	}
	list->tail = node;
	list->size++;

#ifdef WIN32
	ReleaseMutex(list->hMutex);
#else
	pthread_mutex_unlock(&list->pMutex);
#endif
}

static AVFrame *pop_front(struct AVFrameList *list) {

#ifdef WIN32
	WaitForSingleObject(list->hMutex, INFINITE);
#else
	pthread_mutex_lock(&list->pMutex);
#endif

	if (list->size < 1) {
		return NULL;
	}
	struct AVFrameNode *tmp = list->head;
	list->head = list->head->next;
	if (!list->head) {
		list->tail = NULL;
	}
	list->size--;
#ifdef WIN32
	ReleaseMutex(list->hMutex);
#else
	pthread_mutex_unlock(&list->pMutex);
#endif
	AVFrame *frame = tmp->frame;
	free(tmp);
	return frame;
}

AVRational videoTimeBase;
AVRational audioTimeBase;
AVFrame *blend_frames[MAX_STREAM_COUNT];
AVFrame *mix_frames[MAX_STREAM_COUNT];
uint8_t *blendBuffer = NULL;

cJSON* GetJsonObject(char* fileName) {
	long len;
	char* pContent;
	int tmp;
	FILE* fp = fopen(fileName, "rb+");
	if (!fp) {
		return NULL;
	}
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	if (0 == len) {
		return NULL;
	}

	fseek(fp, 0, SEEK_SET);
	pContent = (char*)malloc(sizeof(char)*len);
	tmp = fread(pContent, 1, len, fp);

	fclose(fp);
	cJSON *json = cJSON_Parse(pContent);
	if (!json) {
		return NULL;
	}
	free(pContent);
	return json;
}

int initStreamOutput(struct StreamOutput *streamOutput) {

	int ret = 0;
	AVFormatContext *ofmt_ctx = NULL;
	char *out_stream = streamOutput->out_stream;
	int fps = streamOutput->fps;
	int final_width = streamOutput->final_width;
	int final_height = streamOutput->final_height;
	int bitrate = streamOutput->bitrate;

	if ((out_stream[0] == 'r') && (out_stream[1] == 't') && (out_stream[2] == 's') && (out_stream[3] == 'p')) {
		avformat_alloc_output_context2(&ofmt_ctx, NULL, "rtsp", out_stream);
	}
	else {
		avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_stream);
	}

	if (!ofmt_ctx) {
		ret = AVERROR_UNKNOWN;
		return ret;
	}

	videoTimeBase.num = 1;
	videoTimeBase.den = 90000;
	audioTimeBase.num = 1;
	audioTimeBase.den = 44100;

	AVCodec *videoEncoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
	AVStream *videoOutStream = avformat_new_stream(ofmt_ctx, videoEncoder);
	videoOutStream->time_base = videoTimeBase;
	AVCodecContext *videoEnc_ctx = videoOutStream->codec;
	videoEnc_ctx->width = final_width;
	videoEnc_ctx->height = final_height;
	videoEnc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	videoEnc_ctx->time_base = (AVRational){ 1, fps };
	videoEnc_ctx->bit_rate = bitrate;
	videoEnc_ctx->max_b_frames = 0;
	videoEnc_ctx->profile = FF_PROFILE_H264_MAIN;
	videoEnc_ctx->gop_size = 25;
	videoEnc_ctx->b_frame_strategy = 0;
	streamOutput->cVideo_timestamp_gap = av_rescale_q(1, videoEnc_ctx->time_base, videoOutStream->time_base);

	ret = avcodec_open2(videoEnc_ctx, videoEncoder, NULL);
	if (ret < 0) {
		return ret;
	}

	//AVCodec *audioEncoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
	AVCodec *audioEncoder = avcodec_find_encoder_by_name("aac");
	AVStream *audioOutStream = avformat_new_stream(ofmt_ctx, audioEncoder);
	audioOutStream->time_base = audioTimeBase;
	AVCodecContext *audioEnc_ctx = audioOutStream->codec;
	audioEnc_ctx->sample_rate = 44100;
	audioEnc_ctx->channels = 2;
	audioEnc_ctx->time_base = (AVRational){ 1, audioEnc_ctx->sample_rate };
	audioEnc_ctx->channel_layout = av_get_default_channel_layout(audioEnc_ctx->channels);
	audioEnc_ctx->sample_fmt = audioEncoder->sample_fmts[0];
	audioEnc_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
	if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		audioEnc_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;

	ret = avcodec_open2(audioEnc_ctx, audioEncoder, NULL);
	if (ret < 0) {
		return ret;
	}
	streamOutput->cAudio_timestamp_gap = audioEnc_ctx->frame_size;

	if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		if ((ret = avio_open(&ofmt_ctx->pb, out_stream, AVIO_FLAG_WRITE)) < 0) {
			printf("can not open the out put file handle!\n");
			return ret;
		}
	}

	streamOutput->audioEnc_ctx = audioEnc_ctx;
	streamOutput->videoEnc_ctx = videoEnc_ctx;
	streamOutput->audioOutStream = audioOutStream;
	streamOutput->videoOutStream = videoOutStream;
	streamOutput->ofmt_ctx = ofmt_ctx;

	return ret;
}

int open_input_stream(struct StreamInput *streamInput, AVFormatContext **pFormatCtx) {

	int ret;
	AVCodec *dec;
	char *sdpName = streamInput->stream;
	AVInputFormat* infmt = av_find_input_format("rtsp");
a0
	if ((ret = avformat_open_input(pFormatCtx, sdpName, infmt, NULL)) < 0) {
		printf("Cannot open input \"%s\"\n", sdpName);
		return ret;
	}

	if ((ret = avformat_find_stream_info(*pFormatCtx, NULL)) < 0) {
		printf("Cannot find stream information\n");
		return ret;
	}

	/* select the video stream */
	ret = av_find_best_stream(*pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
	if (ret < 0) {
		streamInput->hasVideo = 0;
		printf("Cannot find a video stream in the input file\n");
		return ret;
	}
	streamInput->video_stream_index = ret;
	streamInput->pVideoCodecCtx = (*pFormatCtx)->streams[streamInput->video_stream_index]->codec;

	/* init the video decoder */
	if ((ret = avcodec_open2(streamInput->pVideoCodecCtx, dec, NULL)) < 0) {
		printf("Cannot open video decoder\n");
		return ret;
	}

	/* select the audio stream */
	ret = av_find_best_stream(*pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
	if (ret < 0) {
		printf("Cannot find a audio stream in the input file\n");
		return ret;
	}
	streamInput->audio_stream_index = ret;
	streamInput->pAudioCodecCtx = (*pFormatCtx)->streams[streamInput->audio_stream_index]->codec;

	/* init the audio decoder */
	if ((ret = avcodec_open2(streamInput->pAudioCodecCtx, dec, NULL)) < 0) {
		printf("Cannot open audio decoder\n");
		return ret;
	}

	return 0;
}

static void setup_array(uint8_t* out[32], AVFrame* in_frame, int format, int samples) {

	if (av_sample_fmt_is_planar(format)) {
		int i; int plane_size = av_get_bytes_per_sample((format & 0xFF)) * samples;
		format &= 0xFF;
		in_frame->data[0] + i*plane_size;
		for (i = 0; i < in_frame->channels; i++) {
			out[i] = in_frame->data[i];
		}
	}
	else {
		out[0] = in_frame->data[0];
	}
}

int audioResample(AVFrame *in_frame, AVFrame *out_frame, AVCodecContext *out_codec_ctx, struct StreamInput *streamInput) {

	int ret;
	int max_dst_nb_samples = 4096;
	int src_nb_samples = in_frame->nb_samples;
	out_frame->pts = in_frame->pts;
	uint8_t* paudiobuf;
	int decode_size, input_size, len;

	AVCodecContext *in_codec_ctx = streamInput->pAudioCodecCtx;
	SwrContext *swr_ctx = streamInput->swr_ctx;

	if (swr_ctx == NULL) {
		swr_ctx = swr_alloc();
		av_opt_set_int(swr_ctx, "ich", in_codec_ctx->channels, 0);
		av_opt_set_int(swr_ctx, "och", out_codec_ctx->channels, 0);
		av_opt_set_int(swr_ctx, "in_sample_rate", in_codec_ctx->sample_rate, 0);
		av_opt_set_int(swr_ctx, "out_sample_rate", out_codec_ctx->sample_rate, 0);
		av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", in_codec_ctx->sample_fmt, 0);
		av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_codec_ctx->sample_fmt, 0);

		swr_init(swr_ctx);
		streamInput->swr_ctx = swr_ctx;
	}

	if (swr_ctx != NULL) {
		out_frame->nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, out_codec_ctx->sample_rate) + src_nb_samples,
			out_codec_ctx->sample_rate, in_codec_ctx->sample_rate, AV_ROUND_UP);

		ret = av_samples_alloc(out_frame->data,
			&out_frame->linesize[0],
			out_codec_ctx->channels,
			out_frame->nb_samples,
			out_codec_ctx->sample_fmt, 0);

		if (ret < 0) {
			av_log(NULL, AV_LOG_WARNING, "[%s.%d %s() Could not allocate samples Buffer\n", __FILE__, __LINE__, __FUNCTION__);
			return -1;
		}

		uint8_t* m_ain[32];
		setup_array(m_ain, in_frame, in_codec_ctx->sample_fmt, src_nb_samples);

		len = swr_convert(swr_ctx, out_frame->data, out_frame->nb_samples, (const uint8_t**)m_ain, src_nb_samples);

		if (len < 0) {
			char errmsg[1000];
			av_strerror(len, errmsg, sizeof(errmsg));
			av_log(NULL, AV_LOG_WARNING, "[%s:%d] swr_convert!(%d)(%s)", __FILE__, __LINE__, len, errmsg);
			return -1;
		}
	}
	else {
		printf("pSwrCtx with out init!\n");
		return -1;
	}
	return 0;
}

void videoScale(struct StreamInput *streamInput, AVFrame *src, AVFrame *dst) {

	SwsContext *img_convert_ctx = streamInput->img_convert_ctx;
	if (img_convert_ctx == NULL) {
		img_convert_ctx = sws_alloc_context();
		av_opt_set_int(img_convert_ctx, "sws_flags", SWS_BICUBIC | SWS_PRINT_INFO, 0);
		av_opt_set_int(img_convert_ctx, "srcw", src->width, 0);
		av_opt_set_int(img_convert_ctx, "srch", src->height, 0);
		av_opt_set_int(img_convert_ctx, "src_format", src->format, 0);
		av_opt_set_int(img_convert_ctx, "dstw", dst->width, 0);
		av_opt_set_int(img_convert_ctx, "dsth", dst->height, 0);
		av_opt_set_int(img_convert_ctx, "dst_format", dst->format, 0);

		sws_init_context(img_convert_ctx, NULL, NULL);
		streamInput->img_convert_ctx = img_convert_ctx;
	}
	sws_scale(img_convert_ctx, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
}

AVFrame *blendImage(AVFrame *blend_frames[], struct StreamInput *streamInputs[], int stream_count, int final_width, int final_height) {

	AVFrame* dst = av_frame_alloc();
	dst->width = final_width;
	dst->height = final_height;
	dst->format = PIX_FMT_YUV420P;

	////// Initialize frame->linesize and frame->data pointers
	//avpicture_alloc((AVPicture*)dst, PIX_FMT_YUV420P, dst->width, dst->height);

	// Allocate a buffer large enough for all data
	int size = avpicture_get_size(PIX_FMT_YUV420P, dst->width, dst->height);
	blendBuffer = (uint8_t*)av_malloc(size);
	memset(blendBuffer, 0, size);
	avpicture_fill((AVPicture*)dst, blendBuffer, PIX_FMT_YUV420P, dst->width, dst->height);

	int64_t max_pts = AV_NOPTS_VALUE;
	for (int st_index = 0; st_index < stream_count; st_index++) {

		AVFrame *frame = blend_frames[st_index];
		struct StreamInput *streamInput = streamInputs[st_index];
		int dest_x = streamInput->dest_x;
		int dest_y = streamInput->dest_y;

		// Copy data from the 3 input buffers
		for (int i = 0; i < frame->height; i++) {
			memcpy(dst->data[0] + dst->linesize[0] * dest_y + dst->linesize[0] * i + dest_x, (uint8_t *)frame->opaque + i * frame->width, frame->width);
		}
		for (int i = 0; i < frame->height / 2; i++) {
			memcpy(dst->data[1] + dst->linesize[1] * dest_y / 2 + dst->linesize[1] * i + dest_x / 2, (uint8_t *)frame->opaque + frame->width * frame->height + i * frame->width / 2, frame->width / 2);
		}
		for (int i = 0; i < frame->height / 2; i++) {
			memcpy(dst->data[2] + dst->linesize[2] * dest_y / 2 + dst->linesize[2] * i + dest_x / 2, (uint8_t *)frame->opaque + frame->width * frame->height * 5 / 4 + i * frame->width / 2, frame->width / 2);
		}
		if (frame->pts != AV_NOPTS_VALUE) {
			if (frame->pts > max_pts) {
				max_pts = frame->pts;
			}
		}
		free(frame->opaque);
		avpicture_free((AVPicture *)frame);
		av_frame_free(&frame);
	}
	dst->pts = max_pts;
	return dst;
}

void audioMix(AVFrame *mix_frames[], int stream_count) {

	int channels = mix_frames[0]->channels;
	int nb_samples = mix_frames[0]->nb_samples;

	for (int c = 0; c < channels; c++) {
		float *channelData = mix_frames[0]->extended_data[c];

		for (int i = 1; i < stream_count; i++) {
			AVFrame *frame = mix_frames[i];
			float *temp = frame->extended_data[c];
			for (int nb = 0; nb < nb_samples; nb++) {
				channelData[nb] += temp[nb];
			}
		}
		for (int nb = 0; nb < nb_samples; nb++) {
			channelData[nb] = channelData[nb] / stream_count;
		}
	}
	for (int i = 1; i < stream_count; i++) {
		free(mix_frames[i]->opaque);
		av_frame_free(&mix_frames[i]);
	}
}

static void pushCycle(struct StreamInput *myStruct) {

	AVFormatContext *pFormatCtx = myStruct->pFormatCtx;
	AVCodecContext *pCodecCtx;
	struct AVFrameList *video_frameList = myStruct->video_frameList;
	struct AVFrameList *audio_frameList = myStruct->audio_frameList;
	int video_stream_index = myStruct->video_stream_index;

	AVPacket packet;
	int got_frame;
	int ret = 0;

	while (1) {

		av_init_packet(&packet);
		packet.data = NULL;
		packet.size = 0;
		ret = av_read_frame(pFormatCtx, &packet);
		if (ret < 0)
			continue;
		if (packet.stream_index == video_stream_index) {
			if (video_frameList->size > MAX_QUEUE_SIZE) {
				av_free_packet(&packet);
				continue;
			}
		}
		else {
			if (audio_frameList->size > MAX_QUEUE_SIZE) {
				av_free_packet(&packet);
				continue;
			}
		}
		int64_t start_time = pFormatCtx->streams[packet.stream_index]->start_time;
		if (packet.dts != AV_NOPTS_VALUE && start_time != AV_NOPTS_VALUE)
			packet.dts -= start_time;
		if (packet.pts != AV_NOPTS_VALUE && start_time != AV_NOPTS_VALUE)
			packet.pts -= start_time;

		if (packet.stream_index == video_stream_index) {

			pCodecCtx = myStruct->pVideoCodecCtx;
			got_frame = 0;
			AVFrame *pFrame = av_frame_alloc();

			ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_frame, &packet);
			if (ret < 0 || !got_frame) {
				continue;
			}
			pFrame->pts = av_frame_get_best_effort_timestamp(pFrame);

			/************************************************************************/
			/*                                                                      */
			/************************************************************************/

			AVFrame* scaledFrame = av_frame_alloc();
			scaledFrame->width = myStruct->dest_width;
			scaledFrame->height = myStruct->dest_height;
			scaledFrame->format = PIX_FMT_YUV420P;

			//int size2 = avpicture_get_size(scaledFrame->format, scaledFrame->width, scaledFrame->height);
			//avpicture_alloc((AVPicture*)scaledFrame, scaledFrame->format, scaledFrame->width, scaledFrame->height);
			int size2 = avpicture_get_size(scaledFrame->format, scaledFrame->width, scaledFrame->height);
			uint8_t *buffer2 = (uint8_t *)av_malloc(size2);
			avpicture_fill((AVPicture*)scaledFrame, buffer2, scaledFrame->format, scaledFrame->width, scaledFrame->height);
			
			videoScale(myStruct, pFrame, scaledFrame);
			scaledFrame->pts = pFrame->pts;
			scaledFrame->pkt_dts = pFrame->pkt_dts;
			scaledFrame->pkt_pts = pFrame->pkt_pts;

			av_frame_free(&pFrame);
			pFrame = scaledFrame;

			/************************************************************************/
			/*                                                                      */
			/************************************************************************/

			// Allocate a buffer large enough for all data
			int size = avpicture_get_size(pCodecCtx->pix_fmt, scaledFrame->width, scaledFrame->height);
			uint8_t *buffer = (uint8_t *)malloc(size);

			for (int i = 0; i < pFrame->height; i++) {
				memcpy(buffer + i*pFrame->width, pFrame->data[0] + pFrame->linesize[0] * i, pFrame->width);
			}
			for (int i = 0; i < pFrame->height / 2; i++) {
				memcpy(buffer + pFrame->width*pFrame->height + i*pFrame->width / 2, pFrame->data[1] + pFrame->linesize[1] * i, pFrame->width / 2);
			}
			for (int i = 0; i < pFrame->height / 2; i++) {
				memcpy(buffer + pFrame->width*pFrame->height * 5 / 4 + i*pFrame->width / 2, pFrame->data[2] + pFrame->linesize[2] * i, pFrame->width / 2);
			}
			pFrame->opaque = buffer;
			push_back(video_frameList, pFrame);
		}
		else {
			pCodecCtx = myStruct->pAudioCodecCtx;
			got_frame = 0;
			AVFrame *pFrame = av_frame_alloc();

			ret = avcodec_decode_audio4(pCodecCtx, pFrame, &got_frame, &packet);
			if (ret < 0 || !got_frame) {
				continue;
			}
			pFrame->pts = av_frame_get_best_effort_timestamp(pFrame);

			/************************************************************************/
			/*                                                                      */
			/************************************************************************/

			AVCodecContext *audioEnc_ctx = myStruct->streamOutput->audioEnc_ctx;
			AVFrame *resampleFrame = av_frame_alloc();
			resampleFrame->nb_samples = audioEnc_ctx->frame_size;
			resampleFrame->channel_layout = audioEnc_ctx->channel_layout;
			resampleFrame->format = audioEnc_ctx->sample_fmt;
			resampleFrame->channels = audioEnc_ctx->channels;
			resampleFrame->sample_rate = audioEnc_ctx->sample_rate;
			audioResample(pFrame, resampleFrame, audioEnc_ctx, myStruct);

			resampleFrame->pts = pFrame->pts;
			resampleFrame->pkt_dts = pFrame->pkt_dts;
			resampleFrame->pkt_pts = pFrame->pkt_pts;

			av_audio_fifo_write(myStruct->audioFifo, (void **)resampleFrame->data, resampleFrame->nb_samples);
			while (av_audio_fifo_size(myStruct->audioFifo) >= audioEnc_ctx->frame_size) {

				AVFrame *resampleFrame2 = av_frame_alloc();
				resampleFrame2->nb_samples = audioEnc_ctx->frame_size;
				resampleFrame2->channels = audioEnc_ctx->channels;
				resampleFrame2->channel_layout = audioEnc_ctx->channel_layout;
				resampleFrame2->format = audioEnc_ctx->sample_fmt;
				resampleFrame2->sample_rate = audioEnc_ctx->sample_rate;

				ret = av_frame_get_buffer(resampleFrame2, 0);
				if (ret < 0) {
					break;
				}
				ret = av_audio_fifo_read(myStruct->audioFifo, (void **)resampleFrame2->data, audioEnc_ctx->frame_size);
				resampleFrame2->pts = resampleFrame->pts;
				resampleFrame2->pkt_dts = resampleFrame->pkt_dts;
				resampleFrame2->pkt_pts = resampleFrame->pkt_pts;

				int size = resampleFrame2->linesize[0] * resampleFrame2->channels;
				uint8_t *buffer = (uint8_t *)malloc(size);
				for (int i = 0; i < resampleFrame2->channels; i++) {
					memcpy(buffer + resampleFrame2->linesize[0] * i, resampleFrame2->extended_data[i], resampleFrame2->linesize[0]);
				}
				resampleFrame2->opaque = buffer;

				push_back(audio_frameList, resampleFrame2);
			}
			av_free(resampleFrame->data[0]);
			av_frame_free(&resampleFrame);
			av_frame_free(&pFrame);
		}
		av_free_packet(&packet);
	}
	if (myStruct->img_convert_ctx != NULL) {
		sws_freeContext(myStruct->img_convert_ctx);
	}
}

#ifdef WIN32
static DWORD WINAPI threadWork(LPVOID threadNo) {
	struct StreamInput *myStruct = (struct StreamInput *)threadNo;
	pushCycle(myStruct);
	return 0;
}

#else
static void *demuxThread(void *data) {
	struct StreamInput *myStruct = (struct StreamInput *)data;
	pushCycle(myStruct);
	return NULL;
}
#endif // WIN32

static int encode_write_frame(AVFrame *filt_frame, unsigned int stream_index, struct StreamOutput *streamOutput) {

	AVFormatContext *ofmt_ctx = streamOutput->ofmt_ctx;
	AVCodecContext *audioEnc_ctx = streamOutput->audioEnc_ctx;
	int ret;
	AVPacket enc_pkt;

	int got = 1;
	int *got_frame = &got;

	filt_frame->pts = AV_NOPTS_VALUE;
	/* encode filtered frame */
	enc_pkt.data = NULL;
	enc_pkt.size = 0;
	av_init_packet(&enc_pkt);
	if (stream_index == 0) {
		streamOutput->video_sequence_number++;
		filt_frame->pts = streamOutput->video_sequence_number;

		//filt_frame->pts = av_rescale_q_rnd(filt_frame->pts, AV_TIME_BASE_Q,
		//	ofmt_ctx->streams[stream_index]->codec->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
		ret = avcodec_encode_video2(ofmt_ctx->streams[stream_index]->codec, &enc_pkt, filt_frame, got_frame);
		avpicture_free((AVPicture *)filt_frame);
	}
	else {
		streamOutput->audio_sequence_number += filt_frame->nb_samples;
		filt_frame->pts = av_rescale_q(streamOutput->audio_sequence_number, (AVRational){ 1, audioEnc_ctx->sample_rate }, audioEnc_ctx->time_base);

		//filt_frame->pts = av_rescale_q_rnd(filt_frame->pts, AV_TIME_BASE_Q,
		//	ofmt_ctx->streams[stream_index]->codec->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
		ret = avcodec_encode_audio2(ofmt_ctx->streams[stream_index]->codec, &enc_pkt, filt_frame, got_frame);
		free(filt_frame->opaque);
	}
	av_frame_free(&filt_frame);

	if (ret < 0)
		return ret;
	if (!(*got_frame))
		return 0;

	/* prepare packet for muxing */
	enc_pkt.stream_index = stream_index;
	av_packet_rescale_ts(&enc_pkt,
		ofmt_ctx->streams[stream_index]->codec->time_base,
		ofmt_ctx->streams[stream_index]->time_base);

	av_log(NULL, AV_LOG_DEBUG, "Muxing frame\n");
	/* mux encoded frame */
	ret = av_interleaved_write_frame(ofmt_ctx, &enc_pkt);
	return ret;
}

int process(struct StreamInput *streamInputs[], int stream_count, struct StreamOutput streamOutput) {

	int ret = 0;

	ret = initStreamOutput(&streamOutput);
	if (ret < 0) {
		return ret;
	}
	AVFormatContext *ofmt_ctx = streamOutput.ofmt_ctx;
	if (ofmt_ctx == NULL) {
		printf("init output stream error!\r\n");
		return -1;
	}

	AVCodecContext *audioEnc_ctx = streamOutput.audioEnc_ctx;
	AVCodecContext *videoEnc_ctx = streamOutput.videoEnc_ctx;
	//parse input stream
	for (int i = 0; i < stream_count; i++) {
		AVFormatContext *pFormatCtx = avformat_alloc_context();

		if ((ret = open_input_stream(streamInputs[i], &pFormatCtx)) < 0)
			return -1;

		streamInputs[i]->pFormatCtx = pFormatCtx;
		streamInputs[i]->pVideoCodecCtx = pFormatCtx->streams[streamInputs[i]->video_stream_index]->codec;
		streamInputs[i]->pAudioCodecCtx = pFormatCtx->streams[streamInputs[i]->audio_stream_index]->codec;
		streamInputs[i]->img_convert_ctx = NULL;
		streamInputs[i]->audioFifo = av_audio_fifo_alloc(audioEnc_ctx->sample_fmt, audioEnc_ctx->channels, 10000);
		streamInputs[i]->swr_ctx = NULL;
		streamInputs[i]->streamOutput = &streamOutput;
	}

	for (int i = 0; i < stream_count; i++) {

		struct AVFrameList *video_frameList = (struct AVFrameList *)malloc(sizeof(struct AVFrameList));
		video_frameList->head = NULL;
		video_frameList->tail = NULL;
		video_frameList->size = 0;

		struct AVFrameList *audio_frameList = (struct AVFrameList *)malloc(sizeof(struct AVFrameList));
		audio_frameList->head = NULL;
		audio_frameList->tail = NULL;
		audio_frameList->size = 0;

#ifdef WIN32
		video_frameList->hMutex = CreateMutex(NULL, FALSE, NULL);
		audio_frameList->hMutex = CreateMutex(NULL, FALSE, NULL);

		streamInputs[i]->video_frameList = video_frameList;
		streamInputs[i]->audio_frameList = audio_frameList;
		CreateThread(NULL, 0, threadWork, streamInputs[i], NULL, NULL);
#else
		pthread_mutex_t vpMutex;
		pthread_mutex_init(&vpMutex, NULL);
		video_frameList->pMutex = vpMutex;
		pthread_mutex_t apMutex;
		pthread_mutex_init(&apMutex, NULL);
		audio_frameList->pMutex = apMutex;

		streamInputs[i]->video_frameList = video_frameList;
		streamInputs[i]->audio_frameList = audio_frameList;
		pthread_t ntid;
		pthread_create(&ntid, NULL, demuxThread, (void*)streamInputs[i]);
#endif
	}

	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) {
		return ret;
	}
	int ccc = 0;
	while (1) {

		//video
		int videoQueueSize = MAX_QUEUE_SIZE;
		for (int i = 0; i < stream_count; i++) {
			if (videoQueueSize > streamInputs[i]->video_frameList->size) {
				videoQueueSize = streamInputs[i]->video_frameList->size;
			}
		}

		while (videoQueueSize > 0){
			videoQueueSize--;

			for (int i = 0; i < stream_count; i++) {
				blend_frames[i] = pop_front(streamInputs[i]->video_frameList);
			}

			AVFrame *dst = blendImage(blend_frames, streamInputs, stream_count, streamOutput.final_width, streamOutput.final_height);
			encode_write_frame(dst, 0, &streamOutput);
		}

		//audio
		int audioQueueSize = MAX_QUEUE_SIZE;
		for (int i = 0; i < stream_count; i++) {
			if (audioQueueSize > streamInputs[i]->audio_frameList->size) {
				audioQueueSize = streamInputs[i]->audio_frameList->size;
			}
		}

		while (audioQueueSize > 0){
			audioQueueSize--;

			for (int i = 0; i < stream_count; i++) {
				mix_frames[i] = pop_front(streamInputs[i]->audio_frameList);
				for (int c = 0; c < mix_frames[i]->channels; c++) {
					mix_frames[i]->extended_data[c] = ((uint8_t *)mix_frames[i]->opaque) + mix_frames[i]->linesize[0] * c;
				}
			}
			audioMix(mix_frames, stream_count);
			encode_write_frame(mix_frames[0], 1, &streamOutput);
		}
	}

	av_write_trailer(ofmt_ctx);
	avformat_close_input(&ofmt_ctx);
	return ret;
}

int main(int argc, char* argv[]) {

	struct StreamInput *streamInputs[20];

	cJSON *root = GetJsonObject(argv[1]);
	cJSON *stream = cJSON_GetObjectItem(root, "stream");
	int stream_count = cJSON_GetArraySize(stream);
	if (stream_count > MAX_STREAM_COUNT) {
		printf("Not support so many streams, 10 streams at most. \r\n");
		return -1;
	}

	char *out_stream = cJSON_GetObjectItem(root, "out_stream")->valuestring;
	int final_width = cJSON_GetObjectItem(root, "final_width")->valueint;
	int final_height = cJSON_GetObjectItem(root, "final_height")->valueint;
	int bitrate = cJSON_GetObjectItem(root, "bitrate")->valueint;
	int fps = cJSON_GetObjectItem(root, "fps")->valueint;

	struct StreamOutput streamOutput;
	streamOutput.out_stream = out_stream;
	streamOutput.final_height = final_height;
	streamOutput.final_width = final_width;
	streamOutput.bitrate = bitrate;
	streamOutput.fps = fps;
	streamOutput.video_sequence_number = 0;
	streamOutput.audio_sequence_number = 0;
	streamOutput.cAudioTimestamp = 0;
	streamOutput.cVideoTimestamp = 0;

	for (int index = 0; index < stream_count; index++) {
		cJSON* item = cJSON_GetArrayItem(stream, index);
		int width = cJSON_GetObjectItem(item, "dest_width")->valueint;

		struct StreamInput *streamInput1 = (struct StreamInput *)malloc(sizeof(struct StreamInput));
		streamInput1->stream = cJSON_GetObjectItem(item, "name")->valuestring;
		streamInput1->layer = cJSON_GetObjectItem(item, "layer")->valueint;
		streamInput1->dest_x = cJSON_GetObjectItem(item, "dest_x")->valueint;
		streamInput1->dest_y = cJSON_GetObjectItem(item, "dest_y")->valueint;
		streamInput1->dest_width = cJSON_GetObjectItem(item, "dest_width")->valueint;
		streamInput1->dest_height = cJSON_GetObjectItem(item, "dest_height")->valueint;

		if (streamInput1->dest_x < 0 || streamInput1->dest_y < 0 || streamInput1->dest_x + streamInput1->dest_width > final_width || streamInput1->dest_y + streamInput1->dest_height > final_height) {
			printf("Resolution out of range.\r\n");
			return -1;
		}
		streamInputs[index] = streamInput1;
	}

	for (int i = 0; i < stream_count - 1; i++) {
		for (int j = i + 1; j < stream_count; j++) {
			if (streamInputs[i]->layer > streamInputs[j]->layer) {

				struct StreamInput *streamTemp = streamInputs[i];
				streamInputs[i] = streamInputs[j];
				streamInputs[j] = streamTemp;
			}
		}
	}

	av_register_all();
	avformat_network_init();
	avfilter_register_all();

	process(streamInputs, stream_count, streamOutput);

	return 0;
}
