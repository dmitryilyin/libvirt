/*
 * Copyright (C) 2008-2013 Red Hat, Inc.
 * Copyright (C) 2008 IBM Corp.
 *
 * lxc_container.c: file description
 *
 * Authors:
 *  David L. Leskovec <dlesko at linux.vnet.ibm.com>
 *  Daniel P. Berrange <berrange@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mntent.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

/* Yes, we want linux private one, for _syscall2() macro */
#include <linux/unistd.h>

/* For MS_MOVE */
#include <stdint.h>
#include <linux/fs.h>

#if WITH_CAPNG
# include <cap-ng.h>
#endif

#if WITH_BLKID
# include <blkid/blkid.h>
#endif

#if WITH_SELINUX
# include <selinux/selinux.h>
#endif

#include "virerror.h"
#include "virlog.h"
#include "lxc_container.h"
#include "viralloc.h"
#include "virnetdevveth.h"
#include "viruuid.h"
#include "virfile.h"
#include "virusb.h"
#include "vircommand.h"
#include "virnetdev.h"
#include "virprocess.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_LXC

/*
 * GLibc headers are behind the kernel, so we define these
 * constants if they're not present already.
 */

#ifndef CLONE_NEWPID
# define CLONE_NEWPID  0x20000000
#endif
#ifndef CLONE_NEWUTS
# define CLONE_NEWUTS  0x04000000
#endif
#ifndef CLONE_NEWUSER
# define CLONE_NEWUSER 0x10000000
#endif
#ifndef CLONE_NEWIPC
# define CLONE_NEWIPC  0x08000000
#endif
#ifndef CLONE_NEWNET
# define CLONE_NEWNET  0x40000000 /* New network namespace */
#endif

/* messages between parent and container */
typedef char lxc_message_t;
#define LXC_CONTINUE_MSG 'c'

typedef struct __lxc_child_argv lxc_child_argv_t;
struct __lxc_child_argv {
    virDomainDefPtr config;
    virSecurityManagerPtr securityDriver;
    size_t nveths;
    char **veths;
    int monitor;
    size_t npassFDs;
    int *passFDs;
    size_t nttyPaths;
    char **ttyPaths;
    int handshakefd;
};

static int lxcContainerMountFSBlock(virDomainFSDefPtr fs,
                                    const char *srcprefix,
                                    const char *sec_mount_options);


/*
 * reboot(LINUX_REBOOT_CMD_CAD_ON) will return -EINVAL
 * in a child pid namespace if container reboot support exists.
 * Otherwise, it will either succeed or return -EPERM.
 */
ATTRIBUTE_NORETURN static int
lxcContainerRebootChild(void *argv)
{
    int *cmd = argv;
    int ret;

    ret = reboot(*cmd);
    if (ret == -1 && errno == EINVAL)
        _exit(1);
    _exit(0);
}


static
int lxcContainerHasReboot(void)
{
    int flags = CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|
        CLONE_NEWIPC|SIGCHLD;
    int cpid;
    char *childStack;
    char *stack;
    char *buf;
    int cmd, v;
    int status;
    char *tmp;
    int stacksize = getpagesize() * 4;

    if (virFileReadAll("/proc/sys/kernel/ctrl-alt-del", 10, &buf) < 0)
        return -1;

    if ((tmp = strchr(buf, '\n')))
        *tmp = '\0';

    if (virStrToLong_i(buf, NULL, 10, &v) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Malformed ctrl-alt-del setting '%s'"), buf);
        VIR_FREE(buf);
        return -1;
    }
    VIR_FREE(buf);
    cmd = v ? LINUX_REBOOT_CMD_CAD_ON : LINUX_REBOOT_CMD_CAD_OFF;

    if (VIR_ALLOC_N(stack, stacksize) < 0)
        return -1;

    childStack = stack + stacksize;

    cpid = clone(lxcContainerRebootChild, childStack, flags, &cmd);
    VIR_FREE(stack);
    if (cpid < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to clone to check reboot support"));
        return -1;
    } else if (virProcessWait(cpid, &status) < 0) {
        return -1;
    }

    if (WEXITSTATUS(status) != 1) {
        VIR_DEBUG("Containerized reboot support is missing "
                  "(kernel probably too old < 3.4)");
        return 0;
    }

    VIR_DEBUG("Containerized reboot support is available");
    return 1;
}


/**
 * lxcContainerBuildInitCmd:
 * @vmDef: pointer to vm definition structure
 *
 * Build a virCommandPtr for launching the container 'init' process
 *
 * Returns a virCommandPtr
 */
static virCommandPtr lxcContainerBuildInitCmd(virDomainDefPtr vmDef,
                                              char **ttyPaths,
                                              size_t nttyPaths)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virCommandPtr cmd;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    /* 'container_ptys' must exclude the PTY associated with
     * the /dev/console device, hence start at 1 not 0
     */
    for (i = 1; i < nttyPaths; i++) {
        if (!STRPREFIX(ttyPaths[i], "/dev/")) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Expected a /dev path for '%s'"),
                           ttyPaths[i]);
            virBufferFreeAndReset(&buf);
            return NULL;
        }
        virBufferAdd(&buf, ttyPaths[i] + 5, -1);
        virBufferAddChar(&buf, ' ');
    }
    virBufferTrim(&buf, NULL, 1);

    if (virBufferError(&buf))
        return NULL;

    virUUIDFormat(vmDef->uuid, uuidstr);

    cmd = virCommandNew(vmDef->os.init);

    if (vmDef->os.initargv && vmDef->os.initargv[0])
        virCommandAddArgSet(cmd, (const char **)vmDef->os.initargv);

    virCommandAddEnvString(cmd, "PATH=/bin:/sbin");
    virCommandAddEnvString(cmd, "TERM=linux");
    virCommandAddEnvString(cmd, "container=lxc-libvirt");
    virCommandAddEnvPair(cmd, "container_uuid", uuidstr);
    if (nttyPaths > 1)
        virCommandAddEnvPair(cmd, "container_ttys", virBufferCurrentContent(&buf));
    virCommandAddEnvPair(cmd, "LIBVIRT_LXC_UUID", uuidstr);
    virCommandAddEnvPair(cmd, "LIBVIRT_LXC_NAME", vmDef->name);
    if (vmDef->os.cmdline)
        virCommandAddEnvPair(cmd, "LIBVIRT_LXC_CMDLINE", vmDef->os.cmdline);

    virBufferFreeAndReset(&buf);
    return cmd;
}

