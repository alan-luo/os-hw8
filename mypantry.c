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
#define PFS_INODE_SIZE sizeof(struct pantryfs_inode)

/* Helper function to get a pointer from the istore buffer for a particular ino # */
struct pantryfs_inode *PFS_inode_from_istore(struct buffer_head *istore_bh, unsigned long ino) {
	return (struct pantryfs_inode *) 
		(istore_bh->b_data + (ino - 1) * PFS_INODE_SIZE);
}
uint64_t PFS_datablock_no_from_inode(struct buffer_head *istore_bh, struct inode *inode) {
	struct pantryfs_inode *disk_inode = PFS_inode_from_istore(istore_bh, inode->i_ino);
	return disk_inode->data_block_number;
}
/* Helper function to get a pointer to a particular dentry given:
- directory data block
- index */
struct pantryfs_dir_entry *PFS_dentry_from_dirblock(struct buffer_head *dir_bh, unsigned int i) {
	return (struct pantryfs_dir_entry *)
		(dir_bh->b_data + (i * PFS_DENTRY_SIZE));
}

/* Helper functions used by create and mkdir */
struct i_db_no {
	unsigned long i_no;
	unsigned long db_no;
};
// returns -1 on error
struct i_db_no PFS_get_free_i_db_no(struct buffer_head *sb_bh) {
	struct pantryfs_super_block *pantry_sb;
	struct i_db_no ret;
	int i;

	pantry_sb = (struct pantryfs_super_block *) sb_bh->b_data;

	ret.i_no = -1;
	ret.db_no = -1;

	// find a free inode
	for (i = 0; i < PFS_MAX_INODES; i++) {
		if (!IS_SET(pantry_sb->free_inodes, i)) {
			ret.i_no = i;
			break;
		}
	}

	// find a data block inode
	for (i = 0; i < PFS_MAX_INODES; i++) {
		if (!IS_SET(pantry_sb->free_data_blocks, i)) {
			ret.db_no = i+1; // inode 1 is datablock 2
			break;
		}
	}

	return ret;
}

// returns NULL on failure
struct pantryfs_dir_entry *PFS_next_empty_dentry(struct buffer_head *par_bh) {
	int i;
	struct pantryfs_dir_entry *pfs_dentry;
	struct pantryfs_dir_entry *ret_dentry = NULL;

	for (i = 0; i < PFS_MAX_CHILDREN; i++) {
		pfs_dentry = PFS_dentry_from_dirblock(par_bh, i);
		if (!pfs_dentry->active) {
			ret_dentry = pfs_dentry;
			break;
		}
	}

	pr_info("Found this empty dentry: %lu", i);
	return ret_dentry;
}

/* P6: helper function used to create new inodes in a consistent way */
// Currently used in fill_super (root inode) and lookup (inode cache)
struct inode *pfs_inode(struct super_block *sb, unsigned long ino, struct pantryfs_inode *pfs_inode) {
	struct inode *inode;
	int isroot;

	isroot = ino == PANTRYFS_ROOT_INODE_NUMBER;

	inode = iget_locked(sb, le64_to_cpu(0));
	if (!inode) {
		pr_err("Could not allocate inode\n");
		return NULL;
	}
	if (!(inode->i_state & I_NEW))
		return inode;

	// universal to all inodes
	inode->i_sb = sb;
	inode->i_op = &pantryfs_inode_ops;
	inode->i_blocks = 1;

	// set this particular inode's values
	inode->i_ino = ino;
	if (isroot)
		inode->i_mode = 0777 | S_IFDIR; // make root drwx-rwx-rwx
	else
		inode->i_mode = le64_to_cpu(pfs_inode->mode);

	set_nlink(inode, pfs_inode->nlink);
	i_uid_write(inode, le64_to_cpu(pfs_inode->uid));
	i_gid_write(inode, le64_to_cpu(pfs_inode->gid));

	inode->i_atime = pfs_inode->i_atime;
	inode->i_mtime = pfs_inode->i_mtime;
	inode->i_ctime = pfs_inode->i_ctime;

