%define app_data /opt/usr/apps/org.tizen.data-provider-slave/data

Name: org.tizen.data-provider-slave
Summary: Plugin type livebox service provider.
Version: 0.11.8
Release: 1
Group: HomeTF/Livebox
License: Flora License
Source0: %{name}-%{version}.tar.gz
BuildRequires: cmake, gettext-tools, coreutils, edje-bin
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
BuildRequires: pkgconfig(com-core)
BuildRequires: pkgconfig(shortcut)
BuildRequires: pkgconfig(capi-system-system-settings)
BuildRequires: hash-signer
Requires: data-provider-master

%description
Plugin type liveboxes are managed by this.
Supporting the EFL.
Supporting the In-house livebox only.

%prep
%setup -q

%build
cmake . -DCMAKE_INSTALL_PREFIX=%{_prefix}
#-fpie  LDFLAGS="${LDFLAGS} -pie -O3"
CFLAGS="${CFLAGS} -Wall -Winline -Werror -fno-builtin-malloc" make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install
%define tizen_sign 1
%define tizen_sign_base %{_prefix}/apps/org.tizen.data-provider-slave
%define tizen_sign_level platform
%define tizen_author_sign 1
%define tizen_dist_sign 1
mkdir -p %{buildroot}/%{_datarootdir}/license
mkdir -p %{buildroot}%{app_data}

%post
chown 5000:5000 %{app_data}
chmod 755 %{app_data}

%files -n org.tizen.data-provider-slave
%manifest org.tizen.data-provider-slave.manifest
%defattr(-,root,root,-)
%{_prefix}/apps/org.tizen.data-provider-slave
%{_datarootdir}/packages/org.tizen.data-provider-slave.xml
%{_datarootdir}/license/*
%{_sysconfdir}/smack/accesses.d/org.tizen.data-provider-slave.rule
%dir %{app_data}

# End of a file
