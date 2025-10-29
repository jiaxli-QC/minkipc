// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <libgen.h>
#include <errno.h>

#include "gpfs_msg.h"
#include "cmn.h"
#include "helper.h"
#include "gpfs.h"

#ifdef USE_GLIB
#include <glib.h>
#define strlcat g_strlcat
#define strlcpy g_strlcpy
#endif

static int gpfile_readwrite_helper(uint32_t cmd_id, char *path_name,
				   uint32_t path_name_len, uint32_t flags,
				   off_t offset, uint8_t *buf, size_t buf_len,
				   size_t *count, bool backup);

/**
 * @brief Checks whether a given directory exists on the filesystem.
 *
 * This function verifies the existence of a directory by attempting to open it
 * using `opendir()`.
 *
 * @param dirname Pointer to the directory path to check.
 * @return Returns 0 if the directory exists, or an error code:
 */
static int dir_exists(char *dirname)
{
	int errcode = 0;
	char *tempdir = NULL;
	unsigned int len = 0;
	DIR *dfd = NULL;

	if (strcmp(dirname, DATA_VENDOR_PATH) == 0 ||
	    strcmp(dirname, DATA_PATH) == 0) {
		MSGD("Ignore checking path: %s\n", dirname);
		return EINVAL;
	}

	len = strlen(dirname);
	tempdir = malloc(len + 1);

	if (tempdir == NULL) {
		errcode = ENOMEM;
		MSGE("Error: malloc failed!\n");
		goto exit;
	}

	if (memscpy(tempdir, len + 1, dirname, strlen(dirname))) {
		errcode = EFAULT;
		MSGE("[%s:%d] Invalid buffer len.\n", __func__, __LINE__);
		goto exit;
	}

	tempdir[len] = '\0';
	dfd = opendir((const char *)tempdir);
	MSGD("calling dir_exists!\n");
	MSGD("opening dir %s\n", tempdir);

	/* The directory exists */
	if (dfd != NULL) {
		closedir(dfd);
	} else { /* The directory does not exist */
		errcode = errno;
		MSGD("dir %s does not exist!!\n", tempdir);
		goto exit;
	}

	MSGD("dir %s exists, return %d\n", tempdir, errcode);

exit:
	if (tempdir != NULL)
		free(tempdir);

	return errcode;
}

/**
 * @brief Recursively creates a directory and its parent directories if they do
 *	not exist.
 *
 * This function attempts to create the specified directory path, including any
 * intermediate
 * directories that do not already exist.
 *
 * @param p_dir The full path of the directory to create.
 *
 * @return
 * - 0 on success (directory already exists or was created successfully)
 * - EINVAL if the input path is NULL, empty, or too long
 * - ENOMEM if memory allocation fails
 * - EFAULT if buffer copy fails
 * - Other error codes returned by mkdir() if directory creation fails
 */
