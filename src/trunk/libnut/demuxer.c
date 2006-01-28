#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include "nut.h"
#include "priv.h"

static size_t stream_read(void * priv, size_t len, uint8_t * buf) {
	return fread(buf, 1, len, priv);
}

static off_t stream_seek(void * priv, long long pos, int whence) {
	fseek(priv, pos, whence);
	return ftello(priv);
}

static void flush_buf(input_buffer_t *bc) {
	assert(!bc->is_mem);
	bc->file_pos += bc->buf_ptr - bc->buf;
	bc->read_len -= bc->buf_ptr - bc->buf;
	memmove(bc->buf, bc->buf_ptr, bc->read_len);
	bc->buf_ptr = bc->buf;
}

static int ready_read_buf(input_buffer_t * bc, int amount) {
	int pos = (bc->buf_ptr - bc->buf);
	if (bc->read_len - pos < amount && !bc->is_mem) {
		amount += 10; // ### + PREALLOC_SIZE ?
		if (bc->write_len - pos < amount) {
			bc->write_len = amount + pos + PREALLOC_SIZE;
			bc->buf = realloc(bc->buf, bc->write_len);
			bc->buf_ptr = bc->buf + pos;
		}
		bc->read_len += bc->isc.read(bc->isc.priv, amount - (bc->read_len - pos), bc->buf + bc->read_len);
	}
	return bc->read_len - (bc->buf_ptr - bc->buf);
}

static void seek_buf(input_buffer_t * bc, long long pos, int whence) {
	assert(!bc->is_mem);
	if (whence != SEEK_END) {
		// don't do anything when already in seeked position. but still flush_buf
		off_t req = pos + (whence == SEEK_CUR ? bctello(bc) : 0);
		if (req >= bc->file_pos && req < bc->file_pos + bc->read_len) {
			bc->buf_ptr = bc->buf + (req - bc->file_pos);
			flush_buf(bc);
			return;
		}
	}
	if (whence == SEEK_CUR) pos -= bc->read_len - (bc->buf_ptr - bc->buf);
	fprintf(stderr, "seeking %d ", (int)pos);
	switch (whence) {
		case SEEK_SET: fprintf(stderr, "SEEK_SET   "); break;
		case SEEK_CUR: fprintf(stderr, "SEEK_CUR   "); break;
		case SEEK_END: fprintf(stderr, "SEEK_END   "); break;
	}
	bc->file_pos = bc->isc.seek(bc->isc.priv, pos, whence);
	bc->buf_ptr = bc->buf;
	bc->read_len = 0;
	if (whence == SEEK_END) bc->filesize = bc->file_pos - pos;
}

static int buf_eof(input_buffer_t * bc) {
	if (bc->is_mem) return -ERR_BAD_EOF;
	if (!bc->isc.eof || bc->isc.eof(bc->isc.priv)) return 1;
	return 2;
}

static input_buffer_t * new_mem_buffer() {
	input_buffer_t * bc = malloc(sizeof(input_buffer_t));
	bc->read_len = 0;
	bc->write_len = 0;
	bc->is_mem = 1;
	bc->file_pos = 0;
	bc->filesize = 0;
	bc->buf_ptr = bc->buf = NULL;
	return bc;
}

static input_buffer_t * new_input_buffer(nut_input_stream_t isc) {
	input_buffer_t * bc = new_mem_buffer();
	bc->is_mem = 0;
	bc->isc = isc;
	bc->file_pos = isc.file_pos;
	if (!bc->isc.read) {
		bc->isc.read = stream_read;
		bc->isc.seek = stream_seek;
		bc->isc.eof = NULL;
	}
	return bc;
}

static void free_buffer(input_buffer_t * bc) {
	if (!bc) return;
	free(bc->buf);
	free(bc);
}

static int get_bytes(input_buffer_t * bc, int count, uint64_t * val) {
	int i;
	if (ready_read_buf(bc, count) < count) return buf_eof(bc);
	*val = 0;
	for(i = 0; i < count; i++){
		*val = (*val << 8) | *(bc->buf_ptr++);
	}
	return 0;
}

static int get_v(input_buffer_t * bc, uint64_t * val) {
	int i, len;
	*val = 0;
	while ((len = ready_read_buf(bc, 16))) {
		for(i = 0; i < len; i++){
			uint8_t tmp= *(bc->buf_ptr++);
			*val = (*val << 7) | (tmp & 0x7F);
			if (!(tmp & 0x80)) return 0;
		}
	}
	return buf_eof(bc);
}

static int get_s(input_buffer_t * bc, int64_t * val) {
	uint64_t tmp;
	int err;
	if ((err = get_v(bc, &tmp))) return err;
	tmp++;
	if (tmp & 1) *val = -(tmp >> 1);
	else         *val =  (tmp >> 1);
	return 0;
}

#ifdef TRACE
static int get_v_trace(input_buffer_t * bc, uint64_t * val, char * var, char * file, int line, char * func) {
	int a = get_v(bc, val);
	printf("GET_V %llu to var `%s' at %s:%d, %s() (ret: %d)\n", *val, var, file, line, func, a);
	return a;
}

static int get_s_trace(input_buffer_t * bc, int64_t * val, char * var, char * file, int line, char * func) {
	int a = get_s(bc, val);
	printf("GET_S %lld to var `%s' at %s:%d, %s() (ret: %d)\n", *val, var, file, line, func, a);
	return a;
}

#define get_v_(bc, var, name) get_v_trace(bc, var, name, __FILE__, __LINE__, __PRETTY_FUNCTION__)
#define get_s_(bc, var, name) get_s_trace(bc, var, name, __FILE__, __LINE__, __PRETTY_FUNCTION__)
#define get_v(bc, var) get_v_(bc, var, #var)
#define get_s(bc, var) get_s_(bc, var, #var)
#else
#define get_v_(bc, var, name) get_v(bc, var)
#define get_s_(bc, var, name) get_s(bc, var)
#endif

