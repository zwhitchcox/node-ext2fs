#include <stdio.h>
#include <nan.h>

#include "ext2fs.h"
#include "js_io.h"
#include "async.h"

#include "node_ext2fs.h"

#define CHECK_ARGS(length) \
	if (info.Length() != length) { \
		ThrowTypeError("Wrong number of arguments"); \
		return; \
	} \
	if (!info[length - 1]->IsFunction()) { \
		ThrowTypeError("A callback is required"); \
		return; \
	}

#define X_NAN_METHOD(name, worker, nb_args) \
	NAN_METHOD(name) { \
		CHECK_ARGS(nb_args) \
		Callback *callback = new Callback(info[nb_args - 1].As<v8::Function>()); \
		AsyncQueueWorker(new worker(info, callback)); \
	}


using namespace Nan;

extern "C" {
	void com_err(const char *a, long b, const char *c, ...) {
		return;
	}
}

ext2_filsys get_filesystem(NAN_METHOD_ARGS_TYPE info) {
	return static_cast<ext2_filsys>(
		info[0]->ToObject().As<v8::External>()->Value()
	);
}

ext2_file_t get_file(NAN_METHOD_ARGS_TYPE info) {
	return static_cast<ext2_file_t>(
		info[0]->ToObject().As<v8::External>()->Value()
	);
}

unsigned int get_flags(NAN_METHOD_ARGS_TYPE info) {
	return info[1]->IntegerValue();
}

char* get_path(NAN_METHOD_ARGS_TYPE info) {
	Nan::Utf8String path_(info[1]);
	int len = strlen(*path_);
	char* path = static_cast<char*>(malloc(len + 1));
	strncpy(path, *path_, len);
	path[len] = '\0';
	return path;
}

class TrimWorker : public AsyncWorker {
	public:
		TrimWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback)
		: AsyncWorker(callback) {
			fs = get_filesystem(info);
		}
		~TrimWorker() {}

		void Execute () {
			unsigned int start, blk, count;

			if (!fs->block_map) {
				if ((ret = ext2fs_read_block_bitmap(fs))) {
					return;
				}
			}

			start = fs->super->s_first_data_block;
			count = fs->super->s_blocks_count;
			for (blk = start; blk <= count; blk++) {
				// Check for either the last iteration or a used block
				if (blk == count || ext2fs_test_block_bitmap(fs->block_map, blk)) {
					if (start < blk) {
						if ((ret = io_channel_discard(fs->io, start, blk - start))) {
							return;
						}
					}
					start = blk + 1;
				}
			}
		}

		void HandleOKCallback () {
			HandleScope scope;

			if (ret) {
				v8::Local<v8::Value> argv[] = {
					ErrnoException(-ret)
				};
				callback->Call(1, argv);
			} else {
				v8::Local<v8::Value> argv[] = {
					Null(),
				};
				callback->Call(1, argv);
			}
		}

	private:
		errcode_t ret = 0;
		ext2_filsys fs;

};
X_NAN_METHOD(trim, TrimWorker, 2);

class MountWorker : public AsyncWorker {
	public:
		MountWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback) : AsyncWorker(callback) {
			request_cb = new Callback(info[0].As<v8::Function>());
		}
		~MountWorker() {}

		void Execute () {
			char hex_ptr[sizeof(void*) * 2 + 3];
			sprintf(hex_ptr, "%p", request_cb);
			ret = ext2fs_open(
				hex_ptr,              // name
				EXT2_FLAG_RW,         // flags
				0,                    // superblock
				0,                    // block_size
				get_js_io_manager(),  // manager
				&fs                   // ret_fs
			);
			if (ret) return;
			ret = ext2fs_read_bitmaps(fs);
			if (ret) return;
		}

		void HandleOKCallback () {
			HandleScope scope;
			if (ret) {
				v8::Local<v8::Value> argv[] = {ErrnoException(-ret)};
				callback->Call(1, argv);
				return;
			}
			// FIXME: when V8 garbage collects this object we should also free
			// any resources allocated by libext2fs
			v8::Local<v8::Value> argv[] = {Null(), New<v8::External>(fs)};
			callback->Call(2, argv);
		}

	private:
		errcode_t ret = 0;
		ext2_filsys fs;
		Callback *request_cb;
};
X_NAN_METHOD(mount, MountWorker, 2);

