/****************************************************************************
 * apps/external/services/usrsock/usrsock_rpmsg_server.c
 * usrsock rpmsg server
 *
 *   Copyright (C) 2018 Pinecone Inc. All rights reserved.
 *   Author: Jianli Dong <dongjianli@pinecone.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include <nuttx/net/net.h>
#include <openamp/open_amp.h>

#include "usrsock_rpmsg.h"

#define USRSOCK_RPMSG_POLL_EVENTS    \
            (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLSOCK)

struct usrsock_rpmsg_s
{
  pid_t                 pid;
  pthread_mutex_t       mutex;
  pthread_cond_t        cond;
  struct socket         socks[CONFIG_NSOCKET_DESCRIPTORS];
  struct rpmsg_channel *channels[CONFIG_NSOCKET_DESCRIPTORS];
  struct pollfd         pfds[CONFIG_NSOCKET_DESCRIPTORS];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void usrsock_rpmsg_send_ack(struct rpmsg_channel *channel,
                                   uint8_t xid, int32_t result);
static void usrsock_rpmsg_send_data_ack(struct rpmsg_channel *channel,
                                        struct usrsock_message_datareq_ack_s *ack,
                                        uint8_t xid, int32_t result,
                                        uint16_t valuelen, uint16_t valuelen_nontrunc);
static void usrsock_rpmsg_send_event(struct rpmsg_channel *channel,
                                     int16_t usockid, uint16_t events);

static void usrsock_rpmsg_socket_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_close_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_connect_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_sendto_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_recvfrom_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_setsockopt_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_getsockopt_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_getsockname_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_getpeername_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_bind_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_listen_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_accept_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);
static void usrsock_rpmsg_ioctl_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);

static void usrsock_rpmsg_channel_created(struct rpmsg_channel *channel);
static void usrsock_rpmsg_channel_destroyed(struct rpmsg_channel *channel);
static void usrsock_rpmsg_channel_received(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);

static int usrsock_rpmsg_prepare_poll(struct usrsock_rpmsg_s *priv,
                                      struct pollfd* pfds);
static void usrsock_rpmsg_process_poll(struct usrsock_rpmsg_s *priv,
                                       struct pollfd *pfds, int count);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const rpmsg_rx_cb_t g_usrsock_rpmsg_handler[] =
{
  [USRSOCK_REQUEST_SOCKET]      = usrsock_rpmsg_socket_handler,
  [USRSOCK_REQUEST_CLOSE]       = usrsock_rpmsg_close_handler,
  [USRSOCK_REQUEST_CONNECT]     = usrsock_rpmsg_connect_handler,
  [USRSOCK_REQUEST_SENDTO]      = usrsock_rpmsg_sendto_handler,
  [USRSOCK_REQUEST_RECVFROM]    = usrsock_rpmsg_recvfrom_handler,
  [USRSOCK_REQUEST_SETSOCKOPT]  = usrsock_rpmsg_setsockopt_handler,
  [USRSOCK_REQUEST_GETSOCKOPT]  = usrsock_rpmsg_getsockopt_handler,
  [USRSOCK_REQUEST_GETSOCKNAME] = usrsock_rpmsg_getsockname_handler,
  [USRSOCK_REQUEST_GETPEERNAME] = usrsock_rpmsg_getpeername_handler,
  [USRSOCK_REQUEST_BIND]        = usrsock_rpmsg_bind_handler,
  [USRSOCK_REQUEST_LISTEN]      = usrsock_rpmsg_listen_handler,
  [USRSOCK_REQUEST_ACCEPT]      = usrsock_rpmsg_accept_handler,
  [USRSOCK_REQUEST_IOCTL]       = usrsock_rpmsg_ioctl_handler,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void usrsock_rpmsg_send_ack(struct rpmsg_channel *channel,
                                   uint8_t xid, int32_t result)
{
  struct usrsock_message_req_ack_s ack;

  ack.head.msgid = USRSOCK_MESSAGE_RESPONSE_ACK;
  ack.head.flags = result == -EINPROGRESS ? USRSOCK_MESSAGE_FLAG_REQ_IN_PROGRESS : 0;

  ack.xid    = xid;
  ack.result = result;

  rpmsg_send(channel, &ack, sizeof(ack));
}

static void usrsock_rpmsg_send_data_ack(struct rpmsg_channel *channel,
                                        struct usrsock_message_datareq_ack_s *ack,
                                        uint8_t xid, int32_t result,
                                        uint16_t valuelen, uint16_t valuelen_nontrunc)
{
  ack->reqack.head.msgid = USRSOCK_MESSAGE_RESPONSE_DATA_ACK;
  ack->reqack.head.flags = 0;

  ack->reqack.xid    = xid;
  ack->reqack.result = result;

  if (result < 0)
    {
      result             = 0;
      valuelen           = 0;
      valuelen_nontrunc  = 0;
    }
  else if (valuelen > valuelen_nontrunc)
    {
      valuelen           = valuelen_nontrunc;
    }

  ack->valuelen          = valuelen;
  ack->valuelen_nontrunc = valuelen_nontrunc;

  rpmsg_send_nocopy(channel, ack, sizeof(*ack) + valuelen + result);
}

static void usrsock_rpmsg_send_event(struct rpmsg_channel *channel,
                                     int16_t usockid, uint16_t events)
{
  struct usrsock_message_socket_event_s event;

  event.head.msgid = USRSOCK_MESSAGE_SOCKET_EVENT;
  event.head.flags = USRSOCK_MESSAGE_FLAG_EVENT;

  event.usockid = usockid;
  event.events  = events;

  rpmsg_send(channel, &event, sizeof(event));
}

static void usrsock_rpmsg_socket_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_socket_s *req = data;
  int i, ret = -ENFILE;

  pthread_mutex_lock(&priv->mutex);
  for (i = 0; i < CONFIG_NSOCKET_DESCRIPTORS; i++)
    {
      if (priv->socks[i].s_crefs == 0)
        {
          ret = psock_socket(req->domain, req->type, req->protocol, &priv->socks[i]);
          if (ret >= 0)
            {
              priv->socks[i].s_crefs++;
              priv->channels[i] = channel;

              psock_fcntl(&priv->socks[i], F_SETFL,
                psock_fcntl(&priv->socks[i], F_GETFL) | O_NONBLOCK);

              if (req->type != SOCK_STREAM && req->type != SOCK_SEQPACKET)
                {
                  priv->pfds[i].ptr = &priv->socks[i];
                  priv->pfds[i].events = USRSOCK_RPMSG_POLL_EVENTS;
                  /* wakeup the poll thread */
                  kill(priv->pid, SIGUSR1);
                }
              ret = i; /* return index as the usockid */
            }
          break;
        }
    }

  /* Send the ack with lock to ensure the event come after response */
  usrsock_rpmsg_send_ack(channel, req->head.xid, ret);
  pthread_mutex_unlock(&priv->mutex);
}