static int mkdir_h(const char *p_dir)
{
	uint32_t len_folder = 0;
	uint32_t len_travel = 0;
	char *dir = NULL;
	char *cur_pos = NULL;
	uint32_t len_dir = 0;
	int errcode = 0;
	int ret_val = -1;

	/* Verify path validity */
	if ((p_dir == NULL) || (p_dir[0] == '\0')) {
		MSGE("input dir in mkdir_h is null, return -1\n");
		return EINVAL;
	}

	MSGD("calling mkdir_h, path = %s\n", p_dir);

	len_dir = strlen(p_dir);

	if (len_dir >= TZ_FILE_DIR_LEN) {
		MSGE("input dir length is too big. return -1\n");
		return EINVAL;
	}

	if (dir_exists((char *)p_dir) == 0) {
		MSGD("input dir %s already exist\n", p_dir);
		return 0;
	}

	/* Copy the name of the path */
	dir = (char *)malloc(len_dir + 1);

	if (NULL == dir) {
		MSGE("malloc in mkdir_h is failed! return -1\n");
		return ENOMEM;
	}

	if (memscpy((void *)dir, len_dir, (void *)p_dir, len_dir)) {
		free(dir);
		MSGE("Invalid buffer len in memscpy %s, %d\n", __func__, __LINE__);
		return EFAULT;
	}

	dir[len_dir] = '\0';
	cur_pos = dir;

	/* Skip the root '/' */
	if (*cur_pos == '/') {
		cur_pos++;
		len_travel++;
	}

	while (len_travel < len_dir) {
		len_folder = 0;
		while (*cur_pos != '/' && *cur_pos != '\0') {
			cur_pos++;
			len_folder++;
			len_travel++;
		}
		if (len_folder == 0) {
			break;
		}
		/* Found a parent directory */
		if (*cur_pos == '/') {
			*cur_pos = '\0';

			/* The parent directory exists */
			if (dir_exists(dir) == 0) {
				*cur_pos = '/';
				cur_pos++;
				len_travel++;
				continue;
			}
		}
		/* mkdir() fails */
		ret_val = mkdir(dir, 0774);
		if (ret_val != 0 && errno != EEXIST) {
			errcode = errno;
			MSGE("mkdir(%s) fails with error code : %d  errno %d\n",
			     dir, ret_val, errcode);
			break;
		}
		*cur_pos = '/';
		cur_pos++;
		len_travel++;
	}

	if (NULL != dir)
		free(dir);

	MSGD("mkdir_h returns %d\n", errcode);
	return errcode;
}

/**
 * @brief Prepares a directory path for file operations.
 *
 * This function extracts the directory portion by trimming the file name, and
 * ensures the directory exists by calling mkdir_h().
 *
 * @param path_name Full path to the file including the file name.
 *
 * @return
 * - 0 on success
 * - EINVAL if the path is invalid or lacks a directory component
 * - EFAULT if buffer copy fails
 */
static int file_preopen(const char *path_name)
{
	int path_len = 0;
	char path[TZ_FILE_DIR_LEN + 1] = { 0 };

	MSGD("calling file_preopen %s\n", path_name);

	path_len = strlen(path_name);
	if (path_len >= TZ_FILE_DIR_LEN || path_len < 0) {
		return EINVAL;
	}

	if (memscpy((void *)path, ARRAY_SIZE(path),
		    (void *)path_name, path_len)) {
		MSGE("Invalid buffer len in memscpy %s, %d\n",
		     __func__, __LINE__);
		return EFAULT;
	}

	while (path_len > 0) {
		if (path[path_len - 1] == '/') {
			path[path_len] = '\0';
			break;
		}
		path_len--;
	}

	if (path_len == 0) {
		/* This condition implies that the pathname from QTEE
		 * did not have a trailing forward-slash / */
		return EINVAL;
	}

	MSGD("file_preopen : path_len =%d\n", path_len);
	return mkdir_h(path);
}

/**
 * @brief Opens a file with the specified flags, creating directories if needed.
 *
 * This function resolves the full vendor path for the given file, optionally
 * creates the parent directory if the O_CREAT flag is set, and opens the file
 * with the specified flags.
 *
 * @param path Path to the file to open.
 * @param flags File open flags (e.g., O_RDONLY, O_CREAT).
 *
 * @return
 * - File descriptor on success
 * - Negative error code on failure
 */
static int file_open(char *path, int flags)
{
	int fd = -1;
	int errcode = 0;
	char *pathname = NULL;

	MSGD("calling file_open %s, flag = %d\n", path, flags);
	char new_vendor_path[TZ_FILE_DIR_LEN] = { 0 };
	pathname = get_resolved_path(path, strlen(path), new_vendor_path,
				   TZ_FILE_DIR_LEN);

	if (flags & O_CREAT) {
		errcode = file_preopen(pathname);
		if (0 != errcode) {
			MSGE("Error: file_open failed!\n");
			return -errcode;
		}
	}
	fd = open(pathname, flags, S_IRUSR | S_IWUSR);
	if (fd < 0)
		errcode = errno;

	MSGD("file_open %s is done and returns %d\n", pathname, fd);

	if (fd < 0)
		return -errcode;
	else
		return fd;
}


