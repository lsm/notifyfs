/*
  2010, 2011, 2012, 2013 Stef Bon <stefbon@gmail.com>

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
#ifndef NOTIFYFS_FILESYSTEM_H
#define NOTIFYFS_FILESYSTEM_H

#define NOTIFYFS_FILESYSTEM_NETWORK				1
#define NOTIFYFS_FILESYSTEM_KERNEL				2
#define NOTIFYFS_FILESYSTEM_STAT				4
#define NOTIFYFS_FILESYSTEM_SYSTEM				8
#define NOTIFYFS_FILESYSTEM_NODEV				16

struct fsfunctions_struct {
    const char *name;
    unsigned char (*apply) (char *fs, unsigned char kernelfs);
    int (*get_remotehost) (char *source, char *options, char *host, int len, unsigned char *islocal);
    void (*construct_url) (char *source, char *options, char *path, char *url, int len);
    char *(*process_url) (char *url);
    char *(*get_localpath) (char *fs, char *options, char *source, char *relpath);
    struct fsfunctions_struct *next;
};

struct notifyfs_filesystem_struct {
    char filesystem[32];
    int mode;
    struct fsfunctions_struct *fsfunctions;
    struct notifyfs_filesystem_struct *next;
    struct notifyfs_filesystem_struct *prev;
};

// Prototypes

struct notifyfs_filesystem_struct *read_supported_filesystems(char *name, int *error);

struct notifyfs_filesystem_struct *create_filesystem();
void add_filesystem(struct notifyfs_filesystem_struct *fs, unsigned char kernelfs);
struct notifyfs_filesystem_struct *get_next_kernel_fs(struct notifyfs_filesystem_struct *fs);
struct notifyfs_filesystem_struct *lookup_filesystem(char *name);

unsigned char is_fusefs(char *fs, unsigned char *isnetwork);

void register_fsfunctions();
void add_fsfunctions(struct fsfunctions_struct *fsfunctions);
struct fsfunctions_struct *lookup_fsfunctions_byfs(char *fs, unsigned char kernelfs);
struct fsfunctions_struct *lookup_fsfunctions_byservice(char *service);

#endif