#define CHECK(expr) do { int _a; if ((_a = (expr))) { err = _a; goto err_out; } } while(0)
#define ERROR(expr, code) do { if (expr) { err = code; goto err_out; } } while(0)
#define GET_V(bc, v) do { uint64_t _tmp; CHECK(get_v_((bc), &_tmp, #v)); (v) = _tmp; } while(0)
#define GET_S(bc, v) do {  int64_t _tmp; CHECK(get_s_((bc), &_tmp, #v)); (v) = _tmp; } while(0)

static int get_data(input_buffer_t * bc, int len, uint8_t * buf) {
	int tmp;

	if (!len) return 0;
	assert(buf);

	tmp = ready_read_buf(bc, len);

	len = MIN(len, tmp);
	memcpy(buf, bc->buf_ptr, len);
	bc->buf_ptr += len;

	return len;
}

static int get_vb(input_buffer_t * in, int * len, uint8_t ** buf) {
	uint64_t tmp;
	int err;
	if ((err = get_v(in, &tmp))) return err;
	*len = tmp;
	*buf = realloc(*buf, tmp);
	if (get_data(in, tmp, *buf) != tmp) return buf_eof(in);
	return 0;
}

static int get_frame(nut_context_t * nut, nut_packet_t * fd, int n) {
	int err = 0, coded_pts, size_lsb = 0, stream_flags = 0, scrap, i;
	stream_context_t * sc;

	if (!nut->ft[n].stream_plus1) GET_V(nut->i, fd->stream);
	else fd->stream = nut->ft[n].stream_plus1 - 1;

	sc = &nut->sc[fd->stream];

	if (!nut->ft[n].pts_delta) {
		GET_V(nut->i, coded_pts);
		if (coded_pts >= (1 << sc->msb_pts_shift))
			fd->pts = coded_pts - (1 << sc->msb_pts_shift);
		else {
			int mask, delta;
			mask = (1 << sc->msb_pts_shift)-1;
			delta = sc->last_pts - mask/2;
			fd->pts = ((coded_pts - delta) & mask) + delta;
		}
	} else {
		fd->pts = sc->last_pts + nut->ft[n].pts_delta;
	}

	if (nut->ft[n].flags & MSB_CODED_FLAG) GET_V(nut->i, size_lsb);
	fd->len = size_lsb * nut->ft[n].mul + nut->ft[n].lsb;

	if (nut->ft[n].flags & STREAM_CODED_FLAG) GET_V(nut->i, stream_flags);
	fd->flags = nut->ft[n].stream_flags ^ stream_flags;

	for (i = 0; i < nut->ft[n].reserved; i++) GET_V(nut->i, scrap);

	sc->last_pts = fd->pts;
	sc->last_dts = get_dts(sc->decode_delay, sc->pts_cache, fd->pts);
	if (fd->flags & NUT_KEY_STREAM_FLAG && !sc->last_key) sc->last_key = fd->pts + 1;
err_out:
	return err;
}

static int get_header(input_buffer_t * in, input_buffer_t * out, int len, int checksum) {
	uint64_t code;

	assert(out->is_mem);
	assert(out->buf == out->buf_ptr);

	if (checksum) len -= 4;

	out->write_len = len;
	free(out->buf);
	out->buf_ptr = out->buf = malloc(out->write_len);
	if (get_data(in, len, out->buf) != len) return buf_eof(in);
	out->read_len = len;

	if (checksum) {
		if (get_bytes(in, 4, &code)) return buf_eof(in); // checksum
		if (code != adler32(out->buf, len)) return -ERR_BAD_CHECKSUM;
	}
	return 0;
}

static int get_main_header(nut_context_t *nut, int len) {
	input_buffer_t * tmp = new_mem_buffer();
	int i, j, err = 0;
	int flag, fields, timestamp = 0, mul = 1, stream = 0, sflag, size, count, reserved;

	CHECK(get_header(nut->i, tmp, len, 1));

	GET_V(tmp, i);
	ERROR(i != NUT_VERSION, -ERR_BAD_VERSION);
	GET_V(tmp, nut->stream_count);
	GET_V(tmp, nut->max_distance);

	for(i = 0; i < 256; ) {
		int scrap;
		GET_V(tmp, flag);
		GET_V(tmp, fields);
		if (fields > 0) GET_V(tmp, sflag);
		else sflag = 0;
		if (fields > 1) GET_S(tmp, timestamp);
		if (fields > 2) GET_V(tmp, mul);
		if (fields > 3) GET_V(tmp, stream);
		if (fields > 4) GET_V(tmp, size);
		else size = 0;
		if (fields > 5) GET_V(tmp, reserved);
		else reserved = 0;
		if (fields > 6) GET_V(tmp, count);
		else count = mul - size;

		for (j = 7; j < fields; j++) {
			GET_V(tmp, scrap);
		}

		for(j = 0; j < count && i < 256; j++, i++) {
			assert(i != 'N' || flag == INVALID_FLAG);
			nut->ft[i].flags = flag;
			nut->ft[i].stream_flags = sflag;
			nut->ft[i].stream_plus1 = stream;
			nut->ft[i].mul = mul;
			nut->ft[i].lsb = size + j;
			nut->ft[i].pts_delta = timestamp;
			nut->ft[i].reserved = reserved;
		}
	}
err_out:
	free_buffer(tmp);
	return err;
}

static int get_stream_header(nut_context_t * nut, int id) {
	input_buffer_t * tmp = new_mem_buffer();
	stream_context_t * sc = &nut->sc[id];
	int i, err = 0, len;
	uint64_t a;

	GET_V(nut->i, len);
	CHECK(get_header(nut->i, tmp, len, 1));

	GET_V(tmp, i);
	ERROR(i != id, -ERR_BAD_STREAM_ORDER);

	GET_V(tmp, sc->sh.type);
	CHECK(get_vb(tmp, &sc->sh.fourcc_len, &sc->sh.fourcc));
	GET_V(tmp, sc->sh.timebase.nom);
	GET_V(tmp, sc->sh.timebase.den);
	GET_V(tmp, sc->msb_pts_shift);
	GET_V(tmp, sc->decode_delay);
	CHECK(get_bytes(tmp, 1, &a));
	sc->sh.fixed_fps = a & 1;
	CHECK(get_vb(tmp, &sc->sh.codec_specific_len, &sc->sh.codec_specific));

	switch (sc->sh.type) {
		case NUT_VIDEO_CLASS:
			GET_V(tmp, sc->sh.width);
			GET_V(tmp, sc->sh.height);
			GET_V(tmp, sc->sh.sample_width);
			GET_V(tmp, sc->sh.sample_height);
			GET_V(tmp, sc->sh.colorspace_type); // TODO understand this
			break;
		case NUT_AUDIO_CLASS:
			GET_V(tmp, sc->sh.samplerate_nom);
			GET_V(tmp, sc->sh.samplerate_denom);
			GET_V(tmp, sc->sh.channel_count); // ### is channel count staying in spec
			break;
	}
err_out:
	free_buffer(tmp);
	return err;
}

