/**
 * eCryptfs: Linux filesystem encryption layer
 *
 * Copyright (C) 1997-2003 Erez Zadok
 * Copyright (C) 2001-2003 Stony Brook University
 * Copyright (C) 2004-2007 International Business Machines Corp.
 *   Author(s): Michael A. Halcrow <mahalcro@us.ibm.com>
 *              Michael C. Thompson <mcthomps@us.ibm.com>
 *              Tyler Hicks <tyhicks@ou.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/skbuff.h>
#include <linux/crypto.h>
#include <linux/netlink.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/key.h>
#include <linux/parser.h>
#include <linux/fs_stack.h>
#include "ecryptfs_kernel.h"

/**
 * Module parameter that defines the ecryptfs_verbosity level.
 */
int ecryptfs_verbosity = 0;

module_param(ecryptfs_verbosity, int, 0);
MODULE_PARM_DESC(ecryptfs_verbosity,
		 "Initial verbosity level (0 or 1; defaults to "
		 "0, which is Quiet)");

/**
 * Module parameter that defines the number of netlink message buffer
 * elements
 */
unsigned int ecryptfs_message_buf_len = ECRYPTFS_DEFAULT_MSG_CTX_ELEMS;

module_param(ecryptfs_message_buf_len, uint, 0);
MODULE_PARM_DESC(ecryptfs_message_buf_len,
		 "Number of message buffer elements");

/**
 * Module parameter that defines the maximum guaranteed amount of time to wait
 * for a response through netlink.  The actual sleep time will be, more than
 * likely, a small amount greater than this specified value, but only less if
 * the netlink message successfully arrives.
 */
signed long ecryptfs_message_wait_timeout = ECRYPTFS_MAX_MSG_CTX_TTL / HZ;

module_param(ecryptfs_message_wait_timeout, long, 0);
MODULE_PARM_DESC(ecryptfs_message_wait_timeout,
		 "Maximum number of seconds that an operation will "
		 "sleep while waiting for a message response from "
		 "userspace");

/**
 * Module parameter that is an estimate of the maximum number of users
 * that will be concurrently using eCryptfs. Set this to the right
 * value to balance performance and memory use.
 */
unsigned int ecryptfs_number_of_users = ECRYPTFS_DEFAULT_NUM_USERS;

module_param(ecryptfs_number_of_users, uint, 0);
MODULE_PARM_DESC(ecryptfs_number_of_users, "An estimate of the number of "
		 "concurrent users of eCryptfs");

unsigned int ecryptfs_transport = ECRYPTFS_DEFAULT_TRANSPORT;

void __ecryptfs_printk(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	if (fmt[1] == '7') { /* KERN_DEBUG */
		if (ecryptfs_verbosity >= 1)
			vprintk(fmt, args);
	} else
		vprintk(fmt, args);
	va_end(args);
}

/**
 * ecryptfs_init_persistent_file
 * @ecryptfs_dentry: Fully initialized eCryptfs dentry object, with
 *                   the lower dentry and the lower mount set
 *
 * eCryptfs only ever keeps a single open file for every lower
 * inode. All I/O operations to the lower inode occur through that
 * file. When the first eCryptfs dentry that interposes with the first
 * lower dentry for that inode is created, this function creates the
 * persistent file struct and associates it with the eCryptfs
 * inode. When the eCryptfs inode is destroyed, the file is closed.
 *
 * The persistent file will be opened with read/write permissions, if
 * possible. Otherwise, it is opened read-only.
 *
 * This function does nothing if a lower persistent file is already
 * associated with the eCryptfs inode.
 *
 * Returns zero on success; non-zero otherwise
 */
int ecryptfs_init_persistent_file(struct dentry *ecryptfs_dentry)
{
	struct ecryptfs_inode_info *inode_info =
		ecryptfs_inode_to_private(ecryptfs_dentry->d_inode);
	int rc = 0;

	mutex_lock(&inode_info->lower_file_mutex);
	if (!inode_info->lower_file) {
		struct dentry *lower_dentry;
		struct vfsmount *lower_mnt =
			ecryptfs_dentry_to_lower_mnt(ecryptfs_dentry);

		lower_dentry = ecryptfs_dentry_to_lower(ecryptfs_dentry);
		rc = ecryptfs_privileged_open(&inode_info->lower_file,
						     lower_dentry, lower_mnt);
		if (rc || IS_ERR(inode_info->lower_file)) {
			printk(KERN_ERR "Error opening lower persistent file "
			       "for lower_dentry [0x%p] and lower_mnt [0x%p]; "
			       "rc = [%d]\n", lower_dentry, lower_mnt, rc);
			rc = PTR_ERR(inode_info->lower_file);
			inode_info->lower_file = NULL;
		}
	}
	mutex_unlock(&inode_info->lower_file_mutex);
	return rc;
}

