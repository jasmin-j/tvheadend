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

/* DD CI send Buffer size in number of 188 byte packages */
// FIXME: make a config parameter
#define LDDCI_SEND_BUF_NUM_DEF  1500
#define LDDCI_RECV_BUF_NUM_DEF  1500

#define LDDCI_SEND_BUFFER_POLL_TMO  150    /* ms */
#define LDDCI_TS_SYNC_BYTE          0x47
#define LDDCI_TS_SIZE               188

#define LDDCI_TO_THREAD(_t)  (linuxdvb_ddci_thread_t *)(_t)

typedef struct linuxdvb_ddci_thread
{
  linuxdvb_ddci_t          *lddci;
  int                       lddci_thread_running;
  int                       lddci_thread_stop;
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
  uint64_t                                lddci_send_buf_size_max;
  uint64_t                                lddci_send_buf_size;
  pthread_mutex_t                         lddci_send_buf_lock;
  tvh_cond_t                              lddci_send_buf_cond;
  tvhlog_limit_t                          lddci_send_buf_loglimit;
  int                                     lddci_send_buf_pkgCntW;
  int                                     lddci_send_buf_pkgCntR;
} linuxdvb_ddci_send_buffer_t;

typedef struct linuxdvb_ddci_wr_thread
{
  linuxdvb_ddci_thread_t;    /* have to be at first */
  int                          lddci_cfg_send_buffer_sz; /* in TS packages */
  linuxdvb_ddci_send_buffer_t  lddci_send_buffer;
} linuxdvb_ddci_wr_thread_t;

typedef struct linuxdvb_ddci_rd_thread
{
  linuxdvb_ddci_thread_t;    /* have to be at first */
  int                          lddci_cfg_recv_buffer_sz; /* in TS packages */
} linuxdvb_ddci_rd_thread_t;

struct linuxdvb_ddci
{
  linuxdvb_ca_t             *lca;    /* back link to the associated CA */
  char                      *lddci_path;
  char                       lddci_id[6];
  int                        lddci_fd;
  linuxdvb_ddci_wr_thread_t  lddci_wr_thread;
  linuxdvb_ddci_rd_thread_t  lddci_rd_thread;
};


/*****************************************************************************
 *
 * DD CI Thread functions
 *
 *****************************************************************************/

static void
linuxdvb_ddci_thread_init
  ( linuxdvb_ddci_t *lddci, linuxdvb_ddci_thread_t *ddci_thread )
{
  ddci_thread->lddci = lddci;
  ddci_thread->lddci_thread_running = 0;
  ddci_thread->lddci_thread_stop = 0;
  pthread_mutex_init(&ddci_thread->lddci_thread_lock, NULL);
  tvh_cond_init(&ddci_thread->lddci_thread_cond);
}

static inline int
linuxdvb_ddci_thread_running ( linuxdvb_ddci_thread_t *ddci_thread )
{
  return ddci_thread->lddci_thread_running;
}

static int
linuxdvb_ddci_thread_start
  ( linuxdvb_ddci_thread_t *ddci_thread, void *(*thread_routine) (void *),
    void *arg, const char *name )
{
  int e = -1;

  if (!linuxdvb_ddci_thread_running(ddci_thread)) {
    pthread_mutex_lock(&ddci_thread->lddci_thread_lock);
    tvhthread_create(&ddci_thread->lddci_thread, NULL, thread_routine, arg,
                     name);
    do {
      e = tvh_cond_wait(&ddci_thread->lddci_thread_cond,
                        &ddci_thread->lddci_thread_lock);
      if (e == ETIMEDOUT) {
        tvherror(LS_DDCI, "create thread %s error", name );
        break;
      }
    } while (ERRNO_AGAIN(e));
    pthread_mutex_unlock(&ddci_thread->lddci_thread_lock);
  }

  return e;
}

