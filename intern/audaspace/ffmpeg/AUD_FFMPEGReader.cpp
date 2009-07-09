/*
 * $Id$
 *
 * ***** BEGIN LGPL LICENSE BLOCK *****
 *
 * Copyright 2009 Jörg Hermann Müller
 *
 * This file is part of AudaSpace.
 *
 * AudaSpace is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * AudaSpace is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with AudaSpace.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ***** END LGPL LICENSE BLOCK *****
 */

#include "AUD_FFMPEGReader.h"
#include "AUD_Buffer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// This function transforms a FFMPEG SampleFormat to or own sample format
static inline AUD_SampleFormat FFMPEG_TO_AUD(SampleFormat fmt)
{
	switch(fmt)
	{
	case SAMPLE_FMT_U8:
		return AUD_FORMAT_U8;
	case SAMPLE_FMT_S16:
		return AUD_FORMAT_S16;
	case SAMPLE_FMT_S32:
		return AUD_FORMAT_S32;
	case SAMPLE_FMT_FLT:
		return AUD_FORMAT_FLOAT32;
	case SAMPLE_FMT_DBL:
		return AUD_FORMAT_FLOAT64;
	default:
		return AUD_FORMAT_INVALID;
	}
}

AUD_FFMPEGReader::AUD_FFMPEGReader(const char* filename)
{
	m_position = 0;
	m_pkgbuf_left = 0;

	// open file
	if(av_open_input_file(&m_formatCtx, filename, NULL, 0, NULL)!=0)
		AUD_THROW(AUD_ERROR_FILE);

	try
	{
		if(av_find_stream_info(m_formatCtx)<0)
			AUD_THROW(AUD_ERROR_FFMPEG);

		// this prints file information to stdout:
		//dump_format(m_formatCtx, 0, filename, 0);

		// find audio stream and codec
		m_stream = -1;

		for(int i = 0; i < m_formatCtx->nb_streams; i++)
			if((m_formatCtx->streams[i]->codec->codec_type == CODEC_TYPE_AUDIO)
				&& (m_stream < 0))
			{
				m_stream=i;
				break;
			}
		if(m_stream == -1)
			AUD_THROW(AUD_ERROR_FFMPEG);

		m_codecCtx = m_formatCtx->streams[m_stream]->codec;

		m_specs.channels = (AUD_Channels) m_codecCtx->channels;
		m_specs.format = FFMPEG_TO_AUD(m_codecCtx->sample_fmt);
		m_specs.rate = (AUD_SampleRate) m_codecCtx->sample_rate;

		// get a decoder and open it
		AVCodec *aCodec = avcodec_find_decoder(m_codecCtx->codec_id);
		if(!aCodec)
			AUD_THROW(AUD_ERROR_FFMPEG);

		if(avcodec_open(m_codecCtx, aCodec)<0)
			AUD_THROW(AUD_ERROR_FFMPEG);
	}
	catch(AUD_Exception e)
	{
		av_close_input_file(m_formatCtx);
		throw;
	}

	// last but not least if there hasn't been any error, create the buffers
	m_buffer = new AUD_Buffer(0); AUD_NEW("buffer")
	m_pkgbuf = new AUD_Buffer(AVCODEC_MAX_AUDIO_FRAME_SIZE<<1);
}

AUD_FFMPEGReader::~AUD_FFMPEGReader()
{
	// close the file
	avcodec_close(m_codecCtx);
	av_close_input_file(m_formatCtx);
	delete m_buffer; AUD_DELETE("buffer")
	delete m_pkgbuf;
}

bool AUD_FFMPEGReader::isSeekable()
{
	return true;
}

void AUD_FFMPEGReader::seek(int position)
{
	if(position >= 0)
	{
		// a value < 0 tells us that seeking failed
		if(av_seek_frame(m_formatCtx,
						 -1,
						 (uint64_t)(((uint64_t)position *
									 (uint64_t)AV_TIME_BASE) /
									(uint64_t)m_specs.rate),
						 AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY) >= 0)
		{
			avcodec_flush_buffers(m_codecCtx);
			m_position = position;
			m_pkgbuf_left = 0;
		}
		else
		{
			// XXX printf("Seeking failed!\n");
		}
	}
}