static int add_syncpoint(nut_context_t * nut, syncpoint_t sp, uint64_t * pts) {
	syncpoint_list_t * sl = &nut->syncpoints;
	int i, j;

	for (i = sl->len; i--; ) { // more often than not, we're adding at end of list
		off_t pos = sl->s[i].pos >> 1;
		if (pos > sp.pos) continue;
		if (sp.pos < pos + 8) { // syncpoint already in list
			sl->s[i].pos = (sp.pos << 1) | (sl->s[i].pos & 1);
			if (pts) {
				sl->s[i].pos |= 1;
				for (j = 0; j < nut->stream_count; j++) {
					assert(!sl->pts[i * nut->stream_count + j] || sl->pts[i * nut->stream_count + j] == pts[j]);
					sl->pts[i * nut->stream_count + j] = pts[j];
				}
			}
			return i;
		}
		break;
	}
	i++;
	if (sl->len + 1 > sl->alloc_len) {
		sl->alloc_len += PREALLOC_SIZE/4;
		sl->s = realloc(sl->s, sl->alloc_len * sizeof(syncpoint_t));
		sl->pts = realloc(sl->pts, sl->alloc_len * nut->stream_count * sizeof(uint64_t));
	}
	memmove(sl->s + i + 1, sl->s + i, (sl->len - i) * sizeof(syncpoint_t));
	memmove(sl->pts + (i + 1) * nut->stream_count, sl->pts + i * nut->stream_count, (sl->len - i) * nut->stream_count * sizeof(uint64_t));

	sl->s[i] = sp;
	sl->s[i].pos <<= 1; // flag
	if (pts) sl->s[i].pos |= 1;
	for (j = 0; j < nut->stream_count; j++) sl->pts[i * nut->stream_count + j] = pts ? pts[j] : 0;
	sl->len++;
	return i;
}

static void set_global_pts(nut_context_t * nut, uint64_t pts) {
	int i;
	nut_timebase_t tb = nut->sc[pts % nut->stream_count].sh.timebase;
	pts = pts / nut->stream_count;

	for (i = 0; i < nut->stream_count; i++) {
		nut->sc[i].last_pts = convert_ts(pts, tb, nut->sc[i].sh.timebase);
	}
}

static int get_syncpoint(nut_context_t * nut) {
	int err = 0;
	syncpoint_t s;
	int after_seek = nut->last_syncpoint ? 0 : 1;

	nut->last_syncpoint = s.pos = bctello(nut->i) - 8;

	GET_V(nut->i, s.pts);
	GET_V(nut->i, s.back_ptr);
	s.back_ptr = (s.back_ptr * 8 + 7)<<1;

	set_global_pts(nut, s.pts);

	if (/*nut->dopts.cache_syncpoints*/1) {
		int i;
		uint64_t pts[nut->stream_count];
		for (i = 0; i < nut->stream_count; i++) {
			pts[i] = nut->sc[i].last_key;
			nut->sc[i].last_key = 0;
		}
		i = add_syncpoint(nut, s, after_seek ? NULL : pts);
		if (!after_seek) nut->syncpoints.s[i - 1].back_ptr |= 1;
	} /*else {
		if (!nut->syncpoints.len) add_syncpoint(nut, s);
	}*/
err_out:
	return err;
}

static int get_index(nut_context_t * nut) {
	input_buffer_t * tmp = new_mem_buffer();
	int err = 0;
	syncpoint_list_t * sl = &nut->syncpoints;
	uint64_t * p;
	uint64_t x;
	int i;

	CHECK(get_bytes(nut->i, 8, &x));
	ERROR(x != INDEX_STARTCODE, -ERR_GENERAL_ERROR);

	GET_V(nut->i, x);
	CHECK(get_header(nut->i, tmp, x, 1));

	GET_V(tmp, x);
	for (i = 0; i < nut->stream_count; i++) {
		TO_PTS(max, x)
		nut->sc[i].sh.max_pts = convert_ts(max_p, nut->sc[max_s].sh.timebase, nut->sc[i].sh.timebase);
	}

	GET_V(tmp, x);
	sl->alloc_len = sl->len = x;
	sl->s = realloc(sl->s, sl->alloc_len * sizeof(syncpoint_t));
	p = sl->pts = realloc(sl->pts, sl->alloc_len * sizeof(uint64_t) * nut->stream_count);

	for (i = 0; i < sl->len; i++) {
		GET_V(tmp, x);
		x *= 8;
		sl->s[i].pos = (x << 1) + (i ? sl->s[i-1].pos : 1);
		sl->s[i].back_ptr = 1;
		sl->s[i].pts = 0;
	}
	for (i = 0; i < nut->stream_count; i++) {
		int j;
		uint64_t last_pts = 0;
		for (j = 0; j < sl->len; ) {
			int type, n, flag;
			GET_V(tmp, x);
			type = x & 1;
			x >>= 1;
			n = j;
			if (type) {
				flag = x & 1;
				x >>= 1;
				while (x--) p[n++ * nut->stream_count + i] = flag;
				if (n < sl->len-1) p[n++ * nut->stream_count + i] = !flag;
			} else {
				while (x != 1) {
					p[n++ * nut->stream_count + i] = x & 1;
					x >>= 1;
				}
			}
			for(; j < n && j < sl->len; j++) {
				if (!p[j * nut->stream_count + i]) continue;
				GET_V(tmp, x);
				last_pts += x;
				p[j * nut->stream_count + i] = last_pts + 1;
			}
		}
	}

	fprintf(stderr, "NUT index read successfully, %d syncpoints\n", sl->len);

err_out:
	free_buffer(tmp);
	return err;
}