class UmountWorker : public AsyncWorker {
	public:
		UmountWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback)
		: AsyncWorker(callback) {
			fs = get_filesystem(info);
		}
		~UmountWorker() {}

		void Execute () {
			ret = ext2fs_close(fs);
		}

		void HandleOKCallback () {
			v8::Local<v8::Value> argv[1];
			if (ret) {
				argv[0] = ErrnoException(-ret);
			} else {
				argv[0] = Null();
			}
			callback->Call(1, argv);
		}

	private:
		errcode_t ret = 0;
		ext2_filsys fs;
};
X_NAN_METHOD(umount, UmountWorker, 2);

ext2_ino_t string_to_inode(ext2_filsys fs, char *str) {
	ext2_ino_t ino;
	int retval;

	retval = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, str, &ino);
	if (retval) {
		com_err(str, retval, 0);
		return 0;
	}
	return ino;
}

ext2_ino_t get_parent_dir_ino(ext2_filsys fs, char* path) {
	char* last_slash = strrchr(path, (int)'/');
	if (last_slash == NULL) {
		return NULL;
	}
	unsigned int parent_len = last_slash - path + 1;
	char* parent_path = static_cast<char*>(malloc(parent_len + 1));
	strncpy(parent_path, path, parent_len);
	parent_path[parent_len] = '\0';
	ext2_ino_t parent_ino = string_to_inode(fs, parent_path);
	free(parent_path);
	return parent_ino;
}

char* get_filename(char* path) {
	char* last_slash = strrchr(path, (int)'/');
	if (last_slash == NULL) {
		return NULL;
	}
	char* filename = last_slash + 1;
	if (strlen(filename) == 0) {
		return NULL;
	}
	return filename;
}

unsigned int translate_open_flags(unsigned int js_flags) {
	unsigned int result = 0;
	if (js_flags & (O_WRONLY | O_RDWR)) {
		result |= EXT2_FILE_WRITE;
	}
	if (js_flags & O_CREAT) {
		result |= EXT2_FILE_CREATE;
	}
//	JS flags:
//	O_RDONLY
//	O_WRONLY		EXT2_FILE_WRITE
//	O_RDWR
//	O_CREAT			EXT2_FILE_CREATE
//	O_EXCL
//	O_NOCTTY
//	O_TRUNC
//	O_APPEND
//	O_DIRECTORY
//	O_NOATIME
//	O_NOFOLLOW
//	O_SYNC
//	O_SYMLINK
//	O_DIRECT
//	O_NONBLOCK
	return result;
}

errcode_t create_file(ext2_filsys fs, char* path, unsigned int mode, ext2_ino_t* ino) {
	errcode_t ret = 0;
	ext2_ino_t parent_ino = get_parent_dir_ino(fs, path);
	if (parent_ino == NULL) {
		return -ENOTDIR;
	}
	ret = ext2fs_new_inode(fs, parent_ino, mode, 0, ino);
	if (ret) return ret;
	char* filename = get_filename(path);
	if (filename == NULL) {
		// This should never happen.
		return -EISDIR;
	}
	ret = ext2fs_link(fs, parent_ino, filename, *ino, EXT2_FT_REG_FILE);
	if (ret == EXT2_ET_DIR_NO_SPACE) {
		ret = ext2fs_expand_dir(fs, parent_ino);
		if (ret) return ret;
		ret = ext2fs_link(fs, parent_ino, filename, *ino, EXT2_FT_REG_FILE);
	}
	if (ret) return ret;
	if (ext2fs_test_inode_bitmap2(fs->inode_map, *ino)) {
		printf("Warning: inode already set\n");
	}
	ext2fs_inode_alloc_stats2(fs, *ino, +1, 0);
	struct ext2_inode inode;
	memset(&inode, 0, sizeof(inode));
	inode.i_mode = (mode & ~LINUX_S_IFMT) | LINUX_S_IFREG;
	inode.i_atime = inode.i_ctime = inode.i_mtime = time(0);
	inode.i_links_count = 1;
	ret = ext2fs_inode_size_set(fs, &inode, 0);  // TODO: udpate size? also on write?
	if (ret) return ret;
	if (ext2fs_has_feature_inline_data(fs->super)) {
		inode.i_flags |= EXT4_INLINE_DATA_FL;
	} else if (ext2fs_has_feature_extents(fs->super)) {
		ext2_extent_handle_t handle;
		inode.i_flags &= ~EXT4_EXTENTS_FL;
		ret = ext2fs_extent_open2(fs, *ino, &inode, &handle);
		if (ret) return ret;
		ext2fs_extent_free(handle);
	}

	ret = ext2fs_write_new_inode(fs, *ino, &inode);
	if (ret) return ret;
	if (inode.i_flags & EXT4_INLINE_DATA_FL) {
		ret = ext2fs_inline_data_init(fs, *ino);
		if (ret) return ret;
	}
	return 0;
}