static void
linuxdvb_ddci_thread_stop ( linuxdvb_ddci_thread_t *ddci_thread )
{
  if (linuxdvb_ddci_thread_running(ddci_thread)) {
    ddci_thread->lddci_thread_stop = 1;
    pthread_join(ddci_thread->lddci_thread, NULL);
  }
}


/*****************************************************************************
 *
 * DD CI Send Buffer functions
 *
 *****************************************************************************/

static void
linuxdvb_ddci_send_buffer_init
  ( linuxdvb_ddci_send_buffer_t *ddci_snd_buf, uint64_t ddci_snd_buf_max )
{
  TAILQ_INIT(&ddci_snd_buf->lddci_send_buf_queue);
  ddci_snd_buf->lddci_send_buf_size_max = ddci_snd_buf_max;
  ddci_snd_buf->lddci_send_buf_size = 0;
  pthread_mutex_init(&ddci_snd_buf->lddci_send_buf_lock, NULL);
  tvh_cond_init(&ddci_snd_buf->lddci_send_buf_cond);
  tvhlog_limit_reset(&ddci_snd_buf->lddci_send_buf_loglimit);
  ddci_snd_buf->lddci_send_buf_pkgCntW = 0;
  ddci_snd_buf->lddci_send_buf_pkgCntR = 0;
}

/* must be called with locked mutex */
static inline void
linuxdvb_ddci_send_buffer_remove
   ( linuxdvb_ddci_send_buffer_t *ddci_snd_buf, linuxdvb_ddci_send_packet_t *sp )
{
  if (sp) {
    assert( ddci_snd_buf->lddci_send_buf_size >= sp->lddci_send_pkt_len);
    ddci_snd_buf->lddci_send_buf_size -= sp->lddci_send_pkt_len;
    // memoryinfo_free(&mpegts_input_queue_memoryinfo, sizeof(mpegts_packet_t) + mp->mp_len);
    ddci_snd_buf->lddci_send_buf_pkgCntR += sp->lddci_send_pkt_len / LDDCI_TS_SIZE;
    TAILQ_REMOVE(&ddci_snd_buf->lddci_send_buf_queue, sp, lddci_send_pkt_link);
  }
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

  linuxdvb_ddci_send_buffer_remove(ddci_snd_buf, sp);

  pthread_mutex_unlock(&ddci_snd_buf->lddci_send_buf_lock);

  return sp;
}

static void
linuxdvb_ddci_send_buffer_put
  ( linuxdvb_ddci_send_buffer_t *ddci_snd_buf, const uint8_t *tsb, int len )
{
  pthread_mutex_lock(&ddci_snd_buf->lddci_send_buf_lock);

  if (ddci_snd_buf->lddci_send_buf_size < ddci_snd_buf->lddci_send_buf_size_max) {
    linuxdvb_ddci_send_packet_t  *sp;

    sp = malloc(sizeof(linuxdvb_ddci_send_packet_t) + len);
    sp->lddci_send_pkt_len = len;
    memcpy(sp->lddci_send_pkt_data, tsb, len);
    ddci_snd_buf->lddci_send_buf_size += len;
    ddci_snd_buf->lddci_send_buf_pkgCntW += len / LDDCI_TS_SIZE;
    // memoryinfo_alloc(&mpegts_input_queue_memoryinfo, sizeof(mpegts_packet_t) + len2);
    TAILQ_INSERT_TAIL(&ddci_snd_buf->lddci_send_buf_queue, sp, lddci_send_pkt_link);
    tvh_cond_signal(&ddci_snd_buf->lddci_send_buf_cond, 0);
  } else {
    if (tvhlog_limit(&ddci_snd_buf->lddci_send_buf_loglimit, 10))
      tvhwarn(LS_DDCI, "too much queued output data in send buffer, discarding new");
  }

  pthread_mutex_unlock(&ddci_snd_buf->lddci_send_buf_lock);
}