	if (pfs_inode->mode & S_IFDIR || isroot) {
		inode->i_fop = &pantryfs_dir_ops;
		inode->i_size = PFS_BLOCK_SIZE;
	} else {
		inode->i_fop = &pantryfs_file_ops;
		inode->i_size = pfs_inode->file_size;
	}

	unlock_new_inode(inode);

	return inode;
}

// Remove inode from disk: unset the bit vector, remove from i store
void PFS_remove_inode(struct buffer_head *sb_bh, struct buffer_head *istore_bh, struct inode *inode) {
	unsigned long ino = inode->i_ino;
	struct pantryfs_super_block *pantry_sb;
	unsigned long db_no;
	struct pantryfs_inode *pfs_inode;

	db_no = PFS_datablock_no_from_inode(istore_bh, inode);


	pr_info("removing inode:");
	pr_info("ino: %lu", ino);
	pr_info("dbno: %lu", db_no);
	pr_info("\n");

	if (db_no == 0) {
		pr_info("already removed\n");
		return;
	}

	// Unset bit vectors in sb
	pantry_sb = (struct pantryfs_super_block *) sb_bh->b_data;

	CLEARBIT(pantry_sb->free_inodes, ino);
	CLEARBIT(pantry_sb->free_data_blocks, db_no);

	mark_buffer_dirty(sb_bh);
	sync_dirty_buffer(sb_bh);

	remove_inode_hash(inode);
}

/* P3: implement `iterate()` */
int pantryfs_iterate(struct file *filp, struct dir_context *ctx)
{
	// basic setup
	int ret = 0;
	struct file *dir = filp; // filp points at dir
	struct buffer_head *istore_bh;
	// stuff to read the dir block
	struct inode *dir_inode;
	struct super_block *sb;
	struct buffer_head *bh;
	// stuff for iterating through data block
	struct pantryfs_dir_entry *pfs_dentry;
	int i;
	int res;

	/* retrieve the dir inode from the file struct */
	dir_inode = file_inode(dir);
	sb = dir_inode->i_sb;

	/* check that ctx-pos isn't too big */
	if (ctx->pos > PFS_MAX_CHILDREN + 2)
		return 0;

	/* try to emit . and .. */
	res = dir_emit_dots(dir, ctx);
	if (!res)
		return 0;

	/* get istore bh */
	istore_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!istore_bh) {
		pr_err("Could not read from inode store");
		ret = -EIO;
		goto iterate_end;
	}


	/* read the dir datablock from disk */
	bh = sb_bread(sb, PFS_datablock_no_from_inode(istore_bh, dir_inode));
	if (!bh) {
		pr_err("Could not read dir block");
		ret = -EIO;
		goto iterate_release_i;
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

		pr_info("%s #%lu: block %u", pfs_dentry->filename, pfs_dentry->inode_no, PFS_inode_from_istore(istore_bh, pfs_dentry->inode_no)->data_block_number);
		res = dir_emit(ctx, pfs_dentry->filename, 2 * PANTRYFS_FILENAME_BUF_SIZE,
			pfs_dentry->inode_no, S_DT(dir_inode->i_mode));
		if (!res)
			break;

		ctx->pos++;
	}
	// if we've made it to the end of the loop, make sure we terminate next time
	ctx->pos = PFS_MAX_CHILDREN + 3;


	brelse(bh);
iterate_release_i:
	brelse(istore_bh);
iterate_end:
	return ret;
}

/* P5: implement read */
ssize_t pantryfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	// basic
	ssize_t ret = 0;
	struct buffer_head *istore_bh;
	struct super_block *sb;
	// read inode data block
	struct inode *inode;
	struct buffer_head *bh;
	size_t amt_to_read;

	/* get inode # from file pointer */
	inode = file_inode(filp);
	sb = inode->i_sb;

	/* check if offset is valid */
	if (*ppos == PFS_BLOCK_SIZE)
		return 0;
	else if (*ppos > PFS_BLOCK_SIZE) {
		pr_err("Offset larger than 4096 bytes (block size)");
		return -EINVAL;
	}

	/* get istore bh */
	istore_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!istore_bh) {
		pr_err("Could not read from inode store");
		ret = -EIO;
		goto read_end;
	}


	/* read data block corresponding to inode */
	bh = sb_bread(inode->i_sb, PFS_datablock_no_from_inode(istore_bh, inode));
	if (!bh) {
		pr_err("Could not read file datablock");
		ret = -EIO;
		goto read_release_i;
	}

	/* copy data from data block */
	if (len + *ppos > PFS_BLOCK_SIZE)
		amt_to_read = PFS_BLOCK_SIZE - *ppos;
	else
		amt_to_read = len;


	if (copy_to_user(buf, bh->b_data + *ppos, amt_to_read)) {
		pr_err("Copy_to_user failed");
		ret = -EFAULT;
		goto read_release;
	}

	*ppos += amt_to_read;
	ret = amt_to_read;


