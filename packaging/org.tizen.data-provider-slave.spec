Name: org.tizen.data-provider-slave
Summary: Slave data provider
Version: 0.8.17
Release: 1
Group: main/app
License: Flora License
Source0: %{name}-%{version}.tar.gz
BuildRequires: cmake, gettext-tools
BuildRequires: pkgconfig(appcore-efl)
BuildRequires: pkgconfig(ail)
BuildRequires: pkgconfig(dlog)
BuildRequires: pkgconfig(aul)
BuildRequires: pkgconfig(vconf)
BuildRequires: pkgconfig(sqlite3)
BuildRequires: pkgconfig(db-util)
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(gio-2.0)
BuildRequires: pkgconfig(bundle)
BuildRequires: pkgconfig(ecore-x)
BuildRequires: pkgconfig(provider)
BuildRequires: pkgconfig(heap-monitor)
BuildRequires: pkgconfig(livebox-service)
BuildRequires: pkgconfig(capi-appfw-application)
BuildRequires: pkgconfig(capi-appfw-app-manager)
BuildRequires: pkgconfig(ecore)
BuildRequires: pkgconfig(edje)
BuildRequires: pkgconfig(evas)
BuildRequires: pkgconfig(livebox)
BuildRequires: pkgconfig(elementary)

%description
Loading livebox and managing their life-cycle to generate contents properly.

%prep
%setup -q

%build
cmake . -DCMAKE_INSTALL_PREFIX=%{_prefix}
#-fpie  LDFLAGS="${LDFLAGS} -pie -O3"
CFLAGS="${CFLAGS} -Wall -Winline -Werror -fno-builtin-malloc" make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install
mkdir -p %{buildroot}/usr/share/license

%post

%files -n org.tizen.data-provider-slave
%manifest org.tizen.data-provider-slave.manifest
%defattr(-,root,root,-)
/usr/apps/org.tizen.data-provider-slave/bin/data-provider-slave
/usr/share/packages/org.tizen.data-provider-slave.xml
/usr/share/license/*