static void
linuxdvb_ddci_send_buffer_clear ( linuxdvb_ddci_send_buffer_t *ddci_snd_buf )
{
  linuxdvb_ddci_send_packet_t  *sp;

  pthread_mutex_lock(&ddci_snd_buf->lddci_send_buf_lock);

  while ((sp = TAILQ_FIRST(&ddci_snd_buf->lddci_send_buf_queue)))
  {
    linuxdvb_ddci_send_buffer_remove(ddci_snd_buf, sp);
    free(sp);
  }
  ddci_snd_buf->lddci_send_buf_pkgCntW = 0;
  ddci_snd_buf->lddci_send_buf_pkgCntR = 0;

  pthread_mutex_unlock(&ddci_snd_buf->lddci_send_buf_lock);
}


/*****************************************************************************
 *
 * DD CI Writer Thread functions
 *
 *****************************************************************************/

static void *
linuxdvb_ddci_write_thread ( void *arg )
{
  linuxdvb_ddci_wr_thread_t *ddci_wr_thread = arg;
  int                        fd = ddci_wr_thread->lddci->lddci_fd;
  char                      *ci_id = ddci_wr_thread->lddci->lddci_id;

  ddci_wr_thread->lddci_thread_running = 1;
  ddci_wr_thread->lddci_thread_stop = 0;
  while (tvheadend_is_running() && !ddci_wr_thread->lddci_thread_stop) {
    linuxdvb_ddci_send_packet_t *sp;

    sp = linuxdvb_ddci_send_buffer_get(&ddci_wr_thread->lddci_send_buffer,
                                       LDDCI_SEND_BUFFER_POLL_TMO);
    if (sp) {
      int r = tvh_write(fd, sp->lddci_send_pkt_data, sp->lddci_send_pkt_len);
      if (r)
        tvhwarn(LS_DDCI, "couldn't write to CAM %s:%m", ci_id);
      free(sp);
    }
  }
  ddci_wr_thread->lddci_thread_stop = 0;
  ddci_wr_thread->lddci_thread_running = 0;
  return NULL;
}

static inline void
linuxdvb_ddci_wr_thread_init ( linuxdvb_ddci_t *lddci )
{
  linuxdvb_ddci_thread_init(lddci, LDDCI_TO_THREAD(&lddci->lddci_wr_thread));
}

static int
linuxdvb_ddci_wr_thread_start ( linuxdvb_ddci_wr_thread_t *ddci_wr_thread )
{
  int e;

  // FIXME: Use a configuration parameter
  ddci_wr_thread->lddci_cfg_send_buffer_sz = LDDCI_SEND_BUF_NUM_DEF * LDDCI_TS_SIZE;
  linuxdvb_ddci_send_buffer_init(&ddci_wr_thread->lddci_send_buffer,
                                 ddci_wr_thread->lddci_cfg_send_buffer_sz);
  e = linuxdvb_ddci_thread_start(LDDCI_TO_THREAD(ddci_wr_thread),
                                 linuxdvb_ddci_write_thread, ddci_wr_thread,
                                 "lnxdvb-ddci-wr");

  return e;
}

static inline void
linuxdvb_ddci_wr_thread_stop ( linuxdvb_ddci_wr_thread_t *ddci_wr_thread )
{
  /* See function linuxdvb_ddci_wr_thread_buffer_put why we lock here.
   */

  pthread_mutex_lock(&ddci_wr_thread->lddci_thread_lock);
  linuxdvb_ddci_thread_stop(LDDCI_TO_THREAD(ddci_wr_thread));
  linuxdvb_ddci_send_buffer_clear(&ddci_wr_thread->lddci_send_buffer);
  pthread_mutex_unlock(&ddci_wr_thread->lddci_thread_lock);
}

