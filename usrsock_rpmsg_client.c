/****************************************************************************
 * apps/external/services/usrsock/usrsock_rpmsg_client.c
 * usrsock rpmsg client
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

#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <semaphore.h>
#include <signal.h>

#include <nuttx/fs/fs.h>
#include <openamp/open_amp.h>

#include "usrsock_rpmsg.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct usrsock_rpmsg_s
{
  struct rpmsg_channel *channel;
  const char           *cpu_name;
  pid_t                 pid;
  sem_t                 sem;
  struct file           file;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void usrsock_rpmsg_device_created(struct remote_device *rdev, void *priv_);
static void usrsock_rpmsg_channel_created(struct rpmsg_channel *channel);
static void usrsock_rpmsg_channel_destroyed(struct rpmsg_channel *channel);
static void usrsock_rpmsg_channel_received(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void usrsock_rpmsg_device_created(struct remote_device *rdev, void *priv_)
{
  struct usrsock_rpmsg_s *priv = priv_;
  struct rpmsg_channel *channel;

  if (!strcmp(priv->cpu_name, rdev->proc->cpu_name))
    {
      channel = rpmsg_create_channel(rdev, USRSOCK_RPMSG_CHANNEL_NAME);
      if (channel != NULL)
        {
          rpmsg_set_privdata(channel, priv);
        }
    }
}

static void usrsock_rpmsg_channel_created(struct rpmsg_channel *channel)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);

  if (priv != NULL)
    {
      priv->channel = channel;
      sem_post(&priv->sem);
    }
}

static void usrsock_rpmsg_channel_destroyed(struct rpmsg_channel *channel)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);

  if (priv != NULL)
    {
      priv->channel = NULL;
      kill(priv->pid, SIGUSR1);
    }
}

static void usrsock_rpmsg_channel_received(struct rpmsg_channel *channel,
                    void *data, int len, void *priv_, unsigned long src)
{
  struct usrsock_rpmsg_s *priv = rpmsg_get_privdata(channel);

  if (priv)
    {
      while (len > 0)
        {
          ssize_t ret = file_write(&priv->file, data, len);
          if (ret < 0)
            {
              break;
            }
          data += ret;
          len  -= ret;
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef BUILD_MODULE
int main(int argc, char *argv[])
#else
int usrsock_main(int argc, char *argv[])
#endif
{
  struct usrsock_rpmsg_s priv = {};
  int ret;

  if (argv[1] == NULL)
    {
      return -EINVAL;
    }

  priv.cpu_name = argv[1];
  priv.pid = getpid();

  sem_init(&priv.sem, 0, 0);
  sem_setprotocol(&priv.sem, SEM_PRIO_NONE);

  sigrelse(SIGUSR1);

  ret = rpmsg_register_callback(USRSOCK_RPMSG_CHANNEL_NAME,
                                &priv,
                                usrsock_rpmsg_device_created,
                                NULL,
                                usrsock_rpmsg_channel_created,
                                usrsock_rpmsg_channel_destroyed,
                                usrsock_rpmsg_channel_received);
  if (ret < 0)
    {
      goto destroy_sem;
    }

  while (1)
    {
      /* Wait until the rpmsg channel is ready */
      do
        {
          ret = sem_wait(&priv.sem);
          if (ret < 0)
            {
              ret = -errno;
            }
        }
      while (ret == -EINTR);

      if (ret < 0)
        {
          goto unregister_rpmsg;
        }

      /* Open the kernel channel */
      ret = file_open(&priv.file, "/dev/usrsock", O_RDWR);
      if (ret < 0)
        {
          ret = -errno;
          goto delete_channel;
        }

      /* Forward the packet from kernel to remote */
      while (1)
        {
          struct pollfd pfd;
          void  *buf;
          size_t len;

          /* Wait the packet ready */
          pfd.ptr = &priv.file;
          pfd.events = POLLIN | POLLFILE;
          ret = poll(&pfd, 1, -1);
          if (ret < 0)
            {
              ret = -errno;
              break;
            }

          /* Read the packet from kernel */
          buf = rpmsg_get_tx_payload_buffer(priv.channel, &len, true);
          if (!buf)
            {
              ret = -ENOMEM;
              break;
            }

          ret = file_read(&priv.file, buf, len);
          if (ret < 0)
            {
              break;
            }

          /* Send the packet to remote */
          ret = rpmsg_send_nocopy(priv.channel, buf, ret);
          if (ret < 0)
            {
              break;
            }
        }

      /* Reclaim the resource */
      file_close(&priv.file);
      if (priv.channel)
        {
          goto delete_channel;
        }

      /* The remote side crash, loop to wait it restore */
    }

delete_channel:
  rpmsg_delete_channel(priv.channel);
unregister_rpmsg:
  rpmsg_unregister_callback(USRSOCK_RPMSG_CHANNEL_NAME);
destroy_sem:
  sem_destroy(&priv.sem);
  return ret;
}