/**
 * lxcContainerSetupFDs:
 * @control: control FD from parent
 * @ttyfd: FD of tty to set as the container console
 * @npassFDs: number of extra FDs
 * @passFDs: list of extra FDs
 *
 * Setup file descriptors in the container. @ttyfd is set to be
 * the container's stdin, stdout & stderr. Any FDs included in
 * @passFDs, will be dup()'d such that they start from stderr+1
 * with no gaps.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcContainerSetupFDs(int *ttyfd,
                                size_t npassFDs, int *passFDs)
{
    int rc = -1;
    int open_max;
    int fd;
    int last_fd;
    size_t i;
    size_t j;

    if (setsid() < 0) {
        virReportSystemError(errno, "%s",
                             _("setsid failed"));
        goto cleanup;
    }

    if (ioctl(*ttyfd, TIOCSCTTY, NULL) < 0) {
        virReportSystemError(errno, "%s",
                             _("ioctl(TIOCSTTY) failed"));
        goto cleanup;
    }

    if (dup2(*ttyfd, STDIN_FILENO) < 0) {
        virReportSystemError(errno, "%s",
                             _("dup2(stdin) failed"));
        goto cleanup;
    }

    if (dup2(*ttyfd, STDOUT_FILENO) < 0) {
        virReportSystemError(errno, "%s",
                             _("dup2(stdout) failed"));
        goto cleanup;
    }

    if (dup2(*ttyfd, STDERR_FILENO) < 0) {
        virReportSystemError(errno, "%s",
                             _("dup2(stderr) failed"));
        goto cleanup;
    }

    VIR_FORCE_CLOSE(*ttyfd);

    /* Any FDs in @passFDs need to be moved around so that
     * they are numbered, without gaps, starting from
     * STDERR_FILENO + 1
     */
    for (i = 0; i < npassFDs; i++) {
        int wantfd;

        wantfd = STDERR_FILENO + i + 1;
        VIR_DEBUG("Pass %d onto %d", passFDs[i], wantfd);

        /* If we already have desired FD number, life
         * is easy. Nothing needs renumbering */
        if (passFDs[i] == wantfd)
            continue;

        /*
         * Lets check to see if any later FDs are occupying
         * our desired FD number. If so, we must move them
         * out of the way
         */
        for (j = i + 1; j < npassFDs; j++) {
            if (passFDs[j] == wantfd) {
                VIR_DEBUG("Clash %zu", j);
                int newfd = dup(passFDs[j]);
                if (newfd < 0) {
                    virReportSystemError(errno,
                                         _("Cannot move fd %d out of the way"),
                                         passFDs[j]);
                    goto cleanup;
                }
                /* We're intentionally not closing the
                 * old value of passFDs[j], because we
                 * don't want later iterations of the
                 * loop to take it back. dup2() will
                 * cause it to be closed shortly anyway
                 */
                VIR_DEBUG("Moved clash onto %d", newfd);
                passFDs[j] = newfd;
            }
        }

        /* Finally we can move into our desired FD number */
        if (dup2(passFDs[i], wantfd) < 0) {
            virReportSystemError(errno,
                                 _("Cannot duplicate fd %d onto fd %d"),
                                 passFDs[i], wantfd);
            goto cleanup;
        }
        VIR_FORCE_CLOSE(passFDs[i]);
    }

    last_fd = STDERR_FILENO + npassFDs;

    /* Just in case someone forget to set FD_CLOEXEC, explicitly
     * close all remaining FDs before executing the container */
    open_max = sysconf(_SC_OPEN_MAX);
    if (open_max < 0) {
        virReportSystemError(errno, "%s",
                             _("sysconf(_SC_OPEN_MAX) failed"));
        goto cleanup;
    }

    for (fd = last_fd + 1; fd < open_max; fd++) {
        int tmpfd = fd;
        VIR_MASS_CLOSE(tmpfd);
    }

    rc = 0;

cleanup:
    VIR_DEBUG("rc=%d", rc);
    return rc;
}

/**
 * lxcContainerSendContinue:
 * @control: control FD to child
 *
 * Sends the continue message via the socket pair stored in the vm
 * structure.
 *
 * Returns 0 on success or -1 in case of error
 */
int lxcContainerSendContinue(int control)
{
    int rc = -1;
    lxc_message_t msg = LXC_CONTINUE_MSG;
    int writeCount = 0;

    VIR_DEBUG("Send continue on fd %d", control);
    writeCount = safewrite(control, &msg, sizeof(msg));
    if (writeCount != sizeof(msg)) {
        goto error_out;
    }

    rc = 0;
error_out:
    return rc;
}

/**
 * lxcContainerWaitForContinue:
 * @control: Control FD from parent
 *
 * This function will wait for the container continue message from the
 * parent process.  It will send this message on the socket pair stored in
 * the vm structure once it has completed the post clone container setup.
 *
 * Returns 0 on success or -1 in case of error
 */
int lxcContainerWaitForContinue(int control)
{
    lxc_message_t msg;
    int readLen;

    VIR_DEBUG("Wait continue on fd %d", control);
    readLen = saferead(control, &msg, sizeof(msg));
    VIR_DEBUG("Got continue on fd %d %d", control, readLen);
    if (readLen != sizeof(msg)) {
        if (readLen >= 0)
            errno = EIO;
        return -1;
    }
    if (msg != LXC_CONTINUE_MSG) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}


/**
 * lxcContainerSetID:
 *
 * This function calls setuid and setgid to create proper
 * cred for tasks running in container.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcContainerSetID(virDomainDefPtr def)
{
    /* Only call virSetUIDGID when user namespace is enabled
     * for this container. And user namespace is only enabled
     * when nuidmap&ngidmap is not zero */

    if (!def->idmap.nuidmap)
        return 0;

    VIR_DEBUG("Setting UID/GID to 0/0");
    if (virSetUIDGID(0, 0, NULL, 0) < 0) {
        virReportSystemError(errno, "%s",
                             _("setuid or setgid failed"));
        return -1;
    }

    return 0;
}


/**
 * lxcContainerRenameAndEnableInterfaces:
 * @nveths: number of interfaces
 * @veths: interface names
 *
 * This function will rename the interfaces to ethN
 * with id ascending order from zero and enable the
 * renamed interfaces for this container.
 *
 * Returns 0 on success or nonzero in case of error
 */
static int lxcContainerRenameAndEnableInterfaces(bool privNet,
                                                 size_t nveths,
                                                 char **veths)
{
    int rc = 0;
    size_t i;
    char *newname = NULL;

    for (i = 0; i < nveths; i++) {
        if (virAsprintf(&newname, "eth%zu", i) < 0) {
            rc = -1;
            goto error_out;
        }

        VIR_DEBUG("Renaming %s to %s", veths[i], newname);
        rc = virNetDevSetName(veths[i], newname);
        if (rc < 0)
            goto error_out;

        VIR_DEBUG("Enabling %s", newname);
        rc = virNetDevSetOnline(newname, true);
        if (rc < 0)
            goto error_out;

        VIR_FREE(newname);
    }

    /* enable lo device only if there were other net devices */
    if (veths || privNet)
        rc = virNetDevSetOnline("lo", true);

error_out:
    VIR_FREE(newname);
    return rc;
}


/*_syscall2(int, pivot_root, char *, newroot, const char *, oldroot)*/
extern int pivot_root(const char * new_root, const char * put_old);

#ifndef MS_REC
# define MS_REC          16384
#endif

#ifndef MNT_DETACH
# define MNT_DETACH      0x00000002
#endif

#ifndef MS_PRIVATE
# define MS_PRIVATE              (1<<18)
#endif

#ifndef MS_SLAVE
# define MS_SLAVE                (1<<19)
#endif