/**
 * @brief Creates a backup of a file by copying its contents to a new file.
 *
 * This function reads the contents of the file associated with the given
 * file descriptor and writes them to a backup file with the same name
 * appended by a backup suffix (e.g., ".bak").
 *
 * @param fd File descriptor of the file to back up.
 * @param path_name Path to the original file.
 *
 * @return
 * - 0 on success
 * - ENOMEM if memory allocation fails
 * - EIO if read/write operations fail
 * - errno value from system calls on failure
 */

static int backup_file(int fd, const char *path_name)
{
	struct stat stats;
	char backup_path[TZ_CM_MAX_NAME_LEN + sizeof(BAK)] = { 0 };
	char new_vendor_bkp_path[TZ_FILE_DIR_LEN] = { 0 };
	const char *bkp_path_name;
	uint8_t *data = NULL;
	int data_size = 0, r_bytes = 0, w_bytes = 0, w_bytes_total = 0;
	int r_bytes_remaining = 0, backup_fd = -1;

	if (fstat(fd, &stats) < 0) {
		MSGE("fstat failed: errno=%d\n", errno);
		return errno;
	}

	if (stats.st_size == 0)
		return 0;

	data_size = (stats.st_size < MAX_READ_SIZE) ?
		    stats.st_size : MAX_READ_SIZE;
	data = (uint8_t *)malloc(data_size);
	if (!data) {
		MSGE("malloc failed for backup buffer\n");
		return ENOMEM;
	}

	strlcpy(backup_path, path_name, sizeof(backup_path));
	strlcat(backup_path, BAK, sizeof(backup_path));
	bkp_path_name = get_resolved_path(backup_path, strlen(backup_path),
					new_vendor_bkp_path, TZ_FILE_DIR_LEN);

	backup_fd = open(bkp_path_name, O_CREAT | O_RDWR | O_TRUNC,
			 S_IRUSR | S_IWUSR);
	if (backup_fd < 0) {
		MSGE("Failed to open backup file: errno=%d\n", errno);
		free(data);
		return errno;
	}

	r_bytes_remaining = stats.st_size;
	while (r_bytes_remaining > 0) {
		r_bytes = read(fd, data, data_size);
		if (r_bytes <= 0)
			break;

		w_bytes_total = 0;
		while (w_bytes_total < r_bytes) {
			w_bytes = write(backup_fd, data + w_bytes_total,
					r_bytes - w_bytes_total);
			if (w_bytes <= 0)
				break;
			w_bytes_total += w_bytes;
		}

		if (w_bytes_total < r_bytes)
			break;

		r_bytes_remaining -= r_bytes;
	}

	fsync(backup_fd);
	close(backup_fd);
	free(data);

	if (r_bytes <= 0 || w_bytes <= 0) {
		MSGE("Backup read/write failed\n");
		return EIO;
	}

	return 0;
}

/**
 * @brief Performs a read or write operation on a file based on the command id
 *
 * This function reads from or writes to a file descriptor depending on the
 * specified command ID. It continues the operation until the requested number
 * of bytes is processed or an error occurs.
 *
 * @param cmd_id Command identifier indicating the type of operation:
 * @paramfd     File descriptor to operate on.
 * @param buf   Buffer to read into or write from.
 * @param count   Pointer to the number of bytes to read/write. Updated with actual bytes processed.
 *
 * @return
 * - 0 on success
 * - EINVAL if the command ID is invalid
 * - errno value if a system call (read/write) fails
 */
