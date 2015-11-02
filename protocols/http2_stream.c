/* ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <stdlib.h>

#include <apr_pools.h>

#include "serf.h"
#include "serf_bucket_util.h"
#include "serf_private.h"

#include "protocols/http2_buckets.h"
#include "protocols/http2_protocol.h"

struct serf_http2_stream_data_t
{
  serf_request_t *request; /* May be NULL as streams may outlive requests */
  serf_bucket_t *response_agg;
};

serf_http2_stream_t *
serf_http2__stream_create(serf_http2_protocol_t *h2,
                          apr_int32_t streamid,
                          apr_uint32_t lr_window,
                          apr_uint32_t rl_window,
                          serf_bucket_alloc_t *alloc)
{
  serf_http2_stream_t *stream = serf_bucket_mem_alloc(alloc, sizeof(*stream));

  stream->h2 = h2;
  stream->alloc = alloc;

  stream->next = stream->prev = NULL;

  /* Delay creating this? */
  stream->data = serf_bucket_mem_alloc(alloc, sizeof(*stream->data));
  stream->data->request = NULL;
  stream->data->response_agg = NULL;

  stream->lr_window = lr_window;
  stream->rl_window = rl_window;

  if (streamid >= 0)
    stream->streamid = streamid;
  else
    stream->streamid = -1; /* Undetermined yet */

  stream->status = (streamid >= 0) ? H2S_IDLE : H2S_INIT;
  stream->new_reserved_stream = NULL;

  return stream;
}

void
serf_http2__stream_cleanup(serf_http2_stream_t *stream)
{
  if (stream->data)
    {
      if (stream->data->response_agg)
        serf_bucket_destroy(stream->data->response_agg);

      serf_bucket_mem_free(stream->alloc, stream->data);
      stream->data = NULL;
    }
  serf_bucket_mem_free(stream->alloc, stream);
}

apr_status_t
serf_http2__stream_setup_next_request(serf_http2_stream_t *stream,
                                      serf_connection_t *conn,
                                      serf_hpack_table_t *hpack_tbl)
{
  serf_request_t *request = conn->unwritten_reqs;
  apr_status_t status;
  serf_bucket_t *hpack;
  serf_bucket_t *body;

  SERF_H2_assert(request != NULL);
  if (!request)
    return APR_EGENERAL;

  stream->data->request = request;

  if (!request->req_bkt)
    {
      status = serf__setup_request(request);
      if (status)
        return status;
    }

  conn->unwritten_reqs = request->next;
  if (conn->unwritten_reqs_tail == request)
    conn->unwritten_reqs = conn->unwritten_reqs_tail = NULL;

  request->next = NULL;

  serf__link_requests(&conn->written_reqs, &conn->written_reqs_tail,
    request);
  conn->nr_of_written_reqs++;
  conn->nr_of_written_reqs--;

  serf__bucket_request_read(request->req_bkt, &body, NULL, NULL);
  status = serf__bucket_hpack_create_from_request(&hpack, hpack_tbl,
                                                  request->req_bkt,
                                                  request->conn->host_info.scheme,
                                                  request->allocator);
  if (status)
    return status;

  if (!body)
    {
      /* This destroys the body... Perhaps we should make an extract
      and clear api */
      serf_bucket_destroy(request->req_bkt);
      request->req_bkt = NULL;
    }

  hpack = serf__bucket_http2_frame_create(hpack, HTTP2_FRAME_TYPE_HEADERS,
                                           HTTP2_FLAG_END_STREAM
                                           | HTTP2_FLAG_END_HEADERS,
                                           &stream->streamid,
                                           serf_http2__allocate_stream_id,
                                           stream,
                                           HTTP2_DEFAULT_MAX_FRAMESIZE,
                                           NULL, NULL, request->allocator);

  serf_http2__enqueue_frame(stream->h2, hpack, TRUE);

  stream->status = H2S_HALFCLOSED_LOCAL; /* Headers sent */

  return APR_SUCCESS;
}

apr_status_t
serf_http2__stream_reset(serf_http2_stream_t *stream,
                         apr_status_t reason,
                         int local_reset)
{
  stream->status = H2S_CLOSED;

  if (stream->streamid < 0)
    return APR_SUCCESS;

  if (local_reset)
    return serf_http2__enqueue_stream_reset(stream->h2,
                                            stream->streamid,
                                            reason);

  return APR_SUCCESS;
}