int nut_read_next_packet(nut_context_t * nut, nut_packet_t * pd) {
	int err = 0;
	uint64_t n, tmp;
	if (!nut->last_headers) {
		const int len = strlen(ID_STRING) + 1;
		char str[len];
		ERROR(get_data(nut->i, len, str) != len, buf_eof(nut->i));
		if (memcmp(str, ID_STRING, len)) nut->i->buf_ptr = nut->i->buf; // rewind
		CHECK(get_bytes(nut->i, 8, &tmp));
		do {
			if (tmp == MAIN_STARTCODE) {
				GET_V(nut->i, pd->len);
				pd->type = e_headers;
				return 0;
			}
			ERROR(ready_read_buf(nut->i, 1) < 1, buf_eof(nut->i));
			tmp = (tmp << 8) | *(nut->i->buf_ptr++);
		} while (bctello(nut->i) < 4096);
	}
	ERROR(!nut->last_headers, -ERR_NO_HEADERS);
	CHECK(get_bytes(nut->i, 1, &n)); // frame_code or 'N'
	if (n != 'N' && !(nut->ft[n].flags & INVALID_FLAG)) { // frame
		pd->type = e_frame;
		CHECK(get_frame(nut, pd, n));
		goto err_out;
	}

	ERROR(n != 'N', -ERR_NOT_FRAME_NOT_N); // ###
	CHECK(get_bytes(nut->i, 7, &tmp));
	switch ((n << 56) | tmp) {
		case SYNCPOINT_STARTCODE:
			CHECK(get_syncpoint(nut));
			flush_buf(nut->i);
			return nut_read_next_packet(nut, pd);
		case INDEX_STARTCODE:
			err = 1; // EOF
			goto err_out;
		case MAIN_STARTCODE:
			GET_V(nut->i, pd->len);
			pd->type = e_unknown;
			break;
		case STREAM_STARTCODE:
			GET_V(nut->i, pd->len);
			pd->type = e_unknown;
			break;
		case INFO_STARTCODE: // FIXME
			GET_V(nut->i, pd->len);
			pd->type = e_unknown;
			break;
		default:
			GET_V(nut->i, pd->len);
			pd->type = e_unknown;
			break;
	}
err_out:
	if (err != 2) flush_buf(nut->i); // unless EAGAIN
	else nut->i->buf_ptr = nut->i->buf; // rewind
	return err;
}

int nut_read_headers(nut_context_t * nut, nut_packet_t * pd, nut_stream_header_t * s []) {
	int i, err = 0;
	off_t headers_pos = bctello(nut->i);
	*s = NULL;
	if (!nut->last_headers) { // we already have headers, we were called just for index
		CHECK(get_main_header(nut, pd->len));

		if (!nut->sc) {
			nut->sc = malloc(sizeof(stream_context_t) * nut->stream_count);
			for (i = 0; i < nut->stream_count; i++) {
				nut->sc[i].last_pts = 0;
				nut->sc[i].last_dts = 0;
				nut->sc[i].last_key = 0;
				nut->sc[i].sh.max_pts = 0;
				nut->sc[i].sh.fourcc = NULL;
				nut->sc[i].sh.codec_specific = NULL;
				nut->sc[i].pts_cache = NULL;
			}
		}

		for (i = 0; i < nut->stream_count; i++) {
			uint64_t tmp;
			int j;
			CHECK(get_bytes(nut->i, 8, &tmp));
			if (tmp != STREAM_STARTCODE) return -ERR_NOSTREAM_STARTCODE;
			CHECK(get_stream_header(nut, i));
			if (!nut->sc[i].pts_cache) {
				nut->sc[i].pts_cache = malloc(nut->sc[i].decode_delay * sizeof(int64_t));
				for (j = 0; j < nut->sc[i].decode_delay; j++)
					nut->sc[i].pts_cache[j] = -1;
			}
		}
		nut->last_headers = headers_pos;
	}

	if (nut->dopts.read_index && nut->i->isc.seek) {
		uint64_t idx_ptr;
		if (nut->seek_status <= 1) {
			if (nut->seek_status == 0) {
				nut->before_seek = bctello(nut->i);
				seek_buf(nut->i, -8, SEEK_END);
			}
			nut->seek_status = 1;
			CHECK(get_bytes(nut->i, 8, &idx_ptr));
			if (idx_ptr) idx_ptr = nut->i->filesize - 8 - idx_ptr;
			if (!idx_ptr || idx_ptr >= nut->i->filesize) nut->dopts.read_index = 0; // invalid ptr
		}
		if (nut->dopts.read_index) {
			if (nut->seek_status == 1) seek_buf(nut->i, idx_ptr, SEEK_SET);
			nut->seek_status = 2;
			// only EAGAIN from get_index is interesting
			if ((err = get_index(nut)) == 2) goto err_out;
			err = 0;
		}
		nut->seek_status = 0;
		seek_buf(nut->i, nut->before_seek, SEEK_SET);
		nut->before_seek = 0;
	}
	*s = malloc(sizeof(nut_stream_header_t) * (nut->stream_count + 1));
	for (i = 0; i < nut->stream_count; i++) (*s)[i] = nut->sc[i].sh;
	(*s)[i].type = -1;
err_out:
	if (err && err != 2 && !nut->last_headers) {
		if (nut->sc) for (i = 0; i < nut->stream_count; i++) {
			free(nut->sc[i].sh.fourcc);
			free(nut->sc[i].sh.codec_specific);
			free(nut->sc[i].pts_cache);
		}
		free(nut->sc);
		nut->sc = NULL;
		nut->stream_count = 0;
	}
	if (err != 2) flush_buf(nut->i); // unless EAGAIN
	else nut->i->buf_ptr = nut->i->buf; // rewind
	return err;
}