static int perform_read_write(uint32_t cmd_id, int fd, uint8_t *buf,
				 size_t *count)
{
	size_t bytes_remaining = *count;
	int ret_val = 0;

	while (bytes_remaining > 0) {
		if (cmd_id == TZ_GPFS_MSG_CMD_DATA_FILE_READ ||
		    cmd_id == TZ_GPFS_MSG_CMD_PERSIST_FILE_READ) {
			ret_val = read(fd, buf, bytes_remaining);
			if (ret_val < 0)
				return errno;
			MSGD("Read %d bytes\n", ret_val);
		} else if (cmd_id == TZ_GPFS_MSG_CMD_DATA_FILE_WRITE ||
			   cmd_id == TZ_GPFS_MSG_CMD_PERSIST_FILE_WRITE) {
			ret_val = write(fd, buf, bytes_remaining);
			if (ret_val < 0)
				return errno;
			MSGD("Wrote %d bytes\n", ret_val);
		} else {
			MSGE("Invalid command ID: %d\n", cmd_id);
			return EINVAL;
		}

		buf += ret_val;
		bytes_remaining -= ret_val;
	}

	*count -= bytes_remaining;
	return 0;
}

/**
 * @brief Helper function to perform read or write operations on a file.
 *
 * This function opens a file, optionally creates a backup, seeks to a
 * specified offset, and performs a read or write operation based on the
 * command ID.
 *
 * @param cmd_id Command identifier indicating read or write operation.
 * @param path_name Path to the file to be accessed.
 * @param path_name_len Length of the file path.
 * @param flags File open flags (e.g., O_RDONLY, O_WRONLY).
 * @param offset Offset in the file to begin the operation.
 * @param buf    Buffer for reading from or writing to the file.
 * @param buf_len Length of the buffer.
 * @param count    Pointer to the number of bytes to read/write. Updated with actual count.
 * @param backup Boolean flag indicating whether to create a backup before writing.
 *
 * @return
 * - 0 on success
 * - Non-zero error code on failure (e.g., ENOMEM, EINVAL, errno from system calls)
 */
static int gpfile_readwrite_helper(uint32_t cmd_id, char *path_name,
				   uint32_t path_name_len, uint32_t flags,
				   off_t offset, uint8_t *buf, size_t buf_len,
				   size_t *count, bool backup)
{
	int fd = -1;
	int err_code = 0;
	uint8_t *data = NULL;

	MSGD("calling gpfile_readwrite_helper!\n");
	MSGD("gpfile_readwrite_helper: pathname:%s,flags:%d,offset:%ld,count:%lu\n",
	     path_name, flags, offset, *count);

	if (path_name_len > TZ_CM_MAX_NAME_LEN) {
		MSGE("Path name length %d exceeds max %d\n",
		     path_name_len, TZ_CM_MAX_NAME_LEN);
		goto error;
	}

	fd = file_open(path_name, flags);
	if (fd < 0) {
		MSGE("Failed to open file: %s\n", path_name);
		err_code = -fd;
		goto error;
	}

	if (backup) {
		err_code = backup_file(fd, path_name);
		if (err_code != 0)
			goto error;
	}

	if (lseek(fd, offset, SEEK_SET) != offset) {
		MSGE("lseek failed: errno=%d\n", errno);
		err_code = errno;
		goto error;
	}

	if (*count > buf_len)
		*count = buf_len;

	err_code = perform_read_write(cmd_id, fd, buf, count);

	if (err_code != 0)
		*count = 0;

error:
	if (data)
		free(data);
	if (fd >= 0)
		close(fd);

	return err_code;
}

