/*-
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "test.h"

/*
 * Regression tests for archive_write_disk_set_root_fd(), the
 * directory-anchored extraction mode.
 *
 * These use real writers and real descriptors.  Races are exercised
 * deterministically: the test either renames/replaces an object at a
 * precise point using the test-only hook (compiled into test builds), or
 * it swaps names between two consecutive API calls.  There are no
 * probabilistic retry loops.
 */

#if defined(_WIN32) && !defined(__CYGWIN__)
#define ANCHORED_TESTS_SUPPORTED 0
#else
#define ANCHORED_TESTS_SUPPORTED 1
#endif

#if ANCHORED_TESTS_SUPPORTED

#include <dirent.h>

#ifdef ARCHIVE_WRITE_DISK_TEST_HOOKS
/* Declared here so the test does not need libarchive private headers. */
int archive_write_disk_set_test_hook(struct archive *,
    void (*)(struct archive *, int, const char *, void *), void *);
#endif

static int
open_dir_fd(const char *path)
{
	return (open(path, O_RDONLY | O_DIRECTORY));
}

static int
count_temp_entries(const char *dir)
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	d = opendir(dir);
	if (d == NULL)
		return (-1);
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "libarchive-tmp-", 15) == 0)
			++n;
	}
	closedir(d);
	return (n);
}

static struct archive_entry *
new_entry(const char *path, mode_t mode, la_int64_t size)
{
	struct archive_entry *ae = archive_entry_new();

	assert(ae != NULL);
	archive_entry_copy_pathname(ae, path);
	archive_entry_set_mode(ae, mode);
	archive_entry_set_size(ae, size);
	return (ae);
}

static int
write_file_entry(struct archive *a, const char *path, const char *data)
{
	struct archive_entry *ae;
	int r;

	ae = new_entry(path, AE_IFREG | 0644, (la_int64_t)strlen(data));
	r = archive_write_header(a, ae);
	if (r == ARCHIVE_OK)
		assertEqualInt((int)strlen(data),
		    (int)archive_write_data(a, data, strlen(data)));
	archive_entry_free(ae);
	return (r);
}

static int
finish_file_entry(struct archive *a, const char *path, const char *data)
{
	int r = write_file_entry(a, path, data);

	if (r != ARCHIVE_OK)
		return (r);
	return (archive_write_finish_entry(a));
}

#endif /* ANCHORED_TESTS_SUPPORTED */

/*
 * Basic contract: fd validation, close-on-exec ownership, -1 restore
 * before the first header, refusal to change the root afterwards,
 * failure leaving the previous configuration intact.
 */