int AUD_FFMPEGReader::getLength()
{
	// return approximated remaning size
	return (int)((m_formatCtx->duration * m_codecCtx->sample_rate)
				 / AV_TIME_BASE)-m_position;
}

int AUD_FFMPEGReader::getPosition()
{
	return m_position;
}

AUD_Specs AUD_FFMPEGReader::getSpecs()
{
	return m_specs;
}

AUD_ReaderType AUD_FFMPEGReader::getType()
{
	return AUD_TYPE_STREAM;
}

void AUD_FFMPEGReader::read(int & length, sample_t* & buffer)
{
	// read packages and decode them
	AVPacket packet;
	int audio_pkg_size;
	uint8_t *audio_pkg_data;
	int read_length;
	int data_size = 0;
	int pkgbuf_size = m_pkgbuf->getSize();
	int pkgbuf_pos;
	int left = length;
	int sample_size = AUD_SAMPLE_SIZE(m_specs);

	// resize output buffer if necessary
	if(m_buffer->getSize() < length*sample_size)
		m_buffer->resize(length*sample_size, false);

	buffer = m_buffer->getBuffer();
	pkgbuf_pos = m_pkgbuf_left;
	m_pkgbuf_left = 0;

	// there may still be data in the buffer from the last call
	if(pkgbuf_pos > 0)
	{
		data_size = AUD_MIN(pkgbuf_pos, left*sample_size);
		memcpy(buffer, m_pkgbuf->getBuffer(), data_size);
		buffer += data_size;
		left -= data_size/sample_size;
	}

	// for each frame read as long as there isn't enough data already
	while((left > 0) && (av_read_frame(m_formatCtx, &packet) >= 0))
	{
		// is it a frame from the audio stream?
		if(packet.stream_index == m_stream)
		{
			// save packet parameters
			audio_pkg_data = packet.data;
			audio_pkg_size = packet.size;
			pkgbuf_pos = 0;

			// as long as there is still data in the package
			while(audio_pkg_size > 0)
			{
				// resize buffer if needed
				if(pkgbuf_size-pkgbuf_pos < AVCODEC_MAX_AUDIO_FRAME_SIZE)
				{
					// XXX printf("resizing\n");
					m_pkgbuf->resize(pkgbuf_size +
									 AVCODEC_MAX_AUDIO_FRAME_SIZE);
					pkgbuf_size += AVCODEC_MAX_AUDIO_FRAME_SIZE;
				}

				// read samples from the packet
				data_size = pkgbuf_size-pkgbuf_pos;
				read_length = avcodec_decode_audio2(m_codecCtx,
					(int16_t*)(m_pkgbuf->getBuffer()+pkgbuf_pos),
					&data_size,
					audio_pkg_data,
					audio_pkg_size);

				pkgbuf_pos += data_size;

				// read error, next packet!
				if(read_length < 0)
					break;

				// move packet parameters
				audio_pkg_data += read_length;
				audio_pkg_size -= read_length;
			}
			// copy to output buffer
			data_size = AUD_MIN(pkgbuf_pos, left*sample_size);
			memcpy(buffer, m_pkgbuf->getBuffer(), data_size);
			buffer += data_size;
			left -= data_size/sample_size;
		}
		av_free_packet(&packet);
	}
	// read more data than necessary?
	if(pkgbuf_pos > data_size)
	{
		m_pkgbuf_left = pkgbuf_pos-data_size;
		memmove(m_pkgbuf->getBuffer(), m_pkgbuf->getBuffer()+data_size,
				pkgbuf_pos-data_size);
	}

	buffer = m_buffer->getBuffer();
	length -= left;
	m_position += length;
}
