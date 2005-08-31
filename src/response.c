#include <sys/types.h>
#include <sys/stat.h>

#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <assert.h>

#include <stdio.h>

#include "response.h"
#include "keyvalue.h"
#include "log.h"
#include "stat_cache.h"
#include "chunk.h"
#include "etag.h"

#include "connections.h"

#include "plugin.h"

#include "sys-socket.h"

int http_response_write_header(server *srv, connection *con) {
	buffer *b;
	size_t i;
	
	b = chunkqueue_get_prepend_buffer(con->write_queue);
	
	if (con->request.http_version == HTTP_VERSION_1_1) {
		BUFFER_COPY_STRING_CONST(b, "HTTP/1.1 ");
	} else {
		BUFFER_COPY_STRING_CONST(b, "HTTP/1.0 ");
	}
	buffer_append_long(b, con->http_status);
	BUFFER_APPEND_STRING_CONST(b, " ");
	buffer_append_string(b, get_http_status_name(con->http_status));
	
	if (con->request.http_version != HTTP_VERSION_1_1 || con->keep_alive == 0) {
		BUFFER_APPEND_STRING_CONST(b, "\r\nConnection: ");
		buffer_append_string(b, con->keep_alive ? "keep-alive" : "close");
	}
	
	if (con->response.transfer_encoding & HTTP_TRANSFER_ENCODING_CHUNKED) {
		BUFFER_APPEND_STRING_CONST(b, "\r\nTransfer-Encoding: chunked");
	}
	
	/* HTTP/1.1 requires a Date: header */
	BUFFER_APPEND_STRING_CONST(b, "\r\nDate: ");
	
	/* cache the generated timestamp */
	if (srv->cur_ts != srv->last_generated_date_ts) {
		buffer_prepare_copy(srv->ts_date_str, 255);
		
		strftime(srv->ts_date_str->ptr, srv->ts_date_str->size - 1, 
			 "%a, %d %b %Y %H:%M:%S GMT", gmtime(&(srv->cur_ts)));
			 
		srv->ts_date_str->used = strlen(srv->ts_date_str->ptr) + 1;
		
		srv->last_generated_date_ts = srv->cur_ts;
	}
	
	buffer_append_string_buffer(b, srv->ts_date_str);
	
	/* add all headers */
	for (i = 0; i < con->response.headers->used; i++) {
		data_string *ds;
		
		ds = (data_string *)con->response.headers->data[i];
		
		if (ds->value->used && ds->key->used &&
		    0 != strncmp(ds->key->ptr, "X-LIGHTTPD-", sizeof("X-LIGHTTPD-") - 1)) {
			BUFFER_APPEND_STRING_CONST(b, "\r\n");
			buffer_append_string_buffer(b, ds->key);
			BUFFER_APPEND_STRING_CONST(b, ": ");
			buffer_append_string_buffer(b, ds->value);
#if 0
			log_error_write(srv, __FILE__, __LINE__, "bb", 
					ds->key, ds->value);
#endif
		}
	}
	
	if (buffer_is_empty(con->conf.server_tag)) {
		BUFFER_APPEND_STRING_CONST(b, "\r\nServer: " PACKAGE_NAME "/" PACKAGE_VERSION);
	} else {
		BUFFER_APPEND_STRING_CONST(b, "\r\nServer: ");
		buffer_append_string_buffer(b, con->conf.server_tag);
	}
	
	BUFFER_APPEND_STRING_CONST(b, "\r\n\r\n");
	
	
	con->bytes_header = b->used - 1;
	
	if (con->conf.log_response_header) {
		log_error_write(srv, __FILE__, __LINE__, "sSb", "Response-Header:", "\n", b);
	}
	
	return 0;
}