/**
 * ecryptfs_interpose
 * @lower_dentry: Existing dentry in the lower filesystem
 * @dentry: ecryptfs' dentry
 * @sb: ecryptfs's super_block
 * @flags: flags to govern behavior of interpose procedure
 *
 * Interposes upper and lower dentries.
 *
 * Returns zero on success; non-zero otherwise
 */
int ecryptfs_interpose(struct dentry *lower_dentry, struct dentry *dentry,
		       struct super_block *sb, u32 flags)
{
	struct inode *lower_inode;
	struct inode *inode;
	int rc = 0;

	lower_inode = lower_dentry->d_inode;
	if (lower_inode->i_sb != ecryptfs_superblock_to_lower(sb)) {
		rc = -EXDEV;
		goto out;
	}
	if (!igrab(lower_inode)) {
		rc = -ESTALE;
		goto out;
	}
	inode = iget5_locked(sb, (unsigned long)lower_inode,
			     ecryptfs_inode_test, ecryptfs_inode_set,
			     lower_inode);
	if (!inode) {
		rc = -EACCES;
		iput(lower_inode);
		goto out;
	}
	if (inode->i_state & I_NEW)
		unlock_new_inode(inode);
	else
		iput(lower_inode);
	if (S_ISLNK(lower_inode->i_mode))
		inode->i_op = &ecryptfs_symlink_iops;
	else if (S_ISDIR(lower_inode->i_mode))
		inode->i_op = &ecryptfs_dir_iops;
	if (S_ISDIR(lower_inode->i_mode))
		inode->i_fop = &ecryptfs_dir_fops;
	if (special_file(lower_inode->i_mode))
		init_special_inode(inode, lower_inode->i_mode,
				   lower_inode->i_rdev);
	dentry->d_op = &ecryptfs_dops;
	fsstack_copy_attr_all(inode, lower_inode, NULL);
	/* This size will be overwritten for real files w/ headers and
	 * other metadata */
	fsstack_copy_inode_size(inode, lower_inode);
	if (flags & ECRYPTFS_INTERPOSE_FLAG_D_ADD)
		d_add(dentry, inode);
	else
		d_instantiate(dentry, inode);
out:
	return rc;
}

enum { ecryptfs_opt_sig, ecryptfs_opt_ecryptfs_sig,
       ecryptfs_opt_cipher, ecryptfs_opt_ecryptfs_cipher,
       ecryptfs_opt_ecryptfs_key_bytes,
       ecryptfs_opt_passthrough, ecryptfs_opt_xattr_metadata,
       ecryptfs_opt_encrypted_view, ecryptfs_opt_force_nfs,
       ecryptfs_opt_force_cifs, ecryptfs_opt_force_ecryptfs,
       ecryptfs_opt_unlink_sigs, ecryptfs_opt_err };

static match_table_t tokens = {
	{ecryptfs_opt_sig, "sig=%s"},
	{ecryptfs_opt_ecryptfs_sig, "ecryptfs_sig=%s"},
	{ecryptfs_opt_cipher, "cipher=%s"},
	{ecryptfs_opt_ecryptfs_cipher, "ecryptfs_cipher=%s"},
	{ecryptfs_opt_ecryptfs_key_bytes, "ecryptfs_key_bytes=%u"},
	{ecryptfs_opt_passthrough, "ecryptfs_passthrough"},
	{ecryptfs_opt_xattr_metadata, "ecryptfs_xattr_metadata"},
	{ecryptfs_opt_encrypted_view, "ecryptfs_encrypted_view"},
	{ecryptfs_opt_force_nfs, "ecryptfs_force_nfs"},
	{ecryptfs_opt_force_cifs, "ecryptfs_force_cifs"},
	{ecryptfs_opt_force_ecryptfs, "ecryptfs_force_ecryptfs"},
	{ecryptfs_opt_unlink_sigs, "ecryptfs_unlink_sigs"},
	{ecryptfs_opt_err, NULL}
};