static void usrsock_rpmsg_close_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_close_s *req = data;
  int ret = -EBADF;

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      pthread_mutex_lock(&priv->mutex);
      priv->channels[req->usockid] = NULL;
      priv->pfds[req->usockid].ptr = NULL;

      /* Signal and wait the poll thread to wakeup */
      kill(priv->pid, SIGUSR1);
      pthread_cond_wait(&priv->cond, &priv->mutex);
      pthread_mutex_unlock(&priv->mutex);

      /* It's safe to close sock here */
      ret = psock_close(&priv->socks[req->usockid]);
    }

  usrsock_rpmsg_send_ack(channel, req->head.xid, ret);
}

static void usrsock_rpmsg_connect_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_connect_s *req = data;
  int ret = -EBADF;

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      usrsock_rpmsg_send_ack(channel, req->head.xid, -EINPROGRESS);

      ret = psock_connect(&priv->socks[req->usockid],
              (const struct sockaddr *)(req + 1), req->addrlen);
    }

  usrsock_rpmsg_send_ack(channel, req->head.xid, ret);

  if (ret >= 0 && priv->pfds[req->usockid].ptr == NULL)
    {
      pthread_mutex_lock(&priv->mutex);
      priv->pfds[req->usockid].ptr = &priv->socks[req->usockid];
      priv->pfds[req->usockid].events = USRSOCK_RPMSG_POLL_EVENTS;
      kill(priv->pid, SIGUSR1); /* Wakeup the poll thread */
      pthread_mutex_unlock(&priv->mutex);
    }
}