DEFINE_TEST(test_write_disk_anchored_root)
{
#if !ANCHORED_TESTS_SUPPORTED
	skipping("Directory-anchored extraction is not supported here");
#else
	struct archive *a;
	struct archive_entry *ae;
	int rfd, nfd, badfd;

	assertMakeDir("root1", 0755);
	assertMakeDir("outside1", 0755);
	assertMakeFile("notadir", 0644, "x");

	rfd = open_dir_fd("root1");
	assert(rfd >= 0);

	assert((a = archive_write_disk_new()) != NULL);
	/* A non-directory descriptor is refused. */
	badfd = open("notadir", O_RDONLY);
	assert(badfd >= 0);
	assertEqualInt(ARCHIVE_FAILED, archive_write_disk_set_root_fd(a, badfd));
	close(badfd);
	/* Invalid descriptor numbers are refused. */
	assertEqualInt(ARCHIVE_FAILED, archive_write_disk_set_root_fd(a, -2));

	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	/*
	 * The writer owns its own close-on-exec duplicate of the root.
	 * Closing the caller's descriptor must not affect extraction.
	 */
	close(rfd);

	/* The duplicate is close-on-exec: check via /dev/fd would be
	 * platform-specific, so verify ownership by closing the original
	 * above and continuing to write successfully. */
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "after-close", "ONE"));
	assertFileContents("ONE", 3, "root1/after-close");

	/* The root is frozen once a header has been seen. */
	assertEqualInt(ARCHIVE_FAILED, archive_write_disk_set_root_fd(a, -1));
	assert(archive_error_string(a) != NULL);
	assert(archive_error_string(a)[0] != '\0');

	/* A second root cannot be installed either, and the old one still
	 * works. */
	assertMakeDir("root2", 0755);
	nfd = open_dir_fd("root2");
	assert(nfd >= 0);
	assertEqualInt(ARCHIVE_FAILED, archive_write_disk_set_root_fd(a, nfd));
	close(nfd);
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "still-root1", "TWO"));
	assertFileContents("TWO", 3, "root1/still-root1");
	assertFileNotExists("root2/still-root1");

	assertEqualInt(ARCHIVE_OK, archive_write_free(a));

	/*
	 * A failed set_root_fd must not discard a previously valid
	 * configuration.
	 */
	assert((a = archive_write_disk_new()) != NULL);
	rfd = open_dir_fd("root1");
	assert(rfd >= 0);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	close(rfd);
	badfd = open("notadir", O_RDONLY);
	assert(badfd >= 0);
	assertEqualInt(ARCHIVE_FAILED, archive_write_disk_set_root_fd(a, badfd));
	close(badfd);
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "kept-config", "KEEP"));
	assertFileContents("KEEP", 4, "root1/kept-config");
	assertEqualInt(ARCHIVE_OK, archive_write_free(a));

	/*
	 * Before the first header, -1 restores ordinary (process-relative)
	 * behaviour.
	 */
	assert((a = archive_write_disk_new()) != NULL);
	rfd = open_dir_fd("root1");
	assert(rfd >= 0);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	close(rfd);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, -1));
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "direct-cwd", "CWD"));
	assertFileContents("CWD", 3, "direct-cwd");
	assertFileNotExists("root1/direct-cwd");
	assertEqualInt(ARCHIVE_OK, archive_write_free(a));

	/*
	 * Without any call to the new entry point, the original relative
	 * behaviour is untouched.
	 */
	assert((a = archive_write_disk_new()) != NULL);
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "legacy", "LEGACY"));
	assertFileContents("LEGACY", 6, "legacy");
	assertEqualInt(ARCHIVE_OK, archive_write_free(a));
#endif
}

/*
 * Path policy: absolute paths, every '..' component, empty paths and
 * special nodes are refused; a failure does not poison later entries.
 */
DEFINE_TEST(test_write_disk_anchored_paths)
{
#if !ANCHORED_TESTS_SUPPORTED
	skipping("Directory-anchored extraction is not supported here");
#else
	struct archive *a;
	struct archive_entry *ae;
	struct stat st;
	int rfd;

	assertMakeDir("pr", 0755);
	assertMakeDir("pr/dir", 0755);
	rfd = open_dir_fd("pr");
	assert(rfd >= 0);
	assert((a = archive_write_disk_new()) != NULL);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	close(rfd);

	/* Absolute path. */
	ae = new_entry("/etc/passwd-anchored-test", AE_IFREG | 0644, 1);
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	assert(archive_error_string(a) != NULL);

	/* Any '..', even in the middle and even if it would cancel out. */
	ae = new_entry("dir/../escape", AE_IFREG | 0644, 1);
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	ae = new_entry("a/../../escape2", AE_IFREG | 0644, 1);
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	assertFileNotExists("escape");
	assertFileNotExists("escape2");

	/* Empty path. */
	ae = new_entry("", AE_IFREG | 0644, 1);
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);

	/* Special nodes are refused before creation. */
	ae = new_entry("a-fifo", AE_IFIFO | 0644, 0);
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	assertEqualInt(-1, lstat("pr/a-fifo", &st));

	/* Options this mode cannot honour are refused before creation. */
	assertEqualInt(ARCHIVE_OK,
	    archive_write_disk_set_options(a, ARCHIVE_EXTRACT_ACL));
	ae = new_entry("with-acl", AE_IFREG | 0644, 1);
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	assertFileNotExists("pr/with-acl");

	assertEqualInt(ARCHIVE_OK,
	    archive_write_disk_set_options(a, ARCHIVE_EXTRACT_XATTR));
	ae = new_entry("with-xattr", AE_IFREG | 0644, 1);
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	assertFileNotExists("pr/with-xattr");

	assertEqualInt(ARCHIVE_OK,
	    archive_write_disk_set_options(a, ARCHIVE_EXTRACT_OWNER));
	ae = new_entry("with-owner", AE_IFREG | 0644, 1);
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	assertFileNotExists("pr/with-owner");

	/* Normal entries still work after all of those failures. */
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_options(a, 0));
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "dir/good", "GOOD"));
	assertFileContents("GOOD", 4, "pr/dir/good");

	assertEqualInt(ARCHIVE_OK, archive_write_free(a));