static int ecryptfs_init_global_auth_toks(
	struct ecryptfs_mount_crypt_stat *mount_crypt_stat)
{
	struct ecryptfs_global_auth_tok *global_auth_tok;
	int rc = 0;

	list_for_each_entry(global_auth_tok,
			    &mount_crypt_stat->global_auth_tok_list,
			    mount_crypt_stat_list) {
		rc = ecryptfs_keyring_auth_tok_for_sig(
			&global_auth_tok->global_auth_tok_key,
			&global_auth_tok->global_auth_tok,
			global_auth_tok->sig);
		if (rc) {
			printk(KERN_ERR "Could not find valid key in user "
			       "session keyring for sig specified in mount "
			       "option: [%s]\n", global_auth_tok->sig);
			global_auth_tok->flags |= ECRYPTFS_AUTH_TOK_INVALID;
			goto out;
		} else
			global_auth_tok->flags &= ~ECRYPTFS_AUTH_TOK_INVALID;
	}
out:
	return rc;
}

static void ecryptfs_init_mount_crypt_stat(
	struct ecryptfs_mount_crypt_stat *mount_crypt_stat)
{
	memset((void *)mount_crypt_stat, 0,
	       sizeof(struct ecryptfs_mount_crypt_stat));
	INIT_LIST_HEAD(&mount_crypt_stat->global_auth_tok_list);
	mutex_init(&mount_crypt_stat->global_auth_tok_list_mutex);
	mount_crypt_stat->flags |= ECRYPTFS_MOUNT_CRYPT_STAT_INITIALIZED;
}

/**
 * ecryptfs_parse_options
 * @sb: The ecryptfs super block
 * @options: The options pased to the kernel
 *
 * Parse mount options:
 * debug=N 	   - ecryptfs_verbosity level for debug output
 * sig=XXX	   - description(signature) of the key to use
 *
 * Returns the dentry object of the lower-level (lower/interposed)
 * directory; We want to mount our stackable file system on top of
 * that lower directory.
 *
 * The signature of the key to use must be the description of a key
 * already in the keyring. Mounting will fail if the key can not be
 * found.
 *
 * Returns zero on success; non-zero on error
 */