static void usrsock_rpmsg_sendto_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_sendto_s *req = data;
  ssize_t ret = -EBADF;

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      ret = psock_sendto(&priv->socks[req->usockid],
              (const void *)(req + 1) + req->addrlen, req->buflen, 0,
              req->addrlen ? (const struct sockaddr *)(req + 1) : NULL,
              req->addrlen);
    }

  usrsock_rpmsg_send_ack(channel, req->head.xid, ret);

  if (ret >= 0 || ret == -EAGAIN)
    {
      pthread_mutex_lock(&priv->mutex);
      priv->pfds[req->usockid].events |= POLLOUT;
      kill(priv->pid, SIGUSR1); /* Wakeup the poll thread */
      pthread_mutex_unlock(&priv->mutex);
    }
}

static void usrsock_rpmsg_recvfrom_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_recvfrom_s *req = data;
  struct usrsock_message_datareq_ack_s *ack;
  socklen_t outaddrlen = req->max_addrlen;
  socklen_t inaddrlen = req->max_addrlen;
  size_t buflen = req->max_buflen;
  ssize_t ret = -EBADF;

  ack = rpmsg_get_tx_payload_buffer(channel, (uint32_t *)&len, true);
  if (sizeof(*ack) + inaddrlen + buflen > len)
    {
      buflen = len - sizeof(*ack) - inaddrlen;
    }

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      ret = psock_recvfrom(&priv->socks[req->usockid],
              (void *)(ack + 1) + inaddrlen, buflen, 0,
              outaddrlen ? (struct sockaddr *)(ack + 1) : NULL,
              outaddrlen ? &outaddrlen : NULL);
      if (ret > 0 && outaddrlen < inaddrlen)
        {
          memcpy((void *)(ack + 1) + outaddrlen,
                 (void *)(ack + 1) + inaddrlen, ret);
        }
    }

  usrsock_rpmsg_send_data_ack(channel,
    ack, req->head.xid, ret, inaddrlen, outaddrlen);

  if (ret >= 0 || ret == -EAGAIN)
    {
      pthread_mutex_lock(&priv->mutex);
      priv->pfds[req->usockid].events |= POLLIN;
      kill(priv->pid, SIGUSR1); /* Wakeup the poll thread */
      pthread_mutex_unlock(&priv->mutex);
    }
}

static void usrsock_rpmsg_setsockopt_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_setsockopt_s *req = data;
  int ret = -EBADF;

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      ret = psock_setsockopt(&priv->socks[req->usockid],
              req->level, req->option, req + 1, req->valuelen);
    }

  usrsock_rpmsg_send_ack(channel, req->head.xid, ret);
}

static void usrsock_rpmsg_getsockopt_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_getsockopt_s *req = data;
  struct usrsock_message_datareq_ack_s *ack;
  socklen_t optlen = req->max_valuelen;
  int ret = -EBADF;

  ack = rpmsg_get_tx_payload_buffer(channel, (uint32_t *)&len, true);

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      ret = psock_getsockopt(&priv->socks[req->usockid],
              req->level, req->option, ack + 1, &optlen);
    }

  usrsock_rpmsg_send_data_ack(channel,
    ack, req->head.xid, ret, optlen, optlen);
}

static void usrsock_rpmsg_getsockname_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_getsockname_s *req = data;
  struct usrsock_message_datareq_ack_s *ack;
  socklen_t outaddrlen = req->max_addrlen;
  socklen_t inaddrlen = req->max_addrlen;
  int ret = -EBADF;

  ack = rpmsg_get_tx_payload_buffer(channel, (uint32_t *)&len, true);

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      ret = psock_getsockname(&priv->socks[req->usockid],
              (struct sockaddr *)(ack + 1), &outaddrlen);
    }

  usrsock_rpmsg_send_data_ack(channel,
    ack, req->head.xid, ret, inaddrlen, outaddrlen);
}

static void usrsock_rpmsg_getpeername_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_getpeername_s *req = data;
  struct usrsock_message_datareq_ack_s *ack;
  socklen_t outaddrlen = req->max_addrlen;
  socklen_t inaddrlen = req->max_addrlen;
  int ret = -EBADF;

  ack = rpmsg_get_tx_payload_buffer(channel, (uint32_t *)&len, true);

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      ret = psock_getpeername(&priv->socks[req->usockid],
              (struct sockaddr *)(ack + 1), &outaddrlen);
    }

  usrsock_rpmsg_send_data_ack(channel,
    ack, req->head.xid, ret, inaddrlen, outaddrlen);
}