read_release:
	brelse(bh);
read_release_i:
	brelse(istore_bh);
read_end:
	return ret;
}

/* P5: also implement this */

// Helpful: generic_file_llseek https://elixir.bootlin.com/linux/v5.10.158/source/fs/read_write.c#L144
loff_t pantryfs_llseek(struct file *filp, loff_t offset, int whence)
{
	return generic_file_llseek(filp, offset, whence);
}

/* P8: create files */
int pantryfs_create(struct inode *parent, struct dentry *dentry, umode_t mode, bool excl)
{
	// basic	
	int ret = 0;
	struct super_block *sb = parent->i_sb;
	struct pantryfs_sb_buffer_heads buf_heads;
	// for reading super block
	struct pantryfs_super_block *pantry_sb;
	// new inode info
	struct i_db_no new_i_db_no;
	struct pantryfs_inode *pfs_new_inode;
	// for opening parent data block
	struct buffer_head *par_bh;
	struct pantryfs_dir_entry *pfs_dentry;
	int new_dentry_no;

	/* 1. Open super block and tell it that a new file and inode have been created */
	buf_heads.sb_bh = sb_bread(sb, PANTRYFS_SUPERBLOCK_DATABLOCK_NUMBER);
	if (!buf_heads.sb_bh) {
		pr_err("Could not read super block");
		ret = -EIO;
		goto create_end;
	}

	new_i_db_no = PFS_get_free_i_db_no(buf_heads.sb_bh);

	if (new_i_db_no.db_no == -1 || new_i_db_no.i_no == -1) {
		pr_err("Could not find a free inode or data block!");
		goto create_end;
	}

	/* 2. Open inode block*/
	buf_heads.i_store_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!buf_heads.i_store_bh) {
		pr_err("Could not read i store block");
		ret = -EIO;
		goto create_release;
	}

	/* 3. Open data block for parent and add dentry */
	par_bh = sb_bread(sb, PFS_datablock_no_from_inode(buf_heads.i_store_bh, parent));
	if (!par_bh) {
		pr_err("Could not read parent dir datablock");
		ret = -EIO;
		goto create_release_2;
	}

	// Get first empty dentry in dirblock
	pfs_dentry = PFS_next_empty_dentry(par_bh);
	if (pfs_dentry == NULL) {
		pr_err("Could not find a free dentry");
		goto create_release_3;
	}

	pr_info("Found this empty inode: %lu", new_i_db_no.i_no);
	pr_info("Found this empty data block: %lu", new_i_db_no.db_no);


	/* 4. Now, write out all information */

	// write sb - mark inode and datablock entries as used
	pantry_sb = (struct pantryfs_super_block *) buf_heads.sb_bh->b_data;
	SETBIT(pantry_sb->free_inodes, new_i_db_no.i_no);
	SETBIT(pantry_sb->free_data_blocks, new_i_db_no.db_no - 1);

	mark_buffer_dirty(buf_heads.sb_bh);
	sync_dirty_buffer(buf_heads.sb_bh);

	// write istore
	pfs_new_inode = PFS_inode_from_istore(buf_heads.i_store_bh, new_i_db_no.i_no);

	pfs_new_inode->nlink = 1;
	pfs_new_inode->mode = S_IFREG | 0666;
	pfs_new_inode->data_block_number = new_i_db_no.db_no;
	pfs_new_inode->file_size = 0;

	pfs_new_inode->uid = parent->i_uid.val;
	pfs_new_inode->gid = parent->i_gid.val;

	pfs_new_inode->i_atime = current_time(parent);
	pfs_new_inode->i_mtime = pfs_new_inode->i_atime;
	pfs_new_inode->i_ctime = pfs_new_inode->i_atime;

	mark_buffer_dirty(buf_heads.i_store_bh);
	sync_dirty_buffer(buf_heads.i_store_bh);

	// write dentry

	pfs_dentry->inode_no = new_i_db_no.i_no;
	pfs_dentry->active = 1;
	strncpy(pfs_dentry->filename, dentry->d_name.name, sizeof(pfs_dentry->filename));

	mark_buffer_dirty(par_bh);
	sync_dirty_buffer(par_bh);

	/* 4. Open data block for newly_created file and zero it out */