static int ecryptfs_parse_options(struct super_block *sb, char *options)
{
	char *p;
	int rc = 0;
	int sig_set = 0;
	int cipher_name_set = 0;
	int cipher_key_bytes;
	int cipher_key_bytes_set = 0;
	struct ecryptfs_mount_crypt_stat *mount_crypt_stat =
		&ecryptfs_superblock_to_private(sb)->mount_crypt_stat;
	substring_t args[MAX_OPT_ARGS];
	int token;
	char *sig_src;
	char *cipher_name_dst;
	char *cipher_name_src;
	char *cipher_key_bytes_src;

	if (!options) {
		rc = -EINVAL;
		goto out;
	}
	ecryptfs_init_mount_crypt_stat(mount_crypt_stat);
	while ((p = strsep(&options, ",")) != NULL) {
		if (!*p)
			continue;
		token = match_token(p, tokens, args);
		switch (token) {
		case ecryptfs_opt_sig:
		case ecryptfs_opt_ecryptfs_sig:
			sig_src = args[0].from;
			rc = ecryptfs_add_global_auth_tok(mount_crypt_stat,
							  sig_src);
			if (rc) {
				printk(KERN_ERR "Error attempting to register "
				       "global sig; rc = [%d]\n", rc);
				goto out;
			}
			sig_set = 1;
			break;
		case ecryptfs_opt_cipher:
		case ecryptfs_opt_ecryptfs_cipher:
			cipher_name_src = args[0].from;
			cipher_name_dst =
				mount_crypt_stat->
				global_default_cipher_name;
			strncpy(cipher_name_dst, cipher_name_src,
				ECRYPTFS_MAX_CIPHER_NAME_SIZE);
			ecryptfs_printk(KERN_DEBUG,
					"The mount_crypt_stat "
					"global_default_cipher_name set to: "
					"[%s]\n", cipher_name_dst);
			cipher_name_set = 1;
			break;
		case ecryptfs_opt_ecryptfs_key_bytes:
			cipher_key_bytes_src = args[0].from;
			cipher_key_bytes =
				(int)simple_strtol(cipher_key_bytes_src,
						   &cipher_key_bytes_src, 0);
			mount_crypt_stat->global_default_cipher_key_size =
				cipher_key_bytes;
			ecryptfs_printk(KERN_DEBUG,
					"The mount_crypt_stat "
					"global_default_cipher_key_size "
					"set to: [%d]\n", mount_crypt_stat->
					global_default_cipher_key_size);
			cipher_key_bytes_set = 1;
			break;
		case ecryptfs_opt_passthrough:
			mount_crypt_stat->flags |=
				ECRYPTFS_PLAINTEXT_PASSTHROUGH_ENABLED;
			break;
		case ecryptfs_opt_xattr_metadata:
			mount_crypt_stat->flags |=
				ECRYPTFS_XATTR_METADATA_ENABLED;
			break;
		case ecryptfs_opt_encrypted_view:
			mount_crypt_stat->flags |=
				ECRYPTFS_XATTR_METADATA_ENABLED;
			mount_crypt_stat->flags |=
				ECRYPTFS_ENCRYPTED_VIEW_ENABLED;
			break;
		case ecryptfs_opt_unlink_sigs:
			mount_crypt_stat->flags |= ECRYPTFS_UNLINK_SIGS;
			break;
		case ecryptfs_opt_err:
		default:
			ecryptfs_printk(KERN_WARNING,
					"eCryptfs: unrecognized option '%s'\n",
					p);
		}
	}
	if (!sig_set) {
		rc = -EINVAL;
		ecryptfs_printk(KERN_ERR, "You must supply at least one valid "
				"auth tok signature as a mount "
				"parameter; see the eCryptfs README\n");
		goto out;
	}
	if (!cipher_name_set) {
		int cipher_name_len = strlen(ECRYPTFS_DEFAULT_CIPHER);

		BUG_ON(cipher_name_len >= ECRYPTFS_MAX_CIPHER_NAME_SIZE);

		strcpy(mount_crypt_stat->global_default_cipher_name,
		       ECRYPTFS_DEFAULT_CIPHER);
	}
	if (!cipher_key_bytes_set) {
		mount_crypt_stat->global_default_cipher_key_size = 0;
	}
	mutex_lock(&key_tfm_list_mutex);
	if (!ecryptfs_tfm_exists(mount_crypt_stat->global_default_cipher_name,
				 NULL))
		rc = ecryptfs_add_new_key_tfm(
			NULL, mount_crypt_stat->global_default_cipher_name,
			mount_crypt_stat->global_default_cipher_key_size);
	mutex_unlock(&key_tfm_list_mutex);
	if (rc) {
		printk(KERN_ERR "Error attempting to initialize cipher with "
		       "name = [%s] and key size = [%td]; rc = [%d]\n",
		       mount_crypt_stat->global_default_cipher_name,
		       mount_crypt_stat->global_default_cipher_key_size, rc);
		rc = -EINVAL;
		goto out;
	}
	rc = ecryptfs_init_global_auth_toks(mount_crypt_stat);
	if (rc) {
		printk(KERN_WARNING "One or more global auth toks could not "
		       "properly register; rc = [%d]\n", rc);
	}
out:
	return rc;
}

struct kmem_cache *ecryptfs_sb_info_cache;

/**
 * ecryptfs_fill_super
 * @sb: The ecryptfs super block
 * @raw_data: The options passed to mount
 * @silent: Not used but required by function prototype
 *
 * Sets up what we can of the sb, rest is done in ecryptfs_read_super
 *
 * Returns zero on success; non-zero otherwise
 */
