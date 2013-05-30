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

static inline int set_watch_backend_inotify(struct watch_struct *watch, char *path)
{
    return 0;
}
static inline int change_watch_backend_inotify(struct watch_struct *watch, char *path)
{
    return 0;
}
static inline void remove_watch_backend_inotify(struct watch_struct *watch)
{
    return;
}

static inline void initialize_inotify()
{
    return;
}

static inline void close_inotify()
{
    return;

}