int gpfile_read(void *req, size_t req_len, void *rsp, size_t rsp_len)
{
	int err_code = 0;
	tz_gpfile_read_req_t *my_req = (tz_gpfile_read_req_t *)req;

	/* Don't change my_rsp until my_req is processed, as they may share buf */
	tz_gpfile_read_rsp_t *my_rsp = (tz_gpfile_read_rsp_t *)rsp;
	size_t count = (size_t)my_req->count;
	char abs_file_path[TZ_CM_MAX_NAME_LEN] = { 0 };

	MSGD("calling gpfile_read abs_file_path! %s\n", my_req->pathname);

	if (req_len < sizeof(tz_gpfile_read_req_t) ||
	    rsp_len < sizeof(tz_gpfile_read_rsp_t)) {
		MSGE("%s Invalid buffer length", __func__);
		return -1;
	}

	if (strlen((char *)my_req->pathname) == 0) {
		MSGE("%s invalid original path length\n", __func__);
		/* my_rsp->err indicates the failure reason */
		err_code = EINVAL;
		my_rsp->err = err_code;
		my_rsp->num_bytes_read = 0;
		goto exit;
	}

	switch (my_req->cmd_id) {
	case TZ_GPFS_MSG_CMD_PERSIST_FILE_READ:
		strlcpy(abs_file_path, PERSIST_PATH, TZ_FILE_NAME_LEN);
		strlcat(abs_file_path, my_req->pathname,
			sizeof(abs_file_path));
		break;
	case TZ_GPFS_MSG_CMD_DATA_FILE_READ:
		strlcpy(abs_file_path, DATA_PATH, TZ_FILE_NAME_LEN);
		strlcat(abs_file_path, my_req->pathname,
			sizeof(abs_file_path));
		break;
	default:
		MSGD("gpfile command %d is not found!, returning ERROR!\n",
		     my_req->cmd_id);
		my_rsp->err = GPFS_ERROR_NO_CMD;
		my_rsp->num_bytes_read = 0;
		return 0;
	}

	err_code = gpfile_readwrite_helper(my_req->cmd_id, abs_file_path,
					   TZ_CM_MAX_NAME_LEN, O_RDONLY,
					   (off_t)my_req->offset,
					   my_rsp->buf, TZ_GPFS_FILE_SIZE,
					   &count, false);

	my_rsp->err = err_code;
	if (err_code != 0)
		my_rsp->num_bytes_read = 0;
	else
		my_rsp->num_bytes_read = count;

exit:
	MSGD("gpfile_read returns as err:%d, num_bytes_read:%d\n",
	     my_rsp->err, my_rsp->num_bytes_read);
	MSGD("gpfile_read %s! err_code=%d\n",
	     (err_code != 0) ? "FAILED" : "PASSED", err_code);
	return 0;
}

int gpfile_write(void *req, size_t req_len, void *rsp, size_t rsp_len)
{
	int err_code = 0;
	tz_gpfile_write_req_t *my_req = (tz_gpfile_write_req_t *)req;
	tz_gpfile_write_rsp_t *my_rsp = (tz_gpfile_write_rsp_t *)rsp;
	size_t count = (size_t)my_req->count;
	char abs_file_path[TZ_CM_MAX_NAME_LEN] = { 0 };

	MSGD("calling gpfile_write!\n");

	if (req_len < sizeof(tz_gpfile_write_req_t) ||
	    rsp_len < sizeof(tz_gpfile_write_rsp_t)) {
		MSGE("%s Invalid buffer length\n", __func__);
		return -1;
	}

	if (strlen((char *)my_req->pathname) == 0) {
		MSGE("%s invalid original path length\n", __func__);
		/* my_rsp->err indicates the failure reason */
		err_code = EINVAL;
		my_rsp->err = err_code;
		my_rsp->num_bytes_written = 0;
		goto exit;
	}

	switch (my_req->cmd_id) {
	case TZ_GPFS_MSG_CMD_PERSIST_FILE_WRITE:
		strlcpy(abs_file_path, PERSIST_PATH, TZ_FILE_NAME_LEN);
		strlcat(abs_file_path, my_req->pathname,
			sizeof(abs_file_path));
		break;
	case TZ_GPFS_MSG_CMD_DATA_FILE_WRITE:
		strlcpy(abs_file_path, DATA_PATH, TZ_FILE_NAME_LEN);
		strlcat(abs_file_path, my_req->pathname,
			sizeof(abs_file_path));
		break;
	default:
		MSGD("gpfile command %d is not found!, returning ERROR!\n",
		     my_req->cmd_id);
		my_rsp->err = GPFS_ERROR_NO_CMD;
		my_rsp->num_bytes_written = 0;
		return 0;
	}

	err_code = gpfile_readwrite_helper(
		my_req->cmd_id, abs_file_path, TZ_CM_MAX_NAME_LEN,
		(O_CREAT | O_RDWR | O_SYNC), (off_t)my_req->offset,
		my_req->buf, TZ_GPFS_FILE_SIZE, &count,
		(my_req->backup == 0) ? false : true);

	my_rsp->err = err_code;
	if (err_code != 0)
		my_rsp->num_bytes_written = 0;
	else
		my_rsp->num_bytes_written = count;

exit:
	MSGD("gpfile_write returns as err:%d, num_bytes_written:%d\n",
	     my_rsp->err, my_rsp->num_bytes_written);
	MSGD("gpfile_write %s! err_code=%d\n",
	     (err_code != 0) ? "FAILED" : "PASSED", err_code);
	return 0;
}