handler_t http_response_prepare(server *srv, connection *con) {
	handler_t r;
	
	/* looks like someone has already done a decision */
	if (con->mode == DIRECT && 
	    (con->http_status != 0 && con->http_status != 200)) {
		/* remove a packets in the queue */
		if (con->file_finished == 0) {
			chunkqueue_reset(con->write_queue);
		}
		
		return HANDLER_FINISHED;
	}
	
	/* no decision yet, build conf->filename */
	if (con->mode == DIRECT && con->physical.path->used == 0) {
		char *qstr;
		
		if (con->conf.log_condition_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "run condition");
		}
		config_patch_connection(srv, con, COMP_SERVER_SOCKET); /* SERVERsocket */
		
		/**
		 * prepare strings
		 * 
		 * - uri.path_raw 
		 * - uri.path (secure)
		 * - uri.query
		 * 
		 */
		
		/** 
		 * Name according to RFC 2396
		 * 
		 * - scheme
		 * - authority
		 * - path
		 * - query
		 * 
		 * (scheme)://(authority)(path)?(query)
		 * 
		 * 
		 */
	
		buffer_copy_string(con->uri.scheme, con->conf.is_ssl ? "https" : "http");
		buffer_copy_string_buffer(con->uri.authority, con->request.http_host);
		buffer_to_lower(con->uri.authority);
		
		config_patch_connection(srv, con, COMP_HTTP_HOST);      /* Host:        */
		config_patch_connection(srv, con, COMP_HTTP_REMOTEIP);  /* Client-IP */
		config_patch_connection(srv, con, COMP_HTTP_REFERER);   /* Referer:     */
		config_patch_connection(srv, con, COMP_HTTP_USERAGENT); /* User-Agent:  */
		config_patch_connection(srv, con, COMP_HTTP_COOKIE);    /* Cookie:  */
		
		/** extract query string from request.uri */
		if (NULL != (qstr = strchr(con->request.uri->ptr, '?'))) {
			buffer_copy_string    (con->uri.query, qstr + 1);
			buffer_copy_string_len(con->uri.path_raw, con->request.uri->ptr, qstr - con->request.uri->ptr);
		} else {
			buffer_reset     (con->uri.query);
			buffer_copy_string_buffer(con->uri.path_raw, con->request.uri);
		}

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- splitting Request-URI");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Request-URI  : ", con->request.uri);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "URI-scheme   : ", con->uri.scheme);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "URI-authority: ", con->uri.authority);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "URI-path     : ", con->uri.path_raw);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "URI-query    : ", con->uri.query);
		}
		
		/* disable keep-alive if requested */
		
		if (con->request_count > con->conf.max_keep_alive_requests) {
			con->keep_alive = 0;
		}
		
		
		/**
		 *  
		 * call plugins 
		 * 
		 * - based on the raw URL
		 * 
		 */
		
		switch(r = plugins_call_handle_uri_raw(srv, con)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_FINISHED:
		case HANDLER_COMEBACK:
		case HANDLER_WAIT_FOR_EVENT:
		case HANDLER_ERROR:
			return r;
		default:
			log_error_write(srv, __FILE__, __LINE__, "sd", "handle_uri_raw: unknown return value", r);
			break;
		}

		/* build filename 
		 *
		 * - decode url-encodings  (e.g. %20 -> ' ')
		 * - remove path-modifiers (e.g. /../)
		 */
		
		
		
		if (con->request.http_method == HTTP_METHOD_OPTIONS &&
		    con->uri.path_raw->ptr[0] == '*' && con->uri.path_raw->ptr[1] == '\0') {
			/* OPTIONS * ... */
			buffer_copy_string_buffer(con->uri.path, con->uri.path_raw);
		} else {
			buffer_copy_string_buffer(srv->tmp_buf, con->uri.path_raw);
			buffer_urldecode_path(srv->tmp_buf);
			buffer_path_simplify(con->uri.path, srv->tmp_buf);
		}

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- sanatising URI");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "URI-path     : ", con->uri.path);
		}

		/**
		 *  
		 * call plugins 
		 * 
		 * - based on the clean URL
		 * 
		 */
		
		config_patch_connection(srv, con, COMP_HTTP_URL); /* HTTPurl */
		
		switch(r = plugins_call_handle_uri_clean(srv, con)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_FINISHED:
		case HANDLER_COMEBACK:
		case HANDLER_WAIT_FOR_EVENT:
		case HANDLER_ERROR:
			return r;
		default:
			log_error_write(srv, __FILE__, __LINE__, "");
			break;
		}
		
		if (con->request.http_method == HTTP_METHOD_OPTIONS &&
		    con->uri.path->ptr[0] == '*' && con->uri.path_raw->ptr[1] == '\0') {
			/* option requests are handled directly without checking of the path */
		
			response_header_insert(srv, con, CONST_STR_LEN("Allow"), CONST_STR_LEN("OPTIONS, GET, HEAD, POST"));

			con->http_status = 200;
			con->file_finished = 1;

			return HANDLER_FINISHED;
		}

		/***
		 * 
		 * border 
		 * 
		 * logical filename (URI) becomes a physical filename here
		 * 
		 * 
		 * 
		 */
		
		
		
		
		/* 1. stat()
		 * ... ISREG() -> ok, go on
		 * ... ISDIR() -> index-file -> redirect
		 * 
		 * 2. pathinfo() 
		 * ... ISREG()
		 * 
		 * 3. -> 404
		 * 
		 */
		
		/*
		 * SEARCH DOCUMENT ROOT
		 */
		
		/* set a default */
		
		buffer_copy_string_buffer(con->physical.doc_root, con->conf.document_root);
		buffer_copy_string_buffer(con->physical.rel_path, con->uri.path);
		
		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- before doc_root");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Doc-Root     :", con->physical.doc_root);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Rel-Path     :", con->physical.rel_path);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
		}
		/* the docroot plugin should set the doc_root and might also set the physical.path
		 * for us (all vhost-plugins are supposed to set the doc_root)
		 * */
		switch(r = plugins_call_handle_docroot(srv, con)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_FINISHED:
		case HANDLER_COMEBACK:
		case HANDLER_WAIT_FOR_EVENT:
		case HANDLER_ERROR:
			return r;
		default:
			log_error_write(srv, __FILE__, __LINE__, "");
			break;
		}
		
		/* MacOS X and Windows can't distiguish between upper and lower-case 
		 * 
		 * convert to lower-case
		 */
		if (con->conf.force_lower_case) {
			buffer_to_lower(con->physical.rel_path);
		}

		/* the docroot plugins might set the servername, if they don't we take http-host */
		if (buffer_is_empty(con->server_name)) {
			buffer_copy_string_buffer(con->server_name, con->uri.authority);
		}
		
		/** 
		 * create physical filename 
		 * -> physical.path = docroot + rel_path
		 * 
		 */
		
		buffer_copy_string_buffer(con->physical.path, con->physical.doc_root);
		BUFFER_APPEND_SLASH(con->physical.path);
		buffer_copy_string_buffer(con->physical.basedir, con->physical.path);
		if (con->physical.rel_path->ptr[0] == '/') {
			buffer_append_string_len(con->physical.path, con->physical.rel_path->ptr + 1, con->physical.rel_path->used - 2);
		} else {
			buffer_append_string_buffer(con->physical.path, con->physical.rel_path);
		}

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- after doc_root");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Doc-Root     :", con->physical.doc_root);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Rel-Path     :", con->physical.rel_path);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
		}

		switch(r = plugins_call_handle_physical(srv, con)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_FINISHED:
		case HANDLER_COMEBACK:
		case HANDLER_WAIT_FOR_EVENT:
		case HANDLER_ERROR:
			return r;
		default:
			log_error_write(srv, __FILE__, __LINE__, "");
			break;
		}
		
		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- logical -> physical");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Doc-Root     :", con->physical.doc_root);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Rel-Path     :", con->physical.rel_path);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
		}
	}
	
	/* 
	 * Noone catched away the file from normal path of execution yet (like mod_access)
	 * 
	 * Go on and check of the file exists at all
	 */
	
	if (con->mode == DIRECT) {
		char *slash = NULL;
		char *pathinfo = NULL;
		int found = 0;
		stat_cache_entry *sce = NULL;
		
		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- handling physical path");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
		}
		
		if (HANDLER_ERROR != stat_cache_get_entry(srv, con, con->physical.path, &sce)) {
			/* file exists */
			
			if (con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__,  "s",  "-- file found");
				log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
			}
			
			if (S_ISDIR(sce->st.st_mode)) {
				if (con->physical.path->ptr[con->physical.path->used - 2] != '/') {
					/* redirect to .../ */
					
					http_response_redirect_to_directory(srv, con);
					
					return HANDLER_FINISHED;
				}
			}
		} else {
			switch (errno) {
			case EACCES:
				con->http_status = 403;
	
				if (con->conf.log_request_handling) {
					log_error_write(srv, __FILE__, __LINE__,  "s",  "-- access denied");
					log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
				}
			
				buffer_reset(con->physical.path);
				return HANDLER_FINISHED;
			case ENOENT:
				con->http_status = 404;

				if (con->conf.log_request_handling) {
					log_error_write(srv, __FILE__, __LINE__,  "s",  "-- file not found");
					log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
				}

				buffer_reset(con->physical.path);
				return HANDLER_FINISHED;
			case ENOTDIR:
				/* PATH_INFO ! :) */
				break;
			default:
				/* we have no idea what happend. let's tell the user so. */
				con->http_status = 500;
				buffer_reset(con->physical.path);
				
				log_error_write(srv, __FILE__, __LINE__, "ssbsb",
						"file not found ... or so: ", strerror(errno),
						con->uri.path,
						"->", con->physical.path);
				
				return HANDLER_FINISHED;
			}
			
			/* not found, perhaps PATHINFO */
			
			buffer_copy_string_buffer(srv->tmp_buf, con->physical.path);
			
			do {
				struct stat st;
				
				if (slash) {
					buffer_copy_string_len(con->physical.path, srv->tmp_buf->ptr, slash - srv->tmp_buf->ptr);
				} else {
					buffer_copy_string_buffer(con->physical.path, srv->tmp_buf);
				}
				
				if (0 == stat(con->physical.path->ptr, &(st)) &&
				    S_ISREG(st.st_mode)) {
					found = 1;
					break;
				}
				
				if (pathinfo != NULL) {
					*pathinfo = '\0';
				}
				slash = strrchr(srv->tmp_buf->ptr, '/');
				
				if (pathinfo != NULL) {
					/* restore '/' */
					*pathinfo = '/';
				}
				
				if (slash) pathinfo = slash;
			} while ((found == 0) && (slash != NULL) && (slash - srv->tmp_buf->ptr > con->physical.basedir->used - 2));
			
			if (found == 0) {
				/* no it really doesn't exists */
				con->http_status = 404;
				
				if (con->conf.log_file_not_found) {
					log_error_write(srv, __FILE__, __LINE__, "sbsb",
							"file not found:", con->uri.path,
							"->", con->physical.path);
				}
				
				buffer_reset(con->physical.path);
				
				return HANDLER_FINISHED;
			}
			
			/* we have a PATHINFO */
			if (pathinfo) {
				buffer_copy_string(con->request.pathinfo, pathinfo);
				
				/*
				 * shorten uri.path
				 */
				
				con->uri.path->used -= strlen(pathinfo);
				con->uri.path->ptr[con->uri.path->used - 1] = '\0';
			}
			
			if (con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__,  "s",  "-- after pathinfo check");
				log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
				log_error_write(srv, __FILE__, __LINE__,  "sb", "URI          :", con->uri.path);
				log_error_write(srv, __FILE__, __LINE__,  "sb", "Pathinfo     :", con->request.pathinfo);
			}
		}
		
		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- handling subrequest");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
		}
		
		/* call the handlers */
		switch(r = plugins_call_handle_subrequest_start(srv, con)) {
		case HANDLER_GO_ON:
			/* request was not handled */
			break;
		case HANDLER_FINISHED:
		default:
			if (con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__,  "s",  "-- subrequest finished");
			}
			
			/* something strange happend */
			return r;
		}
		
		/* if we are still here, no one wanted the file, status 403 is ok I think */
		
		if (con->mode == DIRECT) {
			con->http_status = 403;
			
			return HANDLER_FINISHED;
		}
		
	}
	
	switch(r = plugins_call_handle_subrequest(srv, con)) {
	case HANDLER_GO_ON:
		/* request was not handled, looks like we are done */
		return HANDLER_FINISHED;
	case HANDLER_FINISHED:
		/* request is finished */
	default:
		/* something strange happend */
		return r;
	}
	
	/* can't happen */
	return HANDLER_COMEBACK;
}



