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


struct linuxdvb_ddci
{
  linuxdvb_ca_t            *lca;    /* back link to the associated CA */
  char                     *lddci_path;
  int                       lddci_fd;
};

linuxdvb_ddci_t *
linuxdvb_ddci_create
  ( linuxdvb_ca_t *lca, const char *ci_path)
{
  linuxdvb_ddci_t *lddci;

  lddci = calloc(1, sizeof(*lddci));
  lddci->lca = lca;
  lddci->lddci_path  = strdup(ci_path);
  lddci->lddci_fd = -1;

  return lddci;
}