static inline void
linuxdvb_ddci_wr_thread_buffer_put
  ( linuxdvb_ddci_wr_thread_t *ddci_wr_thread, const uint8_t *tsb, int len )
{
  /* We need to lock this function against linuxdvb_ddci_wr_thread_stop, because
   * linuxdvb_ddci_wr_thread_buffer_put may be executed by another thread
   * simultaneously, although the stop function is already running. Due to the
   * race condition with the tread_running flag, it may happen, that the buffer
   * is not empty after the stop function is finished. The next execution of
   * linuxdvb_ddci_wr_thread_start will then re-init the queue and the wrongly
   * stored data is lost -> memory leak.
   */

  pthread_mutex_lock(&ddci_wr_thread->lddci_thread_lock);
  if (linuxdvb_ddci_thread_running(LDDCI_TO_THREAD(ddci_wr_thread)))
    linuxdvb_ddci_send_buffer_put(&ddci_wr_thread->lddci_send_buffer, tsb, len );
  pthread_mutex_unlock(&ddci_wr_thread->lddci_thread_lock);
}


/*****************************************************************************
 *
 * DD CI Reader Thread functions
 *
 *****************************************************************************/

static int
ddci_ts_sync_search ( const uint8_t *tsb, int len )
{
  int skipped = 0;

  /* find two consecutive packets beginning with TS_SYNC */
  while (len > 0 &&
         (*tsb != LDDCI_TS_SYNC_BYTE ||
          (len > LDDCI_TS_SIZE && tsb[LDDCI_TS_SIZE] != LDDCI_TS_SYNC_BYTE))) {
    tsb++;
    len--;
    skipped++;
  }
  return skipped;
}

static inline int
ddci_ts_sync ( const uint8_t *tsb, int len )
{
  return *tsb == LDDCI_TS_SYNC_BYTE ? 0 : ddci_ts_sync_search(tsb, len);
}

static void *
linuxdvb_ddci_read_thread ( void *arg )
{
  linuxdvb_ddci_rd_thread_t *ddci_rd_thread = arg;
  int                        fd = ddci_rd_thread->lddci->lddci_fd;
  char                      *ci_id = ddci_rd_thread->lddci->lddci_id;
  tvhpoll_event_t ev[1];
  tvhpoll_t *efd;
  ssize_t n;
  sbuf_t sb;
  int nfds;
#define LDDCI_MIN_TS_PKT (100 * LDDCI_TS_SIZE)
#define LDDCI_MIN_TS_SYN (5 * LDDCI_TS_SIZE)

  /* Setup poll */
  efd = tvhpoll_create(1);
  memset(ev, 0, sizeof(ev));
  ev[0].events             = TVHPOLL_IN;
  ev[0].fd = ev[0].data.fd = fd;
  tvhpoll_add(efd, ev, 1);

  /* Allocate memory */
  sbuf_init_fixed(&sb, MINMAX(ddci_rd_thread->lddci_cfg_recv_buffer_sz,
                              LDDCI_TS_SIZE * 100, LDDCI_TS_SIZE * 10000));

  ddci_rd_thread->lddci_thread_running = 1;
  ddci_rd_thread->lddci_thread_stop = 0;
  while (tvheadend_is_running() && !ddci_rd_thread->lddci_thread_stop) {
    nfds = tvhpoll_wait(efd, ev, 1, 150);
    if (nfds <= 1) continue;
    assert(ev[0].data.fd == fd);

    /* Read */
    if ((n = sbuf_read(&sb, fd)) < 0) {
      if (ERRNO_AGAIN(errno))
        continue;
      if (errno == EOVERFLOW)
        tvhwarn(LS_DDCI, "read buffer overflow on CAM %s:%m", ci_id);
      else
        tvhwarn(LS_DDCI, "couldn't read from CAM %s:%m", ci_id);
      continue;
    }

    if (sb.sb_ptr > 0) {
      int len, skip;
      uint8_t *tsb;

// retry:
      tsb  = sb.sb_data;
      len  = sb.sb_ptr;
      if (len < LDDCI_MIN_TS_PKT)
          continue;

      skip = ddci_ts_sync(tsb, len);
      if (skip) {
        len -= skip;
        tsb += skip;
        tvhwarn(LS_DDCI, "CAM %s skipped %d bytes to sync on start of TS packet", ci_id, skip);
      }

      if (len < LDDCI_MIN_TS_SYN)
          continue;

      // FIXME: deliver the read TS data

    }


  }

  sbuf_free(&sb);
  tvhpoll_destroy(efd);

  ddci_rd_thread->lddci_thread_stop = 0;
  ddci_rd_thread->lddci_thread_running = 0;
  return NULL;
}

