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

#define _REENTRANT
#define _GNU_SOURCE
#define _XOPEN_SOURCE 500

#ifdef HAVE_CONFIG_H
#include <config.h>
#else
#define PACKAGE_VERSION "1.0"
#define HAVE_SETXATTR
#endif

#define MODEMASK 07777

#define SMALL_PATH_MAX  	64
#define LINE_MAXLEN 		64
#define UNIX_PATH_MAX           108

#define LOGGING

#define LOG_LOGAREA_FILESYSTEM		1
#define LOG_LOGAREA_PATH_RESOLUTION	2
#define LOG_LOGAREA_MOUNTMONITOR	4
#define LOG_LOGAREA_MAINLOOP		8
#define LOG_LOGAREA_SOCKET		16
#define LOG_LOGAREA_XATTR		32
#define LOG_LOGAREA_WATCHES		64
#define LOG_LOGAREA_INODES		128
#define LOG_LOGAREA_MESSAGE		256
#define LOG_LOGAREA_MAX			511


