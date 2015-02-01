%bcond_with wayland
%define app_data /opt/usr/apps/org.tizen.data-provider-slave/data

Name: org.tizen.data-provider-slave
Summary: Plugin type dynamicbox service provider
Version: 1.0.0
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
BuildRequires: pkgconfig(dynamicbox_provider)
BuildRequires: pkgconfig(dynamicbox_service)
BuildRequires: pkgconfig(capi-appfw-application)
BuildRequires: pkgconfig(capi-appfw-app-manager)
BuildRequires: pkgconfig(ecore)
BuildRequires: pkgconfig(edje)
BuildRequires: pkgconfig(evas)
BuildRequires: pkgconfig(dynamicbox)
BuildRequires: pkgconfig(elementary)
BuildRequires: pkgconfig(com-core)
BuildRequires: pkgconfig(shortcut)
BuildRequires: pkgconfig(efl-assist)
BuildRequires: pkgconfig(json-glib-1.0)
%if %{with wayland}
BuildRequires: pkgconfig(ecore-wayland)
%else
BuildRequires: pkgconfig(ecore-x)
%endif
#BuildRequires: hash-signer
BuildRequires: pkgconfig(capi-system-system-settings)
BuildRequires: model-build-features
#Requires(post): signing-client

%if "%{model_build_feature_livebox}" == "0"
ExclusiveArch:
%endif

%description
Plugin type dynamicboxes are managed by this.
Supporting the EFL.
Supporting the In-house dynamicbox only.

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

%if %{with wayland}
export WAYLAND_SUPPORT=On
export X11_SUPPORT=Off
%else
export WAYLAND_SUPPORT=Off
export X11_SUPPORT=On
%endif

%cmake . -DWAYLAND_SUPPORT=${WAYLAND_SUPPORT} -DX11_SUPPORT=${X11_SUPPORT}
make %{?jobs:-j%jobs}

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
#/usr/bin/signing-client/hash-signer-client.sh -a -d -p platform %{_prefix}/apps/%{name}
chown 5000:5000 %{app_data}
chmod 755 %{app_data}

%files -n org.tizen.data-provider-slave
%manifest %{name}.manifest
%defattr(-,root,root,-)
%{_prefix}/apps/%{name}
%{_datarootdir}/packages/%{name}.xml
%{_datarootdir}/license/*
%{_sysconfdir}/smack/accesses.d/%{name}.efl
/opt/usr/share/data-provider-slave/*
%dir %{app_data}

# End of a file
