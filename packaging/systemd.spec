%if 0%{?tizen_profile_mobile}

%define _sbindir /sbin
Name:       systemd
Summary:    System and Session Manager
Version:    43
Release:    2
Group:      System/System Control
License:    GPL-2.0
URL:        http://www.freedesktop.org/wiki/Software/systemd
Source0:    http://www.freedesktop.org/software/systemd/%{name}-%{version}.tar.xz
Source1:    pamconsole-tmp.conf
Source1001: systemd.manifest
Source1002: max_user_inotify.conf
Patch1:     0002-systemd-fsck-disable-l-until-linux.patch
Patch2:     add-tmp.mount-as-tmpfs.patch
Patch3:     tizen-login-location.patch
Patch4:     tizen-service-file-workaround.patch
Patch5:     journal-make-sure-to-refresh-window-position-and-poi.patch
Patch6:     tizen-preserve-hostname.patch
Patch7:     util-never-follow-symlinks-in-rm_rf_children.patch
Patch8:     util-introduce-memdup.patch
Patch9:     main-allow-system-wide-limits-for-services.patch
Patch10:    enable-core-dumps-globally.patch
Patch11:    SMACK-Add-configuration-options.-v3.patch
Patch12:    reboot_syscall_param.patch
Patch13:    default_oom_score.patch
Patch14:    fix-syscall-NR_fanotify_mark-on-arm.patch
Patch15:    reboot-delay.patch

BuildRequires:  pkgconfig(dbus-1) >= 1.4.0
BuildRequires:  pkgconfig(dbus-glib-1)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(libudev) >= 174
BuildRequires:  libcap-devel
BuildRequires:  gperf
BuildRequires:  libxslt
BuildRequires:  pam-devel
BuildRequires:  intltool >= 0.40.0
BuildRequires:  libacl-devel
BuildRequires:  fdupes
BuildRequires:  pkgconfig(libkmod)
Requires(post): %{_sbindir}/ldconfig
Requires(postun): %{_sbindir}/ldconfig

%description
system and session manager for Linux, compatible with SysV and
LSB init scripts. systemd provides aggressive parallelization
capabilities, uses socket and D-Bus activation for starting
services, offers on-demand starting of daemons, keeps track of
processes using Linux cgroups, supports snapshotting and restoring
of the system state, maintains mount and automount points and
implements an elaborate transactional dependency-based service
control logic. It can work as a drop-in replacement for sysvinit.

# tools package dislabled because of pycairo package dependency
# %package tools
# Summary:    Analyze systemd startup timing
# Group:      Development/Tools
# Requires:   pycairo
# Requires:   python-xml
# Requires:   %{name} = %{version}-%{release}

# %description tools
# This package installs the systemd-analyze tool, which allows one to
# inspect and graph service startup timing in table or graph format.

%package devel
Summary:    Development tools for systemd
Group:      Development/Libraries
Requires:   %{name} = %{?epoch:%{epoch}:}%{version}-%{release}
Requires(post): %{_sbindir}/ldconfig
Requires(postun): %{_sbindir}/ldconfig

%description devel
This package includes the libraries and header files you will need
to compile applications for systemd.

%package console-ttyS0
Summary:    Systemd console ttyS0
Group:      System/System Control
Requires:   %{name}

%description console-ttyS0
This package will setup a serial getty for ttyS0 is desired.


%package console-ttyS1
Summary:    Systemd console ttyS1
Group:      System/System Control
Requires:   %{name}

%description console-ttyS1
This package will setup a serial getty for ttyS1 is desired.


%package console-tty01
Summary:    Systemd console tty01
Group:      System/System Control
Requires:   %{name}

%description console-tty01
This package will setup a serial getty for tty01 is desired.


%package console-ttyO2
Summary:    Systemd console ttyO2
Group:      System/System Control
Requires:   %{name}

%description console-ttyO2
This package will setup a serial getty for ttyO2 is desired.

%package console-ttyMFD2
Summary:    Systemd console ttyMFD2
Group:      System/System Control
Requires:   %{name}

%description console-ttyMFD2
This package will setup a serial getty for ttyMFD2 is desired.

%package console-ttySAC2
Summary:    Systemd console ttySAC2
Group:      System/System Control
Requires:   %{name}

%description console-ttySAC2
This package will setup a serial getty for ttySAC2 is desired.


%package docs
Summary:   System and session manager man pages
Group:     Development/Libraries
Requires:  %{name} = %{?epoch:%{epoch}:}%{version}-%{release}

%description docs
This package includes the man pages for systemd.


# %package sysv-docs
# Summary:   System and session manager man pages - SysV links
# Group:     Development/Libraries
# Requires:  %{name} = %{version}-%{release}

# %description sysv-docs
# This package provides the manual pages needed for systemd
# to replace sysvinit.

