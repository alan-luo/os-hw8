#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "pantryfs_inode.h"
#include "pantryfs_inode_ops.h"
#include "pantryfs_file.h"
#include "pantryfs_file_ops.h"
#include "pantryfs_sb.h"
#include "pantryfs_sb_ops.h"

#define PFS_DENTRY_SIZE sizeof(struct pantryfs_dir_entry)

uint64_t PFS_datablock_no_from_inode(struct inode *inode) {
	return PANTRYFS_ROOT_DATABLOCK_NUMBER + (inode->i_ino - 1);
}

/* P3: implement `iterate()` */
int pantryfs_iterate(struct file *filp, struct dir_context *ctx)
{
	// basic setup
	int ret = 0;
	struct file *dir = filp; // filp points at dir

	// stuff to read the dir block
	struct inode *dir_inode;
	struct super_block *sb;
	struct buffer_head *bh;

	// stuff for iterating through data block
	struct pantryfs_dir_entry *pfs_dentry;
	int i;
	int res;

	/* check that ctx-pos isn't too big */
	if (ctx->pos > PFS_MAX_CHILDREN + 2)
		return 0;

	/* try to emit . and .. */
	res = dir_emit_dots(dir, ctx);
	if (!res)
		return 0;

	/* retrieve the dir inode from the file struct */
	dir_inode = file_inode(dir);

	/* read the root dir inode from disk */
	sb = dir_inode->i_sb;
	bh = sb_bread(sb, PFS_datablock_no_from_inode(dir_inode));
	if (!bh) {
		pr_err("Could not read dir block");
		ret = -EIO;
		goto iterate_end;
	}

	/* read through data buf dentries */
	for (i = 0; i < PFS_MAX_CHILDREN; i++) {
		pfs_dentry = (struct pantryfs_dir_entry *)
			(bh->b_data + (i * PFS_DENTRY_SIZE));

		// This flag is sufficent to check for
		// * A) if the dentry is dead / lazily deleted
		// * B) if the end of the dentry list has been reached - since it's
		//   zeroed out, dentry->active is also zero
		if (!pfs_dentry->active)
			continue;

		res = dir_emit(ctx, pfs_dentry->filename, 2 * PANTRYFS_FILENAME_BUF_SIZE,
			pfs_dentry->inode_no, DT_UNKNOWN);
		if (!res)
			break;

		ctx->pos++;
	}
	// if we've made it to the end of the loop, make sure we terminate next time
	ctx->pos = PFS_MAX_CHILDREN + 3;


	brelse(bh);
iterate_end:
	return ret;
}

/* P5: implement read */
ssize_t pantryfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	// basic
	ssize_t ret = 0;
	// read inode data block
	struct inode *inode;
	struct buffer_head *bh;
	size_t amt_to_read;

	/* check if offset is valid */
	if (*offset == PFS_BLOCK_SIZE)
		return 0;
	else if (*offset > PFS_BLOCK_SIZE) {
		pr_info("Offset larger than 4096 bytes (block size)");
		return -EINVAL;
	}

	/* get inode # from file pointer */
	inode = file_inode(filp);

	/* read data block corresponding to inode */
	bh = sb_bread(inode->i_sb, PFS_datablock_no_from_inode(inode));
	if (!bh) {
		pr_err("Could not read file datablock");
		ret = -EIO;
		goto read_end;
	}

	/* copy data from data block */
	if (len + *offset > PFS_BLOCK_SIZE)
		amt_to_read = PFS_BLOCK_SIZE - *offset;
	else
		amt_to_read = len;


	if (copy_to_user(buf, bh->b_data + *offset, amt_to_read)) {
		pr_err("Copy_to_user failed");
		ret = -EFAULT;
		goto read_release;
	}

	*offset	+= amt_to_read;
	ret = amt_to_read;


read_release:
	brelse(bh);
read_end:
	return ret;
}

loff_t pantryfs_llseek(struct file *filp, loff_t offset, int whence)
{
	return -EPERM;
}

int pantryfs_create(struct inode *parent, struct dentry *dentry, umode_t mode, bool excl)
{
	return -EPERM;
}

int pantryfs_unlink(struct inode *dir, struct dentry *dentry)
{
	return -EPERM;
}

int pantryfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	return -EPERM;
}

void pantryfs_evict_inode(struct inode *inode)
{
	/* Required to be called by VFS. If not called, evict() will BUG out.*/
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

int pantryfs_fsync(struct file *filp, loff_t start, loff_t end, int datasync)
{
	return -EPERM;
}

ssize_t pantryfs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
	return -EPERM;
}


