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
#ifndef NOTIFYFS_XATTR_H
#define NOTIFYFS_XATTR_H

#ifndef XATTR_SYSTEM_NAME
#define XATTR_SYSTEM_NAME  "notifyfs"
#endif

struct xattr_workspace_struct {
    char *name;
    void *value;
    size_t size;
    int nlen;
    int nerror;
};


// Prototypes

int setxattr4workspace(struct notifyfs_entry_struct *entry, const char *name, const char *value);
void getxattr4workspace(struct notifyfs_entry_struct *entry, const char *name, struct xattr_workspace_struct *xattr_workspace);
int listxattr4workspace(struct notifyfs_entry_struct *entry, char *list, size_t size);

#endif