static int
ecryptfs_fill_super(struct super_block *sb, void *raw_data, int silent)
{
	int rc = 0;

	/* Released in ecryptfs_put_super() */
	ecryptfs_set_superblock_private(sb,
					kmem_cache_zalloc(ecryptfs_sb_info_cache,
							 GFP_KERNEL));
	if (!ecryptfs_superblock_to_private(sb)) {
		ecryptfs_printk(KERN_WARNING, "Out of memory\n");
		rc = -ENOMEM;
		goto out;
	}
	sb->s_op = &ecryptfs_sops;
	/* Released through deactivate_super(sb) from get_sb_nodev */
	sb->s_root = d_alloc(NULL, &(const struct qstr) {
			     .hash = 0,.name = "/",.len = 1});
	if (!sb->s_root) {
		ecryptfs_printk(KERN_ERR, "d_alloc failed\n");
		rc = -ENOMEM;
		goto out;
	}
	sb->s_root->d_op = &ecryptfs_dops;
	sb->s_root->d_sb = sb;
	sb->s_root->d_parent = sb->s_root;
	/* Released in d_release when dput(sb->s_root) is called */
	/* through deactivate_super(sb) from get_sb_nodev() */
	ecryptfs_set_dentry_private(sb->s_root,
				    kmem_cache_zalloc(ecryptfs_dentry_info_cache,
						     GFP_KERNEL));
	if (!ecryptfs_dentry_to_private(sb->s_root)) {
		ecryptfs_printk(KERN_ERR,
				"dentry_info_cache alloc failed\n");
		rc = -ENOMEM;
		goto out;
	}
	rc = 0;
out:
	/* Should be able to rely on deactivate_super called from
	 * get_sb_nodev */
	return rc;
}

static struct ecryptfs_lower_filesystem {
       char *name;
       u32 flag;
} ecryptfs_invalid_lower_filesystems[] = {
       {
	       .name = "nfs",
	       .flag = ECRYPTFS_FORCE_NFS,
       },
       {
	       .name = "cifs",
	       .flag = ECRYPTFS_FORCE_CIFS,
       },
       {
	       .name = "ecryptfs",
	       .flag = ECRYPTFS_FORCE_ECRYPTFS,
       },
};

/**
 * This is a temporary hack to prevent inadvertent mounting on lower
 * filesystems that are known to be currently incompatible with
 * eCryptfs. This mainly includes networked filesystems.
 */
static int
ecryptfs_validate_lower(const char *dev_name, char *options)
{
       struct super_block *lower_sb;
       struct nameidata nd;
       int i;
       char *p;
       int token;
       u32 flags = 0;
       substring_t args[MAX_OPT_ARGS];
       char *opts_tmp;
       char *opts_orig = NULL;
       int options_len;
       int rc;

       if (!options) {
	       rc = -EINVAL;
	       goto out;
       }
       options_len = (strlen(options) + 1);
       if ((opts_tmp = opts_orig = kmalloc(options_len, GFP_KERNEL)) == NULL) {
	       rc = -ENOMEM;
	       goto out;
       }
       memcpy(opts_orig, options, options_len);
       opts_orig[options_len - 1] = '\0';
       rc = path_lookup(dev_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &nd);
       if (rc) {
	       printk(KERN_WARNING
		      "path_lookup() failed on dev_name = [%s]\n", dev_name);
	       goto out;
       }
       lower_sb = nd.dentry->d_sb;
       while ((p = strsep(&opts_tmp, ",")) != NULL) {
	       if (!*p)
		       continue;
	       token = match_token(p, tokens, args);
	       switch (token) {
	       case ecryptfs_opt_force_nfs:
		       flags |= ECRYPTFS_FORCE_NFS;
		       break;
	       case ecryptfs_opt_force_cifs:
		       flags |= ECRYPTFS_FORCE_CIFS;
		       break;
	       case ecryptfs_opt_force_ecryptfs:
		       flags |= ECRYPTFS_FORCE_ECRYPTFS;
		       break;
	       }
       }
       for (i = 0; i < ARRAY_SIZE(ecryptfs_invalid_lower_filesystems); i++) {
	       struct ecryptfs_lower_filesystem *lower_fs;

	       lower_fs = &ecryptfs_invalid_lower_filesystems[i];
	       if (strcmp(lower_sb->s_type->name, lower_fs->name) == 0) {
		       printk(KERN_WARNING
			      "Mount request on top of filesystem type [%s]\n",
			      lower_sb->s_type->name);
		       if (lower_fs->flag & flags) {
			       printk(KERN_WARNING "Mount on filesystem of "
				      "type [%s] forced\n",
				      lower_sb->s_type->name);
			       path_release(&nd);
			       goto out;
		       } else {
			       rc = -EINVAL;
			       printk(KERN_ERR "Mount on filesystem of type "
				      "[%s] explicitly disallowed due to "
				      "known incompatibilities\n",
				       lower_sb->s_type->name);
			       path_release(&nd);
			       goto out;
		       }
	       }
       }
       path_release(&nd);
out:
       if (opts_orig)
	       kfree(opts_orig);
       return rc;
}