/* P4: implement subdir lookup */
struct dentry *pantryfs_lookup(struct inode *parent, struct dentry *child_dentry,
		unsigned int flags)
{
	// setup
	struct dentry *ret = NULL;
	struct super_block *sb;
	// read directory data from disk
	struct buffer_head *istore_bh;
	struct pantryfs_inode *pfs_parent_inode;
	struct buffer_head *pardir_bh;
	// iterate through parent dir
	struct pantryfs_dir_entry *pfs_dentry;
	struct pantryfs_dir_entry *dir_dentry;
	int i;
	// store and cache
	struct inode *dd_inode = NULL;
	struct pantryfs_inode *dd_pfs_inode;
	
	sb = parent->i_sb;

	pr_info("parent inode #: %u", parent->i_ino);
	pr_info("child dentry name: %s", child_dentry->d_name.name);

	/* check filename length */
	if (child_dentry->d_name.len > PANTRYFS_MAX_FILENAME_LENGTH) {
		pr_err("File name too long");
		ret = ERR_PTR(-ENAMETOOLONG);
		goto lookup_end;
	}

	/* check if we have the dentry in the cache. if so, return it */

	// Testing indicates that we don't need this (subsequent calls automatically
	// cached) but I don't know why.

	// struct dentry *found_dentry;

	// d_lookup(const struct dentry *parent, const struct qstr *name): 
	// - if the dentry is found its reference count is incremented and the dentry is returned.
	// - NULL is returned if the dentry does not exist.
	// https://elixir.bootlin.com/linux/v5.10.158/source/fs/dcache.c#L2328
	// found_dentry = d_lookup(parent, child_dentry->d_name);
	// if (found_dentry) {
	// 	// store and return the dentry we just found
	// 	d_add(child_dentry, found_dentry->d_inode);
	// 	return found_dentry;
	// }

	/* otherwise...*/

	/* get datablock number from inode number */
	// - read inode store from disk
	istore_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!istore_bh) {
		pr_err("Could not read inode block\n");
		ret = ERR_PTR(-EIO);
		goto lookup_end;
	}

	// - read PFS inode entry from inode #
	pfs_parent_inode = (struct pantryfs_inode *) 
		(istore_bh->b_data + (parent->i_ino - 1) * sizeof(struct pantryfs_inode));
	/* read directory block from disk */
	pardir_bh = sb_bread(sb, pfs_parent_inode->data_block_number);
	if (!pardir_bh) {
		pr_err("Could not read pardir block\n");
		ret = ERR_PTR(-EIO);
		goto lookup_end;
	}

	/* look for dentry in data block */
	dir_dentry = NULL;
	for (i = 0; i < PFS_MAX_CHILDREN; i++) {
		pfs_dentry = (struct pantryfs_dir_entry *)
			(pardir_bh->b_data + (i * PFS_DENTRY_SIZE));

		if (!pfs_dentry->active)
			continue;

		// if we found a match
		if(!strncmp(pfs_dentry->filename, child_dentry->d_name.name, 
				PANTRYFS_FILENAME_BUF_SIZE)) {
			dir_dentry = pfs_dentry;
			break;	
		}
	}
	// if no match was found
	if (!dir_dentry) 
		goto lookup_release;
	
	// otherwise...

	/* store and cache the entry we just found */

	// get inode information
	dd_inode = iget_locked(sb, le64_to_cpu(dir_dentry->inode_no));
	if(!dd_inode) {
		pr_err("Could not allocate dir dentry inode")	;
		ret = ERR_PTR(-ENOMEM);
		goto lookup_release;
	}
	dd_pfs_inode = (struct pantryfs_inode *) 
		(istore_bh->b_data + (dir_dentry->inode_no - 1) * sizeof(struct pantryfs_inode));
	if (dd_inode->i_state & I_NEW) {
		dd_inode->i_sb = sb;
		dd_inode->i_ino = dir_dentry->inode_no;
		dd_inode->i_op = &pantryfs_inode_ops;	
		dd_inode->i_mode = dd_pfs_inode->mode;

		if (dd_pfs_inode->mode & S_IFDIR) {
			dd_inode->i_fop = &pantryfs_dir_ops;
			dd_inode->i_mode = 0777 | S_IFDIR;
		} else {
			dd_inode->i_fop = &pantryfs_file_ops;
			dd_inode->i_mode = 0666 | S_IFREG;
		}
		unlock_new_inode(dd_inode);
	}
	// now finally add it
	d_add(child_dentry, dd_inode);

lookup_release:
	brelse(istore_bh);
lookup_end:
	// Tal has a note on this but I don't quite understand it?
	// if (ret != NULL) { // as of now this only happens on error
	// 	return d_splice_alias(dd_inode, child_dentry);
	// }
	return ret;
}

int pantryfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	return -EPERM;
}

int pantryfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	return -EPERM;
}

int pantryfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	return -EPERM;
}

int pantryfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	return -EPERM;
}

const char *pantryfs_get_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done)
{
	return ERR_PTR(-EPERM);
}

/**
 * Called by VFS to free an inode. free_inode_nonrcu() must be called to free
 * the inode in the default manner.
 *
 * @inode:	The inode that will be free'd by VFS.
 */
void pantryfs_free_inode(struct inode *inode)
{
	free_inode_nonrcu(inode);
}