class OpenWorker : public AsyncWorker {
	public:
		OpenWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback)
		: AsyncWorker(callback) {
			fs = get_filesystem(info);
			path = get_path(info);
			flags = translate_open_flags(info[2]->IntegerValue());
			mode = info[3]->IntegerValue();
		}
		~OpenWorker() {}

		void Execute () {
			// TODO flags
			// TODO: free ?
			// TODO; update access time if file exists and O_NOATIME not in flags
			ext2_ino_t ino = string_to_inode(fs, path);
			if (!ino) {
				if (!(flags & EXT2_FILE_CREATE)) {
					ret = -ENOENT;
					return;
				}
				ret = create_file(fs, path, mode, &ino);
				if (ret) return;
			}
			ret = ext2fs_file_open(fs, ino, flags, &file);
		}

		void HandleOKCallback () {
			HandleScope scope;
			if (ret) {
				v8::Local<v8::Value> argv[] = {ErrnoException(-ret)};
				callback->Call(1, argv);
				return;
			}
			v8::Local<v8::Value> argv[] = {Null(), New<v8::External>(file)};
			callback->Call(2, argv);
		}

	private:
		errcode_t ret = 0;
		ext2_filsys fs;
		char* path;
		unsigned int flags;
		unsigned int mode;
		ext2_file_t file;
};
X_NAN_METHOD(open, OpenWorker, 5);

class CloseWorker : public AsyncWorker {
	public:
		CloseWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback)
		: AsyncWorker(callback) {
			file = get_file(info);
			flags = get_flags(info);
		}
		~CloseWorker() {}

		void Execute () {
			ret = ext2fs_file_close(file);
			// TODO: free
		}

		void HandleOKCallback () {
			HandleScope scope;  // TODO: needed ?
			if (ret) {
				v8::Local<v8::Value> argv[] = {ErrnoException(-ret)};
				callback->Call(1, argv);
				return;
			}
			v8::Local<v8::Value> argv[] = {Null()};
			callback->Call(1, argv);
		}

	private:
		errcode_t ret = 0;
		ext2_file_t file;
		unsigned int flags;
};
X_NAN_METHOD(close, CloseWorker, 3);

static int update_xtime(ext2_file_t file, bool a, bool c, bool m) {
	errcode_t err;
	err = ext2fs_read_inode(file->fs, file->ino, &(file->inode));
	if (err) return err;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	if (a) {
		file->inode.i_atime = now.tv_sec;
	}
	if (c) {
		file->inode.i_ctime = now.tv_sec;
	}
	if (m) {
		file->inode.i_mtime = now.tv_sec;
	}
	increment_version(&(file->inode));
	err = ext2fs_write_inode(file->fs, file->ino, &(file->inode));
	if (err) return err;
	return 0;
}

class ReadWorker : public AsyncWorker {
	public:
		ReadWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback)
		: AsyncWorker(callback) {
			file = get_file(info);
			flags = get_flags(info);
			buffer = (char*) node::Buffer::Data(info[2]);
			offset = info[3]->IntegerValue();  // buffer offset
			length = info[4]->IntegerValue();
			position = info[5]->IntegerValue();  // file offset
		}
		~ReadWorker() {}

		void Execute () {
			// TODO: error handling
			if ((flags & O_WRONLY) != 0) {
				// Don't try to read write only files.
				ret = -EBADF;
				return;
			}
			__u64 pos; // needed?
			if (position != -1) {
				ret = ext2fs_file_llseek(file, position, EXT2_SEEK_SET, &pos);
			}
			ret = ext2fs_file_read(file, buffer + offset, length, &got);
			if (ret) return;
			if ((flags & O_NOATIME) == 0) {
				ret = update_xtime(file, true, false, false);
			}
		}

		void HandleOKCallback () {
			if (ret) {
				v8::Local<v8::Value> argv[] = {ErrnoException(-ret)};
				callback->Call(1, argv);
				return;
			}
			v8::Local<v8::Value> argv[] = {Null(), New<v8::Integer>(got)};
			callback->Call(2, argv);
		}

	private:
		errcode_t ret = 0;
		ext2_file_t file;
		unsigned int flags;
		char *buffer;
		unsigned int offset;
		unsigned int length;
		int position;
		unsigned int got;
};
X_NAN_METHOD(read, ReadWorker, 7);