static int lxcContainerUnmountSubtree(const char *prefix,
                                      bool isOldRootFS)
{
    char **mounts = NULL;
    size_t nmounts = 0;
    size_t i;
    int saveErrno;
    const char *failedUmount = NULL;
    int ret = -1;

    VIR_DEBUG("Unmount subtreee from %s", prefix);

    if (virFileGetMountReverseSubtree("/proc/mounts", prefix,
                                      &mounts, &nmounts) < 0)
        goto cleanup;
    for (i = 0; i < nmounts; i++) {
        VIR_DEBUG("Umount %s", mounts[i]);
        if (umount(mounts[i]) < 0) {
            char ebuf[1024];
            failedUmount = mounts[i];
            saveErrno = errno;
            VIR_WARN("Failed to unmount '%s', trying to detach subtree '%s': %s",
                     failedUmount, mounts[nmounts-1],
                     virStrerror(errno, ebuf, sizeof(ebuf)));
            break;
        }
    }

    if (failedUmount) {
        /* This detaches the subtree */
        if (umount2(mounts[nmounts-1], MNT_DETACH) < 0) {
            virReportSystemError(saveErrno,
                                 _("Failed to unmount '%s' and could not detach subtree '%s'"),
                                 failedUmount, mounts[nmounts-1]);
            goto cleanup;
        }
        /* This unmounts the tmpfs on which the old root filesystem was hosted */
        if (isOldRootFS &&
            umount(mounts[nmounts-1]) < 0) {
            virReportSystemError(saveErrno,
                                 _("Failed to unmount '%s' and could not unmount old root '%s'"),
                                 failedUmount, mounts[nmounts-1]);
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    virStringFreeList(mounts);

    return ret;
}


static int lxcContainerPrepareRoot(virDomainDefPtr def,
                                   virDomainFSDefPtr root,
                                   const char *sec_mount_options)
{
    char *dst;
    char *tmp;

    VIR_DEBUG("Prepare root %d", root->type);

    if (root->type == VIR_DOMAIN_FS_TYPE_MOUNT)
        return 0;

    if (root->type == VIR_DOMAIN_FS_TYPE_FILE) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unexpected root filesystem without loop device"));
        return -1;
    }

    if (root->type != VIR_DOMAIN_FS_TYPE_BLOCK) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Unsupported root filesystem type %s"),
                       virDomainFSTypeToString(root->type));
        return -1;
    }

    if (virAsprintf(&dst, "%s/%s.root",
                    LXC_STATE_DIR, def->name) < 0)
        return -1;

    tmp = root->dst;
    root->dst = dst;

    if (lxcContainerMountFSBlock(root, "", sec_mount_options) < 0) {
        root->dst = tmp;
        VIR_FREE(dst);
        return -1;
    }

    root->dst = tmp;
    root->type = VIR_DOMAIN_FS_TYPE_MOUNT;
    VIR_FREE(root->src);
    root->src = dst;

    return 0;
}

static int lxcContainerPivotRoot(virDomainFSDefPtr root)
{
    int ret;
    char *oldroot = NULL, *newroot = NULL;

    ret = -1;

    VIR_DEBUG("Pivot via %s", root->src);

    /* root->parent must be private, so make / private. */
    if (mount("", "/", NULL, MS_PRIVATE|MS_REC, NULL) < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to make root private"));
        goto err;
    }

    if (virAsprintf(&oldroot, "%s/.oldroot", root->src) < 0)
        goto err;

    if (virFileMakePath(oldroot) < 0) {
        virReportSystemError(errno,
                             _("Failed to create %s"),
                             oldroot);
        goto err;
    }

    /* Create a tmpfs root since old and new roots must be
     * on separate filesystems */
    if (mount("tmprootfs", oldroot, "tmpfs", 0, NULL) < 0) {
        virReportSystemError(errno,
                             _("Failed to mount empty tmpfs at %s"),
                             oldroot);
        goto err;
    }

    /* Create a directory called 'new' in tmpfs */
    if (virAsprintf(&newroot, "%s/new", oldroot) < 0)
        goto err;

    if (virFileMakePath(newroot) < 0) {
        virReportSystemError(errno,
                             _("Failed to create %s"),
                             newroot);
        goto err;
    }

    /* ... and mount our root onto it */
    if (mount(root->src, newroot, NULL, MS_BIND|MS_REC, NULL) < 0) {
        virReportSystemError(errno,
                             _("Failed to bind %s to new root %s"),
                             root->src, newroot);
        goto err;
    }

    if (root->readonly) {
        if (mount(root->src, newroot, NULL, MS_BIND|MS_REC|MS_RDONLY|MS_REMOUNT, NULL) < 0) {
            virReportSystemError(errno,
                                 _("Failed to make new root %s readonly"),
                                 root->src);
            goto err;
        }
    }

    /* Now we chdir into the tmpfs, then pivot into the
     * root->src bind-mounted onto '/new' */
    if (chdir(newroot) < 0) {
        virReportSystemError(errno,
                             _("Failed to chdir into %s"), newroot);
        goto err;
    }

    /* The old root directory will live at /.oldroot after
     * this and will soon be unmounted completely */
    if (pivot_root(".", ".oldroot") < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to pivot root"));
        goto err;
    }

    /* CWD is undefined after pivot_root, so go to / */
    if (chdir("/") < 0)
        goto err;

    ret = 0;

err:
    VIR_FREE(oldroot);
    VIR_FREE(newroot);

    return ret;
}


typedef struct {
    const char *src;
    const char *dst;
    const char *type;
    int mflags;
    bool skipUserNS;
    bool skipUnmounted;
} virLXCBasicMountInfo;

static const virLXCBasicMountInfo lxcBasicMounts[] = {
    { "proc", "/proc", "proc", MS_NOSUID|MS_NOEXEC|MS_NODEV, false, false },
    { "/proc/sys", "/proc/sys", NULL, MS_BIND|MS_RDONLY, false, false },
    { "sysfs", "/sys", "sysfs", MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_RDONLY, false, false },
    { "securityfs", "/sys/kernel/security", "securityfs", MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_RDONLY, true, true },
#if WITH_SELINUX
    { SELINUX_MOUNT, SELINUX_MOUNT, "selinuxfs", MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_RDONLY, true, true },
#endif
};


bool lxcIsBasicMountLocation(const char *path)
{
    size_t i;

    for (i = 0; i < ARRAY_CARDINALITY(lxcBasicMounts); i++) {
        if (STREQ(path, lxcBasicMounts[i].dst))
            return true;
    }

    return false;
}


static int lxcContainerSetReadOnly(void)
{
    FILE *procmnt;
    struct mntent mntent;
    char mntbuf[1024];
    int ret = -1;
    char **mounts = NULL;
    size_t nmounts = 0;
    size_t i;

    if (!(procmnt = setmntent("/proc/mounts", "r"))) {
        virReportSystemError(errno, "%s",
                             _("Failed to read /proc/mounts"));
        return -1;
    }

    while (getmntent_r(procmnt, &mntent, mntbuf, sizeof(mntbuf)) != NULL) {
        if (STREQ(mntent.mnt_dir, "/") ||
            STREQ(mntent.mnt_dir, "/.oldroot") ||
            STRPREFIX(mntent.mnt_dir, "/.oldroot/") ||
            lxcIsBasicMountLocation(mntent.mnt_dir))
            continue;

        if (VIR_REALLOC_N(mounts, nmounts + 1) < 0)
            goto cleanup;
        if (VIR_STRDUP(mounts[nmounts], mntent.mnt_dir) < 0)
            goto cleanup;
        nmounts++;
    }

    if (mounts)
        qsort(mounts, nmounts, sizeof(mounts[0]),
              virStringSortRevCompare);

    for (i = 0; i < nmounts; i++) {
        VIR_DEBUG("Bind readonly %s", mounts[i]);
        if (mount(mounts[i], mounts[i], NULL, MS_BIND|MS_REC|MS_RDONLY|MS_REMOUNT, NULL) < 0) {
            virReportSystemError(errno,
                                 _("Failed to make mount %s readonly"),
                                 mounts[i]);
            goto cleanup;
        }
    }

    ret = 0;
cleanup:
    for (i = 0; i < nmounts; i++)
        VIR_FREE(mounts[i]);
    VIR_FREE(mounts);
    endmntent(procmnt);
    return ret;

}


