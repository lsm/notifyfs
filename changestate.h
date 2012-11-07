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
#ifndef NOTIFYFS_CHANGESTATE_H
#define NOTIFYFS_CHANGESTATE_H

// Prototypes

void del_watch_backend(struct effective_watch_struct *effective_watch);
void set_watch_backend(struct effective_watch_struct *effective_watch, int newmask, unsigned char lockset);

void changestate(struct call_info_struct *call_info, unsigned char typeaction);
void process_notifyfs_fsevent(struct notifyfs_fsevent_struct *notifyfs_fsevent);

void update_notifyfs(unsigned char firstrun);

#endif



