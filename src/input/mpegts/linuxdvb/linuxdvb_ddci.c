 /*
 *  Tvheadend - Linux DVB DDCI
 *
 *  Copyright (C) 2017 Jasmin Jessich
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

#include "tvheadend.h"
#include "linuxdvb_private.h"
#include "tvhpoll.h"

#include <fcntl.h>

/* DD CI write Buffer size in number of 188 byte packages */
// FIXME: make a config parameter
#define LDDCI_WR_BUF_NUM_DEF  1500

#define LDDCI_SEND_BUFFER_POLL_TMO  150    /* ms */

#define LDDCI_TO_THREAD(_t)  (linuxdvb_ddci_thread_t *)(_t)

typedef struct linuxdvb_ddci_thread
{
  linuxdvb_ddci_t          *lddci;
  int                       lddci_thread_running;
  pthread_t                 lddci_thread;
  pthread_mutex_t           lddci_thread_lock;
  tvh_cond_t                lddci_thread_cond;
} linuxdvb_ddci_thread_t;

typedef struct linuxdvb_ddci_send_packet
{
  TAILQ_ENTRY(linuxdvb_ddci_send_packet)  lddci_send_pkt_link;
  size_t                                  lddci_send_pkt_len;
  uint8_t                                 lddci_send_pkt_data[0];
} linuxdvb_ddci_send_packet_t;

typedef struct linuxdvb_ddci_send_buffer
{
  TAILQ_HEAD(,linuxdvb_ddci_send_packet)  lddci_send_buf_queue;
  uint64_t                                lddci_send_buf_size;
  pthread_mutex_t                         lddci_send_buf_lock;
  tvh_cond_t                              lddci_send_buf_cond;
} linuxdvb_ddci_send_buffer_t;

typedef struct linuxdvb_ddci_wr_thread
{
  linuxdvb_ddci_thread_t;    /* have to be at first */
  // int                          lddci_write_cfg_buffer_sz;       /* in 188 byte packages */
  linuxdvb_ddci_send_buffer_t  lddci_send_buffer;
} linuxdvb_ddci_wr_thread_t;

struct linuxdvb_ddci
{
  linuxdvb_ca_t             *lca;    /* back link to the associated CA */
  char                      *lddci_path;
  char                       lddci_id[6];
  int                        lddci_fd;
  linuxdvb_ddci_wr_thread_t  lddci_wr_thread;
};

static void
linuxdvb_ddci_thread_init
  ( linuxdvb_ddci_t *lddci, linuxdvb_ddci_thread_t *ddci_thread )
{
  ddci_thread->lddci = lddci;
  ddci_thread->lddci_thread_running = 0;
  pthread_mutex_init(&ddci_thread->lddci_thread_lock, NULL);
  tvh_cond_init(&ddci_thread->lddci_thread_cond);
}

static int
linuxdvb_ddci_thread_start
  ( linuxdvb_ddci_thread_t *ddci_thread, void *(*thread_routine) (void *),
    void *arg, const char *name )
{
  int e;

  pthread_mutex_lock(&ddci_thread->lddci_thread_lock);
  tvhthread_create(&ddci_thread->lddci_thread, NULL, thread_routine, arg, name);
  do {
    e = tvh_cond_wait(&ddci_thread->lddci_thread_cond,
                      &ddci_thread->lddci_thread_lock);
    if (e == ETIMEDOUT) {
      tvherror(LS_DDCI, "create thread %s error", name );
      break;
    }
  } while (ERRNO_AGAIN(e));
  pthread_mutex_unlock(&ddci_thread->lddci_thread_lock);

  return e;
}

static void
linuxdvb_ddci_thread_stop ( linuxdvb_ddci_thread_t *ddci_thread )
{
  if (ddci_thread->lddci_thread_running) {
    ddci_thread->lddci_thread_running = 0;
    pthread_join(ddci_thread->lddci_thread, NULL);
  }
}

static void
linuxdvb_ddci_send_buffer_init ( linuxdvb_ddci_send_buffer_t *ddci_snd_buf )
{
  TAILQ_INIT(&ddci_snd_buf->lddci_send_buf_queue);
  ddci_snd_buf->lddci_send_buf_size = 0;
  pthread_mutex_init(&ddci_snd_buf->lddci_send_buf_lock, NULL);
  tvh_cond_init(&ddci_snd_buf->lddci_send_buf_cond);
}