static int lxcContainerMountBasicFS(bool userns_enabled)
{
    size_t i;
    int rc = -1;

    VIR_DEBUG("Mounting basic filesystems");

    for (i = 0; i < ARRAY_CARDINALITY(lxcBasicMounts); i++) {
        bool bindOverReadonly;
        virLXCBasicMountInfo const *mnt = &lxcBasicMounts[i];

        VIR_DEBUG("Processing %s -> %s",
                  mnt->src, mnt->dst);

        if (mnt->skipUnmounted) {
            char *hostdir;
            int ret;

            if (virAsprintf(&hostdir, "/.oldroot%s", mnt->dst) < 0)
                goto cleanup;

            ret = virFileIsMountPoint(hostdir);
            VIR_FREE(hostdir);
            if (ret < 0)
                goto cleanup;

            if (ret == 0) {
                VIR_DEBUG("Skipping '%s' which isn't mounted in host",
                          mnt->dst);
                continue;
            }
        }

        if (mnt->skipUserNS && userns_enabled) {
            VIR_DEBUG("Skipping due to user ns enablement");
            continue;
        }

        if (virFileMakePath(mnt->dst) < 0) {
            virReportSystemError(errno,
                                 _("Failed to mkdir %s"),
                                 mnt->src);
            goto cleanup;
        }

        /*
         * We can't immediately set the MS_RDONLY flag when mounting filesystems
         * because (in at least some kernel versions) this will propagate back
         * to the original mount in the host OS, turning it readonly too. Thus
         * we mount the filesystem in read-write mode initially, and then do a
         * separate read-only bind mount on top of that.
         */
        bindOverReadonly = !!(mnt->mflags & MS_RDONLY);

        VIR_DEBUG("Mount %s on %s type=%s flags=%x",
                  mnt->src, mnt->dst, mnt->type, mnt->mflags & ~MS_RDONLY);
        if (mount(mnt->src, mnt->dst, mnt->type, mnt->mflags & ~MS_RDONLY, NULL) < 0) {
            virReportSystemError(errno,
                                 _("Failed to mount %s on %s type %s flags=%x"),
                                 mnt->src, mnt->dst, NULLSTR(mnt->type),
                                 mnt->mflags & ~MS_RDONLY);
            goto cleanup;
        }

        if (bindOverReadonly &&
            mount(mnt->src, mnt->dst, NULL,
                  MS_BIND|MS_REMOUNT|MS_RDONLY, NULL) < 0) {
            virReportSystemError(errno,
                                 _("Failed to re-mount %s on %s flags=%x"),
                                 mnt->src, mnt->dst,
                                 MS_BIND|MS_REMOUNT|MS_RDONLY);
            goto cleanup;
        }
    }

    rc = 0;

cleanup:
    VIR_DEBUG("rc=%d", rc);
    return rc;
}

#if WITH_FUSE
static int lxcContainerMountProcFuse(virDomainDefPtr def,
                                     const char *stateDir)
{
    int ret;
    char *meminfo_path = NULL;

    VIR_DEBUG("Mount /proc/meminfo stateDir=%s", stateDir);

    if ((ret = virAsprintf(&meminfo_path,
                           "/.oldroot/%s/%s.fuse/meminfo",
                           stateDir,
                           def->name)) < 0)
        return ret;

    if ((ret = mount(meminfo_path, "/proc/meminfo",
                     NULL, MS_BIND, NULL)) < 0) {
        virReportSystemError(errno,
                             _("Failed to mount %s on /proc/meminfo"),
                             meminfo_path);
    }

    VIR_FREE(meminfo_path);
    return ret;
}
#else
static int lxcContainerMountProcFuse(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                     const char *stateDir ATTRIBUTE_UNUSED)
{
    return 0;
}
#endif