int http_response_handle_cachable(server *srv, connection *con, buffer *mtime) {
	/*
	 * 14.26 If-None-Match
	 *    [...]
	 *    If none of the entity tags match, then the server MAY perform the
	 *    requested method as if the If-None-Match header field did not exist,
	 *    but MUST also ignore any If-Modified-Since header field(s) in the
	 *    request. That is, if no entity tags match, then the server MUST NOT
	 *    return a 304 (Not Modified) response.
	 */
	
	/* last-modified handling */
	if (con->request.http_if_none_match) {
		if (etag_is_equal(con->physical.etag, con->request.http_if_none_match)) {
			if (con->request.http_method == HTTP_METHOD_GET || 
			    con->request.http_method == HTTP_METHOD_HEAD) {
				
				/* check if etag + last-modified */
				if (con->request.http_if_modified_since) {
					
					size_t used_len;
					char *semicolon;
					
					if (NULL == (semicolon = strchr(con->request.http_if_modified_since, ';'))) {
						used_len = strlen(con->request.http_if_modified_since);
					} else {
						used_len = semicolon - con->request.http_if_modified_since;
					}
					
					if (0 == strncmp(con->request.http_if_modified_since, mtime->ptr, used_len)) {
						con->http_status = 304;
						return HANDLER_FINISHED;
					} else {
						char buf[sizeof("Sat, 23 Jul 2005 21:20:01 GMT")];

						/* convert to timestamp */
						if (used_len < sizeof(buf) - 1) {
							time_t t_header, t_file;
							struct tm tm;
							
							strncpy(buf, con->request.http_if_modified_since, used_len);
							buf[used_len] = '\0';
							
							strptime(buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
							t_header = mktime(&tm);
							
							strptime(mtime->ptr, "%a, %d %b %Y %H:%M:%S GMT", &tm);
							t_file = mktime(&tm);

							if (t_file > t_header) {
								con->http_status = 304;
								return HANDLER_FINISHED;
							}
						} else {
							log_error_write(srv, __FILE__, __LINE__, "ss", 
									con->request.http_if_modified_since, buf);
							
							con->http_status = 412;
							return HANDLER_FINISHED;
						}
					}
				} else {
					con->http_status = 304;
					return HANDLER_FINISHED;
				}
			} else {
				con->http_status = 412;
				return HANDLER_FINISHED;
			}
		}
	} else if (con->request.http_if_modified_since) {
		size_t used_len;
		char *semicolon;
		
		if (NULL == (semicolon = strchr(con->request.http_if_modified_since, ';'))) {
			used_len = strlen(con->request.http_if_modified_since);
		} else {
			used_len = semicolon - con->request.http_if_modified_since;
		}
		
		if (0 == strncmp(con->request.http_if_modified_since, mtime->ptr, used_len)) {
			con->http_status = 304;
			return HANDLER_FINISHED;
		}
	}

	return HANDLER_GO_ON;
}