create_release_3:
	brelse(par_bh);
create_release_2:
	brelse(buf_heads.i_store_bh);
create_release:
	brelse(buf_heads.sb_bh);
create_end:
	return ret;
}

/* P9: remove dentry and decrement link count */
int pantryfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int ret = 0;
	struct super_block *sb = dir->i_sb;
	// read istore and data blocks
	struct pantryfs_sb_buffer_heads buf_heads;
	struct buffer_head *dir_bh;
	// remove dentry
	int i;
	struct pantryfs_dir_entry *pfs_dentry;
	// update links
	struct inode *dentry_inode = dentry->d_inode;
	struct pantryfs_inode *dentry_pfs_inode;

	pr_info("unlinking: %lu", dentry->d_inode->i_ino);
	pr_info("parent: %lu", dir->i_ino);
	pr_info("\n");

	/* read istore block */
	buf_heads.i_store_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!buf_heads.i_store_bh) {
		pr_err("Could not read from inode store");
		ret = -EIO;
		goto unlink_end;
	}

	/* read dir data block */
	dir_bh = sb_bread(sb, PFS_datablock_no_from_inode(buf_heads.i_store_bh, dir));
	if (!dir_bh) {
		pr_err("Could not read parent dir datablock");
		ret = -EIO;
		goto unlink_release;
	}

	/* update dentry within dir data block */
	for (i = 0; i < PFS_MAX_CHILDREN; i++) {
		pfs_dentry = PFS_dentry_from_dirblock(dir_bh, i);
		if (!strcmp(pfs_dentry->filename, dentry->d_name.name))
			break;
	}
	if (i == PFS_MAX_CHILDREN) {
		pr_err("Wasn't able to find a matching dentry!");
		ret = -EFAULT;
		goto unlink_release_2;
	}

	pfs_dentry->active = 0;

	mark_buffer_dirty(dir_bh);
	sync_dirty_buffer(dir_bh);

	/* update inode nlinks */

	// if the file isn't going to be removed
	if (dentry_inode->i_nlink > 1) {
		// first change the node itself
		drop_nlink(dentry_inode);
		// now write to the inode store
		mark_inode_dirty(dentry_inode);

		dentry_pfs_inode = PFS_inode_from_istore(dir_bh, dentry_inode->i_ino);
		dentry_pfs_inode->nlink--;

		mark_buffer_dirty(buf_heads.i_store_bh);
		sync_dirty_buffer(buf_heads.i_store_bh);
	} else {
		buf_heads.sb_bh = sb_bread(sb, PANTRYFS_SUPERBLOCK_DATABLOCK_NUMBER);
		if (!buf_heads.sb_bh) {
			pr_err("Could not read super block");
			ret = -EIO;
			goto unlink_release_3;
		}
		d_delete(dentry);
		PFS_remove_inode(buf_heads.sb_bh, buf_heads.i_store_bh, dentry_inode);

unlink_release_3:
		brelse(buf_heads.sb_bh);
	}

unlink_release_2:
	brelse(dir_bh);
unlink_release:
	brelse(buf_heads.i_store_bh);
unlink_end:
	return ret;
}

