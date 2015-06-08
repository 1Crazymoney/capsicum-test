#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "capsicum.h"
#include "capsicum-test.h"
#include "syscalls.h"

// Check an open call works and close the resulting fd.
#define EXPECT_OPEN_OK(f) do { \
    int fd = f;                \
    EXPECT_OK(fd);             \
    close(fd);                 \
  } while (0)

static void CreateFile(const char *filename, const char *contents) {
  int fd = open(filename, O_CREAT|O_RDWR, 0644);
  EXPECT_OK(fd);
  EXPECT_OK(write(fd, contents, strlen(contents)));
  close(fd);
}

// Test openat(2) in a variety of sitations to ensure that it obeys Capsicum
// "strict relative" rules:
//
// 1. Use strict relative lookups in capability mode or when operating
//    relative to a capability.
// 2. When performing strict relative lookups, absolute paths (including
//    symlinks to absolute paths) are not allowed, nor are paths containing
//    '..' components.
//
// These rules apply when:
//  - the directory FD is a Capsicum capability
//  - the process is in capability mode
//  - the openat(2) operation includes the O_BENEATH flag.
FORK_TEST(Openat, Relative) {
  int etc = open("/etc/", O_RDONLY);
  EXPECT_OK(etc);

  cap_rights_t r_base;
  cap_rights_init(&r_base, CAP_READ, CAP_WRITE, CAP_SEEK, CAP_LOOKUP, CAP_FCNTL, CAP_IOCTL);
  cap_rights_t r_ro;
  cap_rights_init(&r_ro, CAP_READ);
  cap_rights_t r_rl;
  cap_rights_init(&r_rl, CAP_READ, CAP_LOOKUP);

  int etc_cap = dup(etc);
  EXPECT_OK(etc_cap);
  EXPECT_OK(cap_rights_limit(etc_cap, &r_ro));
  int etc_cap_ro = dup(etc);
  EXPECT_OK(etc_cap_ro);
  EXPECT_OK(cap_rights_limit(etc_cap_ro, &r_rl));
  int etc_cap_base = dup(etc);
  EXPECT_OK(etc_cap_base);
  EXPECT_OK(cap_rights_limit(etc_cap_base, &r_base));
#ifdef HAVE_CAP_FCNTLS_LIMIT
  // Also limit fcntl(2) subrights.
  EXPECT_OK(cap_fcntls_limit(etc_cap_base, CAP_FCNTL_GETFL));
#endif
#ifdef HAVE_CAP_IOCTLS_LIMIT
  // Also limit ioctl(2) subrights.
  cap_ioctl_t ioctl_nread = FIONREAD;
  EXPECT_OK(cap_ioctls_limit(etc_cap_base, &ioctl_nread, 1));
#endif

  // openat(2) with regular file descriptors in non-capability mode
  // Should Just Work (tm).
  EXPECT_OPEN_OK(openat(etc, "/etc/passwd", O_RDONLY));
  EXPECT_OPEN_OK(openat(AT_FDCWD, "/etc/passwd", O_RDONLY));
  EXPECT_OPEN_OK(openat(etc, "passwd", O_RDONLY));
  EXPECT_OPEN_OK(openat(etc, "../etc/passwd", O_RDONLY));

  // Lookups relative to capabilities should be strictly relative.
  // When not in capability mode, we don't actually require CAP_LOOKUP.
  EXPECT_OPEN_OK(openat(etc_cap_ro, "passwd", O_RDONLY));
  EXPECT_OPEN_OK(openat(etc_cap_base, "passwd", O_RDONLY));

  // Performing openat(2) on a path with leading slash ignores
  // the provided directory FD.
  EXPECT_OPEN_OK(openat(etc_cap_ro, "/etc/passwd", O_RDONLY));
  EXPECT_OPEN_OK(openat(etc_cap_base, "/etc/passwd", O_RDONLY));
  // Relative lookups that go upward are not allowed.
  EXPECT_FAIL_TRAVERSAL(openat(etc_cap_ro, "../etc/passwd", O_RDONLY));
  EXPECT_FAIL_TRAVERSAL(openat(etc_cap_base, "../etc/passwd", O_RDONLY));

  // A file opened relative to a capability should itself be a capability.
  int fd = openat(etc_cap_base, "passwd", O_RDONLY);
  EXPECT_OK(fd);
  cap_rights_t rights;
  EXPECT_OK(cap_rights_get(fd, &rights));
  EXPECT_RIGHTS_IN(&rights, &r_base);
#ifdef HAVE_CAP_FCNTLS_LIMIT
  cap_fcntl_t fcntls;
  EXPECT_OK(cap_fcntls_get(fd, &fcntls));
  EXPECT_EQ((cap_fcntl_t)CAP_FCNTL_GETFL, fcntls);
#endif
#ifdef HAVE_CAP_IOCTLS_LIMIT
  cap_ioctl_t ioctls[16];
  ssize_t nioctls;
  memset(ioctls, 0, sizeof(ioctls));
  nioctls = cap_ioctls_get(fd, ioctls, 16);
  EXPECT_OK(nioctls);
  EXPECT_EQ(1, nioctls);
  EXPECT_EQ((cap_ioctl_t)FIONREAD, ioctls[0]);
#endif
  close(fd);

  // Enter capability mode; now ALL lookups are strictly relative.
  EXPECT_OK(cap_enter());

  // Relative lookups on regular files or capabilities with CAP_LOOKUP
  // ought to succeed.
  EXPECT_OPEN_OK(openat(etc, "passwd", O_RDONLY));
  EXPECT_OPEN_OK(openat(etc_cap_ro, "passwd", O_RDONLY));
  EXPECT_OPEN_OK(openat(etc_cap_base, "passwd", O_RDONLY));

  // Lookup relative to capabilities without CAP_LOOKUP should fail.
  EXPECT_NOTCAPABLE(openat(etc_cap, "passwd", O_RDONLY));

  // Absolute lookups should fail.
  EXPECT_CAPMODE(openat(AT_FDCWD, "/etc/passwd", O_RDONLY));
  EXPECT_FAIL_TRAVERSAL(openat(etc, "/etc/passwd", O_RDONLY));
  EXPECT_FAIL_TRAVERSAL(openat(etc_cap_ro, "/etc/passwd", O_RDONLY));

  // Lookups containing '..' should fail in capability mode.
  EXPECT_FAIL_TRAVERSAL(openat(etc, "../etc/passwd", O_RDONLY));
  EXPECT_FAIL_TRAVERSAL(openat(etc_cap_ro, "../etc/passwd", O_RDONLY));
  EXPECT_FAIL_TRAVERSAL(openat(etc_cap_base, "../etc/passwd", O_RDONLY));

  fd = openat(etc, "passwd", O_RDONLY);
  EXPECT_OK(fd);

  // A file opened relative to a capability should itself be a capability.
  fd = openat(etc_cap_base, "passwd", O_RDONLY);
  EXPECT_OK(fd);
  EXPECT_OK(cap_rights_get(fd, &rights));
  EXPECT_RIGHTS_IN(&rights, &r_base);
  close(fd);

  fd = openat(etc_cap_ro, "passwd", O_RDONLY);
  EXPECT_OK(fd);
  EXPECT_OK(cap_rights_get(fd, &rights));
  EXPECT_RIGHTS_IN(&rights, &r_rl);
  close(fd);
}

