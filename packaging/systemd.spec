#sbs-git:slp/pkgs/s/systemd systemd 37 8ba5df3f1f236e059a9bd77314ed3ca0b4ffcde7
Name:       systemd
Summary:    Systemd System and Service Manager
Version: 37
Release:    2
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
BuildRequires:  gperf

%description
system and service manager systemd is a replacement for sysvinit.
It is dependency-based and able to read the LSB init script headers
in addition to parsing rcN.d links as hints.  .  It also provides
process supervision using cgroups and the ability to not only depend
on other init script being started, but also availability of a given
mount point or dbus service.

%package devel
Summary:   systemd development files
Requires:  %{name} = %{version}
Group:     Development/Libraries

%description devel
Headers and static libraries for systemd (Development)

%prep
%setup -q

%build

DH_OPTIONS='-Nlibpam-systemd -Nsystemd-gui'
export DH_OPTIONS

libtoolize -c --force
intltoolize -c -f
aclocal -I m4
autoconf -Wall
autoheader 
automake --copy --foreign --add-missing

./configure --prefix=/usr --disable-static \
           --disable-docs \
           --with-rootdir=/ \
           --with-rootlibdir=/lib \
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
mkdir -p $RPM_BUILD_ROOT/usr/lib
cp src/sd-daemon.h $RPM_BUILD_ROOT/usr/include
#cp .libs/libsystemd-daemon.a $RPM_BUILD_ROOT/usr/lib

%remove_docs

%files
%defattr(-,root,root,-)
/bin/*
/etc/dbus-1/system.d/*
/lib/*
/lib/systemd/system-generators/systemd-getty-generator
/lib/systemd/system/*
/lib/systemd/system/*/*
/lib/udev/rules.d/*
/usr/bin/*
/usr/etc/*
/usr/etc/*/*
/usr/etc/*/*/*
/usr/lib/libsystemd-daemon.so
/usr/lib/libsystemd-login.so
/usr/lib/systemd/user/*
/usr/lib/tmpfiles.d/*
/usr/share/dbus-1/interfaces/*.xml
/usr/share/dbus-1/services/org.freedesktop.systemd1.service
/usr/share/dbus-1/system-services/*
/usr/share/polkit-1/actions/*
/usr/share/systemd/kbd-model-map

%files devel
/usr/include/*.h
/usr/include/*/*.h
/usr/lib/pkgconfig/*
/usr/share/pkgconfig/systemd.pc
