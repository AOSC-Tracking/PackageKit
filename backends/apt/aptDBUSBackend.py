#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Provides an apt backend to PackageKit

Copyright (C) 2007 Ali Sabil <ali.sabil@gmail.com>
Copyright (C) 2007 Tom Parker <palfrey@tevp.net>
Copyright (C) 2008 Sebastian Heinlein <glatzor@ubuntu.com>

Licensed under the GNU General Public License Version 2

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
"""

__author__  = "Sebastian Heinlein <devel@glatzor.de>"

import errno
import fcntl
import gdbm
import httplib
import locale
import logging
import optparse
import os
import pty
import re
import signal
import shutil
import socket
import stat
import string
import subprocess
import sys
import time
import threading
import urllib2
import warnings

import apt
import apt_pkg
import dbus
import dbus.glib
import dbus.service
import dbus.mainloop.glib
import gobject

from packagekit.daemonBackend import PACKAGEKIT_DBUS_INTERFACE, PACKAGEKIT_DBUS_PATH, PackageKitBaseBackend, PackagekitProgress, pklog, threaded, serialize
from packagekit.enums import *

import debfile

warnings.filterwarnings(action='ignore', category=FutureWarning)

PACKAGEKIT_DBUS_SERVICE = 'org.freedesktop.PackageKitAptBackend'

apt_pkg.InitConfig()
apt_pkg.Config.Set("DPkg::Options::", '--force-confdef')
apt_pkg.Config.Set("DPkg::Options::", '--force-confold')

# Xapian database is optionally used to speed up package description search
XAPIAN_DB_PATH = os.environ.get("AXI_DB_PATH", "/var/lib/apt-xapian-index")
XAPIAN_DB = XAPIAN_DB_PATH + "/index"
XAPIAN_DB_VALUES = XAPIAN_DB_PATH + "/values"
XAPIAN_SUPPORT = False
try:
    import xapian
except ImportError:
    pass
else:
    if os.access(XAPIAN_DB, os.R_OK):
        pklog.debug("Use XAPIAN for the search")
        XAPIAN_SUPPORT = True

# SoftwareProperties is required to proivde information about repositories
try:
    import softwareproperties.SoftwareProperties
except ImportError:
    REPOS_SUPPORT = False
else:
    REPOS_SUPPORT = True

# Check if update-manager-core is installed to get aware of the
# latest distro releases
try:
    from UpdateManager.Core.MetaRelease import MetaReleaseCore
except ImportError:
    META_RELEASE_SUPPORT = False
else:
    META_RELEASE_SUPPORT = True


# Set a timeout for the changelog download
socket.setdefaulttimeout(2)

# Required for daemon mode
os.putenv("PATH",
          "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")
# Avoid questions from the maintainer scripts as far as possible
os.putenv("DEBIAN_FRONTEND", "noninteractive")
os.putenv("APT_LISTCHANGES_FRONTEND", "none")

# Setup threading support
gobject.threads_init()
dbus.glib.threads_init()

# Map Debian sections to the PackageKit group name space
SECTION_GROUP_MAP = {
    "admin" : GROUP_ADMIN_TOOLS,
    "base" : GROUP_SYSTEM,
    "comm" : GROUP_COMMUNICATION,
    "devel" : GROUP_PROGRAMMING,
    "doc" : GROUP_DOCUMENTATION,
    "editors" : GROUP_PUBLISHING,
    "electronics" : GROUP_ELECTRONICS,
    "embedded" : GROUP_SYSTEM,
    "games" : GROUP_GAMES,
    "gnome" : GROUP_DESKTOP_GNOME,
    "graphics" : GROUP_GRAPHICS,
    "hamradio" : GROUP_COMMUNICATION,
    "interpreters" : GROUP_PROGRAMMING,
    "kde" : GROUP_DESKTOP_KDE,
    "libdevel" : GROUP_PROGRAMMING,
    "libs" : GROUP_SYSTEM,
    "mail" : GROUP_INTERNET,
    "math" : GROUP_SCIENCE,
    "misc" : GROUP_OTHER,
    "net" : GROUP_NETWORK,
    "news" : GROUP_INTERNET,
    "oldlibs" : GROUP_LEGACY,
    "otherosfs" : GROUP_SYSTEM,
    "perl" : GROUP_PROGRAMMING,
    "python" : GROUP_PROGRAMMING,
    "science" : GROUP_SCIENCE,
    "shells" : GROUP_SYSTEM,
    "sound" : GROUP_MULTIMEDIA,
    "tex" : GROUP_PUBLISHING,
    "text" : GROUP_PUBLISHING,
    "utils" : GROUP_ACCESSORIES,
    "web" : GROUP_INTERNET,
    "x11" : GROUP_DESKTOP_OTHER,
    "unknown" : GROUP_UNKNOWN,
    "alien" : GROUP_UNKNOWN,
    "translations" : GROUP_LOCALIZATION,
    "metapackages" : GROUP_COLLECTIONS }

# Regular expressions to detect bug numbers in changelogs according to the
# Debian Policy Chapter 4.4. For details see the footnote 16:
# http://www.debian.org/doc/debian-policy/footnotes.html#f16
MATCH_BUG_CLOSES_DEBIAN=r"closes:\s*(?:bug)?\#?\s?\d+(?:,\s*(?:bug)?\#?\s?\d+)*"
MATCH_BUG_NUMBERS=r"\#?\s?(\d+)"
# URL pointing to a bug in the Debian bug tracker
HREF_BUG_DEBIAN="http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=%s"

# Regular expression to find cve references
MATCH_CVE="CVE-\d{4}-\d{4}"
HREF_CVE="http://web.nvd.nist.gov/view/vuln/detail?vulnId=%s"

def unlock_cache_afterwards(func):
    '''
    Make sure that the package cache is unlocked after the decorated function
    was called
    '''
    def wrapper(*args, **kwargs):
        backend = args[0]
        func(*args, **kwargs)
        backend._unlock_cache()
    wrapper.__name__ = func.__name__
    return wrapper


class PKError(Exception):
    pass

class PackageManagerFailedPKError(PKError):
    def __init__(self, msg, pkg, output):
        self.message = msg
        self.package = pkg
        self.output = output

class InstallTimeOutPKError(PKError):
    pass

class PackageKitCache(apt.cache.Cache):
    """
    Enhanced version of the apt.cache.Cache class which supports some features
    which can only be found in the consolidate branch of python-apt
    """
    def __iter__(self):
        """
        Let the cache behave like a sorted list of packages
        """
        for pkgname in sorted(self._dict.keys()):
            yield self._dict[pkgname]
        raise StopIteration

    def isVirtualPackage(self, name):
        """ 
        Return True if the package of the given name is a virtual package
        """
        try:
            virtual_pkg = self._cache[name]
        except KeyError:
            return False
        if len(virtual_pkg.VersionList) == 0:
            return True
        return False

    def getProvidingPackages(self, virtual):
        """
        Return a list of packages which provide the virtual package of the
        specified name
        """
        providers = []
        try:
            vp = self._cache[virtual]
            if len(vp.VersionList) != 0:
                return providers
        except KeyError:
            return providers
        for pkg in self:
            v = self._depcache.GetCandidateVer(pkg._pkg)
            if v == None:
                continue
            for p in v.ProvidesList:
                #print virtual
                #print p[0]
                if virtual == p[0]:
                    # we found a pkg that provides this virtual
                    # pkg, check if the proivdes is any good
                    providers.append(pkg)
                    #cand = self._cache[pkg.name]
                    #candver = self._cache._depcache.GetCandidateVer(cand._pkg)
                    #instver = cand._pkg.CurrentVer
                    #res = apt_pkg.CheckDep(candver.VerStr,oper,ver)
                    #if res == True:
                    #    self._dbg(1,"we can use %s" % pkg.name)
                    #    or_found = True
                    #    break
        return providers

    def clear(self):
        """ Unmark all changes """
        self._depcache.Init()


class DpkgInstallProgress(apt.progress.InstallProgress):
    """
    Class to initiate and monitor installation of local package files with dpkg
    """
    def recover(self):
        """
        Run "dpkg --configure -a"
        """
        cmd = ["/usr/bin/dpkg", "--status-fd", str(self.writefd),
               "--root", apt_pkg.Config["Dir"],
               "--force-confdef", "--force-confold", 
               "--configure", "-a"]
        self.run(cmd)

    def install(self, filenames):
        """
        Install the given package using a dpkg command line call
        """
        cmd = ["/usr/bin/dpkg", "--force-confdef", "--force-confold",
               "--status-fd", str(self.writefd), 
               "--root", apt_pkg.Config["Dir"], "-i"]
        cmd.extend(map(lambda f: str(f), filenames))
        self.run(cmd)

    def run(self, cmd):
        """
        Run and monitor a dpkg command line call
        """
        pklog.debug("Executing: %s" % cmd)
        (self.master_fd, slave) = pty.openpty()
        fcntl.fcntl(self.master_fd, fcntl.F_SETFL, os.O_NONBLOCK)
        p = subprocess.Popen(cmd, stdout=slave, stdin=slave)
        self.child_pid = p.pid
        res = self.waitChild()
        return res

    def updateInterface(self):
        """
        Process status messages from dpkg
        """
        if self.statusfd == None:
            return
        try:
            while not self.read.endswith("\n"):
                self.read += os.read(self.statusfd.fileno(), 1)
        except OSError, (error_no, error_str):
            # resource temporarly unavailable is ignored
            if error_no not in [errno.EAGAIN, errno.EWOULDBLOCK]:
                print error_str
        if self.read.endswith("\n"):
            statusl = string.split(self.read, ":")
            if len(statusl) < 3:
                pklog.warn("got garbage from dpkg: '%s'" % self.read)
                self.read = ""
            status = statusl[2].strip()
            pkg = statusl[1].strip()
            #print status
            if status == "error":
                self.error(pkg, status)
            elif status == "conffile-prompt":
                # we get a string like this:
                # 'current-conffile' 'new-conffile' useredited distedited
                match = re.search(".+conffile-prompt : '(.+)' '(.+)'",
                                  self.read)
                self.conffile(match.group(1), match.group(2))
            else:
                pklog.debug("Dpkg status: %s" % status)
                self.status = status
            self.read = ""


class PackageKitOpProgress(apt.progress.OpProgress):
    '''
    Handle the cache opening process
    '''
    def __init__(self, backend, prange=(0,100), progress=True):
        self._backend = backend
        apt.progress.OpProgress.__init__(self)
        self.steps = []
        for v in [0.12, 0.25, 0.50, 0.75, 1.00]:
            s = prange[0] + (prange[1] - prange[0]) * v
            self.steps.append(s)
        self.pstart = float(prange[0])
        self.pend = self.steps.pop(0)
        self.pprev = None
        self.show_progress = progress

    # OpProgress callbacks
    def update(self, percent):
        progress = int(self.pstart + percent / 100 * (self.pend - self.pstart))
        if self.show_progress == True and self.pprev < progress:
            self._backend.PercentageChanged(progress)
            self.pprev = progress

    def done(self):
        self.pstart = self.pend
        try:
            self.pend = self.steps.pop(0)
        except:
            pklog.warning("An additional step to open the cache is required")


class PackageKitFetchProgress(apt.progress.FetchProgress):
    '''
    Handle the package download process
    '''
    def __init__(self, backend, prange=(0,100)):
        self._backend = backend
        apt.progress.FetchProgress.__init__(self)
        self.pstart = prange[0]
        self.pend = prange[1]
        self.pprev = None

    def pulse(self):
        apt.progress.FetchProgress.pulse(self)
        # Strange, but we seem to need this to detect a cancel immediately
        time.sleep(0.01)
        if self._backend._canceled.isSet():
            return False
        progress = int(self.pstart + self.percent/100 * \
                       (self.pend - self.pstart))
        if self.pprev < progress:
            self._backend.PercentageChanged(progress)
            self.pprev = progress
        return True

    def start(self):
        self._backend.StatusChanged(STATUS_DOWNLOAD)
        self._backend.AllowCancel(True)

    def stop(self):
        self._backend.PercentageChanged(self.pend)
        self._backend.AllowCancel(False)

    def mediaChange(self, medium, drive):
        #FIXME: Raise an expcetion and handle it in _commit_changes
        #       Strangly _commit_changes does not catch the expcetion
        self._backend.Message(MESSAGE_UNKNOWN,
                              "Installing from CD-Rom (%s) is not "
                              "supported." % medium)
        return False


class PackageKitInstallProgress(apt.progress.InstallProgress):
    '''
    Handle the installation and removal process. Bits taken from
    DistUpgradeViewNonInteractive.
    '''
    def __init__(self, backend, prange=(0,100)):
        apt.progress.InstallProgress.__init__(self)
        self._backend = backend
        self.pstart = prange[0]
        self.pend = prange[1]
        self.pprev = None
        self.last_activity = None
        self.conffile_prompts = set()
        # insanly long timeout to be able to kill hanging maintainer scripts
        self.timeout = 10 * 60
        self.start_time = None
        self.output = ""
        self.master_fd = None
        self.child_pid = None

    def statusChange(self, pkg, percent, status):
        self.last_activity = time.time()
        progress = self.pstart + percent/100 * (self.pend - self.pstart)
        if self.pprev < progress:
            self._backend.PercentageChanged(int(progress))
            self.pprev = progress
        pklog.debug("APT status: %s" % status)

    def startUpdate(self):
        # The apt system lock was set by _lock_cache() before
        self._backend._unlock_cache()
        self._backend.StatusChanged(STATUS_COMMIT)
        self.last_activity = time.time()
        self.start_time = time.time()

    def fork(self):
        pklog.debug("fork()")
        (pid, self.master_fd) = pty.fork()
        if pid != 0:
            fcntl.fcntl(self.master_fd, fcntl.F_SETFL, os.O_NONBLOCK)
        return pid

    def updateInterface(self):
        apt.progress.InstallProgress.updateInterface(self)
        # Collect the output from the package manager
        try:
            out = os.read(self.master_fd, 512)
            self.output = self.output + out
            pklog.debug("APT out: %s " % out)
        except OSError:
            pass
        # catch a time out by sending crtl+c
        if self.last_activity + self.timeout < time.time():
            pklog.critical("no activity for %s time sending ctrl-c" \
                           % self.timeout)
            os.write(self.master_fd, chr(3))
            #FIXME: include this into the normal install progress and add 
            #       correct package information
            raise InstallTimeOutPKError(self.output)

    def conffile(self, current, new):
        pklog.warning("Config file prompt: '%s' (sending no)" % current)
        self.conffile_prompts.add(new)

    def error(self, pkg, msg):
        raise PackageManagerFailedPKError(pkg, msg, self.output)

    def finishUpdate(self):
        pklog.debug("finishUpdate()")
        if self.conffile_prompts:
            self._backend.Message(MESSAGE_CONFIG_FILES_CHANGED, 
                                  "The following conffile prompts were found "
                                  "and need investiagtion: %s" % \
                                  "\n".join(self.conffile_prompts))
        # Check for required restarts
        if os.path.exists("/var/run/reboot-required") and \
           os.path.getmtime("/var/run/reboot-required") > self.start_time:
            self._backend.RequireRestart(RESTART_SYSTEM, "")


class PackageKitDpkgInstallProgress(DpkgInstallProgress,
                                    PackageKitInstallProgress):
    """
    Class to integrate the progress of core dpkg operations into PackageKit
    """
    def run(self, filenames):
        return DpkgInstallProgress.run(self, filenames)

    def updateInterface(self):
        DpkgInstallProgress.updateInterface(self)
        try:
            out = os.read(self.master_fd, 512)
            self.output += out
            if out != "": pklog.debug("Dpkg out: %s" % out)
        except OSError:
            pass
        # we timed out, send ctrl-c
        if self.last_activity + self.timeout < time.time():
            pklog.critical("no activity for %s time sending "
                           "ctrl-c" % self.timeout)
            os.write(self.master_fd, chr(3))
            raise InstallTimeOutPKError(self.output)


if REPOS_SUPPORT == True:
    class PackageKitSoftwareProperties(softwareproperties.SoftwareProperties.SoftwareProperties):
        """
        Helper class to fix a siily bug in python-software-properties
        """
        def set_modified_sourceslist(self):
            self.save_sourceslist()


class PackageKitAptBackend(PackageKitBaseBackend):
    '''
    PackageKit backend for apt
    '''
    def __init__(self, bus_name, dbus_path):
        pklog.info("Initializing APT backend")
        signal.signal(signal.SIGQUIT, sigquit)
        self._cache = None
        self._canceled = threading.Event()
        self._canceled.clear()
        self._lock = threading.Lock()
        self._last_cache_refresh = None
        PackageKitBaseBackend.__init__(self, bus_name, dbus_path)

    # Methods ( client -> engine -> backend )

    def doInit(self):
        pklog.info("Initializing cache")
        self.StatusChanged(STATUS_RUNNING)
        self.AllowCancel(False)
        self.NoPercentageUpdates()
        self._open_cache(progress=False)

    @serialize
    def doExit(self):
        gobject.idle_add(self._doExitDelay)

    def doCancel(self):
        pklog.info("Canceling current action")
        self.StatusChanged(STATUS_CANCEL)
        self._canceled.set()
        self._canceled.wait()

    @serialize
    @threaded
    def doSearchFile(self, filters, filename):
        '''
        Implement the apt2-search-file functionality

        Apt specific: Works only for installed files. Since config files are
        not removed by default even not installed packages can be reported.
        '''
        pklog.info("Searching for file: %s" % filename)
        self.StatusChanged(STATUS_QUERY)
        self.NoPercentageUpdates()
        self._check_init(progress=False)
        self.AllowCancel(True)

        for pkg in self._cache:
            if self._check_canceled(): return False
            for installed_file in self._get_installed_files(pkg):
                if filename in installed_file:
                    self._emit_visible_package(filters, pkg)
                    break
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doSearchGroup(self, filters, group):
        '''
        Implement the apt2-search-group functionality
        '''
        pklog.info("Searching for group: %s" % group)
        self.StatusChanged(STATUS_QUERY)
        self.NoPercentageUpdates()
        self._check_init(progress=False)
        self.AllowCancel(True)

        for pkg in self._cache:
            if self._check_canceled(): return False
            elif self._get_package_group(pkg) == group:
                self._emit_visible_package(filters, pkg)
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doSearchName(self, filters, search):
        '''
        Implement the apt2-search-name functionality
        '''
        pklog.info("Searching for package name: %s" % search)
        self.StatusChanged(STATUS_QUERY)
        self.NoPercentageUpdates()
        self._check_init(progress=False)
        self.AllowCancel(True)

        for pkg in self._cache:
            if self._check_canceled(): return False
            elif search in pkg.name:
                self._emit_visible_package(filters, pkg)
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doSearchDetails(self, filters, search):
        '''
        Implement the apt2-search-details functionality
        '''
        pklog.info("Searching for package name: %s" % search)
        self.StatusChanged(STATUS_QUERY)
        self.NoPercentageUpdates()
        self._check_init(progress=False)
        self.AllowCancel(True)
        results = []

        if XAPIAN_SUPPORT == True:
            search_flags = (xapian.QueryParser.FLAG_BOOLEAN |
                            xapian.QueryParser.FLAG_PHRASE |
                            xapian.QueryParser.FLAG_LOVEHATE |
                            xapian.QueryParser.FLAG_BOOLEAN_ANY_CASE)
            pklog.debug("Performing xapian db based search")
            db = xapian.Database(XAPIAN_DB)
            parser = xapian.QueryParser()
            query = parser.parse_query(unicode(search),
                                       search_flags)
            enquire = xapian.Enquire(db)
            enquire.set_query(query)
            matches = enquire.get_mset(0, 1000)
            for r in  map(lambda m: m[xapian.MSET_DOCUMENT].get_data(),
                          enquire.get_mset(0,1000)):
                if self._cache.has_key(r):
                    results.append(self._cache[r])
        else:
            pklog.debug("Performing apt cache based search")
            for p in self._cache._dict.values():
                if self._check_canceled(): return
                needle = search.strip().lower()
                haystack = p.description.lower()
                if p.name.find(needle) >= 0 or haystack.find(needle) >= 0:
                    results.append(p)

        for r in results:
            if self._check_canceled(): return
            self._emit_visible_package(filters, r)

        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doGetDistroUpgrades(self):
        '''
        Implement the {backend}-get-distro-upgrades functionality
        '''
        pklog.info("Get distro upgrades")
        self.StatusChanged(STATUS_INFO)
        self.AllowCancel(False)
        self.NoPercentageUpdates()

        if META_RELEASE_SUPPORT == False:
            if self._cache.has_key("update-manager-core") and \
               self._cache["update-manager-core"].isInstalled == False:
                self.ErrorCode(ERROR_UNKNOWN,
                               "Please install the package update-manager-core to get notified "
                               "of the latest distribution releases.")
            else:
                self.ErrorCode(ERROR_UNKNOWN,
                               "Please make sure that update-manager-core is"
                               "correctly installed.")
            self.Finished(EXIT_FAILED)
            return

        #FIXME Evil to start the download during init
        meta_release = MetaReleaseCore(False, False)
        #FIXME: should use a lock
        while meta_release.downloading:
            time.sleep(1)
        #FIXME: Add support for description
        if meta_release.new_dist != None:
            self.DistroUpgrade("stable", 
                               "%s %s" % (meta_release.new_dist.name,
                                          meta_release.new_dist.version),
                               "The latest stable release")
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doGetUpdates(self, filters):
        '''
        Implement the {backend}-get-update functionality
        '''
        def succeeds_security_update(pkg):
            """
            Return True if an update succeeds a previous security update

            An example would be a package with version 1.1 in the security
            archive and 1.1.1 in the archive of proposed updates or the
            same version in both archives.
            """
            inst_ver = pkg._pkg.CurrentVer
            for ver in pkg._pkg.VersionList:
                # Skip versions which are not later
                if inst_ver and \
                   apt_pkg.VersionCompare(ver.VerStr, inst_ver.VerStr) <= 0:
                    continue
                for(verFileIter, index) in ver.FileList:
                    if verFileIter.Origin in ["Debian", "Ubuntu"] and \
                       (verFileIter.Archive.endswith("-security") or \
                        verFileIter.Label == "Debian-Security"):
                        indexfile = pkg._list.FindIndex(verFileIter)
                        if indexfile and indexfile.IsTrusted:
                            return True
            return False
        #FIXME: Implment the basename filter
        pklog.info("Get updates")
        self.StatusChanged(STATUS_QUERY)
        self.AllowCancel(True)
        self.NoPercentageUpdates()
        self._check_init(progress=False)
        self._cache.upgrade(False)
        updates = filter(lambda p: self._cache[p].isUpgradable,
                         self._cache.keys())
        for pkg in self._cache.getChanges():
            if self._check_canceled(): return False
            else:
                updates.remove(pkg.name)
                info = INFO_NORMAL
                archive = pkg.candidateOrigin[0].archive
                origin = pkg.candidateOrigin[0].origin
                trusted = pkg.candidateOrigin[0].trusted
                label = pkg.candidateOrigin[0].label
                if origin in ["Debian", "Ubuntu"] and trusted == True:
                    if archive.endswith("-security") or \
                       label == "Debian-Security":
                        info = INFO_SECURITY
                    elif succeeds_security_update(pkg):
                        pklog.debug("Update of %s succeeds a security "
                                    "update. Raising its priority." % pkg.name)
                        info = INFO_SECURITY
                    elif archive.endswith("-backports"):
                        info = INFO_ENHANCEMENT
                    elif archive.endswith("-updates"):
                        info = INFO_BUGFIX
                if origin in ["Backports.org archive"] and trusted == True:
                    info = INFO_ENHANCEMENT
                self._emit_package(pkg, info, force_candidate=True)
        # Report packages that are upgradable but cannot be upgraded
        for missed in updates:
            self._emit_package(self._cache[missed], INFO_BLOCKED)
        self._cache.clear()
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doGetUpdateDetail(self, pkg_ids):
        '''
        Implement the {backend}-get-update-details functionality
        '''
        def get_bug_urls(changelog):
            """
            Create a list of urls pointing to closed bugs in the changelog
            """
            urls = []
            #FIXME: Add support for Launchpad/Ubuntu
            for r in re.findall(MATCH_BUG_CLOSES_DEBIAN, changelog,
                                re.IGNORECASE | re.MULTILINE):
                urls.extend(map(lambda b: HREF_BUG_DEBIAN % b,
                                re.findall(MATCH_BUG_NUMBERS, r)))
            return urls

        def get_cve_urls(changelog):
            """
            Create a list of urls pointing to cves referred in the changelog
            """
            return map(lambda c: HREF_CVE % c,
                       re.findall(MATCH_CVE, changelog, re.MULTILINE))

        pklog.info("Get update details of %s" % pkg_ids)
        self.StatusChanged(STATUS_INFO)
        self.NoPercentageUpdates()
        self.AllowCancel(True)
        self._check_init(progress=False)
        for pkg_id in pkg_ids:
            if self._check_canceled(): return
            pkg = self._find_package_by_id(pkg_id)
            if pkg == None:
                self.ErrorCode(ERROR_PACKAGE_NOT_FOUND,
                               "Package %s isn't available" % id)
                self.Finished(EXIT_FAILED)
                return
            # FIXME add some real data
            updates = self.get_id_from_package(pkg, force_candidate=False)
            obsoletes = ""
            vendor_url = ""
            restart = ""
            update_text = ""
            state = ""
            issued = ""
            updated = ""
            #FIXME: Replace this method with the python-apt one as soon as the
            #       consolidate branch gets merged
            self.StatusChanged(STATUS_DOWNLOAD_CHANGELOG)
            changelog = self._get_changelog(pkg)
            self.StatusChanged(STATUS_INFO)
            bugzilla_url = ";".join(get_bug_urls(changelog))
            cve_url = ";".join(get_cve_urls(changelog))
            self.UpdateDetail(pkg_id, updates, obsoletes, vendor_url,
                              bugzilla_url, cve_url, restart, update_text,
                              changelog, state, issued, updated)
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doGetDetails(self, pkg_ids):
        '''
        Implement the {backend}-get-details functionality
        '''
        pklog.info("Get details of %s" % pkg_ids)
        self.StatusChanged(STATUS_DEP_RESOLVE)
        self.NoPercentageUpdates()
        self.AllowCancel(True)
        self._check_init(progress=False)
        for pkg_id in pkg_ids:
            if self._check_canceled(): return
            pkg = self._find_package_by_id(pkg_id)
            if pkg == None:
                self.ErrorCode(ERROR_PACKAGE_NOT_FOUND,
                               "Package %s isn't available" % id)
                self.Finished(EXIT_FAILED)
                return
            desc = self._get_package_description(pkg)
            #FIXME: We need more fine grained license information!
            candidate = pkg.candidateOrigin
            if candidate != None and  \
               candidate[0].component in ["main", "universe"] and \
               candidate[0].origin in ["Debian", "Ubuntu"]:
                license = "free"
            else:
                license = "unknown"
            group = self._get_package_group(pkg)
            self.Details(pkg_id, license, group, desc,
                         pkg.homepage, pkg.packageSize)
            self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    @unlock_cache_afterwards
    def doUpdateSystem(self):
        '''
        Implement the {backend}-update-system functionality
        '''
        pklog.info("Upgrading system")
        if not self._lock_cache(): return
        self.StatusChanged(STATUS_UPDATE)
        self.AllowCancel(False)
        self.PercentageChanged(0)
        self._check_init(prange=(0,5))
        try:
            self._cache.upgrade(distUpgrade=False)
        except:
            self._cache.clear()
            self.ErrorCode(ERROR_DEP_RESOLUTION_FAILED,
                           "Failed to upgrade the system.")
            self.Finished(EXIT_FAILED)
            return
        if not self._commit_changes(): return False
        self.PercentageChanged(100)
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    @unlock_cache_afterwards
    def doRemovePackages(self, ids, deps=True, auto=False):
        '''
        Implement the {backend}-remove functionality
        '''
        pklog.info("Removing package(s): id %s" % ids)
        if not self._lock_cache(): return
        self.StatusChanged(STATUS_REMOVE)
        self.AllowCancel(False)
        self.PercentageChanged(0)
        self._check_init(prange=(0,10))
        pkgs=[]
        for id in ids:
            pkg = self._find_package_by_id(id)
            if pkg == None:
                self.ErrorCode(ERROR_PACKAGE_NOT_FOUND,
                               "Package %s isn't available" % id)
                self.Finished(EXIT_FAILED)
                return
            if not pkg.isInstalled:
                self.ErrorCode(ERROR_PACKAGE_NOT_INSTALLED,
                               "Package %s isn't installed" % pkg.name)
                self.Finished(EXIT_FAILED)
                return
            pkgs.append(pkg.name[:])
            if pkg._pkg.Essential == True:
                self.ErrorCode(ERROR_CANNOT_REMOVE_SYSTEM_PACKAGE,
                               "Package %s cannot be removed." % pkg.name)
                self.Finished(EXIT_FAILED)
                return
            try:
                pkg.markDelete()
            except:
                self._open_cache(prange=(90,99))
                self.ErrorCode(ERROR_UNKNOWN, "Removal of %s failed" % pkg.name)
                self.Finished(EXIT_FAILED)
                return
        if not self._commit_changes(fetch_range=(10,10),
                                    install_range=(10,90)):
            return False
        self._open_cache(prange=(90,99))
        for p in pkgs:
            if self._cache.has_key(p) and self._cache[p].isInstalled:
                self.ErrorCode(ERROR_UNKNOWN, "%s is still installed" % p)
                self.Finished(EXIT_FAILED)
                return
        self.PercentageChanged(100)
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doGetRepoList(self, filters):
        '''
        Implement the {backend}-get-repo-list functionality

        FIXME: should we use the abstration of software-properties or provide
               low level access using pure aptsources?
        '''
        pklog.info("Getting repository list: %s" % filters)
        self.StatusChanged(STATUS_INFO)
        self.AllowCancel(False)
        self.PercentageChanged(0)
        if REPOS_SUPPORT == False:
            if self._cache.has_key("python-software-properties") and \
               self._cache["python-software-properties"].isInstalled == False:
                self.ErrorCode(ERROR_UNKNOWN,
                               "Please install the package "
                               "python-software-properties to handle repositories")
            else:
                self.ErrorCode(ERROR_UNKNOWN,
                               "Please make sure that python-software-properties is"
                               "correctly installed.")
            self.Finished(EXIT_FAILED)
            return
        filter_list = filters.split(";")
        repos = PackageKitSoftwareProperties()
        # Emit distro components as virtual repositories
        for comp in repos.distro.source_template.components:
            repo_id = "%s_comp_%s" % (repos.distro.id, comp.name)
            description = "%s %s - %s (%s)" % (repos.distro.id,
                                               repos.distro.release,
                                               comp.get_description(),
                                               comp.name)
            #FIXME: There is no inconsitent state in PackageKit
            enabled = repos.get_comp_download_state(comp)[0]
            if not FILTER_DEVELOPMENT in filter_list:
                self.RepoDetail(repo_id, description, enabled)
        # Emit distro's virtual update repositories
        for template in repos.distro.source_template.children:
            repo_id = "%s_child_%s" % (repos.distro.id, template.name)
            description = "%s %s - %s (%s)" % (repos.distro.id,
                                               repos.distro.release,
                                               template.description,
                                               template.name)
            #FIXME: There is no inconsitent state in PackageKit
            enabled = repos.get_comp_child_state(template)[0]
            if not FILTER_DEVELOPMENT in filter_list:
                self.RepoDetail(repo_id, description, enabled)
        # Emit distro's cdrom sources
        for source in repos.get_cdrom_sources():
            if FILTER_NOT_DEVELOPMENT in filter_list and \
               source.type in ("deb-src", "rpm-src"):
                continue
            enabled = not source.disabled
            # Remove markups from the description
            description = re.sub(r"</?b>", "", repos.render_source(source))
            repo_id = "cdrom_%s_%s" % (source.uri, source.dist)
            repo_id.join(map(lambda c: "_%s" % c, source.comps))
            self.RepoDetail(repo_id, description, enabled)
        # Emit distro's virtual source code repositoriy
        if not FILTER_NOT_DEVELOPMENT in filter_list:
            repo_id = "%s_source" % repos.distro.id
            enabled = repos.get_source_code_state() or False
            #FIXME: no translation :(
            description = "%s %s - Source code" % (repos.distro.id, 
                                                   repos.distro.release)
            self.RepoDetail(repo_id, description, enabled)
        # Emit third party repositories
        for source in repos.get_isv_sources():
            if FILTER_NOT_DEVELOPMENT in filter_list and \
               source.type in ("deb-src", "rpm-src"):
                continue
            enabled = not source.disabled
            # Remove markups from the description
            description = re.sub(r"</?b>", "", repos.render_source(source))
            repo_id = "isv_%s_%s" % (source.uri, source.dist)
            repo_id.join(map(lambda c: "_%s" % c, source.comps))
            self.RepoDetail(repo_id, description, enabled)
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doRepoEnable(self, repo_id, enable):
        '''
        Implement the {backend}-repo-enable functionality

        FIXME: should we use the abstration of software-properties or provide
               low level access using pure aptsources?
        '''
        pklog.info("Enabling repository: %s %s" % (repo_id, enable))
        self.StatusChanged(STATUS_RUNNING)
        self.AllowCancel(False)
        self.PercentageChanged(0)
        if REPOS_SUPPORT == False:
            if self._cache.has_key("python-software-properties") and \
               self._cache["python-software-properties"].isInstalled == False:
                self.ErrorCode(ERROR_UNKNOWN,
                               "Please install the package "
                               "python-software-properties to handle repositories")
            else:
                self.ErrorCode(ERROR_UNKNOWN,
                               "Please make sure that python-software-properties is"
                               "correctly installed.")
            self.Finished(EXIT_FAILED)
            return
        repos = PackageKitSoftwareProperties()

        found = False
        # Check if the repo_id matches a distro component, e.g. main
        if repo_id.startswith("%s_comp_" % repos.distro.id):
            for comp in repos.distro.source_template.components:
                if repo_id == "%s_comp_%s" % (repos.distro.id, comp.name):
                    if enable == repos.get_comp_download_state(comp)[0]:
                        pklog.debug("Repository is already enabled")
                        pass
                    if enable == True:
                        repos.enable_component(comp.name)
                    else:
                        repos.disable_component(comp.name)
                    found = True
                    break
        # Check if the repo_id matches a distro child repository, e.g. hardy-updates
        elif repo_id.startswith("%s_child_" % repos.distro.id):
            for template in repos.distro.source_template.children:
                if repo_id == "%s_child_%s" % (repos.distro.id, template.name):
                    if enable == repos.get_comp_child_state(template)[0]:
                        pklog.debug("Repository is already enabled")
                        pass
                    elif enable == True:
                        repos.enable_child_source(template)
                    else:
                        repos.disable_child_source(template)
                    found = True
                    break
        # Check if the repo_id matches a cdrom repository
        elif repo_id.startswith("cdrom_"):
            for source in repos.get_isv_sources():
                source_id = "cdrom_%s_%s" % (source.uri, source.dist)
                source_id.join(map(lambda c: "_%s" % c, source.comps))
                if repo_id == source_id:
                    if source.disabled == enable:
                        source.disabled = not enable
                        repos.save_sourceslist()
                    else:
                        pklog.debug("Repository is already enabled")
                    found = True
                    break
        # Check if the repo_id matches an isv repository
        elif repo_id.startswith("isv_"):
            for source in repos.get_isv_sources():
                source_id = "isv_%s_%s" % (source.uri, source.dist)
                source_id.join(map(lambda c: "_%s" % c, source.comps))
                if repo_id == source_id:
                    if source.disabled == enable:
                        source.disabled = not enable
                        repos.save_sourceslist()
                    else:
                        pklog.debug("Repository is already enabled")
                    found = True
                    break
        if found == False:
            self.ErrorCode(ERROR_REPO_NOT_AVAILABLE,
                           "The repository of the id %s isn't available" % repo_id)
            self.Finished(EXIT_FAILED)
            return
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    @unlock_cache_afterwards
    def doUpdatePackages(self, ids):
        '''
        Implement the {backend}-update functionality
        '''
        pklog.info("Updating package with id %s" % ids)
        if not self._lock_cache(): return
        self.StatusChanged(STATUS_UPDATE)
        self.AllowCancel(False)
        self.PercentageChanged(0)
        self._check_init(prange=(0,10))
        pkgs=[]
        for id in ids:
            pkg = self._find_package_by_id(id)
            if pkg == None:
                self.ErrorCode(ERROR_PACKAGE_NOT_FOUND,
                               "Package %s isn't available" % id)
                self.Finished(EXIT_FAILED)
                return
            if not pkg.isUpgradable:
                self.ErrorCode(ERROR_PACKAGE_ALREADY_INSTALLED,
                               "Package %s is already up-to-date" % pkg.name)
                self.Finished(EXIT_FAILED)
                return
            pkgs.append(pkg.name[:])
            try:
                pkg.markUpgrade()
            except:
                self._open_cache(prange=(90,100))
                self.ErrorCode(ERROR_UNKNOWN, "%s could not be queued for "
                                              "update" % pkg.name)
                self.Finished(EXIT_FAILED)
                return
        if not self._commit_changes(): return False
        self._open_cache(prange=(90,100))
        self.PercentageChanged(100)
        pklog.debug("Checking success of operation")
        for p in pkgs:
            if not self._cache.has_key(p) or not self._cache[p].isInstalled \
               or self._cache[p].isUpgradable:
                self.ErrorCode(ERROR_UNKNOWN, "%s was not updated" % p)
                self.Finished(EXIT_FAILED)
                return
        pklog.debug("Sending success signal")
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doDownloadPackages(self, ids, dest):
        '''
        Implement the {backend}-download-packages functionality
        '''
        pklog.info("Downloading packages: %s" % ids)
        self.StatusChanged(STATUS_DOWNLOAD)
        self.AllowCancel(True)
        self.PercentageChanged(0)
        # Check the destination directory
        if not os.path.isdir(dest) or not os.access(dest, os.W_OK):
            self.ErrorCode(ERROR_UNKNOWN,
                           "The directory '%s' is not writable" % dest)
            self.Finished(EXIT_FAILED)
            return
        # Setup the fetcher
        self._check_init(prange=(0,10))
        progress = PackageKitFetchProgress(self, prange=(10,90))
        fetcher = apt_pkg.GetAcquire(progress)
        pm = apt_pkg.GetPackageManager(self._cache._depcache)
        recs = apt_pkg.GetPkgRecords(self._cache._cache)
        list = apt_pkg.GetPkgSourceList()
        list.ReadMainList()
        # Mark installed packages for reinstallation and not installed packages
        # for installation without dependencies
        for id in ids:
            if self._check_canceled(): return
            pkg = self._find_package_by_id(id)
            if pkg == None:
                self.ErrorCode(ERROR_PACKAGE_NOT_FOUND,
                               "There is no package %s" % id)
                self.Finished(EXIT_FAILED)
                return
            if pkg.isInstalled:
                self._cache._depcache.SetReInstall(pkg._pkg, True)
            else:
                self._cache._depcache.MarkInstall(pkg._pkg, False)
        # Download 
        pm.GetArchives(fetcher, list, recs)
        res = fetcher.Run()
        self._cache.clear()
        self.PercentageChanged(95)
        # Copy files from cache to final destination
        for item in fetcher.Items:
            if self._check_canceled(): return
            pklog.debug("Download item: %s" % item)
            if (item.Status != item.StatDone and not item.StatIdle) or \
                res == fetcher.ResultCancelled:
                self.ErrorCode(ERROR_PACKAGE_DOWNLOAD_FAILED,
                               "Failed to download %s" % item.DescURI)
                self.Finished(EXIT_FAILED)
                return
            pklog.debug("Copying %s to %s ..." % (item.DestFile, dest))
            try:
                shutil.copy(item.DestFile, dest)
            except Exception, e:
                self.ErrorCode(ERROR_INTERNAL_ERROR,
                               "Failed to copy %s to %s: %s" % (item.DestFile,
                                                                dest, e))
                self.Finished(EXIT_FAILED)
                return
        self.PercentageChanged(100)
        pklog.debug("Sending success signal")
        self.Finished(EXIT_SUCCESS)
 
    @serialize
    @threaded
    @unlock_cache_afterwards
    def doInstallPackages(self, ids):
        '''
        Implement the {backend}-install functionality
        '''
        pklog.info("Installing package with id %s" % ids)
        if not self._lock_cache(): return
        self.StatusChanged(STATUS_INSTALL)
        self.AllowCancel(False)
        self.PercentageChanged(0)
        self._check_init(prange=(0,10))
        pkgs=[]
        for id in ids:
            pkg = self._find_package_by_id(id)
            if pkg == None:
                self.ErrorCode(ERROR_PACKAGE_NOT_FOUND,
                               "Package %s isn't available" % id)
                self.Finished(EXIT_FAILED)
                return
            if pkg.isInstalled:
                self.ErrorCode(ERROR_PACKAGE_ALREADY_INSTALLED,
                               "Package %s is already installed" % pkg.name)
                self.Finished(EXIT_FAILED)
                return
            pkgs.append(pkg.name[:])
            try:
                pkg.markInstall()
            except Exception, e:
                self._open_cache(prange=(90,100))
                self.ErrorCode(ERROR_UNKNOWN, "%s could not be queued for "
                                              "installation: %s" % (pkg.name,e))
                self.Finished(EXIT_FAILED)
                return
        if not self._commit_changes(): return False
        self._open_cache(prange=(90,100))
        self.PercentageChanged(100)
        pklog.debug("Checking success of operation")
        for p in pkgs:
            if not self._cache.has_key(p) or not self._cache[p].isInstalled:
                self.ErrorCode(ERROR_UNKNOWN, "%s was not installed" % p)
                self.Finished(EXIT_FAILED)
                return
        pklog.debug("Sending success signal")
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    @unlock_cache_afterwards
    def doInstallFiles(self, trusted, full_paths):
        '''
        Implement install-files for the apt backend
        Install local Debian package files
        '''
        pklog.info("Installing package files: %s" % full_paths)
        if not self._lock_cache(): return
        self.StatusChanged(STATUS_INSTALL)
        self.AllowCancel(False)
        self.PercentageChanged(0)
        self._check_init(prange=(0,10))
        packages = []
        # Collect all dependencies which need to be installed
        self.StatusChanged(STATUS_DEP_RESOLVE)
        for path in full_paths:
            deb = debfile.DebPackage(path, self._cache)
            packages.append(deb)
            if not deb.checkDeb():
                self.ErrorCode(ERROR_UNKNOWN, deb._failureString)
                self.Finished(EXIT_FAILED)
                return
            (install, remove, unauthenticated) = deb.requiredChanges
            pklog.debug("Changes: Install %s, Remove %s, Unauthenticated "
                        "%s" % (install, remove, unauthenticated))
            if len(remove) > 0:
                self.ErrorCode(ERROR_DEP_RESOLUTION_FAILED, 
                               "Remove the following packages "
                               "before: %s" % remove)
                self.Finished(EXIT_FAILED)
                return
            if deb.compareToVersionInCache() == debfile.VERSION_OUTDATED:
                self.Message(MESSAGE_NEWER_PACKAGE_EXISTS, 
                             "There is a later version of %s "
                             "available in the repositories." % deb.pkgname)
        if len(self._cache.getChanges()) > 0 and not \
           self._commit_changes((10,25), (25,50)): 
            return False
       # Install the Debian package files
        d = PackageKitDpkgInstallProgress(self)
        try:
            d.startUpdate()
            d.install(full_paths)
            d.finishUpdate()
        except InstallTimeOutPKError, e:
            self._recover()
            #FIXME: should provide more information
            self.ErrorCode(ERROR_UNKNOWN,
                           "Transaction was cancelled since the installation "
                           "of a package hung.\n"
                           "This can be caused by maintainer scripts which "
                           "require input on the terminal:\n%s" % e.message)
            self.Finished(EXIT_KILLED)
            return
        except PackageManagerFailedPKError, e:
            self._recover()
            self.ErrorCode(ERROR_UNKNOWN, "%s\n%s" % (e.message, e.output))
            self.Finished(EXIT_FAILED)
            return
        except Exception, e:
            self._recover()
            self.ErrorCode(ERROR_INTERNAL_ERROR, e.message)
            self.Finished(EXIT_FAILED)
            return
        self.PercentageChanged(100)
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    @unlock_cache_afterwards
    def doRefreshCache(self, force):
        '''
        Implement the {backend}-refresh_cache functionality
        '''
        pklog.info("Refresh cache")
        if not self._lock_cache(): return
        self.StatusChanged(STATUS_REFRESH_CACHE)
        self.last_action_time = time.time()
        self.AllowCancel(False);
        self.PercentageChanged(0)
        self._check_init((0,10))
        try:
            self._cache.update(PackageKitFetchProgress(self, prange=(10,95)))
        except Exception, e:
            self._open_cache(prange=(95,100))
            if self._check_canceled(): return False
            self.ErrorCode(ERROR_UNKNOWN, "Refreshing cache failed: %s" % e)
            self.Finished(EXIT_FAILED)
            return
        self._open_cache(prange=(95,100))
        self.PercentageChanged(100)
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doGetPackages(self, filters):
        '''
        Implement the apt2-get-packages functionality
        '''
        pklog.info("Get all packages")
        self.StatusChanged(STATUS_QUERY)
        self.NoPercentageUpdates()
        self._check_init(progress=False)
        self.AllowCancel(True)

        for pkg in self._cache:
            if self._check_canceled(): return False
            elif self._is_package_visible(pkg, filters):
                self._emit_package(pkg)
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doResolve(self, filters, names):
        '''
        Implement the apt2-resolve functionality
        '''
        pklog.info("Resolve")
        self.StatusChanged(STATUS_QUERY)
        self.NoPercentageUpdates()
        self._check_init(progress=False)
        self.AllowCancel(False)

        for name in names:
            if self._cache.has_key(name):
                self._emit_visible_package(filters, self._cache[name])
            else:
                self.ErrorCode(ERROR_PACKAGE_NOT_FOUND,
                               "Package name %s could not be resolved" % name)
                self.Finished(EXIT_FAILED)
                return
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doGetDepends(self, filter, ids, recursive=False):
        '''
        Implement the apt2-get-depends functionality

        Emit all packages that need to be installed or updated to install
        the given package ids. It behaves like a preview of the changes
        required for the installation. An error will be emitted if the 
        dependecies cannot be satisfied.
        In contrast to the yum backend the whole dependency resoltions is done 
        by the package manager. Therefor the list of satisfied packages cannot
        be computed easily. GDebi features this. Perhaps this should be moved
        to python-apt.
        '''
        pklog.info("Get depends (%s,%s,%s)" % (filter, ids, recursive))
        #FIXME: recursive is not yet implemented
        if recursive == True:
            pklog.warn("Recursive dependencies are not implemented")
        self.StatusChanged(STATUS_QUERY)
        self.NoPercentageUpdates()
        self._check_init(progress=False)
        self.AllowCancel(True)

        # Mark all packages for installation
        pkgs = []
        for id in ids:
            if self._check_canceled(): return
            pkg = self._find_package_by_id(id)
            if pkg == None:
                self.ErrorCode(ERROR_PACKAGE_NOT_FOUND,
                               "Package %s isn't available" % id)
                self.Finished(EXIT_FAILED)
                return
            try:
                pkg.markInstall()
            except Exception, e:
                #FIXME: Introduce a new info enumerate PK_INFO_MISSING for
                #       missing dependecies
                self.ErrorCode(ERROR_DEP_RESOLUTION_FAILED,
                               "Dependecies for %s cannot be satisfied: %s" % e)
                self.Finished(EXIT_FAILED)
                return
            pkgs.append(pkg)
        # Check the status of the resulting changes
        for p in self._cache.getChanges():
            if self._check_canceled(): return
            if p in pkgs: continue
            if p.markedDelete:
                # Packagekit policy forbids removing packages for installation
                self.ErrorCode(ERROR_DEP_RESOLUTION_FAILED,
                               "Remove the package %s before" % p.name)
                self.Finished(EXIT_FAILED)
                return
            elif p.markedInstall or p.markedUpgrade:
                if self._is_package_visible(p, filter):
                    self._emit_package(p)
            else:
                self.ErrorCode(ERROR_DEP_RESOLUTION_FAILED,
                               "Please use an advanced package management tool "
                               "e.g. Synaptic or aptitude, since there is a "
                               "complex dependency situation.")
                self.Finished(EXIT_FAILED)
                return
        # Clean up
        self._cache.clear()
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doGetRequires(self, filter, ids, recursive=False):
        '''
        Implement the apt2-get-requires functionality
        '''
        pklog.info("Get requires (%s,%s,%s)" % (filter, ids, recursive))
        #FIXME: recursive is not yet implemented
        if recursive == True:
            pklog.warn("Recursive dependencies are not implemented")
        self.StatusChanged(STATUS_DEP_RESOLVE)
        self.NoPercentageUpdates()
        self._check_init(progress=False)
        self.AllowCancel(True)
        pkgs = []

        # Mark all packages for installation
        for id in ids:
            if self._check_canceled(): return
            pkg = self._find_package_by_id(id)
            if pkg == None:
                self.ErrorCode(ERROR_PACKAGE_NOT_FOUND,
                               "Package %s isn't available" % id)
                self.Finished(EXIT_FAILED)
                return
            if pkg._pkg.Essential == True:
                self.ErrorCode(ERROR_CANNOT_REMOVE_SYSTEM_PACKAGE,
                               "Package %s cannot be removed." % pkg.name)
                self.Finished(EXIT_FAILED)
                return
            pkgs.append(pkg)
            try:
                pkg.markDelete()
            except Exception, e:
                #FIXME: Introduce a new info enumerate PK_INFO_MISSING for
                #       missing dependecies
                self.ErrorCode(ERROR_DEP_RESOLUTION_FAILED,
                               "Error removing %s: %s" % (pkg.name, e))
                self.Finished(EXIT_FAILED)
                return
        # Check the status of the resulting changes
        for p in self._cache.getChanges():
            if self._check_canceled(): return
            if p.markedDelete:
                if not p in pkgs and self._is_package_visible(p, filter):
                    self._emit_package(p)
            else:
                self.ErrorCode(ERROR_DEP_RESOLUTION_FAILED,
                               "Please use an advanced package management tool "
                               "e.g. Synaptic or aptitude, since there is a "
                               "complex dependency situation.")
                self.Finished(EXIT_FAILED)
                return
        # Clean up
        self._cache.clear()
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doWhatProvides(self, filters, provides_type, search):
        def get_mapping_db(path):
            """
            Return the gdbm database at the given path or send an
            appropriate error message
            """
            if not os.access(path, os.R_OK):
                if self._cache.has_key("app-install-data") and \
                   self._cache["app-install-data"].isInstalled == False:
                    self.ErrorCode(ERROR_UNKNOWN,
                                   "Please install the package "
                                   "app-install data for a list of "
                                   "applications that can handle files of "
                                   "the given type")
                else:
                    self.ErrorCode(ERROR_UNKNOWN,
                                   "The list of applications that can handle "
                                   "files of the given type cannot be opened.\n"
                                   "Try to reinstall the package "
                                   "app-install-data.")
                return None
            try:
                db = gdbm.open(path)
            except:
                self.ErrorCode(ERROR_UNKNOWN,
                               "The list of applications that can handle "
                               "files of the given type cannot be opened.\n"
                               "Try to reinstall the package "
                               "app-install-data.")
                return None
            else:
                return db

        self.StatusChanged(STATUS_QUERY)
        self.NoPercentageUpdates()
        self._check_init(progress=False)
        self.AllowCancel(False)
        if provides_type == PROVIDES_CODEC:
            # The search term from the codec helper looks like this one:
            match = re.match(r"gstreamer([0-9\.]+)\((.+?)\)", search)
            if not match:
                self.ErrorCode(ERROR_UNKNOWN,
                               "The search term is invalid")
                self.Finished(EXIT_FAILED)
                return
            codec = "%s:%s" % (match.group(1), match.group(2))
            db = get_mapping_db("/var/cache/app-install/gai-codec-map.gdbm")
            if db == None:
                self.ErrorCode(ERROR_INTERNAL_ERROR,
                               "Failed to open codec mapping database")
                self.Finished(EXIT_FAILED)
                return
            if db.has_key(codec):
                # The codec mapping db stores the packages as a string
                # separated by spaces. Each package has its section
                # prefixed and separated by a slash
                # FIXME: Should make use of the section and emit a 
                #        RepositoryRequired signal if the package does 
                #        not exist
                pkgs = map(lambda s: s.split("/")[1],
                           db[codec].split(" "))
                self._emit_visible_packages_by_name(filters, pkgs)
        elif provides_type == PROVIDES_MIMETYPE:
            # Emit packages that contain an application that can handle
            # the given mime type
            handlers = set()
            db = get_mapping_db("/var/cache/app-install/gai-mime-map.gdbm")
            if db == None:
                self.Finished(EXIT_FAILED)
                return
            if db.has_key(search):
                pklog.debug("Mime type is registered: %s" % db[search])
                # The mime type handler db stores the packages as a string
                # separated by spaces. Each package has its section
                # prefixed and separated by a slash
                # FIXME: Should make use of the section and emit a 
                #        RepositoryRequired signal if the package does not exist
                handlers = map(lambda s: s.split("/")[1],
                               db[search].split(" "))
                self._emit_visible_packages_by_name(filters, handlers)
        else:
            self.ErrorCode(ERROR_NOT_SUPPORTED,
                           "This function is not implemented in this backend")
            self.Finished(EXIT_FAILED)
            return
        self.Finished(EXIT_SUCCESS)

    @serialize
    @threaded
    def doGetFiles(self, package_ids):
        """
        Emit the Files signal which includes the files included in a package
        Apt only supports this for installed packages
        """
        self.StatusChanged(STATUS_INFO)
        for id in package_ids:
            pkg = self._find_package_by_id(id)
            if pkg == None:
                self.ErrorCode(ERROR_PACKAGE_NOT_FOUND,
                               "Package %s doesn't exist" % pkg.name)
                self.Finished(EXIT_FAILED)
                return
            files = string.join(self._get_installed_files(pkg), ";")
            self.Files(id, files)
        self.Finished(EXIT_SUCCESS)

    def doSetProxy(self, http_proxy, ftp_proxy):
        '''
        Set a proxy server for http and ftp transfer
        '''
        if http_proxy:
            pklog.debug("Set http proxy to %s" % http_proxy)
            apt_pkg.Config.set("http::Proxy", http_proxy)
        if ftp_proxy:
            pklog.debug("Set ftp proxy to %s" % ftp_proxy)
            apt_pkg.Config.set("ftp::Proxy", ftp_proxy)

    def doSetLocale(self, code):
        '''
        Set the locale of the daemon

        '''
        #FIXME: Needs testing
        if code != "":
            pklog.debug("Setting language to %s" % code)
            locale.setlocale("LANG", code)

    # Helpers

    def _lock_cache(self):
        """
        Emit an error message and return true if the apt system lock cannot
        be acquired.
        """
        try:
            apt_pkg.PkgSystemLock()
        except SystemError:
            self.ErrorCode(ERROR_CANNOT_GET_LOCK,
                           "Only use one package management programme at the "
                           "the same time.")
            self.Finished(EXIT_FAILED)
            return False
        return True

    def _unlock_cache(self):
        """
        Unlock the system package cache
        """
        try:
            apt_pkg.PkgSystemUnLock()
        except SystemError:
            return False
        return True

    def _open_cache(self, prange=(0,100), progress=True):
        '''
        (Re)Open the APT cache
        '''
        pklog.debug("Open APT cache")
        self.StatusChanged(STATUS_LOADING_CACHE)
        try:
            self._cache = PackageKitCache(PackageKitOpProgress(self, prange,
                                                               progress))
        except:
            self.ErrorCode(ERROR_NO_CACHE, "Package cache could not be opened")
            self.Finished(EXIT_FAILED)
            self.Exit()
            return
        if self._cache._depcache.BrokenCount > 0:
            self.ErrorCode(ERROR_DEP_RESOLUTION_FAILED,
                           "There are broken dependecies on your system. "
                           "Please use an advanced package manage e.g. "
                           "Synaptic or aptitude to resolve this situation.")
            self.Finished(EXIT_FAILED)
            self.Exit()
            return
        self._last_cache_refresh = time.time()

    def _recover(self, prange=(95,100)):
        """
        Try to recover from a package manager failure
        """
        self.StatusChanged(STATUS_CLEANUP)
        self.NoPercentageUpdates()
        try:
            d = PackageKitDpkgInstallProgress(self)
            d.startUpdate()
            d.recover()
            d.finishUpdate()
        except:
            pass
        self._open_cache(prange)

    def _commit_changes(self, fetch_range=(5,50), install_range=(50,90)):
        """
        Commit changes to the cache and handle errors
        """
        try:
            self._cache.commit(PackageKitFetchProgress(self, fetch_range), 
                               PackageKitInstallProgress(self, install_range))
        except apt.cache.FetchFailedException:
            self._open_cache(prange=(95,100))
            self.ErrorCode(ERROR_PACKAGE_DOWNLOAD_FAILED, "Download failed")
            self.Finished(EXIT_FAILED)
        except apt.cache.FetchCancelledException:
            self._open_cache(prange=(95,100))
            self.Finished(EXIT_CANCELLED)
            self._canceled.clear()
        except InstallTimeOutPKError, e:
            self._recover()
            self._open_cache(prange=(95,100))
            #FIXME: should provide more information
            self.ErrorCode(ERROR_UNKNOWN,
                           "Transaction was cancelled since the installation "
                           "of a package hung.\n"
                           "This can be caused by maintainer scripts which "
                           "require input on the terminal:\n%s" % e.message)
            self.Finished(EXIT_KILLED)
        except PackageManagerFailedPKError, e:
            self._recover()
            self.ErrorCode(ERROR_UNKNOWN, "%s\n%s" % (e.message, e.output))
            self.Finished(EXIT_FAILED)
        else:
            return True
        return False

    def _check_init(self, prange=(0,10), progress=True):
        '''
        Check if the backend was initialized well and try to recover from
        a broken setup
        '''
        pklog.debug("Checking apt cache and xapian database")
        pkg_cache = os.path.join(apt_pkg.Config["Dir"],
                                 apt_pkg.Config["Dir::Cache"],
                                 apt_pkg.Config["Dir::Cache::pkgcache"])
        src_cache = os.path.join(apt_pkg.Config["Dir"],
                                 apt_pkg.Config["Dir::Cache"],
                                 apt_pkg.Config["Dir::Cache::srcpkgcache"])
        # Check if the cache instance is of the coorect class type, contains
        # any broken packages and if the dpkg status or apt cache files have 
        # been changed since the last refresh
        if not isinstance(self._cache, apt.cache.Cache) or \
           (self._cache._depcache.BrokenCount > 0) or \
           (os.stat(apt_pkg.Config["Dir::State::status"])[stat.ST_MTIME] > \
            self._last_cache_refresh) or \
           (os.stat(pkg_cache)[stat.ST_MTIME] > self._last_cache_refresh) or \
           (os.stat(src_cache)[stat.ST_MTIME] > self._last_cache_refresh):
            pklog.debug("Reloading the cache is required")
            self._open_cache(prange, progress)
        else:
            self._cache.clear()

    def _check_canceled(self):
        '''
        Check if the current transaction was canceled. If so send the
        corresponding error message and return True
        '''
        if self._canceled.isSet():
            self.Finished(EXIT_CANCELLED)
            self._canceled.clear()
            return True
        return False
 
    def get_id_from_package(self, pkg, force_candidate=False):
        '''
        Return the packagekit id of package. By default this will be the 
        installed version for installed packages and the candidate version
        for not installed packages.

        The force_candidate option will also report the id of the candidate
        version for installed packages.
        '''
        origin = ""
        cand_origin = pkg.candidateOrigin
        if not pkg.isInstalled or force_candidate:
            version = pkg.candidateVersion
            if cand_origin:
                origin = cand_origin[0].label
        else:
            version = pkg.installedVersion
            if cand_origin and cand_origin[0].site != "" and \
               pkg.installedVersion == pkg.candidateVersion:
                origin = cand_origin[0].label
        id = self._get_package_id(pkg.name, version, pkg.architecture, origin)
        return id

    def _emit_package(self, pkg, info=None, force_candidate=False):
        '''
        Send the Package signal for a given apt package
        '''
        id = self.get_id_from_package(pkg, force_candidate)
        section = pkg.section.split("/")[-1]
        if info == None:
            if pkg.isInstalled:
                if section == "metapackages":
                    info = INFO_COLLECTION_INSTALLED
                else:
                    info = INFO_INSTALLED
            else:
                if section == "metapackages":
                    info = INFO_COLLECTION_AVAILABLE
                else:
                    info = INFO_AVAILABLE
        summary = pkg.summary
        self.Package(info, id, summary)

    def _emit_visible_package(self, filters, pkg, info=None):
        """
        Filter and emit a package
        """
        if self._is_package_visible(pkg, filters):
            self._emit_package(pkg, info)

    def _emit_visible_packages(self, filters, pkgs, info=None):
        """
        Filter and emit packages
        """
        for p in pkgs:
            if self._is_package_visible(p, filters):
                self._emit_package(p, info)

    def _emit_visible_packages_by_name(self, filters, pkgs, info=None):
        """
        Find the packages with the given namens. Afterwards filter and emit
        them
        """
        for name in pkgs:
            if self._cache.has_key(name) and \
               self._is_package_visible(self._cache[name], filters):
                self._emit_package(self._cache[name], info)


    def _is_package_visible(self, pkg, filters):
        '''
        Return True if the package should be shown in the user interface
        '''
        if filters == FILTER_NONE:
            return True
        for filter in filters.split(";"):
            if (filter == FILTER_INSTALLED and not pkg.isInstalled) or \
               (filter == FILTER_NOT_INSTALLED and pkg.isInstalled) or \
               (filter == FILTER_SUPPORTED and not \
                self._is_package_supported(pkg)) or \
               (filter == FILTER_NOT_SUPPORTED and \
                self._is_package_supported(pkg)) or \
               (filter == FILTER_FREE and not self._is_package_free(pkg)) or \
               (filter == FILTER_NOT_FREE and \
                not self._is_package_not_free(pkg)) or \
               (filter == FILTER_GUI and not self._has_package_gui(pkg)) or \
               (filter == FILTER_NOT_GUI and self._has_package_gui(pkg)) or \
               (filter == FILTER_COLLECTIONS and not \
                self._is_package_collection(pkg)) or \
               (filter == FILTER_NOT_COLLECTIONS and \
                self._is_package_collection(pkg)) or\
                (filter == FILTER_DEVELOPMENT and not \
                self._is_package_devel(pkg)) or \
               (filter == FILTER_NOT_DEVELOPMENT and \
                self._is_package_devel(pkg)):
                return False
        return True

    def _is_package_not_free(self, pkg):
        """
        Return True if we can be sure that the package's license isn't any 
        free one
        """
        candidate = pkg.candidateOrigin
        return candidate != None and \
               ((candidate[0].origin == "Ubuntu" and \
                 candidate[0].component in ["multiverse", "restricted"]) or \
                (candidate[0].origin == "Debian" and \
                 candidate[0].component in ["contrib", "non-free"])) and \
               candidate[0].trusted == True

    def _is_package_collection(self, pkg):
        """
        Return True if the package is a metapackge
        """
        section = pkg.section.split("/")[-1]
        return section == "metapackages"

    def _is_package_free(self, pkg):
        """
        Return True if we can be sure that the package has got a free license
        """
        candidate = pkg.candidateOrigin
        return candidate != None and \
               ((candidate[0].origin == "Ubuntu" and \
                 candidate[0].component in ["main", "universe"]) or \
                (candidate[0].origin == "Debian" and \
                 candidate[0].component == "main")) and\
               candidate[0].trusted == True

    def _has_package_gui(self, pkg):
        #FIXME: should go to a modified Package class
        #FIXME: take application data into account. perhaps checking for
        #       property in the xapian database
        return pkg.section.split('/')[-1].lower() in ['x11', 'gnome', 'kde']

    def _is_package_devel(self, pkg):
        #FIXME: should go to a modified Package class
        return pkg.name.endswith("-dev") or pkg.name.endswith("-dbg") or \
               pkg.section.split('/')[-1].lower() in ['devel', 'libdevel']

    def _is_package_supported(self, pkg):
        candidate = pkg.candidateOrigin[0]
        return candidate != None and \
               candidate[0].origin == "Ubuntu" and \
               candidate[0].component in ["main", "restricted"] and \
               candidate[0].trusted == True

    def _find_package_by_id(self, id):
        '''
        Return a package matching to the given package id
        '''
        # FIXME: Perform more checks
        name, version, arch, data = self.get_package_from_id(id)
        if self._cache.has_key(name):
            return self._cache[name]
        else:
            return None

    def _get_installed_files(self, pkg):
        """
        Return the list of unicode names of the files which have
        been installed by the package

        This method should be obsolete by the apt.package.Package.installedFiles
        attribute as soon as the consolidate branch of python-apt gets merged
        """
        path = os.path.join(apt_pkg.Config["Dir"],
                            "var/lib/dpkg/info/%s.list" % pkg.name)
        try:
            list = open(path)
            files = list.read().decode().split("\n")
            list.close()
        except:
            return []
        return files

    def _get_changelog(self, pkg, uri=None, cancel_lock=None):
        """
        Download the changelog of the package and return it as unicode 
        string

        This method is already part of the consolidate branch of python-apt

        uri: Is the uri to the changelog file. The following named variables
             will be substituted: src_section, prefix, src_pkg and src_ver
             For example the Ubuntu changelog:
             uri = "http://changelogs.ubuntu.com/changelogs/pool" \\
                   "/%(src_section)s/%(prefix)s/%(src_pkg)s" \\
                   "/%(src_pkg)s_%(src_ver)s/changelog"
        cancel_lock: If this threading.Lock() is set, the download will be
                     canceled
        """
        if uri == None:
            if pkg.candidateOrigin[0].origin == "Debian":
                uri = "http://packages.debian.org/changelogs/pool" \
                      "/%(src_section)s/%(prefix)s/%(src_pkg)s" \
                      "/%(src_pkg)s_%(src_ver)s/changelog"
            elif pkg.candidateOrigin[0].origin == "Ubuntu":
                uri = "http://changelogs.ubuntu.com/changelogs/pool" \
                      "/%(src_section)s/%(prefix)s/%(src_pkg)s" \
                      "/%(src_pkg)s_%(src_ver)s/changelog"
            else:
                return "The list of changes is not available"

        # get the src package name
        src_pkg = pkg.sourcePackageName

        # assume "main" section 
        src_section = "main"
        # use the section of the candidate as a starting point
        section = pkg._depcache.GetCandidateVer(pkg._pkg).Section

        # get the source version, start with the binaries version
        bin_ver = pkg.candidateVersion
        src_ver = pkg.candidateVersion
        #print "bin: %s" % binver
        try:
            # try to get the source version of the pkg, this differs
            # for some (e.g. libnspr4 on ubuntu)
            # this feature only works if the correct deb-src are in the 
            # sources.list
            # otherwise we fall back to the binary version number
            src_records = apt_pkg.GetPkgSrcRecords()
            src_rec = src_records.Lookup(src_pkg)
            if src_rec:
                src_ver = src_records.Version
                #if apt_pkg.VersionCompare(binver, srcver) > 0:
                #    srcver = binver
                if not src_ver:
                    src_ver = bin_ver
                #print "srcver: %s" % src_ver
                section = src_records.Section
                #print "srcsect: %s" % section
            else:
                # fail into the error handler
                raise SystemError
        except SystemError, e:
            src_ver = bin_ver

        l = section.split("/")
        if len(l) > 1:
            src_section = l[0]

        # lib is handled special
        prefix = src_pkg[0]
        if src_pkg.startswith("lib"):
            prefix = "lib" + src_pkg[3]

        # stip epoch
        l = src_ver.split(":")
        if len(l) > 1:
            src_ver = "".join(l[1:])

        uri = uri % {"src_section" : src_section,
                     "prefix" : prefix,
                     "src_pkg" : src_pkg,
                     "src_ver" : src_ver}
        try:
            # Check if the download was canceled
            if cancel_lock and cancel_lock.isSet(): return ""
            changelog_file = urllib2.urlopen(uri)
            # do only get the lines that are new
            changelog = ""
            regexp = "^%s \((.*)\)(.*)$" % (re.escape(src_pkg))

            i=0
            while True:
                # Check if the download was canceled
                if cancel_lock and cancel_lock.isSet(): return ""
                # Read changelog line by line
                line_raw = changelog_file.readline()
                if line_raw == "":
                    break
                # The changelog is encoded in utf-8, but since there isn't any
                # http header, urllib2 seems to treat it as ascii
                line = line_raw.decode("utf-8")

                #print line.encode('utf-8')
                match = re.match(regexp, line)
                if match:
                    # strip epoch from installed version
                    # and from changelog too
                    installed = pkg.installedVersion
                    if installed and ":" in installed:
                        installed = installed.split(":",1)[1]
                    changelog_ver = match.group(1)
                    if changelog_ver and ":" in changelog_ver:
                        changelog_ver = changelog_ver.split(":", 1)[1]
                    if installed and \
                        apt_pkg.VersionCompare(changelog_ver, installed) <= 0:
                        break
                # EOF (shouldn't really happen)
                changelog += line

            # Print an error if we failed to extract a changelog
            if len(changelog) == 0:
                changelog = "The list of changes is not available"
        except urllib2.HTTPError,e:
            return "The list of changes is not available yet.\n\n" \
                    "Please use http://launchpad.net/ubuntu/+source/%s/%s/" \
                    "+changelog\n" \
                    "until the changes become available or try again " \
                    "later." % (src_pkg, src_ver)
        except IOError, httplib.BadStatusLine:
            return "Failed to download the list of changes.\nPlease " \
                   "check your Internet connection."
        return changelog

    def _get_package_group(self, pkg):
        """
        Return the packagekit group corresponding to the package's section
        """
        section = pkg.section.split("/")[-1]
        if SECTION_GROUP_MAP.has_key(section):
            return SECTION_GROUP_MAP[section]
        else:
            pklog.debug("Unkown package section %s of %s" % (pkg.section,
                                                             pkg.name))
            return GROUP_UNKNOWN

    def _get_package_description(self, pkg):
        """
        Return the formated long description according to the Debian policy
        (Chapter 5.6.13).
        See http://www.debian.org/doc/debian-policy/ch-controlfields.html
        for more information.
        """
        if not pkg._lookupRecord():
            return ""
        # get the translated description
        ver = pkg._depcache.GetCandidateVer(pkg._pkg)
        desc_iter = ver.TranslatedDescription
        pkg._records.Lookup(desc_iter.FileList.pop(0))
        desc = ""
        try:
            s = unicode(pkg._records.LongDesc,"utf-8")
        except UnicodeDecodeError,e:
            s = "Invalid unicode in description for '%s' (%s)" % (pkg.name, e)
        lines = string.split(s, "\n")
        for i in range(len(lines)):
            # Skip the first line, since its a duplication of the summary
            if i == 0: continue
            raw_line = lines[i]
            if raw_line.strip() == ".":
                # The line is just line break
                if not desc.endswith("\n"):
                    desc += "\n"
                continue
            elif raw_line.startswith("  "):
                # The line should be displayed verbatim without word wrapping
                if not desc.endswith("\n"):
                    line = "\n%s\n" % raw_line[2:]
                else:
                    line = "%s\n" % raw_line[2:]
            elif raw_line.startswith(" "):
                # The line is part of a paragraph.
                if desc.endswith("\n") or desc == "":
                    # Skip the leading white space
                    line = raw_line[1:]
                else:
                    line = raw_line
            else:
                line = raw_line
                pklog.debug("invalid line %s in description for %s:\n%s" % \
                            (i, pkg.name, pkg.rawDescription))
            # Use dots for lists
            line = re.sub(r"^(\s*)(\*|0|o|-) ", ur"\1\u2022 ", line, 1)
            # Add current line to the description
            desc += line
        return desc


def sigquit(signum, frame):
    pklog.error("Was killed")
    sys.exit(1)

def debug_exception(type, value, tb):
    """
    Provides an interactive debugging session on unhandled exceptions
    See http://aspn.activestate.com/ASPN/Cookbook/Python/Recipe/65287
    """
    if hasattr(sys, 'ps1') or not sys.stderr.isatty() or \
       not sys.stdin.isatty() or not sys.stdout.isatty() or type==SyntaxError:
        # Calls the default handler in interactive mode, if output is·
        # redirected or on syntax errors
        sys.__excepthook__(type, value, tb)
    else:
        import traceback, pdb
        traceback.print_exception(type, value, tb)
        print
        pdb.pm()

def takeover():
    """
    Exit the currently running backend
    """
    PACKAGEKIT_DBUS_SERVICE = 'org.freedesktop.PackageKitAptBackend'
    PACKAGEKIT_DBUS_INTERFACE = 'org.freedesktop.PackageKitBackend'
    PACKAGEKIT_DBUS_PATH = '/org/freedesktop/PackageKitBackend'
    try:
        bus = dbus.SystemBus()
    except dbus.DBusException, e:
        pklog.critical("Unable to connect to dbus: %s" % e)
        sys.exit(1)
    proxy = bus.get_object(PACKAGEKIT_DBUS_SERVICE, PACKAGEKIT_DBUS_PATH)
    iface = dbus.Interface(proxy, PACKAGEKIT_DBUS_INTERFACE)
    try:
        iface.Exit()
    except dbus.DBusException:
        pass

def run():
    """
    Start the apt backend
    """
    loop = dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus(mainloop=loop)
    bus_name = dbus.service.BusName(PACKAGEKIT_DBUS_SERVICE, bus=bus)
    manager = PackageKitAptBackend(bus_name, PACKAGEKIT_DBUS_PATH)
    manager.run()

def main():
    parser = optparse.OptionParser(description="APT backend for PackageKit")
    parser.add_option("-t", "--takeover",
                      action="store_true", dest="takeover",
                      help="Exit the currently running backend "
                           "(Only needed by developers)")
    parser.add_option("-r", "--root",
                      action="store", type="string", dest="root",
                      help="Use the given directory as the system root "
                           "(Only needed by developers)")
    parser.add_option("-p", "--profile",
                      action="store", type="string", dest="profile",
                      help="Store profiling stats in the given file "
                           "(Only needed by developers)")
    parser.add_option("-d", "--debug",
                      action="store_true", dest="debug",
                      help="Show a lot of additional information and drop to "
                           "a debugging console on unhandled exceptions "
                           "(Only needed by developers)")
    (options, args) = parser.parse_args()
    if options.debug:
        pklog.setLevel(logging.DEBUG)
        sys.excepthook = debug_exception

    if options.root:
        config = apt_pkg.Config
        config.Set("Dir", options.root)
        config.Set("Dir::State::status",
                   os.path.join(options.root, "/var/lib/dpkg/status"))

    if options.takeover:
        takeover()

    if options.profile:
        import hotshot
        prof = hotshot.Profile(options.profile)
        prof.runcall(run)
        prof.close()
    else:
        run()

if __name__ == '__main__':
    main()

# vim: ts=4 et sts=4