#define TOPDIR "/tmp/cap_topdir"
#define SUBDIR_ABS TOPDIR "/subdir"
class OpenatTest : public ::testing::Test {
 public:
  // Build a collection of files, subdirs and symlinks:
  //  /tmp/cap_topdir/
  //                 /topfile
  //                 /subdir/
  //                 /subdir/bottomfile
  //                 /symlink.samedir       -> topfile
  //                 /symlink.down          -> subdir/bottomfile
  //                 /symlink.absolute_in   -> /tmp/cap_topdir/topfile
  //                 /symlink.absolute_out  -> /etc/passwd
  //                 /symlink.relative_in   -> ../../tmp/cap_topdir/topfile
  //                 /symlink.relative_out  -> ../../etc/passwd
  //                 /subdir/symlink.up     -> ../topfile
  OpenatTest() {
    // Create a couple of nested directories
    int rc = mkdir(TOPDIR, 0755);
    EXPECT_OK(rc);
    if (rc < 0) EXPECT_EQ(EEXIST, errno);
    rc = mkdir(SUBDIR_ABS, 0755);
    EXPECT_OK(rc);
    if (rc < 0) EXPECT_EQ(EEXIST, errno);
    // Create normal files in each.
    CreateFile(TOPDIR "/topfile", "Top-level file");
    CreateFile(SUBDIR_ABS "/bottomfile", "File in subdirectory");

    // Create various symlinks
    EXPECT_OK(symlink("topfile", TOPDIR "/symlink.samedir"));
    EXPECT_OK(symlink("subdir/bottomfile", TOPDIR "/symlink.down"));
    EXPECT_OK(symlink(TOPDIR "/topfile", TOPDIR "/symlink.absolute_in"));
    EXPECT_OK(symlink("/etc/passwd", TOPDIR "/symlink.absolute_out"));
    EXPECT_OK(symlink("../.." TOPDIR "/topfile", TOPDIR "/symlink.relative_in"));
    EXPECT_OK(symlink("../../etc/passwd", TOPDIR "/symlink.relative_out"));
    EXPECT_OK(symlink("../topfile", SUBDIR_ABS "/symlink.up"));

    // Open directory FDs for those directories and for cwd.
    dir_fd_ = open(TOPDIR, O_RDONLY);
    EXPECT_OK(dir_fd_);
    sub_fd_ = open(SUBDIR_ABS, O_RDONLY);
    EXPECT_OK(sub_fd_);
    cwd_ = openat(AT_FDCWD, ".", O_RDONLY);
    EXPECT_OK(cwd_);
    // Move into the directory for the test.
    EXPECT_OK(fchdir(dir_fd_));
  }
  ~OpenatTest() {
    fchdir(cwd_);
    close(cwd_);
    close(sub_fd_);
    close(dir_fd_);
    unlink(SUBDIR_ABS "/symlink.up");
    unlink(TOPDIR "/symlink.absolute_in");
    unlink(TOPDIR "/symlink.absolute_out");
    unlink(TOPDIR "/symlink.relative_in");
    unlink(TOPDIR "/symlink.relative_out");
    unlink(TOPDIR "/symlink.down");
    unlink(TOPDIR "/symlink.samedir");
    unlink(SUBDIR_ABS "/bottomfile");
    unlink(TOPDIR "/topfile");
    rmdir(SUBDIR_ABS);
    rmdir(TOPDIR);
  }