static apr_status_t
stream_response_eof(void *baton,
                    serf_bucket_t *aggregate_bucket)
{
  serf_http2_stream_t *stream = baton;

  switch (stream->status)
    {
      case H2S_CLOSED:
      case H2S_HALFCLOSED_REMOTE:
        return APR_EOF;
      default:
        return APR_EAGAIN;
    }
}

static void
stream_setup_response(serf_http2_stream_t *stream,
                      serf_config_t *config)
{
  serf_request_t *request;
  serf_bucket_t *agg;

  agg = serf_bucket_aggregate_create(stream->alloc);
  serf_bucket_aggregate_hold_open(agg, stream_response_eof, stream);

  serf_bucket_set_config(agg, config);

  request = stream->data->request;

  if (!request)
    return;

  if (! request->resp_bkt)
    {
      apr_pool_t *scratch_pool = request->respool; /* ### Pass scratch pool */

      request->resp_bkt = request->acceptor(request, agg, request->acceptor_baton,
                                            scratch_pool);
    }

  stream->data->response_agg = agg;
}

static apr_status_t
stream_promise_item(void *baton,
                    const char *key,
                    apr_size_t key_sz,
                    const char *value,
                    apr_size_t value_sz)
{
  serf_http2_stream_t *parent_stream = baton;
  serf_http2_stream_t *stream = parent_stream->new_reserved_stream;

  SERF_H2_assert(stream != NULL);

  /* TODO: Store key+value somewhere to allow asking the application
           if it is interested in the promised stream.

           Most likely it is not interested *yet* as the HTTP/2 spec
           recommends pushing promised items *before* the stream that
           references them.

           So we probably want to store the request anyway, to allow
           matching this against a later added outgoing request.
   */
  return APR_SUCCESS;
}

static apr_status_t
stream_promise_done(void *baton,
                    serf_bucket_t *done_agg)
{
  serf_http2_stream_t *parent_stream = baton;
  serf_http2_stream_t *stream = parent_stream->new_reserved_stream;

  SERF_H2_assert(stream != NULL);
  SERF_H2_assert(stream->status == H2S_RESERVED_REMOTE);
  parent_stream->new_reserved_stream = NULL; /* End of PUSH_PROMISE */

  /* Anything else? */


  /* ### Absolute minimal implementation.
         Just sending that we are not interested in the initial SETTINGS
         would be the easier approach. */
  serf_http2__stream_reset(stream, SERF_ERROR_HTTP2_REFUSED_STREAM, TRUE);




  /* Exit condition:
     * Either we should accept the stream and are ready to receive
       HEADERS and DATA on it.
     * Or we aren't and reject the stream
   */
  SERF_H2_assert(stream->status == H2S_CLOSED
                 || stream->data->request != NULL);

  /* We must return a proper error or EOF here! */
  return APR_EOF;
}

serf_bucket_t *
serf_http2__stream_handle_hpack(serf_http2_stream_t *stream,
                                serf_bucket_t *bucket,
                                unsigned char frametype,
                                int end_stream,
                                apr_size_t max_entry_size,
                                serf_hpack_table_t *hpack_tbl,
                                serf_config_t *config,
                                serf_bucket_alloc_t *allocator)
{
  if (frametype == HTTP2_FRAME_TYPE_HEADERS)
    {
      if (!stream->data->response_agg)
        stream_setup_response(stream, config);

      bucket = serf__bucket_hpack_decode_create(bucket, NULL, NULL, max_entry_size,
                                                hpack_tbl, allocator);

      serf_bucket_aggregate_append(stream->data->response_agg, bucket);

      if (end_stream)
        {
          if (stream->status == H2S_HALFCLOSED_LOCAL)
            stream->status = H2S_CLOSED;
          else
            stream->status = H2S_HALFCLOSED_REMOTE;
        }
      return NULL; /* We want to drain the bucket ourselves */
    }
  else
    {
      serf_bucket_t *agg;
      SERF_H2_assert(frametype == HTTP2_FRAME_TYPE_PUSH_PROMISE);

      /* First create the HPACK decoder as requested */
      bucket = serf__bucket_hpack_decode_create(bucket,
                                                stream_promise_item, stream,
                                                max_entry_size,
                                                hpack_tbl, allocator);

      /* And now wrap around it the easiest way to get an EOF callback */
      agg = serf_bucket_aggregate_create(allocator);
      serf_bucket_aggregate_append(agg, bucket);

      serf_bucket_aggregate_hold_open(agg, stream_promise_done, stream);

      /* And return the aggregate, so the bucket will be drained for us */
      return agg;
    }
}