static int lxcContainerMountFSDev(virDomainDefPtr def,
                                  const char *stateDir)
{
    int ret = -1;
    char *path = NULL;
    int flags = def->idmap.nuidmap ? MS_BIND : MS_MOVE;

    VIR_DEBUG("Mount /dev/ stateDir=%s", stateDir);

    if ((ret = virAsprintf(&path, "/.oldroot/%s/%s.dev",
                           stateDir, def->name)) < 0)
        return ret;

    if (virFileMakePath("/dev") < 0) {
        virReportSystemError(errno, "%s",
                             _("Cannot create /dev"));
        goto cleanup;
    }

    VIR_DEBUG("Trying to %s %s to /dev", def->idmap.nuidmap ?
              "bind" : "move", path);

    if (mount(path, "/dev", NULL, flags, NULL) < 0) {
        virReportSystemError(errno,
                             _("Failed to mount %s on /dev"),
                             path);
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(path);
    return ret;
}

static int lxcContainerMountFSDevPTS(virDomainDefPtr def,
                                     const char *stateDir)
{
    int ret = -1;
    char *path = NULL;
    int flags = def->idmap.nuidmap ? MS_BIND : MS_MOVE;

    VIR_DEBUG("Mount /dev/pts stateDir=%s", stateDir);

    if (virAsprintf(&path, "/.oldroot/%s/%s.devpts",
                    stateDir, def->name) < 0)
        return ret;

    if (virFileMakePath("/dev/pts") < 0) {
        virReportSystemError(errno, "%s",
                             _("Cannot create /dev/pts"));
        goto cleanup;
    }

    VIR_DEBUG("Trying to %s %s to /dev/pts", def->idmap.nuidmap ?
              "bind" : "move", path);

    if (mount(path, "/dev/pts", NULL, flags, NULL) < 0) {
        virReportSystemError(errno,
                             _("Failed to mount %s on /dev/pts"),
                             path);
        goto cleanup;
    }

    ret = 0;
cleanup:
    VIR_FREE(path);
    return ret;
}

static int lxcContainerSetupDevices(char **ttyPaths, size_t nttyPaths)
{
    size_t i;
    const struct {
        const char *src;
        const char *dst;
    } links[] = {
        { "/proc/self/fd/0", "/dev/stdin" },
        { "/proc/self/fd/1", "/dev/stdout" },
        { "/proc/self/fd/2", "/dev/stderr" },
        { "/proc/self/fd", "/dev/fd" },
    };

    for (i = 0; i < ARRAY_CARDINALITY(links); i++) {
        if (symlink(links[i].src, links[i].dst) < 0) {
            virReportSystemError(errno,
                                 _("Failed to symlink device %s to %s"),
                                 links[i].dst, links[i].src);
            return -1;
        }
    }

    /* We have private devpts capability, so bind that */
    if (virFileTouch("/dev/ptmx", 0666) < 0)
        return -1;

    if (mount("/dev/pts/ptmx", "/dev/ptmx", "ptmx", MS_BIND, NULL) < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to bind /dev/pts/ptmx on to /dev/ptmx"));
        return -1;
    }

    for (i = 0; i < nttyPaths; i++) {
        char *tty;
        if (virAsprintf(&tty, "/dev/tty%zu", i+1) < 0)
            return -1;
        if (symlink(ttyPaths[i], tty) < 0) {
            virReportSystemError(errno,
                                 _("Failed to symlink %s to %s"),
                                 ttyPaths[i], tty);
            VIR_FREE(tty);
            return -1;
        }
        VIR_FREE(tty);
        if (i == 0 &&
            symlink(ttyPaths[i], "/dev/console") < 0) {
            virReportSystemError(errno,
                                 _("Failed to symlink %s to /dev/console"),
                                 ttyPaths[i]);
            return -1;
        }
    }
    return 0;
}


static int lxcContainerMountFSBind(virDomainFSDefPtr fs,
                                   const char *srcprefix)
{
    char *src = NULL;
    int ret = -1;
    struct stat st;

    VIR_DEBUG("src=%s dst=%s", fs->src, fs->dst);

    if (virAsprintf(&src, "%s%s", srcprefix, fs->src) < 0)
        goto cleanup;

    if (stat(fs->dst, &st) < 0) {
        if (errno != ENOENT) {
            virReportSystemError(errno, _("Unable to stat bind target %s"),
                                 fs->dst);
            goto cleanup;
        }
        /* ENOENT => create the target dir or file */
        if (stat(src, &st) < 0) {
            virReportSystemError(errno, _("Unable to stat bind source %s"),
                                 src);
            goto cleanup;
        }
        if (S_ISDIR(st.st_mode)) {
            if (virFileMakePath(fs->dst) < 0) {
                virReportSystemError(errno,
                                     _("Failed to create %s"),
                                     fs->dst);
                goto cleanup;
            }
        } else {
            /* Create Empty file for target mount point */
            int fd = open(fs->dst, O_WRONLY|O_CREAT|O_NOCTTY|O_NONBLOCK, 0666);
            if (fd < 0) {
                if (errno != EEXIST) {
                    virReportSystemError(errno,
                                         _("Failed to create bind target %s"),
                                         fs->dst);
                    goto cleanup;
                }
            }
            if (VIR_CLOSE(fd) < 0) {
                virReportSystemError(errno,
                                     _("Failed to close bind target %s"),
                                     fs->dst);
                goto cleanup;
            }
        }
    }

    if (mount(src, fs->dst, NULL, MS_BIND, NULL) < 0) {
        virReportSystemError(errno,
                             _("Failed to bind mount directory %s to %s"),
                             src, fs->dst);
        goto cleanup;
    }

    if (fs->readonly) {
        VIR_DEBUG("Binding %s readonly", fs->dst);
        if (mount(src, fs->dst, NULL, MS_BIND|MS_REMOUNT|MS_RDONLY, NULL) < 0) {
            virReportSystemError(errno,
                                 _("Failed to make directory %s readonly"),
                                 fs->dst);
        }
    }

    ret = 0;

cleanup:
    VIR_FREE(src);
    return ret;
}


#ifdef WITH_BLKID
static int
lxcContainerMountDetectFilesystem(const char *src, char **type)
{
    int fd;
    int ret = -1;
    int rc;
    const char *data = NULL;
    blkid_probe blkid = NULL;

    *type = NULL;

    if ((fd = open(src, O_RDONLY)) < 0) {
        virReportSystemError(errno,
                             _("Unable to open filesystem %s"), src);
        return -1;
    }

    if (!(blkid = blkid_new_probe())) {
        virReportSystemError(errno, "%s",
                             _("Unable to create blkid library handle"));
        goto cleanup;
    }
    if (blkid_probe_set_device(blkid, fd, 0, 0) < 0) {
        virReportSystemError(EINVAL,
                             _("Unable to associate device %s with blkid library"),
                             src);
        goto cleanup;
    }

    blkid_probe_enable_superblocks(blkid, 1);

    blkid_probe_set_superblocks_flags(blkid, BLKID_SUBLKS_TYPE);

    rc = blkid_do_safeprobe(blkid);
    if (rc != 0) {
        if (rc == 1) /* Nothing found, return success with *type == NULL */
            goto done;

        if (rc == -2) {
            virReportSystemError(EINVAL,
                                 _("Too many filesystems detected for %s"),
                                 src);
        } else {
            virReportSystemError(errno,
                                 _("Unable to detect filesystem for %s"),
                                 src);
        }
        goto cleanup;
    }

    if (blkid_probe_lookup_value(blkid, "TYPE", &data, NULL) < 0) {
        virReportSystemError(ENOENT,
                             _("Unable to find filesystem type for %s"),
                             src);
        goto cleanup;
    }

    if (VIR_STRDUP(*type, data) < 0)
        goto cleanup;

done:
    ret = 0;
cleanup:
    VIR_FORCE_CLOSE(fd);
    if (blkid)
        blkid_free_probe(blkid);
    return ret;
}
#else /* ! WITH_BLKID */
static int
lxcContainerMountDetectFilesystem(const char *src ATTRIBUTE_UNUSED,
                                  char **type)
{
    /* No libblkid, so just return success with no detected type */
    *type = NULL;
    return 0;
}
#endif /* ! WITH_BLKID */

/*
 * This function attempts to do automatic detection of filesystem
 * type following the same rules as the util-linux 'mount' binary.
 *
 * The main difference is that we don't (currently) try to use
 * libblkid to detect the format first. We go straight to using
 * /etc/filesystems, and then /proc/filesystems
 */
static int lxcContainerMountFSBlockAuto(virDomainFSDefPtr fs,
                                        int fsflags,
                                        const char *src,
                                        const char *srcprefix,
                                        const char *sec_mount_options)
{
    FILE *fp = NULL;
    int ret = -1;
    bool tryProc = false;
    bool gotStar = false;
    char *fslist = NULL;
    char *line = NULL;
    const char *type;

    VIR_DEBUG("src=%s dst=%s srcprefix=%s", src, fs->dst, srcprefix);

    /* First time around we use /etc/filesystems */
retry:
    if (virAsprintf(&fslist, "%s%s", srcprefix,
                    tryProc ? "/proc/filesystems" : "/etc/filesystems") < 0)
        goto cleanup;

    VIR_DEBUG("Open fslist %s", fslist);
    if (!(fp = fopen(fslist, "r"))) {
        /* If /etc/filesystems does not exist, then we need to retry
         * with /proc/filesystems next
         */
        if (errno == ENOENT &&
            !tryProc) {
            tryProc = true;
            VIR_FREE(fslist);
            goto retry;
        }

        virReportSystemError(errno,
                             _("Unable to read %s"),
                             fslist);
        goto cleanup;
    }

    while (!feof(fp)) {
        size_t n;
        VIR_FREE(line);
        if (getline(&line, &n, fp) <= 0) {
            if (feof(fp))
                break;

            goto cleanup;
        }

        if (strstr(line, "nodev"))
            continue;

        type = strchr(line, '\n');
        if (type)
            line[type-line] = '\0';

        type = line;
        virSkipSpaces(&type);

        /*
         * /etc/filesystems is only allowed to contain '*' on the last line
         */
        if (gotStar && !tryProc) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("%s has unexpected '*' before last line"),
                           fslist);
            goto cleanup;
        }

        /* An '*' on the last line in /etc/filesystems
         * means try /proc/filesystems next. We don't
         * jump immediately though, since we need to see
         * if any more lines follow
         */
        if (!tryProc &&
            STREQ(type, "*"))
            gotStar = true;

        VIR_DEBUG("Trying mount '%s' on '%s' with '%s' opts '%s'",
                  src, fs->dst, type, sec_mount_options);
        if (mount(src, fs->dst, type, fsflags, sec_mount_options) < 0) {
            /* These errnos indicate a bogus filesystem type for
             * the image we have, so skip to the next type
             */
            if (errno == EINVAL || errno == ENODEV)
                continue;

            virReportSystemError(errno,
                                 _("Failed to mount device %s to %s"),
                                 src, fs->dst);
            goto cleanup;
        }

        ret = 0;
        break;
    }

    /* We've got to the end of /etc/filesystems and saw
     * a '*', so we must try /proc/filesystems next
     */
    if (ret != 0 &&
        !tryProc &&
        gotStar) {
        tryProc = true;
        VIR_FREE(fslist);
        VIR_FORCE_FCLOSE(fp);
        goto retry;
    }

    if (ret != 0) {
        virReportSystemError(ENODEV,
                             _("Failed to mount device %s to %s, unable to detect filesystem"),
                             src, fs->dst);
    }

    VIR_DEBUG("Done mounting filesystem ret=%d tryProc=%d", ret, tryProc);

cleanup:
    VIR_FREE(line);
    VIR_FREE(fslist);
    VIR_FORCE_FCLOSE(fp);
    return ret;
}


/*
 * Mount a block device 'src' on fs->dst, automatically
 * probing for filesystem type
 */