/* P7: write_inode back to disk */
int pantryfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	// basic
	int ret = 0;
	struct super_block *sb = inode->i_sb;
	// read inode store data block
	struct pantryfs_inode *disk_inode;
	struct buffer_head *istore_bh;

	/* read istore data block from disk */
	istore_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!istore_bh) {
		pr_err("Could not read from inode store");
		ret = -EIO;
		goto write_inode_end;
	}

	/* get inode struct from istore data block */
	disk_inode = PFS_inode_from_istore(istore_bh, inode->i_ino);

	/* write new inode info to disk_inode */
	disk_inode->mode = inode->i_mode;
	disk_inode->uid = inode->i_uid.val; // don't support this now
	disk_inode->gid = inode->i_gid.val;
	disk_inode->nlink = inode->i_nlink;
	disk_inode->i_atime = inode->i_atime;
	disk_inode->i_mtime = inode->i_mtime;
	disk_inode->i_ctime = inode->i_ctime;
	disk_inode->file_size = inode->i_size;

	mark_buffer_dirty(istore_bh);
	sync_dirty_buffer(istore_bh);

	/* clean up */
	brelse(istore_bh);
write_inode_end:
	return ret;
}

/* P9 */
void pantryfs_evict_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct pantryfs_sb_buffer_heads buf_heads;

	pr_info("evicting: %lu", inode->i_ino);
	pr_info("\n");

	// <------ begin TA code ----->
	/* Required to be called by VFS. If not called, evict() will BUG out.*/
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	// </------ end TA code ----->

	buf_heads.i_store_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!buf_heads.i_store_bh) {
		pr_err("Could not read from inode store");
		return;
	}
	buf_heads.sb_bh = sb_bread(sb, PANTRYFS_SUPERBLOCK_DATABLOCK_NUMBER);
	if (!buf_heads.sb_bh) {
		pr_err("Could not read super block");
		goto evict_release;
	}

	PFS_remove_inode(buf_heads.sb_bh, buf_heads.i_store_bh, inode);

	brelse(buf_heads.sb_bh);
evict_release:
	brelse(buf_heads.i_store_bh);
}

/* P7: implement fsync */
int pantryfs_fsync(struct file *filp, loff_t start, loff_t end, int datasync)
{
	return generic_file_fsync(filp, start, end, datasync);
}

/* P7: implement write */
ssize_t pantryfs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
	// basic
	ssize_t ret = 0;
	int isappend = 0;
	struct super_block *sb;
	// read istore
	struct buffer_head *istore_bh;
	// read inode data block
	struct inode *inode;
	struct buffer_head *bh;
	// write from user buffer
	size_t amt_to_write;

	/* get inode # from file pointer */
	inode = file_inode(filp);
	sb = inode->i_sb;

	/* check if arguments are valid */
	if (*ppos > inode->i_size) {
		pr_err("Offset larger than file size");
		return -EINVAL;
	}

	/* check for O_APPEND flag */
	isappend = filp->f_flags & O_APPEND;
	pr_info("append: %d", isappend);

	if (isappend)
		*ppos = inode->i_size;

	/* get istore bh */
	istore_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!istore_bh) {
		pr_err("Could not read from inode store");
		ret = -EIO;
		goto write_end;
	}

	/* read data block corresponding to inode */
	bh = sb_bread(inode->i_sb, PFS_datablock_no_from_inode(istore_bh, inode));
	if (!bh) {
		pr_err("Could not read file datablock");
		ret = -EIO;
		goto write_release_i;
	}

	/* copy from user buf to data block */
	if (len + *ppos > PFS_BLOCK_SIZE)
		amt_to_write = PFS_BLOCK_SIZE - *ppos;
	else
		amt_to_write = len;

	if(copy_from_user(bh->b_data + *ppos, buf, amt_to_write)) {
		pr_err("copy_from_user failed");
		ret = -EFAULT;
		goto write_release;
	}
	mark_buffer_dirty(bh);

	*ppos += amt_to_write;
	ret = amt_to_write;

	/* write inode if needed*/
	if (*ppos > inode->i_size) {
		i_size_write(inode, *ppos);
		mark_inode_dirty(inode);
	}

write_release:
	brelse(bh);
write_release_i:
	brelse(istore_bh);
