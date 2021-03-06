/*
 *  Functions for probing file contents
 *  Copyright (C) 2008 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#if ENABLE_LIBGME
#include <gme/gme.h>
#endif

#include <libavutil/avstring.h>

#include "showtime.h"
#include "fileaccess.h"
#include "fa_probe.h"
#include "fa_libav.h"
#include "navigator.h"
#include "media.h"
#include "misc/string.h"
#include "misc/isolang.h"
#include "misc/jpeg.h"
#include "htsmsg/htsmsg_json.h"
#include "metadata.h"

/**
 *
 */
static const char *
codecname(enum CodecID id)
{
  AVCodec *c;

  switch(id) {
  case CODEC_ID_AC3:
    return "AC3";
  case CODEC_ID_EAC3:
    return "EAC3";
  case CODEC_ID_DTS:
    return "DTS";
  case CODEC_ID_TEXT:
  case CODEC_ID_MOV_TEXT:
    return "Text";
  case CODEC_ID_SSA:
    return "SSA";

  default:
    c = avcodec_find_decoder(id);
    if(c)
      return c->name;
    return "Unsupported Codec";
  }
}






static const uint8_t pngsig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
static const uint8_t isosig[8] = {0x1, 0x43, 0x44, 0x30, 0x30, 0x31, 0x1, 0x0};
static const uint8_t gifsig[6] = {'G', 'I', 'F', '8', '9', 'a'};


/**
 *
 */
static rstr_t *
ffmpeg_metadata_get(AVMetadata *m, const char *key)
{
  AVMetadataTag *tag;
  int len;
  rstr_t *ret;
  const char *str;
  char *d;

  if((tag = av_metadata_get(m, key, NULL, AV_METADATA_IGNORE_SUFFIX)) == NULL)
    return NULL;

  if(!utf8_verify(tag->value))
    return NULL;

  str = tag->value;
  len = strlen(str);
  ret = rstr_allocl(str, len);
  d = rstr_data(ret);

  while(len > 0) {
    len--;
    if(d[len] <= ' ' || d[len] == '-')
      d[len] = 0;
    else
      break;
  }
  if(*d == 0 || !strncasecmp(d, "http://", 7)) {
    rstr_release(ret);
    return NULL;
  }
  return ret;
}

#if 0
/**
 * Obtain details from playlist
 */
static void
fa_probe_playlist(metadata_t *md, const char *url, uint8_t *pb, size_t pbsize)
{
  const char *t;
  char tmp1[300];
  int i;

  t = strrchr(url, '/');
  t = t ? t + 1 : url;

  i = 0;
  while(*t && *t != '.')
    tmp1[i++] = *t++;
  tmp1[i] = 0;
  
  md->md_title = rstr_alloc(tmp1);
  
  t = strstr((char *)pb, "NumberOfEntries=");
  
  if(t != NULL)
    md->md_tracks = atoi(t + 16);
}
#endif

/**
 * Probe SPC files
 */
static void
fa_probe_spc(metadata_t *md, uint8_t *pb)
{
  char buf[33];
  buf[32] = 0;

  if(memcmp("v0.30", pb + 0x1c, 4))
    return;

  if(pb[0x23] != 0x1a)
    return;

  memcpy(buf, pb + 0x2e, 32);
  md->md_title = rstr_alloc(buf);

  memcpy(buf, pb + 0x4e, 32);
  md->md_album = rstr_alloc(buf);

  memcpy(buf, pb + 0xa9, 3);
  buf[3] = 0;

  md->md_duration = atoi(buf);
}


/**
 *
 */
static void 
fa_probe_psid(metadata_t *md, uint8_t *pb)
{
  md->md_title  = rstr_alloc(utf8_from_bytes((char *)pb + 0x16, 32, NULL));
  md->md_artist = rstr_alloc(utf8_from_bytes((char *)pb + 0x36, 32, NULL));
}


/**
 * 
 */
static void
metdata_set_redirect(metadata_t *md, const char *fmt, ...)
{
  char buf[URL_MAX];
  va_list ap;
  va_start(ap, fmt);

  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  md->md_redirect = strdup(buf);
}


/**
 *
 */