serf_bucket_t *
serf_http2__stream_handle_data(serf_http2_stream_t *stream,
                               serf_bucket_t *bucket,
                               unsigned char frametype,
                               int end_stream,
                               serf_config_t *config,
                               serf_bucket_alloc_t *allocator)
{
  if (!stream->data->response_agg)
    stream_setup_response(stream, config);

  serf_bucket_aggregate_append(stream->data->response_agg, bucket);

  if (end_stream)
    {
      if (stream->status == H2S_HALFCLOSED_LOCAL)
        stream->status = H2S_CLOSED;
      else
        stream->status = H2S_HALFCLOSED_REMOTE;
    }

  return NULL;
}

apr_status_t
serf_http2__stream_processor(void *baton,
                             serf_http2_protocol_t *h2,
                             serf_bucket_t *bucket)
{
  serf_http2_stream_t *stream = baton;
  apr_status_t status = APR_SUCCESS;
  serf_request_t *request = stream->data->request;

  SERF_H2_assert(stream->data->response_agg != NULL);

  if (request)
    {
      SERF_H2_assert(request->resp_bkt != NULL);

      status = stream->data->request->handler(request, request->resp_bkt,
                                              request->handler_baton,
                                              request->respool);

      if (! APR_STATUS_IS_EOF(status)
          && !SERF_BUCKET_READ_ERROR(status))
        return status;

      /* Ok, the request thinks is done, let's handle the bookkeeping,
         to remove it from the outstanding requests */
      {
        serf_connection_t *conn = serf_request_get_conn(request);
        serf_request_t **rq = &conn->written_reqs;
        serf_request_t *last = NULL;

        while (*rq && (*rq != request))
          {
            last = *rq;
            rq = &last->next;
          }

        if (*rq)
          {
            (*rq) = request->next;

            if (conn->written_reqs_tail == request)
              conn->written_reqs_tail = last;

            conn->nr_of_written_reqs--;
          }

        serf__destroy_request(request);
        stream->data->request = NULL;
      }

      if (SERF_BUCKET_READ_ERROR(status))
        {
          if (stream->status != H2S_CLOSED)
            {
              /* Tell the other side that we are no longer interested
                 to receive more data */
              serf_http2__stream_reset(stream, status, TRUE);
            }

          return status;
        }

      SERF_H2_assert(APR_STATUS_IS_EOF(status));

      /* Even though the request reported that it is done, we might not
         have read all the data that we should (*cough* padding *cough*),
         or perhaps an invalid 'Content-Length' value; maybe both.

         This may even handle not-interested - return EOF cases, but that
         would have broken the pipeline for HTTP/1.1.
         */

      /* ### For now, fall through and eat whatever is left.
             Usually this is 0 bytes */

      status = APR_SUCCESS;
    }

  while (!status)
    {
      struct iovec vecs[IOV_MAX];
      int vecs_used;

      /* Drain the bucket as efficiently as possible */
      status = serf_bucket_read_iovec(stream->data->response_agg,
                                      SERF_READ_ALL_AVAIL,
                                      IOV_MAX, vecs, &vecs_used);

      if (vecs_used)
        {
          /* We have data... What should we do with it? */
        }
    }

  if (APR_STATUS_IS_EOF(status)
      && (stream->status == H2S_CLOSED
          || stream->status == H2S_HALFCLOSED_REMOTE))
    {
      /* If there was a request, it is already gone, so we can now safely
         destroy our aggregate which may include everything upto the http2
         frames */
      serf_bucket_destroy(stream->data->response_agg);
      stream->data->response_agg = NULL;
    }

  return status;
}