int nut_read_frame(nut_context_t * nut, int * len, uint8_t * buf) {
	int tmp = MIN(*len, nut->i->read_len - (nut->i->buf_ptr - nut->i->buf));
	if (tmp) {
		memcpy(buf, nut->i->buf_ptr, tmp);
		nut->i->buf_ptr += tmp;
		*len -= tmp;
	}
	if (*len) {
		int read = nut->i->isc.read(nut->i->isc.priv, *len, buf + tmp);
		nut->i->file_pos += read;
		*len -= read;
	}

	flush_buf(nut->i); // ALWAYS flush...

	if (*len) return buf_eof(nut->i);
	return 0;
}

int nut_skip_packet(nut_context_t * nut, int * len) {
	// FIXME or fseek
#ifdef __TCC__
	uint8_t * tmp = malloc(*len);
	int a = nut_read_frame(nut, len, tmp);
	free(tmp);
	return a;
#else
	uint8_t tmp[*len];
	return nut_read_frame(nut, len, tmp);
#endif
}

int nut_read_info(nut_context_t * nut, nut_info_packet_t * info []) {
#if 0
	BufferContext *tmp = new_mem_buffer();

	for(;;){
		char * type;
		int id = info[0].id;
		put_v(tmp, id);
		if (!id) break;
		type = info_table[id].type;
		if (!type) {
			type = info[0].type.data;
			put_vb(tmp, info[0].type.len, info[0].type.data);
		}
		if (!info_table[id].name)
			put_vb(tmp, info[0].name.len, info[0].name.data);
		if (!strcmp(type, "v"))
			put_v(tmp, info[0].val.i);
		else
			put_vb(tmp, info[0].val.s.len, info[0].val.s.data);
		id++;
	}

	put_header(nut->i, tmp, INFO_STARTCODE, 1);
	free_buffer(tmp);
#endif
	return 0;
}

void nut_free_info(nut_info_packet_t info []) {
	// FIXME ...
}

static int find_syncpoint(nut_context_t * nut, int backwards, syncpoint_t * res, off_t stop) {
	int read;
	int err = 0;
	uint64_t tmp;
	uint8_t * ptr = NULL;
	assert(!backwards || !stop); // can't have both

	if (backwards) seek_buf(nut->i, -nut->max_distance, SEEK_CUR);
retry:
	read = nut->max_distance;
	if (stop) read = MIN(read, stop - bctello(nut->i));
	read = ready_read_buf(nut->i, read);
	if (stop) read = MIN(read, stop - bctello(nut->i));
	tmp = 0;

	while (nut->i->buf_ptr - nut->i->buf < read) {
		tmp = (tmp << 8) | *(nut->i->buf_ptr++);
		if (tmp != SYNCPOINT_STARTCODE) continue;
		if (res) {
			res->pos = bctello(nut->i) - 8;
			GET_V(nut->i, res->pts);
			GET_V(nut->i, tmp);
			res->back_ptr = (tmp * 8 + 7) << 1;
		}
		if (!backwards) return 0;
		else ptr = nut->i->buf_ptr;
	}

	if (ptr) {
		nut->i->buf_ptr = ptr;
		return 0;
	}

	if (stop && bctello(nut->i) >= stop) {
		if (res) res->back_ptr = 1;
		return 0;
	}

	if (read < nut->max_distance) return buf_eof(nut->i); // too little read

	if (backwards) {
		nut->i->buf_ptr = nut->i->buf;
		seek_buf(nut->i, -(nut->max_distance - 7), SEEK_CUR);
	} else {
		nut->i->buf_ptr -= 7; // repeat on last 7 bytes
		if (nut->i->buf_ptr < nut->i->buf) nut->i->buf_ptr = nut->i->buf;
		flush_buf(nut->i);
	}

	goto retry;
err_out:
	return err;
}

static int find_basic_syncpoints(nut_context_t * nut) {
	int i, err = 0;
	syncpoint_list_t * sl = &nut->syncpoints;
	syncpoint_t s;

	if (!sl->len) { // not even a single syncpoint, find first one.
		// the first syncpoint put in the cache is always always the BEGIN syncpoint
		if (!nut->seek_status) seek_buf(nut->i, 0, SEEK_SET);
		nut->seek_status = 1;
		CHECK(find_syncpoint(nut, 0, &s, 0));
		add_syncpoint(nut, s, NULL);
		nut->seek_status = 0;
	}

	// find last syncpoint if it's not already found
	if (!(sl->s[sl->len-1].back_ptr & 1)) {
		// searching bakwards from EOF
		if (!nut->seek_status) seek_buf(nut->i, 0, SEEK_END);
		nut->seek_status = 1;
		CHECK(find_syncpoint(nut, 1, &s, 0));
		i = add_syncpoint(nut, s, NULL);
		assert(i == sl->len-1);
		sl->s[i].back_ptr |= 1;
		nut->seek_status = 0;
	}
err_out:
	return err;
}

static void clear_dts_cache(nut_context_t * nut) {
	int i;
	for (i = 0; i < nut->stream_count; i++) {
		int j;
		for (j = 0; j < nut->sc[i].decode_delay; j++) nut->sc[i].pts_cache[j] = -1;
		nut->sc[i].last_dts = -1;
	}
}

