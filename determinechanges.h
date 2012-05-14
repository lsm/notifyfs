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
#ifndef _DETERMINECHANGES_H
#define _DETERMINECHANGES_H

#define FSEVENT_FILECHANGED_NONE	0
#define FSEVENT_FILECHANGED_FILE	1
#define FSEVENT_FILECHANGED_METADATA	2
#define FSEVENT_FILECHANGED_XATTR	4
#define FSEVENT_FILECHANGED_REMOVED	8

// Prototypes

unsigned char compare_file_simple(struct stat *st1, struct stat *st2);
unsigned char compare_metadata_simple(struct stat *st1, struct stat *st2);
unsigned char determinechanges(struct stat *cached_st, int mask, struct stat *st);
void update_timespec(struct timespec *laststat);

#endif



