%define app_data /opt/usr/apps/org.tizen.data-provider-slave/data

Name: org.tizen.data-provider-slave
Summary: Plugin type livebox service provider.
Version: 0.9.14
Release: 1
Group: frameowrk/livebox
License: Flora License
Source0: %{name}-%{version}.tar.gz
BuildRequires: cmake, gettext-tools, coreutils
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
Plugin type liveboxes are managed by this.
Supporting the EFL.

%prep
%setup -q

%build
cmake . -DCMAKE_INSTALL_PREFIX=%{_prefix}
#-fpie  LDFLAGS="${LDFLAGS} -pie -O3"
CFLAGS="${CFLAGS} -Wall -Winline -Werror -fno-builtin-malloc" make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install
mkdir -p %{buildroot}/%{_datarootdir}/license
mkdir -p %{buildroot}%{app_data}

%post

%files -n org.tizen.data-provider-slave
%manifest org.tizen.data-provider-slave.manifest
%defattr(-,root,root,-)
%{_prefix}/apps/org.tizen.data-provider-slave/bin/data-provider-slave
%{_datarootdir}/packages/org.tizen.data-provider-slave.xml
%{_datarootdir}/license/*
%{_sysconfdir}/smack/accesses2.d/org.tizen.data-provider-slave.rule
%attr(-,app,app) %dir %{app_data}

# End of a file
