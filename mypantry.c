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
	char *data_buf;

	// stuff for iterating through data block
	struct pantryfs_dir_entry *pfs_dentry;
	size_t DENTRY_SIZE;
	int i;
	int res;

	DENTRY_SIZE = sizeof(struct pantryfs_dir_entry);

	/* check that ctx-pos isn't too big */
	if (ctx->pos > PFS_MAX_CHILDREN + 2)
		return 0;

	/* try to emit . and .. */
	res = dir_emit_dots(dir, ctx);
	if (!res)
		return 0;

	/* retrieve the dir inode from the file struct */
	dir_inode = file_inode(dir);

	/* check that we're root */
	if (dir_inode->i_ino != PANTRYFS_ROOT_INODE_NUMBER) {
		ret = -EPERM;
		goto iterate_end;
	}

	/* read the root dir inode from disk */
	data_buf = kmalloc(PFS_BLOCK_SIZE, GFP_KERNEL);
	if (!data_buf) {
		pr_err("Malloc failed!");
		ret = -ENOMEM;
		goto iterate_end;
	}
	sb = dir_inode->i_sb;
	bh = sb_bread(sb, PANTRYFS_ROOT_DATABLOCK_NUMBER);
	if (!bh) {
		pr_err("Could not read dir block");
		ret = -EIO;
		goto iterate_free;
	}
	memcpy(data_buf, bh->b_data, PFS_BLOCK_SIZE);

	/* read through data buf dentries */
	// for (i = 0; i < PFS_MAX_CHILDREN; i++) {
	for (i = 0; i < 1; i++) {
		pfs_dentry = (struct pantryfs_dir_entry *) data_buf + (i * DENTRY_SIZE);

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
iterate_free:
	kfree(data_buf);
iterate_end:
	return ret;
}

ssize_t pantryfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	return -EPERM;
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

struct dentry *pantryfs_lookup(struct inode *parent, struct dentry *child_dentry,
		unsigned int flags)
{
	return NULL;
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
	char inode_buf[sizeof(struct pantryfs_inode)];
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
	memcpy(inode_buf, buf_heads.i_store_bh->b_data, sizeof(struct pantryfs_inode));
	pfs_root_inode = (struct pantryfs_inode *) inode_buf;
	root_inode->i_private = pfs_root_inode;
	root_inode->i_sb = sb; // is this the right sb?



fill_super_release_both:
	brelse(buf_heads.i_store_bh);
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