write_end:
	return ret;
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
	struct buffer_head *pardir_bh;
	// iterate through parent dir
	struct pantryfs_dir_entry *pfs_dentry;
	struct pantryfs_dir_entry *dir_dentry;
	int i;
	// store and cache
	struct inode *dd_inode = NULL;
	struct pantryfs_inode *dd_pfs_inode;
	
	sb = parent->i_sb;

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
	/* read directory block from disk */
	pardir_bh = sb_bread(sb, PFS_datablock_no_from_inode(istore_bh, parent));
	if (!pardir_bh) {
		pr_err("Could not read pardir block\n");
		ret = ERR_PTR(-EIO);
		goto lookup_end;
	}

	/* look for dentry in data block */
	dir_dentry = NULL;
	for (i = 0; i < PFS_MAX_CHILDREN; i++) {
		pfs_dentry = PFS_dentry_from_dirblock(pardir_bh, i);

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
	dd_pfs_inode = (struct pantryfs_inode *) 
		(istore_bh->b_data + (dir_dentry->inode_no - 1) * sizeof(struct pantryfs_inode));

	dd_inode = pfs_inode(sb, dir_dentry->inode_no, dd_pfs_inode);
	if (dd_inode == NULL) {
		pr_err("pfs_inode failed");
		ret = ERR_PTR(-ENOMEM);
		goto lookup_release;
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

/* P10: implement mkdir */
int pantryfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	// basic
	int ret = 0;
	struct super_block *sb = dir->i_sb;
	// buffers
	struct buffer_head *par_bh;
	struct pantryfs_sb_buffer_heads buf_heads;
	// new i_no, db_no, dentry
	struct i_db_no new_i_db_no;
	struct pantryfs_dir_entry *pfs_dentry;
	// writing information
	struct pantryfs_super_block *pantry_sb;
	struct pantryfs_inode *pfs_new_inode;
	// new data block
	struct buffer_head *new_bh;

	/* 1. Open sb and get new i_no, db_no */

	buf_heads.sb_bh = sb_bread(sb, PANTRYFS_SUPERBLOCK_DATABLOCK_NUMBER);
	if (!buf_heads.sb_bh) {
		pr_err("Could not read super block");
		ret = -EIO;
		goto mkdir_end;
	}
	new_i_db_no = PFS_get_free_i_db_no(buf_heads.sb_bh);
	if (new_i_db_no.db_no == -1 || new_i_db_no.i_no == -1) {
		pr_err("Could not find a free inode or data block!");
		goto mkdir_release;
	}

	/* 2. Open i store */

	buf_heads.i_store_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!buf_heads.i_store_bh) {
		pr_err("Could not read i store block");
		ret = -EIO;
		goto mkdir_release;
	}

	/* 3. Open dir block */
	par_bh = sb_bread(sb, PFS_datablock_no_from_inode(buf_heads.i_store_bh, dir));
	if (!par_bh) {
		pr_err("Could not read parent dir datablock");
		ret = -EIO;
		goto mkdir_release_2;
	}

	/* 3. Find empty i_no, db_no, dentry */
	// Get first empty dentry in dirblock
	pfs_dentry = PFS_next_empty_dentry(par_bh);
	if (pfs_dentry == NULL) {
		pr_err("Could not find a free dentry");
		goto mkdir_release_3;
	}

	/* 4. Now write out information */
	// write sb - mark inode and datablock entries as used
	pantry_sb = (struct pantryfs_super_block *) buf_heads.sb_bh->b_data;
	SETBIT(pantry_sb->free_inodes, new_i_db_no.i_no);
	SETBIT(pantry_sb->free_data_blocks, new_i_db_no.db_no - 1); // need to subtract 1 bc datablocks are offset

	mark_buffer_dirty(buf_heads.sb_bh);
	sync_dirty_buffer(buf_heads.sb_bh);

	// write istore
	pfs_new_inode = PFS_inode_from_istore(buf_heads.i_store_bh, new_i_db_no.i_no);

	pfs_new_inode->nlink = 2;
	pfs_new_inode->mode = S_IFDIR | 0777;
	pfs_new_inode->data_block_number = new_i_db_no.db_no;
	pfs_new_inode->file_size = PFS_BLOCK_SIZE;

	pfs_new_inode->uid = dir->i_uid.val;
	pfs_new_inode->gid = dir->i_gid.val;

	pfs_new_inode->i_atime = current_time(dir);
	pfs_new_inode->i_mtime = pfs_new_inode->i_atime;
	pfs_new_inode->i_ctime = pfs_new_inode->i_atime;

	mark_buffer_dirty(buf_heads.i_store_bh);
	sync_dirty_buffer(buf_heads.i_store_bh);

	// write dentry
	pfs_dentry->inode_no = new_i_db_no.i_no;
	pfs_dentry->active = 1;
	strncpy(pfs_dentry->filename, dentry->d_name.name, sizeof(pfs_dentry->filename));

	mark_buffer_dirty(par_bh);
	sync_dirty_buffer(par_bh);

	/* zero out new data block */
	new_bh = sb_bread(sb, new_i_db_no.db_no);
	if (!new_bh) {
		pr_err("Could not read new dir datablock");
		ret = -EIO;
		goto mkdir_release_3;
	}

	memset(new_bh->b_data, 0, PFS_BLOCK_SIZE);

	mark_buffer_dirty(new_bh);
	sync_dirty_buffer(new_bh);

	/* clean up */

	brelse(new_bh);
mkdir_release_3:
	brelse(par_bh);
mkdir_release_2:
	brelse(buf_heads.i_store_bh);
mkdir_release:
	brelse(buf_heads.sb_bh);

mkdir_end:
	return ret;
}