  // Check openat(2) policing that is common across capabilities, capability mode and O_BENEATH.
  void CheckPolicing(int oflag) {
    // OK for normal access.
    EXPECT_OPEN_OK(openat(dir_fd_, "topfile", O_RDONLY|oflag));
    EXPECT_OPEN_OK(openat(dir_fd_, "subdir/bottomfile", O_RDONLY|oflag));
    EXPECT_OPEN_OK(openat(sub_fd_, "bottomfile", O_RDONLY|oflag));
    EXPECT_OPEN_OK(openat(sub_fd_, ".", O_RDONLY|oflag));

    // Can't open paths with ".." in them.
    EXPECT_FAIL_TRAVERSAL(openat(dir_fd_, "subdir/../topfile", O_RDONLY|oflag));
    EXPECT_FAIL_TRAVERSAL(openat(sub_fd_, "../topfile", O_RDONLY|oflag));
    EXPECT_FAIL_TRAVERSAL(openat(sub_fd_, "../subdir/bottomfile", O_RDONLY|oflag));
    EXPECT_FAIL_TRAVERSAL(openat(sub_fd_, "..", O_RDONLY|oflag));

    // Check that we can't escape the top directory by the cunning
    // ruse of going via a subdirectory.
    EXPECT_FAIL_TRAVERSAL(openat(dir_fd_, "subdir/../../etc/passwd", O_RDONLY|oflag));

    // Should only be able to open symlinks that stay within the directory.
    EXPECT_OPEN_OK(openat(dir_fd_, "symlink.samedir", O_RDONLY|oflag));
    EXPECT_OPEN_OK(openat(dir_fd_, "symlink.down", O_RDONLY|oflag));
    EXPECT_FAIL_TRAVERSAL(openat(dir_fd_, "symlink.absolute_in", O_RDONLY|oflag));
    EXPECT_FAIL_TRAVERSAL(openat(dir_fd_, "symlink.absolute_out", O_RDONLY|oflag));
    EXPECT_FAIL_TRAVERSAL(openat(dir_fd_, "symlink.relative_in", O_RDONLY|oflag));
    EXPECT_FAIL_TRAVERSAL(openat(dir_fd_, "symlink.relative_out", O_RDONLY|oflag));
    EXPECT_FAIL_TRAVERSAL(openat(sub_fd_, "symlink.up", O_RDONLY|oflag));

    // Although recall that O_NOFOLLOW prevents symlink following.
    EXPECT_SYSCALL_FAIL(ELOOP, openat(dir_fd_, "symlink.samedir", O_RDONLY|O_NOFOLLOW|oflag));
    EXPECT_SYSCALL_FAIL(ELOOP, openat(dir_fd_, "symlink.down", O_RDONLY|O_NOFOLLOW|oflag));
  }