#endif
}

/*
 * Root rename + the old root name replaced by a symlink: later writes
 * must continue to land in the directory object we anchored to.
 */
DEFINE_TEST(test_write_disk_anchored_root_rename)
{
#if !ANCHORED_TESTS_SUPPORTED
	skipping("Directory-anchored extraction is not supported here");
#else
	struct archive *a;
	int rfd;

	assertMakeDir("ra", 0755);
	assertMakeDir("ra-outside", 0755);

	rfd = open_dir_fd("ra");
	assert(rfd >= 0);
	assert((a = archive_write_disk_new()) != NULL);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	close(rfd);

	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "first", "FIRST"));

	/* Rename the anchored directory, then plant a symlink with the old
	 * name pointing outside the tree. */
	assertEqualInt(0, rename("ra", "ra-renamed"));
	assertMakeSymlink("ra", "ra-outside", 1);

	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "second", "SECOND"));
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "sub/third", "THIRD"));
	assertEqualInt(ARCHIVE_OK, archive_write_free(a));

	assertFileContents("FIRST", 5, "ra-renamed/first");
	assertFileContents("SECOND", 6, "ra-renamed/second");
	assertFileContents("THIRD", 5, "ra-renamed/sub/third");
	assertFileNotExists("ra-outside/second");
	assertFileNotExists("ra-outside/sub/third");
#endif
}

/*
 * Two writers anchored at different roots, interleaved.  Their
 * operations must not leak into each other's tree.
 */
DEFINE_TEST(test_write_disk_anchored_two_roots)
{
#if !ANCHORED_TESTS_SUPPORTED
	skipping("Directory-anchored extraction is not supported here");
#else
	struct archive *a, *b;
	int rfd;

	assertMakeDir("rootA", 0755);
	assertMakeDir("rootB", 0755);

	rfd = open_dir_fd("rootA");
	assert(rfd >= 0);
	assert((a = archive_write_disk_new()) != NULL);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	close(rfd);

	rfd = open_dir_fd("rootB");
	assert(rfd >= 0);
	assert((b = archive_write_disk_new()) != NULL);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(b, rfd));
	close(rfd);

	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "same", "AAAA"));
	assertEqualInt(ARCHIVE_OK, finish_file_entry(b, "same", "BBBB"));
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "dir/x", "AX"));
	assertEqualInt(ARCHIVE_OK, finish_file_entry(b, "dir/y", "BY"));

	assertEqualInt(ARCHIVE_OK, archive_write_free(a));
	assertEqualInt(ARCHIVE_OK, archive_write_free(b));

	assertFileContents("AAAA", 4, "rootA/same");
	assertFileContents("BBBB", 4, "rootB/same");
	assertFileContents("AX", 2, "rootA/dir/x");
	assertFileContents("BY", 2, "rootB/dir/y");
	assertFileNotExists("rootA/dir/y");
	assertFileNotExists("rootB/dir/x");
#endif
}

/*
 * A parent component replaced by a symlink after it was used once must
 * not be traversed; after restoring the real directory, writes resume.
 */
DEFINE_TEST(test_write_disk_anchored_parent_symlink)
{
#if !ANCHORED_TESTS_SUPPORTED
	skipping("Directory-anchored extraction is not supported here");
#else
	struct archive *a;
	struct archive_entry *ae;
	int rfd;

	assertMakeDir("ps", 0755);
	assertMakeDir("ps/p", 0755);
	assertMakeDir("ps-outside", 0755);

	rfd = open_dir_fd("ps");
	assert(rfd >= 0);
	assert((a = archive_write_disk_new()) != NULL);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	close(rfd);

	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "p/one", "ONE"));

	/* Replace the parent component with a symlink pointing outside. */
	assertEqualInt(0, rename("ps/p", "ps/p-real"));
	assertMakeSymlink("ps/p", "../ps-outside", 1);

	assertEqualInt(ARCHIVE_FAILED, write_file_entry(a, "p/two", "TWO"));
	/* The failed header leaves the writer usable. */
	assertFileNotExists("ps-outside/two");

	/* Restore the real directory and continue. */
	assertEqualInt(0, unlink("ps/p"));
	assertEqualInt(0, rename("ps/p-real", "ps/p"));
	ae = NULL;
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "p/three", "THREE"));
	assertFileContents("THREE", 5, "ps/p/three");
	assertFileContents("ONE", 3, "ps/p/one");

	assertEqualInt(ARCHIVE_OK, archive_write_free(a));