class WriteWorker : public AsyncWorker {
	public:
		WriteWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback)
		: AsyncWorker(callback) {
			file = get_file(info);
			flags = get_flags(info);
			buffer = static_cast<char*>(node::Buffer::Data(info[2]));
			offset = info[3]->IntegerValue();  // buffer offset
			length = info[4]->IntegerValue();
			position = info[5]->IntegerValue();  // file offset
		}
		~WriteWorker() {}

		void Execute () {
			// TODO: error handling
			if ((flags & (O_WRONLY | O_RDWR)) == 0) {
				// Don't try to write to readonly files.
				ret = -EBADF;
				return;
			}
			__u64 pos; // needed?
			if (position != -1) {
				ret = ext2fs_file_llseek(file, position, EXT2_SEEK_SET, &pos);
				if (ret) return;
			}
			ret = ext2fs_file_write(file, buffer + offset, length, &written);
			if (ret) return;
			if ((flags & O_CREAT) != 0) {
				ret = update_xtime(file, false, true, true);
			}
		}

		void HandleOKCallback () {
			if (ret) {
				v8::Local<v8::Value> argv[] = {ErrnoException(-ret)};
				callback->Call(1, argv);
				return;
			}
			v8::Local<v8::Value> argv[] = {Null(), New<v8::Integer>(written)};
			callback->Call(2, argv);
		}

	private:
		errcode_t ret = 0;
		ext2_file_t file;
		unsigned int flags;
		char *buffer;
		unsigned int offset;
		unsigned int length;
		int position;
		unsigned int written;
};
X_NAN_METHOD(write, WriteWorker, 7);

int copy_filename_to_result(
	struct ext2_dir_entry *dirent,
	int offset,
	int blocksize,
	char *buf,
	void *priv_data
) {
	size_t len = ext2fs_dirent_name_len(dirent);
	if (
		(strncmp(dirent->name, ".", len) != 0) &&
		(strncmp(dirent->name, "..", len) != 0)
	) {
		auto filenames = static_cast<std::vector<std::string*>*>(priv_data);
		filenames->push_back(new std::string(dirent->name, len));
	}
	return 0;
}

class ReadDirWorker : public AsyncWorker {
	public:
		ReadDirWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback)
		: AsyncWorker(callback) {
			fs = get_filesystem(info);
			path = get_path(info);
		}
		~ReadDirWorker() {}

		void Execute () {
			// TODO: error handling
			ext2_ino_t ino = string_to_inode(fs, path);
			ret = ext2fs_file_open(
				fs,
				ino, // inode,
				0, // flags TODO
				&file
			);
			if (ret) return;
			ret = ext2fs_check_directory(fs, ino);
			if (ret) return;
			char *block_buf = static_cast<char*>(malloc(1024));  // TODO: constant?
			ret = ext2fs_dir_iterate(
				fs,
				ino,
				0,  // flags
				block_buf,
				copy_filename_to_result,
				&filenames
			);
			//TODO: free
		}

		void HandleOKCallback () {
			if (ret) {
				v8::Local<v8::Value> argv[] = {ErrnoException(-ret)};
				callback->Call(1, argv);
				return;
			}
			v8::Local<v8::Array> result = Nan::New<v8::Array>();
			for(auto const& filename: filenames) {
				result->Set(
					result->Length(),
					Nan::CopyBuffer(
						filename->c_str(),
						filename->length()
					).ToLocalChecked()
				);
			}
			v8::Local<v8::Value> argv[] = {Null(), result};
			callback->Call(2, argv);
		}

	private:
		errcode_t ret = 0;
		ext2_filsys fs;
		char* path;
		ext2_file_t file;
		std::vector<std::string*> filenames;
};
X_NAN_METHOD(readdir, ReadDirWorker, 3);

class UnlinkWorker : public AsyncWorker {
	public:
		UnlinkWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback)
		: AsyncWorker(callback) {
			fs = get_filesystem(info);
			path = get_path(info);
			rmdir = false;
		}
		~UnlinkWorker() {}

		void Execute () {
			if (strlen(path) == 0) {
				ret = -ENOENT;
				return;
			}
			ext2_ino_t ino = string_to_inode(fs, path);
			if (ino == 0) {
				ret = -ENOENT;
				return;
			}
			ret = ext2fs_check_directory(fs, ino);
			bool is_dir = (ret == 0);
			if (rmdir) {
				if (!is_dir) {
					ret = -ENOTDIR;
					return;
				}
			} else {
				if (is_dir) {
					ret = -EISDIR;
					return;
				}
			}
			// Remove the slash at the beginning if there is one.
			const char* path_with_no_slash;
			if (path[0] == '/') {
				path_with_no_slash = path + 1;
			} else {
				path_with_no_slash = path;
			}
			ret = ext2fs_unlink(fs, EXT2_ROOT_INO, path_with_no_slash, 0, 0);
		}

		void HandleOKCallback () {
			if (ret) {
				v8::Local<v8::Value> argv[] = {ErrnoException(-ret)};
				callback->Call(1, argv);
				return;
			}
			v8::Local<v8::Value> argv[] = {Null()};
			callback->Call(1, argv);
		}

	private:
		errcode_t ret = 0;
		ext2_filsys fs;
		char* path;

	protected:
		bool rmdir;
};
X_NAN_METHOD(unlink, UnlinkWorker, 3);