 protected:
  int dir_fd_;
  int sub_fd_;
  int cwd_;
};

TEST_F(OpenatTest, WithCapability) {
  // Any kind of symlink can be opened relative to an ordinary directory FD.
  EXPECT_OPEN_OK(openat(dir_fd_, "symlink.samedir", O_RDONLY));
  EXPECT_OPEN_OK(openat(dir_fd_, "symlink.down", O_RDONLY));
  EXPECT_OPEN_OK(openat(dir_fd_, "symlink.absolute_in", O_RDONLY));
  EXPECT_OPEN_OK(openat(dir_fd_, "symlink.absolute_out", O_RDONLY));
  EXPECT_OPEN_OK(openat(dir_fd_, "symlink.relative_in", O_RDONLY));
  EXPECT_OPEN_OK(openat(dir_fd_, "symlink.relative_out", O_RDONLY));
  EXPECT_OPEN_OK(openat(sub_fd_, "symlink.up", O_RDONLY));

  // Now make both DFDs into Capsicum capabilities.
  cap_rights_t r_rl;
  cap_rights_init(&r_rl, CAP_READ, CAP_LOOKUP, CAP_FCHDIR);
  EXPECT_OK(cap_rights_limit(dir_fd_, &r_rl));
  EXPECT_OK(cap_rights_limit(sub_fd_, &r_rl));
  CheckPolicing(0);
  // Use of AT_FDCWD is independent of use of a capability.
  // Can open paths starting with "/" against a capability dfd, because the dfd is ignored.
}

FORK_TEST_F(OpenatTest, InCapabilityMode) {
  EXPECT_OK(cap_enter());  // Enter capability mode
  CheckPolicing(0);

  // Use of AT_FDCWD is banned in capability mode.
  EXPECT_CAPMODE(openat(AT_FDCWD, "topfile", O_RDONLY));
  EXPECT_CAPMODE(openat(AT_FDCWD, "subdir/bottomfile", O_RDONLY));
  EXPECT_CAPMODE(openat(AT_FDCWD, "/etc/passwd", O_RDONLY));

  // Can't open paths starting with "/" in capability mode.
  EXPECT_FAIL_TRAVERSAL(openat(dir_fd_, "/etc/passwd", O_RDONLY));
  EXPECT_FAIL_TRAVERSAL(openat(sub_fd_, "/etc/passwd", O_RDONLY));
}

#ifdef O_BENEATH
TEST_F(OpenatTest, WithFlag) {
  CheckPolicing(O_BENEATH);

  // Check with AT_FDCWD.
  EXPECT_OPEN_OK(openat(AT_FDCWD, "topfile", O_RDONLY|O_BENEATH));
  EXPECT_OPEN_OK(openat(AT_FDCWD, "subdir/bottomfile", O_RDONLY|O_BENEATH));

  // Can't open paths starting with "/" with O_BENEATH specified.
  EXPECT_FAIL_TRAVERSAL(openat(AT_FDCWD, "/etc/passwd", O_RDONLY|O_BENEATH));
  EXPECT_FAIL_TRAVERSAL(openat(dir_fd_, "/etc/passwd", O_RDONLY|O_BENEATH));
  EXPECT_FAIL_TRAVERSAL(openat(sub_fd_, "/etc/passwd", O_RDONLY|O_BENEATH));
}
#endif