#endif
}

/* Deterministic hardlink-race state. */
#ifdef ARCHIVE_WRITE_DISK_TEST_HOOKS
static int hook_root_fd = -1;

static void
hardlink_source_swap_hook(struct archive *a, int op, const char *path,
    void *data)
{
	(void)a;
	(void)data;
	if (op != 0 || hook_root_fd < 0)
		return;
	/*
	 * Replace the validated source name with a different, previously
	 * created file.  The original inode stays alive (the writer holds
	 * it open), so the replacement cannot accidentally reuse it.
	 */
	unlinkat(hook_root_fd, path, 0);
	if (renameat(hook_root_fd, "src/replacement",
	    hook_root_fd, path) != 0)
		unlinkat(hook_root_fd, path, 0);
}
#endif

/*
 * Hardlinks are only accepted for regular files inside the root, with
 * both parents resolved through the anchor.  If the source is replaced
 * between verification and linking, nothing is published at the final
 * name and no temporary is left behind.
 */
DEFINE_TEST(test_write_disk_anchored_hardlink)
{
#if !ANCHORED_TESTS_SUPPORTED
	skipping("Directory-anchored extraction is not supported here");
#else
	struct archive *a;
	struct archive_entry *ae;
	struct stat st;
	int rfd;

	assertMakeDir("hl", 0755);
	assertMakeDir("hl/src", 0755);
	assertMakeDir("hl/dst", 0755);
	assertMakeFile("hl/src/data", 0644, "SOURCE-DATA");
	assertMakeSymlink("hl/src/slink", "data", 0);
	assertMakeDir("hl-outside", 0755);
	assertMakeFile("hl-outside/target", 0644, "OUTSIDE");

	rfd = open_dir_fd("hl");
	assert(rfd >= 0);
	assert((a = archive_write_disk_new()) != NULL);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	close(rfd);

	/* Out-of-bounds hardlink targets are rejected. */
	ae = new_entry("dst/esc1", AE_IFREG | 0644, 0);
	archive_entry_set_hardlink(ae, "../hl-outside/target");
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	ae = new_entry("dst/esc2", AE_IFREG | 0644, 0);
	archive_entry_set_hardlink(ae, "/etc/passwd");
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	assertFileNotExists("hl/dst/esc1");
	assertFileNotExists("hl/dst/esc2");
	assertFileNotExists("hl-outside/esc1");

	/* A symlink is not an acceptable hardlink source. */
	ae = new_entry("dst/from-symlink", AE_IFREG | 0644, 0);
	archive_entry_set_hardlink(ae, "src/slink");
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	assertFileNotExists("hl/dst/from-symlink");

	/* A plain hardlink succeeds and shares the source's inode. */
	ae = new_entry("dst/alias", AE_IFREG | 0644, 0);
	archive_entry_set_hardlink(ae, "src/data");
	assertEqualInt(ARCHIVE_OK, archive_write_header(a, ae));
	assertEqualInt(ARCHIVE_OK, archive_write_finish_entry(a));
	archive_entry_free(ae);
	assertIsHardlink("hl/src/data", "hl/dst/alias");
	assertFileContents("SOURCE-DATA", 11, "hl/dst/alias");
	assertEqualInt(0, count_temp_entries("hl/dst"));

#ifdef ARCHIVE_WRITE_DISK_TEST_HOOKS
	/* Now the source-swap race. */
	assertMakeFile("hl/src/replacement", 0644, "REPLACED");
	rfd = open_dir_fd("hl");
	assert(rfd >= 0);
	hook_root_fd = rfd;
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_test_hook(a,
	    hardlink_source_swap_hook, NULL));
	ae = new_entry("dst/raced", AE_IFREG | 0644, 0);
	archive_entry_set_hardlink(ae, "src/data");
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_test_hook(a, NULL,
	    NULL));
	close(hook_root_fd);
	hook_root_fd = -1;

	/* The unverified replacement must not be published. */
	assertFileNotExists("hl/dst/raced");
	/* And the writer's private temporary must be gone. */
	assertEqualInt(0, count_temp_entries("hl/dst"));
	/* The verified original is still reachable under its own name. */
	assertEqualInt(0, lstat("hl/src/data", &st));
	assert(S_ISREG(st.st_mode));
