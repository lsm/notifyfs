/*
  2010, 2011 Stef Bon <stefbon@gmail.com>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef FUSE_WORKSPACE_HANDLELOCKINFOEVENTS_H
#define FUSE_WORKSPACE_HANDLELOCKINFOEVENTS_H


// Prototypes

int process_lockinfo_event(int fd, void *data, uint32_t events);
void init_handlelockinfoevent(struct workerthreads_queue_struct *workerthreads_queue);

#endif