static void usrsock_rpmsg_bind_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_bind_s *req = data;
  int ret = -EBADF;

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      ret = psock_bind(&priv->socks[req->usockid],
              (const struct sockaddr *)(req + 1), req->addrlen);
    }

  usrsock_rpmsg_send_ack(channel, req->head.xid, ret);
}

static void usrsock_rpmsg_listen_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_listen_s *req = data;
  int ret = -EBADF;

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      ret = psock_listen(&priv->socks[req->usockid], req->backlog);
    }

  usrsock_rpmsg_send_ack(channel, req->head.xid, ret);

  if (ret >= 0)
    {
      pthread_mutex_lock(&priv->mutex);
      priv->pfds[req->usockid].ptr = &priv->socks[req->usockid];
      priv->pfds[req->usockid].events = USRSOCK_RPMSG_POLL_EVENTS;
      kill(priv->pid, SIGUSR1); /* Wakeup the poll thread */
      pthread_mutex_unlock(&priv->mutex);
    }
}

static void usrsock_rpmsg_accept_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_accept_s *req = data;
  struct usrsock_message_datareq_ack_s *ack;
  socklen_t outaddrlen = req->max_addrlen;
  socklen_t inaddrlen = req->max_addrlen;
  int i, ret = -ENFILE;

  ack = rpmsg_get_tx_payload_buffer(channel, (uint32_t *)&len, true);

  pthread_mutex_lock(&priv->mutex);
  for (i = 0; i < CONFIG_NSOCKET_DESCRIPTORS; i++)
    {
      if (req->usockid < 0 || req->usockid >= CONFIG_NSOCKET_DESCRIPTORS)
        {
          ret = -EBADF;
          break;
        }
      else if (priv->socks[i].s_crefs == 0)
        {
          ret = psock_accept(&priv->socks[req->usockid],
                  outaddrlen ? (struct sockaddr *)(ack + 1) : NULL,
                  outaddrlen ? &outaddrlen : NULL, &priv->socks[i]);
          if (ret >= 0)
            {
              priv->socks[i].s_crefs++;
              priv->channels[i] = channel;

              priv->pfds[i].ptr = &priv->socks[i];
              priv->pfds[i].events = USRSOCK_RPMSG_POLL_EVENTS;

              /* wakeup the poll thread */
              kill(priv->pid, SIGUSR1);

              /* Append index as usockid to the payload */
              if (outaddrlen <= inaddrlen)
                {
                  *(int16_t *)((void *)(ack + 1) + outaddrlen) = i;
                }
              else
                {
                  *(int16_t *)((void *)(ack + 1) + inaddrlen) = i;
                }
              ret = sizeof(int16_t); /* Return usockid size */
            }
          break;
        }
    }

  /* Send the ack with lock to ensure the event come after response */
  usrsock_rpmsg_send_data_ack(channel,
    ack, req->head.xid, ret, inaddrlen, outaddrlen);

  pthread_mutex_unlock(&priv->mutex);
}

static void usrsock_rpmsg_ioctl_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct usrsock_request_ioctl_s *req = data;
  struct usrsock_message_datareq_ack_s *ack;
  int ret = -EBADF;

  ack = rpmsg_get_tx_payload_buffer(channel, (uint32_t *)&len, true);

  if (req->usockid >= 0 && req->usockid < CONFIG_NSOCKET_DESCRIPTORS)
    {
      memcpy(ack + 1, req + 1, req->arglen);
      ret = psock_ioctl(&priv->socks[req->usockid],
              req->cmd, (unsigned long)(ack + 1));
    }

  usrsock_rpmsg_send_data_ack(channel,
    ack, req->head.xid, ret, req->arglen, req->arglen);
}

static void usrsock_rpmsg_channel_created(struct rpmsg_channel *channel)
{
  struct usrsock_rpmsg_s *priv;

  priv = rpmsg_get_callback_privdata(channel->name);
  rpmsg_set_privdata(channel, priv);
}

static void usrsock_rpmsg_channel_destroyed(struct rpmsg_channel *channel)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);
  struct socket *socks[CONFIG_NSOCKET_DESCRIPTORS];
  int i, count = 0;

  pthread_mutex_lock(&priv->mutex);
  /* Collect all socks belong to the dead client */
  for (i = 0; i < CONFIG_NSOCKET_DESCRIPTORS; i++)
    {
      if (priv->channels[i] == channel)
        {
          socks[count++] = &priv->socks[i];
          priv->channels[i] = NULL;
          priv->pfds[i].ptr = NULL;
        }
    }

  /* Signal and wait the poll thread to wakeup */
  kill(priv->pid, SIGUSR1);
  pthread_cond_wait(&priv->cond, &priv->mutex);
  pthread_mutex_unlock(&priv->mutex);

  /* It's safe to close all socks here */
  for (i = 0; i < count; i++)
    {
      psock_close(socks[i]);
    }
}