#else
	skipping("test hook not built in");
#endif

	assertEqualInt(ARCHIVE_OK, archive_write_free(a));
#endif
}

/*
 * ARCHIVE_EXTRACT_SAFE_WRITES creates and commits its temporary in the
 * destination parent directory.  A failed commit (renaming a regular
 * file over a non-empty directory) must not leave the temporary behind
 * and must not damage the existing object.
 */
DEFINE_TEST(test_write_disk_anchored_safe_writes)
{
#if !ANCHORED_TESTS_SUPPORTED
	skipping("Directory-anchored extraction is not supported here");
#else
	struct archive *a;
	struct archive_entry *ae;
	int rfd, r;

	assertMakeDir("sw", 0755);
	assertMakeDir("sw/sub", 0755);
	assertMakeDir("sw/sub/blocked", 0755);
	assertMakeFile("sw/sub/blocked/keep", 0644, "KEEP");
	assertMakeDir("sw-outside", 0755);

	rfd = open_dir_fd("sw");
	assert(rfd >= 0);
	assert((a = archive_write_disk_new()) != NULL);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	close(rfd);
	assertEqualInt(ARCHIVE_OK,
	    archive_write_disk_set_options(a, ARCHIVE_EXTRACT_SAFE_WRITES));

	/* Successful atomic write. */
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "sub/ok", "ATOMIC"));
	assertFileContents("ATOMIC", 6, "sw/sub/ok");
	assertEqualInt(0, count_temp_entries("sw/sub"));

	/* Commit that must fail: the final name is occupied by a non-empty
	 * directory, and rename(2) refuses to replace it. */
	ae = new_entry("sub/blocked", AE_IFREG | 0644, 4);
	assertEqualInt(ARCHIVE_OK, archive_write_header(a, ae));
	assertEqualInt(4, (int)archive_write_data(a, "FAIL", 4));
	r = archive_write_finish_entry(a);
	assertEqualInt(ARCHIVE_FAILED, r);
	archive_entry_free(ae);

	/* Existing object untouched, private temporary cleaned up. */
	assertFileContents("KEEP", 4, "sw/sub/blocked/keep");
	assertEqualInt(0, count_temp_entries("sw/sub"));
	assertFileNotExists("sw-outside/blocked");

	/* Writer is still usable afterwards. */
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "sub/after", "AFTER"));
	assertFileContents("AFTER", 5, "sw/sub/after");

	assertEqualInt(ARCHIVE_OK, archive_write_free(a));
#endif
}

/*
 * Deferred directory mode/times are bound to the directory object that
 * was created, not to its name.  Replacing the name must not cause the
 * replacement directory to receive the archived attributes.
 */
DEFINE_TEST(test_write_disk_anchored_deferred_dir)
{
#if !ANCHORED_TESTS_SUPPORTED
	skipping("Directory-anchored extraction is not supported here");
#else
	struct archive *a;
	struct archive_entry *ae;
	struct stat st;
	int rfd;

	assertMakeDir("dd", 0755);
	rfd = open_dir_fd("dd");
	assert(rfd >= 0);
	assert((a = archive_write_disk_new()) != NULL);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	close(rfd);
	assertEqualInt(ARCHIVE_OK,
	    archive_write_disk_set_options(a,
	    ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME));

	/* Directory entry with a distinctive mode, deferred to close. */
	ae = new_entry("target", AE_IFDIR | 0700, 0);
	archive_entry_set_mtime(ae, 1000000000, 0);
	assertEqualInt(ARCHIVE_OK, archive_write_header(a, ae));
	assertEqualInt(ARCHIVE_OK, archive_write_finish_entry(a));
	archive_entry_free(ae);

	/* Rename the created directory and put a different directory at
	 * the archived name, with a different mode. */
	assertEqualInt(0, rename("dd/target", "dd/target-renamed"));
	assertMakeDir("dd/target", 0755);
	assertEqualInt(0, chmod("dd/target", 0755));

	/* Closing applies the deferred fixup through the retained object. */
	assertEqualInt(ARCHIVE_OK, archive_write_close(a));
	assertEqualInt(ARCHIVE_OK, archive_write_free(a));

	assertEqualInt(0, lstat("dd/target", &st));
	assert(S_ISDIR(st.st_mode));
	assertEqualInt(0755, (int)(st.st_mode & 0777));
	assertEqualInt(0, lstat("dd/target-renamed", &st));
	assert(S_ISDIR(st.st_mode));
	assertEqualInt(0700, (int)(st.st_mode & 0777));
	assertEqualInt(1000000000, (long)st.st_mtime);
#endif
}