static linuxdvb_ddci_send_packet_t *
linuxdvb_ddci_send_buffer_get
  ( linuxdvb_ddci_send_buffer_t *ddci_snd_buf, int64_t tmo )
{
  linuxdvb_ddci_send_packet_t  *sp;

  pthread_mutex_lock(&ddci_snd_buf->lddci_send_buf_lock);

  /* packet present? */
  sp = TAILQ_FIRST(&ddci_snd_buf->lddci_send_buf_queue);
  if (!sp) {
    int r;

    do {
      int64_t mono = mclk() + ms2mono(tmo);

      /* Wait for a packet */
      r = tvh_cond_timedwait(&ddci_snd_buf->lddci_send_buf_cond,
                             &ddci_snd_buf->lddci_send_buf_lock, mono);
      if (r == ETIMEDOUT) {
        break;
      }
    } while (ERRNO_AGAIN(r));

    sp = TAILQ_FIRST(&ddci_snd_buf->lddci_send_buf_queue);
  }

  /* packet now present? */
  if (sp) {
    ddci_snd_buf->lddci_send_buf_size -= sp->lddci_send_pkt_len;
    // memoryinfo_free(&mpegts_input_queue_memoryinfo, sizeof(mpegts_packet_t) + mp->mp_len);
    TAILQ_REMOVE(&ddci_snd_buf->lddci_send_buf_queue, sp, lddci_send_pkt_link);
  }

  pthread_mutex_unlock(&ddci_snd_buf->lddci_send_buf_lock);

  return sp;
}

static void *
linuxdvb_ddci_write_thread ( void *arg )
{
  linuxdvb_ddci_wr_thread_t *ddci_wr_thread = arg;
#if 0
  int                        fd = ddci_wr_thread->lddci->lddci_fd;
  char                      *ci_id = ddci_wr_thread->lddci->lddci_id;
#endif // if 0

  ddci_wr_thread->lddci_thread_running = 1;
  while (tvheadend_is_running() && ddci_wr_thread->lddci_thread_running) {
    linuxdvb_ddci_send_packet_t *sp;

    sp = linuxdvb_ddci_send_buffer_get(&ddci_wr_thread->lddci_send_buffer,
                                       LDDCI_SEND_BUFFER_POLL_TMO);
    if (sp) {
      // FIXME: Send to device
      free(sp);
    }
  }

  return NULL;
}

static int
linuxdvb_ddci_wr_thread_start
  ( linuxdvb_ddci_t *lddci, linuxdvb_ddci_wr_thread_t *ddci_wr_thread )
{
  int e;

  // FIXME: Use a configuration parameter
  // ddci_wr_thread->lddci_write_cfg_buffer_sz = LDDCI_WR_BUF_NUM_DEF * 188;

  linuxdvb_ddci_thread_init(lddci, LDDCI_TO_THREAD(ddci_wr_thread));
  linuxdvb_ddci_send_buffer_init(&ddci_wr_thread->lddci_send_buffer);

  e = linuxdvb_ddci_thread_start(LDDCI_TO_THREAD(ddci_wr_thread),
                                 linuxdvb_ddci_write_thread, ddci_wr_thread,
                                 "lnxdvb-ddci-wr");

  return e;
}