static int
jpeginfo_reader(void *handle, void *buf, off_t offset, size_t size)
{
  AVIOContext *avio = handle;
  if(avio_seek(avio, offset, SEEK_SET) != offset)
    return -1;
  return avio_read(avio, buf, size);
}


static void
fa_probe_exif(metadata_t *md, const char *url, uint8_t *pb, AVIOContext *avio)
{
  jpeginfo_t ji;

  if(jpeg_info(&ji, jpeginfo_reader, avio, 
	       JPEG_INFO_DIMENSIONS | JPEG_INFO_ORIENTATION,
	       pb, 256, NULL, 0))
    return;
  
  md->md_time = ji.ji_time;
}


/**
 * Probe file by checking its header
 *
 * pb is guaranteed to point to at least 256 bytes of valid data
 */
static int
fa_probe_header(metadata_t *md, const char *url, AVIOContext *avio)
{
  uint16_t flags;
  uint8_t buf[256];

  if(avio_read(avio, buf, sizeof(buf)) != sizeof(buf))
    return 0;

  if(!memcmp(buf, "SNES-SPC700 Sound File Data", 27)) {
    fa_probe_spc(md, buf);
    md->md_contenttype = CONTENT_AUDIO;
    return 1;
  }

  if(!memcmp(buf, "PSID", 4) || !memcmp(buf, "RSID", 4)) {
    fa_probe_psid(md, buf); 
    md->md_contenttype = CONTENT_ALBUM;
    metdata_set_redirect(md, "sidfile://%s/", url);
    return 1;
  }

  if(buf[0] == 'R'  && buf[1] == 'a'  && buf[2] == 'r' && buf[3] == '!' &&
     buf[4] == 0x1a && buf[5] == 0x07 && buf[6] == 0x0 && buf[9] == 0x73) {

    flags = buf[10] | buf[11] << 8;
    if((flags & 0x101) == 1) {
      /* Don't include slave volumes */
      md->md_contenttype = CONTENT_UNKNOWN;
      return 1;
    }

    metdata_set_redirect(md, "rar://%s", url);
    md->md_contenttype = CONTENT_ARCHIVE;
    return 1;
  }

  if(buf[0] == 0x50 && buf[1] == 0x4b && buf[2] == 0x03 && buf[3] == 0x04) {

    char path[256];
    char *buf;
    struct fa_stat fs;

    snprintf(path, sizeof(path), "zip://%s/plugin.json", url);
    buf = fa_quickload(path, &fs, NULL, NULL, 0);
    if(buf != NULL) {
      htsmsg_t *json = htsmsg_json_deserialize(buf);
      free(buf);

      const char *title = htsmsg_get_str(json, "title");
      if(title != NULL && htsmsg_get_str(json, "id") != NULL &&
	 htsmsg_get_str(json, "type") != NULL) {
	md->md_title = rstr_alloc(title);
	md->md_contenttype = CONTENT_PLUGIN;
	htsmsg_destroy(json);
	return 1;
      }
      htsmsg_destroy(json);
    }
    metdata_set_redirect(md, "zip://%s", url);
    md->md_contenttype = CONTENT_ARCHIVE;
    return 1;
  }

#if 0
  if(!strncasecmp((char *)buf, "[playlist]", 10)) {
    /* Playlist */
    fa_probe_playlist(md, url, buf, sizeof(buf));
    md->md_contenttype = CONTENT_PLAYLIST;
    return 1;
  }
#endif

  if((buf[6] == 'J' && buf[7] == 'F' && buf[8] == 'I' && buf[9] == 'F') ||
     (buf[6] == 'E' && buf[7] == 'x' && buf[8] == 'i' && buf[9] == 'f')) {
    /* JPEG image */
    md->md_contenttype = CONTENT_IMAGE;
    fa_probe_exif(md, url, buf, avio); // Try to get more info
    return 1;
  }

  if(!memcmp(buf, "<showtimeplaylist", strlen("<showtimeplaylist"))) {
    /* Ugly playlist thing (see fa_video.c) */
    md->md_contenttype = CONTENT_VIDEO;
    return 1;
  }

  if(!memcmp(buf, pngsig, 8)) {
    /* PNG */
    md->md_contenttype = CONTENT_IMAGE;
    return 1;
  }

  if(!memcmp(buf, gifsig, sizeof(gifsig))) {
    /* GIF */
    md->md_contenttype = CONTENT_IMAGE;
    return 1;
  }

  if(buf[0] == '%' && buf[1] == 'P' && buf[2] == 'D' && buf[3] == 'F') {
    md->md_contenttype = CONTENT_UNKNOWN;
    return 1;
  }

  return 0;
}