/*
 * A symlink entry is stored with its target verbatim (relative or
 * absolute), but later entries must not traverse it.  Regular files and
 * directories refuse an existing final symlink.
 */
DEFINE_TEST(test_write_disk_anchored_symlink)
{
#if !ANCHORED_TESTS_SUPPORTED
	skipping("Directory-anchored extraction is not supported here");
#else
	struct archive *a;
	struct archive_entry *ae;
	struct stat st;
	char target[256];
	int rfd;

	assertMakeDir("sl", 0755);
	assertMakeDir("sl-outside", 0755);
	assertMakeFile("sl-outside/victim", 0644, "SAFE");

	rfd = open_dir_fd("sl");
	assert(rfd >= 0);
	assert((a = archive_write_disk_new()) != NULL);
	assertEqualInt(ARCHIVE_OK, archive_write_disk_set_root_fd(a, rfd));
	close(rfd);

	/* Relative target stored verbatim. */
	ae = new_entry("rel", AE_IFLNK | 0777, 0);
	archive_entry_set_symlink(ae, "../sl-outside");
	assertEqualInt(ARCHIVE_OK, archive_write_header(a, ae));
	assertEqualInt(ARCHIVE_OK, archive_write_finish_entry(a));
	archive_entry_free(ae);
	assertIsSymlink("sl/rel", "../sl-outside", 0);

	/* Absolute target stored verbatim. */
	snprintf(target, sizeof(target), "%s/sl-outside", testworkdir);
	ae = new_entry("abs", AE_IFLNK | 0777, 0);
	archive_entry_set_symlink(ae, target);
	assertEqualInt(ARCHIVE_OK, archive_write_header(a, ae));
	assertEqualInt(ARCHIVE_OK, archive_write_finish_entry(a));
	archive_entry_free(ae);
	assertIsSymlink("sl/abs", target, 0);

	/* No later entry may traverse either link. */
	assertEqualInt(ARCHIVE_FAILED, write_file_entry(a, "rel/victim", "BAD"));
	assertEqualInt(ARCHIVE_FAILED, write_file_entry(a, "abs/victim", "BAD"));
	assertFileContents("SAFE", 4, "sl-outside/victim");

	/* A regular file must not be extracted over an existing symlink. */
	assertEqualInt(ARCHIVE_FAILED, write_file_entry(a, "rel", "BAD"));
	assertIsSymlink("sl/rel", "../sl-outside", 0);

	/* Nor may a directory entry adopt an existing symlink as a dir. */
	ae = new_entry("abs", AE_IFDIR | 0755, 0);
	assertEqualInt(ARCHIVE_FAILED, archive_write_header(a, ae));
	archive_entry_free(ae);
	assertIsSymlink("sl/abs", target, 0);

	/* "." names the root directory object. */
	assertEqualInt(ARCHIVE_OK,
	    archive_write_disk_set_options(a, ARCHIVE_EXTRACT_PERM));
	ae = new_entry(".", AE_IFDIR | 0755, 0);
	assertEqualInt(ARCHIVE_OK, archive_write_header(a, ae));
	assertEqualInt(ARCHIVE_OK, archive_write_finish_entry(a));
	archive_entry_free(ae);
	assertEqualInt(0, lstat("sl", &st));
	assert(S_ISDIR(st.st_mode));

	assertEqualInt(ARCHIVE_OK,
	    archive_write_disk_set_options(a, 0));
	assertEqualInt(ARCHIVE_OK, finish_file_entry(a, "after", "AFTER"));
	assertFileContents("AFTER", 5, "sl/after");

	assertEqualInt(ARCHIVE_OK, archive_write_free(a));
#endif
}