static int binary_search_syncpoint(nut_context_t * nut, double time_pos, uint64_t * pts, off_t * start, off_t * end, syncpoint_t * stopper) {
	int i, err = 0;
	syncpoint_t s;
	off_t hi, lo;
	uint64_t hip, lop;
	syncpoint_list_t * sl = &nut->syncpoints;
	int a = 0;

	CHECK(find_basic_syncpoints(nut));
	// sl->len MUST be >=2, which is the first and last syncpoints in the file
	ERROR(sl->len < 2, -ERR_NOT_SEEKABLE);

	for (i = 0; i < sl->len; i++) {
		TO_PTS(tmp, sl->s[i].pts)
		if ((pts[tmp_s] >> 1) <= tmp_p) break;
	}

	if (i == sl->len) { // there isn't any syncpoint bigger than requested
		seek_buf(nut->i, 0, SEEK_END); // intentionally seeking to EOF
		err = 1; // EOF
		goto err_out;
	}
	if (i == 0) { // there isn't any syncpoint smaller than requested
		seek_buf(nut->i, (sl->s[0].pos >> 1), SEEK_SET); // seeking to first syncpoint
		clear_dts_cache(nut);
		nut->last_syncpoint = 0;
		err = -ERR_NOT_SEEKABLE;
		goto err_out;
	}

	i--;

	/*if (!nut->dopts.cache_syncpoints && sl->len == 4 && (i == 0 || i == 2)) {
		if (i == 2) sl->s[1] = sl->s[2];
		else sl->s[1].back_ptr &= ~1;
		sl->s[2] = sl->s[3];
		i >>= 1;
		sl->len = 3;
	}*/

	lo = sl->s[i].pos >> 1;
	lop = sl->s[i].pts;
	hi = sl->s[i+1].pos >> 1;
	hip = sl->s[i+1].pts;
	if (nut->seek_status) hi = nut->seek_status;

	while (!(sl->s[i].back_ptr & 1)) {
		// start binary search between sl->s[i].pos (lo) to sl->s[i+1].pos (hi) ...
		off_t guess;
		int res;
		double hi_pd = TO_DOUBLE_PTS(hip);
		double lo_pd = TO_DOUBLE_PTS(lop);
		a++;
		if (hi - lo < nut->max_distance) guess = lo + 8;
		else { // linear interpolation
#define INTERPOLATE_WEIGHT (7./8)
			double a = (double)(hi - lo) / (hi_pd - lo_pd);
			guess = lo + a * (time_pos - lo_pd);
			guess = guess * INTERPOLATE_WEIGHT + (lo+hi)/2 * (1 - INTERPOLATE_WEIGHT);
			if (hi - guess < nut->max_distance*2) guess = hi - nut->max_distance*2; //(lo + hi)/2;
		}
		if (guess < lo + 8) guess = lo + 8;
		fprintf(stderr, "\n%d [ (%d,%.3f) .. (%d,%.3f) .. (%d,%.3f) ] ", i, (int)lo, lo_pd, (int)guess, time_pos, (int)hi, hi_pd);
		if (!nut->seek_status) seek_buf(nut->i, guess, SEEK_SET);
		nut->seek_status = hi; // so we know where to continue off...
		CHECK(find_syncpoint(nut, 0, &s, hi));
		nut->seek_status = 0;

		if (s.back_ptr == 1 || s.pos >= hi) { // we got back to 'hi'
			// either we scanned everything from lo to hi, or we keep trying
			if (guess <= lo + 11) sl->s[i].back_ptr |= 1; // we are done!
			else hi = guess;
			continue;
		}

		res = s.pts / nut->stream_count > (pts[s.pts % nut->stream_count] >> 1);
		if (res) {
			hi = s.pos;
			hip = s.pts;
		} else {
			lo = s.pos;
			lop = s.pts;
		}
		if (1/*nut->dopts.cache_syncpoints || sl->len == 2*/) {
			int tmp = add_syncpoint(nut, s, NULL);
			if (!res) i = tmp;
		}/* else if (sl->len == 3) {
			if (s.pts > pts) {
				if (sl->s[1].pts > pts) sl->s[1] = s;
				else add_syncpoint(nut, s);
			} else {
				if (sl->s[1].pts <= pts) sl->s[1] = s;
				else i = add_syncpoint(nut, s);
			}
		} else {
			if (s.pts > pts) sl->s[2] = s;
			else sl->s[1] = s;
		}*/
	}

	fprintf(stderr, "\n[ (%d,%d) .. %d .. (%d,%d) ] => %d (%d seeks) %d\n",
		(int)lo, (int)lop, (int)pts[0], (int)hi, (int)hip, (int)(lo - (sl->s[i].back_ptr>>1)), a, sl->s[i].back_ptr);
	// at this point, s[i].pts < P < s[i+1].pts, and s[i].flag is set
	// meaning, there are no more syncpoints between s[i] to s[i+1]
	*start = (sl->s[i].pos >> 1) - (sl->s[i].back_ptr>>1);
	*end = sl->s[i+1].pos >> 1;
	*stopper = sl->s[i+1];
err_out:
	return err;
}

