/*
 * GrWebSDR: a web SDR receiver
 *
 * Copyright (C) 2017 Ondřej Lysoněk
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING).  If not,
 * see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include "http.h"
#include "globals.h"
#include "utils.h"
#include <cstdio>
#include <cstring>
#include <iostream>

using namespace std;

const char *stream_name(const char *url)
{
	const char *ret;
	size_t len;
	const char *ogg = ".ogg";
	const size_t ogg_len = strlen(ogg);
	const char *prefix = "/streams/";

	if (strncmp(url, prefix, strlen(prefix)))
		return nullptr;
	ret = strrchr(url, '/') + 1;
	if (url + strlen(prefix) != ret)
		return nullptr;
	len = strlen(ret);
	if (len != STREAM_NAME_LEN)
		return nullptr;
	if (!strcmp(ogg, ret + len - ogg_len))
		return ret;
	else
		return nullptr;
}

int add_pollfd(int fd, short events)
{
	if (count_pollfds >= max_fds) {
		cerr << "Too many fds." << endl;
		return 1;
	}
	fd_lookup[fd] = count_pollfds;
	pollfds[count_pollfds].fd = fd;
	pollfds[count_pollfds].events = events;
	pollfds[count_pollfds].revents = 0;
	++count_pollfds;
	return 0;
}

void delete_pollfd(int fd)
{
	if (--count_pollfds) {
		pollfds[fd_lookup[fd]] = pollfds[count_pollfds];
		fd_lookup[pollfds[count_pollfds].fd] = fd_lookup[fd];
	}
}

int handle_new_stream(struct lws *wsi, const char *stream,
		struct http_user_data *data)
{
	unsigned char *buffer = (unsigned char *) data->buf;
	unsigned char *buf_pos = (unsigned char *) data->buf + LWS_PRE;
	unsigned char *buf_end = (unsigned char *) data->buf + sizeof(data->buf);
	const char *header;
	int n;
	receiver::sptr rec;

	auto iter = receiver_map.find(stream);
	if (iter == receiver_map.end()) {
		lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, nullptr);
		return -1;
	}
	rec = iter->second;
	if (!rec->is_ready() || rec->is_running()) {
		lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, nullptr);
		return -1;
	}
	data->fd = rec->get_fd()[0];
	add_pollfd(data->fd, POLLIN);
	fd2wsi[data->fd] = wsi;

	topbl->lock();
	rec->start();
	topbl->unlock();
	if (count_receivers_running() == 1)
		topbl->start();
	if (lws_add_http_header_status(wsi, 200, &buf_pos, buf_end))
		return 1;
	header = "audio/ogg";
	if (lws_add_http_header_by_token(wsi,
				WSI_TOKEN_HTTP_CONTENT_TYPE,
				(unsigned char *) header,
				strlen(header),
				&buf_pos, buf_end))
		return 1;
	header = "0";
	if (lws_add_http_header_by_token(wsi,
				WSI_TOKEN_HTTP_EXPIRES,
				(unsigned char *) header,
				strlen(header),
				&buf_pos, buf_end))
		return 1;
	header = "no-cache";
	if (lws_add_http_header_by_token(wsi,
				WSI_TOKEN_HTTP_PRAGMA,
				(unsigned char *) header,
				strlen(header),
				&buf_pos, buf_end))
		return 1;
	header = "no-cache, no-store, must-revalidate";
	if (lws_add_http_header_by_token(wsi,
				WSI_TOKEN_HTTP_CACHE_CONTROL,
				(unsigned char *) header,
				strlen(header),
				&buf_pos, buf_end))
		return 1;

	if (lws_finalize_http_header(wsi, &buf_pos, buf_end))
		return 1;
	*buf_pos = '\0';
	n = lws_write(wsi, buffer + LWS_PRE,
			buf_pos - (buffer + LWS_PRE),
			LWS_WRITE_HTTP_HEADERS);
	if (n < 0) {
		return -1;
	}

	return 0;
}

int init_http_session(struct lws *wsi, void *user, void *in, size_t len)
{
	struct http_user_data *data = (struct http_user_data *) user;
	const char *stream;

	(void) user;
	(void) len;

	cout << "Received LWS_CALLBACK_HTTP" << endl;
	cout << "URL requested: " << in << endl;

	if (len > MAX_URL_LEN) {
		lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, nullptr);
		return -1;
	}

	data->fd = -1;
	strncpy(data->url, (char *) in, len);
	data->url[len] = '\0';
	stream = stream_name((char *) in);
	if (stream) {
		return handle_new_stream(wsi, stream, data);
	} else {
		lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, nullptr);
		return -1;
	}
}

void end_http_session(struct http_user_data *data)
{
	const char *stream;
	receiver::sptr rec;

	if (!data)
		return;
	stream = stream_name(data->url);
	if (!stream)
		return;
	cout << "Closing stream " << stream << endl;
	if (data->fd >= 0) {
		delete_pollfd(data->fd);
		fd2wsi[data->fd] = nullptr;
	}
	auto iter = receiver_map.find(stream);
	if (iter == receiver_map.end())
		return;
	if (count_receivers_running() == 1) {
		topbl->stop();
		topbl->wait();
	}
	rec = iter->second;
	topbl->lock();
	rec->stop();
	topbl->unlock();
}

int send_audio(struct lws *wsi, struct http_user_data *data)
{
	size_t max = sizeof(data->buf) - LWS_PRE;
	ssize_t res;
	unsigned char *buffer = (unsigned char *) data->buf;

	res = read(data->fd, data->buf + LWS_PRE, max);
	if (res <= 0) {
		if (errno == EAGAIN) {
			return 0;
		} else {
			perror("read");
			return -1;
		}
	}
	res = lws_write(wsi, buffer + LWS_PRE, res, LWS_WRITE_HTTP);
	if (res < 0) {
		cerr << "lws_write() failed." << endl;
		return -1;
	}
	lws_set_timeout(wsi, PENDING_TIMEOUT_HTTP_CONTENT, 5);
	return 0;
}

int http_cb(struct lws *wsi, enum lws_callback_reasons reason,
		void *user, void *in, size_t len)
{
	struct http_user_data *data = (struct http_user_data *) user;
	struct lws_pollargs *pollargs = (struct lws_pollargs *) in;

	switch (reason) {
	case LWS_CALLBACK_HTTP:
		return init_http_session(wsi, user, in, len);
	case LWS_CALLBACK_CLOSED_HTTP:
		end_http_session(data);
		break;
	case LWS_CALLBACK_HTTP_FILE_COMPLETION:
		goto try_to_reuse;
	case LWS_CALLBACK_HTTP_WRITEABLE:
		if (data->fd < 0)
			goto try_to_reuse;
		return send_audio(wsi, data);
	case LWS_CALLBACK_ADD_POLL_FD:
		return add_pollfd(pollargs->fd, pollargs->events);
	case LWS_CALLBACK_DEL_POLL_FD:
		delete_pollfd(pollargs->fd);
		break;
	case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
		pollfds[fd_lookup[pollargs->fd]].events = pollargs->events;
		break;
	default:
		break;
	}
	return 0;

try_to_reuse:
	if (lws_http_transaction_completed(wsi))
		return -1;
	return 0;
}