int gpfile_remove(void *req, size_t req_len, void *rsp, size_t rsp_len)
{
	tz_gpfile_remove_req_t *my_req = (tz_gpfile_remove_req_t *)req;
	tz_gpfile_remove_rsp_t *my_rsp = (tz_gpfile_remove_rsp_t *)rsp;
	int ret_val = -1;
	char abs_file_path[TZ_CM_MAX_NAME_LEN] = { 0 };

	MSGD("calling gpfile_remove!\n");

	if (req_len < sizeof(tz_gpfile_remove_req_t) ||
	    rsp_len < sizeof(tz_gpfile_remove_rsp_t)) {
		MSGE("%s Invalid buffer length\n", __func__);
		return -1;
	}

	if (strlen((char *)my_req->pathname) == 0) {
		MSGE("%s invalid original path length\n", __func__);
		/* my_rsp->err indicates the failure reason */
		my_rsp->err = EINVAL;
		goto exit;
	}

	switch (my_req->cmd_id) {
	case TZ_GPFS_MSG_CMD_PERSIST_FILE_REMOVE:
		strlcpy(abs_file_path, PERSIST_PATH, TZ_FILE_NAME_LEN);
		strlcat(abs_file_path, my_req->pathname,
			sizeof(abs_file_path));
		break;
	case TZ_GPFS_MSG_CMD_DATA_FILE_REMOVE:
		strlcpy(abs_file_path, DATA_PATH, TZ_FILE_NAME_LEN);
		strlcat(abs_file_path, my_req->pathname,
			sizeof(abs_file_path));
		break;
	default:
		MSGD("gpfile command %d is not found!, returning ERROR!\n",
		     my_req->cmd_id);
		my_rsp->err = GPFS_ERROR_NO_CMD;
		return 0;
	}

	MSGD("%s Original path = %s, Final path = %s\n", __func__,
	     my_req->pathname, abs_file_path);

	if (dir_exists(abs_file_path) != 0) {
		ret_val = unlink(abs_file_path);
		if (ret_val < 0) {
			my_rsp->err = errno;
		} else {
			my_rsp->err = 0;
		}
	} else {
		/* GPFS listener only operates on files,
		 * if QTEE sends a directory path, something
		 * is wrong!
		 */
		my_rsp->err = EINVAL;
	}

	MSGD("gpfile_remove %s! ret_val=%d\n",
	     (ret_val < 0) ? "FAILED" : "PASSED", ret_val);

exit:
	MSGD("gpfile_remove returns with err:%d\n", my_rsp->err);
	return 0;
}