# %package sysv
# Summary:   System and session manager - SysV links
# Group:     System/Startup Services
# Requires:  %{name} = %{version}-%{release}
# Obsoletes: sysvinit < 3.0
# Provides:  sysvinit = 3.0


 #%description sysv
# Systemd is a replacement for sysvinit.  It is dependency-based and
# able to read the LSB init script headers in addition to parsing rcN.d
# links as hints.

# It also provides process supervision using cgroups and the ability to
# not only depend on other init script being started, but also
# availability of a given mount point or dbus service.

# This package provides the links needed for systemd
# to replace sysvinit.


%prep
%setup -q -n %{name}-%{version}
cd ./systemd_43
%patch1 -p1
%patch2 -p1
%patch3 -p1
%patch4 -p1
%patch5 -p1
%patch6 -p1
%patch7 -p1
%patch8 -p1
%patch9 -p1
%patch10 -p1
%patch11 -p1
%patch12 -p1
%patch13 -p1
%patch14 -p1
%patch15 -p1

%build
cd ./systemd_43
cp %{SOURCE1001} ../
%autogen
%configure --disable-static \
    --with-rootdir="" \
    --with-distro=meego \
    --disable-gtk \
    --disable-selinux \
    --disable-tcpwrap \
    --disable-coredump \
    --enable-split-usr \
    --disable-manpages \
    --with-pamlibdir="/%{_libdir}/security" \
    --with-udevrulesdir="%{_libdir}/udev/rules.d"

make %{?_smp_mflags}

%install
cd ./systemd_43
%make_install

# Create SysV compatibility symlinks. systemctl/systemd are smart
# enough to detect in which way they are called.
# install -d %{buildroot}%{_sbindir}/
# ln -s ..%{_libdir}/systemd/systemd %{buildroot}%{_sbindir}/init
# ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/halt
# ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/poweroff
# ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/reboot
# ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/runlevel
# ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/shutdown
# ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/telinit

# /usr compat - delete when no longer needed
install -d %{buildroot}/bin/
ln -s ..%{_bindir}/systemctl %{buildroot}/bin/systemctl

mkdir %{buildroot}/run

# Make sure these directories are properly owned
mkdir -p %{buildroot}%{_libdir}/systemd/system/basic.target.wants
mkdir -p %{buildroot}%{_libdir}/systemd/system/dbus.target.wants

# enable readahead by default
ln -s ../systemd-readahead-collect.service %{buildroot}%{_libdir}/systemd/system/sysinit.target.wants/systemd-readahead-collect.service
ln -s ../systemd-readahead-replay.service %{buildroot}%{_libdir}/systemd/system/sysinit.target.wants/systemd-readahead-replay.service