/**
 * Check if file is an iso image
 * pb is guaranteed to point at 128 byts
 * of data starting 0x8000 of start of file
 */
static int
fa_probe_iso0(metadata_t *md, uint8_t *pb)
{
  uint8_t *p;

  if(memcmp(pb, isosig, 8))
    return -1;

  p = &pb[40];
  while(*p > 32 && p != &pb[72])
    p++;

  *p = 0;

  if(md != NULL) {
    md->md_title = rstr_alloc((const char *)pb + 40);
    md->md_contenttype = CONTENT_DVD;
  }
  return 0;
}



/**
 * Check if file is an iso image
 * pb is guaranteed to point at 64k of data
 */
int
fa_probe_iso(metadata_t *md, AVIOContext *avio)
{
  uint8_t pb[128];

  if(avio_seek(avio, 0x8000, SEEK_SET) != 0x8000)
    return -1;

  if(avio_read(avio, pb, sizeof(pb)) != sizeof(pb))
    return -1;
  return fa_probe_iso0(md, pb);
}


#if ENABLE_LIBGME
/**
 *
 */
static int
gme_probe(metadata_t *md, const char *url, AVIOContext *avio)
{
  uint8_t b4[4], *buf;
  gme_err_t err;
  Music_Emu *emu;
  gme_info_t *info;
  int tracks;
  size_t size;
  const char *type;

  if(avio_read(avio, b4, 4) != 4)
    return 0;

  type = gme_identify_header(b4);

  if(*type == 0)
    return 0;

  size = avio_size(avio);
  if(size == -1)
    return -1;

  buf = malloc(size);

  avio_seek(avio, 0, SEEK_SET);

  if(avio_read(avio, buf, size) != size) {
    free(buf);
    return 0;
  }


  err = gme_open_data(buf, size, &emu, gme_info_only);
  free(buf);
  if(err != NULL)
    return 0;

  err = gme_track_info(emu, &info, 0);
  if(err != NULL) {
    gme_delete(emu);
    return 0;
  }

  tracks = gme_track_count(emu);

#if 0
  printf("tracks   : %d\n", tracks);
  printf("system   : %s\n", info->system);
  printf("game     : %s\n", info->game);
  printf("song     : %s\n", info->song);
  printf("author   : %s\n", info->author);
  printf("copyright: %s\n", info->copyright);
  printf("comment  : %s\n", info->comment);
  printf("dumper   : %s\n", info->dumper);
#endif

  if(tracks == 1) {

    md->md_title  = info->song[0]   ? rstr_alloc(info->song)   : NULL;
    md->md_album  = info->game[0]   ? rstr_alloc(info->game)   : NULL;
    md->md_artist = info->author[0] ? rstr_alloc(info->author) : NULL;

    md->md_duration = info->play_length / 1000.0;
    md->md_contenttype = CONTENT_AUDIO;

  } else {

    md->md_title  = info->game[0] ? rstr_alloc(info->game)   : NULL;
    md->md_artist = info->author[0] ? rstr_alloc(info->author) : NULL;

    md->md_contenttype = CONTENT_ALBUM;
    metdata_set_redirect(md, "gmefile://%s/", url);
  }

  gme_free_info(info);
  gme_delete(emu);
  return 1;
}
#endif


/**
 *
 */
