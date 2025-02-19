#pragma once


#include <fs.h>
#include <mm.h>
#include <ck/vec.h>

namespace tmpfs {


  struct FileNode : public fs::FileNode {
    using fs::FileNode::FileNode;


    int seek_check(fs::File &, off_t old_off, off_t new_off) override;
    ssize_t read(fs::File &, char *dst, size_t count) override;
    ssize_t write(fs::File &, const char *, size_t) override;
    int resize(fs::File &, size_t) override;
    ck::ref<mm::VMObject> mmap(fs::File &, size_t npages, int prot, int flags, off_t off) override;

    ck::ref<mm::Page> get_page(off_t off) const {
      if (off >= 0 && off < m_pages.size()) {
      	return m_pages[off];
			}
      return nullptr;
    }

   private:
    int resize_r(fs::File &, size_t);

    ssize_t access(fs::File &f, void *data, size_t count, bool write);
    spinlock m_lock;
    ck::vec<ck::ref<mm::Page>> m_pages;
  };

  struct DirNode : public fs::DirectoryNode {
    using fs::DirectoryNode::DirectoryNode;
    ck::map<ck::string, ck::box<fs::DirectoryEntry>> entries;

    virtual int touch(ck::string name, fs::Ownership &);
    virtual int mkdir(ck::string name, fs::Ownership &);
    virtual int unlink(ck::string name);
    virtual ck::ref<fs::Node> lookup(ck::string name);
    virtual ck::vec<fs::DirectoryEntry *> dirents(void);
    virtual fs::DirectoryEntry *get_direntry(ck::string name);
    virtual int link(ck::string name, ck::ref<fs::Node> node);
  };


  struct FileSystem : public fs::FileSystem {
    /* memory statistics, to keep tmpfs from using too much memory */
    size_t allowed_pages;
    size_t used_pages;
    spinlock lock;
    uint64_t next_inode = 0;


    FileSystem(ck::string args, int flags);

    virtual ~FileSystem();

    static ck::ref<fs::FileSystem> mount(ck::string options, int flags, ck::string device);
  };

  void init(void);


}  // namespace tmpfs