#if 0
static void *
linuxdvb_ddci_write_thread ( void *arg )
{
  linuxdvb_ddci_wr_thread_t *ddci_wr_thread = arg;
  char name[256], b;
  tvhpoll_event_t ev[1];
  tvhpoll_t *efd;
  ssize_t n;
  size_t counter = 0;
  sbuf_t sb;
  int i, nfds, nodata = 4;

  /* Setup poll */
  efd = tvhpoll_create(1);
  memset(ev, 0, sizeof(ev));
  ev[0].events             = TVHPOLL_IN;
  ev[0].fd = ev[0].data.fd = ddci_wr_thread->lddci->lddci_fd;
  tvhpoll_add(efd, ev, 1);

  /* Allocate memory */
  sbuf_init_fixed(&sb, MINMAX(ddci_wr_thread->lddci_write_cfg_buffer_sz, 18800, 1880000));

  /* Read */
  while (tvheadend_is_running()) {
    nfds = tvhpoll_wait(efd, ev, 1, 150);
    if (nfds == 0) { /* timeout */
      if (nodata == 0) {

        tvhwarn(LS_LINUXDVB, "%s - poll TIMEOUT",
                ddci_wr_thread->lddci->lddci_id);
        nodata = 50;
        lfe->lfe_nodata = 1;
      } else {
        nodata--;
      }
    }
    if (nfds < 1) continue;
    if (ev[0].data.fd == lfe->lfe_dvr_pipe.rd) {
      if (read(lfe->lfe_dvr_pipe.rd, &b, 1) > 0) {
        if (b == 'c')
          linuxdvb_update_pids(lfe, name, &tuned, pids, ARRAY_SIZE(pids));
        else
          break;
      }
      continue;
    }
    if (ev[0].data.fd != dvr) break;

    nodata = 50;
    lfe->lfe_nodata = 0;

    /* Read */
    if ((n = sbuf_tsdebug_read(mmi->mmi_mux, &sb, dvr)) < 0) {
      if (ERRNO_AGAIN(errno))
        continue;
      if (errno == EOVERFLOW) {
        tvhwarn(LS_LINUXDVB, "%s - read() EOVERFLOW", name);
        continue;
      }
      tvherror(LS_LINUXDVB, "%s - read() error %d (%s)",
               name, errno, strerror(errno));
      break;
    }

    /* Skip the initial bytes */
    if (counter < skip) {
      counter += n;
      if (counter < skip) {
        sbuf_cut(&sb, n);
      } else {
        sbuf_cut(&sb, skip - (counter - n));
      }
    }

    /* Process */
    mpegts_input_recv_packets((mpegts_input_t*)lfe, mmi, &sb, 0, NULL);
  }

  sbuf_free(&sb);
  tvhpoll_destroy(efd);
  for (i = 0; i < ARRAY_SIZE(pids); i++)
    if (pids[i].fd >= 0)
      close(pids[i].fd);
  mpegts_pid_done(&tuned);
  close(dvr);
  return NULL;
}
#endif // if 0



linuxdvb_ddci_t *
linuxdvb_ddci_create ( linuxdvb_ca_t *lca, const char *ci_path)
{
  linuxdvb_ddci_t *lddci;

  lddci = calloc(1, sizeof(*lddci));
  lddci->lca = lca;
  lddci->lddci_path  = strdup(ci_path);
  snprintf(lddci->lddci_id, sizeof(lddci->lddci_id), "ci%u", lca->lca_number);
  lddci->lddci_fd = -1;

  return lddci;
}

#if 0

// code to write to send buffer (muster)

pthread_mutex_lock(&mi->mi_input_lock);
if (mmi->mmi_mux->mm_active == mmi) {
  if (mi->mi_input_queue_size < 50*1024*1024) {
    mi->mi_input_queue_size += len2;
    memoryinfo_alloc(&mpegts_input_queue_memoryinfo, sizeof(mpegts_packet_t) + len2);
    mpegts_mux_grab(mp->mp_mux);
    TAILQ_INSERT_TAIL(&mi->mi_input_queue, mp, mp_link);
    tvh_cond_signal(&mi->mi_input_cond, 0);
  } else {
    if (tvhlog_limit(&mi->mi_input_queue_loglimit, 10))
      tvhwarn(LS_MPEGTS, "too much queued input data (over 50MB), discarding new");
    free(mp);
  }
} else {
  free(mp);
}
pthread_mutex_unlock(&mi->mi_input_lock);

#endif // if 0

void
linuxdvb_ddci_close(linuxdvb_ddci_t *lddci)
{
  if (lddci->lddci_fd >= 0) {
    tvhtrace(LS_DDCI, "closing %s %s (fd %d)",
             lddci->lddci_id, lddci->lddci_path, lddci->lddci_fd);

    linuxdvb_ddci_thread_stop(LDDCI_TO_THREAD(&lddci->lddci_wr_thread));

    // FIXME: destroy buffers

    close(lddci->lddci_fd);
    lddci->lddci_fd = -1;
  }
}

int
linuxdvb_ddci_open(linuxdvb_ddci_t *lddci)
{
  int ret = 0;

  if (lddci->lddci_fd < 0) {
    lddci->lddci_fd = tvh_open(lddci->lddci_path, O_RDWR | O_NONBLOCK, 0);
    tvhtrace(LS_DDCI, "opening %s %s (fd %d)",
             lddci->lddci_id, lddci->lddci_path, lddci->lddci_fd);
    if (lddci->lddci_fd >= 0) {
      ret = linuxdvb_ddci_wr_thread_start(lddci, &lddci->lddci_wr_thread);
    }
    else {
      tvhtrace(LS_DDCI, "open failed %s %s (fd %d)",
               lddci->lddci_id, lddci->lddci_path, lddci->lddci_fd);
      ret = -1;
    }
  }

  if (ret < 0)
    linuxdvb_ddci_close(lddci);

  return ret;
}