static int linear_search_seek(nut_context_t * nut, int backwards, uint64_t * pts, off_t start, off_t end, syncpoint_t * stopper) {
	syncpoint_list_t * sl = &nut->syncpoints;
	int i, err = 0;
	off_t good_key[nut->stream_count], min_pos = 0;
	uint64_t old_last_pts[nut->stream_count];
	uint8_t * buf_before = NULL;
	off_t stopper_syncpoint = 0;

	for (i = 0; i < nut->stream_count; i++) {
		good_key[i] = nut->seek_state ? nut->seek_state[i].good_key : 0;
		old_last_pts[i] = nut->seek_state ? nut->seek_state[i].old_last_pts : nut->sc[i].last_pts;
	}

	if (nut->seek_status <= 1) {
		syncpoint_t s;
		if (!nut->seek_status) seek_buf(nut->i, start, SEEK_SET);
		nut->seek_status = 1;
		// find closest syncpoint by linear search, SHOULD be one pointed by back_ptr...
		CHECK(find_syncpoint(nut, 0, &s, 0));
		if (1/*nut->dopts.cache_syncpoints*/) add_syncpoint(nut, s, NULL);
		set_global_pts(nut, s.pts);
		for (i = 0; i < nut->stream_count; i++) nut->sc[i].last_key = 0;
		clear_dts_cache(nut);
		if (s.pos > start + 7) goto err_out; // error condition, we didn't get the syncpoint we wanted
		nut->seek_status = s.pos << 1;
	}

	if (stopper) for (i = 1; i < sl->len; i++) {
		if ((sl->s[i].pos >> 1) > (stopper->pos >> 1) - (stopper->back_ptr>>1) + 7) {
			if (sl->s[i-1].back_ptr & 1) stopper_syncpoint = sl->s[i].pos >> 1;
			break;
		}
	}

	if (!(nut->seek_status & 1)) while (bctello(nut->i) < end || !end) {
		uint64_t tmp;
		nut_packet_t pd;
		off_t begin = bctello(nut->i);
		int read;
		buf_before = nut->i->buf_ptr;
		CHECK(get_bytes(nut->i, 1, &tmp)); // frame_code or 'N'
		if (tmp != 'N' && !(nut->ft[tmp].flags & INVALID_FLAG)) { // frame
			CHECK(get_frame(nut, &pd, tmp));
			if (pts[pd.stream] & 1) {
				if (end && pd.pts > pts[pd.stream]>>1) break; // we are done
				if (pd.flags & NUT_KEY_STREAM_FLAG) {
					good_key[pd.stream] = begin;
					if (!end && pd.pts >= pts[pd.stream]>>1) { // forward seek end
						nut->i->buf_ptr = buf_before;
						break;
					}
				}
			} else if (stopper) {
				TO_PTS(stopper, stopper->pts)
				if (compare_ts(stopper_p, nut->sc[stopper_s].sh.timebase, pd.pts, nut->sc[pd.stream].sh.timebase) > 0) {
					good_key[pd.stream] = begin;
					if (stopper_syncpoint) {
						int n = 1;
						for (i = 0; i < nut->stream_count; i++) {
							if (!(pts[i] & 1) && good_key[i] < stopper_syncpoint) n = 0;
						}
						if (n) break; // smart linear search stop
					}
				}
			}
			read = ready_read_buf(nut->i, pd.len);
			ERROR(read < pd.len, buf_eof(nut->i));
			nut->i->buf_ptr += pd.len;
			continue;
		}

		ERROR(tmp != 'N', -ERR_NOT_FRAME_NOT_N);
		CHECK(get_bytes(nut->i, 7, &tmp));

		switch (((uint64_t)'N' << 56) | tmp) {
			case SYNCPOINT_STARTCODE: {
				int dont_flush = 0;
				nut->i->buf_ptr -= 8;
				if (stopper) {
					if (!stopper_syncpoint && bctello(nut->i) > (stopper->pos >> 1) - (stopper->back_ptr>>1) + 7) {
						int n = 1;
						stopper_syncpoint = bctello(nut->i);
						for (i = 0; i < nut->stream_count; i++) if (!(pts[i] & 1)) n = 0;
						if (n) break; // no inactive streams, stop now
					}
					// give it a chance, we might be able to do this in a single seek
					if (stopper_syncpoint == bctello(nut->i)) dont_flush = 1;
				}
				if (!dont_flush) flush_buf(nut->i); // flush at every syncpoint
				nut->i->buf_ptr += 8;
				// dopts.cache_syncpoints
				CHECK(get_syncpoint(nut)); // FIXME we're counting on syncpoint cache!!
				break;
			}
			case MAIN_STARTCODE:
			case STREAM_STARTCODE:
			case INFO_STARTCODE:
			case INDEX_STARTCODE:
				GET_V(nut->i, pd.len);
				read = ready_read_buf(nut->i, pd.len);
				ERROR(read < pd.len, buf_eof(nut->i));
				nut->i->buf_ptr += pd.len;
				break;
			default:
				err = -ERR_NOT_FRAME_NOT_N;
				goto err_out;
		}
	}
	if (!end) goto err_out; // forward seek

	for (i = 0; i < nut->stream_count; i++) {
		if (!(pts[i] & 1)) continue;
		if (good_key[i] && (!min_pos || good_key[i] < min_pos)) min_pos = good_key[i];
	}
	if (!min_pos) min_pos = start + 7;

	// after ALL this, we ended up in a worse position than where we were...
	ERROR(!backwards && min_pos < nut->before_seek, -ERR_NOT_SEEKABLE);

	// FIXME we're counting on syncpoint cache dopts.cache_syncpoints
	for (i = 1; i < sl->len; i++) if ((sl->s[i].pos >> 1) > min_pos) break;
	i--;
	if (!(nut->seek_status & 1)) seek_buf(nut->i, (sl->s[i].pos >> 1) + 8, SEEK_SET);

	buf_before = nut->i->buf_ptr;
	nut->seek_status |= 1;
	nut->last_syncpoint = 0;
	clear_dts_cache(nut);
	CHECK(get_syncpoint(nut));

	while (bctello(nut->i) < min_pos) {
		uint64_t tmp;
		nut_packet_t pd;
		int read;
		CHECK(get_bytes(nut->i, 1, &tmp)); // frame_code or 'N'
		if (tmp != 'N' && !(nut->ft[tmp].flags & INVALID_FLAG)) { // frame
			CHECK(get_frame(nut, &pd, tmp));
		} else { // should never be a syncpoint!
			CHECK(get_bytes(nut->i, 7, &tmp));
			GET_V(nut->i, pd.len);
		}
		read = ready_read_buf(nut->i, pd.len);
		ERROR(read < pd.len, buf_eof(nut->i));
		nut->i->buf_ptr += pd.len;
	}

err_out:
	if (err != 2) { // unless EAGAIN
		if (err) {
			if (err == -ERR_NOT_SEEKABLE) { // a failed seek - then go back to before everything started
				for (i = 0; i < nut->stream_count; i++) {
					nut->sc[i].last_pts = old_last_pts[i];
				}
				seek_buf(nut->i, nut->before_seek, SEEK_SET);
			} else { // some NUT error, let's just go back to last good syncpoint
				err = 0;
				seek_buf(nut->i, nut->seek_status >> 1, SEEK_SET);
			}
			nut->last_syncpoint = 0;
			clear_dts_cache(nut);
		}
		nut->seek_status = 0;
		free(nut->seek_state);
		nut->seek_state = NULL;
	} else {
		// for EAGAIN, store state
		if (!nut->seek_state) nut->seek_state = malloc(nut->stream_count * 2 * sizeof(*nut->seek_state));
		for (i = 0; i < nut->stream_count; i++) {
			nut->seek_state[i].good_key = good_key[i];
			nut->seek_state[i].old_last_pts = old_last_pts[i];
		}
		if (buf_before) nut->i->buf_ptr = buf_before; // rewind
	}
	return err;
}