/**
 * ecryptfs_read_super
 * @sb: The ecryptfs super block
 * @dev_name: The path to mount over
 *
 * Read the super block of the lower filesystem, and use
 * ecryptfs_interpose to create our initial inode and super block
 * struct.
 */
static int ecryptfs_read_super(struct super_block *sb, const char *dev_name)
{
	int rc;
	struct nameidata nd;
	struct dentry *lower_root;
	struct vfsmount *lower_mnt;

	memset(&nd, 0, sizeof(struct nameidata));
	rc = path_lookup(dev_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &nd);
	if (rc) {
		ecryptfs_printk(KERN_WARNING, "path_lookup() failed\n");
		goto out;
	}
	lower_root = nd.dentry;
	lower_mnt = nd.mnt;
	ecryptfs_set_superblock_lower(sb, lower_root->d_sb);
	sb->s_maxbytes = lower_root->d_sb->s_maxbytes;
	sb->s_blocksize = lower_root->d_sb->s_blocksize;
	ecryptfs_set_dentry_lower(sb->s_root, lower_root);
	ecryptfs_set_dentry_lower_mnt(sb->s_root, lower_mnt);
	rc = ecryptfs_interpose(lower_root, sb->s_root, sb, 0);
	if (rc)
		goto out_free;
	rc = 0;
	goto out;
out_free:
	path_release(&nd);
out:
	return rc;
}

/**
 * ecryptfs_get_sb
 * @fs_type
 * @flags
 * @dev_name: The path to mount over
 * @raw_data: The options passed into the kernel
 *
 * The whole ecryptfs_get_sb process is broken into 4 functions:
 * ecryptfs_parse_options(): handle options passed to ecryptfs, if any
 * ecryptfs_fill_super(): used by get_sb_nodev, fills out the super_block
 *                        with as much information as it can before needing
 *                        the lower filesystem.
 * ecryptfs_read_super(): this accesses the lower filesystem and uses
 *                        ecryptfs_interpolate to perform most of the linking
 * ecryptfs_interpolate(): links the lower filesystem into ecryptfs
 */
static int ecryptfs_get_sb(struct file_system_type *fs_type, int flags,
			const char *dev_name, void *raw_data,
			struct vfsmount *mnt)
{
	int rc;
	struct super_block *sb;

	if ((rc = ecryptfs_validate_lower(dev_name, raw_data))) {
		printk(KERN_ERR "Invalid lower filesystem type\n");
		goto out;
	}

	rc = get_sb_nodev(fs_type, flags, raw_data, ecryptfs_fill_super, mnt);
	if (rc < 0) {
		printk(KERN_ERR "Getting sb failed; rc = [%d]\n", rc);
		goto out;
	}
	sb = mnt->mnt_sb;
	rc = ecryptfs_parse_options(sb, raw_data);
	if (rc) {
		printk(KERN_ERR "Error parsing options; rc = [%d]\n", rc);
		goto out_abort;
	}
	rc = ecryptfs_read_super(sb, dev_name);
	if (rc) {
		printk(KERN_ERR "Reading sb failed; rc = [%d]\n", rc);
		goto out_abort;
	}
	goto out;
out_abort:
	dput(sb->s_root);
	up_write(&sb->s_umount);
	deactivate_super(sb);
out:
	return rc;
}

/**
 * ecryptfs_kill_block_super
 * @sb: The ecryptfs super block
 *
 * Used to bring the superblock down and free the private data.
 * Private data is free'd in ecryptfs_put_super()
 */
static void ecryptfs_kill_block_super(struct super_block *sb)
{
	generic_shutdown_super(sb);
}

static struct file_system_type ecryptfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "ecryptfs",
	.get_sb = ecryptfs_get_sb,
	.kill_sb = ecryptfs_kill_block_super,
	.fs_flags = 0
};