static int lxcContainerMountFSBlockHelper(virDomainFSDefPtr fs,
                                          const char *src,
                                          const char *srcprefix,
                                          const char *sec_mount_options)
{
    int fsflags = 0;
    int ret = -1;
    char *format = NULL;

    if (fs->readonly)
        fsflags |= MS_RDONLY;

    if (virFileMakePath(fs->dst) < 0) {
        virReportSystemError(errno,
                             _("Failed to create %s"),
                             fs->dst);
        goto cleanup;
    }

    if (lxcContainerMountDetectFilesystem(src, &format) < 0)
        goto cleanup;

    if (format) {
        VIR_DEBUG("Mount '%s' on '%s' with detected format '%s' opts '%s'",
                  src, fs->dst, format, sec_mount_options);
        if (mount(src, fs->dst, format, fsflags, sec_mount_options) < 0) {
            virReportSystemError(errno,
                                 _("Failed to mount device %s to %s as %s"),
                                 src, fs->dst, format);
            goto cleanup;
        }
        ret = 0;
    } else {
        ret = lxcContainerMountFSBlockAuto(fs, fsflags, src, srcprefix, sec_mount_options);
    }

cleanup:
    VIR_FREE(format);
    return ret;
}


static int lxcContainerMountFSBlock(virDomainFSDefPtr fs,
                                    const char *srcprefix,
                                    const char *sec_mount_options)
{
    char *src = NULL;
    int ret = -1;

    VIR_DEBUG("src=%s dst=%s", fs->src, fs->dst);

    if (virAsprintf(&src, "%s%s", srcprefix, fs->src) < 0)
        goto cleanup;

    ret = lxcContainerMountFSBlockHelper(fs, src, srcprefix, sec_mount_options);

    VIR_DEBUG("Done mounting filesystem ret=%d", ret);

cleanup:
    VIR_FREE(src);
    return ret;
}


static int lxcContainerMountFSTmpfs(virDomainFSDefPtr fs,
                                    char *sec_mount_options)
{
    int ret = -1;
    char *data = NULL;

    VIR_DEBUG("usage=%lld sec=%s", fs->usage, sec_mount_options);

    if (virAsprintf(&data,
                    "size=%lld%s", fs->usage, sec_mount_options) < 0)
        goto cleanup;

    if (virFileMakePath(fs->dst) < 0) {
        virReportSystemError(errno,
                             _("Failed to create %s"),
                             fs->dst);
        goto cleanup;
    }

    if (mount("tmpfs", fs->dst, "tmpfs", MS_NOSUID|MS_NODEV, data) < 0) {
        virReportSystemError(errno,
                             _("Failed to mount directory %s as tmpfs"),
                             fs->dst);
        goto cleanup;
    }

    if (fs->readonly) {
        VIR_DEBUG("Binding %s readonly", fs->dst);
        if (mount(fs->dst, fs->dst, NULL, MS_BIND|MS_REMOUNT|MS_RDONLY, NULL) < 0) {
            virReportSystemError(errno,
                                 _("Failed to make directory %s readonly"),
                                 fs->dst);
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    VIR_FREE(data);
    return ret;
}


static int lxcContainerMountFS(virDomainFSDefPtr fs,
                               char *sec_mount_options)
{
    switch (fs->type) {
    case VIR_DOMAIN_FS_TYPE_MOUNT:
        if (lxcContainerMountFSBind(fs, "/.oldroot") < 0)
            return -1;
        break;
    case VIR_DOMAIN_FS_TYPE_BLOCK:
        if (lxcContainerMountFSBlock(fs, "/.oldroot", sec_mount_options) < 0)
            return -1;
        break;
    case VIR_DOMAIN_FS_TYPE_RAM:
        if (lxcContainerMountFSTmpfs(fs, sec_mount_options) < 0)
            return -1;
        break;
    case VIR_DOMAIN_FS_TYPE_BIND:
        if (lxcContainerMountFSBind(fs, "") < 0)
            return -1;
        break;
    case VIR_DOMAIN_FS_TYPE_FILE:
        /* We do actually support this, but the lxc controller
         * should have associated the file with a loopback
         * device and changed this to TYPE_BLOCK for us */
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected filesystem type %s"),
                       virDomainFSTypeToString(fs->type));
        return -1;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Cannot mount filesystem type %s"),
                       virDomainFSTypeToString(fs->type));
        return -1;
    }
    return 0;
}


static int lxcContainerMountAllFS(virDomainDefPtr vmDef,
                                  char *sec_mount_options)
{
    size_t i;
    VIR_DEBUG("Mounting all non-root filesystems");

    /* Pull in rest of container's mounts */
    for (i = 0; i < vmDef->nfss; i++) {
        if (STREQ(vmDef->fss[i]->dst, "/"))
            continue;

        if (lxcContainerUnmountSubtree(vmDef->fss[i]->dst,
                                       false) < 0)
            return -1;

        if (lxcContainerMountFS(vmDef->fss[i], sec_mount_options) < 0)
            return -1;
    }

    VIR_DEBUG("Mounted all non-root filesystems");
    return 0;
}