/* P10 rmdir */
int pantryfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int ret = 0;
	struct super_block *sb = dir->i_sb;
	struct buffer_head *bh;
	struct buffer_head *istore_bh;
	int i;
	struct pantryfs_dir_entry *pfs_dentry;
	struct inode *dentry_inode;

	int n_active = 0;


	/* get istore */
	istore_bh = sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!istore_bh) {
		pr_err("Could not read i store block");
		return -EIO;
	}

	/* open datablock for this entry */
	dentry_inode = dentry->d_inode;
	bh = sb_bread(sb, PFS_datablock_no_from_inode(istore_bh, dentry_inode));
	if (!bh) {
		pr_err("Could not read from data block");
		return -EIO;
		goto rmdir_release;
	}

	/* if the directory is not empty, fail */	
	pr_info("n links: %d", dir->i_nlink);
	pr_info("datablock: %lu", PFS_datablock_no_from_inode(istore_bh, dentry_inode));

	if (dir->i_nlink > 2)
		return -ENOTEMPTY;

	for (i = 0; i < PFS_MAX_CHILDREN; i++) {
		pfs_dentry = PFS_dentry_from_dirblock(bh, i);
		pr_info("active %d (%s) #%u: %d", i, pfs_dentry->filename, pfs_dentry->inode_no, pfs_dentry->active);

		if (pfs_dentry->active) {
			n_active++;
		}
	}

	if (n_active > 1)
		return -ENOTEMPTY;
	pr_info("\n");
	
	brelse(bh);
rmdir_release:
	brelse(istore_bh);

	if (ret != 0)
		return ret;
	/* otherwise just use unlink */
	return pantryfs_unlink(dir, dentry);
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

	/* P3: read PantryFS root inode from disk and associate it with root_inode */
	// Not sure if we strictly have to do malloc - but I don't know if we can guarantee
	// that buffer heads will stick around, so this seems reasonable
	inode_buf = kmalloc(sizeof(struct pantryfs_inode), GFP_KERNEL);
	memcpy(inode_buf, buf_heads.i_store_bh->b_data, sizeof(struct pantryfs_inode));
	pfs_root_inode = (struct pantryfs_inode *) inode_buf;

	root_inode = pfs_inode(sb, PANTRYFS_ROOT_INODE_NUMBER, pfs_root_inode);
	if (root_inode == NULL) {
		pr_err("pfs_inode failed");
		ret = -ENOMEM;
		goto fill_super_release_both;
	}
	root_inode->i_private = pfs_root_inode;

	/* create dentry for root inode */
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		pr_err("Could not allocate root dentry");
		ret = -ENOMEM;
		goto fill_super_release_both;
	}


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

	pr_info("PFS_MAX_CHILDREN: %u", PFS_MAX_CHILDREN);

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
