#pragma once

#include <dev/hardware.h>
#include <fs.h>
#include <ck/ptr.h>
#include <ck/string.h>
#include <fs/FileSystem.h>
/**
 *
 * The virtual file system allows filesystems to be mounted recursively in the
 * directory tree. It is the main API for generalized filesystem access
 */

namespace vfs {

  /**
   * mountpoint - describes a single filesystem mounted on the system
   */
  struct mountpoint {
    ck::ref<fs::FileSystem> sb;

    // if this is null, it is the root node.
    ck::ref<fs::Node> host;
    int mountflags;

    ck::string devname;

    int id;

    struct mount *next;
    struct mount *prev;
  };

  // for use by the kernel only. (user cant access this function)
  int mount_root(const char *source, const char *type);

  // mount a device to a target with a certain filesystem
  int mount(const char *source, const char *targ, const char *type, unsigned long /* TODO */ flags, const char *options);

  /*
   * open a a file, rooted at task_process::cwd() if the path is not rooted at
   * `/`, and return the inode pointer. NULL on failure to open
   */
  ck::ref<fs::Node> open(ck::string path, int opts = 0, int mode = 0000);

  fs::File fdopen(ck::string path, int opts = 0, int mode = 0000);

  /*
   * namei()
   *
   * main "low level" interface for opening a file. Must have all arguments
   * passed.
   *
   * parent: stop at the parent directory of the last entry
   */
  int namei(const char *path, int flags, int mode, ck::ref<fs::Node> cwd, ck::ref<fs::Node> &res, bool get_last = false);


  int unlink(const char *path, ck::ref<fs::Node> cwd);

  /*
   * cwd()
   *
   * return the current working directory for whatever task we are in. If
   * we are not in a task, use vfs_root (/)
   */
  ck::ref<fs::Node> cwd(void);
  int getcwd(fs::Node &, ck::string &dst);

  int chroot(ck::string path);

  ck::ref<fs::Node> get_root(void);
  using Mounter = ck::ref<fs::FileSystem> (*)(ck::string options, int flags, ck::string path);
  void register_filesystem(ck::string name, Mounter mount);
  template <typename FileSystem>
  void register_filesystem(ck::string name) {
    register_filesystem(name, FileSystem::mount);
  }


  void init_boot_filesystem(void);

};  // namespace vfs