int lxcContainerSetupHostdevCapsMakePath(const char *dev)
{
    int ret = -1;
    char *dir, *tmp;

    if (VIR_STRDUP(dir, dev) < 0)
        return -1;

    if ((tmp = strrchr(dir, '/'))) {
        *tmp = '\0';
        if (virFileMakePath(dir) < 0) {
            virReportSystemError(errno,
                                 _("Failed to create directory for '%s' dev '%s'"),
                                 dir, dev);
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    VIR_FREE(dir);
    return ret;
}


static int lxcContainerUnmountForSharedRoot(const char *stateDir,
                                            const char *domain)
{
    int ret = -1;
    char *tmp = NULL;

#if WITH_SELINUX
    /* Some versions of Linux kernel don't let you overmount
     * the selinux filesystem, so make sure we kill it first
     */
    /* Filed coverity bug for false positive 'USE_AFTER_FREE' due to swap
     * of root->src with root->dst and the VIR_FREE(root->src) prior to the
     * reset of root->src in lxcContainerPrepareRoot()
     */
    /* coverity[deref_arg] */
    if (lxcContainerUnmountSubtree(SELINUX_MOUNT, false) < 0)
        goto cleanup;
#endif

    /* These filesystems are created by libvirt temporarily, they
     * shouldn't appear in container. */
    if (virAsprintf(&tmp, "%s/%s.dev", stateDir, domain) < 0 ||
        lxcContainerUnmountSubtree(tmp, false) < 0)
        goto cleanup;

    VIR_FREE(tmp);
    if (virAsprintf(&tmp, "%s/%s.devpts", stateDir, domain) < 0 ||
        lxcContainerUnmountSubtree(tmp, false) < 0)
        goto cleanup;

#if WITH_FUSE
    VIR_FREE(tmp);
    if (virAsprintf(&tmp, "%s/%s.fuse", stateDir, domain) < 0 ||
        lxcContainerUnmountSubtree(tmp, false) < 0)
        goto cleanup;
#endif

    /* If we have the root source being '/', then we need to
     * get rid of any existing stuff under /proc, /sys & /tmp.
     * We need new namespace aware versions of those. We must
     * do /proc last otherwise we won't find /proc/mounts :-) */
    if (lxcContainerUnmountSubtree("/sys", false) < 0 ||
        lxcContainerUnmountSubtree("/dev", false) < 0 ||
        lxcContainerUnmountSubtree("/proc", false) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    VIR_FREE(tmp);
    return ret;
}


/* Got a FS mapped to /, we're going the pivot_root
 * approach to do a better-chroot-than-chroot
 * this is based on this thread http://lkml.org/lkml/2008/3/5/29
 */
static int lxcContainerSetupPivotRoot(virDomainDefPtr vmDef,
                                      virDomainFSDefPtr root,
                                      char **ttyPaths,
                                      size_t nttyPaths,
                                      virSecurityManagerPtr securityDriver)
{
    virCgroupPtr cgroup = NULL;
    int ret = -1;
    char *sec_mount_options;
    char *stateDir = NULL;

    VIR_DEBUG("Setup pivot root");

    if (!(sec_mount_options = virSecurityManagerGetMountOptions(securityDriver, vmDef)))
        return -1;

    /* Before pivoting we need to identify any
     * cgroups controllers that are mounted */
    if (virCgroupNewSelf(&cgroup) < 0)
        goto cleanup;

    if (virFileResolveAllLinks(LXC_STATE_DIR, &stateDir) < 0)
        goto cleanup;

    /* Ensure the root filesystem is mounted */
    if (lxcContainerPrepareRoot(vmDef, root, sec_mount_options) < 0)
        goto cleanup;

    /* Gives us a private root, leaving all parent OS mounts on /.oldroot */
    if (lxcContainerPivotRoot(root) < 0)
        goto cleanup;

    /* FIXME: we should find a way to unmount these mounts for container
     * even user namespace is enabled. */
    if (STREQ(root->src, "/") && (!vmDef->idmap.nuidmap) &&
        lxcContainerUnmountForSharedRoot(stateDir, vmDef->name) < 0)
        goto cleanup;

    /* Mounts the core /proc, /sys, etc filesystems */
    if (lxcContainerMountBasicFS(vmDef->idmap.nuidmap) < 0)
        goto cleanup;

    /* Ensure entire root filesystem (except /.oldroot) is readonly */
    if (root->readonly &&
        lxcContainerSetReadOnly() < 0)
        goto cleanup;

    /* Mounts /proc/meminfo etc sysinfo */
    if (lxcContainerMountProcFuse(vmDef, stateDir) < 0)
        goto cleanup;

    /* Now we can re-mount the cgroups controllers in the
     * same configuration as before */
    if (virCgroupIsolateMount(cgroup, "/.oldroot/", sec_mount_options) < 0)
        goto cleanup;

    /* Mounts /dev */
    if (lxcContainerMountFSDev(vmDef, stateDir) < 0)
        goto cleanup;

    /* Mounts /dev/pts */
    if (lxcContainerMountFSDevPTS(vmDef, stateDir) < 0)
        goto cleanup;

    /* Setup device nodes in /dev/ */
    if (lxcContainerSetupDevices(ttyPaths, nttyPaths) < 0)
        goto cleanup;

    /* Sets up any non-root mounts from guest config */
    if (lxcContainerMountAllFS(vmDef, sec_mount_options) < 0)
        goto cleanup;

   /* Gets rid of all remaining mounts from host OS, including /.oldroot itself */
    if (lxcContainerUnmountSubtree("/.oldroot", true) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    VIR_FREE(stateDir);
    virCgroupFree(&cgroup);
    VIR_FREE(sec_mount_options);
    return ret;
}


static int lxcContainerResolveSymlinks(virDomainDefPtr vmDef)
{
    char *newroot;
    size_t i;

    VIR_DEBUG("Resolving symlinks");

    for (i = 0; i < vmDef->nfss; i++) {
        virDomainFSDefPtr fs = vmDef->fss[i];
        if (!fs->src)
            continue;

        if (access(fs->src, F_OK)) {
            virReportSystemError(errno,
                                 _("Failed to access '%s'"), fs->src);
            return -1;
        }

        VIR_DEBUG("Resolving '%s'", fs->src);
        if (virFileResolveAllLinks(fs->src, &newroot) < 0) {
            virReportSystemError(errno,
                                 _("Failed to resolve symlink at %s"),
                                 fs->src);
            return -1;
        }

        VIR_DEBUG("Resolved '%s' to %s", fs->src, newroot);

        VIR_FREE(fs->src);
        fs->src = newroot;
    }
    VIR_DEBUG("Resolved all filesystem symlinks");

    return 0;
}

/*
 * This is running as the 'init' process inside the container.
 * It removes some capabilities that could be dangerous to
 * host system, since they are not currently "containerized"
 */
#if WITH_CAPNG
static int lxcContainerDropCapabilities(bool keepReboot)
{
    int ret;

    capng_get_caps_process();

    if ((ret = capng_updatev(CAPNG_DROP,
                             CAPNG_EFFECTIVE | CAPNG_PERMITTED |
                             CAPNG_INHERITABLE | CAPNG_BOUNDING_SET,
                             CAP_SYS_MODULE, /* No kernel module loading */
                             CAP_SYS_TIME, /* No changing the clock */
                             CAP_MKNOD, /* No creating device nodes */
                             CAP_AUDIT_CONTROL, /* No messing with auditing status */
                             CAP_MAC_ADMIN, /* No messing with LSM config */
                             keepReboot ? -1 : CAP_SYS_BOOT, /* No use of reboot */
                             -1)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to remove capabilities: %d"), ret);
        return -1;
    }

    if ((ret = capng_apply(CAPNG_SELECT_BOTH)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to apply capabilities: %d"), ret);
        return -1;
    }

    /* We do not need to call capng_lock() in this case. The bounding
     * set restriction will prevent them reacquiring sys_boot/module/time,
     * etc which is all that matters for the container. Once inside the
     * container it is fine for SECURE_NOROOT / SECURE_NO_SETUID_FIXUP to
     * be unmasked  - they can never escape the bounding set. */

    return 0;
}
#else
static int lxcContainerDropCapabilities(bool keepReboot ATTRIBUTE_UNUSED)
{
    VIR_WARN("libcap-ng support not compiled in, unable to clear capabilities");
    return 0;
}
#endif


/**
 * lxcContainerChild:
 * @data: pointer to container arguments
 *
 * This function is run in the process clone()'d in lxcStartContainer.
 * Perform a number of container setup tasks:
 *     Setup container file system
 *     mount container /proca
 * Then exec's the container init
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcContainerChild(void *data)
{
    lxc_child_argv_t *argv = data;
    virDomainDefPtr vmDef = argv->config;
    int ttyfd = -1;
    int ret = -1;
    char *ttyPath = NULL;
    virDomainFSDefPtr root;
    virCommandPtr cmd = NULL;
    int hasReboot;

    if (NULL == vmDef) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("lxcChild() passed invalid vm definition"));
        goto cleanup;
    }

    /* Wait for controller to finish setup tasks, including
     * things like move of network interfaces, uid/gid mapping
     */
    if (lxcContainerWaitForContinue(argv->monitor) < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to read the container continue message"));
        goto cleanup;
    }
    VIR_DEBUG("Received container continue message");

    if ((hasReboot = lxcContainerHasReboot()) < 0)
        goto cleanup;

    cmd = lxcContainerBuildInitCmd(vmDef,
                                   argv->ttyPaths,
                                   argv->nttyPaths);
    virCommandWriteArgLog(cmd, 1);

    if (lxcContainerSetID(vmDef) < 0)
        goto cleanup;

    root = virDomainGetFilesystemForTarget(vmDef, "/");

    if (argv->nttyPaths) {
        const char *tty = argv->ttyPaths[0];
        if (STRPREFIX(tty, "/dev/pts/"))
            tty += strlen("/dev/pts/");
        if (virAsprintf(&ttyPath, "%s/%s.devpts/%s",
                        LXC_STATE_DIR, vmDef->name, tty) < 0)
            goto cleanup;
    } else if (VIR_STRDUP(ttyPath, "/dev/null") < 0) {
            goto cleanup;
    }

    VIR_DEBUG("Container TTY path: %s", ttyPath);

    ttyfd = open(ttyPath, O_RDWR|O_NOCTTY);
    if (ttyfd < 0) {
        virReportSystemError(errno,
                             _("Failed to open tty %s"),
                             ttyPath);
        goto cleanup;
    }

    if (lxcContainerResolveSymlinks(vmDef) < 0)
        goto cleanup;

    VIR_DEBUG("Setting up pivot");
    if (lxcContainerSetupPivotRoot(vmDef, root,
                                   argv->ttyPaths, argv->nttyPaths,
                                   argv->securityDriver) < 0)
        goto cleanup;

    if (!virFileExists(vmDef->os.init)) {
        virReportSystemError(errno,
                    _("cannot find init path '%s' relative to container root"),
                    vmDef->os.init);
        goto cleanup;
    }

    /* rename and enable interfaces */
    if (lxcContainerRenameAndEnableInterfaces(vmDef->features[VIR_DOMAIN_FEATURE_PRIVNET] ==
                                              VIR_DOMAIN_FEATURE_STATE_ON,
                                              argv->nveths,
                                              argv->veths) < 0) {
        goto cleanup;
    }

    /* drop a set of root capabilities */
    if (lxcContainerDropCapabilities(!!hasReboot) < 0)
        goto cleanup;

    if (lxcContainerSendContinue(argv->handshakefd) < 0) {
        virReportSystemError(errno, "%s",
                            _("Failed to send continue signal to controller"));
        goto cleanup;
    }

    VIR_DEBUG("Setting up security labeling");
    if (virSecurityManagerSetProcessLabel(argv->securityDriver, vmDef) < 0)
        goto cleanup;

    VIR_FORCE_CLOSE(argv->handshakefd);
    VIR_FORCE_CLOSE(argv->monitor);
    if (lxcContainerSetupFDs(&ttyfd,
                             argv->npassFDs, argv->passFDs) < 0)
        goto cleanup;

    ret = 0;
cleanup:
    VIR_FREE(ttyPath);
    VIR_FORCE_CLOSE(ttyfd);
    VIR_FORCE_CLOSE(argv->monitor);
    VIR_FORCE_CLOSE(argv->handshakefd);

    if (ret == 0) {
        /* this function will only return if an error occurred */
        ret = virCommandExec(cmd);
    }

    if (ret != 0) {
        virErrorPtr err = virGetLastError();
        if (err && err->message)
            fprintf(stderr, "%s\n", err->message);
        else
            fprintf(stderr, "%s\n",
                    _("Unknown failure in libvirt_lxc startup"));
    }

    virCommandFree(cmd);
    return ret;
}

static int userns_supported(void)
{
    return lxcContainerAvailable(LXC_CONTAINER_FEATURE_USER) == 0;
}

static int userns_required(virDomainDefPtr def)
{
    return def->idmap.uidmap && def->idmap.gidmap;
}

virArch lxcContainerGetAlt32bitArch(virArch arch)
{
    /* Any Linux 64bit arch which has a 32bit
     * personality available should be listed here */
    if (arch == VIR_ARCH_X86_64)
        return VIR_ARCH_I686;
    if (arch == VIR_ARCH_S390X)
        return VIR_ARCH_S390;
    if (arch == VIR_ARCH_PPC64)
        return VIR_ARCH_PPC;
    if (arch == VIR_ARCH_PARISC64)
        return VIR_ARCH_PARISC;
    if (arch == VIR_ARCH_SPARC64)
        return VIR_ARCH_SPARC;
    if (arch == VIR_ARCH_MIPS64)
        return VIR_ARCH_MIPS;
    if (arch == VIR_ARCH_MIPS64EL)
        return VIR_ARCH_MIPSEL;

    return VIR_ARCH_NONE;
}


static bool
lxcNeedNetworkNamespace(virDomainDefPtr def)
{
    size_t i;
    if (def->nets != NULL)
        return true;
    if (def->features[VIR_DOMAIN_FEATURE_PRIVNET] == VIR_DOMAIN_FEATURE_STATE_ON)
        return true;
    for (i = 0; i < def->nhostdevs; i++) {
        if (def->hostdevs[i]->mode == VIR_DOMAIN_HOSTDEV_MODE_CAPABILITIES &&
            def->hostdevs[i]->source.caps.type == VIR_DOMAIN_HOSTDEV_CAPS_TYPE_NET)
            return true;
    }
    return false;
}

/**
 * lxcContainerStart:
 * @def: pointer to virtual machine structure
 * @nveths: number of interfaces
 * @veths: interface names
 * @control: control FD to the container
 * @ttyPath: path of tty to set as the container console
 *
 * Starts a container process by calling clone() with the namespace flags
 *
 * Returns PID of container on success or -1 in case of error
 */
int lxcContainerStart(virDomainDefPtr def,
                      virSecurityManagerPtr securityDriver,
                      size_t nveths,
                      char **veths,
                      size_t npassFDs,
                      int *passFDs,
                      int control,
                      int handshakefd,
                      size_t nttyPaths,
                      char **ttyPaths)
{
    pid_t pid;
    int cflags;
    int stacksize = getpagesize() * 4;
    char *stack, *stacktop;
    lxc_child_argv_t args = {
        .config = def,
        .securityDriver = securityDriver,
        .nveths = nveths,
        .veths = veths,
        .npassFDs = npassFDs,
        .passFDs = passFDs,
        .monitor = control,
        .nttyPaths = nttyPaths,
        .ttyPaths = ttyPaths,
        .handshakefd = handshakefd
    };

    /* allocate a stack for the container */
    if (VIR_ALLOC_N(stack, stacksize) < 0)
        return -1;

    stacktop = stack + stacksize;

    cflags = CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWIPC|SIGCHLD;

    if (userns_required(def)) {
        if (userns_supported()) {
            VIR_DEBUG("Enable user namespace");
            cflags |= CLONE_NEWUSER;
        } else {
            virReportSystemError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                 _("Kernel doesn't support user namespace"));
            VIR_FREE(stack);
            return -1;
        }
    }

    if (lxcNeedNetworkNamespace(def)) {
        VIR_DEBUG("Enable network namespaces");
        cflags |= CLONE_NEWNET;
    }

    pid = clone(lxcContainerChild, stacktop, cflags, &args);
    VIR_FREE(stack);
    VIR_DEBUG("clone() completed, new container PID is %d", pid);

    if (pid < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to run clone container"));
        return -1;
    }

    return pid;
}

ATTRIBUTE_NORETURN static int
lxcContainerDummyChild(void *argv ATTRIBUTE_UNUSED)
{
    _exit(0);
}

int lxcContainerAvailable(int features)
{
    int flags = CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|
        CLONE_NEWIPC|SIGCHLD;
    int cpid;
    char *childStack;
    char *stack;
    int stacksize = getpagesize() * 4;

    if (features & LXC_CONTAINER_FEATURE_USER)
        flags |= CLONE_NEWUSER;

    if (features & LXC_CONTAINER_FEATURE_NET)
        flags |= CLONE_NEWNET;

    if (VIR_ALLOC_N(stack, stacksize) < 0)
        return -1;

    childStack = stack + stacksize;

    cpid = clone(lxcContainerDummyChild, childStack, flags, NULL);
    VIR_FREE(stack);
    if (cpid < 0) {
        char ebuf[1024] ATTRIBUTE_UNUSED;
        VIR_DEBUG("clone call returned %s, container support is not enabled",
                  virStrerror(errno, ebuf, sizeof(ebuf)));
        return -1;
    } else if (virProcessWait(cpid, NULL) < 0) {
        return -1;
    }

    VIR_DEBUG("container support is enabled");
    return 0;
}

int lxcContainerChown(virDomainDefPtr def, const char *path)
{
    uid_t uid;
    gid_t gid;

    if (!def->idmap.uidmap)
        return 0;

    uid = def->idmap.uidmap[0].target;
    gid = def->idmap.gidmap[0].target;

    if (chown(path, uid, gid) < 0) {
        virReportSystemError(errno,
                             _("Failed to change owner of %s to %u:%u"),
                             path, uid, gid);
        return -1;
    }

    return 0;
}
