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
#ifndef NOTIFYFS_NETWORKUTILS_H
#define NOTIFYFS_NETWORKUTILS_H


// Prototypes

int get_hostname(char *address, const char *service, char *host, int len, unsigned char *islocal);

unsigned char isvalid_ipv4(char *address);
unsigned char isvalid_ipv6(char *address);

struct notifyfs_connection_struct *compare_notifyfs_connections(struct notifyfs_connection_struct *new_connection);
int get_value_mountoptions(char *options, const char *option, char *value, int len);

#endif