class RmDirWorker : public UnlinkWorker {
	public:
		RmDirWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback)
		: UnlinkWorker(info, callback) {
			rmdir = true;
		}
		~RmDirWorker() {}
};
X_NAN_METHOD(rmdir, RmDirWorker, 3);

class MkDirWorker : public AsyncWorker {
	public:
		MkDirWorker(NAN_METHOD_ARGS_TYPE info, Callback *callback)
		: AsyncWorker(callback) {
			fs = get_filesystem(info);
			path = get_path(info);
			mode = info[2]->IntegerValue();
		}
		~MkDirWorker() {}

		void Execute () {
			// TODO: error handling
			// TODO: free ?
			ext2_ino_t parent_ino = get_parent_dir_ino(fs, path);
			if (parent_ino == NULL) {
				ret = -ENOTDIR;
				return;
			}
			char* filename = get_filename(path);
			if (filename == NULL) {
				// This should never happen.
				ret = -EISDIR;
				return;
			}
			ext2_ino_t newdir;
			ret = ext2fs_new_inode(
				fs,
				parent_ino,
				LINUX_S_IFDIR,
				NULL,
				&newdir
			);
			if (ret) return;
			ret = ext2fs_mkdir(fs, parent_ino, newdir, filename);
			//
			struct ext2_inode inode;
			ret = ext2fs_read_inode(fs, newdir, &inode);
			if (ret) return;
			inode.i_mode = (mode & 0xFFFF) | LINUX_S_IFDIR;
			ret = ext2fs_write_inode(fs, newdir, &inode);
		}

		void HandleOKCallback () {
			if (ret) {
				v8::Local<v8::Value> argv[] = {ErrnoException(-ret)};
				callback->Call(1, argv);
				return;
			}
			v8::Local<v8::Value> argv[] = {Null()};
			callback->Call(1, argv);
		}

	private:
		errcode_t ret = 0;
		ext2_filsys fs;
		char* path;
		unsigned int mode;
};
X_NAN_METHOD(mkdir, MkDirWorker, 4);

v8::Local<v8::Value> castUint32(long unsigned int x) {
	return Nan::New<v8::Uint32>(static_cast<uint32_t>(x));
}

v8::Local<v8::Value> castInt32(long int x) {
	return Nan::New<v8::Int32>(static_cast<int32_t>(x));
}

v8::Local<v8::Value> timespecToMilliseconds(__u32 seconds) {
    return Nan::New<v8::Number>(static_cast<double>(seconds) * 1000);
}

NAN_METHOD(fstat) {
	CHECK_ARGS(4)
	auto file = get_file(info);
	auto flags = get_flags(info);
	auto Stats = info[2].As<v8::Function>();
	auto *callback = new Callback(info[3].As<v8::Function>());
	v8::Local<v8::Value> statsArgs[] = {
		castInt32(0),   // dev
		castInt32(file->inode.i_mode),
		castInt32(file->inode.i_links_count),
		castInt32(file->inode.i_uid),
		castUint32(file->inode.i_gid),
		castUint32(0),  // rdev
		castInt32(file->fs->blocksize),
		castInt32(file->ino),
		castUint32(file->inode.i_size),
		castUint32(file->inode.i_blocks),
		timespecToMilliseconds(file->inode.i_atime),
		timespecToMilliseconds(file->inode.i_mtime),
		timespecToMilliseconds(file->inode.i_ctime),
		timespecToMilliseconds(file->inode.i_ctime),
	};
	auto stats = Nan::NewInstance(Stats, 14, statsArgs).ToLocalChecked();
	v8::Local<v8::Value> argv[] = {Null(), stats};
	callback->Call(2, argv);
}

NAN_METHOD(init) {
	init_async();
}

NAN_METHOD(closeExt) {
	close_async();
}