int gpfile_rename(void *req, size_t req_len, void *rsp, size_t rsp_len)
{
	tz_gpfile_rename_req_t *my_req = (tz_gpfile_rename_req_t *)req;
	tz_gpfile_rename_rsp_t *my_rsp = (tz_gpfile_rename_rsp_t *)rsp;
	int ret = -1;
	char abs_file_path_old[TZ_CM_MAX_NAME_LEN] = { 0 };
	char abs_file_path_new[TZ_CM_MAX_NAME_LEN] = { 0 };

	MSGD("calling gpfile_rename! cmd id = %d %s %s\n", my_req->cmd_id,
	     my_req->from, my_req->to);

	if (req_len < sizeof(tz_gpfile_rename_req_t) ||
	    rsp_len < sizeof(tz_gpfile_rename_rsp_t)) {
		MSGE("%s Invalid buffer length\n", __func__);
		return -1;
	}

	if (strlen((char *)my_req->from) == 0) {
		MSGE("%s invalid old path length\n", __func__);
		/* my_rsp->err indicates the failure reason */
		my_rsp->err = EINVAL;
		goto exit;
	}

	if (strlen((char *)my_req->to) == 0) {
		MSGE("%s invalid new path length\n", __func__);
		/* my_rsp->err indicates the failure reason */
		my_rsp->err = EINVAL;
		goto exit;
	}

	switch (my_req->cmd_id) {
	case TZ_GPFS_MSG_CMD_PERSIST_FILE_RENAME:
		strlcpy(abs_file_path_old, PERSIST_PATH, TZ_FILE_NAME_LEN);
		strlcat(abs_file_path_old, my_req->from,
			TZ_CM_MAX_NAME_LEN);
		strlcpy(abs_file_path_new, PERSIST_PATH, TZ_FILE_NAME_LEN);
		strlcat(abs_file_path_new, my_req->to, TZ_CM_MAX_NAME_LEN);
		break;
	case TZ_GPFS_MSG_CMD_DATA_FILE_RENAME:
		strlcpy(abs_file_path_old, DATA_PATH, TZ_FILE_NAME_LEN);
		strlcat(abs_file_path_old, my_req->from,
			TZ_CM_MAX_NAME_LEN);
		strlcpy(abs_file_path_new, DATA_PATH, TZ_FILE_NAME_LEN);
		strlcat(abs_file_path_new, my_req->to, TZ_CM_MAX_NAME_LEN);
		break;
	default:
		MSGD("gpfile command %d is not found!, returning ERROR!\n",
		     my_req->cmd_id);
		my_rsp->err = GPFS_ERROR_NO_CMD;
		return 0;
	}

	ret = rename(abs_file_path_old, abs_file_path_new);
	if (ret < 0)
		my_rsp->err = errno;
	else
		my_rsp->err = 0;

	MSGD("gpfile_rename %s! ret=%d\n",
	     (ret < 0) ? "FAILED" : "PASSED", ret);

exit:
	MSGD("gpfile_rename returns with err:%d\n", my_rsp->err);
	return 0;
}

int gpfile_check_version(void *req, size_t req_len,
			 void *rsp, size_t rsp_len)
{
	tz_gpfile_version_rsp_t *my_rsp = (tz_gpfile_version_rsp_t *)rsp;
	UNUSED(req);
	UNUSED(req_len);

	if (rsp_len < sizeof(tz_gpfile_version_rsp_t)) {
		MSGE("%s Invalid buffer length.\n", __func__);
		return -1;
	}

	my_rsp->version = GP_FS_VERSION;
	my_rsp->err = 0;

	MSGD("gpfile_check_version version %u\n", my_rsp->version);
	MSGD("gpfile_check_version ret as err %d\n", my_rsp->err);
	return 0;
}


int gpfile_error(void *rsp, size_t rsp_len)
{
	tz_gpfile_err_rsp_t *my_rsp = (tz_gpfile_err_rsp_t *)rsp;

	if (rsp_len < sizeof(tz_gpfile_err_rsp_t)) {
		MSGE("%s Invalid buffer length.\n", __func__);
		return -1;
	}

	MSGD("calling %s!\n", __func__);
	my_rsp->err = GPFS_ERROR_NO_CMD;
	MSGD("%s is done and returns = %d\n", __func__, GPFS_ERROR_NO_CMD);

	return 0;
}

int gpfile_partition_error(void *rsp, size_t rsp_len)
{
	tz_gpfile_err_rsp_t *my_rsp = (tz_gpfile_err_rsp_t *)rsp;

	if (rsp_len < sizeof(tz_gpfile_err_rsp_t)) {
		MSGE("%s Invalid buffer length.\n", __func__);
		return -1;
	}

	MSGD("calling %s!\n", __func__);
	my_rsp->err = EAGAIN;
	MSGD("%s is done and returns EAGAIN\n", __func__);

	return 0;
}