static void usrsock_rpmsg_channel_received(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_request_common_s *common = data;

  if (common->reqid >= 0 && common->reqid < USRSOCK_REQUEST__MAX)
    {
      g_usrsock_rpmsg_handler[common->reqid](channel, data, len, priv_, src);
    }
}

static int usrsock_rpmsg_prepare_poll(struct usrsock_rpmsg_s *priv,
                                      struct pollfd *pfds)
{
  int i, count = 0;

  pthread_mutex_lock(&priv->mutex);
  /* Signal the worker it's safe to close sock */
  pthread_cond_signal(&priv->cond);

  for (i = 0; i < CONFIG_NSOCKET_DESCRIPTORS; i++)
    {
      if (priv->pfds[i].ptr)
        {
          pfds[count++] = priv->pfds[i];
        }
    }
  pthread_mutex_unlock(&priv->mutex);

  return count;
}

static void usrsock_rpmsg_process_poll(struct usrsock_rpmsg_s *priv,
                                       struct pollfd *pfds, int count)
{
  int i, j;

  for(i = 0; i < count; i++)
    {
      j = (struct socket *)pfds[i].ptr - priv->socks;

      pthread_mutex_lock(&priv->mutex);
      if (priv->channels[j] != NULL)
        {
          int events = 0;

          if (pfds[i].revents & POLLIN)
            {
              events |= USRSOCK_EVENT_RECVFROM_AVAIL;
              /* Stop poll in until recv get called */
              priv->pfds[j].events &= ~POLLIN;
            }
          if (pfds[i].revents & POLLOUT)
            {
              events |= USRSOCK_EVENT_SENDTO_READY;
              /* Stop poll out until send get called */
              priv->pfds[j].events &= ~POLLOUT;
            }
          if (pfds[i].revents & (POLLHUP | POLLERR))
            {
              events |= USRSOCK_EVENT_REMOTE_CLOSED;
              /* Stop poll at all */
              priv->pfds[j].ptr = NULL;
            }

          if (events != 0)
            {
              usrsock_rpmsg_send_event(priv->channels[j], j, events);
            }
        }
      pthread_mutex_unlock(&priv->mutex);
    }
}

static int usrsock_rpmsg_daemon(int argc, char *argv[])
{
  struct pollfd pfds[CONFIG_NSOCKET_DESCRIPTORS];
  struct usrsock_rpmsg_s *priv;
  int ret;

  priv = calloc(1, sizeof(*priv));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->pid = getpid();

  pthread_mutex_init(&priv->mutex, NULL);
  pthread_cond_init(&priv->cond, NULL);

  sigrelse(SIGUSR1);

  ret = rpmsg_register_callback(USRSOCK_RPMSG_CHANNEL_NAME,
                                priv,
                                NULL,
                                NULL,
                                usrsock_rpmsg_channel_created,
                                usrsock_rpmsg_channel_destroyed,
                                usrsock_rpmsg_channel_received);
  if (ret < 0)
    {
      goto free_priv;
    }

  while (1)
    {
      /* Collect all socks which need monitor */
      ret = usrsock_rpmsg_prepare_poll(priv, pfds);

      /* Monitor the state change from them */
      if (poll(pfds, ret, -1) > 0)
        {
          /* Process all changed socks */
          usrsock_rpmsg_process_poll(priv, pfds, ret);
        }
    }

  rpmsg_unregister_callback(USRSOCK_RPMSG_CHANNEL_NAME);
free_priv:
  pthread_cond_destroy(&priv->cond);
  pthread_mutex_destroy(&priv->mutex);
  free(priv);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_BUILD_KERNEL
int main(int argc, char *argv[])
#else
int usrsock_main(int argc, char *argv[])
#endif
{
  int ret;

  ret = task_create(argv[0],
                     CONFIG_RPMSG_USRSOCK_PRIORITY,
                     CONFIG_RPMSG_USRSOCK_STACKSIZE,
                     usrsock_rpmsg_daemon,
                     argv + 1);

  return ret > 0 ? 0 : ret;
}