/**
 * inode_info_init_once
 *
 * Initializes the ecryptfs_inode_info_cache when it is created
 */
static void
inode_info_init_once(void *vptr, struct kmem_cache *cachep, unsigned long flags)
{
	struct ecryptfs_inode_info *ei = (struct ecryptfs_inode_info *)vptr;

	inode_init_once(&ei->vfs_inode);
}

static struct ecryptfs_cache_info {
	struct kmem_cache **cache;
	const char *name;
	size_t size;
	void (*ctor)(void *obj, struct kmem_cache *cache, unsigned long flags);
} ecryptfs_cache_infos[] = {
	{
		.cache = &ecryptfs_auth_tok_list_item_cache,
		.name = "ecryptfs_auth_tok_list_item",
		.size = sizeof(struct ecryptfs_auth_tok_list_item),
	},
	{
		.cache = &ecryptfs_file_info_cache,
		.name = "ecryptfs_file_cache",
		.size = sizeof(struct ecryptfs_file_info),
	},
	{
		.cache = &ecryptfs_dentry_info_cache,
		.name = "ecryptfs_dentry_info_cache",
		.size = sizeof(struct ecryptfs_dentry_info),
	},
	{
		.cache = &ecryptfs_inode_info_cache,
		.name = "ecryptfs_inode_cache",
		.size = sizeof(struct ecryptfs_inode_info),
		.ctor = inode_info_init_once,
	},
	{
		.cache = &ecryptfs_sb_info_cache,
		.name = "ecryptfs_sb_cache",
		.size = sizeof(struct ecryptfs_sb_info),
	},
	{
		.cache = &ecryptfs_header_cache_1,
		.name = "ecryptfs_headers_1",
		.size = PAGE_CACHE_SIZE,
	},
	{
		.cache = &ecryptfs_header_cache_2,
		.name = "ecryptfs_headers_2",
		.size = PAGE_CACHE_SIZE,
	},
	{
		.cache = &ecryptfs_xattr_cache,
		.name = "ecryptfs_xattr_cache",
		.size = PAGE_CACHE_SIZE,
	},
	{
		.cache = &ecryptfs_key_record_cache,
		.name = "ecryptfs_key_record_cache",
		.size = sizeof(struct ecryptfs_key_record),
	},
	{
		.cache = &ecryptfs_key_sig_cache,
		.name = "ecryptfs_key_sig_cache",
		.size = sizeof(struct ecryptfs_key_sig),
	},
	{
		.cache = &ecryptfs_global_auth_tok_cache,
		.name = "ecryptfs_global_auth_tok_cache",
		.size = sizeof(struct ecryptfs_global_auth_tok),
	},
	{
		.cache = &ecryptfs_key_tfm_cache,
		.name = "ecryptfs_key_tfm_cache",
		.size = sizeof(struct ecryptfs_key_tfm),
	},
	{
		.cache = &ecryptfs_open_req_cache,
		.name = "ecryptfs_open_req_cache",
		.size = sizeof(struct ecryptfs_open_req),
	},
};

static void ecryptfs_free_kmem_caches(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ecryptfs_cache_infos); i++) {
		struct ecryptfs_cache_info *info;

		info = &ecryptfs_cache_infos[i];
		if (*(info->cache))
			kmem_cache_destroy(*(info->cache));
	}
}

/**
 * ecryptfs_init_kmem_caches
 *
 * Returns zero on success; non-zero otherwise
 */
static int ecryptfs_init_kmem_caches(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ecryptfs_cache_infos); i++) {
		struct ecryptfs_cache_info *info;

		info = &ecryptfs_cache_infos[i];
		*(info->cache) = kmem_cache_create(info->name, info->size,
						   0, SLAB_HWCACHE_ALIGN,
						   info->ctor, NULL);
		if (!*(info->cache)) {
			ecryptfs_free_kmem_caches();
			ecryptfs_printk(KERN_WARNING, "%s: "
					"kmem_cache_create failed\n",
					info->name);
			return -ENOMEM;
		}
	}
	return 0;
}

static decl_subsys(ecryptfs, NULL, NULL);

static ssize_t version_show(struct subsystem *subsys, char *buff)
{
	return snprintf(buff, PAGE_SIZE, "%d\n", ECRYPTFS_VERSIONING_MASK);
}