/* P2: implement this to make mount/umount work */
int pantryfs_fill_super(struct super_block *sb, void *data, int silent)
{
	int ret = 0;

	// P2: for sb init and mounting
	struct pantryfs_sb_buffer_heads buf_heads;
	struct pantryfs_super_block *pantry_sb;
	struct inode *root_inode;

	// P3: for reading inodes from PantryFS
	char *inode_buf;
	struct pantryfs_inode *pfs_root_inode;

	/* initialize super block */
	sb->s_magic = PANTRYFS_MAGIC_NUMBER;
	sb_set_blocksize(sb, PFS_BLOCK_SIZE); // sets s_blocksize, s_blocksize_bits
	sb->s_maxbytes = PFS_BLOCK_SIZE;
	sb->s_op = &pantryfs_sb_ops;

	/* read superblock from disk */
	buf_heads.sb_bh = sb_bread(sb, PANTRYFS_SUPERBLOCK_DATABLOCK_NUMBER);
	if (!buf_heads.sb_bh) {
		pr_err("Could not read super block\n");
		ret = -EIO;
		goto fill_super_end;
	}

	pantry_sb = (struct pantryfs_super_block *) buf_heads.sb_bh->b_data;

	// - check magic number
	if (sb->s_magic != pantry_sb->magic) {
		pr_err("Wrong magic number\n");
		ret = -EINVAL;
		goto fill_super_release;
	}

	/* read inode block from disk */
	buf_heads.i_store_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!buf_heads.i_store_bh) {
		pr_err("Could not read inode block\n");
		ret = -EIO;
		goto fill_super_end;
	}
	
	/* create VFS inode for root directory */
	root_inode = iget_locked(sb, 0);
	if (!root_inode) {
		pr_err("Could not allocate root inode\n");
		ret = -ENOMEM;
		goto fill_super_release_both;
	}
	// Not entirely sure if this is necessary but I added it to stay consistent
	// with kernel code (which pretty much always checks this)
	if (!(root_inode->i_state & I_NEW)) {
		pr_err("Something weird happened");
		ret = -EPERM;
		goto fill_super_release_both;
	}

	root_inode->i_ino = PANTRYFS_ROOT_INODE_NUMBER;
	root_inode->i_mode = 0777 | S_IFDIR; // make root drwx-rwx-rwx
	root_inode->i_op = &pantryfs_inode_ops;
	root_inode->i_fop = &pantryfs_dir_ops;



	/* create dentry for root inode */
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		pr_err("Could not allocate root dentry");
		ret = -ENOMEM;
		goto fill_super_release_both;
	}

	/* P3: read PantryFS root inode from disk and associate it with root_inode */
	// Not sure if we strictly have to do this - but I don't know if we can guarantee
	// that buffer heads will stick around, so this seems reasonable
	inode_buf = kmalloc(sizeof(struct pantryfs_inode), GFP_KERNEL);

	memcpy(inode_buf, buf_heads.i_store_bh->b_data, sizeof(struct pantryfs_inode));
	pfs_root_inode = (struct pantryfs_inode *) inode_buf;
	root_inode->i_private = pfs_root_inode;
	root_inode->i_sb = sb; // is this the right sb?



fill_super_release_both:
	brelse(buf_heads.i_store_bh);
	unlock_new_inode(root_inode);
fill_super_release:
	brelse(buf_heads.sb_bh);

fill_super_end:
	return ret;
}

static struct dentry *pantryfs_mount(struct file_system_type *fs_type, int flags,
		const char *dev_name, void *data)
{
	struct dentry *ret;

	/* mount_bdev is "mount block device". */
	ret = mount_bdev(fs_type, flags, dev_name, data, pantryfs_fill_super);

	if (IS_ERR(ret))
		pr_err("Error mounting mypantryfs");
	else
		pr_info("Mounted mypantryfs on [%s]\n", dev_name);

	return ret;
}

static void pantryfs_kill_superblock(struct super_block *sb)
{
	// make sure to free i_sb from the root node

	kill_block_super(sb);
	pr_info("mypantryfs superblock destroyed. Unmount successful.\n");
}

struct file_system_type pantryfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "mypantryfs",
	.mount = pantryfs_mount,
	.kill_sb = pantryfs_kill_superblock,
};

static int pantryfs_init(void)
{
	int ret;

	ret = register_filesystem(&pantryfs_fs_type);
	if (likely(ret == 0))
		pr_info("Successfully registered mypantryfs\n");
	else
		pr_err("Failed to register mypantryfs. Error:[%d]", ret);

	return ret;
}

static void pantryfs_exit(void)
{
	int ret;

	ret = unregister_filesystem(&pantryfs_fs_type);

	if (likely(ret == 0))
		pr_info("Successfully unregistered mypantryfs\n");
	else
		pr_err("Failed to unregister mypantryfs. Error:[%d]", ret);
}

module_init(pantryfs_init);
module_exit(pantryfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group N");