static void
fa_lavf_load_meta(metadata_t *md, AVFormatContext *fctx, const char *url)
{
  int i;
  char tmp1[1024];
  int has_video = 0;
  int has_audio = 0;

  /* Format meta info */

  md->md_title = ffmpeg_metadata_get(fctx->metadata, "title");
  if(md->md_title == NULL) {
    fa_url_get_last_component(tmp1, sizeof(tmp1), url);

    // Strip .xxx ending in filenames
    i = strlen(tmp1);
    if(i > 4 && tmp1[i - 4] == '.')
      tmp1[i - 4] = 0;

    url_deescape(tmp1);
    md->md_title = rstr_alloc(tmp1);
  }

  md->md_artist = ffmpeg_metadata_get(fctx->metadata, "artist") ?:
    ffmpeg_metadata_get(fctx->metadata, "author");

  md->md_album = ffmpeg_metadata_get(fctx->metadata, "album");

  md->md_format = rstr_alloc(fctx->iformat->long_name);

  if(fctx->duration != AV_NOPTS_VALUE)
    md->md_duration = (float)fctx->duration / 1000000;


  if(fctx->nb_streams == 1 && 
     fctx->streams[0]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
    md->md_contenttype = CONTENT_AUDIO;
    return;
  }

  /* Check each stream */

  for(i = 0; i < fctx->nb_streams; i++) {
    AVStream *stream = fctx->streams[i];
    AVCodecContext *avctx = stream->codec;
    AVCodec *codec = avcodec_find_decoder(avctx->codec_id);
    AVMetadataTag *tag;

    switch(avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
      has_video = !!codec;
      break;
    case AVMEDIA_TYPE_AUDIO:
      has_audio = !!codec;
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      break;

    default:
      continue;
    }

    if(codec == NULL) {
      snprintf(tmp1, sizeof(tmp1), "%s", codecname(avctx->codec_id));
    } else {
      metadata_from_ffmpeg(tmp1, sizeof(tmp1), codec, avctx);
    }

    tag = av_metadata_get(stream->metadata, "language", NULL,
			  AV_METADATA_IGNORE_SUFFIX);

    metadata_add_stream(md, codecname(avctx->codec_id),
			avctx->codec_type, i, tmp1,
			tag ? tag->value : NULL,
			stream->disposition);
  }
  
  md->md_contenttype = CONTENT_FILE;
  if(has_video)
    md->md_contenttype = CONTENT_VIDEO;
  else if(has_audio)
    md->md_contenttype = CONTENT_AUDIO;
}
  

/**
 *
 */
metadata_t *
fa_probe_metadata(const char *url, char *errbuf, size_t errsize)
{
  AVFormatContext *fctx;
  AVIOContext *avio;

  if((avio = fa_libav_open(url, 32768, errbuf, errsize, 0, NULL)) == NULL)
    return NULL;

  metadata_t *md = metadata_create();

#if ENABLE_LIBGME
  if(gme_probe(md, url, avio))
    return md;
#endif

  avio_seek(avio, 0, SEEK_SET);

  if(fa_probe_header(md, url, avio)) {
    fa_libav_close(avio);
    return md;
  }

  if((fctx = fa_libav_open_format(avio, url, errbuf, errsize, NULL)) == NULL) {
    fa_libav_close(avio);
    metadata_destroy(md);
    return NULL;
  }

  fa_lavf_load_meta(md, fctx, url);
  fa_libav_close_format(fctx);
  return md;
}


/**
 *
 */
metadata_t *
fa_metadata_from_fctx(AVFormatContext *fctx, const char *url)
{
  metadata_t *md = metadata_create();
  fa_lavf_load_meta(md, fctx, url);
  return md;
}


/**
 * Probe a directory
 */
metadata_t *
fa_probe_dir(const char *url)
{
  metadata_t *md = metadata_create();
  char path[URL_MAX];
  struct fa_stat fs;

  md->md_contenttype = CONTENT_DIR;

  fa_pathjoin(path, sizeof(path), url, "VIDEO_TS");
  if(fa_stat(path, &fs, NULL, 0) == 0 && fs.fs_type == CONTENT_DIR) {
    md->md_contenttype = CONTENT_DVD;
    return md;
  }

  fa_pathjoin(path, sizeof(path), url, "video_ts");
  if(fa_stat(path, &fs, NULL, 0) == 0 && fs.fs_type == CONTENT_DIR) {
    md->md_contenttype = CONTENT_DVD;
    return md;
  }
  
  return md;
}