static struct subsys_attribute version_attr = __ATTR_RO(version);

static struct attribute *attributes[] = {
	&version_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attributes,
};

static int do_sysfs_registration(void)
{
	int rc;

	rc = subsystem_register(&ecryptfs_subsys);
	if (rc) {
		printk(KERN_ERR
		       "Unable to register ecryptfs sysfs subsystem\n");
		goto out;
	}
	rc = sysfs_create_group(&ecryptfs_subsys.kset.kobj, &attr_group);
	if (rc) {
		printk(KERN_ERR
	 	      "Unable to create ecryptfs version attributes\n");
		subsystem_unregister(&ecryptfs_subsys);
	}
out:
	return rc;
}

static void do_sysfs_unregistration(void)
{
	sysfs_remove_group(&ecryptfs_subsys.kset.kobj, &attr_group);
	subsystem_unregister(&ecryptfs_subsys);
}

static int __init ecryptfs_init(void)
{
	int rc;

	if (ECRYPTFS_DEFAULT_EXTENT_SIZE > PAGE_CACHE_SIZE) {
		rc = -EINVAL;
		ecryptfs_printk(KERN_ERR, "The eCryptfs extent size is "
				"larger than the host's page size, and so "
				"eCryptfs cannot run on this system. The "
				"default eCryptfs extent size is [%d] bytes; "
				"the page size is [%d] bytes.\n",
				ECRYPTFS_DEFAULT_EXTENT_SIZE, PAGE_CACHE_SIZE);
		goto out;
	}
	rc = ecryptfs_init_kmem_caches();
	if (rc) {
		printk(KERN_ERR
		       "Failed to allocate one or more kmem_cache objects\n");
		goto out;
	}
	rc = register_filesystem(&ecryptfs_fs_type);
	if (rc) {
		printk(KERN_ERR "Failed to register filesystem\n");
		goto out_free_kmem_caches;
	}
	kobj_set_kset_s(&ecryptfs_subsys.kset, fs_subsys);
	rc = do_sysfs_registration();
	if (rc) {
		printk(KERN_ERR "sysfs registration failed\n");
		goto out_unregister_filesystem;
	}
	rc = ecryptfs_init_kthread();
	if (rc) {
		printk(KERN_ERR "%s: kthread initialization failed; "
		       "rc = [%d]\n", __func__, rc);
		goto out_do_sysfs_unregistration;
	}
	rc = ecryptfs_init_messaging(ecryptfs_transport);
	if (rc) {
		printk(KERN_ERR "Failure occured while attempting to "
				"initialize the eCryptfs netlink socket\n");
		goto out_destroy_kthread;
	}
	rc = ecryptfs_init_crypto();
	if (rc) {
		printk(KERN_ERR "Failure whilst attempting to init crypto; "
		       "rc = [%d]\n", rc);
		goto out_release_messaging;
	}
	if (ecryptfs_verbosity > 0)
		printk(KERN_CRIT "eCryptfs verbosity set to %d. Secret values "
			"will be written to the syslog!\n", ecryptfs_verbosity);

	goto out;
out_release_messaging:
	ecryptfs_release_messaging(ecryptfs_transport);
out_destroy_kthread:
	ecryptfs_destroy_kthread();
out_do_sysfs_unregistration:
	do_sysfs_unregistration();
out_unregister_filesystem:
	unregister_filesystem(&ecryptfs_fs_type);
out_free_kmem_caches:
	ecryptfs_free_kmem_caches();
out:
	return rc;
}

static void __exit ecryptfs_exit(void)
{
	int rc;

	rc = ecryptfs_destroy_crypto();
	if (rc)
		printk(KERN_ERR "Failure whilst attempting to destroy crypto; "
		       "rc = [%d]\n", rc);
	ecryptfs_release_messaging(ecryptfs_transport);
	ecryptfs_destroy_kthread();
	do_sysfs_unregistration();
	unregister_filesystem(&ecryptfs_fs_type);
	ecryptfs_free_kmem_caches();
}

MODULE_AUTHOR("Michael A. Halcrow <mhalcrow@us.ibm.com>");
MODULE_DESCRIPTION("eCryptfs");

MODULE_LICENSE("GPL");

module_init(ecryptfs_init)
module_exit(ecryptfs_exit)
