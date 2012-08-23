Name:       systemd
Summary:    Systemd System and Service Manager
Version:    25.10
Release:    1
Group:      System/Base
License:    GPLv2
Source0:    %{name}-%{version}.tar.gz
BuildRequires:  pkgconfig(dbus-1) >= 1.3.2
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(libudev) >= 160
BuildRequires:  libcap-devel
BuildRequires:  libacl-devel
BuildRequires:  intltool
BuildRequires:  fdupes

%description
system and service manager systemd is a replacement for sysvinit.
It is dependency-based and able to read the LSB init script headers
in addition to parsing rcN.d links as hints.  .  It also provides
process supervision using cgroups and the ability to not only depend
on other init script being started, but also availability of a given
mount point or dbus service.


%prep
%setup -q

%build

%autogen --disable-static
%configure --disable-static \
           --with-rootdir= \
           --with-udevrulesdir=/lib/udev/rules.d \
           --disable-gtk \
           --disable-libcryptsetup \
           --disable-audit \
           --disable-pam \
           --disable-tcpwrap \
           --disable-selinux \
           --with-distro=slp
make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install

mkdir -p $RPM_BUILD_ROOT/usr/include
cp $RPM_BUILD_ROOT/usr/share/doc/systemd/sd-daemon.h $RPM_BUILD_ROOT/usr/include

%files
%defattr(-,root,root,-)
/bin/systemctl
/bin/systemd
/bin/systemd-ask-password
/bin/systemd-machine-id-setup
/bin/systemd-notify
/bin/systemd-tmpfiles
/bin/systemd-tty-ask-password-agent
/etc/bash_completion.d/systemctl-bash-completion.sh
/etc/dbus-1/system.d/org.freedesktop.hostname1.conf
/etc/dbus-1/system.d/org.freedesktop.systemd1.conf
/etc/systemd/system.conf
/etc/systemd/system/getty.target.wants/serial-getty@serial_console.service
/etc/systemd/system/multi-user.target.wants/remote-fs.target
/etc/systemd/system/sysinit.target.wants/hwclock-load.service
/etc/tmpfiles.d/systemd.conf
/etc/tmpfiles.d/x11.conf
/etc/xdg/systemd/user
/lib/systemd/system*
/lib/udev/rules.d/99-systemd.rules
/usr/bin/systemd*
/usr/share/dbus-1/interfaces/org.freedesktop.systemd1*
/usr/share/dbus-1/services/org.freedesktop.systemd1.service
/usr/share/dbus-1/system-services/org.freedesktop.hostname1.service
/usr/share/dbus-1/system-services/org.freedesktop.systemd1.service
%exclude /usr/share/doc/systemd/*
/usr/include/sd-daemon.h
/usr/lib/pkgconfig/libsystemd-daemon.pc
/usr/share/pkgconfig/systemd.pc
/usr/share/polkit-1/actions/org.freedesktop.hostname1.policy
/usr/share/polkit-1/actions/org.freedesktop.systemd1.policy
/usr/lib/systemd*