static void req_to_pts(nut_context_t * nut, double * time_pos, int flags, uint64_t * pts, const int * active_streams) {
	uint64_t orig_pts = 0;
	int orig_stream = 0;
	int i;

	for (i = 0; i < nut->stream_count; i++) pts[i] = 0;
	if (active_streams) for (i = 0; active_streams[i] != -1; i++) pts[active_streams[i]] = 1;
	else for (i = 0; i < nut->stream_count; i++) pts[i] = 1;

	if (flags & 1) {
		for (i = 0; i < nut->stream_count; i++) {
			if (!pts[i]) continue;
			if (compare_ts(orig_pts, nut->sc[orig_stream].sh.timebase, nut->sc[i].last_dts, nut->sc[i].sh.timebase) < 0) {
				orig_pts = nut->sc[i].last_dts;
				orig_stream = i;
			}
		}
		*time_pos += TO_DOUBLE(orig_stream, orig_pts);
	}

	for (i = 0; i < nut->stream_count; i++) {
		pts[i] |= (uint64_t)(*time_pos / nut->sc[i].sh.timebase.nom * nut->sc[i].sh.timebase.den) << 1;
	}
}

int nut_seek(nut_context_t * nut, double time_pos, int flags, const int * active_streams) {
	int err = 0;
	off_t start = 0, end = 0;
	uint64_t pts[nut->stream_count];
	int backwards = flags & 1 ? time_pos < 0 : 1;
	syncpoint_t stopper = { 0, 0, 0 };

	if (!nut->i->isc.seek) return -ERR_NOT_SEEKABLE;

	req_to_pts(nut, &time_pos, flags, pts, active_streams);

	if (!nut->before_seek) nut->before_seek = bctello(nut->i);

	if (nut->syncpoints.len) {
		syncpoint_list_t * sl = &nut->syncpoints;
		int i;
		int sync[nut->stream_count];
		int good_sync = -2;
		int last_sync = 0;
		for (i = 0; i < nut->stream_count; i++) sync[i] = -1;

		for (i = 1; i < sl->len; i++) {
			int j;
			if (!(sl->s[i].pos & 1)) continue;
			for (j = 0; j < nut->stream_count; j++) {
				uint64_t tmp = sl->pts[i * nut->stream_count + j];
				if (pts[j]&1 && tmp--) {
					if ((pts[j] >> 1) < tmp) {
						if (!last_sync) last_sync = i;
					} else sync[j] = (i-1);
				}
			}
		}
		for (i = 0; i < nut->stream_count; i++) {
			if (!(pts[i] & 1)) continue;
			if (good_sync == -2 || good_sync > sync[i]) good_sync = sync[i];
		}

		if ((sl->s[sl->len-1].back_ptr & 1) && last_sync && good_sync != -1) {
			for (i = good_sync; i <= last_sync; i++) if (!(sl->s[i].pos & 1)) break;
			if (i != last_sync+1 && good_sync <= last_sync) good_sync = -1;
		} else good_sync = -1;
		if (good_sync >= 0) {
			start = sl->s[good_sync].pos >> 1;
			end = sl->s[++good_sync].pos >> 1;
			if (flags & 2) end = sl->s[last_sync - 1].pos >> 1; // for forward seek
		}
	}

	if (start == 0) CHECK(binary_search_syncpoint(nut, time_pos, pts, &start, &end, &stopper));
	else fprintf(stderr, "============= NO BINARY SEARCH\n");

	if (!(flags & 2)) { // regular seek
		CHECK(linear_search_seek(nut, backwards, pts, start, end, stopper.pos ? &stopper : NULL));
	} else { // forwards seek, find keyframe
		CHECK(linear_search_seek(nut, backwards, pts, end, 0, NULL));
	}
err_out:
	if (err != 2) { // unless EAGAIN
		flush_buf(nut->i);
		nut->before_seek = 0;
	}
	return err;
}

nut_context_t * nut_demuxer_init(nut_demuxer_opts_t * dopts) {
	nut_context_t * nut = malloc(sizeof(nut_context_t));

	nut->i = new_input_buffer(dopts->input);

	nut->syncpoints.len = 0;
	nut->syncpoints.alloc_len = 0;
	nut->syncpoints.s = NULL;
	nut->syncpoints.pts = NULL;

	nut->fti = NULL;
	nut->sc = NULL;
	nut->last_headers = 0;
	nut->stream_count = 0;
	nut->dopts = *dopts;
	nut->seek_status = 0;
	nut->before_seek = 0;
	nut->last_syncpoint = 0;
	nut->seek_state = NULL;
	return nut;
}

void nut_demuxer_uninit(nut_context_t * nut) {
	int i;
	if (!nut) return;
	for (i = 0; i < nut->stream_count; i++) {
		free(nut->sc[i].sh.fourcc);
		free(nut->sc[i].sh.codec_specific);
		free(nut->sc[i].pts_cache);
	}

	free(nut->syncpoints.s);
	free(nut->syncpoints.pts);
	free(nut->sc);
	free(nut->seek_state);
	free_buffer(nut->i);
	free(nut);
}

const char * nut_error(int error) {
	switch((enum errors)error) {
		case ERR_GENERAL_ERROR: return "General Error.";
		case ERR_BAD_VERSION: return "Bad NUT Version.";
		case ERR_NOT_FRAME_NOT_N: return "Invalid Framecode.";
		case ERR_BAD_CHECKSUM: return "Bad Checksum.";
		case ERR_NO_HEADERS: return "No headers found!";
		case ERR_NOT_SEEKABLE: return "Cannot seek to that position.";
		case ERR_BAD_STREAM_ORDER: return "Stream headers are stored in wrong order.";
		case ERR_NOSTREAM_STARTCODE: return "Expected stream startcode not found.";
		case ERR_BAD_EOF: return "Invalid forward_ptr!";
	}
	return NULL;
}
