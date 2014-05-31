%define app_data /opt/usr/apps/org.tizen.data-provider-slave/data

Name: org.tizen.data-provider-slave
Summary: Plugin type livebox service provider
Version: 0.14.1
Release: 1
Group: HomeTF/Livebox
License: Flora
Source0: %{name}-%{version}.tar.gz
Source1001: %{name}.manifest
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
BuildRequires: pkgconfig(efl-assist)
BuildRequires: pkgconfig(json-glib-1.0)
BuildRequires: hash-signer
BuildRequires: pkgconfig(capi-system-system-settings)

%if "%{sec_product_feature_livebox}" == "0"
ExclusiveArch:
%endif

%description
Plugin type liveboxes are managed by this.
Supporting the EFL.
Supporting the In-house livebox only.

%prep
%setup -q
cp %{SOURCE1001} .

%build
%if 0%{?sec_build_binary_debug_enable}
export CFLAGS="$CFLAGS -DTIZEN_DEBUG_ENABLE"
export CXXFLAGS="$CXXFLAGS -DTIZEN_DEBUG_ENABLE"
export FFLAGS="$FFLAGS -DTIZEN_DEBUG_ENABLE"
%endif

%if 0%{?tizen_build_binary_release_type_eng}
export CFLAGS="${CFLAGS} -DTIZEN_ENGINEER_MODE"
export CXXFLAGS="${CXXFLAGS} -DTIZEN_ENGINEER_MODE"
export FFLAGS="${FFLAGS} -DTIZEN_ENGINEER_MODE"
%endif

%if "%{_repository}" == "wearable"
export MOBILE=Off
export WEARABLE=On
%else
export MOBILE=On
export WEARABLE=Off
%endif

%cmake . -DMOBILE=${MOBILE} -DWEARABLE=${WEARABLE}
#-fpie  LDFLAGS="${LDFLAGS} -pie -O3"
CFLAGS="${CFLAGS} -Wall -Winline -Werror -fno-builtin-malloc" make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install
%define tizen_sign 1
%define tizen_sign_base %{_prefix}/apps/%{name}
%define tizen_sign_level platform
%define tizen_author_sign 1
%define tizen_dist_sign 1
mkdir -p %{buildroot}/%{_datarootdir}/license
mkdir -p %{buildroot}%{app_data}

%post
chown 5000:5000 %{app_data}
chmod 755 %{app_data}

%files -n org.tizen.data-provider-slave
%manifest %{name}.manifest
%defattr(-,root,root,-)
%{_prefix}/apps/%{name}
%{_datarootdir}/packages/%{name}.xml
%{_datarootdir}/license/*

%if "%{_repository}" == "wearable"
%{_sysconfdir}/smack/accesses2.d/%{name}.rule
%else
/opt/%{_sysconfdir}/smack/accesses.d/%{name}.rule
%endif
%dir %{app_data}

# End of a file