static inline void
linuxdvb_ddci_rd_thread_init ( linuxdvb_ddci_t *lddci )
{
  linuxdvb_ddci_thread_init(lddci, LDDCI_TO_THREAD(&lddci->lddci_rd_thread));
}

static int
linuxdvb_ddci_rd_thread_start ( linuxdvb_ddci_rd_thread_t *ddci_rd_thread )
{
  int e;

  // FIXME: Use a configuration parameter
  ddci_rd_thread->lddci_cfg_recv_buffer_sz = LDDCI_RECV_BUF_NUM_DEF * LDDCI_TS_SIZE;
  e = linuxdvb_ddci_thread_start(LDDCI_TO_THREAD(ddci_rd_thread),
                                 linuxdvb_ddci_read_thread, ddci_rd_thread,
                                 "lnxdvb-ddci-rd");

  return e;
}

static inline void
linuxdvb_ddci_rd_thread_stop ( linuxdvb_ddci_rd_thread_t *ddci_rd_thread )
{
  /* See function linuxdvb_ddci_wr_thread_buffer_put why we lock here.
   */

  linuxdvb_ddci_thread_stop(LDDCI_TO_THREAD(ddci_rd_thread));
}


/*****************************************************************************
 *
 * DD CI API functions
 *
 *****************************************************************************/

linuxdvb_ddci_t *
linuxdvb_ddci_create ( linuxdvb_ca_t *lca, const char *ci_path)
{
  linuxdvb_ddci_t *lddci;

  lddci = calloc(1, sizeof(*lddci));
  lddci->lca = lca;
  lddci->lddci_path  = strdup(ci_path);
  snprintf(lddci->lddci_id, sizeof(lddci->lddci_id), "ci%u", lca->lca_number);
  lddci->lddci_fd = -1;
  linuxdvb_ddci_wr_thread_init(lddci);
  linuxdvb_ddci_rd_thread_init(lddci);
  return lddci;
}

void
linuxdvb_ddci_close ( linuxdvb_ddci_t *lddci )
{
  if (lddci->lddci_fd >= 0) {
    tvhtrace(LS_DDCI, "closing %s %s (fd %d)",
             lddci->lddci_id, lddci->lddci_path, lddci->lddci_fd);
    linuxdvb_ddci_wr_thread_stop(&lddci->lddci_wr_thread);
    linuxdvb_ddci_rd_thread_stop(&lddci->lddci_rd_thread);
    close(lddci->lddci_fd);
    lddci->lddci_fd = -1;
  }
}

int
linuxdvb_ddci_open ( linuxdvb_ddci_t *lddci )
{
  int ret = 0;

  if (lddci->lddci_fd < 0) {
    lddci->lddci_fd = tvh_open(lddci->lddci_path, O_RDWR | O_NONBLOCK, 0);
    tvhtrace(LS_DDCI, "opening %s %s (fd %d)",
             lddci->lddci_id, lddci->lddci_path, lddci->lddci_fd);
    if (lddci->lddci_fd >= 0) {
      ret = linuxdvb_ddci_wr_thread_start(&lddci->lddci_wr_thread);
      if (!ret)
        ret = linuxdvb_ddci_rd_thread_start(&lddci->lddci_rd_thread);
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

void
linuxdvb_ddci_put ( linuxdvb_ddci_t *lddci, const uint8_t *tsb, int len )
{
  linuxdvb_ddci_wr_thread_buffer_put(&lddci->lddci_wr_thread, tsb, len );
}