# Don't ship documentation in the wrong place
rm %{buildroot}/%{_docdir}/systemd/*

mkdir -p %{buildroot}/etc/systemd/system/basic.target.wants
mkdir -p %{buildroot}/etc/systemd/system/getty.target.wants
mkdir -p %{buildroot}%{_libdir}/systemd/system/getty.target.wants

# Modify inotify max_user_instances
mkdir -p %{buildroot}/%{_libdir}/sysctl.d
cp %{SOURCE1002} %{buildroot}%{_libdir}/sysctl.d/

#console-ttyMFD2
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyMFD2.service

#console-ttySAC2
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/getty.target.wants/serial-getty@ttySAC2.service

#console-ttyS0
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyS0.service

#console-ttyS1
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyS1.service

#console-tty01
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/getty.target.wants/serial-getty@tty01.service

#console-ttyO2
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyO2.service

#license
mkdir -p %{buildroot}/usr/share/license
cp LICENSE %{buildroot}/usr/share/license/%{name}
cp LICENSE %{buildroot}/usr/share/license/%{name}-console-ttySAC2
cp LICENSE %{buildroot}/usr/share/license/%{name}-console-ttyMFD2
cp LICENSE %{buildroot}/usr/share/license/%{name}-console-ttyS0
cp LICENSE %{buildroot}/usr/share/license/%{name}-console-ttyS1
cp LICENSE %{buildroot}/usr/share/license/%{name}-console-tty01
cp LICENSE %{buildroot}/usr/share/license/%{name}-console-ttyO2
cp LICENSE %{buildroot}/usr/share/license/%{name}-devel
cp LICENSE %{buildroot}/usr/share/license/%{name}


%post
rm -f /etc/mtab
ln -sf /proc/self/mounts /etc/mtab

/usr/bin/systemd-machine-id-setup >/dev/null 2>&1 || :

%{_sbindir}/ldconfig

%postun -p %{_sbindir}/ldconfig
%post devel -p %{_sbindir}/ldconfig
%postun devel -p %{_sbindir}/ldconfig


%files
%defattr(-,root,root,-)
%manifest systemd.manifest
/bin/systemctl
%{_bindir}/*
# systemd-analyze is excluded for removing dependency with python-base
%exclude %{_bindir}/systemd-analyze
/run
%config %{_sysconfdir}/dbus-1/system.d/org.freedesktop.systemd1.conf
%config %{_sysconfdir}/dbus-1/system.d/org.freedesktop.hostname1.conf
%config %{_sysconfdir}/dbus-1/system.d/org.freedesktop.locale1.conf
%config %{_sysconfdir}/dbus-1/system.d/org.freedesktop.login1.conf
%config %{_sysconfdir}/dbus-1/system.d/org.freedesktop.timedate1.conf
%attr(0644,root,root) %{_libdir}/udev/rules.d/70-uaccess.rules
%attr(0644,root,root) %{_libdir}/udev/rules.d/71-seat.rules
%attr(0644,root,root) %{_libdir}/udev/rules.d/73-seat-late.rules
%config %{_sysconfdir}/systemd
%config %{_sysconfdir}/xdg/systemd/user
%config %{_sysconfdir}/bash_completion.d/systemd-bash-completion.sh
%{_prefix}/%{_lib}/tmpfiles.d/*
%{_libdir}/systemd
/%{_libdir}/security/pam_systemd.so
%{_libdir}/udev/rules.d/99-systemd.rules
%{_libdir}/libsystemd-daemon.so.0
%{_libdir}/libsystemd-daemon.so.0.0.1
%{_libdir}/libsystemd-login.so.0
%{_libdir}/libsystemd-login.so.0.2.0
%{_libdir}/libsystemd-id128.so.0
%{_libdir}/libsystemd-id128.so.0.0.2
%{_libdir}/libsystemd-journal.so.0
%{_libdir}/libsystemd-journal.so.0.0.2
%{_libdir}/sysctl.d/*
%{_datadir}/dbus-1/*/org.freedesktop.systemd1.*
%{_defaultdocdir}/systemd
%{_datadir}/polkit-1/actions/org.freedesktop.systemd1.policy
%{_datadir}/polkit-1/actions/org.freedesktop.locale1.policy
%{_datadir}/polkit-1/actions/org.freedesktop.login1.policy
%{_datadir}/polkit-1/actions/org.freedesktop.timedate1.policy
%{_datadir}/polkit-1/actions/org.freedesktop.hostname1.policy
%{_datadir}/dbus-1/system-services/org.freedesktop.hostname1.service
%{_datadir}/dbus-1/system-services/org.freedesktop.locale1.service
%{_datadir}/dbus-1/system-services/org.freedesktop.login1.service
%{_datadir}/dbus-1/system-services/org.freedesktop.timedate1.service
%{_datadir}/dbus-1/interfaces/org.freedesktop.hostname1.xml
%{_datadir}/dbus-1/interfaces/org.freedesktop.locale1.xml
%{_datadir}/dbus-1/interfaces/org.freedesktop.timedate1.xml
%exclude %{_libdir}/systemd/system/getty.target.wants/serial-getty@tty01.service
%exclude %{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyMFD2.service
%exclude %{_libdir}/systemd/system/getty.target.wants/serial-getty@ttySAC2.service
%exclude %{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyO2.service
%exclude %{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyS0.service
%exclude %{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyS1.service
%exclude %{_libdir}/systemd/system/sysinit.target.wants/systemd-vconsole-setup.service
%exclude %{_libdir}/systemd/user/default.target
/usr/share/license/%{name}

%files console-ttySAC2
%defattr(-,root,root,-)
%{_libdir}/systemd/system/getty.target.wants/serial-getty@ttySAC2.service
/usr/share/license/%{name}-console-ttySAC2

%files console-ttyMFD2
%defattr(-,root,root,-)
%{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyMFD2.service
/usr/share/license/%{name}-console-ttyMFD2

%files console-ttyS0
%defattr(-,root,root,-)
%{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyS0.service
/usr/share/license/%{name}-console-ttyS0

%files console-ttyS1
%defattr(-,root,root,-)
%{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyS1.service
/usr/share/license/%{name}-console-ttyS1

%files console-tty01
%defattr(-,root,root,-)
%{_libdir}/systemd/system/getty.target.wants/serial-getty@tty01.service
/usr/share/license/%{name}-console-tty01

%files console-ttyO2
%defattr(-,root,root,-)
%{_libdir}/systemd/system/getty.target.wants/serial-getty@ttyO2.service
/usr/share/license/%{name}-console-ttyO2


# tools package dislabled because of pycairo package dependency
# %files tools
# %defattr(-,root,root,-)
# %manifest systemd.manifest
# %{_bindir}/systemd-analyze

%files devel
%defattr(-,root,root,-)
%manifest systemd.manifest
%{_datadir}/pkgconfig/systemd.pc
%{_includedir}/systemd/sd-daemon.h
%{_includedir}/systemd/sd-login.h
%{_includedir}/systemd/sd-id128.h
%{_includedir}/systemd/sd-journal.h
%{_includedir}/systemd/sd-messages.h
%{_libdir}/libsystemd-daemon.so
%{_libdir}/libsystemd-login.so
%{_libdir}/libsystemd-journal.so
%{_libdir}/libsystemd-id128.so
%{_libdir}/pkgconfig/libsystemd-daemon.pc
%{_libdir}/pkgconfig/libsystemd-login.pc
%{_libdir}/pkgconfig/libsystemd-id128.pc
%{_libdir}/pkgconfig/libsystemd-journal.pc
%{_datadir}/systemd/kbd-model-map
/usr/share/license/%{name}-devel

# %files sysv
# %defattr(-,root,root,-)
# %doc LICENSE
# %{_sbindir}/halt
# %{_sbindir}/init
# %{_sbindir}/poweroff
# %{_sbindir}/reboot
# %{_sbindir}/runlevel
# %{_sbindir}/shutdown
# %{_sbindir}/telinit

%else
%global _hardened_build 1

# We ship a .pc file but don't want to have a dep on pkg-config. We
# strip the automatically generated dep here and instead co-own the
# directory.
%global __requires_exclude pkg-config

Name:           systemd
Url:            http://www.freedesktop.org/wiki/Software/systemd
Version:        208
Release:        1
VCS:            external/systemd#v206-664-g3646f01d3f41cbef9200abe7ffe97a137a1f520d
License:        LGPLv2+ and MIT and GPLv2+
Summary:        A System and Service Manager
Source0:        http://www.freedesktop.org/software/systemd/%{name}-%{version}.tar.xz
Source1:        apply-conf.sh
Source11:       tizen-system.conf
Source12:       tizen-journald.conf
Source13:       tizen-bootchart.conf
Source102:      tizen-boot.target
Source103:      tizen-system.target
Source104:      tizen-runtime.target
Source1001:     systemd.manifest

# The base number of patch for systemd is 101.

# The base number of patch for udev is 201.

%if "%{_repository}" == "wearable"
BuildRequires:  glib2-devel
BuildRequires:  kmod-devel >= 5
BuildRequires:  hwdata

BuildRequires:  libacl-devel
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(dbus-1) >= 1.4.0
BuildRequires:  libcap-devel
BuildRequires:  libblkid-devel >= 2.20
BuildRequires:  pkgconfig(libkmod) >= 5
BuildRequires:  pam-devel
BuildRequires:  dbus-devel
BuildRequires:  libacl-devel
BuildRequires:  glib2-devel
BuildRequires:  libblkid-devel
BuildRequires:  xz-devel
BuildRequires:  kmod-devel
BuildRequires:  libgcrypt-devel
BuildRequires:  libxslt
BuildRequires:  intltool >= 0.40.0
BuildRequires:  gperf

BuildRequires:  gawk
BuildRequires:  automake
BuildRequires:  autoconf
BuildRequires:  libtool
BuildRequires:  smack-devel
Requires:       dbus
Requires:       %{name}-libs = %{version}-%{release}

Provides:       /bin/systemctl
Provides:       /sbin/shutdown
Provides:       syslog
Provides:       systemd-units = %{version}-%{release}
Provides:       udev = %{version}
Obsoletes:      udev < 183
Obsoletes:      system-setup-keyboard < 0.9
Provides:       system-setup-keyboard = 0.9
Obsoletes:      nss-myhostname < 0.4
Provides:       nss-myhostname = 0.4
Obsoletes:      systemd < 204-10
Obsoletes:      systemd-analyze < 198
Provides:       systemd-analyze = 198
%else
BuildRequires:  pkgconfig(dbus-1) >= 1.4.0
BuildRequires:  pkgconfig(dbus-glib-1)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(libudev) >= 174
BuildRequires:  libcap-devel
BuildRequires:  gperf
BuildRequires:  libxslt
BuildRequires:  pam-devel
BuildRequires:  intltool >= 0.40.0
BuildRequires:  libacl-devel
BuildRequires:  fdupes
BuildRequires:  pkgconfig(libkmod)
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig
%endif

%description
systemd is a system and service manager for Linux, compatible with
SysV and LSB init scripts. systemd provides aggressive parallelization
capabilities, uses socket and D-Bus activation for starting services,
offers on-demand starting of daemons, keeps track of processes using
Linux cgroups, supports snapshotting and restoring of the system
state, maintains mount and automount points and implements an
elaborate transactional dependency-based service control logic. It can
work as a drop-in replacement for sysvinit.

%package libs
Summary:        systemd libraries
License:        LGPLv2+ and MIT
Provides:       libudev
Obsoletes:      systemd < 185-4
Conflicts:      systemd < 185-4

%description libs
Libraries for systemd and udev, as well as the systemd PAM module.

%package devel
Summary:        Development headers for systemd
License:        LGPLv2+ and MIT
Requires:       %{name} = %{version}-%{release}
Provides:       pkgconfig(libudev)
Provides:       libudev-devel = %{version}
Obsoletes:      libudev-devel < 183

%description devel
Development headers and auxiliary files for developing applications for systemd.

%package -n libgudev1
Summary:        Libraries for adding libudev support to applications that use glib
License:        LGPLv2+
Requires:       %{name} = %{version}-%{release}

%description -n libgudev1
This package contains the libraries that make it easier to use libudev
functionality from applications that use glib.

%package -n libgudev1-devel
Summary:        Header files for adding libudev support to applications that use glib
Requires:       libgudev1 = %{version}-%{release}
License:        LGPLv2+

%description -n libgudev1-devel
This package contains the header and pkg-config files for developing
glib-based applications using libudev functionality.

%package journal-gateway
Summary:        Gateway for serving journal events over the network using HTTP
Requires:       %{name} = %{version}-%{release}
License:        LGPLv2+
Requires(pre):    /usr/bin/getent
Requires(post):   systemd
Requires(preun):  systemd
Requires(postun): systemd

%description journal-gateway
systemd-journal-gatewayd serves journal events over the network using HTTP.

%prep
%setup -q -n %{name}-%{version}

%build
cd ./systemd_208
cp %{SOURCE1001} ../

export CFLAGS+=" -DCONFIG_TIZEN -DCONFIG_TIZEN_WIP"
autoreconf -fiv
%configure \
    --disable-static \
    --with-sysvinit-path= \
    --with-sysvrcnd-path= \
    --disable-gtk-doc-html \
    --disable-selinux \
    --disable-ima \
    --disable-tcpwrap \
    --enable-split-usr \
    --disable-nls \
    --disable-manpages \
    --disable-efi \
    --with-pamlibdir="/%{_libdir}/security" \
    --with-firmware-path="/%{_libdir}/firmware" \
    --with-zshcompletiondir="" \
    --disable-hostnamed \
    --disable-machined \
    --disable-binfmt \
    --disable-vconsole \
    --disable-quotacheck \
    --disable-timedated \
    --disable-localed \
    --disable-polkit \
    --disable-myhostname \
    --without-python

make %{?_smp_mflags}

%install
cd ./systemd_208
%make_install

rm -f %{buildroot}/usr/bin/kernel-install
rm -rf %{buildroot}/usr/lib/kernel
rm -f %{buildroot}/usr/bin/systemd-detect-virt
find %{buildroot} \( -name '*.a' -o -name '*.la' \) -delete

# udev links
mkdir -p %{buildroot}/%{_sbindir}
mkdir -p %{buildroot}/sbin
ln -sf ../bin/udevadm %{buildroot}%{_sbindir}/udevadm

# Create SysV compatibility symlinks. systemctl/systemd are smart
# enough to detect in which way they are called.
ln -s ../lib/systemd/systemd %{buildroot}%{_sbindir}/init
ln -s /usr/lib/systemd/systemd %{buildroot}/sbin/init
ln -s ../lib/systemd/systemd %{buildroot}%{_bindir}/systemd
ln -s ../bin/systemctl %{buildroot}%{_sbindir}/reboot
ln -s /usr/bin/systemctl %{buildroot}/sbin/reboot
ln -s ../bin/systemctl %{buildroot}%{_sbindir}/halt
ln -s ../bin/systemctl %{buildroot}%{_sbindir}/poweroff
ln -s ../bin/systemctl %{buildroot}%{_sbindir}/shutdown
ln -s ../bin/systemctl %{buildroot}%{_sbindir}/telinit
ln -s ../bin/systemctl %{buildroot}%{_sbindir}/runlevel
ln -s /usr/lib/systemd/systemd-udevd %{buildroot}/%{_sbindir}/udevd
ln -s /lib/firmware %{buildroot}%{_libdir}/firmware

# We create all wants links manually at installation time to make sure
# they are not owned and hence overriden by rpm after the user deleted
# them.
rm -r %{buildroot}%{_sysconfdir}/systemd/system/*.target.wants

# Make sure these directories are properly owned
mkdir -p %{buildroot}%{_prefix}/lib/systemd/system/basic.target.wants
mkdir -p %{buildroot}%{_prefix}/lib/systemd/system/default.target.wants
mkdir -p %{buildroot}%{_prefix}/lib/systemd/system/dbus.target.wants

# Make sure the user generators dir exists too
mkdir -p %{buildroot}%{_prefix}/lib/systemd/system-generators
mkdir -p %{buildroot}%{_prefix}/lib/systemd/user-generators

# Create new-style configuration files so that we can ghost-own them
touch %{buildroot}%{_sysconfdir}/hostname
touch %{buildroot}%{_sysconfdir}/locale.conf
touch %{buildroot}%{_sysconfdir}/machine-id
touch %{buildroot}%{_sysconfdir}/machine-info
touch %{buildroot}%{_sysconfdir}/localtime
mkdir -p %{buildroot}%{_sysconfdir}/X11/xorg.conf.d
touch %{buildroot}%{_sysconfdir}/X11/xorg.conf.d/00-keyboard.conf

# Make sure the shutdown/sleep drop-in dirs exist
mkdir -p %{buildroot}%{_prefix}/lib/systemd/system-shutdown/
mkdir -p %{buildroot}%{_prefix}/lib/systemd/system-sleep/

# Make sure directories in /var exist
mkdir -p %{buildroot}%{_localstatedir}/lib/systemd/coredump
mkdir -p %{buildroot}%{_localstatedir}/lib/systemd/catalog
mkdir -p %{buildroot}%{_localstatedir}/log/journal
touch %{buildroot}%{_localstatedir}/lib/systemd/catalog/database
touch %{buildroot}%{_sysconfdir}/udev/hwdb.bin

# To avoid making life hard for Rawhide-using developers, don't package the
# kernel.core_pattern setting until systemd-coredump is a part of an actual
# systemd release and it's made clear how to get the core dumps out of the
# journal.
rm -f %{buildroot}%{_prefix}/lib/sysctl.d/50-coredump.conf

rm -rf %{buildroot}%{_prefix}/lib/udev/hwdb.d/

# These rules doesn't make much sense on Tizen right now.
rm -f %{buildroot}%{_libdir}/udev/rules.d/42-usb-hid-pm.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/60-persistent-storage-tape.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/60-cdrom_id.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/60-keyboard.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/61-accelerometer.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/64-btrfs.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/70-power-switch.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/75-net-description.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/75-probe_mtd.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/75-tty-description.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/80-net-name-slot.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/95-keyboard-force-release.rules
rm -f %{buildroot}%{_libdir}/udev/rules.d/95-keymap.rules

# This will be done by tizen specific init script.
rm -f %{buildroot}%{_libdir}/systemd/system/local-fs.target.wants/systemd-fsck-root.service

# We will just use systemd just as system, user not yet. Until that that service will be disabled.
rm -f %{buildroot}%{_libdir}/systemd/system/multi-user.target.wants/systemd-logind.service

# Make sure default extra dependencies ignore unit directory exist
mkdir -p %{buildroot}%{_sysconfdir}/systemd/default-extra-dependencies/ignore-units.d

# Tizen customized units are added. To solve the legacy of sysvinit style dependency.
install -m644 %{SOURCE102} %{buildroot}%{_prefix}/lib/systemd/system/
mkdir -p %{buildroot}%{_prefix}/lib/systemd/system/tizen-boot.target.wants
install -m644 %{SOURCE103} %{buildroot}%{_prefix}/lib/systemd/system/
mkdir -p %{buildroot}%{_prefix}/lib/systemd/system/tizen-system.target.wants
install -m644 %{SOURCE104} %{buildroot}%{_prefix}/lib/systemd/system/
mkdir -p %{buildroot}%{_prefix}/lib/systemd/system/tizen-runtime.target.wants

# Apply Tizen configs
chmod +x %{SOURCE1}
%{SOURCE1} %{SOURCE11} %{buildroot}%{_sysconfdir}/systemd/system.conf
%{SOURCE1} %{SOURCE12} %{buildroot}%{_sysconfdir}/systemd/journald.conf
%{SOURCE1} %{SOURCE13} %{buildroot}%{_sysconfdir}/systemd/bootchart.conf

# All of licenses should be manifested in /usr/share/license.
mkdir -p %{buildroot}%{_datadir}/license
cat LICENSE.LGPL2.1 >> %{buildroot}%{_datadir}/license/systemd
cat LICENSE.LGPL2.1 >> %{buildroot}%{_datadir}/license/systemd-libs
cat LICENSE.LGPL2.1 >> %{buildroot}%{_datadir}/license/libgudev1

%remove_docs

%pre
getent group cdrom >/dev/null 2>&1 || groupadd -r -g 11 cdrom >/dev/null 2>&1 || :
getent group tape >/dev/null 2>&1 || groupadd -r -g 33 tape >/dev/null 2>&1 || :
getent group dialout >/dev/null 2>&1 || groupadd -r -g 18 dialout >/dev/null 2>&1 || :
getent group floppy >/dev/null 2>&1 || groupadd -r -g 19 floppy >/dev/null 2>&1 || :
getent group systemd-journal >/dev/null 2>&1 || groupadd -r -g 190 systemd-journal 2>&1 || :

systemctl stop systemd-udevd-control.socket systemd-udevd-kernel.socket systemd-udevd.service >/dev/null 2>&1 || :

%post
systemd-machine-id-setup >/dev/null 2>&1 || :
/usr/lib/systemd/systemd-random-seed save >/dev/null 2>&1 || :
systemctl daemon-reexec >/dev/null 2>&1 || :
systemctl start systemd-udevd.service >/dev/null 2>&1 || :
udevadm hwdb --update >/dev/null 2>&1 || :
journalctl --update-catalog >/dev/null 2>&1 || :

# Stop-gap until rsyslog.rpm does this on its own. (This is supposed
# to fail when the link already exists)
ln -s /usr/lib/systemd/system/rsyslog.service /etc/systemd/system/syslog.service >/dev/null 2>&1 || :

# Masked unnecessary units in tizen.
ln -s /dev/null /etc/systemd/system/systemd-tmpfiles-clean.timer
ln -s /dev/null /etc/systemd/system/systemd-remount-fs.service
ln -s /dev/null /etc/systemd/system/systemd-journal-flush.service
ln -s /dev/null /etc/systemd/system/systemd-update-utmp.service
ln -s /dev/null /etc/systemd/system/kmod-static-nodes.service

# Tizen is not supporting /usr/lib/macros.d yet.
# To avoid this, the macro will be copied to origin macro dir.
if [ -f %{_libdir}/rpm/macros.d/macros.systemd ]; then
        mkdir -p %{_sysconfdir}/rpm
        cp %{_libdir}/rpm/macros.d/macros.systemd %{_sysconfdir}/rpm
fi

%postun
if [ $1 -ge 1 ] ; then
        systemctl daemon-reload > /dev/null 2>&1 || :
        systemctl try-restart systemd-logind.service >/dev/null 2>&1 || :
fi

%post libs -p /sbin/ldconfig
%postun libs -p /sbin/ldconfig

%post -n libgudev1 -p /sbin/ldconfig
%postun -n libgudev1 -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{_datadir}/license/systemd
%dir %{_sysconfdir}/systemd
%dir %{_sysconfdir}/systemd/system
%dir %{_sysconfdir}/systemd/user
%dir %{_sysconfdir}/tmpfiles.d
%dir %{_sysconfdir}/sysctl.d
%dir %{_sysconfdir}/modules-load.d
%dir %{_sysconfdir}/udev
%dir %{_sysconfdir}/udev/rules.d
%dir %{_sysconfdir}/systemd/default-extra-dependencies/ignore-units.d
%dir %{_prefix}/lib/systemd
%dir %{_prefix}/lib/systemd/system-generators
%dir %{_prefix}/lib/systemd/user-generators
%dir %{_prefix}/lib/systemd/system-shutdown
%dir %{_prefix}/lib/systemd/system-sleep
%dir %{_prefix}/lib/systemd/catalog
%dir %{_prefix}/lib/tmpfiles.d
%dir %{_prefix}/lib/sysctl.d
%dir %{_prefix}/lib/modules-load.d
%{_prefix}/lib/firmware
%dir %{_datadir}/systemd
%dir %{_datadir}/pkgconfig
%dir %{_localstatedir}/log/journal
%dir %{_localstatedir}/lib/systemd
%dir %{_localstatedir}/lib/systemd/catalog
%dir %{_localstatedir}/lib/systemd/coredump
%config(noreplace) %{_sysconfdir}/dbus-1/system.d/org.freedesktop.systemd1.conf
%config(noreplace) %{_sysconfdir}/dbus-1/system.d/org.freedesktop.login1.conf
%config(noreplace) %{_sysconfdir}/systemd/system.conf
%config(noreplace) %{_sysconfdir}/systemd/user.conf
%config(noreplace) %{_sysconfdir}/systemd/logind.conf
%config(noreplace) %{_sysconfdir}/systemd/journald.conf
%config(noreplace) %{_sysconfdir}/systemd/bootchart.conf
%config(noreplace) %{_sysconfdir}/udev/udev.conf
%ghost %{_sysconfdir}/udev/hwdb.bin
%{_libdir}/rpm/macros.d/macros.systemd
%{_sysconfdir}/pam.d/systemd-user
%{_sysconfdir}/xdg/systemd
%ghost %config(noreplace) %{_sysconfdir}/hostname
%ghost %config(noreplace) %{_sysconfdir}/localtime
%ghost %config(noreplace) %{_sysconfdir}/locale.conf
%ghost %config(noreplace) %{_sysconfdir}/machine-id
%ghost %config(noreplace) %{_sysconfdir}/machine-info
%ghost %config(noreplace) %{_sysconfdir}/X11/xorg.conf.d/00-keyboard.conf
%ghost %{_localstatedir}/lib/systemd/catalog/database
%{_bindir}/systemd
%{_bindir}/systemctl
%{_bindir}/systemd-run
%{_bindir}/systemd-notify
%{_bindir}/systemd-analyze
%{_bindir}/systemd-ask-password
%{_bindir}/systemd-tty-ask-password-agent
%{_bindir}/systemd-machine-id-setup
%{_bindir}/loginctl
%{_bindir}/journalctl
%{_bindir}/systemd-tmpfiles
%{_bindir}/systemd-nspawn
%{_bindir}/systemd-stdio-bridge
%{_bindir}/systemd-cat
%{_bindir}/systemd-cgls
%{_bindir}/systemd-cgtop
%{_bindir}/systemd-delta
%{_bindir}/systemd-inhibit
%{_bindir}/systemd-coredumpctl
%{_bindir}/udevadm
%{_prefix}/lib/systemd/systemd
%{_prefix}/lib/systemd/system
%{_prefix}/lib/systemd/user
%{_prefix}/lib/systemd/systemd-*
%{_prefix}/lib/udev
%{_prefix}/lib/systemd/system-generators/systemd-getty-generator
%{_prefix}/lib/systemd/system-generators/systemd-fstab-generator
%exclude %{_prefix}/lib/systemd/system-generators/systemd-system-update-generator
%exclude %{_prefix}/lib/systemd/system-generators/systemd-gpt-auto-generator
%{_prefix}/lib/tmpfiles.d/systemd.conf
%{_prefix}/lib/tmpfiles.d/x11.conf
%{_prefix}/lib/tmpfiles.d/tmp.conf
%{_prefix}/lib/sysctl.d/50-default.conf
%{_prefix}/lib/systemd/catalog/systemd.catalog
%{_sbindir}/init
/sbin/init
%{_sbindir}/reboot
/sbin/reboot
%{_sbindir}/halt
%{_sbindir}/poweroff
%{_sbindir}/shutdown
%{_sbindir}/telinit
%{_sbindir}/runlevel
%{_sbindir}/udevd
%{_sbindir}/udevadm
%{_datadir}/dbus-1/services/org.freedesktop.systemd1.service
%{_datadir}/dbus-1/system-services/org.freedesktop.systemd1.service
%{_datadir}/dbus-1/system-services/org.freedesktop.login1.service
%{_datadir}/dbus-1/interfaces/org.freedesktop.systemd1.*.xml
%{_datadir}/pkgconfig/systemd.pc
%{_datadir}/pkgconfig/udev.pc
%{_datadir}/bash-completion/completions/journalctl
%{_datadir}/bash-completion/completions/kernel-install
%{_datadir}/bash-completion/completions/loginctl
%{_datadir}/bash-completion/completions/systemctl
%{_datadir}/bash-completion/completions/systemd-coredumpctl
%{_datadir}/bash-completion/completions/systemd-run
%{_datadir}/bash-completion/completions/udevadm
%{_datadir}/bash-completion/completions/systemd-analyze
%manifest systemd.manifest

%files libs
%defattr(-,root,root,-)
%{_datadir}/license/systemd-libs
%{_libdir}/security/pam_systemd.so
%{_libdir}/libsystemd-daemon.so.*
%{_libdir}/libsystemd-login.so.*
%{_libdir}/libsystemd-journal.so.*
%{_libdir}/libsystemd-id128.so.*
%{_libdir}/libudev.so.*
%manifest systemd.manifest

%files devel
%defattr(-,root,root,-)
%dir %{_includedir}/systemd
%{_libdir}/libsystemd-daemon.so
%{_libdir}/libsystemd-login.so
%{_libdir}/libsystemd-journal.so
%{_libdir}/libsystemd-id128.so
%{_libdir}/libudev.so
%{_includedir}/systemd/sd-daemon.h
%{_includedir}/systemd/sd-login.h
%{_includedir}/systemd/sd-journal.h
%{_includedir}/systemd/sd-id128.h
%{_includedir}/systemd/sd-messages.h
%{_includedir}/systemd/sd-shutdown.h
%{_includedir}/libudev.h
%{_libdir}/pkgconfig/libsystemd-daemon.pc
%{_libdir}/pkgconfig/libsystemd-login.pc
%{_libdir}/pkgconfig/libsystemd-journal.pc
%{_libdir}/pkgconfig/libsystemd-id128.pc
%{_libdir}/pkgconfig/libudev.pc
%manifest systemd.manifest

%files -n libgudev1
%defattr(-,root,root,-)
%{_datadir}/license/libgudev1
%{_libdir}/libgudev-1.0.so.*
%manifest systemd.manifest

%files -n libgudev1-devel
%defattr(-,root,root,-)
%{_libdir}/libgudev-1.0.so
%dir %{_includedir}/gudev-1.0
%dir %{_includedir}/gudev-1.0/gudev
%{_includedir}/gudev-1.0/gudev/*.h
%{_libdir}/pkgconfig/gudev-1.0*
%manifest systemd.manifest
%endif
