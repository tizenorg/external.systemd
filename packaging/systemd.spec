%define _sbindir /sbin
Name:       systemd
Summary:    System and Session Manager
Version:    43
Release:    1
Group:      System/System Control
License:    GPLv2
URL:        http://www.freedesktop.org/wiki/Software/systemd
Source0:    http://www.freedesktop.org/software/systemd/%{name}-%{version}.tar.xz
Source1:    packaging/pamconsole-tmp.conf
Source1001: packaging/systemd.manifest
Patch1:     packaging/0002-systemd-fsck-disable-l-until-linux.patch
Patch2:     packaging/add-tmp.mount-as-tmpfs.patch
Patch3:     packaging/tizen-login-location.patch
Patch4:     packaging/tizen-service-file-workaround.patch

BuildRequires:  pkgconfig(dbus-1) >= 1.4.0
BuildRequires:  pkgconfig(dbus-glib-1)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(libudev) >= 174
BuildRequires:  libcap-devel
BuildRequires:  gperf
BuildRequires:  libxslt
BuildRequires:  docbook-dtds
BuildRequires:  docbook-xsl
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

%package tools
Summary:    Analyze systemd startup timing
Group:      Development/Tools
Requires:   pycairo
Requires:   python-xml
Requires:   %{name} = %{version}-%{release}

%description tools
This package installs the systemd-analyze tool, which allows one to
inspect and graph service startup timing in table or graph format.

%package devel
Summary:    Development tools for systemd
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}
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

%package docs
Summary:   System and session manager man pages
Group:     Development/Libraries
Requires:  %{name} = %{version}-%{release}

%description docs
This package includes the man pages for systemd.


%package sysv-docs
Summary:   System and session manager man pages - SysV links
Group:     Development/Libraries
Requires:  %{name} = %{version}-%{release}

%description sysv-docs
This package provides the manual pages needed for systemd
to replace sysvinit.

%package sysv
Summary:   System and session manager - SysV links
Group:     System/Startup Services
Requires:  %{name} = %{version}-%{release}

%description sysv
Systemd is a replacement for sysvinit.  It is dependency-based and
able to read the LSB init script headers in addition to parsing rcN.d
links as hints.

It also provides process supervision using cgroups and the ability to
not only depend on other init script being started, but also
availability of a given mount point or dbus service.

This package provides the links needed for systemd
to replace sysvinit.


%prep
%setup -q -n %{name}-%{version}
%patch1 -p1
%patch2 -p1
%patch3 -p1
%patch4 -p1

%build
cp %{SOURCE1001} .
%configure --disable-static \
    --with-rootdir="" \
    --with-distro=meego \
    --disable-gtk \
    --disable-selinux \
    --disable-tcpwrap \
    --enable-manpages \
    --with-pamlibdir="/%{_libdir}/security" \
    --with-udevrulesdir="%{_libdir}/udev/rules.d"

make %{?_smp_mflags}

%install
%make_install

# Create SysV compatibility symlinks. systemctl/systemd are smart
# enough to detect in which way they are called.
install -d %{buildroot}%{_sbindir}/
ln -s ..%{_libdir}/systemd/systemd %{buildroot}%{_sbindir}/init
ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/halt
ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/poweroff
ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/reboot
ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/runlevel
ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/shutdown
ln -s ..%{_bindir}/systemctl %{buildroot}%{_sbindir}/telinit

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

#console-ttyMFD2
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyMFD2.service

#console-ttyS0
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyS0.service

#console-ttyS1
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyS1.service

#console-tty01
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/sysinit.target.wants/serial-getty@tty01.service

#console-ttyO2
ln -s ../serial-getty@.service %{buildroot}%{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyO2.service

%fdupes  %{buildroot}/%{_datadir}/man/

%post
if [ "`readlink /etc/mtab`" != "/proc/self/mounts" ]; then
	rm -f /etc/mtab
	ln -s /proc/self/mounts /etc/mtab
fi

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
/run
%config %{_libdir}/sysctl.d/coredump.conf
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
%{_mandir}/man1/*
%{_mandir}/man3/*
%{_mandir}/man5/*
%{_mandir}/man7/*
%{_mandir}/man8/*
%exclude %{_libdir}/systemd/system/sysinit.target.wants/serial-getty@tty01.service
%exclude %{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyMFD2.service
%exclude %{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyO2.service
%exclude %{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyS0.service
%exclude %{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyS1.service
%exclude %{_libdir}/systemd/system/sysinit.target.wants/systemd-vconsole-setup.service
%exclude %{_libdir}/systemd/user/default.target

%files console-ttyMFD2
%defattr(-,root,root,-)
%{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyMFD2.service

%files console-ttyS0
%defattr(-,root,root,-)
%{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyS0.service

%files console-ttyS1
%defattr(-,root,root,-)
%{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyS1.service

%files console-tty01
%defattr(-,root,root,-)
%{_libdir}/systemd/system/sysinit.target.wants/serial-getty@tty01.service

%files console-ttyO2
%defattr(-,root,root,-)
%{_libdir}/systemd/system/sysinit.target.wants/serial-getty@ttyO2.service


%files tools
%defattr(-,root,root,-)
%manifest systemd.manifest
%{_bindir}/systemd-analyze

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

%files sysv
%defattr(-,root,root,-)
%doc LICENSE
%{_sbindir}/halt
%{_sbindir}/init
%{_sbindir}/poweroff
%{_sbindir}/reboot
%{_sbindir}/runlevel
%{_sbindir}/shutdown
%{_sbindir}/telinit
