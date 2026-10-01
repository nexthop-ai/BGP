Name: bgpd
Version: 1.0
Release: 1
Summary: BGP routing daemon
License: Meta
Source0: bgpd.service
Source1: bgpcpp.conf
Source2: setup_bgp_env
BuildRequires: systemd-rpm-macros rpm jemalloc-devel
AutoReqProv: no

# bgpd ships pre-built with debug info (-g from FBCompilerSettingsUnix.cmake).
# rpmbuild's default %%__os_install_post runs brp-strip et al. on packaged
# binaries, which strips .symtab/DWARF from /opt/bgp/bin/bgpd and leaves
# coredumps on testbeds un-symbolizable for bgpd's own frames. Disable all
# post-install brp scripts so the binary keeps its symbols; the size cost is
# accepted to make crashes debuggable.
%global __os_install_post %{nil}

%description
BGP routing daemon for FBOSS-based network switches.

%prep
# Binary pre-staged as rpmbuild/SOURCES/bgpd by distro_build.sh — nothing to extract.

%install
mkdir -p %{buildroot}/opt/bgp/bin
install -m 755 %{_sourcedir}/bgpd %{buildroot}/opt/bgp/bin/bgpd
install -m 644 %{_sourcedir}/setup_bgp_env %{buildroot}/opt/bgp/bin/setup_bgp_env

mkdir -p %{buildroot}/opt/bgp/lib
cp -a %{_sourcedir}/lib/. %{buildroot}/opt/bgp/lib/

mkdir -p %{buildroot}%{_unitdir}
install -m 644 %{_sourcedir}/bgpd.service %{buildroot}%{_unitdir}/bgpd.service

mkdir -p %{buildroot}/etc/coop
install -m 644 %{_sourcedir}/bgpcpp.conf %{buildroot}/etc/coop/bgpcpp.conf

%files
%defattr(-,root,root,-)
/opt/bgp/bin/bgpd
/opt/bgp/bin/setup_bgp_env
/opt/bgp/lib
%{_unitdir}/bgpd.service
%config(noreplace) /etc/coop/bgpcpp.conf

%post
systemctl enable bgpd.service

%preun
%systemd_preun bgpd.service

%postun
%systemd_postun_with_restart bgpd.service
